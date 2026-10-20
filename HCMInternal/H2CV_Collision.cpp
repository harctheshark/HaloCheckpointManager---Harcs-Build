#include "pch.h"
#include "H2CV_Collision.h"
#include "H2CV_Engine.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <cmath>

using namespace mem;

namespace h2cv
{
	namespace
	{
		constexpr uintptr_t kCollisionBspBlock = 0x14;  // sbsp -> collision_bsp tag block
		constexpr uintptr_t kSurfacesBlock = 0x28, kEdgesBlock = 0x30, kVerticesBlock = 0x38;
		constexpr uintptr_t kInstancesBlock = 0x140, kDefinitionsBlock = 0x138; // sbsp IG blocks
		constexpr size_t kInstanceStride = 0x58, kDefinitionStride = 0xC8;
		constexpr uintptr_t kInstDefIndexOff = 0x34, kDefCollisionOff = 0xB4, kMidCollbspPtrOff = 0x18;
		// object -> def tag (+0x00 u16 index) -> hlmt (+0x38) -> coll (+0x0C); coll walked exactly like the engine's
		// sub_180734FB0: regions @+0x1C (stride 12) -> permutations @region+4 (stride 20) -> bsps @perm+4 (stride 0x44)
		constexpr uintptr_t kDefModelRefOff = 0x38, kModelCollRefOff = 0x0C;
		constexpr uintptr_t kCollRegionsBlock = 0x1C, kRegionPermsBlock = 4, kPermBspsBlock = 4;
		constexpr size_t kCollRegionStride = 12, kCollPermStride = 20, kCollModelBspStride = 0x44;
		// hlmt region table (sub_180793C40): regions {count @+0x70, offset @+0x74} stride 16, perms stride 8
		constexpr uintptr_t kModelRegionsBlock = 0x70;
		constexpr size_t kModelRegionStride = 16, kModelPermStride = 8;
		constexpr uint8_t kHdrBeingDeleted = 0x10;   // object header entry +2
		constexpr uint8_t kInvisible = 0x02, kBreakable = 0x08, kInvalid = 0x10;

		// real_matrix4x3 sanity (scale, forward, left, up, position): finite, positive scale, unit axes, bounded position
		bool sanePlacement(const float* m)
		{
			for (int i = 0; i < 13; ++i) if (!std::isfinite(m[i])) return false;
			if (!(m[0] > 1e-4f && m[0] < 1e4f)) return false;
			for (int a = 1; a <= 7; a += 3)   // forward, left, up
			{
				const float l2 = m[a] * m[a] + m[a + 1] * m[a + 1] + m[a + 2] * m[a + 2];
				if (l2 < 0.8f || l2 > 1.25f) return false;
			}
			return std::fabs(m[10]) < 1e5f && std::fabs(m[11]) < 1e5f && std::fabs(m[12]) < 1e5f;
		}
	}

	const char* catName(Cat c)
	{
		switch (c)
		{
		case Cat::Bsp: return "BSP";
		case Cat::BspInvisible: return "BSP invisible (*)";
		case Cat::Breakable: return "Breakable";
		case Cat::Instanced: return "Instanced";
		case Cat::InstancedInvisible: return "Instanced invisible";
		case Cat::Scenery: return "Scenery";
		case Cat::Crate: return "Crates";
		case Cat::Machine: return "Machines";
		case Cat::KillTrigger: return "Kill triggers";
		case Cat::SoftCeiling: return "Soft ceilings";
		default: return "?";
		}
	}

	Vec3 igTransform(const float* m, const Vec3& v)
	{
		const float s = m[0];
		const float x = v.x * s, y = v.y * s, z = v.z * s;
		return { x * m[1] + y * m[4] + z * m[7] + m[10],
		         x * m[2] + y * m[5] + z * m[8] + m[11],
		         x * m[3] + y * m[6] + z * m[9] + m[12] };
	}

	bool CollBsp::parse(uintptr_t collbsp, const h2::TagPools& p)
	{
		surfaces.clear(); edges.clear(); verts.clear(); planes.clear();
		// planes block {count, offset} @+0x08 (collision_bsp: bsp3d nodes +0, planes +8, leaves +0x10, bsp2d refs +0x18,
		// bsp2d nodes +0x20, surfaces +0x28 ...); optional - only the crease highlight uses it
		{
			uintptr_t pF = 0; uint32_t pN = 0;
			if (h2::resolveBlock(collbsp + 0x08, p, pF, pN) && pN > 0 && pN < 200000)
			{
				planes.resize((size_t)pN * 4);
				if (!copy(planes.data(), pF, planes.size() * 4)) planes.clear();
			}
		}
		uintptr_t sF = 0, eF = 0, vF = 0; uint32_t sN = 0, eN = 0, vN = 0;
		if (!h2::resolveBlock(collbsp + kSurfacesBlock, p, sF, sN)) return false;
		if (!h2::resolveBlock(collbsp + kEdgesBlock, p, eF, eN)) return false;
		if (!h2::resolveBlock(collbsp + kVerticesBlock, p, vF, vN)) return false;
		if (sN > 200000 || eN > 400000 || vN > 200000 || !eN || !vN) return false;
		struct RawSurf { uint16_t plane, firstEdge; uint8_t flags; int8_t breakable; uint16_t material; };   // 8-byte tag records
		struct RawEdge { uint16_t v0, v1, fwd, rev, left, right; };                                         // 12-byte tag records
		std::vector<RawSurf> rs(sN); std::vector<RawEdge> re(eN);
		std::vector<float> vraw((size_t)vN * 4);
		if (sN && !copy(rs.data(), sF, (size_t)sN * 8)) return false;
		if (!copy(re.data(), eF, (size_t)eN * 12)) return false;
		if (!copy(vraw.data(), vF, vraw.size() * 4)) return false;
		surfaces.resize(sN); edges.resize(eN);
		for (uint32_t i = 0; i < sN; ++i)
		{
			const RawSurf& r = rs[i];
			surfaces[i] = { (uint32_t)(r.plane & 0x7FFF), r.firstEdge, (r.plane & 0x8000) != 0, r.flags, r.breakable, r.material, (uint16_t)(uint8_t)r.breakable, 0 };
		}
		auto edgeRef = [](uint16_t v) -> uint32_t { return v == 0xFFFF ? kNone : v; };
		for (uint32_t i = 0; i < eN; ++i)
		{
			const RawEdge& r = re[i];
			edges[i] = { r.v0, r.v1, r.fwd, r.rev, edgeRef(r.left), edgeRef(r.right) };
		}
		verts.resize(vN);
		for (uint32_t i = 0; i < vN; ++i) verts[i] = { vraw[i * 4 + 0], vraw[i * 4 + 1], vraw[i * 4 + 2] };
		return true;
	}

	bool CollBsp::polygon(uint32_t s, std::vector<Vec3>& out) const
	{
		out.clear();
		if (s >= surfaces.size()) return false;
		// the engine's own walk (sub_180791FB0): RIGHT surface checked first; Havok assumes <= 8 vertices
		uint32_t e = surfaces[s].firstEdge;
		const uint32_t start = e;
		for (int guard = 0; guard < 16; ++guard)
		{
			if (e >= edges.size()) return false;
			const Edge& ed = edges[e];
			uint32_t v, next;
			if (ed.right == s) { v = ed.v1; next = ed.rev; }
			else { v = ed.v0; next = ed.fwd; }
			if (v >= verts.size()) return false;
			out.push_back(verts[v]);
			e = next;
			if (e == start) return out.size() >= 3;
		}
		return false;
	}

	bool CollBsp::normal(uint32_t s, Vec3& out) const
	{
		if (s >= surfaces.size()) return false;
		const uint32_t pi = surfaces[s].plane;
		if ((size_t)pi * 4 + 3 >= planes.size()) return false;
		Vec3 n{ planes[pi * 4 + 0], planes[pi * 4 + 1], planes[pi * 4 + 2] };
		const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		if (!(l > 0.5f && l < 1.5f)) return false;
		n = { n.x / l, n.y / l, n.z / l };
		if (surfaces[s].flip) n = { -n.x, -n.y, -n.z };
		out = n;
		return true;
	}

	void CollisionWorld::findCreases(const CollBsp& bsp, const float* m)
	{
		const size_t sN = bsp.surfaces.size(), vN = bsp.verts.size();
		if (!sN || bsp.planes.empty()) return;
		auto X = [&](const Vec3& v) { return m ? igTransform(m, v) : v; };
		auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
		auto sub = [](const Vec3& a, const Vec3& b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; };
		// world-space normal + centroid per surface, computed lazily (only surfaces on candidate edges)
		struct SInfo { bool done = false, ok = false; Vec3 n{}, c{}; };
		std::vector<SInfo> si(sN);
		std::vector<Vec3> poly;
		auto info = [&](uint32_t s) -> const SInfo&
		{
			SInfo& r = si[s];
			if (r.done) return r;
			r.done = true;
			if (bsp.surfaces[s].flags & 0x10) return r;                      // invalid
			Vec3 n;
			if (!bsp.normal(s, n) || !bsp.polygon(s, poly)) return r;
			// winding cross-check (diagnostics only): Newell normal vs the signed plane normal
			Vec3 nw{};
			for (size_t i = 0; i < poly.size(); ++i)
			{
				const Vec3& a = poly[i]; const Vec3& b = poly[(i + 1) % poly.size()];
				nw.x += (a.y - b.y) * (a.z + b.z); nw.y += (a.z - b.z) * (a.x + b.x); nw.z += (a.x - b.x) * (a.y + b.y);
			}
			if (dot(nw, n) >= 0) ++mPlaneAgree; else ++mPlaneDisagree;
			Vec3 c{};
			for (auto& v : poly) { const Vec3 w = X(v); c.x += w.x; c.y += w.y; c.z += w.z; }
			const float k = 1.f / (float)poly.size();
			r.c = { c.x * k, c.y * k, c.z * k };
			if (m) { const Vec3 o = X({ 0, 0, 0 }); Vec3 t = sub(X(n), o); const float l = std::sqrt(dot(t, t)); if (!(l > 1e-6f)) return r; n = { t.x / l, t.y / l, t.z / l }; }
			r.n = n; r.ok = true;
			return r;
		};
		std::vector<uint8_t> faceDone(sN, 0);
		auto addFace = [&](uint32_t s)
		{
			if (faceDone[s]) return;
			faceDone[s] = 1;
			if (!bsp.polygon(s, poly)) return;
			for (size_t i = 0; i < poly.size(); ++i) mCreaseFaces.push_back({ X(poly[i]), X(poly[(i + 1) % poly.size()]) });
		};
		for (const auto& e : bsp.edges)
		{
			if (e.left >= sN || e.right >= sN || e.left == e.right || e.v0 >= vN || e.v1 >= vN) continue;
			const SInfo& A = info(e.left);
			const SInfo& B = info(e.right);
			if (!A.ok || !B.ok) continue;
			const float d = dot(A.n, B.n);
			if (d >= -0.02f) continue;                                         // 90 degrees or wider: can't be charged
			// concave: each face lies in front of the other's plane (a convex ridge has them behind)
			if (dot(A.n, sub(B.c, A.c)) <= 1e-3f || dot(B.n, sub(A.c, B.c)) <= 1e-3f) continue;
			mCreases.push_back({ { X(bsp.verts[e.v0]), X(bsp.verts[e.v1]) }, std::min(1.f, -d) });
			addFace(e.left); addFace(e.right);
		}
	}

	bool CollisionWorld::igPolygon(uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const
	{
		if (instance >= mIgInstances.size()) return false;
		const IgInstance& in = mIgInstances[instance];
		if (in.def < 0 || in.def >= (int)mIgDefs.size()) return false;
		if (!mIgDefs[in.def].polygon(surface, out)) return false;
		for (auto& v : out) v = igTransform(in.m, v);
		return true;
	}

	size_t CollisionWorld::igSurfaceCount(uint32_t instance) const
	{
		if (instance >= mIgInstances.size()) return 0;
		const int d = mIgInstances[instance].def;
		return (d >= 0 && d < (int)mIgDefs.size()) ? mIgDefs[d].surfaces.size() : 0;
	}

	size_t CollisionWorld::edgeCountTotal() const
	{
		size_t n = 0;
		for (const auto& l : mLines) n += l.size();
		return n;
	}

	// Classify a bsp's edges into normal / invisible (/ breakable), matching xlive sub_10172480 and the HCM port:
	// skip edges touching invalid or out-of-range surfaces; breakable edges go to their own list.
	void CollisionWorld::classify(const CollBsp& bsp, const float* m, Cat normal, Cat invisible, bool withBreakable,
	                              std::vector<Tri>* trisOut, std::vector<Tri>* invisTrisOut, int slot)
	{
		const size_t sN = bsp.surfaces.size(), vN = bsp.verts.size();
		auto flagsOf = [&](uint32_t s) -> int { return (s == CollBsp::kNone || s >= sN) ? -1 : bsp.surfaces[s].flags; };
		auto X = [&](const Vec3& v) { return m ? igTransform(m, v) : v; };
		for (const auto& e : bsp.edges)
		{
			if (e.v0 >= vN || e.v1 >= vN || e.v0 == e.v1) continue;
			const int fL = flagsOf(e.left), fR = flagsOf(e.right);
			const bool brL = fL >= 0 && (fL & kBreakable), brR = fR >= 0 && (fR & kBreakable);
			const bool skipL = e.left != CollBsp::kNone && (fL < 0 || (fL & kInvalid));
			const bool skipR = e.right != CollBsp::kNone && (fR < 0 || (fR & kInvalid));
			if (skipL || skipR) continue;
			if (brL || brR)
			{
				// breakable edges never enter the static lists; update() draws them while a side is still solid
				if (withBreakable) mBreakable.push_back({ { X(bsp.verts[e.v0]), X(bsp.verts[e.v1]) }, e.left, e.right, slot });
				continue;
			}
			const bool invis = (fL >= 0 && (fL & kInvisible)) || (fR >= 0 && (fR & kInvisible));
			mLines[(int)(invis ? invisible : normal)].push_back({ X(bsp.verts[e.v0]), X(bsp.verts[e.v1]) });
		}
		if (trisOut || invisTrisOut)
		{
			std::vector<Vec3> poly;
			for (uint32_t s = 0; s < sN; ++s)
			{
				const uint8_t f = bsp.surfaces[s].flags;
				if (f & (kInvalid | kBreakable)) continue;
				// invisible "*" surfaces are not visual occluders; they get their own translucent fill instead
				std::vector<Tri>* out = (f & kInvisible) ? invisTrisOut : trisOut;
				if (!out || !bsp.polygon(s, poly)) continue;
				for (size_t i = 1; i + 1 < poly.size(); ++i)
					out->push_back({ X(poly[0]), X(poly[i]), X(poly[i + 1]) });
			}
		}
	}

	void CollisionWorld::rebuildStatic(uintptr_t sbsp)
	{
		for (int c = 0; c <= (int)Cat::InstancedInvisible; ++c) mLines[c].clear();
		mBreakable.clear(); mStaticTris.clear(); mBarrierTris.clear(); mIgDefs.clear(); mIgInstances.clear(); mWorld = {}; mCreases.clear(); mCreaseFaces.clear(); mPlaneAgree = mPlaneDisagree = 0;
		h2::TagPools p;
		if (!h2::tagPools(p) || !sbsp) return;

		// world collision bsp
		uintptr_t first = 0; uint32_t count = 0;
		if (h2::resolveBlock(sbsp + kCollisionBspBlock, p, first, count) && count >= 1 && mWorld.parse(first, p))
			{ classify(mWorld, nullptr, Cat::Bsp, Cat::BspInvisible, true, &mStaticTris, &mBarrierTris); findCreases(mWorld, nullptr); }

		// instanced geometry
		uintptr_t iF = 0, dF = 0; uint32_t iN = 0, dN = 0;
		if (h2::resolveBlock(sbsp + kInstancesBlock, p, iF, iN) && h2::resolveBlock(sbsp + kDefinitionsBlock, p, dF, dN)
			&& iN && dN && iN < 20000 && dN < 20000)
		{
			mIgDefs.resize(dN);
			std::vector<bool> parsed(dN, false), ok(dN, false);
			for (uint32_t i = 0; i < iN; ++i)
			{
				IgInstance in{};
				in.def = -1;
				const uintptr_t inst = iF + (uintptr_t)i * kInstanceStride;
				if (!copy(in.m, inst, sizeof(in.m))) { mIgInstances.push_back(in); continue; }
				const uint16_t d = u16(inst + kInstDefIndexOff);
				if (d < dN)
				{
					if (!parsed[d])
					{
						parsed[d] = true;
						const uintptr_t def = dF + (uintptr_t)d * kDefinitionStride;
						const uintptr_t mid = h2::resolveTagOffset(i32(def + kDefCollisionOff, -1), p);
						const uintptr_t cb = okp(mid) ? q(mid + kMidCollbspPtrOff) : 0;
						ok[d] = okp(cb) && mIgDefs[d].parse(cb, p);
					}
					if (ok[d]) in.def = d;
				}
				mIgInstances.push_back(in);
				if (in.def >= 0)
					{ classify(mIgDefs[in.def], in.m, Cat::Instanced, Cat::InstancedInvisible, false, &mStaticTris, &mBarrierTris); findCreases(mIgDefs[in.def], in.m); }
			}
		}
		buildKillTriggers();
		LOGF("desyncable creases: %zu (faces %zu lines)  plane/winding agree %d disagree %d", mCreases.size(), mCreaseFaces.size(), mPlaneAgree, mPlaneDisagree);
		LOGF("collision rebuilt: sbsp 0x%llX  bsp %zu  invis %zu  breakable %zu  IG %zu (+%zu invis) from %zu instances  occluder tris %zu",
			(unsigned long long)sbsp, mLines[(int)Cat::Bsp].size(), mLines[(int)Cat::BspInvisible].size(), mBreakable.size(),
			mLines[(int)Cat::Instanced].size(), mLines[(int)Cat::InstancedInvisible].size(), mIgInstances.size(), mStaticTris.size());
	}

	// Scenario kill-trigger volumes (Cartographer CollisionLines.cpp port). MCC Halo 2 keeps the 32-bit tag layout:
	//   scenario = *(halo2.dll+0xE6F768) (HCM scenarioAddress 1.3528)
	//   +0x108 trigger_volumes   (stride 0x44: forward +0x0C, up +0x18, position (a CORNER) +0x24, extents (full) +0x30)
	//   +0x230 scenario_kill_triggers (stride 2: i16 trigger volume index)   [Cartographer scenario_definitions.h]
	void CollisionWorld::buildKillTriggers()
	{
		mLines[(int)Cat::KillTrigger].clear(); mKillTris.clear();
		h2::TagPools p;
		const uintptr_t scen = q(h2::base() + 0xE6F768);
		if (!h2::tagPools(p) || !okp(scen)) return;
		uintptr_t tvF = 0, ktF = 0; uint32_t tvN = 0, ktN = 0;
		if (!h2::resolveBlock(scen + 0x108, p, tvF, tvN) || !h2::resolveBlock(scen + 0x230, p, ktF, ktN)) return;
		if (tvN > 256 || ktN > 256) return;
		static const int edges[12][2] = { {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7} };
		static const int faces[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
		for (uint32_t k = 0; k < ktN; ++k)
		{
			const int16_t tv = (int16_t)u16(ktF + 2 * (uintptr_t)k);
			if (tv < 0 || (uint32_t)tv >= tvN) continue;
			float g[12];
			if (!copy(g, tvF + (uintptr_t)tv * 0x44 + 0x0C, sizeof(g))) continue;
			auto nrm = [](Vec3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return l > 1e-6f ? Vec3{ v.x / l, v.y / l, v.z / l } : v; };
			const Vec3 fwd = nrm({ g[0], g[1], g[2] }), up = nrm({ g[3], g[4], g[5] });
			const Vec3 left = nrm({ up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x });
			Vec3 c[8];
			for (int i = 0; i < 8; ++i)
			{
				const float fi = (i & 1) ? g[9] : 0.f, fj = (i & 2) ? g[10] : 0.f, fk = (i & 4) ? g[11] : 0.f;
				c[i] = { g[6] + fwd.x * fi + left.x * fj + up.x * fk, g[7] + fwd.y * fi + left.y * fj + up.y * fk, g[8] + fwd.z * fi + left.z * fj + up.z * fk };
			}
			for (const auto& e : edges) mLines[(int)Cat::KillTrigger].push_back({ c[e[0]], c[e[1]] });
			for (const auto& f : faces) { mKillTris.push_back({ c[f[0]], c[f[1]], c[f[2]] }); mKillTris.push_back({ c[f[0]], c[f[2]], c[f[3]] }); }
		}
	}

	// Parse the coll tag exactly the way the engine walks it (sub_180734FB0, which builds the Havok shape of every
	// permutation bsp at map load): regions -> permutations -> bsps. Cached per tag; geometry is node-local.
	const CollisionWorld::ObjColl* CollisionWorld::loadColl(uintptr_t coll, const h2::TagPools& p)
	{
		auto it = mCollCache.find(coll);
		if (it != mCollCache.end()) return &it->second;
		ObjColl oc;
		uintptr_t rF = 0; uint32_t rN = 0;
		if (!h2::resolveBlock(coll + kCollRegionsBlock, p, rF, rN) || rN > 64) return nullptr;   // not cached: retry
		oc.regions.resize(rN);
		for (uint32_t r = 0; r < rN; ++r)
		{
			uintptr_t pF = 0; uint32_t pN = 0;
			if (!h2::resolveBlock(rF + (uintptr_t)r * kCollRegionStride + kRegionPermsBlock, p, pF, pN) || pN > 256) continue;
			oc.regions[r].perms.resize(pN);
			for (uint32_t pi = 0; pi < pN; ++pi)
			{
				uintptr_t bF = 0; uint32_t bN = 0;
				if (!h2::resolveBlock(pF + (uintptr_t)pi * kCollPermStride + kPermBspsBlock, p, bF, bN) || bN > 64) continue;
				for (uint32_t k = 0; k < bN; ++k)
				{
					const uintptr_t el = bF + (uintptr_t)k * kCollModelBspStride;
					CollBsp cb;
					if (!cb.parse(el + 4, p)) continue;
					ObjBsp ob; ob.node = u16(el);
					for (const auto& e : cb.edges)
						if (e.v0 < cb.verts.size() && e.v1 < cb.verts.size() && e.v0 != e.v1)
							ob.lines.push_back({ cb.verts[e.v0], cb.verts[e.v1] });
					std::vector<Vec3> poly;
					for (uint32_t s = 0; s < cb.surfaces.size(); ++s)
					{
						if (cb.surfaces[s].flags & kInvisible) continue;
						if (!cb.polygon(s, poly)) continue;
						for (size_t i = 1; i + 1 < poly.size(); ++i) ob.tris.push_back({ poly[0], poly[i], poly[i + 1] });
					}
					if (!ob.lines.empty()) oc.regions[r].perms[pi].bsps.push_back(std::move(ob));
				}
			}
		}
		return &mCollCache.emplace(coll, std::move(oc)).first->second;
	}

	// hlmt "regions" block -> which collision region / permutation each model region / permutation selects.
	// Engine: sub_180793C40 (key generation) and sub_180793760 (key -> shape) read exactly these bytes.
	const CollisionWorld::ModelMap* CollisionWorld::loadModelMap(uintptr_t hlmt, const h2::TagPools& p)
	{
		auto it = mModelCache.find(hlmt);
		if (it != mModelCache.end()) return &it->second;
		ModelMap mm;
		uintptr_t rF = 0; uint32_t rN = 0;
		if (!h2::resolveBlock(hlmt + kModelRegionsBlock, p, rF, rN) || rN > 64) return nullptr;
		mm.regions.resize(rN);
		for (uint32_t r = 0; r < rN; ++r)
		{
			const uintptr_t re = rF + (uintptr_t)r * kModelRegionStride;
			mm.regions[r].collRegion = (int8_t)u8(re + 4, 0xFF);
			if (mm.regions[r].collRegion >= 0) mm.anyMapped = true;
			uintptr_t pF = 0; uint32_t pN = 0;
			if (!h2::resolveBlock(re + 8, p, pF, pN) || pN > 256) continue;
			mm.regions[r].collPerm.resize(pN);
			for (uint32_t k = 0; k < pN; ++k) mm.regions[r].collPerm[k] = (int8_t)u8(pF + (uintptr_t)k * kModelPermStride + 5, 0xFF);
		}
		return &mModelCache.emplace(hlmt, std::move(mm)).first->second;
	}

	void CollisionWorld::gatherObjects()
	{
		for (Cat c : { Cat::Scenery, Cat::Crate, Cat::Machine }) mLines[(int)c].clear();
		mObjectTris.clear();
		h2::TagPools p;
		if (!h2::tagPools(p)) return;
		std::vector<h2::ObjectRef> objs;
		h2::enumerateObjects(objs);
		for (const auto& o : objs)
		{
			Cat cat;
			if (o.type == h2::Scenery) cat = Cat::Scenery;
			else if (o.type == h2::Crate) cat = Cat::Crate;
			else if (o.type == h2::Machine) cat = Cat::Machine;
			else continue;

			// lifecycle, as the engine's own object walkers test it (sub_1808D70C0 / sub_1808D7130):
			// header +2 bit 0x10 (being deleted) or object flags bit 0 (hidden) => not a live object
			if ((o.hdrFlags & kHdrBeingDeleted) || h2::objectHidden(o.obj)) continue;

			const uint16_t defIdx = u16(o.obj + 0x00);
			if (defIdx == 0xFFFF) continue;
			const uintptr_t defTag = h2::tagData(defIdx, p);
			if (!okp(defTag)) continue;
			const uint16_t hlmt = u16(defTag + kDefModelRefOff);
			const uintptr_t model = hlmt == 0xFFFF ? 0 : h2::tagData(hlmt, p);
			if (!okp(model)) continue;
			const uint16_t collIdx = u16(model + kModelCollRefOff);
			const uintptr_t coll = collIdx == 0xFFFF ? 0 : h2::tagData(collIdx, p);
			if (!okp(coll)) continue;

			const ObjColl* oc = loadColl(coll, p);
			if (!oc || oc->regions.empty()) continue;
			const ModelMap* mm = loadModelMap(model, p);

			// Which (collision region, permutation) pairs are live NOW. The engine accepts a collision bsp of key
			// (model region r, model permutation q) only while the object's runtime region state byte[r] == q
			// (sub_180794590 key types 3/4: `*(char*)((key & 0x1F) + state) != key >> 5 => rejected`).
			// A destroyed destructible switches those bytes, so its old geometry stops being drawn.
			int8_t state[64];
			const int ns = h2::objectRegionState(o.obj, state, 64);
			struct Sel { uint32_t r, p; };
			Sel sel[64]; int nsel = 0;
			auto add = [&](int cr, int cp)
			{
				if (cr < 0 || cr >= (int)oc->regions.size()) return;
				if (cp < 0 || cp >= (int)oc->regions[cr].perms.size()) return;
				for (int i = 0; i < nsel; ++i) if (sel[i].r == (uint32_t)cr && sel[i].p == (uint32_t)cp) return;   // several model regions can share one
				if (nsel < 64) sel[nsel++] = { (uint32_t)cr, (uint32_t)cp };
			};
			if (mm && mm->anyMapped)
			{
				for (size_t r = 0; r < mm->regions.size(); ++r)
				{
					const ModelRegion& mr = mm->regions[r];
					if (mr.collRegion < 0) continue;
					const int mp = (int)r < ns ? (int)state[r] : 0;    // no state block: placement default permutation 0
					if (mp < 0 || mp >= (int)mr.collPerm.size()) continue;
					add(mr.collRegion, mr.collPerm[mp]);
				}
			}
			else
			{
				// the model carries no collision-region mapping at all (the engine would never reach these bsps through
				// the hlmt): still show the coll geometry, one permutation per region (xlive's behaviour)
				for (size_t cr = 0; cr < oc->regions.size(); ++cr)
				{
					const int s = (int)cr < ns ? (int)state[cr] : 0;
					if (s < 0) continue;
					add((int)cr, s < (int)oc->regions[cr].perms.size() ? s : 0);
				}
			}
			if (!nsel) continue;

			// root transform (fallback only): pos +0x64, forward +0x70, up +0x7C
			float loc[9] = {};
			const bool haveLoc = copy(loc, o.obj + 0x64, sizeof(loc));
			const Vec3 pos{ loc[0], loc[1], loc[2] }, fwd{ loc[3], loc[4], loc[5] }, up{ loc[6], loc[7], loc[8] };
			const float lf = fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z, lu = up.x * up.x + up.y * up.y + up.z * up.z;
			const bool rootOk = haveLoc && lf >= 0.8f && lf <= 1.25f && lu >= 0.8f && lu <= 1.25f && std::isfinite(pos.x) && std::fabs(pos.x) <= 1e5f;
			const Vec3 left{ up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x };
			const float m[13] = { 1.f, fwd.x, fwd.y, fwd.z, left.x, left.y, left.z, up.x, up.y, up.z, pos.x, pos.y, pos.z };
			auto& out = mLines[(int)cat];
			for (int si = 0; si < nsel; ++si)
				for (const auto& b : oc->regions[sel[si].r].perms[sel[si].p].bsps)
				{
					// each collision bsp is bound to a node (collision_model_bsp.node_index): use that node's world matrix,
					// the root transform only when the node is missing, out of range, or invalid (xlive does the same, node-first)
					float nm[13];
					const float* bm = m;
					if (b.node != 0xFFFF && h2::objectNodeMatrix(o.obj, b.node, nm) && sanePlacement(nm)) bm = nm;
					else if (!rootOk) continue;
					for (const auto& l : b.lines) out.push_back({ igTransform(bm, l.a), igTransform(bm, l.b) });
					for (const auto& t : b.tris) mObjectTris.push_back({ igTransform(bm, t.a), igTransform(bm, t.b), igTransform(bm, t.c) });
				}
		}
	}

	void CollisionWorld::update(bool wantObjects, bool wantBreakableLive)
	{
		if (isH3()) { updateH3(wantObjects, wantBreakableLive); return; }
		++mFrame;
		const uintptr_t sbsp = h2::structureBsp();
		if (sbsp != mSbsp)
		{
			mSbsp = sbsp;
			mCollCache.clear(); mModelCache.clear();
			if (okp(sbsp)) rebuildStatic(sbsp);
			else { mSbsp = 0; for (auto& l : mLines) l.clear(); mStaticTris.clear(); mObjectTris.clear(); return; }
		}
		if (wantBreakableLive && (mFrame % 15) == 0)
		{
			// Destruction is not recorded in the surface records (tag data, flags never change); the engine keeps it in
			// the runtime breakable bit row, which collision itself tests (sub_180794590). Read that row once per refresh.
			uint32_t row[8] = {};
			const bool haveRow = h2::readWorldBreakableRow(row);
			const size_t sN = mWorld.surfaces.size();
			auto intact = [&](uint8_t idx) { return !haveRow || ((row[idx >> 5] >> (idx & 31)) & 1u) != 0; };
			// a side is solid if it exists, is not invalid, and (when breakable) its breakable bit is still set
			auto solid = [&](uint32_t s) -> bool {
				if (s == CollBsp::kNone || s >= sN) return false;
				const CollBsp::Surface& sf = mWorld.surfaces[s];
				if (sf.flags & kInvalid) return false;
				return !(sf.flags & kBreakable) || intact((uint8_t)sf.breakable);
			};
			auto& out = mLines[(int)Cat::Breakable];
			out.clear();
			for (const auto& be : mBreakable)
				if (solid(be.left) || solid(be.right)) out.push_back(be.l);
		}
		if (wantObjects) gatherObjects();
		else { for (Cat c : { Cat::Scenery, Cat::Crate, Cat::Machine }) mLines[(int)c].clear(); mObjectTris.clear(); }
	}
}
