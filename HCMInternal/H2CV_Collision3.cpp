#include "pch.h"
// Halo 3 / Halo 3: ODST side of the collision cache (see Collision.h). Reads the winged-edge collision_bsp of every ACTIVE structure bsp
// (16-bit or 32-bit variant), its instanced-geometry definitions, the breakable-surface rows, the scenario kill volumes, and the Havok
// shapes of scenery / crates / machines. Static RE: REPORT_H3_ODST.md + NOTES.md in the research / work folders.
#include "H2CV_Collision.h"
#include "H2CV_Engine.h"
#include "H2CV_Halo3.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <algorithm>
#include <array>
#include <cmath>

using namespace mem;

namespace h2cv
{
	namespace
	{
		constexpr uint8_t kInvisible = 0x02, kBreakable = 0x08, kInvalid = 0x10;

		bool sanePlacement(const float* m)
		{
			for (int i = 0; i < 13; ++i) if (!std::isfinite(m[i])) return false;
			if (!(m[0] > 1e-4f && m[0] < 1e4f)) return false;
			for (int a = 1; a <= 7; a += 3)
			{
				const float l2 = m[a] * m[a] + m[a + 1] * m[a + 1] + m[a + 2] * m[a + 2];
				if (l2 < 0.8f || l2 > 1.25f) return false;
			}
			return std::fabs(m[10]) < 1e5f && std::fabs(m[11]) < 1e5f && std::fabs(m[12]) < 1e5f;
		}

		uint64_t mix(uint64_t h, uint64_t v) { return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)); }
	}

	bool CollBsp::parse3(uintptr_t b, bool large)
	{
		surfaces.clear(); edges.clear(); verts.clear(); planes.clear();
		uintptr_t pF = 0, sF = 0, eF = 0, vF = 0; uint32_t pN = 0, sN = 0, eN = 0, vN = 0;
		if (!h3::block(b + 0x0C, pF, pN) || !h3::block(b + 0x3C, sF, sN) || !h3::block(b + 0x48, eF, eN) || !h3::block(b + 0x54, vF, vN)) return false;
		if (!pN || !sN || !eN || !vN || pN > 3000000 || sN > 3000000 || eN > 6000000 || vN > 3000000) return false;
		const size_t sStride = large ? 16 : 12, eStride = large ? 24 : 12, vStride = large ? 20 : 16;
		std::vector<uint8_t> rs((size_t)sN * sStride), re((size_t)eN * eStride), rv((size_t)vN * vStride);
		planes.resize((size_t)pN * 4);
		if (!copy(planes.data(), pF, (size_t)pN * 16) || !copy(rs.data(), sF, rs.size()) || !copy(re.data(), eF, re.size()) || !copy(rv.data(), vF, rv.size()))
		{
			planes.clear();
			return false;
		}
		surfaces.resize(sN); edges.resize(eN); verts.resize(vN);
		for (uint32_t i = 0; i < sN; ++i)
		{
			const uint8_t* r = &rs[(size_t)i * sStride];
			Surface& s = surfaces[i];
			if (!large)
			{
				const uint16_t pl = *(const uint16_t*)(r + 0);
				s.plane = pl & 0x7FFF; s.flip = (pl & 0x8000) != 0;
				s.firstEdge = *(const uint16_t*)(r + 2); s.material = *(const uint16_t*)(r + 4);
				s.breakBit = *(const uint16_t*)(r + 6); s.breakGroup = *(const uint16_t*)(r + 8); s.flags = r[10];
			}
			else
			{
				const uint32_t pl = *(const uint32_t*)(r + 0);
				s.plane = pl & 0x7FFFFFFFu; s.flip = (pl & 0x80000000u) != 0;
				s.firstEdge = *(const uint32_t*)(r + 4); s.material = *(const uint16_t*)(r + 8);
				s.breakBit = *(const uint16_t*)(r + 10); s.breakGroup = *(const uint16_t*)(r + 12); s.flags = r[14];
			}
			s.breakable = (int8_t)(uint8_t)s.breakBit;
		}
		for (uint32_t i = 0; i < eN; ++i)
		{
			const uint8_t* r = &re[(size_t)i * eStride];
			Edge& e = edges[i];
			if (!large)
			{
				const uint16_t* u = (const uint16_t*)r;
				auto ref = [](uint16_t v) -> uint32_t { return v == 0xFFFF ? kNone : v; };
				e = { u[0], u[1], ref(u[2]), ref(u[3]), ref(u[4]), ref(u[5]) };
			}
			else
			{
				const uint32_t* u = (const uint32_t*)r;
				e = { u[0], u[1], u[2], u[3], u[4], u[5] };
			}
		}
		for (uint32_t i = 0; i < vN; ++i)
		{
			const float* f = (const float*)&rv[(size_t)i * vStride];
			verts[i] = { f[0], f[1], f[2] };
		}
		return true;
	}

	void LineGrid::build(const std::vector<Line>& lines, float cell)
	{
		mCell = cell; mCells.clear(); mItems.clear(); mLong.clear(); mStamp.clear(); mTick = 0; mBuilt = true;
		std::vector<std::pair<uint64_t, uint32_t>> pairs;
		pairs.reserve(lines.size() * 2);
		auto cellOf = [&](float v) { return (int)std::floor(v / cell); };
		auto pack = [](int x, int y, int z) -> uint64_t
		{
			return ((uint64_t)(uint32_t)(x + (1 << 20)) << 42) | ((uint64_t)(uint32_t)(y + (1 << 20)) << 21) | (uint64_t)(uint32_t)(z + (1 << 20));
		};
		for (uint32_t i = 0; i < (uint32_t)lines.size(); ++i)
		{
			const Line& l = lines[i];
			if (!std::isfinite(l.a.x + l.a.y + l.a.z + l.b.x + l.b.y + l.b.z)) continue;
			const float mnx = std::min(l.a.x, l.b.x), mxx = std::max(l.a.x, l.b.x), mny = std::min(l.a.y, l.b.y), mxy = std::max(l.a.y, l.b.y);
			const float mnz = std::min(l.a.z, l.b.z), mxz = std::max(l.a.z, l.b.z);
			if (std::fabs(mnx) > 3e4f || std::fabs(mxx) > 3e4f || std::fabs(mny) > 3e4f || std::fabs(mxy) > 3e4f || std::fabs(mnz) > 3e4f || std::fabs(mxz) > 3e4f) { mLong.push_back(i); continue; }
			const int x0 = cellOf(mnx), x1 = cellOf(mxx), y0 = cellOf(mny), y1 = cellOf(mxy), z0 = cellOf(mnz), z1 = cellOf(mxz);
			const int64_t n = (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
			if (n > 64) { mLong.push_back(i); continue; }
			for (int x = x0; x <= x1; ++x) for (int y = y0; y <= y1; ++y) for (int z = z0; z <= z1; ++z) pairs.push_back({ pack(x, y, z), i });
		}
		std::sort(pairs.begin(), pairs.end());
		mItems.reserve(pairs.size());
		for (size_t k = 0; k < pairs.size();)
		{
			size_t e = k;
			while (e < pairs.size() && pairs[e].first == pairs[k].first) ++e;
			const uint64_t key = pairs[k].first;
			Cell c;
			c.x = (int)((key >> 42) & 0x1FFFFF) - (1 << 20); c.y = (int)((key >> 21) & 0x1FFFFF) - (1 << 20); c.z = (int)(key & 0x1FFFFF) - (1 << 20);
			c.first = (uint32_t)mItems.size(); c.count = (uint32_t)(e - k);
			for (size_t j = k; j < e; ++j) mItems.push_back(pairs[j].second);
			mCells.push_back(c);
			k = e;
		}
	}

	const CollisionWorld::Bsp3* CollisionWorld::bsp3(int index) const
	{
		for (const auto& b : mB3) if (b.index == index) return &b;
		return nullptr;
	}

	bool CollisionWorld::worldPolygon3(int bsp, uint32_t surface, std::vector<Vec3>& out) const
	{
		const Bsp3* b = bsp3(bsp);
		return b && b->world.polygon(surface, out);
	}

	bool CollisionWorld::igPolygon3(int bsp, uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const
	{
		const Bsp3* b = bsp3(bsp);
		if (!b || instance >= b->igInst.size()) return false;
		const IgInstance& in = b->igInst[instance];
		if (in.def < 0 || (size_t)in.def >= b->igDefs.size() || !b->igOk[in.def]) return false;
		if (!b->igDefs[in.def].polygon(surface, out)) return false;
		for (auto& v : out) v = igTransform(in.m, v);
		return true;
	}

	// Scenario kill volumes: the trigger-volume boxes of the scenario's kill-volume list (position is a corner, extents are full sizes).
	void CollisionWorld::buildKillTriggers3()
	{
		mLines[(int)Cat::KillTrigger].clear(); mKillTris.clear();
		std::vector<h3::VolumeBox> boxes;
		if (!h3::killVolumes(boxes)) return;
		static const int edges[12][2] = { {0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7} };
		static const int faces[6][4] = { {0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
		auto nrm = [](Vec3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return l > 1e-6f ? Vec3{ v.x / l, v.y / l, v.z / l } : v; };
		for (const auto& b : boxes)
		{
			const Vec3 fwd = nrm(b.fwd), up = nrm(b.up);
			const Vec3 left = nrm({ up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x });
			Vec3 c[8];
			for (int i = 0; i < 8; ++i)
			{
				const float fi = (i & 1) ? b.ext.x : 0.f, fj = (i & 2) ? b.ext.y : 0.f, fk = (i & 4) ? b.ext.z : 0.f;
				c[i] = { b.pos.x + fwd.x * fi + left.x * fj + up.x * fk, b.pos.y + fwd.y * fi + left.y * fj + up.y * fk, b.pos.z + fwd.z * fi + left.z * fj + up.z * fk };
			}
			for (const auto& e : edges) mLines[(int)Cat::KillTrigger].push_back({ c[e[0]], c[e[1]] });
			for (const auto& f : faces) { mKillTris.push_back({ c[f[0]], c[f[1]], c[f[2]] }); mKillTris.push_back({ c[f[0]], c[f[2]], c[f[3]] }); }
		}
	}

	void CollisionWorld::buildSoftCeilings3(const std::vector<h3::BspRes>& act)
	{
		auto& out = mLines[(int)Cat::SoftCeiling];
		out.clear();
		size_t tris = 0;
		for (const auto& r : act)
		{
			std::vector<h3::SoftTri> st;
			if (!h3::softCeilings(r.index, st)) continue;
			tris += st.size();
			for (const auto& t : st) { out.push_back({ t.v[0], t.v[1] }); out.push_back({ t.v[1], t.v[2] }); out.push_back({ t.v[2], t.v[0] }); }
		}
		if (tris) LOGF("collision: soft ceilings %zu triangles (%zu lines)", tris, out.size());
	}

	void CollisionWorld::rebuildStatic3(const std::vector<h3::BspRes>& act)
	{
		for (int c = 0; c <= (int)Cat::InstancedInvisible; ++c) mLines[c].clear();
		for (auto& g : mGrid) g = LineGrid{};
		mBreakable.clear(); mStaticTris.clear(); mBarrierTris.clear(); mCreases.clear(); mCreaseFaces.clear();
		mPlaneAgree = mPlaneDisagree = 0; mB3.clear(); mB3.reserve(act.size()); mIgDefs.clear(); mIgInstances.clear(); mWorld = {};
		mCreasesBuilt = false;
		size_t igInstances = 0, igLive = 0;
		// bsp seams (2026-11-02): a seam face (its collision material has a seam index) is left out where another loaded bsp has a
		// seam face lying on it - the two bsps' closing faces of a shared border (h3sb: "seam surfaces (borders with other loaded
		// BSPs) left out", 33 + 33 on h100 bsp 3 + 8). The seam identifiers' 16-byte ids did not tell the bsps apart (every seam
		// face matched), so the faces are matched by place: the centroid of one on the other's polygon, planes parallel.
		std::vector<CollBsp> worlds(act.size());
		std::vector<uint8_t> parsed(act.size(), 0);
		for (size_t i = 0; i < act.size(); ++i) parsed[i] = worlds[i].parse3(act[i].worldBsp, act[i].large) ? 1 : 0;
		struct SeamFace { uint32_t s; Vec3 c, n, lo, hi; std::vector<Vec3> poly; };
		std::vector<std::vector<SeamFace>> sf(act.size());
		for (size_t i = 0; i < act.size(); ++i)
		{
			if (!parsed[i]) continue;
			h3::SeamInfo si;
			if (!h3::seamInfo(act[i].sbsp, si)) continue;
			const CollBsp& w = worlds[i];
			for (uint32_t s = 0; s < (uint32_t)w.surfaces.size(); ++s)
			{
				const uint16_t m = w.surfaces[s].material;
				if (m >= si.matSeam.size() || si.matSeam[m] < 0) continue;
				SeamFace f; f.s = s;
				if (!w.polygon(s, f.poly) || f.poly.size() < 3) continue;
				f.c = { 0, 0, 0 }; f.lo = f.hi = f.poly[0];
				for (const Vec3& v : f.poly)
				{
					f.c = { f.c.x + v.x, f.c.y + v.y, f.c.z + v.z };
					f.lo = { std::min(f.lo.x, v.x), std::min(f.lo.y, v.y), std::min(f.lo.z, v.z) };
					f.hi = { std::max(f.hi.x, v.x), std::max(f.hi.y, v.y), std::max(f.hi.z, v.z) };
				}
				const float k = 1.f / (float)f.poly.size(); f.c = { f.c.x * k, f.c.y * k, f.c.z * k };
				const Vec3 a = f.poly[0], b = f.poly[1], c = f.poly[2];
				const Vec3 u{ b.x - a.x, b.y - a.y, b.z - a.z }, v{ c.x - a.x, c.y - a.y, c.z - a.z };
				Vec3 n{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
				const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
				if (nl < 1e-8f) continue;
				f.n = { n.x / nl, n.y / nl, n.z / nl };
				sf[i].push_back(std::move(f));
			}
		}
		// p on face f (within 0.01 of its plane, inside its edges - either winding)
		auto onFace = [](const SeamFace& f, const Vec3& p) {
			const float e = 0.01f;
			if (p.x < f.lo.x - e || p.x > f.hi.x + e || p.y < f.lo.y - e || p.y > f.hi.y + e || p.z < f.lo.z - e || p.z > f.hi.z + e) return false;
			const Vec3 a = f.poly[0];
			if (std::fabs((p.x - a.x) * f.n.x + (p.y - a.y) * f.n.y + (p.z - a.z) * f.n.z) > e) return false;
			int pos = 0, neg = 0;
			const size_t k = f.poly.size();
			for (size_t i = 0; i < k; ++i)
			{
				const Vec3 q0 = f.poly[i], q1 = f.poly[(i + 1) % k];
				const Vec3 ed{ q1.x - q0.x, q1.y - q0.y, q1.z - q0.z }, w{ p.x - q0.x, p.y - q0.y, p.z - q0.z };
				const Vec3 cr{ ed.y * w.z - ed.z * w.y, ed.z * w.x - ed.x * w.z, ed.x * w.y - ed.y * w.x };
				const float sd = cr.x * f.n.x + cr.y * f.n.y + cr.z * f.n.z;
				if (sd > 1e-4f) ++pos; else if (sd < -1e-4f) ++neg;
			}
			return pos == 0 || neg == 0;
		};
		for (size_t i = 0; i < act.size(); ++i)
		{
			size_t out = 0;
			for (const SeamFace& f : sf[i])
			{
				bool joined = false;
				for (size_t o = 0; o < act.size() && !joined; ++o)
				{
					if (o == i) continue;
					for (const SeamFace& g : sf[o])
						if (std::fabs(f.n.x * g.n.x + f.n.y * g.n.y + f.n.z * g.n.z) > 0.999f && (onFace(g, f.c) || onFace(f, g.c))) { joined = true; break; }
				}
				if (joined) { worlds[i].surfaces[f.s].flags |= kInvalid; ++out; }
			}
			if (!sf[i].empty()) LOGF("collision: bsp %d  %zu seam surfaces (borders with other loaded bsps) left out, %zu kept (the bsp beyond is not loaded)", act[i].index, out, sf[i].size() - out);
		}
		size_t ai = (size_t)-1;
		for (const h3::BspRes& r : act)
		{
			++ai;
			Bsp3 b; b.index = r.index; b.sbsp = r.sbsp; b.how = r.how;
			if (!parsed[ai]) { LOGF("collision: bsp %d world collision_bsp @0x%llX (%s) failed to parse", r.index, (unsigned long long)r.worldBsp, r.how.c_str()); continue; }
			b.world = std::move(worlds[ai]);
			if (r.igInstances && r.igInstanceCount && r.igDefs)
			{
				const uint32_t iN = r.igInstanceCount;
				b.igInst.assign(iN, IgInstance{});
				uint32_t maxDef = 0;
				for (uint32_t i = 0; i < iN; ++i)
				{
					IgInstance& in = b.igInst[i];
					in.def = -1;
					const uintptr_t inst = r.igInstances + (uintptr_t)i * 0x78;
					if (!copy(in.m, inst, sizeof(in.m)) || !sanePlacement(in.m)) continue;
					const uint16_t d = u16(inst + 52, 0xFFFF);
					if (d >= 8192) continue;
					in.def = d; maxDef = std::max<uint32_t>(maxDef, d);
				}
				b.igDefs.resize((size_t)maxDef + 1); b.igOk.assign((size_t)maxDef + 1, 0);
				std::vector<uint8_t> tried(b.igDefs.size(), 0);
				for (auto& in : b.igInst)
				{
					if (in.def < 0) continue;
					if (!tried[in.def])
					{
						tried[in.def] = 1;
						const uintptr_t bs = h3::igDefBsp(r, (uint32_t)in.def);
						b.igOk[in.def] = bs && b.igDefs[in.def].parse3(bs, false) ? 1 : 0;   // IG collision is always the 16-bit variant, inline in the definition
					}
					if (!b.igOk[in.def]) in.def = -1;
				}
			}
			mB3.push_back(std::move(b));
			const int slot = (int)mB3.size() - 1;
			Bsp3& nb = mB3.back();
			classify(nb.world, nullptr, Cat::Bsp, Cat::BspInvisible, true, &mStaticTris, &mBarrierTris, slot);
			size_t lines = 0;
			for (const auto& in : nb.igInst)
			{
				++igInstances;
				if (in.def < 0) continue;
				++igLive;
				classify(nb.igDefs[in.def], in.m, Cat::Instanced, Cat::InstancedInvisible, false, &mStaticTris, &mBarrierTris, slot);
				++lines;
			}
			LOGF("collision: bsp %d  %s  surfaces %zu edges %zu verts %zu  IG instances %zu (%zu with collision)", nb.index, nb.how.c_str(),
				nb.world.surfaces.size(), nb.world.edges.size(), nb.world.verts.size(), nb.igInst.size(), lines);
		}
		if (mWantCreases)
		{
			for (const auto& nb : mB3)
			{
				findCreases(nb.world, nullptr);
				for (const auto& in : nb.igInst) if (in.def >= 0) findCreases(nb.igDefs[in.def], in.m);
			}
			mCreasesBuilt = true;
		}
		buildKillTriggers3();
		buildSoftCeilings3(act);
		for (Cat c : { Cat::Bsp, Cat::BspInvisible, Cat::Instanced, Cat::InstancedInvisible, Cat::KillTrigger, Cat::SoftCeiling })
			if (mLines[(int)c].size() > 2000) mGrid[(int)c].build(mLines[(int)c], 32.f);
		LOGF("collision rebuilt (H3): %zu bsp(s)  bsp lines %zu  invis %zu  breakable edges %zu  IG %zu (+%zu invis) from %zu instances (%zu live)  occluder tris %zu  kill volumes %zu lines",
			mB3.size(), mLines[(int)Cat::Bsp].size(), mLines[(int)Cat::BspInvisible].size(), mBreakable.size(), mLines[(int)Cat::Instanced].size(),
			mLines[(int)Cat::InstancedInvisible].size(), igInstances, igLive, mStaticTris.size(), mLines[(int)Cat::KillTrigger].size());
	}

	void CollisionWorld::updateH3(bool wantObjects, bool wantBreakableLive)
	{
		++mFrame;
		const uint32_t mask = h3::activeBspMask() & 0xFFFFu;
		std::vector<h3::BspRes> act;
		std::string why;
		for (int i = 0; i < 16; ++i)
		{
			if (!((mask >> i) & 1u)) continue;
			h3::BspRes r;
			if (h3::resolveBsp(i, r, &why)) act.push_back(std::move(r));
		}
		if (act.empty())
		{
			if (why.empty()) why = mask ? "no bsp resolved" : "no structure bsp active (menu / loading)";
			if (why != mWhy3) { mWhy3 = why; LOGF("collision: nothing to draw (active mask 0x%X): %s", mask, why.c_str()); }
			if (mSbsp) { for (auto& l : mLines) l.clear(); for (auto& g : mGrid) g = LineGrid{}; mStaticTris.clear(); mObjectTris.clear(); mObjSpans.clear(); mBarrierTris.clear(); mKillTris.clear(); mB3.clear(); }
			mSbsp = 0; mSig3 = 0;
			return;
		}
		mWhy3.clear();
		uint64_t sig = 0xC0111510Dull;
		for (const auto& r : act) { sig = mix(sig, (uint64_t)r.index); sig = mix(sig, r.sbsp); sig = mix(sig, r.worldBsp); sig = mix(sig, r.igDefs); sig = mix(sig, r.igInstances); }
		if (sig != mSig3 || mSbsp == 0)
		{
			mSig3 = sig;
			mCollCache.clear(); mModelCache.clear();
			rebuildStatic3(act);
			mSbsp = (uintptr_t)(sig | 1ull);
		}
		if (mWantCreases && !mCreasesBuilt)
		{
			mCreases.clear(); mCreaseFaces.clear(); mPlaneAgree = mPlaneDisagree = 0;
			for (const auto& nb : mB3)
			{
				findCreases(nb.world, nullptr);
				for (const auto& in : nb.igInst) if (in.def >= 0) findCreases(nb.igDefs[in.def], in.m);
			}
			mCreasesBuilt = true;
		}
		if (wantBreakableLive && (mFrame % 15) == 0)
		{
			// bit set in the (bsp, group) row = surface still intact; no row = nothing broken / pool unreadable => intact
			std::unordered_map<uint32_t, std::array<uint32_t, 12>> rows;
			std::unordered_map<uint32_t, bool> have;
			auto row = [&](int slot, uint16_t group) -> const std::array<uint32_t, 12>* {
				const uint32_t key = ((uint32_t)slot << 16) | group;
				auto it = have.find(key);
				if (it == have.end())
				{
					std::array<uint32_t, 12> a{};
					const bool ok = h3::breakableRow(mB3[slot].index, group, a.data());
					have[key] = ok; if (ok) rows[key] = a;
					it = have.find(key);
				}
				return it->second ? &rows[key] : nullptr;
			};
			auto solid = [&](int slot, uint32_t s) -> bool {
				const auto& sfs = mB3[slot].world.surfaces;
				if (s == CollBsp::kNone || s >= sfs.size()) return false;
				const CollBsp::Surface& sf = sfs[s];
				if (sf.flags & kInvalid) return false;
				if (!(sf.flags & kBreakable)) return true;
				const auto* r = row(slot, sf.breakGroup);
				if (!r || sf.breakBit >= 384) return true;
				return (((*r)[sf.breakBit >> 5] >> (sf.breakBit & 31)) & 1u) != 0;
			};
			auto& out = mLines[(int)Cat::Breakable];
			out.clear();
			for (const auto& be : mBreakable)
				if (be.slot >= 0 && (size_t)be.slot < mB3.size() && (solid(be.slot, be.left) || solid(be.slot, be.right))) out.push_back(be.l);
		}
		if (wantObjects) gatherObjects3();
		else { for (Cat c : { Cat::Scenery, Cat::Crate, Cat::Machine }) mLines[(int)c].clear(); mObjectTris.clear(); mObjSpans.clear(); }
	}

	// Scenery / crates / machines: the Havok shapes the player actually collides with (box, sphere, capsule, convex hull, lists and
	// transform wrappers). MOPP-backed shapes (some machines / large props) need the engine's own shape-container calls and are counted
	// in objectShapesSkipped() instead of drawn.
	void CollisionWorld::gatherObjects3()
	{
		for (Cat c : { Cat::Scenery, Cat::Crate, Cat::Machine }) mLines[(int)c].clear();
		mObjectTris.clear(); mObjSpans.clear();
		mObjSkipped3 = 0;
		std::vector<h2::ObjectRef> objs;
		h3::enumerateObjects(objs);
		h3::ShapeLines sl;
		for (const auto& o : objs)
		{
			Cat cat;
			if (o.type == h3::Scenery) cat = Cat::Scenery;
			else if (o.type == h3::Crate) cat = Cat::Crate;
			else if (o.type == h3::Machine) cat = Cat::Machine;
			else continue;
			if (h3::objectHidden(o.obj)) continue;
			uintptr_t bodies[8];
			const int nb = h3::havokBodies(o.obj, bodies);
			for (int i = 0; i < nb; ++i)
			{
				sl.a.clear(); sl.b.clear();
				mObjSkipped3 += h3::bodyShapeLines(bodies[i], sl);
				auto& out = mLines[(int)cat];
				for (size_t k = 0; k < sl.a.size(); ++k) out.push_back({ sl.a[k], sl.b[k] });
			}
		}
	}
}
