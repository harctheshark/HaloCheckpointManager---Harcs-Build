#include "pch.h"
#include "H2CV_Collision.h"
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
		// object -> def tag (+0x00 u16 index) -> hlmt (+0x38) -> coll (+0x0C) -> collision_model_bsps (+0x48, stride 0x44)
		constexpr uintptr_t kDefModelRefOff = 0x38, kModelCollRefOff = 0x0C, kCollBspBlockOff = 0x48;
		constexpr size_t kCollModelBspStride = 0x44;
		constexpr uint8_t kInvisible = 0x02, kBreakable = 0x08, kInvalid = 0x10;
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
		surfaces.clear(); edges.clear(); verts.clear(); liveSurfaces = 0;
		uintptr_t sF = 0, eF = 0, vF = 0; uint32_t sN = 0, eN = 0, vN = 0;
		if (!h2::resolveBlock(collbsp + kSurfacesBlock, p, sF, sN)) return false;
		if (!h2::resolveBlock(collbsp + kEdgesBlock, p, eF, eN)) return false;
		if (!h2::resolveBlock(collbsp + kVerticesBlock, p, vF, vN)) return false;
		if (sN > 200000 || eN > 400000 || vN > 200000 || !eN || !vN) return false;
		surfaces.resize(sN); edges.resize(eN);
		std::vector<float> vraw((size_t)vN * 4);
		if (sN && !copy(surfaces.data(), sF, (size_t)sN * 8)) return false;
		if (!copy(edges.data(), eF, (size_t)eN * 12)) return false;
		if (!copy(vraw.data(), vF, vraw.size() * 4)) return false;
		verts.resize(vN);
		for (uint32_t i = 0; i < vN; ++i) verts[i] = { vraw[i * 4 + 0], vraw[i * 4 + 1], vraw[i * 4 + 2] };
		liveSurfaces = sF;
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

	bool CollisionWorld::igPolygon(uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const
	{
		if (instance >= mIgInstances.size()) return false;
		const IgInstance& in = mIgInstances[instance];
		if (in.def < 0 || in.def >= (int)mIgDefs.size()) return false;
		if (!mIgDefs[in.def].polygon(surface, out)) return false;
		for (auto& v : out) v = igTransform(in.m, v);
		return true;
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
	                              std::vector<Tri>* trisOut, std::vector<Tri>* invisTrisOut)
	{
		const size_t sN = bsp.surfaces.size(), vN = bsp.verts.size();
		auto flagsOf = [&](uint16_t s) -> int { return (s == 0xFFFF || s >= sN) ? -1 : bsp.surfaces[s].flags; };
		auto X = [&](const Vec3& v) { return m ? igTransform(m, v) : v; };
		for (const auto& e : bsp.edges)
		{
			if (e.v0 >= vN || e.v1 >= vN || e.v0 == e.v1) continue;
			const int fL = flagsOf(e.left), fR = flagsOf(e.right);
			const bool brL = fL >= 0 && (fL & kBreakable), brR = fR >= 0 && (fR & kBreakable);
			const bool skipL = e.left != 0xFFFF && (fL < 0 || (fL & (kBreakable | kInvalid)));
			const bool skipR = e.right != 0xFFFF && (fR < 0 || (fR & (kBreakable | kInvalid)));
			if (withBreakable && (brL || brR))
			{
				mBreakable.push_back({ { X(bsp.verts[e.v0]), X(bsp.verts[e.v1]) }, brL ? e.left : e.right });
				continue;
			}
			if (skipL || skipR) continue;
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
		mBreakable.clear(); mStaticTris.clear(); mBarrierTris.clear(); mIgDefs.clear(); mIgInstances.clear(); mWorld = {};
		h2::TagPools p;
		if (!h2::tagPools(p) || !sbsp) return;

		// world collision bsp
		uintptr_t first = 0; uint32_t count = 0;
		if (h2::resolveBlock(sbsp + kCollisionBspBlock, p, first, count) && count >= 1 && mWorld.parse(first, p))
			classify(mWorld, nullptr, Cat::Bsp, Cat::BspInvisible, true, &mStaticTris, &mBarrierTris);

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
					classify(mIgDefs[in.def], in.m, Cat::Instanced, Cat::InstancedInvisible, false, &mStaticTris, &mBarrierTris);
			}
		}
		buildKillTriggers();
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

			auto it = mCollCache.find(coll);
			if (it == mCollCache.end())
			{
				std::vector<ObjBsp> bsps;
				uintptr_t bF = 0; uint32_t bN = 0;
				if (h2::resolveBlock(coll + kCollBspBlockOff, p, bF, bN) && bN > 0 && bN <= 256)
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
						if (!ob.lines.empty()) bsps.push_back(std::move(ob));
					}
				it = mCollCache.emplace(coll, std::move(bsps)).first;
			}
			if (it->second.empty()) continue;

			// object world transform from its location sub-struct: pos +0x64, forward +0x70, up +0x7C
			float loc[9];
			if (!copy(loc, o.obj + 0x64, sizeof(loc))) continue;
			const Vec3 pos{ loc[0], loc[1], loc[2] }, fwd{ loc[3], loc[4], loc[5] }, up{ loc[6], loc[7], loc[8] };
			const float lf = fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z, lu = up.x * up.x + up.y * up.y + up.z * up.z;
			if (lf < 0.8f || lf > 1.25f || lu < 0.8f || lu > 1.25f || !std::isfinite(pos.x) || std::fabs(pos.x) > 1e5f) continue;
			const Vec3 left{ up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x };
			const float m[13] = { 1.f, fwd.x, fwd.y, fwd.z, left.x, left.y, left.z, up.x, up.y, up.z, pos.x, pos.y, pos.z };
			auto& out = mLines[(int)cat];
			for (const auto& b : it->second)
			{
				for (const auto& l : b.lines) out.push_back({ igTransform(m, l.a), igTransform(m, l.b) });
				for (const auto& t : b.tris) mObjectTris.push_back({ igTransform(m, t.a), igTransform(m, t.b), igTransform(m, t.c) });
			}
		}
	}

	void CollisionWorld::update(bool wantObjects, bool wantBreakableLive)
	{
		++mFrame;
		const uintptr_t sbsp = h2::structureBsp();
		if (sbsp != mSbsp)
		{
			mSbsp = sbsp;
			mCollCache.clear();
			if (okp(sbsp)) rebuildStatic(sbsp);
			else { mSbsp = 0; for (auto& l : mLines) l.clear(); mStaticTris.clear(); mObjectTris.clear(); return; }
		}
		if (wantBreakableLive && (mFrame % 15) == 0)
		{
			auto& out = mLines[(int)Cat::Breakable];
			out.clear();
			const size_t sN = mWorld.surfaces.size();
			if (sN && okp(mWorld.liveSurfaces))
			{
				std::vector<CollBsp::Surface> live(sN);
				if (copy(live.data(), mWorld.liveSurfaces, sN * 8))
					for (const auto& be : mBreakable)
						if (be.surface < sN && (live[be.surface].flags & kBreakable) && !(live[be.surface].flags & kInvalid))
							out.push_back(be.l);
			}
		}
		if (wantObjects) gatherObjects();
		else { for (Cat c : { Cat::Scenery, Cat::Crate, Cat::Machine }) mLines[(int)c].clear(); mObjectTris.clear(); }
	}
}
