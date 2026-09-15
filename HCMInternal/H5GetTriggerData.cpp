#include "pch.h"
#include "H5GetTriggerData.h"
#include "H5GetPlayerState.h"
#include "H5TriggerVolumeNames.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5GetTriggerData.h for the full memory layout and for why categories come from index blocks.

namespace
{
	constexpr uintptr_t kRvaScenarioGlobals = 0x05A62538;

	constexpr uintptr_t kGlobalsVolumeArray = 0x840;
	constexpr uintptr_t kGlobalsVolumeCount = 0x850;
	constexpr uintptr_t kVolumeStride       = 0xC8;

	constexpr uintptr_t kVolNameHash   = 0x00;
	constexpr uintptr_t kVolShapeType  = 0x10;
	constexpr uintptr_t kVolForward    = 0x14;
	constexpr uintptr_t kVolUp         = 0x20;
	constexpr uintptr_t kVolCentre     = 0x2C;
	constexpr uintptr_t kVolExtents    = 0x40;
	constexpr uintptr_t kVolRadius     = 0xA4;

	// ---- sector geometry. All established from engine code, see buildSector. -------------------------
	constexpr uintptr_t kVolPointsPtr   = 0x54;   // -> real_point3d[], the polygon outline (LOCAL space)
	constexpr uintptr_t kVolPointsCount = 0x64;   // i32
	constexpr uintptr_t kPointStride    = 0x14;   // engine walks it with `mov ecx,0x14 / add rcx,0x14`
	constexpr uintptr_t kVolPrismsPtr   = 0x70;   // -> convex prisms; non-null IFF sector
	constexpr uintptr_t kVolPrismCount  = 0x80;   // i32
	constexpr uintptr_t kPrismStride    = 0x70;
	constexpr uintptr_t kPrismPlane0    = 0x00;   // (0,0,+1, zTop)
	constexpr uintptr_t kPrismPlane1    = 0x10;   // (0,0,-1, -zBottom)
	constexpr uintptr_t kVolLocalAabb   = 0x8C;   // 6 floats: x0,x1,y0,y1,z0,z1
	constexpr uintptr_t kVolKillIndex   = 0xA8;   // u16, 0xFFFF = not a kill trigger
	constexpr uint16_t  kNoKillIndex    = 0xFFFF;

	// A sector with more of either than this is not something we understand; skip rather than allocate.
	constexpr int32_t kMaxPoints = 4096;
	constexpr int32_t kMaxPrisms = 4096;

	constexpr uintptr_t kGlobalsZoneSwitchArray = 0x8B0;
	constexpr uintptr_t kGlobalsZoneSwitchCount = 0x8C0;
	constexpr uintptr_t kZoneSwitchStride       = 0x08;

	constexpr uintptr_t kGlobalsKillArray = 0xBD8;
	constexpr uintptr_t kGlobalsKillCount = 0xBE8;
	constexpr uintptr_t kKillStride       = 0x04;

	constexpr uint16_t kNoneU16 = 0xFFFF;

	// A scenario with more volumes than this is not a scenario we understand; refuse rather than allocate on
	// a garbage count read during a load.
	constexpr int32_t kMaxVolumes = 8192;

	bool sehCopy(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template<typename T>
	bool readAt(uintptr_t addr, T& out)
	{
		if (!addr) return false;
		return sehCopy(&out, (const void*)addr, sizeof(T));
	}

	SimpleMath::Vector3 readVec3(const uint8_t* base, uintptr_t off)
	{
		float v[3]{};
		memcpy(v, base + off, sizeof(v));
		return { v[0], v[1], v[2] };
	}

	bool finite3(const SimpleMath::Vector3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}
}

class H5GetTriggerData::Impl
{
private:
	GameState mGame;
	std::weak_ptr<H5GetPlayerState> mPlayerStateWeak;

	std::vector<Volume> mVolumes;
	uintptr_t mCachedGlobals = 0;
	uintptr_t mCachedArray = 0;
	int32_t mCachedCount = -1;
	bool mHaveSnapshot = false;

	uintptr_t getScenarioGlobals()
	{
		lockOrThrow(mPlayerStateWeak, playerState);
		uintptr_t g = 0;
		if (!readAt(playerState->getExeBase() + kRvaScenarioGlobals, g) || !g)
			throw HCMRuntimeException("No Halo 5 scenario is loaded");
		return g;
	}

	// ⚠ The box is ORIENTED. forward/up give its frame; right is their cross product. Building corners from
	// an axis-aligned box would be wrong for every rotated volume, and plenty of them are rotated (the very
	// first volume in the test scenario has forward (0.731, -0.683, 0)).
	//
	// ⚠⚠ THE ONE ASSUMPTION HERE THAT IS **NOT** INDEPENDENTLY VERIFIED: that extents.x measures along
	// FORWARD, extents.y along RIGHT and extents.z along UP. That is the conventional Blam local frame and
	// the tag lists the fields in that order, but nothing proves it - the +0xA4 bounding-radius check that
	// confirmed the extents are full dimensions is a length, so it is invariant to permuting the axes and
	// cannot distinguish this.
	// HOW IT WOULD LOOK IF WRONG: every NON-SQUARE volume appears rotated 90 degrees in plan view while
	// cubic ones look perfect, and the error does not change with the camera. The fix is to swap the x and
	// y terms below. Not guessed at further because it needs one look in game to settle.
	static void buildCorners(Volume& v)
	{
		SimpleMath::Vector3 f = v.forward, u = v.up;
		if (f.LengthSquared() < 1e-8f) f = { 1.f, 0.f, 0.f };
		if (u.LengthSquared() < 1e-8f) u = { 0.f, 0.f, 1.f };
		f.Normalize();
		u.Normalize();

		// ⚠⚠⚠ THE SECOND AXIS IS `up CROSS forward`, NOT `forward CROSS up`. Getting this backwards
		// MIRRORS every volume across its own forward axis, which moves it sideways by its full width.
		//
		// Straight out of the engine's point transform, exe+0x00676830. With d = point - translation:
		//     out.x = dot(d, [mat+0x04])   = dot(d, forward)
		//     out.y = dot(d, [mat+0x10])   = dot(d, up x forward)
		//     out.z = dot(d, [mat+0x1C])   = dot(d, up)
		// and exe+0x00677850 fills mat+0x10 by calling exe+0x00678BF0(up, forward) - a plain cross
		// product, decoded term by term - so the middle row is up x forward.
		//
		// ⚠ A LIVE CONTAINMENT TEST GOT THIS WRONG AND I BELIEVED IT: sampling the player at 64 positions
		// scored `forward x up` at 64/64 versus 28/64 for the correct axis. A MIRRORED box still contains
		// the player whenever he is near the volume's forward axis, which is most of the time in a
		// corridor, so the count favours the wrong answer. Containment sampling cannot resolve a mirror;
		// only the engine's own transform can. The user's screenshot agreed with the disassembly.
		SimpleMath::Vector3 r = u.Cross(f);
		if (r.LengthSquared() < 1e-8f) r = { 0.f, 1.f, 0.f };
		r.Normalize();
		// Re-orthogonalise up, so a slightly non-perpendicular pair from the tag cannot shear the box.
		u = f.Cross(r);
		u.Normalize();

		// ⚠⚠⚠ THE STORED POSITION IS A CORNER, ON ALL THREE AXES. The box spans [0, extents] along each
		// local axis - it does NOT straddle the position on any of them.
		//
		// This is not inferred, it is the ENGINE'S OWN TEST, read out of the box branch of
		// ManagedGameEngineTriggerVolume_ContainsPoint (exe+0x0144A5B0 -> exe+0x00A85700, box path at
		// exe+0x00A85848). After transforming the world point into the volume's frame it does exactly:
		//     local.x > 0 && extents.x > local.x        (exe+0x00A858B4, +0x00A858D5)
		//     local.y > 0 && extents.y > local.y        (exe+0x00A858C1, +0x00A858E0)
		//     local.z > 0 && extents.z > local.z        (exe+0x00A858CB, +0x00A858EB)
		// and returns 1. Six compares, no halving anywhere.
		//
		// TWO WRONG VERSIONS SHIPPED BEFORE THIS, each found by a user looking at the overlay:
		//   1. centred on all three axes    -> every box half its height too low as well as offset in XY.
		//   2. centred in F/R, based in U   -> height fixed, but still offset by HALF THE EXTENTS in both
		//      horizontal axes, which is what "all volumes misplaced X/Y by a decent amount" was.
		// The lesson: this engine's own predicate was readable the whole time and settles in one look what
		// containment sampling could only rank. Read the engine's test before modelling its data.
		//
		// ⚠ Axis order, all three from exe+0x00676830: extents.x along FORWARD, extents.y along
		// UP x FORWARD (see the basis note above), extents.z along UP.
		v.axisF = f;
		v.axisR = r;
		v.axisU = u;
		v.halfExtents = v.extents * 0.5f;
		// The TRUE geometric centre, for labels and distance culling - the far corner is origin + all three
		// extents, so the middle is half of that.
		v.centre = v.origin + (f * v.extents.x + r * v.extents.y + u * v.extents.z) * 0.5f;

		// Corner index bits stay (bit2 = x, bit1 = y, bit0 = z), so the triangle and edge index lists in
		// H5TriggerOverlay remain correct.
		int i = 0;
		for (int sx = 0; sx <= 1; ++sx)
			for (int sy = 0; sy <= 1; ++sy)
				for (int sz = 0; sz <= 1; ++sz)
					v.corners[i++] = v.origin + f * (v.extents.x * (float)sx)
						+ r * (v.extents.y * (float)sy)
						+ u * (v.extents.z * (float)sz);
	}

	// Turn the 8 corners into the shared mesh form the overlay draws.
	static void buildBoxMesh(Volume& v)
	{
		// Corner index bits are (bit2 = x, bit1 = y, bit0 = z) - see buildCorners.
		static constexpr uint16_t tris[36] = {
			0,1,3, 0,3,2,  4,6,7, 4,7,5,  0,4,5, 0,5,1,
			2,3,7, 2,7,6,  0,2,6, 0,6,4,  1,5,7, 1,7,3,
		};
		static constexpr uint16_t edges[24] = {
			0,1, 1,3, 3,2, 2,0,  4,5, 5,7, 7,6, 6,4,  0,4, 1,5, 2,6, 3,7,
		};
		v.meshVerts.clear();
		v.meshVerts.reserve(8);
		for (const auto& c : v.corners) v.meshVerts.emplace_back(c);
		v.meshTriangles.assign(std::begin(tris), std::end(tris));
		v.meshEdges.assign(std::begin(edges), std::end(edges));
	}

	// ⚠⚠ SECTOR VOLUMES ARE EXTRUDED POLYGONS, NOT BOXES, AND THEIR extents FIELD IS JUNK.
	//
	// Everything here comes from the engine, not from pattern-matching the data:
	//   * exe+0x00A835B0 (the sector centroid helper) walks the POINT LOOP: it gates on i32 [vol+0x64],
	//     loads [vol+0x54], and steps with `mov ecx,0x14 / add rcx,0x14` reading 3 floats per element -
	//     which is where the pointer, the count and the 0x14 stride all come from.
	//   * exe+0x00A85940 (the sector containment kernel) tests, per prism, five planes as
	//     dot3(n, p) <= d with the constant in lane 3, and the caller ORs the prisms together.
	//   * exe+0x00679E10 is the AABB early-out at [vol+0x8C], INCLUSIVE on both ends, NaN fails.
	// Cross-checked live across all 173 sectors of the test scenario: plane 0 is exactly (0,0,+1) and
	// plane 1 exactly (0,0,-1) on every prism of every sector (error 0.0, not approximately), the three
	// side planes always have nz == 0 exactly and unit length to 1.3e-7, and the caps are identical
	// across all prisms of a volume - so prism 0 is authoritative for zTop/zBottom.
	//
	// ⚠ Everything in the prism and point blocks is in the volume's LOCAL frame, so it all has to be
	// pushed back out through the basis to draw. local -> world is the transpose of the world -> local
	// transform at exe+0x00676830: p = origin + lx*forward + ly*(up x forward) + lz*up.
	//
	// ⚠ The polygon may be CONCAVE (L-shaped rooms are ordinary), so the caps are NOT a triangle fan.
	// We do not need to triangulate it ourselves though: the prisms already ARE a valid triangulation -
	// verified combinatorially, with every polygon edge used once, every diagonal twice, and the summed
	// triangle area equal to the polygon area to a relative error of 0.
	void buildSector(Volume& v, const uint8_t* rec)
	{
		uintptr_t prismPtr = 0; int32_t prismCount = 0;
		memcpy(&prismPtr, rec + kVolPrismsPtr, sizeof(prismPtr));
		memcpy(&prismCount, rec + kVolPrismCount, sizeof(prismCount));
		if (!prismPtr || prismCount <= 0 || prismCount > kMaxPrisms) return;

		std::vector<uint8_t> blk((size_t)prismCount * kPrismStride);
		if (!sehCopy(blk.data(), (const void*)prismPtr, blk.size())) return;

		v.prisms.reserve((size_t)prismCount);
		for (int32_t i = 0; i < prismCount; ++i)
		{
			const uint8_t* pr = blk.data() + (size_t)i * kPrismStride;
			Volume::Prism out;
			bool ok = true;
			for (int pl = 0; pl < 5; ++pl)
			{
				float f[4]{};
				memcpy(f, pr + (size_t)pl * 0x10, sizeof(f));
				if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3]))
				{
					ok = false; break;
				}
				out.planesLocal[(size_t)pl] = { f[0], f[1], f[2], f[3] };
			}
			if (ok) v.prisms.push_back(out);
		}
		if (v.prisms.empty()) return;

		// Caps are uniform across prisms, so prism 0 is exact: plane0 = (0,0,+1, zTop),
		// plane1 = (0,0,-1, -zBottom).
		const float zTop = v.prisms[0].planesLocal[0].w;
		const float zBottom = -v.prisms[0].planesLocal[1].w;
		if (!std::isfinite(zTop) || !std::isfinite(zBottom) || zTop < zBottom) return;

		float aabb[6]{};
		memcpy(aabb, rec + kVolLocalAabb, sizeof(aabb));
		bool aabbOk = true;
		for (int i = 0; i < 6; ++i) if (!std::isfinite(aabb[i])) aabbOk = false;
		if (aabbOk && aabb[0] <= aabb[1] && aabb[2] <= aabb[3] && aabb[4] <= aabb[5])
		{
			std::copy(std::begin(aabb), std::end(aabb), v.localAabb.begin());
			v.hasLocalAabb = true;
		}

		// ---- the polygon outline, for the wireframe and the side walls ----
		uintptr_t ptsPtr = 0; int32_t ptsCount = 0;
		memcpy(&ptsPtr, rec + kVolPointsPtr, sizeof(ptsPtr));
		memcpy(&ptsCount, rec + kVolPointsCount, sizeof(ptsCount));
		std::vector<SimpleMath::Vector2> poly;
		if (ptsPtr && ptsCount >= 3 && ptsCount <= kMaxPoints)
		{
			std::vector<uint8_t> pb((size_t)ptsCount * kPointStride);
			if (sehCopy(pb.data(), (const void*)ptsPtr, pb.size()))
			{
				poly.reserve((size_t)ptsCount);
				for (int32_t i = 0; i < ptsCount; ++i)
				{
					float p[3]{};
					memcpy(p, pb.data() + (size_t)i * kPointStride, sizeof(p));
					if (!std::isfinite(p[0]) || !std::isfinite(p[1])) { poly.clear(); break; }
					poly.emplace_back(p[0], p[1]);
				}
			}
		}

		const auto toWorld = [&](float lx, float ly, float lz)
			{
				return v.origin + v.axisF * lx + v.axisR * ly + v.axisU * lz;
			};

		v.meshVerts.clear();
		v.meshTriangles.clear();
		v.meshEdges.clear();

		// CAPS: one bottom and one top triangle per prism, using the prism's own three 2D verts. That
		// reuses the engine's triangulation instead of inventing one, which is what makes concave
		// polygons come out right.
		for (int32_t i = 0; i < prismCount && (size_t)i < v.prisms.size(); ++i)
		{
			const uint8_t* pr = blk.data() + (size_t)i * kPrismStride;
			float t[6]{};
			memcpy(t, pr + 0x50, sizeof(t));
			bool ok = true;
			for (float f : t) if (!std::isfinite(f)) ok = false;
			if (!ok) continue;
			if (v.meshVerts.size() + 6 > 60000) break;   // uint16 indices

			const uint16_t b = (uint16_t)v.meshVerts.size();
			for (int k = 0; k < 3; ++k)
				v.meshVerts.emplace_back(toWorld(t[k * 2], t[k * 2 + 1], zBottom));
			for (int k = 0; k < 3; ++k)
				v.meshVerts.emplace_back(toWorld(t[k * 2], t[k * 2 + 1], zTop));

			const uint16_t bot[3] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2) };
			const uint16_t top[3] = { (uint16_t)(b + 3), (uint16_t)(b + 4), (uint16_t)(b + 5) };
			for (uint16_t x : bot) v.meshTriangles.push_back(x);
			for (uint16_t x : top) v.meshTriangles.push_back(x);
		}

		// SIDE WALLS + WIREFRAME from the outer polygon loop only, so the internal triangulation is
		// neither drawn nor outlined.
		if (poly.size() >= 3)
		{
			const uint16_t base = (uint16_t)v.meshVerts.size();
			const size_t n = poly.size();
			if (base + n * 2 < 60000)
			{
				for (size_t i = 0; i < n; ++i) v.meshVerts.emplace_back(toWorld(poly[i].x, poly[i].y, zBottom));
				for (size_t i = 0; i < n; ++i) v.meshVerts.emplace_back(toWorld(poly[i].x, poly[i].y, zTop));

				for (size_t i = 0; i < n; ++i)
				{
					const uint16_t b0 = (uint16_t)(base + i);
					const uint16_t b1 = (uint16_t)(base + (i + 1) % n);
					const uint16_t t0 = (uint16_t)(base + n + i);
					const uint16_t t1 = (uint16_t)(base + n + (i + 1) % n);
					v.meshTriangles.insert(v.meshTriangles.end(), { b0, b1, t1, b0, t1, t0 });
					v.meshEdges.insert(v.meshEdges.end(), { b0, b1, t0, t1, b0, t0 });
				}
			}
		}
	}

	void applyCategories(uintptr_t globals)
	{
		lockOrThrow(mPlayerStateWeak, playerState);

		// ---- zone set switch volumes ----
		uintptr_t zsArr = 0; int32_t zsCount = 0;
		if (readAt(globals + kGlobalsZoneSwitchArray, zsArr) && zsArr
			&& readAt(globals + kGlobalsZoneSwitchCount, zsCount)
			&& zsCount > 0 && zsCount <= kMaxVolumes)
		{
			std::vector<uint8_t> blk((size_t)zsCount * kZoneSwitchStride);
			if (sehCopy(blk.data(), (const void*)zsArr, blk.size()))
			{
				for (int32_t i = 0; i < zsCount; ++i)
				{
					const uint8_t* e = blk.data() + (size_t)i * kZoneSwitchStride;
					uint16_t beginZs = 0, volIdx = 0, commitZs = 0;
					memcpy(&beginZs, e + 0x02, 2);
					memcpy(&volIdx, e + 0x04, 2);
					memcpy(&commitZs, e + 0x06, 2);
					if (volIdx >= mVolumes.size()) continue;

					Volume& v = mVolumes[volIdx];
					if (beginZs != kNoneU16)
					{
						v.category = Category::BeginZoneSet;
						v.zoneSetIndex = beginZs;
					}
					else if (commitZs != kNoneU16)
					{
						v.category = Category::CommitZoneSet;
						v.zoneSetIndex = commitZs;
					}

					if (v.zoneSetIndex >= 0)
					{
						try { v.zoneSetName = playerState->getZoneSetName(v.zoneSetIndex); }
						catch (HCMRuntimeException&) { v.zoneSetName.clear(); }

						// ★ Zone set volume names are "zone_set:<zs>" / "begin_zone_set:<zs>". They contain a
						// colon, so they are never Lua identifiers and are absent from the name table - but
						// they are perfectly predictable, so synthesise and hash-check them here. This works
						// on every level with no table entry at all.
						if (v.name.empty() && !v.zoneSetName.empty())
						{
							const std::string candidate = (v.category == Category::BeginZoneSet)
								? ("begin_zone_set:" + v.zoneSetName)
								: ("zone_set:" + v.zoneSetName);
							if (H5TriggerVolumeNames::murmur3(candidate) == v.nameHash)
								v.name = candidate;
						}
					}
				}
			}
		}

		// ---- kill volumes ----
		// ★ Taken from the PER-VOLUME field the engine itself reads (volume+0xA8, set during the read
		// loop), not from the sibling index block at globals+0xBD8. The block agrees exactly - it lists
		// the same 30 rows in kill-index order - but the field is the engine's own answer and needs no
		// cross-referencing, so the block is left unread.
		// ⚠ Do not overwrite a zone set category: a volume can be both, and the zone set role is the one
		// that matters for navigation.
		for (auto& v : mVolumes)
		{
			if (v.index >= 0 && v.isKillVolume && v.category == Category::Regular)
				v.category = Category::Kill;
		}
	}

	void rebuild()
	{
		const uintptr_t globals = getScenarioGlobals();

		uintptr_t arr = 0; int32_t count = 0;
		if (!readAt(globals + kGlobalsVolumeArray, arr) || !arr)
			throw HCMRuntimeException("The Halo 5 trigger volume array is null");
		if (!readAt(globals + kGlobalsVolumeCount, count) || count <= 0)
			throw HCMRuntimeException("Halo 5 reports no trigger volumes");
		if (count > kMaxVolumes)
			throw HCMRuntimeException(std::format(
				"Halo 5 reports an implausible trigger volume count ({})", count));

		// One bulk read. 379 * 0xC8 is about 74KB - far cheaper than 379 separate cross-page reads, and it
		// gives a consistent snapshot rather than one smeared across the rebuild.
		std::vector<uint8_t> raw((size_t)count * kVolumeStride);
		if (!sehCopy(raw.data(), (const void*)arr, raw.size()))
			throw HCMRuntimeException("Could not read the Halo 5 trigger volume array");

		std::vector<Volume> out;
		out.reserve((size_t)count);
		for (int32_t i = 0; i < count; ++i)
		{
			const uint8_t* e = raw.data() + (size_t)i * kVolumeStride;
			Volume v;
			v.index = i;
			memcpy(&v.nameHash, e + kVolNameHash, sizeof(v.nameHash));

			// ⚠⚠ THE SHAPE FLAG IS A **u16**, NOT A u32. Reading it as 32 bits was a real shipped bug: the
			// adjacent halfword at +0x12 is a constant 0xBCBC, so a u32 read yields 0xBCBC0000 / 0xBCBC0001
			// and the `== 1` test can never fire. Every sector volume was therefore misclassified as a box
			// and drawn, despite the filter being off.
			// Verified: the u16 test agrees with "the +0x70 geometry pointer is non-null" on 379/379 volumes.
			uint16_t shape = 0;
			memcpy(&shape, e + kVolShapeType, sizeof(shape));
			v.shape = (shape == 1) ? Shape::Sector : Shape::Box;

			v.forward = readVec3(e, kVolForward);
			v.up      = readVec3(e, kVolUp);
			v.origin  = readVec3(e, kVolCentre);   // NOT the centre - buildCorners derives that
			v.extents = readVec3(e, kVolExtents);
			memcpy(&v.boundingRadius, e + kVolRadius, sizeof(v.boundingRadius));

			// Skip anything that cannot be drawn rather than emitting NaN geometry into the renderer.
			if (!finite3(v.origin) || !finite3(v.extents)) continue;
			if (v.extents.x <= 0.f && v.extents.y <= 0.f && v.extents.z <= 0.f) continue;

			if (const auto* ent = H5TriggerVolumeNames::find(v.nameHash))
			{
				v.name.assign(ent->name);
				v.scriptTier = ent->tier;
			}

			// Matched by HASH, not by the resolved name, so a volume whose name is not in the corpus can
			// still be flagged if its hash is one of the level's goal volumes.
			v.speedrunStep = H5SpeedrunTriggers::stepOf(v.nameHash);
			v.isSpeedrun = v.speedrunStep != 0;

			// ★ The engine's OWN kill flag, read per volume rather than inferred from a sibling block.
			// kill_volume_enable / kill_volume_disable (exe+0x00A7E710 / +0x00A7E7C0) both load this u16
			// and treat 0xFFFF as "not a kill trigger".
			memcpy(&v.killIndex, e + kVolKillIndex, sizeof(v.killIndex));
			v.isKillVolume = (v.killIndex != kNoKillIndex);

			buildCorners(v);
			if (v.shape == Shape::Sector)
			{
				buildSector(v, e);
				// A sector we could not decode has no geometry and must not fall back to its extents -
				// they are stale junk for sectors (9 distinct values across the 173 in the test scenario,
				// NOT the 1.0 placeholders an earlier pass assumed).
				if (v.prisms.empty()) continue;
			}
			else
			{
				buildBoxMesh(v);
			}
			out.push_back(std::move(v));
		}

		// ⚠ Index by ARRAY POSITION, not by push order - the category blocks reference the engine's index and
		// the loop above can skip degenerate entries. Pad so mVolumes[i] is always engine volume i.
		mVolumes.assign((size_t)count, Volume{});
		for (auto& v : out) mVolumes[(size_t)v.index] = std::move(v);

		applyCategories(globals);

		mCachedGlobals = globals;
		mCachedArray = arr;
		mCachedCount = count;
		mHaveSnapshot = true;
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mPlayerStateWeak(resolveDependentCheat(H5GetPlayerState))
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5GetTriggerData only supports Halo 5: Forge");
	}

	const std::vector<Volume>& getVolumes()
	{
		// Cheap staleness check so this is safe to call every frame: the scenario pointer, the array pointer
		// and the count all have to be unchanged for the snapshot to stand.
		if (mHaveSnapshot)
		{
			const uintptr_t globals = getScenarioGlobals();
			uintptr_t arr = 0; int32_t count = 0;
			if (globals == mCachedGlobals
				&& readAt(globals + kGlobalsVolumeArray, arr) && arr == mCachedArray
				&& readAt(globals + kGlobalsVolumeCount, count) && count == mCachedCount)
				return mVolumes;
		}
		rebuild();
		return mVolumes;
	}

	void invalidate() noexcept
	{
		mHaveSnapshot = false;
		mCachedGlobals = 0;
		mCachedArray = 0;
		mCachedCount = -1;
		mVolumes.clear();
	}
};


H5GetTriggerData::H5GetTriggerData(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5GetTriggerData::~H5GetTriggerData() { PLOG_VERBOSE << "~" << getName(); }

const std::vector<H5GetTriggerData::Volume>& H5GetTriggerData::getVolumes() { return pimpl->getVolumes(); }
void H5GetTriggerData::invalidate() noexcept { pimpl->invalidate(); }

std::string_view H5GetTriggerData::categoryName(Category c) noexcept
{
	switch (c)
	{
	case Category::Kill:           return "Kill";
	case Category::BeginZoneSet:   return "Begin Zone Set";
	case Category::CommitZoneSet:  return "Zone Set Commit";
	default:                       return "Regular";
	}
}
