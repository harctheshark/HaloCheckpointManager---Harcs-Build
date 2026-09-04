#include "pch.h"
#include <shared_mutex>   // pch.h carries <mutex> only; the render-path gate is a shared_timed_mutex
#include "HCETriggerOverlay.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include "HCETriggerActivity.h"
#include "HCESpeedrunTriggerNames.h"
#include "PointerDataStore.h"
#include "RenderTextHelper.h"
#include "GlobalKill.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "Render3DEventProvider.h"
#include "IRenderer3D.h"
#include "IModel.h"
#include "ModalDialogRenderer.h"
#include "ModalDialogFactory.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include "HCEAnchors.h"
#include "HCECheckpointGrantTriggers.h"

// ================================================================================================================
// Halo Campaign Evolved trigger volume overlay.
//
// WHY THE TWO ADDRESSES ARE RESOLVED BY BYTE SIGNATURE AND NOT FROM InternalPointerData.xml
// ------------------------------------------------------------------------------------------
// Reading trigger volumes needs two module globals: the scenario tag-data pointer slot, and the tag block
// ADDRESS TABLE that turns an encoded tag-block address into a real pointer. Both MOVE between game builds -
// the address table is 0x2C2DCC0 on one observed build and 0x2C2CCC0 on another - and HaloSimulation_tag_release.dll
// carries no version resource, so every build reports 0.0.0.0 and the XML Version attribute cannot tell them
// apart. A stale global here would not fail loudly; it would resolve to whatever now occupies that address and
// draw confident nonsense (on one observed update the equivalent stale address landed inside a string table).
//
// So both are located by a pattern that is verified unique across two different builds, and a pattern that
// matches zero or more than one time DISABLES the feature rather than falling back to a last-known address.
// That is the same mechanism repos\HceHavokDebugger uses, and it is the only update detector this title admits.
// Everything that is genuinely a structural offset (block offsets, strides, field offsets) IS in the XML.
//
// COORDINATE FRAME - READ THIS BEFORE TOUCHING makeCamera()
// ---------------------------------------------------------
// Trigger volume tag data, the player position, the camera-entry position and the s_player_control view angle
// are all one consistent Blam world frame - so NOTHING is scaled or mirrored here. The external CER tool's
// "multiply by 304.8 and negate Y" transform exists only because it took its camera from a hook inside the UE5
// exe, whose camera lives in UE centimetres. We take the camera from the sim, so that transform must NOT be
// applied.
//
// BUT: `w -> (w.x, -w.y, w.z) * 304.8` is NOT a unit conversion. The 304.8 is; the Y negation is a HANDEDNESS
// CHANGE. Blam is right-handed, Z up, +X forward and +Y LEFT; UE5 is left-handed with +Y RIGHT. Dropping the
// negation - which is correct, because we never leave the Blam frame - therefore ALSO flips which way the
// camera's right vector points, and the reference's basis formulas must be rewritten accordingly. Derivation,
// with M = diag(1,-1,1) (its own inverse, and symmetric, so <M(d), v> == <d, M(v)>, and the uniform 304.8
// cancels in x/z and y/z):
//
//     forward = M(f_ue) and up = M(u_ue) reproduce the reference EXACTLY once yaw_ue = -yaw_blam, pitch_ue = pitch_blam
//     right   = M(r_ue) = M(-sin(yaw_ue), cos(yaw_ue), 0) = (sin(yaw), -cos(yaw), 0)
//
// i.e. the right vector is the NEGATION of the reference's literal `(-sin, cos, 0)`. Using the reference's
// formula verbatim in the Blam frame yields Blam's LEFT, which mirrors the overlay horizontally: pitch and
// translation stay right, but every volume lands on the wrong side of the screen and sweeps the wrong way when
// the player turns - which reads exactly like "the triggers rotate with the camera instead of staying put".
// That was the shipped bug. The corrected vector is also literally what HCEGetPlayerState::lookRelativeOffset
// (Teleport/Launch relative modes, in-game verified) builds as `forward x UnitZ`.
//
// THE GEOMETRY IS THE ENGINE'S OWN, NOT A GUESS
// ---------------------------------------------
// Every number used below was read back out of trigger_volume_test_point (sim rva 0x2E45F0) and the basis
// builder it calls (rva 0x2E41B0), so the drawn shape is the shape the engine actually tests:
//   * element stride is 124 (the test indexes `31 * i` DWORDs), the block is count @ scenario+0x278 /
//     encoded address @ +0x27C, and an encoded address resolves as tagAddressTable[enc >> 28] + 4 * enc.
//   * type (uint16 @ +0x0C) NON-ZERO takes the sector path, zero takes the box path.
//   * the box basis is position @ +0x28 VERBATIM, forward @ +0x10, up @ +0x1C, and a third axis = up X forward
//     (verified instruction by instruction in the basis builder's vmulss/vfmsub231ss triple, which writes
//     S.x = f.z*u.y - f.y*u.z, S.y = u.z*f.x - u.x*f.z, S.z = u.x*f.y - u.y*f.x into the matrix at +0x10).
//     That axis is Blam's LEFT, not right - real_matrix4x3 is {scale, forward, LEFT, up, position} - and
//     extents.y runs along it. Naming it "right" is what seeded the camera-basis sign bug; do not do it again.
//     The test projects (point - position) onto that basis and accepts 0 <= local <= extents, so the box
//     spans [0, extents] FROM position - it is NOT centred on it.
//   * there is NO zSink offset. The external CER tool anchors its box at `position - up * zSink` using the
//     float at +0x40; the basis builder copies +0x28..+0x30 into the transform untouched, so that is wrong.
//   * the uniform scale the test divides by is written as a literal 1.0f for any volume not attached to an
//     object, so it is ignored here.
//
// KNOWN LIMITS:
//   * a volume ATTACHED to an object (uint16 @ +0x04 != 0xFFFF) is transformed by that object's node at
//     runtime. We draw its authored placement, which is where it sits unless the object has moved.
//   * for a sector, the engine tests a RUNTIME convex decomposition (5-plane records, stride 112, at
//     +0x50/+0x54), not the authoring polygon. We draw the authoring polygon prism, which is the union
//     those pieces decompose - the right thing to look at, and what the standalone HCE Havok debugger
//     already draws.
//   * the camera is assembled from TWO objects: position from the observer (camera entry + 0x20) and
//     orientation from s_player_control. Those agree in ordinary first person, which is what the overlay is
//     for. They can disagree while the observer is being smoothed or driven by something other than the
//     player's aim - a cinematic camera, a vehicle/third-person camera, the death cam, or HCM's own Freecam
//     (which moves the observer without touching s_player_control, so the overlay's rotation would stop
//     tracking). Fixing that properly means taking the whole camera from one source; see below.
//
// HOW IT DRAWS - TWO PATHS, 3D FIRST
// ----------------------------------
// PRIMARY: HCM's own IRenderer3D, via Render3DEventProvider. On HaloCER that resolves to Renderer3DImplD3D12,
// which records real depth-tested triangles and lines into the command list D3D12Hook already owns. Solid faces
// are ear-clipped into actual triangles (sector caps are frequently CONCAVE - L-shaped rooms - which ImGui's
// AddConvexPolyFilled cannot fill correctly), the player's trigger test point is a real sphere, and volumes sort
// against EACH OTHER through a depth buffer the renderer owns and clears every frame. Exactly like MCC's
// TriggerOverlay, they are NOT occluded by world geometry - that is deliberate, not a limitation to fix here.
//
// FALLBACK: the original ImGui background-draw-list path below, unchanged. It runs whenever the 3D path has not
// drawn a frame recently - which covers Renderer3DImplD3D12 failing to initialise (no PSO, no device, an
// unsupported back buffer format), Render3DEventProvider failing to construct at all, or the D3D12 hook not being
// up yet. The ImGui RenderEvent fires EARLIER in the frame than the D3D12 render event, so "did the 3D path draw
// recently" is a timestamp check rather than a same-frame flag.
//
// THE FIELD OF VIEW
// -----------------
// Camera position (camera entry + 0x20) and orientation (s_player_control + 0x14/+0x18) come from the sim. The
// FOV does NOT exist in the sim in a usable form, and is read live from the UE5 renderer instead: a midhook on
// APlayerCameraManager::DoUpdateCamera (exe RVA 0x64B3B60) captures `this`, and POV.FOV is a float at +0x14E0.
// See InternalPointerData.xml, region hceCameraManager, for the derivation and the offset cross-check.
//
// THAT MIDHOOK NOW LIVES IN HCEGetCameraData, not here - both this overlay's fallback path and the D3D12 renderer
// need the camera, and installing the same midhook twice is not something to discover at runtime. The maths moved
// verbatim; see HCEGetCameraData.h, which carries the full derivation.
//
// This used to be a user setting (hceTriggerOverlayFOV) and is not any more - nobody should have to eyeball a
// slider until boxes line up.
//
// MIXING SIM POSITION WITH A UE FOV IS SAFE, and is NOT the frame-mixing that caused the mirrored-overlay bug.
// That bug was a HANDEDNESS error: a basis VECTOR formula written for UE's left-handed +Y-right frame, used in
// Blam's right-handed +Y-left frame. FOV is a scalar with no handedness and no frame - it is the same number in
// both. Only vectors have to agree on a frame.
//
// Why the sim's own FOV is not usable: camera_set_field_of_view's evaluator (sim rva 0x1DFB90) writes
// `argument * (pi/180)` to *(tls + 0x78) + 0x10 - director globals, radians - alongside a start time at +0x0C
// and a blend duration at +0x08. That is the SCRIPT OVERRIDE, not the render value. The renderer blends it with
// the unit/globals FOV and then the exe applies render_first_person_fov_scale and the widescreen adjustment.
//
// Why not FMinimalViewInfo::operator= (RVA 0x5AA7BB0), which the external CER tool hooks: its first 238 bytes
// are DUPLICATED at RVA 0x7ECC950 (the two differ only in one rel32), so a byte signature matches twice and the
// "exactly one match or nothing" rule at the top of this file would refuse it. DoUpdateCamera has no twin.
//
// STILL NOT RESOLVED: position and orientation come from the sim's player camera, so they track the PLAYER, not
// necessarily what is being rendered - a cinematic camera, a vehicle/third-person camera, the death cam, or
// HCM's own Freecam will all drift. Fixing that means taking position AND rotation from the same UE POV struct
// (Location at +0x14B0, Rotation at +0x14C8, both doubles, rotation in DEGREES) and transforming the volume
// vertices with (x, -y, z) * 304.8 as the reference tool does. That is a coherent whole-frame swap; what must
// never happen is taking a basis vector from one frame and geometry from the other.
// ================================================================================================================

namespace
{
	// ---------------------------------------------------------------------------------------------------------
	// Byte-signature scanning.
	//
	// Every function that contains __try here is deliberately POD-only: MSVC rejects __try in a function that
	// requires C++ object unwinding (C2712), which is the same constraint HCEGetPlayerState.cpp documents.
	// ---------------------------------------------------------------------------------------------------------
	constexpr int kMaxSigBytes = 64;

	struct HcePattern
	{
		unsigned char bytes[kMaxSigBytes];
		bool wild[kMaxSigBytes];
		int length;
	};

	int hexNibble(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	// "48 8B 05 ?? ?? ?? ?? 3B 88" - hex bytes, ?? for a wildcard, whitespace ignored.
	bool parsePattern(const char* text, HcePattern& out)
	{
		out.length = 0;
		while (*text && out.length < kMaxSigBytes)
		{
			if (*text == ' ' || *text == '\t') { ++text; continue; }
			if (*text == '?')
			{
				out.bytes[out.length] = 0;
				out.wild[out.length] = true;
				++out.length;
				++text;
				if (*text == '?') ++text;
				continue;
			}
			const int hi = hexNibble(text[0]); if (hi < 0) return false;
			const int lo = hexNibble(text[1]); if (lo < 0) return false;
			out.bytes[out.length] = (unsigned char)((hi << 4) | lo);
			out.wild[out.length] = false;
			++out.length;
			text += 2;
		}
		return out.length > 0;
	}

	// Scans every EXECUTABLE section of the module. Returns the match count (which the caller must require to
	// be exactly 1), or -1 if the PE headers could not be walked. POD-only, see above.
	int scanExecutableSections(uintptr_t base, const unsigned char* pattern, const bool* wild, int length,
		uintptr_t* outHits, int maxHits)
	{
		int found = 0;
		__try
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
			for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
			{
				if (!(section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				const DWORD virtualSize = section[s].Misc.VirtualSize ? section[s].Misc.VirtualSize : section[s].SizeOfRawData;
				if (virtualSize <= (DWORD)length) continue;

				const unsigned char* const bytes = (const unsigned char*)(base + section[s].VirtualAddress);
				const size_t span = (size_t)virtualSize - (size_t)length;
				const unsigned char firstByte = pattern[0];
				const bool firstWild = wild[0];

				for (size_t i = 0; i <= span; ++i)
				{
					if (!firstWild && bytes[i] != firstByte) continue;
					int k = 1;
					for (; k < length; ++k)
						if (!wild[k] && bytes[i + k] != pattern[k]) break;
					if (k != length) continue;
					if (found < maxHits) outHits[found] = (uintptr_t)(bytes + i);
					++found;
					if (found > maxHits) return found;   // caller only needs to know it is > 1
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		return found;
	}

	// Reads the disp32 of a rip-relative instruction and returns its target. insnLength is the length of the
	// WHOLE instruction (rip is the address of the next one).
	uintptr_t ripTarget(uintptr_t instruction, int dispOffset, int insnLength)
	{
		int32_t displacement = 0;
		if (!HCEGetPlayerState::tryReadRaw(instruction + dispOffset, &displacement, sizeof(displacement))) return 0;
		return instruction + insnLength + (intptr_t)displacement;
	}

	// Exactly one hit or nothing. Zero means this build moved the code; more than one means the pattern is not
	// specific enough to trust. Neither may fall back to a remembered address - see the header comment.
	uintptr_t resolveUniqueMatch(uintptr_t base, const char* patternText, int& outHits)
	{
		outHits = 0;
		HcePattern pattern{};
		if (!parsePattern(patternText, pattern)) return 0;
		uintptr_t hits[4]{};
		const int count = scanExecutableSections(base, pattern.bytes, pattern.wild, pattern.length, hits, 4);
		outHits = count;
		return (count == 1) ? hits[0] : 0;
	}

	// ---------------------------------------------------------------------------------------------------------
	// The engine's string_id -> text function.
	//
	// Its whole body is a hash-table lookup through one module global (a bucket array with a virtual hash and a
	// virtual compare) plus a memcpy out of the matched record - there is NO thread-local storage anywhere in
	// it, which is what makes it safe to call from HCM's render thread rather than the game thread. It is still
	// wrapped in SEH and pre-gated on its table pointer being non-null, because during a level load the table
	// is being rebuilt underneath us.
	// ---------------------------------------------------------------------------------------------------------
	typedef const char* (__fastcall* FnStringIdText)(uint32_t);

	bool callStringIdText(FnStringIdText function, uint32_t stringId, char* out, size_t capacity)
	{
		out[0] = '\0';
		__try
		{
			const char* text = function(stringId);
			if (!text) return false;
			size_t i = 0;
			for (; i + 1 < capacity; ++i)
			{
				const char c = text[i];
				if (c == '\0') break;
				if (c < 0x20 || c > 0x7E) break;   // stop at the first non-printable rather than showing garbage
				out[i] = c;
			}
			out[i] = '\0';
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out[0] = '\0';
			return false;
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// Guarded reads. All of these go through HCEGetPlayerState::tryReadRaw, which is SEH-wrapped, so a moved
	// offset degrades to a zero rather than a crash.
	// ---------------------------------------------------------------------------------------------------------
	uint32_t readU32(uintptr_t address) { uint32_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	uint16_t readU16(uintptr_t address) { uint16_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	int32_t  readI32(uintptr_t address) { int32_t v = 0;  HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	float    readF32(uintptr_t address) { float v = 0.f;  HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t address) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }

	SimpleMath::Vector3 readVec3(uintptr_t address)
	{
		float v[3]{};
		HCEGetPlayerState::tryReadRaw(address, v, sizeof(v));
		return SimpleMath::Vector3(v[0], v[1], v[2]);
	}

	bool plausiblePointer(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }

	// ---------------------------------------------------------------------------------------------------------
	// One trigger volume, already expanded into world-space wireframe geometry so the render path does no tag
	// reading at all.
	// ---------------------------------------------------------------------------------------------------------
	struct HceTriggerVolume
	{
		// Classified ONCE at tag-refresh time, not per frame: both are string work, and the name cannot change
		// without a refresh anyway.
		//
		// TWO zone-set categories, because the tag carries two independent fields on ONE 8-byte element:
		//   'begin zone set'  (+0x02) -> prepare_to_switch_to_zone_set. PURELY SUBTRACTIVE: it unloads the BSPs
		//                                the target zone set does not want and loads NOTHING. Proven by
		//                                arithmetic, not by the field name - see InternalPointerData.xml.
		//   'commit zone set' (+0x06) -> switch_zone_set. This is the one that brings new BSP geometry in.
		// Either may be -1. Shipped data across all 13 levels has no volume with both, but the struct allows it,
		// so both bools can be set and COMMIT wins for colour and filtering.
		bool isBeginZoneSet = false;
		bool isCommitZoneSet = false;
		// INVARIANT: isBspSwitch == (isBeginZoneSet || isCommitZoneSet) after classification. Kept so every
		// existing call site still compiles and behaves.
		bool isBspSwitch = false;    // from the scenario's zone-set switch block (name heuristic as fallback)
		int16_t beginZoneSet = -1;      // index into the scenario 'zone sets' block, -1 = none
		int16_t commitZoneSet = -1;
		uint16_t zoneSetSwitchFlags = 0;   // bit0 = "teleport vehicles" - a MODIFIER, never a category
		// CHECKPOINT GRANTS. ⚠ Unlike every other category here, this one is NOT scenario-tag data - it comes
		// from a sweep of the LEVEL SCRIPTS, because whether a volume grants a checkpoint is a HaloScript fact
		// with no tag-side flag at all. See HCECheckpointGrantTriggers.h.
		bool isCheckpointGrant = false;
		// Empty for an UNCONDITIONAL grant. Non-empty means the grant is gated on something (difficulty, an
		// objective, being in a vehicle), and this is the phrase shown on entry.
		std::string_view checkpointNote;

		bool isKill = false;         // scenario kill trigger block (name heuristic as fallback)
		// ⚠ OPPOSITE MEANING to isKill: a kill volume is "stay out", a safe zone is "stay in" - the engine kills
		// you when you are inside NO safe zone. Same toggle, deliberately different colour.
		bool isSafeZone = false;
		bool isSpeedrun = false;     // on the community completion-requirement list

		std::string name;
		// SHAPE, not a category. Selects the containment test in pointIsInside(); it no longer affects colour
		// or filtering (sectors are Regular volumes that happen to be prisms).
		bool isSector = false;

		// ---- containment, for the zone-set entry report. Filled by buildBox / buildSectorPrism.
		SimpleMath::Vector3 boxOrigin{}, boxForward{}, boxLeft{}, boxUp{}, boxExtents{};
		std::vector<SimpleMath::Vector2> sectorPolygon;   // XY footprint, sectors only
		float sectorZ0 = 0.f, sectorZ1 = 0.f;
		bool playerWasInside = false;                     // edge state for reportZoneSetEntries
		std::string zoneSetReportPrefix;                  // preformatted at refresh time
		SimpleMath::Vector3 center{};
		float radius = 0.f;                                   // bounding radius about center, for the distance cull
		std::vector<SimpleMath::Vector3> vertices;
		std::vector<std::pair<uint16_t, uint16_t>> edges;

		// Faces, for solid rendering. Each entry is one polygon's vertex indices, wound around its perimeter.
		// Boxes are all quads; sector prisms are quad side walls plus two n-gon caps.
		std::vector<std::vector<uint16_t>> faces;

		// Scenario trigger-volume block index. This is the identity the engine itself uses (it is what sits in
		// ECX at trigger_volume_test_point's call sites), so it is what the activity tracker is keyed on.
		uint32_t index = 0;

		// ---- geometry for the 3D renderer path, built once per refresh so the render thread allocates nothing.
		// Same points as `vertices`, plus index buffers: `faces` triangulated, and `edges` flattened.
		VertexCollection renderVertices;
		IndexCollection triangleIndices;
		IndexCollection edgeIndices;
	};

	// Feeds one volume's cached geometry to IRenderer3D::drawTriangleCollection / drawEdgeCollection. Holds a
	// reference to the volume, so it is only ever a stack temporary inside the render callback (which holds
	// mVolumesMutex for its whole duration).
	class HceTriggerVolumeModel : public IModelTriangles, public IModelEdges
	{
	public:
		explicit HceTriggerVolumeModel(const HceTriggerVolume& volume) : mVolume(volume) {}
		const VertexCollection& getTriangleVertices() const override { return mVolume.renderVertices; }
		const IndexCollection& getTriangleIndices() const override { return mVolume.triangleIndices; }
		const VertexCollection& getEdgeVertices() const override { return mVolume.renderVertices; }
		const IndexCollection& getEdgeIndices() const override { return mVolume.edgeIndices; }
	private:
		const HceTriggerVolume& mVolume;
	};

	// Ear clipping. A sector cap polygon may be CONCAVE (L-shaped rooms are common), so a triangle fan is simply
	// wrong for it - which is the same reason the ImGui path's AddConvexPolyFilled fills those caps incorrectly.
	// This runs on the tag-refresh path (4x a second at most), never per frame.
	//
	// Everything is drawn with CullingOption::CullNone, exactly as MCC's TriggerOverlay does, so the winding of
	// the emitted triangles does not affect visibility - only their coverage matters.
	void triangulateFace(const std::vector<SimpleMath::Vector3>& vertices, const std::vector<uint16_t>& face,
		IndexCollection& out)
	{
		const size_t n = face.size();
		if (n < 3) return;
		for (uint16_t vi : face) if (vi >= vertices.size()) return;

		if (n == 3)
		{
			out.push_back(face[0]); out.push_back(face[1]); out.push_back(face[2]);
			return;
		}

		// Newell's method: a plane normal that stays sane for concave and slightly non-planar polygons, which a
		// single edge cross product does not.
		SimpleMath::Vector3 normal{};
		for (size_t i = 0; i < n; ++i)
		{
			const SimpleMath::Vector3& a = vertices[face[i]];
			const SimpleMath::Vector3& b = vertices[face[(i + 1) % n]];
			normal.x += (a.y - b.y) * (a.z + b.z);
			normal.y += (a.z - b.z) * (a.x + b.x);
			normal.z += (a.x - b.x) * (a.y + b.y);
		}
		if (normal.LengthSquared() < 1e-20f) return;   // degenerate face, nothing to fill
		normal.Normalize();

		// A 2D basis on the polygon's plane, so the clipping itself is plain 2D geometry.
		SimpleMath::Vector3 axisU = (std::abs(normal.z) < 0.9f)
			? SimpleMath::Vector3::UnitZ.Cross(normal)
			: SimpleMath::Vector3::UnitX.Cross(normal);
		if (axisU.LengthSquared() < 1e-20f) return;
		axisU.Normalize();
		const SimpleMath::Vector3 axisV = normal.Cross(axisU);

		std::vector<SimpleMath::Vector2> flat(n);
		for (size_t i = 0; i < n; ++i)
			flat[i] = SimpleMath::Vector2(vertices[face[i]].Dot(axisU), vertices[face[i]].Dot(axisV));

		float signedArea2 = 0.f;
		for (size_t i = 0; i < n; ++i)
		{
			const SimpleMath::Vector2& a = flat[i];
			const SimpleMath::Vector2& b = flat[(i + 1) % n];
			signedArea2 += a.x * b.y - b.x * a.y;
		}

		std::vector<size_t> remaining(n);
		for (size_t i = 0; i < n; ++i) remaining[i] = i;
		// Ear clipping below assumes counter-clockwise input; reverse if this projection came out clockwise.
		if (signedArea2 < 0.f) std::reverse(remaining.begin(), remaining.end());

		auto cross2 = [](const SimpleMath::Vector2& o, const SimpleMath::Vector2& a, const SimpleMath::Vector2& b)
			{ return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };

		size_t guard = 0;
		const size_t guardLimit = n * n + 16;   // hard bound: a malformed polygon must not spin the tag refresh
		while (remaining.size() > 3 && guard++ < guardLimit)
		{
			bool clippedAnEar = false;
			const size_t m = remaining.size();
			for (size_t i = 0; i < m; ++i)
			{
				const size_t ia = remaining[(i + m - 1) % m];
				const size_t ib = remaining[i];
				const size_t ic = remaining[(i + 1) % m];
				if (cross2(flat[ia], flat[ib], flat[ic]) <= 0.f) continue;   // reflex or collinear: not an ear

				bool containsAnother = false;
				for (size_t k = 0; k < m; ++k)
				{
					const size_t ip = remaining[k];
					if (ip == ia || ip == ib || ip == ic) continue;
					const SimpleMath::Vector2& p = flat[ip];
					if (cross2(flat[ia], flat[ib], p) >= 0.f
						&& cross2(flat[ib], flat[ic], p) >= 0.f
						&& cross2(flat[ic], flat[ia], p) >= 0.f)
					{
						containsAnother = true;
						break;
					}
				}
				if (containsAnother) continue;

				out.push_back(face[ia]); out.push_back(face[ib]); out.push_back(face[ic]);
				remaining.erase(remaining.begin() + (ptrdiff_t)i);
				clippedAnEar = true;
				break;
			}
			if (!clippedAnEar) break;   // self-intersecting or numerically degenerate; fall through to the fan
		}

		if (remaining.size() == 3)
		{
			out.push_back(face[remaining[0]]); out.push_back(face[remaining[1]]); out.push_back(face[remaining[2]]);
		}
		else if (remaining.size() > 3)
		{
			// Ear clipping stalled. A fan is wrong for a concave remainder, but it is never a crash and the
			// wireframe still conveys the true shape.
			for (size_t i = 1; i + 1 < remaining.size(); ++i)
			{
				out.push_back(face[remaining[0]]);
				out.push_back(face[remaining[i]]);
				out.push_back(face[remaining[i + 1]]);
			}
		}
	}

	void buildRenderGeometry(HceTriggerVolume& volume)
	{
		volume.renderVertices.clear();
		volume.triangleIndices.clear();
		volume.edgeIndices.clear();
		if (volume.vertices.empty() || volume.vertices.size() > 0xFFFFu) return;

		volume.renderVertices.reserve(volume.vertices.size());
		for (const auto& v : volume.vertices)
			volume.renderVertices.push_back(DirectX::VertexPosition(DirectX::XMFLOAT3(v.x, v.y, v.z)));

		volume.edgeIndices.reserve(volume.edges.size() * 2);
		for (const auto& [a, b] : volume.edges)
		{
			if (a >= volume.vertices.size() || b >= volume.vertices.size()) continue;
			volume.edgeIndices.push_back(a);
			volume.edgeIndices.push_back(b);
		}

		for (const auto& face : volume.faces)
			triangulateFace(volume.vertices, face, volume.triangleIndices);
	}

	void finaliseBounds(HceTriggerVolume& volume)
	{
		if (volume.vertices.empty()) return;
		SimpleMath::Vector3 sum{};
		for (const auto& v : volume.vertices) sum += v;
		volume.center = sum / (float)volume.vertices.size();
		float maxDistanceSquared = 0.f;
		for (const auto& v : volume.vertices)
		{
			const float d = (v - volume.center).LengthSquared();
			if (d > maxDistanceSquared) maxDistanceSquared = d;
		}
		volume.radius = std::sqrt(maxDistanceSquared);
	}

	// Point-in-volume, matching the engine's own convention: trigger_volume_test_point accepts
	// 0 <= local <= extents along (forward, left, up) FROM position - the box is NOT centred on position,
	// which is exactly what buildBox draws. Sectors are point-in-polygon on XY plus a z-range test.
	//
	// ⚠ APPROXIMATE ON PURPOSE. The engine tests a point the unit supplies; we use the player object origin,
	// the same point the Show Trigger Vertex sphere is drawn at. Entry can therefore be reported a fraction
	// before or after the engine acts. That is acceptable for a message; do not build anything that must agree
	// with the engine tick-for-tick on top of it.
	bool pointIsInside(const HceTriggerVolume& volume, const SimpleMath::Vector3& p)
	{
		// Cheap reject first - every zone-set volume is tested every frame.
		if ((p - volume.center).LengthSquared() > volume.radius * volume.radius) return false;

		if (volume.isSector && volume.sectorPolygon.size() >= 3)
		{
			if (p.z < volume.sectorZ0 || p.z > volume.sectorZ1) return false;
			bool inside = false;
			const size_t n = volume.sectorPolygon.size();
			for (size_t i = 0, j = n - 1; i < n; j = i++)
			{
				const SimpleMath::Vector2& a = volume.sectorPolygon[i];
				const SimpleMath::Vector2& b = volume.sectorPolygon[j];
				if ((a.y > p.y) != (b.y > p.y)
					&& p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
					inside = !inside;
			}
			return inside;
		}

		if (volume.boxExtents == SimpleMath::Vector3::Zero) return false;
		const SimpleMath::Vector3 d = p - volume.boxOrigin;
		const float lf = d.Dot(volume.boxForward);
		const float ll = d.Dot(volume.boxLeft);
		const float lu = d.Dot(volume.boxUp);
		return lf >= 0.f && lf <= volume.boxExtents.x
			&& ll >= 0.f && ll <= volume.boxExtents.y
			&& lu >= 0.f && lu <= volume.boxExtents.z;
	}

	// Oriented box. The engine's own point test accepts 0 <= local <= extents, so the box spans [0, extents]
	// FROM position along (forward, left, up) - it is not centred on position. The second axis is up X forward,
	// which is the exact cross product the engine's basis builder writes; in Blam that is LEFT, and extents.y
	// runs along it (see the header comment). This is the VOLUME's basis and has nothing to do with the camera
	// basis in makeCamera() - they are separate, and both are right for their own job.
	void buildBox(HceTriggerVolume& volume, const SimpleMath::Vector3& position, const SimpleMath::Vector3& extents,
		SimpleMath::Vector3 forward, SimpleMath::Vector3 up)
	{
		if (forward.LengthSquared() < 1e-12f) forward = SimpleMath::Vector3::UnitX;
		if (up.LengthSquared() < 1e-12f) up = SimpleMath::Vector3::UnitZ;
		forward.Normalize();
		up.Normalize();
		SimpleMath::Vector3 left = up.Cross(forward);
		if (left.LengthSquared() < 1e-12f) return;
		left.Normalize();

		// Kept for pointIsInside(): the same basis the engine's own point test uses, so containment agrees with
		// what is drawn rather than with a re-derived approximation.
		volume.boxOrigin = position; volume.boxForward = forward;
		volume.boxLeft = left;       volume.boxUp = up;
		volume.boxExtents = extents;

		volume.vertices.resize(8);
		for (int i = 0; i < 8; ++i)
			volume.vertices[i] = position
				+ forward * ((i & 1) ? extents.x : 0.f)
				+ left    * ((i & 2) ? extents.y : 0.f)
				+ up      * ((i & 4) ? extents.z : 0.f);

		static const uint16_t boxEdges[12][2] =
		{ {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
		volume.edges.reserve(12);
		for (const auto& e : boxEdges) volume.edges.emplace_back(e[0], e[1]);

		// Six quads, each fixing one axis bit and cycling the other two so the indices walk the perimeter rather
		// than crossing the face diagonally (a crossed order renders as an hourglass, not a quad).
		static const uint16_t boxFaces[6][4] =
		{ {0,2,6,4},{1,3,7,5},{0,1,5,4},{2,3,7,6},{0,1,3,2},{4,5,7,6} };
		volume.faces.reserve(6);
		for (const auto& f : boxFaces) volume.faces.push_back({ f[0], f[1], f[2], f[3] });
	}

	// Sector: an XY polygon extruded between two z bounds. Drawing these as their bounding box (which the
	// external tool did) is visibly wrong for every L-shaped or curved volume in the game.
	void buildSectorPrism(HceTriggerVolume& volume, const std::vector<SimpleMath::Vector2>& points, float z0, float z1)
	{
		const size_t n = points.size();
		if (n < 3) return;

		// Kept for pointIsInside(): point-in-polygon on the XY footprint plus a z-range test.
		volume.sectorPolygon = points;
		volume.sectorZ0 = std::min(z0, z1);
		volume.sectorZ1 = std::max(z0, z1);

		volume.vertices.reserve(n * 2);
		for (const auto& p : points) volume.vertices.emplace_back(p.x, p.y, z0);
		for (const auto& p : points) volume.vertices.emplace_back(p.x, p.y, z1);

		volume.edges.reserve(n * 3);
		for (size_t i = 0; i < n; ++i)
		{
			const uint16_t a = (uint16_t)i;
			const uint16_t b = (uint16_t)((i + 1) % n);
			volume.edges.emplace_back(a, b);                                         // bottom ring
			volume.edges.emplace_back((uint16_t)(a + n), (uint16_t)(b + n));         // top ring
			volume.edges.emplace_back(a, (uint16_t)(a + n));                         // vertical
		}

		volume.faces.reserve(n + 2);
		for (size_t i = 0; i < n; ++i)                                              // side walls, always convex
		{
			const uint16_t a = (uint16_t)i;
			const uint16_t b = (uint16_t)((i + 1) % n);
			volume.faces.push_back({ a, b, (uint16_t)(b + n), (uint16_t)(a + n) });
		}

		// Caps. NOTE: a sector polygon may be CONCAVE (L-shaped rooms are common), and ImGui's filled-polygon
		// path assumes convexity, so a concave cap fills slightly wrong. The side walls and the wireframe are
		// still exact, and this only affects the solid style. Triangulating properly would need an ear-clipper.
		std::vector<uint16_t> bottom, top;
		bottom.reserve(n); top.reserve(n);
		for (size_t i = 0; i < n; ++i) bottom.push_back((uint16_t)i);
		for (size_t i = n; i-- > 0; ) top.push_back((uint16_t)(i + n));   // reversed, so it winds the other way
		volume.faces.push_back(std::move(bottom));
		volume.faces.push_back(std::move(top));
	}
}


class HCETriggerOverlay::HCETriggerOverlayImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::weak_ptr<ModalDialogRenderer> modalDialogsWeak;   // the name picker, see onEditNameFilter

	// Structural offsets, from InternalPointerData.xml. The two ADDRESSES are resolved by signature instead -
	// see the header comment.
	int64_t mScenarioTriggerVolumeBlock = 0x278;

	// Zone-set switch trigger volumes. See InternalPointerData.xml for the derivation and the code proof that
	// the index field lives in the same index space as HceTriggerVolume::index.
	int64_t mScenarioZoneSetSwitchBlock = 0x29C;
	int64_t mZoneSetSwitchStride = 0x8;
	int64_t mZoneSetSwitchTriggerVolumeIndexOffset = 0x4;
	// The other two index fields of the SAME element. ⚠ All three are int16 with a LIVE -1 sentinel.
	int64_t mZoneSetSwitchBeginZoneSetOffset = 0x2;
	int64_t mZoneSetSwitchCommitZoneSetOffset = 0x6;
	int64_t mZoneSetSwitchFlagsOffset = 0x0;

	// The scenario's 'zone sets' block - what a begin/commit index points at. See InternalPointerData.xml.
	int64_t mScenarioZoneSetBlock = 0xD0;
	int64_t mZoneSetStride = 0x130;
	int64_t mZoneSetNameStringIdOffset = 0x0;
	int64_t mZoneSetNameStringOffset = 0x4;
	int64_t mZoneSetFlagsOffset = 0x108;
	int64_t mZoneSetBspZoneFlagsOffset = 0x10C;
	int64_t mZoneSetRuntimeBspZoneFlagsOffset = 0x114;

	// Kill volumes and safe zones - same shape, opposite meaning. See InternalPointerData.xml.
	uint32_t mLoggedEmptySpeedrunFor = 0;   // encoded block address we last warned about, see refreshVolumes

	int64_t mScenarioKillTriggerBlock = 0x45C;
	int64_t mScenarioSafeZoneTriggerBlock = 0x468;
	int64_t mKillTriggerStride = 0x4;
	int64_t mKillTriggerVolumeIndexOffset = 0x0;
	int64_t mTriggerVolumeStride = 0x7C;
	int64_t mTriggerVolumeNameOffset = 0x00;
	int64_t mTriggerVolumeTypeOffset = 0x0C;

	// UE5 render camera. The midhook and the POV read live in HCEGetCameraData now, because the D3D12 renderer
	// needs exactly the same camera and there must only ever be one hook on DoUpdateCamera. Optional: without it
	// the overlay still draws, using the sim's player-aim camera and a default FOV.
	std::optional<std::weak_ptr<HCEGetCameraData>> mCameraDataOptionalWeak;
	bool mCameraHookRequested = false;   // so a redundant release cannot unbalance the reference count

	int64_t mTriggerVolumeForwardOffset = 0x10;
	int64_t mTriggerVolumeUpOffset = 0x1C;
	int64_t mTriggerVolumePositionOffset = 0x28;
	int64_t mTriggerVolumeExtentsOffset = 0x34;
	int64_t mTriggerVolumeSectorPointsBlock = 0x44;
	int64_t mTriggerVolumeSectorPointStride = 0x14;
	int64_t mTriggerVolumeBoundsOffset = 0x5C;

	std::atomic<bool> mReady{ false };
	bool mIsActive = false;

	// ---- signature-resolved engine addresses, resolved once per session, lazily ----
	bool mAnchorsTried = false;
	bool mAnchorsGood = false;
	uintptr_t mScenarioSlot = 0;        // module global holding the scenario tag data pointer
	uintptr_t mTagAddressTable = 0;     // 16-entry table of tag block region bases
	FnStringIdText mStringIdText = nullptr;
	uintptr_t mStringIdTableSlot = 0;   // the global mStringIdText loads; null while a level is loading
	// The resident-BSP bitmask, bit i = scenario 'structure bsps' element i. OPTIONAL: without it the zone-set
	// entry report still names the zone set, it just cannot say which BSPs change.
	uintptr_t mLoadedBspMaskSlot = 0;
	std::string mAnchorFailure;

	// ---- cached geometry ----
	// GUARDED BY mVolumesMutex. Three different threads reach it: the render thread reads it every frame,
	// the hotkey/GUI thread rebuilds it on toggle, and the MCC state-hook thread clears it on a level change.
	// Without the lock a rebuild reallocates the vector out from under the render loop's iterator.
	// The render path holds it for the whole draw, which is fine - the other two are rare and brief.
	std::mutex mVolumesMutex;
	std::vector<HceTriggerVolume> mVolumes;
	// Parallel to mVolumes: the last-hit timestamp we have already reported for each. Resized (and re-seeded
	// without reporting) whenever mVolumes changes, which is what makes a level change not spam the feed.
	std::vector<uint32_t> mLastHitTicks;
	uint32_t mCachedEncodedAddress = 0;
	int32_t mCachedCount = -1;
	std::chrono::steady_clock::time_point mLastRefresh{};

	// scratch, reused every frame so the render path allocates nothing
	std::vector<SimpleMath::Vector3> mCameraSpace;
	// Reused per volume so solid rendering allocates nothing per frame.
	std::vector<std::pair<float, size_t>> mFaceOrder;   // (centroid depth, face index), sorted far -> near
	std::vector<ImVec2> mPoly;

	// ---- the 3D renderer path ----
	// Optional in every sense: Render3DEventProvider may fail to construct (no camera service, no D3D12
	// renderer), and Renderer3DImplD3D12 may fail to initialise later, on the render thread. Either way the
	// ImGui path below keeps working.
	std::optional<std::weak_ptr<Render3DEventProvider>> mRender3DProviderOptionalWeak;
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRender3DEventCallback;

	// Render-path lifetime gate. Replaces a ScopedAtomicBool + `mCurrentlyRendering3D.wait(true)` pair that had
	// two defects. ScopedAtomicBool is `wait(true); atom = true;` - a TOCTOU, not a lock; D3D12Hook.h documents
	// and rejects the same flaw. And the waiter had NO timeout, so if the render thread ever stalled inside
	// onRender3DEvent, whoever called set3DRenderingEnabled(false) blocked forever at 0% CPU - including the
	// state-hook thread, which reaches it on every game state change, and therefore on every revert.
	//
	// The render path now never blocks (try_to_lock, skip the frame), so our teardown cannot stall the game's
	// render thread, and the drain side is bounded. See dropRender3DSubscription.
	std::shared_timed_mutex mRender3DGuard;
	std::atomic_bool mRender3DDraining{ false };

	// Serialises set3DRenderingEnabled as a whole, so mRender3DEventCallback has ONE owner rather than being
	// guarded on the drop side only. Three unsynchronised threads reach that function: the state-hook poll
	// thread (onGameStateChanged), and a thread GUISimpleToggle spawns and DETACHES per checkbox click
	// (onToggleEvent). BinarySetting's own mutex serialises toggle-against-toggle but not toggle-against-state-
	// change, so without this an enable and a disable can race on the same unique_ptr: one resets (deletes) it
	// while the other stores into it, and a lost update leaves a live subscription that the destructor's drain
	// never sees - a callback into freed memory every frame. A revert reaches the disable side on the poll
	// thread, which is exactly when a click is likely to be in flight.
	//
	// NOT mRender3DGuard: dropRender3DSubscription takes that one exclusively, and shared_timed_mutex is not
	// recursive. Lock order is mRender3DSubscriptionMutex -> mRender3DGuard, one way only. The render path takes
	// neither by blocking - it try_locks the guard and bails - so it cannot close a cycle.
	std::mutex mRender3DSubscriptionMutex;

	// GetTickCount64 of the last frame the 3D path actually drew. The ImGui RenderEvent fires EARLIER in the
	// frame than the D3D12 render event, so the fallback cannot ask "did 3D draw this frame" - it asks "has 3D
	// drawn recently", which is also exactly the right question when the 3D path dies mid-session.
	std::atomic<uint64_t> mLast3DDrawTick{ 0 };
	static constexpr uint64_t kFallbackAfterMs = 500;

	bool threeDPathIsLive() const
	{
		const uint64_t last = mLast3DDrawTick.load(std::memory_order_acquire);
		if (last == 0) return false;
		const uint64_t now = GetTickCount64();
		return (now >= last) && (now - last) < kFallbackAfterMs;
	}

	// WHICH PATH IS DRAWING, reported on change.
	//
	// This handover was completely silent, which made it indistinguishable from a camera problem when the
	// overlay "felt desynced": the two paths are different renderers reading the camera through different code,
	// so a flip between them looks like the overlay jumping even when both are healthy. A log that says nothing
	// during a steady state and one line per flip settles which of the two is happening, instead of leaving it
	// to be inferred from a description.
	//
	// -1 = nothing decided yet, 0 = ImGui fallback, 1 = the D3D12 3D path.
	mutable std::atomic<int> mLoggedDrawPath{ -1 };
	mutable std::atomic<uint64_t> mLastDrawPathLogTick{ 0 };
	static constexpr uint64_t kDrawPathLogMinIntervalMs = 2000;

	void logDrawPathIfChanged(bool threeD) const
	{
		const int path = threeD ? 1 : 0;
		if (mLoggedDrawPath.load(std::memory_order_relaxed) == path) return;

		// Rate limit as well as latch: a path that flaps once per frame would otherwise bury the log, and that
		// is exactly the state most worth being able to read afterwards.
		const uint64_t now = GetTickCount64();
		const uint64_t lastLog = mLastDrawPathLogTick.load(std::memory_order_relaxed);
		if (lastLog != 0 && (now - lastLog) < kDrawPathLogMinIntervalMs)
		{
			mLoggedDrawPath.store(path, std::memory_order_relaxed);   // track it, just do not narrate every flip
			return;
		}
		mLastDrawPathLogTick.store(now, std::memory_order_relaxed);
		mLoggedDrawPath.store(path, std::memory_order_relaxed);

		if (threeD)
			PLOG_INFO << "HCETriggerOverlay is drawing through the D3D12 3D renderer (depth-tested)";
		else
			PLOG_WARNING << "HCETriggerOverlay fell back to the ImGui draw list - the 3D path has not drawn for "
				<< kFallbackAfterMs << " ms. The two paths resolve the camera through different code, so the "
				"overlay can visibly shift as it hands over. Whether this is a camera problem or a renderer one "
				"is answered by the HCEGetCameraData and Renderer3DImplD3D12 lines around this point.";
	}

	// Whether a mission script has tested this volume recently. Supplied by HCETriggerActivity, which patches
	// the 8 HaloScript call sites of trigger_volume_test_point (NOT its prologue - 15 of its 23 call sites are
	// AI/damage/safe-zone code that polls every volume every tick, which would report everything as live).
	//
	// Optional: if the tracker failed to construct (an unrecognised build fails its patches closed), the overlay
	// still draws, everything just reads as dormant.
	std::optional<std::weak_ptr<HCETriggerActivity>> mTriggerActivityOptionalWeak;

	// The per-category "Types Shown" filter, read once per frame so neither draw path pays for it per volume.
	struct TypeFilter
	{
		bool speedrunOnly = false;
		// ⚠ There is deliberately NO sector toggle. "Sector" is a SHAPE - an XY polygon extruded between two
		// heights, rather than a box - not a kind of trigger, and the engine gives it no different meaning. It
		// used to have its own category, toggle and colour, which split the plain-trigger list in two along a
		// line the user never cares about. Sectors are now regular triggers that happen to be prisms; the prism
		// GEOMETRY is still built and drawn exactly as before (see buildSectorPrism).
		bool showRegular = true, showKill = true, showZoneSet = true, showBeginZoneSet = true;
		bool showCheckpointGrant = true;

		// NAME FILTER. Shares MCC's triggerOverlayFilterString / FilterToggle / FilterExactMatch settings, so a
		// preset made on either side means the same thing - the two can never run in one process.
		//
		// ⚠ EMPTY MEANS EVERYTHING. Switching the filter on with nothing chosen must not blank the overlay and
		// leave the user hunting for what broke, so "draw nothing" needs its own value; the picker writes
		// HCETriggerNameFilterDialog::kNoneSentinel for that. Built once per frame, never per volume.
		bool nameFilterActive = false;
		bool exactMatch = true;
		std::unordered_set<std::string> names;   // exact-match mode
		std::vector<std::string> substrings;     // substring mode, lowercased
	};

	static TypeFilter readTypeFilter(const std::shared_ptr<SettingsStateAndEvents>& settings)
	{
		TypeFilter f;
		f.speedrunOnly = settings->hceTriggerOverlaySpeedrunOnly->GetValue();
		f.showRegular = settings->hceTriggerOverlayShowRegular->GetValue();
		f.showKill = settings->hceTriggerOverlayShowKill->GetValue();
		f.showZoneSet = settings->hceTriggerOverlayShowZoneSet->GetValue();
		f.showBeginZoneSet = settings->hceTriggerOverlayShowBeginZoneSet->GetValue();
		f.showCheckpointGrant = settings->hceTriggerOverlayShowCheckpointGrant->GetValue();

		if (settings->triggerOverlayFilterToggle->GetValue())
		{
			f.exactMatch = settings->triggerOverlayFilterExactMatch->GetValue();
			const std::string raw = settings->triggerOverlayFilterString->GetValue();

			std::string current;
			auto push = [&]()
				{
					while (!current.empty() && (current.back() == ' ' || current.back() == '\t')) current.pop_back();
					if (current.empty()) return;
					if (f.exactMatch) f.names.insert(current);
					else
					{
						std::string lowered;
						for (char c : current) lowered += (char)std::tolower((unsigned char)c);
						f.substrings.push_back(lowered);
					}
					current.clear();
				};
			for (char c : raw)
			{
				if (c == ';' || c == '\n' || c == '\r') push();
				else if (c != ' ' || !current.empty()) current += c;
			}
			push();

			// An empty list is "no filter", NOT "hide everything" - see the note on TypeFilter.
			f.nameFilterActive = !f.names.empty() || !f.substrings.empty();
		}
		return f;
	}

	// ONE definition of "is this volume drawn", shared by the 3D and ImGui paths so they cannot drift apart.
	// Categories are tested in the SAME priority the colours use - zone-set switch, then kill, then regular - so
	// a volume that is both a zone-set switch and a kill volume is governed by the zone-set toggle and drawn in
	// the zone-set colour. Anything else would let a volume be hidden by one toggle while wearing the colour of
	// another. Sector-shaped volumes are regular triggers here; shape is not a category.
	static bool shouldDrawVolume(const HceTriggerVolume& volume, const TypeFilter& filter)
	{
		if (filter.speedrunOnly && !volume.isSpeedrun) return false;

		// Name filter first: it is the most specific thing the user can have asked for, and an explicit list of
		// names should not be quietly overruled by a category toggle they forgot was off. The sentinel the
		// picker writes for "None" matches no real volume, so it hides everything without a special case.
		if (filter.nameFilterActive)
		{
			if (filter.exactMatch)
			{
				if (!filter.names.contains(volume.name)) return false;
			}
			else
			{
				std::string lowered;
				lowered.reserve(volume.name.size());
				for (char c : volume.name) lowered += (char)std::tolower((unsigned char)c);
				bool any = false;
				for (const auto& needle : filter.substrings)
					if (lowered.find(needle) != std::string::npos) { any = true; break; }
				if (!any) return false;
			}
		}

		// COMMIT outranks BEGIN for the same reason the engine picks it when both fire on one tick: it is the
		// more consequential event. Shipped data has no volume with both, but the struct allows it.
		if (volume.isCommitZoneSet)             return filter.showZoneSet;
		if (volume.isBeginZoneSet)              return filter.showBeginZoneSet;
		// Below the zone-set pair on purpose: crossing one of those reloads the world, which outranks
		// "this also happens to save". Above kill, because a checkpoint is the thing a runner is looking for.
		//
		// ⚠ THE ONLY NON-EXCLUSIVE CATEGORY. A plain checkpoint-grant volume IS a regular trigger that also
		// happens to save, so it shows under EITHER toggle - turning Regular on must not make checkpoints
		// disappear from it. The Checkpoint Grant toggle stays exact in the other direction: on its own it
		// still reveals checkpoint volumes and nothing else.
		// A grant that is ALSO a kill or safe zone stays out of Regular - that one has its own filter, and
		// folding it in would put a "stay out or die" volume in the harmless set.
		if (volume.isCheckpointGrant)
		{
			if (!(volume.isKill || volume.isSafeZone) && filter.showRegular) return true;
			return filter.showCheckpointGrant;
		}
		if (volume.isKill || volume.isSafeZone) return filter.showKill;   // one toggle, two colours
		return filter.showRegular;                                        // sectors included - shape, not category
	}

	bool isVolumeActive(uint32_t volumeIndex) const
	{
		if (!mTriggerActivityOptionalWeak.has_value()) return false;
		auto activity = mTriggerActivityOptionalWeak.value().lock();
		return activity && activity->isVolumeActive(volumeIndex);
	}

	// "A script tested this volume and you were INSIDE it" - the event, not the polling.
	//
	// ⚠ THIS IS NOT isVolumeActive. Active stays true for a multi-second window because scripts poll in
	// bursts; a HIT is instantaneous. Conflating them is why entering a volume used to look like the volume
	// slowly going dormant several seconds later instead of flashing the moment you crossed it.
	bool isVolumeHit(uint32_t volumeIndex, uint32_t flashMilliseconds) const
	{
		if (!mTriggerActivityOptionalWeak.has_value()) return false;
		auto activity = mTriggerActivityOptionalWeak.value().lock();
		return activity && activity->isVolumeHit(volumeIndex, flashMilliseconds);
	}

	uint32_t volumeLastHitTick(uint32_t volumeIndex) const
	{
		if (!mTriggerActivityOptionalWeak.has_value()) return 0;
		auto activity = mTriggerActivityOptionalWeak.value().lock();
		return activity ? activity->getLastHitTick(volumeIndex) : 0;
	}

	// Edge-triggered "you just hit these" report, for the message feed. Keyed on each volume's last-hit
	// timestamp changing, with the previous values held HERE rather than in HCETriggerActivity - that class
	// has no idea when a level changed and would otherwise carry stale per-scenario state.
	void reportNewHits(const std::shared_ptr<IMessagesGUI>& messagesGUI)
	{
		if (mVolumes.empty()) return;
		if (mLastHitTicks.size() != mVolumes.size())
		{
			// First pass after a rebuild: seed WITHOUT reporting, or every volume already hit this session
			// would announce itself the moment the overlay is switched on.
			mLastHitTicks.assign(mVolumes.size(), 0);
			for (size_t i = 0; i < mVolumes.size(); ++i)
				mLastHitTicks[i] = volumeLastHitTick(mVolumes[i].index);
			return;
		}

		for (size_t i = 0; i < mVolumes.size(); ++i)
		{
			const uint32_t tick = volumeLastHitTick(mVolumes[i].index);
			if (tick == mLastHitTicks[i]) continue;
			mLastHitTicks[i] = tick;
			if (tick == 0) continue;   // cleared, not hit

			messagesGUI->addMessage(std::format("Trigger hit: {}", mVolumes[i].name));
		}
	}

	// Re-seeded WITHOUT reporting on any rebuild, or a level change would announce every volume you happen to
	// be standing in. Same trap, same fix, as reportNewHits.
	bool mZoneSetInsideSeeded = false;

	// "You just entered a zone-set volume; here is what it does."
	//
	// ⚠ GEOMETRIC, not an engine event, and that is forced rather than chosen: HCETriggerActivity patches only
	// the EIGHT HaloScript call sites of trigger_volume_test_point, and the zone-set evaluator sub_18018F870
	// is on the deliberately-NOT-patched list - so a zone-set volume can never produce a script "hit" and
	// reportNewHits can never fire for one.
	//
	// CALLER MUST HOLD mVolumesMutex.
	void reportZoneSetEntries(const std::shared_ptr<IMessagesGUI>& messagesGUI,
		const SimpleMath::Vector3& playerPoint, bool reportZoneSets, bool reportCheckpointNotes)
	{
		const bool seeding = !mZoneSetInsideSeeded;
		mZoneSetInsideSeeded = true;

		// Live state, read ONCE per frame: one dword. Everything else is cached tag data.
		uint32_t loadedBsps = 0;
		bool haveLoaded = false;
		if (mLoadedBspMaskSlot)
			haveLoaded = HCEGetPlayerState::tryReadRaw(mLoadedBspMaskSlot, &loadedBsps, sizeof(loadedBsps));

		for (HceTriggerVolume& volume : mVolumes)
		{
			const bool wantZoneSet = reportZoneSets && volume.isBspSwitch;
			// ⚠ ONLY volumes whose grant is CONDITIONAL get a note. An unconditional checkpoint needs no
			// explanation - saying "this gives a checkpoint" on all 285 of them would be noise, and the user
			// asked for a print only where there is a distinction to report.
			const bool wantCheckpoint = reportCheckpointNotes
				&& volume.isCheckpointGrant && !volume.checkpointNote.empty();
			if (!wantZoneSet && !wantCheckpoint) continue;

			const bool inside = pointIsInside(volume, playerPoint);
			const bool wasInside = volume.playerWasInside;
			volume.playerWasInside = inside;
			if (seeding || !inside || wasInside) continue;   // report only the false -> true edge

			if (wantCheckpoint)
				messagesGUI->addMessage(std::format("Checkpoint: {}  [{}]", volume.checkpointNote, volume.name));

			if (!wantZoneSet) continue;

			std::string line = std::format("{}  [{}]", volume.zoneSetReportPrefix, volume.name);

			// The consequence, from tag data plus the one live mask. Both sides are bitmasks over the SAME
			// index space (scenario 'structure bsps'), which is what makes the diff meaningful.
			const int16_t target = volume.isCommitZoneSet ? volume.commitZoneSet : volume.beginZoneSet;
			if (haveLoaded && target >= 0 && (size_t)target < mZoneSetWantedBsps.size())
			{
				const uint32_t wanted = mZoneSetWantedBsps[(size_t)target];
				if (volume.isCommitZoneSet)
				{
					// COMMIT passes the full target mask, so load = wanted & ~loaded.
					const std::string load = bitList(wanted & ~loadedBsps, mStructureBspCount);
					const std::string drop = bitList(loadedBsps & ~wanted, mStructureBspCount);
					line += std::format("\n  loads BSP: {}   unloads BSP: {}",
						load.empty() ? "none" : load, drop.empty() ? "none" : drop);
				}
				else
				{
					// BEGIN passes (loaded & wanted), so its load set is identically zero - purely subtractive.
					const std::string drop = bitList(loadedBsps & ~wanted, mStructureBspCount);
					line += std::format("\n  loads BSP: none (prepare only)   unloads BSP: {}",
						drop.empty() ? "none" : drop);
					// The engine gate: no shared BSP means it skips the prepare and full-switches instead.
					if ((loadedBsps & wanted) == 0)
						line += "\n  (no overlap with what is resident - the engine will full-switch instead)";
				}
				line += std::format("\n  resident now: {}", bitList(loadedBsps, mStructureBspCount));
			}
			else if (target >= 0)
			{
				line += "\n  (resident-BSP mask unavailable on this build - see the log)";
			}

			messagesGUI->addMessage(line);
		}
	}

	// The call-site patches only exist while the overlay wants them - no reason to carry them otherwise.
	void setActivityTracking(bool enabled)
	{
		if (!mTriggerActivityOptionalWeak.has_value()) return;
		if (auto activity = mTriggerActivityOptionalWeak.value().lock()) activity->setEnabled(enabled);
	}

	// ---------------------------------------------------------------------------------------------------------
	// enc -> real address. tagAddressTable[enc >> 28] + 4 * enc. The high nibble selects one of 16 region bases,
	// so this is NOT expressible as a single flat magic offset (which is why HCM's existing TagBlockReader
	// pimpls cannot be reused as-is).
	// ---------------------------------------------------------------------------------------------------------
	uintptr_t resolveTagBlock(uint32_t encoded) const
	{
		if (encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		const uintptr_t regionBase = readPtr(mTagAddressTable + 8ull * (encoded >> 28));
		if (!plausiblePointer(regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	void resolveAnchors(uintptr_t simBase) // never throws
	{
		mAnchorsTried = true;
		mAnchorsGood = false;
		mAnchorFailure.clear();

		int hits = 0;

		// mov rax, [rip+scenarioDataPtr] ; cmp ecx, [rax+278h]
		// Note the "78 02" inside the pattern IS the trigger-volume count compare, so the anchor is literally
		// the engine's own trigger volume access.
		const uintptr_t scenarioInsn = resolveUniqueMatch(simBase, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!scenarioInsn) { mAnchorFailure = std::format("scenario data pointer ({} matches)", hits); return; }
		mScenarioSlot = ripTarget(scenarioInsn, 3, 7);

		// mov rbx, rcx ; lea r8, [rip+tagAddressTable] ; ... ; mov rax, [r8+rax*8] ...
		const uintptr_t tableInsn = resolveUniqueMatch(simBase,
			"48 8B D9 4C 8D 05 ?? ?? ?? ?? 8B 09 8B 53 08 8B C2 48 C1 E8 1C 49 8B 04 C0 3B 4C 90 10 7D", hits);
		if (!tableInsn) { mAnchorFailure = std::format("tag address table ({} matches)", hits); return; }
		mTagAddressTable = ripTarget(tableInsn, 6, 10);

		if (!mScenarioSlot || !mTagAddressTable) { mAnchorFailure = "rip-relative operand read failed"; return; }

		// string_id -> text. Optional: without it every volume is still drawn, just named "trigger_N".
		const uintptr_t sidFunction = resolveUniqueMatch(simBase,
			"40 53 55 56 57 48 83 EC 28 48 8B 3D ?? ?? ?? ?? 48 63 E9", hits);
		if (sidFunction)
		{
			// The function's 6th instruction is `mov rdi, [rip+stringTable]`; verify those three opcode bytes
			// before trusting the operand, so a pattern that ever matched something else cannot make us call
			// through a bad pointer.
			const uintptr_t loadInsn = sidFunction + 9;
			uint8_t opcode[3]{};
			if (HCEGetPlayerState::tryReadRaw(loadInsn, opcode, sizeof(opcode))
				&& opcode[0] == 0x48 && opcode[1] == 0x8B && opcode[2] == 0x3D)
			{
				mStringIdTableSlot = ripTarget(loadInsn, 3, 7);
				if (mStringIdTableSlot) mStringIdText = (FnStringIdText)sidFunction;
			}
		}

		// OPTIONAL, and deliberately never a reason to disable the overlay: it only enriches the zone-set entry
		// report with which BSPs load and unload.
		mLoadedBspMaskSlot = HCEAnchors::get(HCEAnchors::Anchor::LoadedBspZoneFlags);
		if (!mLoadedBspMaskSlot)
			PLOG_WARNING << "HCETriggerOverlay: the resident-BSP mask anchor did not resolve; zone-set entry "
				"reports will name the zone set but omit the BSP load/unload lines.";

		mAnchorsGood = true;
	}

	std::vector<std::string> mZoneSetNames;     // indexed by zone-set block index; rebuilt with mVolumes
	std::vector<uint32_t> mZoneSetWantedBsps;   // parallel: element +0x114, falling back to +0x10C
	int32_t mStructureBspCount = 0;             // scenario+0x60, caps the bit loop

	// Reads the scenario 'zone sets' block once per refresh, so the entry report allocates nothing later.
	// Called from refreshVolumes AFTER the trigger-volume block has validated - never on its own.
	void refreshZoneSets(uintptr_t scenario)
	{
		mZoneSetNames.clear();
		mZoneSetWantedBsps.clear();
		mStructureBspCount = 0;

		const int32_t bspCount = readI32(scenario + 0x60);   // 'structure bsps' count; caps the bit loop
		if (bspCount > 0 && bspCount <= 32) mStructureBspCount = bspCount;

		const int32_t count = readI32(scenario + mScenarioZoneSetBlock);
		const uint32_t encoded = readU32(scenario + mScenarioZoneSetBlock + 4);
		// 64 is the declared k_maximum_scenario_zone_set_count. A count of 0 means the block pointer is wrong -
		// every shipped level has at least 4, and always at least one INTERNAL zone set.
		if (count <= 0 || count > 64 || encoded == 0 || encoded == 0xFFFFFFFFu) return;

		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausiblePointer(base)) return;

		mZoneSetNames.resize((size_t)count);
		mZoneSetWantedBsps.assign((size_t)count, 0u);

		char zoneNameBuffer[257]{};
		for (int32_t i = 0; i < count; ++i)
		{
			const uintptr_t element = base + (uintptr_t)mZoneSetStride * i;

			// PRIMARY: the literal char[256] at +0x04. No call into the game, no thread considerations, no
			// null-table gate - and it was read out of the shipped tag data of ALL 13 levels, so it is known
			// populated (a50 = set_landing / set_lift_approach / ...; c20 = set_floor_1..4; etc).
			std::memset(zoneNameBuffer, 0, sizeof(zoneNameBuffer));
			HCEGetPlayerState::tryReadRaw(element + mZoneSetNameStringOffset, zoneNameBuffer, 256);
			zoneNameBuffer[256] = '\0';
			std::string name;
			for (int c = 0; c < 256; ++c)
			{
				const char ch = zoneNameBuffer[c];
				if (ch == '\0') break;
				if (ch < 0x20 || ch > 0x7E) break;   // stop at the first non-printable rather than show garbage
				name.push_back(ch);
			}

			// FALLBACK: the string_id at +0x00, through the same SEH-wrapped, table-gated call the volume names
			// already use. The engine itself names zone sets this way.
			if (name.empty() && mStringIdText && readPtr(mStringIdTableSlot) != 0)
			{
				char sidBuffer[64]{};
				if (callStringIdText(mStringIdText, readU32(element + mZoneSetNameStringIdOffset),
					sidBuffer, sizeof(sidBuffer)))
					name = sidBuffer;
			}

			// LAST RESORT: every level ships exactly one unnamed zone set with flags bit2 ("internal zone set")
			// whose runtime BSP mask is every BSP. An empty name is NOT a read failure there.
			if (name.empty())
				name = (readU32(element + mZoneSetFlagsOffset) & 0x4u)
					? std::format("(internal zone set {})", i)
					: std::format("zone set {}", i);

			mZoneSetNames[(size_t)i] = std::move(name);

			// +0x114 is what every engine read uses. It is a runtime field, so fall back to the authored +0x10C
			// if it reads zero while +0x10C does not.
			uint32_t wanted = readU32(element + mZoneSetRuntimeBspZoneFlagsOffset);
			if (wanted == 0) wanted = readU32(element + mZoneSetBspZoneFlagsOffset);
			mZoneSetWantedBsps[(size_t)i] = wanted;
		}
	}

	std::string zoneSetName(int16_t index) const
	{
		if (index < 0) return "(none)";
		if ((size_t)index >= mZoneSetNames.size()) return std::format("zone set {} (out of range)", index);
		return mZoneSetNames[(size_t)index];
	}

	// The part of the report that cannot change between refreshes.
	std::string makeZoneSetPrefix(const HceTriggerVolume& volume) const
	{
		if (!volume.isBspSwitch) return {};
		std::string out;
		if (volume.isCommitZoneSet)
			out += std::format("Zone Set -> {}", zoneSetName(volume.commitZoneSet));
		if (volume.isBeginZoneSet)
		{
			if (!out.empty()) out += " + ";
			out += std::format("Begin Zone Set -> {}", zoneSetName(volume.beginZoneSet));
		}
		if (out.empty()) out = "Zone Set volume (target unknown - matched by name only)";
		if (volume.zoneSetSwitchFlags & 0x1u) out += " [teleport vehicles]";
		return out;
	}

	static std::string bitList(uint32_t mask, int32_t cap)
	{
		std::string out;
		const int32_t limit = (cap > 0 && cap <= 32) ? cap : 32;
		for (int32_t b = 0; b < limit; ++b)
		{
			if (!(mask & (1u << b))) continue;
			if (!out.empty()) out += ',';
			out += std::to_string(b);
		}
		return out;
	}

	// Rebuilds mVolumes from the scenario tag. Returns false when there is nothing to draw (no level loaded,
	// no trigger volumes, or the data looked implausible). CALLER MUST HOLD mVolumesMutex.
	bool refreshVolumes()
	{
		const uintptr_t scenario = readPtr(mScenarioSlot);
		if (!plausiblePointer(scenario)) { mVolumes.clear(); mCachedCount = -1; mCachedEncodedAddress = 0; return false; }

		const int32_t count = readI32(scenario + mScenarioTriggerVolumeBlock);
		const uint32_t encoded = readU32(scenario + mScenarioTriggerVolumeBlock + 4);

		// Sanity gates carried over from the reference reader. MAXIMUM_TRIGGER_VOLUMES_PER_SCENARIO is 350;
		// 8192 is deliberately loose, it only has to reject "we are reading a non-scenario".
		if (count <= 0 || count > 8192 || encoded == 0 || encoded == 0xFFFFFFFFu)
		{
			mVolumes.clear();
			mCachedCount = count;
			mCachedEncodedAddress = encoded;
			return false;
		}

		// (encodedAddress, count) is the level-change detector: it is the pair the reference tool rebuilt on,
		// and it changes on every scenario swap.
		if (count == mCachedCount && encoded == mCachedEncodedAddress && !mVolumes.empty()) return true;

		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausiblePointer(base)) { mVolumes.clear(); return false; }

		// Zone-set names and BSP masks, BEFORE the switch-block walk, so makeZoneSetPrefix has names to use
		// when classification runs below.
		refreshZoneSets(scenario);

		// Which volumes actually switch zone set / BSP. Read from the scenario's own block, so this is tag
		// truth rather than the name heuristic - a volume that switches the world without "bsp" in its name is
		// still caught. The name heuristic stays as a FALLBACK below for the case where this block is empty or
		// unreadable, because a missed switch volume is worse than a false positive.
		// Every one of these blocks has the same shape: count, encoded address, then N elements each carrying a
		// trigger-volume index with 0xFFFF meaning "none". Read them the same way.
		auto collectVolumeIndices = [&](int64_t blockOffset, int64_t stride, int64_t indexOffset)
			{
				std::unordered_set<uint32_t> indices;
				const int32_t blockCount = readI32(scenario + blockOffset);
				const uint32_t blockEncoded = readU32(scenario + blockOffset + 4);

				// Loose gate, deliberately: the numeric max for the kill/safe blocks is not reliably readable
				// from their definitions, so this only has to reject "we are not looking at a scenario".
				if (blockCount <= 0 || blockCount > 8192 || blockEncoded == 0 || blockEncoded == 0xFFFFFFFFu)
					return indices;

				const uintptr_t blockBase = resolveTagBlock(blockEncoded);
				if (!plausiblePointer(blockBase)) return indices;

				for (int32_t e = 0; e < blockCount; ++e)
				{
					const uint16_t volumeIndex = readU16(blockBase + (uintptr_t)stride * e + indexOffset);
					if (volumeIndex == 0xFFFF) continue;   // the engine's own "none" sentinel
					indices.insert(volumeIndex);
				}
				return indices;
			};

		// The zone-set switch block needs more than a set of indices: each element carries TWO zone-set indices,
		// and which one is set is the whole category. Same walk, same gates, richer result.
		struct ZoneSetSwitchTargets { int16_t begin = -1; int16_t commit = -1; uint16_t flags = 0; };
		std::unordered_map<uint32_t, ZoneSetSwitchTargets> zoneSetSwitchVolumes;
		{
			const int32_t blockCount = readI32(scenario + mScenarioZoneSetSwitchBlock);
			const uint32_t blockEncoded = readU32(scenario + mScenarioZoneSetSwitchBlock + 4);
			// 8192 stays as the loose "is this even a scenario" gate, matching collectVolumeIndices.
			if (blockCount > 0 && blockCount <= 8192 && blockEncoded != 0 && blockEncoded != 0xFFFFFFFFu)
			{
				const uintptr_t blockBase = resolveTagBlock(blockEncoded);
				if (plausiblePointer(blockBase))
				{
					for (int32_t e = 0; e < blockCount; ++e)
					{
						const uintptr_t element = blockBase + (uintptr_t)mZoneSetSwitchStride * e;
						const int16_t volumeIndex = (int16_t)readU16(element + mZoneSetSwitchTriggerVolumeIndexOffset);
						if (volumeIndex < 0) continue;             // the engine skips these outright
						const int16_t begin = (int16_t)readU16(element + mZoneSetSwitchBeginZoneSetOffset);
						const int16_t commit = (int16_t)readU16(element + mZoneSetSwitchCommitZoneSetOffset);
						const uint16_t flags = readU16(element + mZoneSetSwitchFlagsOffset);

						// Accumulate rather than assign: one trigger volume MAY appear in more than one element
						// (shipped data never does, but the format allows it), and losing a begin because a later
						// element only set a commit would silently drop a category.
						auto& slot = zoneSetSwitchVolumes[(uint32_t)volumeIndex];
						if (slot.begin < 0) slot.begin = begin;
						if (slot.commit < 0) slot.commit = commit;
						slot.flags |= flags;
					}
				}
			}
		}

		const std::unordered_set<uint32_t> killVolumes =
			collectVolumeIndices(mScenarioKillTriggerBlock, mKillTriggerStride, mKillTriggerVolumeIndexOffset);
		const std::unordered_set<uint32_t> safeZoneVolumes =
			collectVolumeIndices(mScenarioSafeZoneTriggerBlock, mKillTriggerStride, mKillTriggerVolumeIndexOffset);

		std::vector<HceTriggerVolume> built;
		built.reserve((size_t)count);

		std::vector<SimpleMath::Vector2> sectorPoints;
		char nameBuffer[64]{};

		for (int i = 0; i < count; ++i)
		{
			const uintptr_t element = base + (uintptr_t)mTriggerVolumeStride * i;

			HceTriggerVolume volume;
			volume.index = (uint32_t)i;   // scenario block index - the identity the engine's own test call uses

			// The engine's own trigger_volume_test_point branches on this field being NON-ZERO, not on it
			// being exactly 1, so match that. A non-zero type takes the sector path and falls through to the
			// box below only if the polygon is unusable.
			if (readU16(element + mTriggerVolumeTypeOffset) != 0)   // non-zero = sector
			{
				const int32_t pointCount = readI32(element + mTriggerVolumeSectorPointsBlock);
				const uintptr_t pointBase = resolveTagBlock(readU32(element + mTriggerVolumeSectorPointsBlock + 4));
				if (plausiblePointer(pointBase) && pointCount >= 3 && pointCount <= 256)
				{
					sectorPoints.clear();
					sectorPoints.reserve((size_t)pointCount);
					for (int k = 0; k < pointCount; ++k)
					{
						const uintptr_t p = pointBase + (uintptr_t)mTriggerVolumeSectorPointStride * k;
						sectorPoints.emplace_back(readF32(p), readF32(p + 4));
					}
					// bounds are x0,x1,y0,y1,z0,z1 - the prism is extruded between the last two
					const float z0 = readF32(element + mTriggerVolumeBoundsOffset + 16);
					const float z1 = readF32(element + mTriggerVolumeBoundsOffset + 20);
					buildSectorPrism(volume, sectorPoints, z0, z1);
					volume.isSector = true;
				}
			}

			if (volume.vertices.empty())   // box, or a sector with unusable points
			{
				const SimpleMath::Vector3 extents = readVec3(element + mTriggerVolumeExtentsOffset);
				if (extents.x == 0.f && extents.y == 0.f && extents.z == 0.f) continue;   // degenerate, nothing to draw
				buildBox(volume,
					readVec3(element + mTriggerVolumePositionOffset),
					extents,
					readVec3(element + mTriggerVolumeForwardOffset),
					readVec3(element + mTriggerVolumeUpOffset));
				volume.isSector = false;
			}

			if (volume.vertices.empty()) continue;
			finaliseBounds(volume);
			// Vertex/index buffers for the 3D renderer, built HERE (4x a second at most) so the render thread
			// never triangulates and never allocates.
			buildRenderGeometry(volume);

			if (mStringIdText && readPtr(mStringIdTableSlot) != 0
				&& callStringIdText(mStringIdText, readU32(element + mTriggerVolumeNameOffset), nameBuffer, sizeof(nameBuffer)))
				volume.name = nameBuffer;
			else
				volume.name = std::format("trigger_{}", i);

			// Classify once, here - never per frame. Tag data first, name heuristic only as a fallback.
			//
			// ⚠ ORDER IS LOAD-BEARING. isBspOrZoneSetTrigger uses a SUBSTRING test on "zone_set", and
			// "begin_zone_set:hangar" contains "zone_set" - testing the plain one first would put every begin
			// volume in the commit bucket. Begin is tested first; do not reorder.
			if (auto it = zoneSetSwitchVolumes.find(volume.index); it != zoneSetSwitchVolumes.end())
			{
				volume.beginZoneSet = it->second.begin;
				volume.commitZoneSet = it->second.commit;
				volume.zoneSetSwitchFlags = it->second.flags;
				volume.isBeginZoneSet = (it->second.begin >= 0);
				volume.isCommitZoneSet = (it->second.commit >= 0);
			}
			else if (HCESpeedrunTriggerNames::isBeginZoneSetTrigger(volume.name))
			{
				// Name heuristic, fallback only (the block is authoritative). The authoring prefixes ship in the
				// binary as literals with ZERO code xrefs, so the convention is tool-side - it can only ever be a
				// fallback for a volume the block does not cover. We know nothing about the TARGET here, so the
				// indices stay -1 and the entry report degrades to the name alone.
				volume.isBeginZoneSet = true;
			}
			else if (HCESpeedrunTriggerNames::isBspOrZoneSetTrigger(volume.name))
			{
				volume.isCommitZoneSet = true;
			}
			volume.isBspSwitch = volume.isBeginZoneSet || volume.isCommitZoneSet;
			// Tag truth. The name heuristic is only a fallback for kill volumes, and safe zones have no name
			// convention at all - they can ONLY be found by walking the block.
			volume.isKill = killVolumes.contains(volume.index)
				|| HCESpeedrunTriggerNames::isKillTrigger(volume.name);
			volume.isSafeZone = safeZoneVolumes.contains(volume.index);
			volume.isSpeedrun = HCESpeedrunTriggerNames::isSpeedrunTrigger(volume.name);

			// ⚠ SCRIPT data, not tag data. Whether entering a volume grants a checkpoint is decided by the
			// level's HaloScript, and the scenario format carries no flag for it - so unlike every other
			// category this is a curated name list, swept from the scripts of all 13 levels.
			// An empty note means the grant is unconditional; a non-empty one is shown when the player enters.
			{
				const auto grant = HCECheckpointGrantTriggers::lookup(volume.name);
				volume.isCheckpointGrant = grant.isGrant;
				volume.checkpointNote = grant.note;
			}

			volume.zoneSetReportPrefix = makeZoneSetPrefix(volume);

			built.push_back(std::move(volume));
		}

		mVolumes = std::move(built);
		mZoneSetInsideSeeded = false;   // re-seed without reporting; see reportZoneSetEntries

		// If the speedrun filter would hide EVERYTHING, say so and dump what the level actually calls its
		// volumes. The filter is a fixed name list transcribed from the community completion-requirement docs,
		// and the failure mode is silent: a level whose volumes are named differently just renders empty, which
		// looks identical to "this level has no speedrun triggers". Observed on The Maw.
		// Latched per scenario (mLoggedEmptySpeedrunFor), so this cannot spam on the 4x/second refresh.
		{
			size_t speedrunMatches = 0;
			for (const auto& v : mVolumes) if (v.isSpeedrun) ++speedrunMatches;

			if (speedrunMatches == 0 && !mVolumes.empty() && mLoggedEmptySpeedrunFor != encoded)
			{
				mLoggedEmptySpeedrunFor = encoded;
				std::string names;
				size_t listed = 0;
				for (const auto& v : mVolumes)
				{
					if (listed >= 80) { names += " ...(truncated)"; break; }
					if (!names.empty()) names += ", ";
					names += v.name;
					++listed;
				}
				PLOG_WARNING << "HCE Trigger Overlay: NONE of this level's " << mVolumes.size()
					<< " trigger volumes matched the speedrun name list, so 'Speedrun Triggers Only' will draw "
					"nothing here. The level's actual volume names are: " << names;
			}
		}
		mCachedCount = count;
		mCachedEncodedAddress = encoded;
		return !mVolumes.empty();
	}

	// ---------------------------------------------------------------------------------------------------------
	// Projection. Blam's frame: right-handed, Z up, +X forward at yaw 0, +Y LEFT - which is the frame the view
	// angle, the camera position and the trigger tag data are all defined in.
	//     forward = ( cos(pitch)cos(yaw),  cos(pitch)sin(yaw), sin(pitch) )
	//     right   = ( sin(yaw),           -cos(yaw),           0          )   <- forward x UnitZ. NOT (-sin, cos, 0).
	//     up      = (-sin(pitch)cos(yaw), -sin(pitch)sin(yaw), cos(pitch) )   == right x forward
	// The sign of `right` is the entire difference between this and the external CER tool, whose (-sin, cos, 0)
	// is correct only in UE5's left-handed frame - see COORDINATE FRAME at the top of this file. It is not a
	// cosmetic choice: getting it backwards mirrors the overlay horizontally.
	// Roll is ignored: s_player_control's roll is always 0 for the player.
	// ---------------------------------------------------------------------------------------------------------
	struct Camera
	{
		SimpleMath::Vector3 position, forward, right, up;
		float focalPixels = 0.f;
		SimpleMath::Vector2 screenCentre{};
	};

	static constexpr float kNearPlane = 0.01f;   // world units; 1 world unit = 10 feet

	static Camera makeCamera(const SimpleMath::Vector3& position, const SimpleMath::Vector2& viewAngle,
		const SimpleMath::Vector2& screenSize, float horizontalFovDegrees)
	{
		Camera camera;
		camera.position = position;
		// The basis maths moved to HCEGetCameraData so the D3D12 renderer and this fallback cannot drift apart.
		// It is the SAME code, verbatim - including the (sin, -cos, 0) right vector whose sign is the whole
		// difference from the external CER tool.
		HCEGetCameraData::simViewAngleToBlamBasis(viewAngle, camera.forward, camera.right, camera.up);

		const float fov = DirectX::XMConvertToRadians(std::clamp(horizontalFovDegrees, 10.f, 170.f));
		camera.focalPixels = screenSize.x / (2.f * std::tan(fov * 0.5f));
		camera.screenCentre = SimpleMath::Vector2(screenSize.x * 0.5f, screenSize.y * 0.5f);
		return camera;
	}

	// Same basis as makeCamera, but built from the UE camera's Pitch/Yaw/Roll (DEGREES) instead of the sim's
	// player-aim angles.
	//
	// UE -> Blam is a single Y negation (right-handed +Y LEFT vs left-handed +Y RIGHT); positions additionally
	// scale, which readUeCamera has already done. Applying that negation to UE's right = (-sinY, cosY, 0), with
	// yaw_ue == -yaw_blam, yields exactly (sin(yaw_blam), -cos(yaw_blam), 0) - the in-game-verified Blam right
	// vector this file already used. So this changes WHERE the angles come from, not the frame they live in.
	// Do not "simplify" by dropping the negation; that is the mirrored-overlay bug.
	//
	// Roll is applied about the forward axis. The reference tool reads roll and then silently ignores it, so
	// rolled cameras are wrong there; they are not wrong here.
	static Camera makeCameraFromUe(const SimpleMath::Vector3& positionBlam, float pitchDeg, float yawDeg,
		float rollDeg, const SimpleMath::Vector2& screenSize, float horizontalFovDegrees)
	{
		Camera camera;
		camera.position = positionBlam;
		// Again, the basis maths lives in HCEGetCameraData now - moved verbatim, including the single Y negation
		// that IS the UE->Blam handedness change, and the roll about the forward axis the reference tool drops.
		HCEGetCameraData::ueRotationToBlamBasis(pitchDeg, yawDeg, rollDeg, camera.forward, camera.right, camera.up);

		const float fov = DirectX::XMConvertToRadians(std::clamp(horizontalFovDegrees, 10.f, 170.f));
		camera.focalPixels = screenSize.x / (2.f * std::tan(fov * 0.5f));
		camera.screenCentre = SimpleMath::Vector2(screenSize.x * 0.5f, screenSize.y * 0.5f);
		return camera;
	}

	static SimpleMath::Vector3 toCameraSpace(const Camera& camera, const SimpleMath::Vector3& world)
	{
		const SimpleMath::Vector3 delta = world - camera.position;
		return SimpleMath::Vector3(delta.Dot(camera.right), delta.Dot(camera.up), delta.Dot(camera.forward));
	}

	static ImVec2 project(const Camera& camera, const SimpleMath::Vector3& cameraSpace)
	{
		const float scale = camera.focalPixels / cameraSpace.z;
		return ImVec2(camera.screenCentre.x + cameraSpace.x * scale,
			camera.screenCentre.y - cameraSpace.y * scale);
	}

	// Clips a camera-space segment against the near plane. The reference implementation simply DROPPED any edge
	// with an endpoint behind the camera, which makes whole boxes pop out of existence the moment you stand
	// inside one - precisely when you most want to see it.
	static bool clipToNearPlane(SimpleMath::Vector3& a, SimpleMath::Vector3& b)
	{
		if (a.z >= kNearPlane && b.z >= kNearPlane) return true;
		if (a.z < kNearPlane && b.z < kNearPlane) return false;
		const float denominator = b.z - a.z;
		if (std::abs(denominator) < 1e-9f) return false;
		const float t = (kNearPlane - a.z) / denominator;
		const SimpleMath::Vector3 crossing = a + (b - a) * t;
		if (a.z < kNearPlane) a = crossing; else b = crossing;
		return true;
	}

	// The WHOLE camera - position, rotation and FOV - from the single UE POV struct.
	//
	// This is the fix for "the overlay does not follow the camera". The sim's s_player_control angles are the
	// PLAYER'S AIM; the game renders from the UE camera. Any divergence between the two (vehicle/third person,
	// cinematics, the death cam, Freecam) shows up as the overlay sliding around. Taking every field from one
	// struct is the only way they cannot disagree - and it is what the external CER tool does.
	//
	// Position is converted UE centimetres -> Blam world units here (1 world unit = 304.8 cm) and the handedness
	// flip is folded in, so EVERYTHING downstream stays in the Blam frame exactly as before. Rotation is left in
	// degrees for makeCameraFromUe.
	//
	// The finite/range checks that make a wrong offset or a changed build FAIL SOFT live in
	// HCEGetCameraData::getUeCamera now; this is a thin adapter onto the shape the drawing code below wants.
	bool readUeCamera(SimpleMath::Vector3& outPositionBlam, float& outPitchDeg, float& outYawDeg,
		float& outRollDeg, float& outFovDeg)
	{
		if (!mCameraDataOptionalWeak.has_value()) return false;
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData) return false;

		HCEGetCameraData::UeCamera camera;
		if (!cameraData->getUeCamera(camera)) return false;

		outPositionBlam = camera.positionBlam;
		outPitchDeg = camera.pitchDegrees;
		outYawDeg = camera.yawDegrees;
		outRollDeg = camera.rollDegrees;
		outFovDeg = camera.horizontalFovDegrees;
		return true;
	}

	float lastGoodFov() const
	{
		if (mCameraDataOptionalWeak.has_value())
			if (auto cameraData = mCameraDataOptionalWeak.value().lock())
				return cameraData->getLastGoodHorizontalFov();
		return 78.f;
	}

	// Refuse to hook a build we do not recognise. HCEGetCameraData owns the byte verification and the reference
	// count; this just makes sure THIS overlay's request is added/removed exactly once. Throws on mismatch.
	void attachCameraHook()
	{
		if (!mCameraDataOptionalWeak.has_value())
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the trigger "
				"overlay cannot read the render camera");
		auto cameraData = mCameraDataOptionalWeak.value().lock();
		if (!cameraData)
			throw HCMRuntimeException("The Halo Campaign Evolved camera service is unavailable, so the trigger "
				"overlay cannot read the render camera");

		if (mCameraHookRequested) return;
		cameraData->setHookWanted(this, true);   // may throw; the request is only recorded once it did not
		mCameraHookRequested = true;
	}

	void detachCameraHook()
	{
		if (!mCameraHookRequested) return;
		mCameraHookRequested = false;
		if (!mCameraDataOptionalWeak.has_value()) return;
		if (auto cameraData = mCameraDataOptionalWeak.value().lock())
		{
			try { cameraData->setHookWanted(this, false); }
			catch (HCMRuntimeException) {}   // releasing a request cannot meaningfully fail
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// FALLBACK PATH: ImGui background draw list. Unchanged from the original implementation except for the
	// "is the 3D path alive?" gate at the top. See the file header for when each path runs.
	// ---------------------------------------------------------------------------------------------------------
	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;
		if (screenSize.x < 1.f || screenSize.y < 1.f) return;

		// The 3D renderer is drawing, so drawing here too would double every volume.
		const bool threeDLive = threeDPathIsLive();
		logDrawPathIfChanged(threeDLive);
		if (threeDLive) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			lockOrThrow(playerStateWeak, playerState);

			if (!mAnchorsGood) return;   // resolveAnchors already reported why, on the toggle

			std::scoped_lock volumesLock(mVolumesMutex);

			// Re-read the tag data at most 4x a second. Between refreshes the cached geometry is drawn every
			// frame, so nothing flickers while the chain is briefly down.
			const auto now = std::chrono::steady_clock::now();
			if (mLastRefresh.time_since_epoch().count() == 0 || (now - mLastRefresh) >= std::chrono::milliseconds(250))
			{
				mLastRefresh = now;
				refreshVolumes();
			}
			if (mVolumes.empty()) return;

			// Same two reports as the 3D path. The two paths are mutually exclusive (onRenderEvent returns
			// early when threeDPathIsLive()), so nothing is announced twice. ⚠ reportNewHits used to be called
			// ONLY from the 3D path; including it here fixes that asymmetry. playerState is already locked
			// above on this path, so it is reused rather than re-locked.
			if (settings->triggerOverlayMessageOnCheckHit->GetValue()
				|| settings->hceTriggerOverlayZoneSetReport->GetValue()
				|| settings->hceTriggerOverlayShowCheckpointGrant->GetValue())
			{
				if (auto messagesGUI = messagesGUIWeak.lock())
				{
					if (settings->triggerOverlayMessageOnCheckHit->GetValue())
						reportNewHits(messagesGUI);
					if (settings->hceTriggerOverlayZoneSetReport->GetValue()
						|| settings->hceTriggerOverlayShowCheckpointGrant->GetValue())
					{
						try
						{
							SimpleMath::Vector3 playerPoint, unusedVelocity;
							playerState->getPlayerPositionAndVelocity(playerPoint, unusedVelocity);
							reportZoneSetEntries(messagesGUI, playerPoint,
								settings->hceTriggerOverlayZoneSetReport->GetValue(),
								settings->hceTriggerOverlayShowCheckpointGrant->GetValue());
						}
						catch (HCMRuntimeException) {}   // dead or mid-load; skip the frame
					}
				}
			}

			// The UE camera is authoritative - it is what the frame is actually rendered from. The sim's
			// player-aim camera is only a fallback for the frames before DoUpdateCamera has run, or if the POV
			// read ever stops being plausible; it tracks the player rather than the render camera, so the
			// overlay will drift in a vehicle or a cutscene while it is in use.
			SimpleMath::Vector3 cameraPosition;
			float pitchDeg = 0.f, yawDeg = 0.f, rollDeg = 0.f, fovDeg = 0.f;

			Camera camera;
			if (readUeCamera(cameraPosition, pitchDeg, yawDeg, rollDeg, fovDeg))
			{
				camera = makeCameraFromUe(cameraPosition, pitchDeg, yawDeg, rollDeg, screenSize, fovDeg);
			}
			else
			{
				SimpleMath::Vector2 viewAngle;
				playerState->getCameraView(cameraPosition, viewAngle);   // ONE tls walk for both
				camera = makeCamera(cameraPosition, viewAngle, screenSize,
					lastGoodFov());
			}

			// EXACTLY the world origin means whichever source we used handed back a zeroed/unresolved position
			// rather than a real camera - no loaded level ever puts the camera there. Skip the frame instead of
			// drawing the whole overlay from (0,0,0), which is what the user actually saw.
			if (camera.position == SimpleMath::Vector3::Zero) return;

			const float renderDistance = settings->hceTriggerOverlayRenderDistance->GetValue();
			const bool wantLabels = settings->hceTriggerOverlayShowLabels->GetValue();
			const float labelScale = settings->triggerOverlayLabelScale->GetValue();

			const SettingsEnums::TriggerRenderStyle renderStyle = settings->triggerOverlayRenderStyle->GetValue();
			if (renderStyle == SettingsEnums::TriggerRenderStyle::None) return;
			const bool wantSolid = renderStyle == SettingsEnums::TriggerRenderStyle::Solid
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;
			const bool wantWire = renderStyle == SettingsEnums::TriggerRenderStyle::Wireframe
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;

			const bool highlightActive = settings->hceTriggerOverlayHighlightActive->GetValue();
			const SimpleMath::Vector4 activeColour = settings->hceTriggerOverlayActiveColor->GetValue();

			const SimpleMath::Vector4 normalColour = settings->triggerOverlayNormalColor->GetValue();
			// ⚠ triggerOverlaySectorColor is deliberately NOT read any more - sectors are regular triggers now.
			// The SETTING must stay: it is SHARED with the MCC trigger overlay (TriggerOverlay.cpp, Halo 3 ODST /
			// Reach / Halo 4), which still has a real sector distinction. Deleting it would break those games.
			const float wireAlpha = std::clamp(settings->triggerOverlayWireframeAlpha->GetValue(), 0.f, 1.f);
			const float solidAlpha = std::clamp(settings->triggerOverlayAlpha->GetValue(), 0.f, 1.f);

			// Pack once per frame per (class, style) rather than per volume.
			auto pack = [](SimpleMath::Vector4 c, float alpha)
				{
					return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alpha, 0.f, 1.f)));
				};
			const TypeFilter typeFilter = readTypeFilter(settings);
			const SimpleMath::Vector4 bspBase = settings->hceTriggerOverlayBspColor->GetValue();
			const SimpleMath::Vector4 labelColour = settings->hceTriggerOverlayLabelColor->GetValue();
			const ImU32 labelPacked = pack(labelColour, 1.f);

			const ImU32 boxWire = pack(normalColour, wireAlpha);
			const ImU32 activeWire = pack(activeColour, wireAlpha);
			const ImU32 bspWire = pack(bspBase, wireAlpha);
			const ImU32 bspFill = pack(bspBase, solidAlpha);
			const SimpleMath::Vector4 beginBase = settings->hceTriggerOverlayBeginZoneSetColor->GetValue();
			const ImU32 beginWire = pack(beginBase, wireAlpha);
			const ImU32 beginFill = pack(beginBase, solidAlpha);
			const SimpleMath::Vector4 cpBase = settings->hceTriggerOverlayCheckpointGrantColor->GetValue();
			const ImU32 cpWire = pack(cpBase, wireAlpha);
			const ImU32 cpFill = pack(cpBase, solidAlpha);
			const SimpleMath::Vector4 killBase = settings->hceTriggerOverlayKillColor->GetValue();
			const SimpleMath::Vector4 safeBase = settings->hceTriggerOverlaySafeZoneColor->GetValue();
			const ImU32 killWire = pack(killBase, wireAlpha);
			const ImU32 killFill = pack(killBase, solidAlpha);
			const ImU32 safeWire = pack(safeBase, wireAlpha);
			const ImU32 safeFill = pack(safeBase, solidAlpha);
			const ImU32 boxFill = pack(normalColour, solidAlpha);
			const ImU32 activeFill = pack(activeColour, solidAlpha);

			ImDrawList* drawList = ImGui::GetBackgroundDrawList();
			if (!drawList) return;

			for (const HceTriggerVolume& volume : mVolumes)
			{
				const float distance = (volume.center - camera.position).Length();
				if (distance - volume.radius > renderDistance) continue;

				// Whole-volume reject: entirely behind the camera.
				if (toCameraSpace(camera, volume.center).z + volume.radius < kNearPlane) continue;

				mCameraSpace.clear();
				mCameraSpace.reserve(volume.vertices.size());
				for (const auto& v : volume.vertices) mCameraSpace.push_back(toCameraSpace(camera, v));

				if (!shouldDrawVolume(volume, typeFilter)) continue;

				// Colour priority: ACTIVE beats everything (it is transient and the most urgent thing to see),
				// then BSP/zone-set (crossing one reloads the world), then kill, then plain. Sector-shaped
				// volumes take the plain colour - shape is not a category.
				const bool isLive = highlightActive && isVolumeActive(volume.index);
				const ImU32 colour = isLive ? activeWire
					: volume.isCommitZoneSet ? bspWire
					: volume.isBeginZoneSet ? beginWire
					: volume.isCheckpointGrant ? cpWire
					: volume.isKill ? killWire
					: volume.isSafeZone ? safeWire
					: boxWire;
				const ImU32 fill = isLive ? activeFill
					: volume.isCommitZoneSet ? bspFill
					: volume.isBeginZoneSet ? beginFill
					: volume.isCheckpointGrant ? cpFill
					: volume.isKill ? killFill
					: volume.isSafeZone ? safeFill
					: boxFill;

				// SOLID. There is no depth buffer on this D3D12 path, so faces are sorted back-to-front by
				// centroid depth (painter's algorithm) to look right against EACH OTHER. They are still not
				// occluded by world geometry - that needs a real depth-tested renderer.
				//
				// Faces with any vertex behind the near plane are skipped rather than clipped: polygon clipping
				// would have to re-wind the resulting n-gon, and the wireframe (which IS clipped properly)
				// already conveys the shape when you are stood inside a volume.
				if (wantSolid && !volume.faces.empty())
				{
					mFaceOrder.clear();
					for (size_t f = 0; f < volume.faces.size(); ++f)
					{
						const auto& face = volume.faces[f];
						if (face.size() < 3) continue;

						float depthSum = 0.f;
						bool visible = true;
						for (uint16_t vi : face)
						{
							if (vi >= mCameraSpace.size() || mCameraSpace[vi].z < kNearPlane) { visible = false; break; }
							depthSum += mCameraSpace[vi].z;
						}
						if (!visible) continue;
						mFaceOrder.emplace_back(depthSum / (float)face.size(), f);
					}

					std::sort(mFaceOrder.begin(), mFaceOrder.end(),
						[](const auto& a, const auto& b) { return a.first > b.first; });   // far -> near

					for (const auto& [depth, faceIndex] : mFaceOrder)
					{
						const auto& face = volume.faces[faceIndex];
						mPoly.clear();
						mPoly.reserve(face.size());
						for (uint16_t vi : face) mPoly.push_back(project(camera, mCameraSpace[vi]));
						drawList->AddConvexPolyFilled(mPoly.data(), (int)mPoly.size(), fill);
					}
				}

				if (wantWire)
				{
					for (const auto& edge : volume.edges)
					{
						if (edge.first >= mCameraSpace.size() || edge.second >= mCameraSpace.size()) continue;
						SimpleMath::Vector3 a = mCameraSpace[edge.first];
						SimpleMath::Vector3 b = mCameraSpace[edge.second];
						if (!clipToNearPlane(a, b)) continue;
						drawList->AddLine(project(camera, a), project(camera, b), colour, 1.5f);
					}
				}

				if (wantLabels)
				{
					const SimpleMath::Vector3 centreCamera = toCameraSpace(camera, volume.center);
					if (centreCamera.z >= kNearPlane)
					{
						const ImVec2 screen = project(camera, centreCamera);
						// Simple distance falloff so a nearby label does not fill the screen and a far one is
						// still legible. Deliberately not RenderTextHelper::scaleTextDistance, which is tuned
						// for MCC's world scale - one HCE world unit is 10 feet.
						const float sizeScale = std::clamp(3.f / std::max(distance, 0.5f), 0.35f, 1.f);
						// Label colour is its OWN setting, not the volume's - a wireframe colour that reads
						// fine as a thin line is far too dark as text.
						RenderTextHelper::drawCenteredOutlinedText(volume.name,
							SimpleMath::Vector2(screen.x, screen.y), labelPacked, labelScale * sizeScale);
					}
				}
			}

			// The player's trigger test point, last so nothing overdraws it. The MCC overlay renders a real 3D
			// sphere through IRenderer3D; with only a 2D draw list the honest equivalent is a screen-space disc
			// whose radius is scaled by depth, which is what a sphere projects to anyway.
			if (settings->hceTriggerOverlayShowVertex->GetValue())
			{
				// Its own try: it throws while the player is dead or mid-load, and that must not abort the
				// volumes that were already drawn this frame.
				try
				{
					SimpleMath::Vector3 vertexPosition, unusedVelocity;
					playerState->getPlayerPositionAndVelocity(vertexPosition, unusedVelocity);

					const SimpleMath::Vector3 vertexCamera = toCameraSpace(camera, vertexPosition);
					if (vertexCamera.z >= kNearPlane)
					{
						const SimpleMath::Vector4 vertexColour = settings->triggerOverlayPositionColor->GetValue();
						const float vertexScale = settings->triggerOverlayPositionScale->GetValue();

						// World-sized: a sphere of radius r at depth z subtends r*focal/z pixels.
						const float radiusPixels = std::clamp(
							(vertexScale * 0.1f) * camera.focalPixels / vertexCamera.z, 2.f, 400.f);
						const ImVec2 screen = project(camera, vertexCamera);

						drawList->AddCircleFilled(screen, radiusPixels,
							pack(vertexColour, 1.f), 24);
						// Denser core, as the MCC version does with its second 1/10th-scale sphere.
						drawList->AddCircleFilled(screen, std::max(radiusPixels / 3.f, 1.5f),
							pack(vertexColour, 1.f) | 0xFF000000, 16);
					}
				}
				catch (HCMRuntimeException) {}
			}
		}
		catch (HCMRuntimeException)
		{
			// Expected constantly: no level loaded, player dead, mid-load. The overlay simply skips the frame
			// rather than spamming the message log every 16 ms.
		}
	}


	// ---------------------------------------------------------------------------------------------------------
	// PRIMARY PATH: HCM's IRenderer3D (Renderer3DImplD3D12 under HaloCER).
	//
	// Every setting the ImGui path honours is honoured here, and the colours are computed the same way. What
	// differs is what the pixels are made of:
	//   * solid faces are REAL depth-tested triangles (ear-clipped at refresh time, so concave sector caps fill
	//     correctly) instead of ImGui convex polygons sorted back-to-front by centroid,
	//   * edges are real 3D lines, clipped by the GPU rather than by hand against a near plane,
	//   * the player's trigger test point is a real sphere, exactly like MCC's TriggerOverlay.
	// Labels still go through RenderTextHelper (ImGui text) - the MCC overlay does the same thing.
	//
	// Runs on the game's render thread, inside D3D12Hook's command list. It must never throw: everything is
	// caught here, and Render3DEventProvider catches anything that somehow escapes.
	// ---------------------------------------------------------------------------------------------------------
	void onRender3DEvent(GameState game, IRenderer3D* renderer)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;
		if (!renderer) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER) return;

		// Never blocks: if a teardown is draining the render path, skip this frame rather than stall the game's
		// render thread. try_to_lock also means a waiting drain can never park us behind it.
		if (mRender3DDraining.load(std::memory_order_acquire)) return;
		std::shared_lock<std::shared_timed_mutex> renderLock(mRender3DGuard, std::try_to_lock);
		if (!renderLock.owns_lock()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);

			if (!mAnchorsGood) return;

			std::scoped_lock volumesLock(mVolumesMutex);

			// Same 4-per-second tag re-read as the ImGui path; the cached geometry is drawn every frame.
			const auto now = std::chrono::steady_clock::now();
			if (mLastRefresh.time_since_epoch().count() == 0 || (now - mLastRefresh) >= std::chrono::milliseconds(250))
			{
				mLastRefresh = now;
				refreshVolumes();
			}
			if (mVolumes.empty()) return;

			// Announce anything hit since the last frame, and anything entered. Done here, under the same lock
			// that owns mVolumes, so the name and the index can never disagree.
			if (settings->triggerOverlayMessageOnCheckHit->GetValue()
				|| settings->hceTriggerOverlayZoneSetReport->GetValue()
				|| settings->hceTriggerOverlayShowCheckpointGrant->GetValue())
			{
				if (auto messagesGUI = messagesGUIWeak.lock())
				{
					if (settings->triggerOverlayMessageOnCheckHit->GetValue())
						reportNewHits(messagesGUI);
					if (settings->hceTriggerOverlayZoneSetReport->GetValue()
						|| settings->hceTriggerOverlayShowCheckpointGrant->GetValue())
					{
						try
						{
							lockOrThrow(playerStateWeak, playerState);
							SimpleMath::Vector3 playerPoint, unusedVelocity;
							playerState->getPlayerPositionAndVelocity(playerPoint, unusedVelocity);
							reportZoneSetEntries(messagesGUI, playerPoint,
								settings->hceTriggerOverlayZoneSetReport->GetValue(),
								settings->hceTriggerOverlayShowCheckpointGrant->GetValue());
						}
						catch (HCMRuntimeException) {}   // dead or mid-load; skip the frame, never abort the draw
					}
				}
			}

			const SettingsEnums::TriggerRenderStyle renderStyle = settings->triggerOverlayRenderStyle->GetValue();
			if (renderStyle == SettingsEnums::TriggerRenderStyle::None)
			{
				// Nothing to draw, but the 3D path IS alive - claim the frame so the ImGui fallback stays quiet.
				mLast3DDrawTick.store(GetTickCount64(), std::memory_order_release);
				return;
			}
			const bool wantSolid = renderStyle == SettingsEnums::TriggerRenderStyle::Solid
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;
			const bool wantWire = renderStyle == SettingsEnums::TriggerRenderStyle::Wireframe
				|| renderStyle == SettingsEnums::TriggerRenderStyle::SolidAndWireframe;

			const float renderDistance = settings->hceTriggerOverlayRenderDistance->GetValue();
			const bool wantLabels = settings->hceTriggerOverlayShowLabels->GetValue();
			const float labelScale = settings->triggerOverlayLabelScale->GetValue();

			const bool highlightActive = settings->hceTriggerOverlayHighlightActive->GetValue();
			const SimpleMath::Vector4 activeColour = settings->hceTriggerOverlayActiveColor->GetValue();

			// Hit flash. Shares the MCC trigger overlay's settings, which already model exactly this
			// (triggerOverlayCheckHitToggle / Falloff / Color) - the falloff is in TICKS there, so convert.
			const bool flashOnHit = settings->triggerOverlayCheckHitToggle->GetValue();
			const uint32_t hitFlashMs = (uint32_t)std::clamp(
				settings->triggerOverlayCheckHitFalloff->GetValue(), 1, 600) * 1000u / 60u;
			const SimpleMath::Vector4 hitBase = settings->triggerOverlayCheckHitColor->GetValue();
			const bool messageOnHit = settings->triggerOverlayMessageOnCheckHit->GetValue();

			const SimpleMath::Vector4 normalColour = settings->triggerOverlayNormalColor->GetValue();
			// ⚠ triggerOverlaySectorColor is deliberately NOT read any more - sectors are regular triggers now.
			// The SETTING must stay: it is SHARED with the MCC trigger overlay (TriggerOverlay.cpp, Halo 3 ODST /
			// Reach / Halo 4), which still has a real sector distinction. Deleting it would break those games.
			const float wireAlpha = std::clamp(settings->triggerOverlayWireframeAlpha->GetValue(), 0.f, 1.f);
			const float solidAlpha = std::clamp(settings->triggerOverlayAlpha->GetValue(), 0.f, 1.f);

			auto withAlpha = [](SimpleMath::Vector4 c, float alpha)
				{
					c.w = std::clamp(c.w * alpha, 0.f, 1.f);
					return c;
				};
			auto packU32 = [](const SimpleMath::Vector4& c)
				{
					return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w));
				};

			const SimpleMath::Vector4 hitWire = withAlpha(hitBase, wireAlpha);
			const SimpleMath::Vector4 hitFill = withAlpha(hitBase, solidAlpha);
			const SimpleMath::Vector4 boxWire = withAlpha(normalColour, wireAlpha);
			const SimpleMath::Vector4 activeWire = withAlpha(activeColour, wireAlpha);
			const SimpleMath::Vector4 boxFill = withAlpha(normalColour, solidAlpha);
			const SimpleMath::Vector4 activeFill = withAlpha(activeColour, solidAlpha);

			// Kept in lockstep with the ImGui path above - same settings, same priority, same filter.
			const TypeFilter typeFilter = readTypeFilter(settings);
			const SimpleMath::Vector4 bspBase = settings->hceTriggerOverlayBspColor->GetValue();
			const SimpleMath::Vector4 bspWire = withAlpha(bspBase, wireAlpha);
			const SimpleMath::Vector4 bspFill = withAlpha(bspBase, solidAlpha);
			const SimpleMath::Vector4 beginBase = settings->hceTriggerOverlayBeginZoneSetColor->GetValue();
			const SimpleMath::Vector4 beginWire = withAlpha(beginBase, wireAlpha);
			const SimpleMath::Vector4 beginFill = withAlpha(beginBase, solidAlpha);
			const SimpleMath::Vector4 cpBase = settings->hceTriggerOverlayCheckpointGrantColor->GetValue();
			const SimpleMath::Vector4 cpWire = withAlpha(cpBase, wireAlpha);
			const SimpleMath::Vector4 cpFill = withAlpha(cpBase, solidAlpha);
			const SimpleMath::Vector4 killBase = settings->hceTriggerOverlayKillColor->GetValue();
			const SimpleMath::Vector4 safeBase = settings->hceTriggerOverlaySafeZoneColor->GetValue();
			const SimpleMath::Vector4 killWire = withAlpha(killBase, wireAlpha);
			const SimpleMath::Vector4 killFill = withAlpha(killBase, solidAlpha);
			const SimpleMath::Vector4 safeWire = withAlpha(safeBase, wireAlpha);
			const SimpleMath::Vector4 safeFill = withAlpha(safeBase, solidAlpha);
			const ImU32 labelPacked = packU32(settings->hceTriggerOverlayLabelColor->GetValue());

			const SimpleMath::Vector3 cameraPosition = renderer->getCameraPosition();
			const DirectX::BoundingFrustum& cameraFrustum = renderer->getCameraFrustum();

			for (const HceTriggerVolume& volume : mVolumes)
			{
				if (volume.renderVertices.empty()) continue;

				const float distance = (volume.center - cameraPosition).Length();
				if (distance - volume.radius > renderDistance) continue;

				// Whole-volume reject against the real world-space frustum, tested as the volume's BOUNDING
				// SPHERE - not its centre. Testing the centre would make a big volume vanish the moment you
				// stepped inside it, which is precisely when you most want to see it (the ImGui path guards the
				// same case with `centre.z + radius < near`). This is the same test MCC's TriggerOverlay does
				// with its bounding box.
				if (!cameraFrustum.Intersects(DirectX::BoundingSphere(volume.center, volume.radius))) continue;

				if (!shouldDrawVolume(volume, typeFilter)) continue;

				// HIT beats ACTIVE beats everything else. A hit is a moment - the thing you are actually
				// watching for - so it must not be outranked by the multi-second "a script is polling this"
				// state that surrounds it.
				const bool isHit = flashOnHit && isVolumeHit(volume.index, hitFlashMs);
				const bool isLive = !isHit && highlightActive && isVolumeActive(volume.index);
				if (isHit)
				{
					const HceTriggerVolumeModel hitModel(volume);
					if (wantSolid && !volume.triangleIndices.empty())
						renderer->drawTriangleCollection(&hitModel, hitFill, CullingOption::CullNone, std::nullopt);
					if (wantWire && !volume.edgeIndices.empty())
						renderer->drawEdgeCollection(&hitModel, hitWire);
					if (wantLabels && !renderer->pointBehindCamera(volume.center))
					{
						const SimpleMath::Vector3 screen = renderer->worldPointToScreenPosition(volume.center, false);
						const float sizeScale = std::clamp(3.f / std::max(distance, 0.5f), 0.35f, 1.f);
						RenderTextHelper::drawCenteredOutlinedText(volume.name,
							SimpleMath::Vector2(screen.x, screen.y), labelPacked, labelScale * sizeScale);
					}
					continue;
				}

				const SimpleMath::Vector4& wireColour = isLive ? activeWire
					: volume.isCommitZoneSet ? bspWire
					: volume.isBeginZoneSet ? beginWire
					: volume.isCheckpointGrant ? cpWire
					: volume.isKill ? killWire
					: volume.isSafeZone ? safeWire
					: boxWire;                                    // sectors included - shape is not a category
				const SimpleMath::Vector4& fillColour = isLive ? activeFill
					: volume.isCommitZoneSet ? bspFill
					: volume.isBeginZoneSet ? beginFill
					: volume.isCheckpointGrant ? cpFill
					: volume.isKill ? killFill
					: volume.isSafeZone ? safeFill
					: boxFill;

				const HceTriggerVolumeModel model(volume);

				// CullNone, exactly as MCC's TriggerOverlay does - a trigger volume must look the same whether
				// you are inside it or outside it, and the tag data has no reliable winding convention.
				if (wantSolid && !volume.triangleIndices.empty())
					renderer->drawTriangleCollection(&model, fillColour, CullingOption::CullNone, std::nullopt);

				if (wantWire && !volume.edgeIndices.empty())
					renderer->drawEdgeCollection(&model, wireColour);

				if (wantLabels && !renderer->pointBehindCamera(volume.center))
				{
					const SimpleMath::Vector3 screen = renderer->worldPointToScreenPosition(volume.center, false);
					// Deliberately NOT RenderTextHelper::scaleTextDistance, which is tuned for MCC's world scale
					// - one HaloCER world unit is 10 feet. Same falloff the ImGui path uses.
					const float sizeScale = std::clamp(3.f / std::max(distance, 0.5f), 0.35f, 1.f);
					// Own colour, not the volume's - see the ImGui path.
					RenderTextHelper::drawCenteredOutlinedText(volume.name,
						SimpleMath::Vector2(screen.x, screen.y), labelPacked, labelScale * sizeScale);
				}
			}

			// The player's trigger test point, last so nothing overdraws it. A real sphere here, plus the
			// second 1/10th-scale denser sphere MCC's TriggerOverlay draws.
			if (settings->hceTriggerOverlayShowVertex->GetValue())
			{
				try
				{
					lockOrThrow(playerStateWeak, playerState);
					SimpleMath::Vector3 vertexPosition, unusedVelocity;
					playerState->getPlayerPositionAndVelocity(vertexPosition, unusedVelocity);

					SimpleMath::Vector4 vertexColour = settings->triggerOverlayPositionColor->GetValue();
					const float vertexScale = settings->triggerOverlayPositionScale->GetValue();
					const bool isWireframe = settings->triggerOverlayPositionWireframe->GetValue();

					renderer->renderSphere(vertexPosition, vertexColour, vertexScale, isWireframe);
					vertexColour.w += (1.f - vertexColour.w) / 2.f;
					renderer->renderSphere(vertexPosition, vertexColour, vertexScale / 10.f, isWireframe);
				}
				catch (HCMRuntimeException) {}   // dead, or mid-load. Must not abort what was already drawn.
			}

			// Claim the frame LAST, so the ImGui fallback only stands down once this path really did draw.
			mLast3DDrawTick.store(GetTickCount64(), std::memory_order_release);
		}
		catch (HCMRuntimeException)
		{
			// Expected constantly (no level loaded, mid-load). Skipping the frame here also lets the ImGui
			// fallback take over after kFallbackAfterMs, which is exactly what should happen.
		}
		catch (...)
		{
			LOG_ONCE(PLOG_ERROR << "HCETriggerOverlay's 3D render path threw an unknown exception; suppressing further reports");
		}
	}

	// Subscribes/unsubscribes the 3D render event. Subscribing is what makes Render3DEventProvider do its
	// per-frame camera work at all, so it is kept off unless the overlay is actually on.
	// How long a drain waits for an in-flight render callback. A frame is milliseconds, so this is generous for a
	// slow one and still finite - which is the entire point, since the wait it replaces had no bound at all.
	static constexpr std::chrono::milliseconds kRender3DDrainTimeout{ 1000 };

	// Stops new entries to onRender3DEvent, waits for any in-flight one, and drops the subscription - all before
	// releasing the guard, so a callback cannot slip in between the wait and the reset.
	//
	// mustSucceed distinguishes the two callers, and they need genuinely different answers:
	//
	//  - false (a toggle or a game state change, i.e. every revert): give up after the timeout and leave the
	//    subscription IN PLACE. Dropping it while a callback is still inside would free state that callback is
	//    using. Staying subscribed is harmless here because THIS OBJECT STILL EXISTS - mReady and mIsActive make
	//    the callback a no-op, and the destructor will drain it properly later. This is the path that used to
	//    block the state-hook thread forever.
	//
	//  - true (the destructor): there is no safe way to give up. Every member is about to be freed while the
	//    provider still holds a lambda capturing `this`, so returning early would turn a hang into a
	//    use-after-free. Keep waiting, but log each attempt so a stall is visible rather than silent.
	bool dropRender3DSubscription(bool mustSucceed)
	{
		mRender3DDraining.store(true, std::memory_order_release);
		for (;;)
		{
			{
				std::unique_lock<std::shared_timed_mutex> drain(mRender3DGuard, kRender3DDrainTimeout);
				if (drain.owns_lock())
				{
					mRender3DEventCallback.reset();
					break;
				}
			}

			if (!mustSucceed)
			{
				mRender3DDraining.store(false, std::memory_order_release);
				PLOG_ERROR << "HCETriggerOverlay: timed out draining the 3D render path; keeping the subscription "
					"rather than freeing state it may still be using";
				return false;
			}

			PLOG_ERROR << "HCETriggerOverlay: still waiting for the 3D render path to finish before teardown; "
				"cannot give up here without freeing state a live callback is using";
		}
		mRender3DDraining.store(false, std::memory_order_release);
		return true;
	}

	// forTeardown is set only by the destructor - see dropRender3DSubscription for why it cannot give up there.
	void set3DRenderingEnabled(bool enabled, bool forTeardown = false)
	{
		// Covers BOTH branches: every read and write of mRender3DEventCallback happens under it.
		std::scoped_lock subscriptionLock(mRender3DSubscriptionMutex);

		if (!enabled)
		{
			if (mRender3DEventCallback)
				dropRender3DSubscription(forTeardown);
			mLast3DDrawTick.store(0, std::memory_order_release);
			// Forget which path was last reported, so switching the overlay back on says so again rather than
			// staying silent because it happens to match the state from before it was switched off.
			mLoggedDrawPath.store(-1, std::memory_order_relaxed);
			mLastDrawPathLogTick.store(0, std::memory_order_relaxed);
			return;
		}

		if (mRender3DEventCallback) return;   // already subscribed
		if (!mRender3DProviderOptionalWeak.has_value()) return;   // no 3D path in this build/process

		auto provider = mRender3DProviderOptionalWeak.value().lock();
		if (!provider) return;

		// If the D3D12 renderer has already latched an initialisation failure there is no point subscribing -
		// it would never draw, and the ImGui fallback is what the user should get.
		if (provider->d3d12RendererHasFailed()) return;

		mRender3DEventCallback = provider->getRender3DEvent()->subscribe(
			[this](GameState g, IRenderer3D* r) { onRender3DEvent(g, r); });
	}

	void onToggleEvent(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) { mIsActive = false; return; }

			if (!newValue)
			{
				mIsActive = false;
				set3DRenderingEnabled(false);
				detachCameraHook();
				setActivityTracking(false);
				messagesGUI->addMessage("Disabling Trigger Overlay.");
				return;
			}

			lockOrThrow(playerStateWeak, playerState);

			// Before the anchor scan: without the render FOV the overlay draws at the wrong scale, so a failure
			// here is worth surfacing rather than silently falling back.
			attachCameraHook();
			setActivityTracking(true);
			// Subscribing is also what makes Render3DEventProvider start doing per-frame camera work. If it
			// cannot be subscribed (no provider, or the D3D12 renderer already failed) the ImGui path runs.
			set3DRenderingEnabled(true);

			// Resolve on the TOGGLE, never on the render thread: the scan walks the whole executable image and
			// would be a visible hitch every frame. It is a one-off per session; a failure is re-tried on the
			// next toggle rather than latched forever.
			if (!mAnchorsTried || !mAnchorsGood)
				resolveAnchors(playerState->getSimModuleBase());

			if (!mAnchorsGood)
			{
				mIsActive = false;
				throw HCMRuntimeException(std::format(
					"Trigger Overlay can't run on this game build: could not locate the {} by byte signature. "
					"This usually means Halo Campaign Evolved updated. (Halo Campaign Evolved reports no version "
					"number at all, so HCM cannot detect an update any other way, and would rather refuse than "
					"draw triggers read from the wrong address.)", mAnchorFailure));
			}

			// Force a rebuild so the count we report is this level's.
			size_t volumeCount = 0;
			{
				std::scoped_lock volumesLock(mVolumesMutex);
				mCachedCount = -1;
				mCachedEncodedAddress = 0;
				mVolumes.clear();
				mZoneSetInsideSeeded = false;   // re-seed without reporting; see reportZoneSetEntries
				mLastRefresh = {};
				refreshVolumes();
				volumeCount = mVolumes.size();
			}   // released before messaging - messagesGUI takes its own locks and the render thread wants this one

			mIsActive = true;
			if (volumeCount == 0)
				messagesGUI->addMessage("Trigger Overlay on, but this level has no trigger volumes loaded yet.");
			else
				messagesGUI->addMessage(std::format("Trigger Overlay on: {} trigger volumes.", volumeCount));
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onGameStateChanged(const MCCState& newState)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);

			// A level transition invalidates every cached tag pointer.
			{
				std::scoped_lock volumesLock(mVolumesMutex);
				mCachedCount = -1;
				mCachedEncodedAddress = 0;
				mVolumes.clear();
				mZoneSetInsideSeeded = false;   // re-seed without reporting; see reportZoneSetEntries
				mLastRefresh = {};
			}

			const bool want = settings->triggerOverlayToggle->GetValue()
				&& newState.currentGameState == mGame
				&& newState.currentPlayState == PlayState::Ingame;

			// The toggle can already be on when a level loads (a persisted setting, or a preset), in which case
			// nothing has ever called onToggleEvent while the sim dll was loaded. Resolve here too - this runs on
			// the state-hook thread, not the render thread, so the image scan is not a frame hitch. Silent on
			// failure: the toggle path is where the user gets told why.
			if (want && !mAnchorsGood)
			{
				if (auto playerState = playerStateWeak.lock())
				{
					try { resolveAnchors(playerState->getSimModuleBase()); }
					catch (HCMRuntimeException) {}
				}
			}

			// Same reasoning as the anchors: the toggle may already have been on when this level loaded, so the
			// camera hook has to be (re)established here too. Silent on failure - currentFov falls back to the
			// last good value, and the toggle path is where the user gets told.
			try
			{
				if (want) attachCameraHook();
				else detachCameraHook();
			}
			catch (HCMRuntimeException) {}

			// Self-heal: attachCameraHook() is a no-op once this overlay has already registered its request, so
			// if anything detached the midhook behind our back (a module reload, a teardown that ran early) it
			// would stay detached for the rest of the session and every consumer would silently drop to the sim's
			// player-aim camera. This runs on the state-hook thread, never the render thread.
			if (want && mCameraDataOptionalWeak.has_value())
				if (auto cameraData = mCameraDataOptionalWeak.value().lock())
					cameraData->ensureHookLive();

			setActivityTracking(want);
			// Same reasoning again: the 3D subscription has to follow the level, not just the toggle.
			set3DRenderingEnabled(want);

			mIsActive = want && mAnchorsGood;
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// THE NAME PICKER.
	//
	// This lives HERE, in the overlay, rather than in a cheat of its own, for one reason: the list of volume
	// names only exists here, behind mVolumesMutex, and it is rebuilt on every scenario change. A separate cheat
	// would have to re-walk the scenario tag to populate the dialog and would be reading a second, potentially
	// disagreeing copy of the level's volumes.
	//
	// The dialog is a BLOCKING call rendered on the ImGui thread, so mVolumesMutex is copied out of and released
	// BEFORE it opens. Holding it across the dialog would stall the render path for as long as the user left the
	// window open.
	// ---------------------------------------------------------------------------------------------------------
	void onEditNameFilter()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			auto modalDialogs = modalDialogsWeak.lock();
			if (!modalDialogs) throw HCMRuntimeException("The dialog service is unavailable.");

			std::vector<HCETriggerNameFilterDialog::Entry> entries;
			{
				std::scoped_lock volumesLock(mVolumesMutex);
				entries.reserve(mVolumes.size());
				for (const auto& v : mVolumes)
				{
					HCETriggerNameFilterDialog::Entry e;
					e.name = v.name;
					e.speedrun = v.isSpeedrun;
					// SAME priority order the colours and shouldDrawVolume use, so a volume is listed under the
					// category it actually behaves as.
					e.category = v.isCommitZoneSet ? HCETriggerNameFilterDialog::Category::ZoneSet
						: v.isBeginZoneSet ? HCETriggerNameFilterDialog::Category::BeginZoneSet
						: v.isCheckpointGrant ? HCETriggerNameFilterDialog::Category::CheckpointGrant
						: v.isKill ? HCETriggerNameFilterDialog::Category::Kill
						: v.isSafeZone ? HCETriggerNameFilterDialog::Category::SafeZone
						: HCETriggerNameFilterDialog::Category::Regular;   // sectors included
					entries.push_back(std::move(e));
				}
			}

			if (entries.empty())
			{
				if (auto messagesGUI = messagesGUIWeak.lock())
					messagesGUI->addMessage("No trigger volumes loaded yet - turn the Trigger Overlay on, in a level.");
			}

			const std::string current = settings->triggerOverlayFilterString->GetValue();
			const std::string result = modalDialogs->showReturningDialog(   // blocking
				ModalDialogFactory::makeHCETriggerNameFilterDialog("Filter Trigger Volumes by Name", current, std::move(entries)));

			if (result == current) return;

			settings->triggerOverlayFilterString->GetValueDisplay() = result;
			settings->triggerOverlayFilterString->UpdateValueWithInput();

			// Choosing names and then not seeing any change would read as a broken picker, so switch the filter
			// on for them. Never switch it OFF - clearing the list back to "everything" is a perfectly reasonable
			// thing to do while leaving the filter armed.
			if (!result.empty() && !settings->triggerOverlayFilterToggle->GetValue())
			{
				settings->triggerOverlayFilterToggle->GetValueDisplay() = true;
				settings->triggerOverlayFilterToggle->UpdateValueWithInput();
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp - the callbacks must be destroyed before anything they touch.
	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;
	ScopedCallback<ActionEvent> mEditNameFilterCallback;

public:
	HCETriggerOverlayImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); }),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->triggerOverlayToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); }),
		mEditNameFilterCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceTriggerOverlayEditNameFilterEvent, [this]() { onEditNameFilter(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCETriggerOverlay only supports Halo Campaign Evolved");

		modalDialogsWeak = dicon.Resolve<ModalDialogRenderer>();

		// Pure PointerDataStore lookups. NOTHING here touches game memory - the sim dll is not guaranteed to be
		// loaded when cheats are constructed (see the SAFETY note in HCECheckpointDetours.cpp).
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
#define hceOffset(member, name) member = *ptr->getData<std::shared_ptr<int64_t>>(nameof(name), mGame)
		hceOffset(mScenarioTriggerVolumeBlock, hceScenarioTriggerVolumeBlock);
		hceOffset(mScenarioZoneSetSwitchBlock, hceScenarioZoneSetSwitchTriggerVolumeBlock);
		hceOffset(mZoneSetSwitchStride, hceZoneSetSwitchStride);
		hceOffset(mZoneSetSwitchTriggerVolumeIndexOffset, hceZoneSetSwitchTriggerVolumeIndexOffset);
		hceOffset(mZoneSetSwitchBeginZoneSetOffset, hceZoneSetSwitchBeginZoneSetOffset);
		hceOffset(mZoneSetSwitchCommitZoneSetOffset, hceZoneSetSwitchCommitZoneSetOffset);
		hceOffset(mZoneSetSwitchFlagsOffset, hceZoneSetSwitchFlagsOffset);
		hceOffset(mScenarioZoneSetBlock, hceScenarioZoneSetBlock);
		hceOffset(mZoneSetStride, hceZoneSetStride);
		hceOffset(mZoneSetNameStringIdOffset, hceZoneSetNameStringIdOffset);
		hceOffset(mZoneSetNameStringOffset, hceZoneSetNameStringOffset);
		hceOffset(mZoneSetFlagsOffset, hceZoneSetFlagsOffset);
		hceOffset(mZoneSetBspZoneFlagsOffset, hceZoneSetBspZoneFlagsOffset);
		hceOffset(mZoneSetRuntimeBspZoneFlagsOffset, hceZoneSetRuntimeBspZoneFlagsOffset);
		hceOffset(mScenarioKillTriggerBlock, hceScenarioKillTriggerBlock);
		hceOffset(mScenarioSafeZoneTriggerBlock, hceScenarioSafeZoneTriggerBlock);
		hceOffset(mKillTriggerStride, hceKillTriggerStride);
		hceOffset(mKillTriggerVolumeIndexOffset, hceKillTriggerVolumeIndexOffset);
		hceOffset(mTriggerVolumeStride, hceTriggerVolumeStride);
		hceOffset(mTriggerVolumeNameOffset, hceTriggerVolumeNameOffset);
		hceOffset(mTriggerVolumeTypeOffset, hceTriggerVolumeTypeOffset);
		hceOffset(mTriggerVolumeForwardOffset, hceTriggerVolumeForwardOffset);
		hceOffset(mTriggerVolumeUpOffset, hceTriggerVolumeUpOffset);
		hceOffset(mTriggerVolumePositionOffset, hceTriggerVolumePositionOffset);
		hceOffset(mTriggerVolumeExtentsOffset, hceTriggerVolumeExtentsOffset);
		hceOffset(mTriggerVolumeSectorPointsBlock, hceTriggerVolumeSectorPointsBlock);
		hceOffset(mTriggerVolumeSectorPointStride, hceTriggerVolumeSectorPointStride);
		hceOffset(mTriggerVolumeBoundsOffset, hceTriggerVolumeBoundsOffset);
#undef hceOffset

		// Optional dependency: the overlay is still useful without active/inactive colouring, so a tracker that
		// cannot initialise (unrecognised build -> patches fail closed) must not take the whole overlay down.
		try
		{
			mTriggerActivityOptionalWeak = resolveDependentCheat(HCETriggerActivity);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCETriggerOverlay could not resolve HCETriggerActivity, continuing without "
				"active/inactive trigger colouring";
		}

		// The UE render camera. Optional: without it the overlay falls back to the sim's player-aim camera and
		// the default FOV, exactly as it does on the frames before DoUpdateCamera has run. The midhook is not
		// installed by constructing this - nothing is patched until the overlay is switched on.
		try
		{
			mCameraDataOptionalWeak = resolveDependentCheat(HCEGetCameraData);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCETriggerOverlay could not resolve HCEGetCameraData; the overlay will use the sim's "
				"player camera and a default field of view";
		}

		// The 3D renderer. Optional in the strongest sense: if this fails to construct - or succeeds and then
		// fails to initialise on the render thread - the ImGui drawing path below carries the whole feature.
		try
		{
			mRender3DProviderOptionalWeak = resolveDependentCheat(Render3DEventProvider);
		}
		catch (HCMInitException)
		{
			PLOG_ERROR << "HCETriggerOverlay could not resolve Render3DEventProvider; falling back to ImGui "
				"drawing for the trigger overlay";
		}

		mReady.store(true, std::memory_order_release);
	}

	~HCETriggerOverlayImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
		// Drop the 3D subscription FIRST, and wait for any in-flight render callback: it touches every member
		// below this point. forTeardown = true, because giving up here would free that state underneath a live
		// callback rather than merely leaving it subscribed.
		set3DRenderingEnabled(false, true);
		detachCameraHook();
		setActivityTracking(false);
		mRenderEventCallback.removeCallback();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCETriggerOverlay::HCETriggerOverlay(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCETriggerOverlayImpl>(game, dicon))
{
}

HCETriggerOverlay::~HCETriggerOverlay()
{
	PLOG_VERBOSE << "~" << getName();
}
