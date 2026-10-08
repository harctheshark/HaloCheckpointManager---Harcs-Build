#include "pch.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <windows.h>
#include <cmath>
#include <cstring>

using namespace mem;

namespace h2
{
	uintptr_t base()
	{
		return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"halo2.dll"));
	}

	bool buildOk()
	{
		static uintptr_t sCheckedBase = 0;
		static bool sOk = false;
		const uintptr_t b = base();
		if (!b) return false;
		if (b == sCheckedBase) return sOk;
		sCheckedBase = b;
		// vtable slots must point at the functions the RE was done on (same checks as GhostingTimLogger.lua)
		struct Check { uintptr_t slot, fn; const char* what; };
		const Check checks[] = {
			{ vt::WELDER_AGENT + 4 * 8,     0x9AC680, "welder processCollision" },
			{ vt::WELDER_AGENT_S + 4 * 8,   0x9AC610, "symmetric welder processCollision" },
			{ vt::GSK_AGENT + 8 * 8,        0x98DA20, "GSK getClosestPoint (TIM)" },
			{ vt::SWEEP_AGENT + 8 * 8,      0x989210, "sweep getClosestPoint" },
			{ vt::STAB_BOX_MOTION + 29 * 8, 0x9DFEB0, "stab-box setTime" },
			{ vt::STAB_BOX_MOTION + 1 * 8,  0x9E02E0, "stab-box integrate" },
		};
		sOk = true;
		for (const auto& c : checks)
		{
			const uintptr_t v = q(b + c.slot);
			if (v != b + c.fn) { LOGF("BUILD MISMATCH: %s slot holds rva 0x%llX, expected 0x%llX", c.what, (unsigned long long)(v - b), (unsigned long long)c.fn); sOk = false; }
		}
		LOGF("halo2.dll @ 0x%llX, build self-check %s", (unsigned long long)b, sOk ? "OK (1.3528)" : "FAILED");
		return sOk;
	}

	uint32_t gameTick()
	{
		const uintptr_t gt = q(base() + rva::GAME_TIME);
		return okp(gt) ? u32(gt + 8, 0) : 0;
	}

	Camera readCamera()
	{
		Camera c;
		const uintptr_t b = base();
		if (!b) return c;
		const uintptr_t cam = b + rva::CAMERA;
		float buf[15];
		if (!copy(buf, cam, sizeof(buf))) return c;
		c.pos = { buf[0], buf[1], buf[2] };
		c.fwd = { buf[8], buf[9], buf[10] };   // +0x20
		c.up  = { buf[11], buf[12], buf[13] }; // +0x2C
		c.vfov = f32(b + rva::VERTICAL_FOV, 0.f);
		c.nearClip = f32(b + rva::NEAR_CLIP, 0.f);
		c.farClip = f32(b + rva::FAR_CLIP, 0.f);
		const float lf = c.fwd.x * c.fwd.x + c.fwd.y * c.fwd.y + c.fwd.z * c.fwd.z;
		const float lu = c.up.x * c.up.x + c.up.y * c.up.y + c.up.z * c.up.z;
		c.ok = std::isfinite(buf[0]) && lf > 0.5f && lf < 1.5f && lu > 0.5f && lu < 1.5f && c.vfov > 0.05f && c.vfov < 3.0f;
		return c;
	}

	bool readFrame(GameFrame& f)
	{
		const uintptr_t b = base();
		f.ok = false;
		if (!b) return false;
		float cam[18];
		if (!copy(cam, b + rva::FRAME_CAMERA, sizeof(cam))) return false;
		f.pos = { cam[0], cam[1], cam[2] }; f.fwd = { cam[3], cam[4], cam[5] }; f.up = { cam[6], cam[7], cam[8] };
		f.vfov = cam[10];
		if (!copy(f.vp, b + rva::FRAME_CAMERA + 0x30, sizeof(f.vp))) return false;
		f.nearClip = f32(b + rva::FRAME_CAMERA + 0x40); f.farClip = f32(b + rva::FRAME_CAMERA + 0x44);
		if (!copy(f.w2v, b + rva::FRAME_W2V, sizeof(f.w2v)) || !copy(f.proj, b + rva::FRAME_PROJ, sizeof(f.proj))) return false;
		f.textureCamera = u8(b + rva::FRAME_TEXCAM) != 0;
		const int w = f.vp[3] - f.vp[1], h = f.vp[2] - f.vp[0];
		f.ok = w > 16 && h > 16 && std::isfinite(f.proj[0]) && std::fabs(f.proj[15]) < 1e6f && f.nearClip > 0 && f.farClip > f.nearClip;
		return f.ok;
	}

	// ---- tag data -------------------------------------------------------------------------------
	bool tagPools(TagPools& out)
	{
		const uintptr_t b = base();
		out.a = q(b + rva::TAG_POOL_A);
		out.b = q(b + rva::TAG_POOL_B);
		return okp(out.a);
	}

	uintptr_t resolveTagOffset(int32_t dataOffset, const TagPools& p)
	{
		if (dataOffset == -1) return 0;
		return dataOffset >= 0 ? p.a + (uint32_t)dataOffset : p.b + (uint32_t)(dataOffset & 0x7FFFFFFF);
	}

	bool resolveBlock(uintptr_t blockAddr, const TagPools& p, uintptr_t& first, uint32_t& count)
	{
		uint32_t hdr[2];
		if (!copy(hdr, blockAddr, 8)) return false;
		count = hdr[0];
		if (count == 0) { first = 0; return true; }
		first = resolveTagOffset((int32_t)hdr[1], p);
		return okp(first);
	}

	uintptr_t structureBsp() { return q(base() + rva::SBSP); }

	uintptr_t tagData(uint16_t tagIndex, const TagPools& p)
	{
		const uintptr_t meta = q(base() + rva::META_HEADER);
		if (!okp(meta)) return 0;
		const int32_t groups = i32(meta + 0x04, -1);
		if (groups < 0 || groups > 4096) return 0;
		const uintptr_t table = meta + 0x20 + (size_t)groups * 0x0C; // tag instances {group, datum, dataOff, size}
		const int32_t off = i32(table + (size_t)tagIndex * 0x10 + 0x08, -1);
		return resolveTagOffset(off, p);
	}

	// ---- data arrays / objects --------------------------------------------------------------------
	// Halo data array (sub_18067B9A0): elem = hdr + *(hdr+72) + idx * *(hdr+36), count bound at hdr+60, salt u16 at elem+0
	static uintptr_t datumGet(uintptr_t hdr, uint32_t datum, bool checkSalt, int32_t* elszOut = nullptr)
	{
		if (!okp(hdr) || datum == 0xFFFFFFFF) return 0;
		const uint32_t idx = datum & 0xFFFF, salt = datum >> 16;
		const int32_t elsz = i32(hdr + 36, 0), cnt = i32(hdr + 60, 0);
		const int64_t doff = get<int64_t>(hdr + 72, 0);
		if (elsz <= 0 || elsz > 0x10000 || (int32_t)idx >= cnt || doff == 0) return 0;
		const uintptr_t elem = hdr + (uintptr_t)doff + (uintptr_t)idx * elsz;
		if (checkSalt)
		{
			const uint16_t s = u16(elem);
			if (s == 0 || s == 0xFFFF || s != salt) return 0;
		}
		if (elszOut) *elszOut = elsz;
		return elem;
	}

	static uintptr_t objectPoolBase()
	{
		const uintptr_t pool = q(base() + rva::OBJ_POOL);
		return okp(pool) ? ((pool + 0x57) & ~uintptr_t(0xF)) : 0; // sub_18076B320
	}

	uintptr_t objectFromDatum(uint32_t datum)
	{
		const uintptr_t entry = datumGet(q(base() + rva::OBJ_HEADER), datum, true);
		if (!entry) return 0;
		const uint32_t off = u32(entry + 8);
		const uintptr_t pb = objectPoolBase();
		if (off == 0xFFFFFFFF || !pb) return 0;
		return pb + off;
	}

	void enumerateObjects(std::vector<ObjectRef>& out)
	{
		out.clear();
		const uintptr_t hdr = q(base() + rva::OBJ_HEADER);
		const uintptr_t pb = objectPoolBase();
		if (!okp(hdr) || !pb) return;
		const int32_t elsz = i32(hdr + 36, 0), cnt = i32(hdr + 60, 0);
		const int64_t doff = get<int64_t>(hdr + 72, 0);
		if (elsz != 12 || cnt <= 0 || cnt > 4096 || doff == 0) return;
		std::vector<uint8_t> raw((size_t)cnt * 12);
		if (!copy(raw.data(), hdr + (uintptr_t)doff, raw.size())) return;
		for (int32_t i = 0; i < cnt; ++i)
		{
			const uint8_t* e = &raw[(size_t)i * 12];
			const uint16_t salt = *(const uint16_t*)e;
			const uint32_t off = *(const uint32_t*)(e + 8);
			if (salt == 0 || off == 0xFFFFFFFF) continue;
			out.push_back({ pb + off, ((uint32_t)salt << 16) | (uint32_t)i, e[3] });
		}
	}

	uintptr_t localPlayerUnitObject()
	{
		const uintptr_t b = base();
		const uintptr_t pg = q(b + rva::PLAYERS_GLOBALS), phdr = q(b + rva::PLAYERS_ARRAY);
		if (!okp(pg) || !okp(phdr)) return 0;
		uintptr_t player = datumGet(phdr, u32(pg + 28), true);  // user 0 (sub_18069CFF0)
		if (!player)
		{
			// fallback: first live player with a unit
			int32_t elsz = i32(phdr + 36, 0), cnt = i32(phdr + 60, 0);
			const int64_t doff = get<int64_t>(phdr + 72, 0);
			if (elsz == 548 && cnt > 0 && cnt <= 64 && doff)
				for (int32_t i = 0; i < cnt && !player; ++i)
				{
					const uintptr_t e = phdr + (uintptr_t)doff + (uintptr_t)i * elsz;
					const uint16_t s = u16(e, 0);
					const uint32_t u = u32(e + 44);
					if (s && s != 0xFFFF && u != 0xFFFFFFFF) player = e;
				}
		}
		if (!player) return 0;
		const uint32_t unit = u32(player + 44);
		return unit == 0xFFFFFFFF ? 0 : objectFromDatum(unit);
	}

	// ---- Havok ------------------------------------------------------------------------------------
	uintptr_t havokEntityForObject(uintptr_t obj, bool wantDynamic)
	{
		const uintptr_t b = base();
		const uint32_t hd = u32(obj + 0xB4);                 // havok component datum
		if (hd == 0xFFFFFFFF || hd == 0) return 0;
		int32_t elsz = 0;
		const uintptr_t comp = datumGet(q(b + rva::HAVOK_COMPS), hd, false, &elsz);
		if (!comp || elsz != 192) return 0;
		const uintptr_t recs = q(comp + 112);               // sub_180715200: record array (112-B records)
		const int32_t n = i32(comp + 120, 0);
		if (!okp(recs) || n <= 0 || n > 64) return 0;
		uintptr_t firstAny = 0;
		for (int32_t i = 0; i < n; ++i)
		{
			const uintptr_t e = q(recs + 112 * (uintptr_t)i + 64);
			if (!okp(e) || q(e) != b + vt::RIGID_BODY) continue;
			if (!firstAny) firstAny = e;
			if (!wantDynamic || u8(e + 0x70) == 0) return e;  // +0x70 = is-fixed (sub_1809CE960)
		}
		return wantDynamic ? 0 : firstAny;
	}

	static inline Vec3 xform(const float R[12], const float t[3], const float p[3])
	{
		// motion transform: three rotation COLUMNS (16-B stride) at motion+0x90/+0xA0/+0xB0, translation at +0xC0
		return { R[0] * p[0] + R[4] * p[1] + R[8] * p[2] + t[0],
		         R[1] * p[0] + R[5] * p[1] + R[9] * p[2] + t[1],
		         R[2] * p[0] + R[6] * p[1] + R[10] * p[2] + t[2] };
	}

	Capsule entityCapsule(uintptr_t entity)
	{
		Capsule c;
		const uintptr_t motion = q(entity + 0x68);
		uintptr_t shape = q(entity + 0x18);                  // collidable (entity+0x18) +0 = shape
		if (!okp(motion) || !okp(shape)) return c;
		// unwrap hkConvexSweepShape (vtbl 0xB6E468, getType 23): child at +0x40 (sub_18098AA90); then require
		// hkCapsuleShape (vtbl 0xB70230, ctor sub_1809B1350): radius +0x18 (= A.w = B.w), A +0x20, B +0x30
		const uintptr_t b = base();
		if (q(shape) == b + 0xB6E468) shape = q(shape + 0x40);
		if (!okp(shape) || q(shape) != b + 0xB70230) return c;
		float a[4], bb[4], R[12], t[4];
		if (!copy(a, shape + 0x20, 16) || !copy(bb, shape + 0x30, 16)) return c;
		if (!copy(R, motion + 0x90, 48) || !copy(t, motion + 0xC0, 16)) return c;
		float r = a[3];                                      // H2 Havok 2 capsule: radius in A.w (+0x2C)
		if (!(r > 0.02f && r < 2.f)) r = f32(shape + 0x18, 0.f);
		if (!(r > 0.02f && r < 2.f)) return c;
		const float ab2 = (a[0] - bb[0]) * (a[0] - bb[0]) + (a[1] - bb[1]) * (a[1] - bb[1]) + (a[2] - bb[2]) * (a[2] - bb[2]);
		if (!(ab2 < 16.f)) return c;
		c.a = xform(R, t, a); c.b = xform(R, t, bb); c.r = r;
		c.ok = std::isfinite(c.a.x) && std::isfinite(c.b.z);
		return c;
	}

	PairInfo findBspPair(uintptr_t entity)
	{
		PairInfo out;
		const uintptr_t b = base();
		const uintptr_t world = q(b + rva::WORLD), bsp = q(b + rva::BSP_ENTITY);
		if (!okp(world) || !okp(bsp)) return out;
		const uintptr_t H = entity + 0x38, bspH = bsp + 0x38; // pair entries hold collidable+0x20 = entity+0x38

		auto scan = [&](uintptr_t island) -> bool
		{
			const uintptr_t parr = q(island + 0x90);
			const int32_t np = i32(island + 0x98, -1);
			if (!okp(parr) || np <= 0 || np > 8192) return false;
			std::vector<uint8_t> blk((size_t)np * 32);
			if (!copy(blk.data(), parr, blk.size())) return false;
			for (int32_t j = 0; j < np; ++j)
			{
				const uint8_t* e = &blk[(size_t)j * 32];
				const uintptr_t a0 = *(const uintptr_t*)e, a1 = *(const uintptr_t*)(e + 8);
				if ((a0 == H && a1 == bspH) || (a1 == H && a0 == bspH))
				{
					out.entry = parr + (uintptr_t)j * 32;
					out.agent = *(const uintptr_t*)(e + 16);
					out.S = *(const int32_t*)(e + 24);
					out.found = true;
					const uintptr_t avt = q(out.agent) - b;
					out.agentIsWelder = okp(out.agent) && (avt == vt::WELDER_AGENT || avt == vt::WELDER_AGENT_S);
					return true;
				}
			}
			return false;
		};

		const uintptr_t island = q(entity + 0x78);
		if (okp(island) && q(island + 0x30) == world && scan(island)) return out;
		const uintptr_t arr = q(world + 0x10);
		const int32_t n = i32(world + 0x18, 0);
		if (okp(arr) && n > 0 && n <= 1024)
			for (int32_t k = 0; k < n; ++k)
			{
				const uintptr_t isl = q(arr + 8 * (uintptr_t)k);
				if (okp(isl) && isl != island && scan(isl)) return out;
			}
		return out;
	}

	bool readChildren(uintptr_t agent, std::vector<Child>& out)
	{
		out.clear();
		const uintptr_t b = base();
		const uintptr_t arr = q(agent + 0x18);
		const int32_t n = i32(agent + 0x20, -1);
		const int32_t cap = i32(agent + 0x24, 0) & 0x7FFFFFFF;
		if (!okp(arr) || n < 0 || n > 512 || cap < n) return false;
		struct Entry { uint32_t key; uint32_t pad; uintptr_t agent; };
		std::vector<Entry> ents((size_t)n);
		if (n && !copy(ents.data(), arr, ents.size() * sizeof(Entry))) return false;
		// the sim thread rewrites this array in place every collide: re-read pointer + count, reject a torn copy
		if (q(agent + 0x18) != arr || i32(agent + 0x20, -1) != n) return false;
		for (size_t i = 1; i < ents.size(); ++i)
			if (ents[i].key < ents[i - 1].key) return false;                 // children are kept sorted by key
		for (const auto& e : ents)
		{
			Child c; c.key = e.key; c.agent = e.agent;
			const uintptr_t cvt = q(e.agent) - b;
			if (e.agent == b + vt::NULL_AGENT_INST) c.kind = Child::Null;
			else if (cvt == vt::SWEEP_AGENT) c.kind = Child::Sweep;
			else if (cvt == vt::GSK_AGENT) c.kind = Child::Gsk;
			else c.kind = Child::Other;
			if (c.kind == Child::Sweep || c.kind == Child::Gsk)
			{
				uint8_t g[0x28];
				if (copy(g, e.agent + 0x18, sizeof(g)))
				{
					memcpy(c.ids, g, 8);
					c.dimA = g[8]; c.dimB = g[9];
					c.manifold = *(const int32_t*)(g + 0x0C);
					c.normal = { *(const float*)(g + 0x18), *(const float*)(g + 0x1C), *(const float*)(g + 0x20) };
					c.tim = *(const float*)(g + 0x24);
					if (c.dimA <= 4 && c.dimB >= 1 && c.dimB <= 3 && c.dimA + c.dimB <= 4)
					{
						const int i0 = c.ids[c.dimA], i1 = c.dimB >= 2 ? c.ids[c.dimA + 1] : -1;
						if (c.dimB == 1) { c.feature = 0; c.featA = i0; }
						else if (c.dimB == 2) { c.feature = 1; c.featA = i0; c.featB = i1; } // adjacency checked by the caller
						else c.feature = 2;
					}
				}
			}
			out.push_back(c);
		}
		return true;
	}
}
