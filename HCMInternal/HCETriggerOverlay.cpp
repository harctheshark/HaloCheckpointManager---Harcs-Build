#include "pch.h"
#include "HCETriggerOverlay.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCETriggerActivity.h"
#include "PointerDataStore.h"
#include "RenderTextHelper.h"
#include "GlobalKill.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cmath>

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
// THE FIELD OF VIEW
// -----------------
// Camera position (camera entry + 0x20) and orientation (s_player_control + 0x14/+0x18) come from the sim. The
// FOV does NOT exist in the sim in a usable form, and is read live from the UE5 renderer instead: a midhook on
// APlayerCameraManager::DoUpdateCamera (exe RVA 0x64B3B60) captures `this`, and POV.FOV is a float at +0x14E0.
// See InternalPointerData.xml, region hceCameraManager, for the derivation and the offset cross-check.
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
	// The UE5 render camera's field of view.
	//
	// APlayerCameraManager::DoUpdateCamera (exe RVA 0x64B3B60) is midhooked ONLY to capture `this` - RCX at the
	// function entry, before the prologue has moved anything. Nothing about the camera update is changed. The
	// POV's horizontal FOV is then a plain read at PCM + 0x14E0 (see InternalPointerData.xml for the full
	// derivation and the cross-check that validates the offset).
	//
	// This runs on the game thread once per frame, so it does the absolute minimum: one atomic store.
	// ---------------------------------------------------------------------------------------------------------
	std::atomic<uintptr_t> gCameraManager{ 0 };

	void cameraManagerUpdateHook(SafetyHookContext& ctx)
	{
		gCameraManager.store(ctx.rcx, std::memory_order_release);
	}

	std::string bytesToString(const std::vector<byte>& bytes)
	{
		std::string out;
		for (auto b : bytes)
		{
			if (!out.empty()) out += ' ';
			out += std::format("{:02X}", (uint32_t)(unsigned char)b);
		}
		return out;
	}

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
		std::string name;
		bool isSector = false;
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
	};

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

	// Structural offsets, from InternalPointerData.xml. The two ADDRESSES are resolved by signature instead -
	// see the header comment.
	int64_t mScenarioTriggerVolumeBlock = 0x278;
	int64_t mTriggerVolumeStride = 0x7C;
	int64_t mTriggerVolumeNameOffset = 0x00;
	int64_t mTriggerVolumeTypeOffset = 0x0C;
	// UE5 render camera. mCameraManagerHook is attached only while the overlay is on.
	std::shared_ptr<MultilevelPointer> mCameraManagerFunction;
	std::shared_ptr<std::vector<byte>> mCameraManagerOriginalBytes;
	std::shared_ptr<ModuleMidHook> mCameraManagerHook;
	int64_t mPovFovOffset = 0x14E0;
	std::mutex mCameraHookMutex;
	bool mCameraHookVerified = false;   // guarded by mCameraHookMutex, sticky once the site matches

	// Last FOV that read back as plausible. Seeded with a sane default so the overlay still draws on the very
	// first frames, before DoUpdateCamera has run even once.
	std::atomic<float> mLastGoodFov{ 78.f };

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
	std::string mAnchorFailure;

	// ---- cached geometry ----
	// GUARDED BY mVolumesMutex. Three different threads reach it: the render thread reads it every frame,
	// the hotkey/GUI thread rebuilds it on toggle, and the MCC state-hook thread clears it on a level change.
	// Without the lock a rebuild reallocates the vector out from under the render loop's iterator.
	// The render path holds it for the whole draw, which is fine - the other two are rare and brief.
	std::mutex mVolumesMutex;
	std::vector<HceTriggerVolume> mVolumes;
	uint32_t mCachedEncodedAddress = 0;
	int32_t mCachedCount = -1;
	std::chrono::steady_clock::time_point mLastRefresh{};

	// scratch, reused every frame so the render path allocates nothing
	std::vector<SimpleMath::Vector3> mCameraSpace;
	// Reused per volume so solid rendering allocates nothing per frame.
	std::vector<std::pair<float, size_t>> mFaceOrder;   // (centroid depth, face index), sorted far -> near
	std::vector<ImVec2> mPoly;

	// Whether a mission script has tested this volume recently. Supplied by HCETriggerActivity, which patches
	// the 8 HaloScript call sites of trigger_volume_test_point (NOT its prologue - 15 of its 23 call sites are
	// AI/damage/safe-zone code that polls every volume every tick, which would report everything as live).
	//
	// Optional: if the tracker failed to construct (an unrecognised build fails its patches closed), the overlay
	// still draws, everything just reads as dormant.
	std::optional<std::weak_ptr<HCETriggerActivity>> mTriggerActivityOptionalWeak;

	bool isVolumeActive(uint32_t volumeIndex) const
	{
		if (!mTriggerActivityOptionalWeak.has_value()) return false;
		auto activity = mTriggerActivityOptionalWeak.value().lock();
		return activity && activity->isVolumeActive(volumeIndex);
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

		mAnchorsGood = true;
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

			if (mStringIdText && readPtr(mStringIdTableSlot) != 0
				&& callStringIdText(mStringIdText, readU32(element + mTriggerVolumeNameOffset), nameBuffer, sizeof(nameBuffer)))
				volume.name = nameBuffer;
			else
				volume.name = std::format("trigger_{}", i);

			built.push_back(std::move(volume));
		}

		mVolumes = std::move(built);
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
		const float yaw = viewAngle.x;
		const float pitch = viewAngle.y;
		const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
		const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);

		Camera camera;
		camera.position = position;
		camera.forward = SimpleMath::Vector3(cosPitch * cosYaw, cosPitch * sinYaw, sinPitch);
		// forward x UnitZ. Blam's +Y is LEFT, so the camera's right is -Y at yaw 0 - the opposite of the
		// external CER tool's UE-frame (-sin, cos, 0). Do not "restore" that; it mirrors the whole overlay.
		camera.right = SimpleMath::Vector3(sinYaw, -cosYaw, 0.f);
		camera.up = SimpleMath::Vector3(-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch);

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
		const float pitch = DirectX::XMConvertToRadians(pitchDeg);
		const float yaw = DirectX::XMConvertToRadians(yawDeg);
		const float roll = DirectX::XMConvertToRadians(rollDeg);
		const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
		const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);

		SimpleMath::Vector3 forwardUe(cosPitch * cosYaw, cosPitch * sinYaw, sinPitch);
		SimpleMath::Vector3 rightUe(-sinYaw, cosYaw, 0.f);
		SimpleMath::Vector3 upUe(-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch);

		if (std::abs(roll) > 1e-6f)
		{
			const float cosRoll = std::cos(roll), sinRoll = std::sin(roll);
			const SimpleMath::Vector3 rolledRight = rightUe * cosRoll + upUe * sinRoll;
			const SimpleMath::Vector3 rolledUp = upUe * cosRoll - rightUe * sinRoll;
			rightUe = rolledRight;
			upUe = rolledUp;
		}

		Camera camera;
		camera.position = positionBlam;
		camera.forward = SimpleMath::Vector3(forwardUe.x, -forwardUe.y, forwardUe.z);
		camera.right = SimpleMath::Vector3(rightUe.x, -rightUe.y, rightUe.z);
		camera.up = SimpleMath::Vector3(upUe.x, -upUe.y, upUe.z);

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
	// The finite/range checks make a wrong offset or a changed build FAIL SOFT rather than draw nonsense.
	static constexpr float kUeCmPerWorldUnit = 304.8f;

	bool readUeCamera(SimpleMath::Vector3& outPositionBlam, float& outPitchDeg, float& outYawDeg,
		float& outRollDeg, float& outFovDeg)
	{
		const uintptr_t cameraManager = gCameraManager.load(std::memory_order_acquire);
		if (cameraManager == 0) return false;

		// FMinimalViewInfo base. The FOV offset is the anchor we store in pointer data; Location and Rotation sit
		// at fixed offsets INSIDE the same struct (Location +0x00, Rotation +0x18, FOV +0x30), so deriving the
		// base from the FOV offset keeps all three in lockstep if the struct ever moves.
		const uintptr_t pov = cameraManager + mPovFovOffset - 0x30;

		struct RawPov { double location[3]; double rotation[3]; float fov; } raw{};
		if (!HCEGetPlayerState::tryReadRaw(pov, &raw, offsetof(RawPov, fov) + sizeof(float))) return false;

		if (!std::isfinite(raw.fov) || raw.fov <= 10.f || raw.fov >= 170.f) return false;
		for (double d : raw.location) if (!std::isfinite(d)) return false;
		for (double d : raw.rotation) if (!std::isfinite(d)) return false;

		outPositionBlam = SimpleMath::Vector3(
			(float)(raw.location[0] / kUeCmPerWorldUnit),
			-(float)(raw.location[1] / kUeCmPerWorldUnit),   // UE +Y is RIGHT, Blam +Y is LEFT
			(float)(raw.location[2] / kUeCmPerWorldUnit));

		outPitchDeg = (float)raw.rotation[0];
		outYawDeg = (float)raw.rotation[1];
		outRollDeg = (float)raw.rotation[2];
		outFovDeg = raw.fov;
		mLastGoodFov.store(raw.fov, std::memory_order_release);
		return true;
	}

	// Refuse to hook a build we do not recognise, exactly as HCEInvulnerability does. Throws on mismatch.
	void attachCameraHook()
	{
		if (!mCameraManagerFunction || !mCameraManagerHook) return;

		std::scoped_lock lock(mCameraHookMutex);
		if (!mCameraHookVerified)
		{
			if (!mCameraManagerOriginalBytes || mCameraManagerOriginalBytes->empty())
				throw HCMRuntimeException("No expected original bytes for the HaloCER camera manager");

			const std::vector<byte>& expected = *mCameraManagerOriginalBytes;
			std::vector<byte> actual(expected.size());
			if (!mCameraManagerFunction->readArrayData(actual.data(), actual.size()))
				throw HCMRuntimeException(std::format("Could not read the HaloCER camera manager site: {}",
					MultilevelPointer::GetLastError()));

			if (actual != expected)
				throw HCMRuntimeException(std::format(
					"The HaloCER camera manager site did not match its expected original bytes - this build of "
					"Halo Campaign Evolved is not supported. Expected [{}], found [{}]",
					bytesToString(expected), bytesToString(actual)));

			mCameraHookVerified = true;
			PLOG_INFO << "HaloCER camera manager site matched its expected original bytes";
		}

		mCameraManagerHook->setWantsToBeAttached(true);
		if (!mCameraManagerHook->isHookInstalled())
		{
			mCameraManagerHook->setWantsToBeAttached(false);
			throw HCMRuntimeException("Failed to install the Halo Campaign Evolved camera manager hook");
		}
	}

	void detachCameraHook()
	{
		if (!mCameraManagerHook) return;
		mCameraManagerHook->setWantsToBeAttached(false);
		// The captured pointer belongs to the level that is unloading. Drop it so a stale APlayerCameraManager
		// is never read back on the next level - currentFov falls through to the last good value instead.
		gCameraManager.store(0, std::memory_order_release);
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;
		if (screenSize.x < 1.f || screenSize.y < 1.f) return;

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
					mLastGoodFov.load(std::memory_order_acquire));
			}

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
			const SimpleMath::Vector4 sectorBase = settings->triggerOverlaySectorColor->GetValue();
			const float wireAlpha = std::clamp(settings->triggerOverlayWireframeAlpha->GetValue(), 0.f, 1.f);
			const float solidAlpha = std::clamp(settings->triggerOverlayAlpha->GetValue(), 0.f, 1.f);

			// Pack once per frame per (class, style) rather than per volume.
			auto pack = [](SimpleMath::Vector4 c, float alpha)
				{
					return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alpha, 0.f, 1.f)));
				};
			const ImU32 boxWire = pack(normalColour, wireAlpha);
			const ImU32 sectorWire = pack(sectorBase, wireAlpha);
			const ImU32 activeWire = pack(activeColour, wireAlpha);
			const ImU32 boxFill = pack(normalColour, solidAlpha);
			const ImU32 sectorFill = pack(sectorBase, solidAlpha);
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

				const bool isLive = highlightActive && isVolumeActive(volume.index);
				const ImU32 colour = isLive ? activeWire : (volume.isSector ? sectorWire : boxWire);
				const ImU32 fill = isLive ? activeFill : (volume.isSector ? sectorFill : boxFill);

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
						RenderTextHelper::drawCenteredOutlinedText(volume.name,
							SimpleMath::Vector2(screen.x, screen.y), colour, labelScale * sizeScale);
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

			setActivityTracking(want);

			mIsActive = want && mAnchorsGood;
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp - the callbacks must be destroyed before anything they touch.
	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

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
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCETriggerOverlay only supports Halo Campaign Evolved");

		// Pure PointerDataStore lookups. NOTHING here touches game memory - the sim dll is not guaranteed to be
		// loaded when cheats are constructed (see the SAFETY note in HCECheckpointDetours.cpp).
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
#define hceOffset(member, name) member = *ptr->getData<std::shared_ptr<int64_t>>(nameof(name), mGame)
		hceOffset(mScenarioTriggerVolumeBlock, hceScenarioTriggerVolumeBlock);
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
		hceOffset(mPovFovOffset, hceCameraManagerPovFovOffset);
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

		mCameraManagerFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCameraManagerUpdateFunction), mGame);
		mCameraManagerOriginalBytes = ptr->getVectorData<byte>(nameof(hceCameraManagerUpdateOriginalBytes), mGame);

		// startEnabled = false, and the hook is on the EXE (L"main"), not the sim dll. Nothing is patched until
		// the overlay is actually switched on.
		mCameraManagerHook = ModuleMidHook::make(L"main", mCameraManagerFunction, &cameraManagerUpdateHook, false);

		mReady.store(true, std::memory_order_release);
	}

	~HCETriggerOverlayImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
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
