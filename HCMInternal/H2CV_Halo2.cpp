#include "pch.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include "H2CV_Log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

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

	bool readWorldBreakableRow(uint32_t out[8])
	{
		const uintptr_t block = q(base() + rva::BREAKABLE_BLOCK);
		const int16_t ctx = (int16_t)u16(base() + rva::BREAKABLE_CTX, 0xFFFF);
		if (!okp(block) || ctx < 0) return false;
		// the row sub_18071ABE0 uses for instance -1: block + 1 + ctx * 32 (8 dwords = 256 breakable bits)
		return copy(out, block + 1 + (uintptr_t)ctx * 32, 8 * sizeof(uint32_t));
	}

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

	bool objectNodeMatrix(uintptr_t obj, uint16_t node, float out[13])
	{
		// nodes block reference in the object datum: i16 size @+0x118, i16 offset @+0x11A (52-B stride).
		// capstone on halo2_1.3528.dll: sub_1808D6600 (movsx [obj+0x11a], imul 0x34), sub_1808D11A0 (movsx [+0x118], /52)
		int16_t size = 0, off = 0;
		if (!okp(obj) || !copy(&size, obj + 0x118, 2) || !copy(&off, obj + 0x11A, 2)) return false;
		if (off <= 0 || size < 52) return false;               // 0xFFFF (NONE) or empty block
		if (node >= (uint32_t)size / 52) return false;         // count = size / sizeof(real_matrix4x3)
		return copy(out, obj + (uintptr_t)off + (uintptr_t)node * 52, 52);
	}

	bool objectHidden(uintptr_t obj)
	{
		return (u32(obj + 4, 0) & 1u) != 0;
	}

	int objectRegionState(uintptr_t obj, int8_t* out, int maxN)
	{
		// sub_1808D6940 (decompiled): v11 = *(i16*)(obj+284); count = v11 / 10; base = obj + *(i16*)(obj+286)
		int16_t size = 0, off = 0;
		if (!okp(obj) || !copy(&size, obj + 0x11C, 2) || !copy(&off, obj + 0x11E, 2)) return 0;
		if (off <= 0 || size < 10) return 0;
		int n = size / 10;
		if (n > maxN) n = maxN;
		if (n <= 0 || !copy(out, obj + (uintptr_t)off, (size_t)n)) return 0;
		return n;
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
			out.push_back({ pb + off, ((uint32_t)salt << 16) | (uint32_t)i, e[3], e[2] });
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

	int localPlayerUnits(uintptr_t out[4])
	{
		for (int i = 0; i < 4; ++i) out[i] = 0;
		int n = 0;
		const uintptr_t b = base();
		const uintptr_t pg = q(b + rva::PLAYERS_GLOBALS), phdr = q(b + rva::PLAYERS_ARRAY);
		if (okp(pg) && okp(phdr))
			for (int u = 0; u < 4; ++u)
			{
				const uint32_t pd = u32(pg + 28 + 4 * (uintptr_t)u);
				if (pd == 0xFFFFFFFF) continue;
				const uintptr_t player = datumGet(phdr, pd, true);
				if (!player) continue;
				const uint32_t unit = u32(player + 44);
				if (unit == 0xFFFFFFFF) continue;
				if ((out[u] = objectFromDatum(unit)) != 0) ++n;
			}
		if (!n && (out[0] = localPlayerUnitObject()) != 0) n = 1;
		return n;
	}

	uint32_t localPlayerUnitDatum(int user)
	{
		const uintptr_t b = base();
		const uintptr_t pg = q(b + rva::PLAYERS_GLOBALS), phdr = q(b + rva::PLAYERS_ARRAY);
		if (!okp(pg) || !okp(phdr) || user < 0 || user > 3) return 0xFFFFFFFF;
		const uint32_t pd = u32(pg + 28 + 4 * (uintptr_t)user);
		if (pd == 0xFFFFFFFF) return 0xFFFFFFFF;
		const uintptr_t player = datumGet(phdr, pd, true);
		return player ? u32(player + 44) : 0xFFFFFFFF;
	}

	OobTimer readOobTimer(int user)
	{
		OobTimer o;
		const uintptr_t b = base();
		const uintptr_t pg = q(b + rva::PLAYERS_GLOBALS), phdr = q(b + rva::PLAYERS_ARRAY);
		if (!okp(pg) || !okp(phdr) || user < 0 || user > 3) return o;
		const uint32_t pd = u32(pg + 28 + 4 * (uintptr_t)user);
		if (pd == 0xFFFFFFFF) return o;
		const uintptr_t player = datumGet(phdr, pd, true);
		if (!player) return o;
		o.counter = (int)u8(player + 0x21C, 0);
		const int16_t tps = (int16_t)u16(q(b + rva::GAME_TIME) + 2, 30);
		if (tps > 0 && tps <= 120) o.ticksPerSec = tps;
		float sec = 0.5f;
		if (!copy(&sec, b + rva::OOB_LIMIT_SEC, 4) || !(sec > 0.f && sec < 10.f)) sec = 0.5f;
		o.limit = (int)(o.ticksPerSec * sec + 0.5f);                 // sub_180706710: seconds -> ticks, rounded
		o.playerIndex = pd & 0xFFFF;
		const uint32_t tick = gameTick();
		o.nextCheck = (int)((16u - ((tick + o.playerIndex) & 15u)) & 15u);   // ticks until (tick + index) % 16 == 0
		o.valid = true;
		return o;
	}

	namespace
	{
		typedef uint8_t(__fastcall* t_trigger_test)(int32_t trigger, uint32_t objectDatum);
		int callTriggerTest(t_trigger_test fn, int32_t trigger, uint32_t datum)
		{
			__try { return fn(trigger, datum) ? 1 : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
	}

	int gameTriggerVolumeTest(int16_t trigger, uint32_t objectDatum)
	{
		if (trigger < 0 || objectDatum == 0xFFFFFFFF) return 0;
		const uintptr_t b = base();
		if (!b) return -1;
		return callTriggerTest((t_trigger_test)(b + rva::FN_TRIGGER_TEST), trigger, objectDatum);
	}

	bool killTestPoint(uintptr_t object, Vec3& out)
	{
		float f[3];
		if (!okp(object) || !copy(f, object + 0x30, 12) || !std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
		out = { f[0], f[1], f[2] };
		return true;
	}

	bool readKillVolumes(std::vector<KillVolume>& out)
	{
		out.clear();
		TagPools p;
		const uintptr_t scen = q(base() + rva::SCENARIO);
		if (!okp(scen) || !tagPools(p)) return false;
		uintptr_t kills = 0, tvs = 0; uint32_t nk = 0, ntv = 0;
		if (!resolveBlock(scen + 0x230, p, kills, nk) || !resolveBlock(scen + 0x108, p, tvs, ntv)) return false;
		if (nk == 0) return true;
		if (nk > 4096 || ntv > 65535) return false;
		const uintptr_t bits = q(base() + rva::KILL_DISABLED);
		for (uint32_t i = 0; i < nk; ++i)
		{
			KillVolume kv;
			kv.trigger = (int16_t)u16(kills + 2 * (uintptr_t)i, 0xFFFF);
			if (okp(bits)) kv.enabled = ((u32(bits + 4 * (uintptr_t)(i >> 5), 0) >> (i & 31)) & 1) == 0;
			if (kv.trigger >= 0 && (uint32_t)kv.trigger < ntv)
			{
				// scenario_trigger_volume (0x44 B): object name i16 @+0x04, forward @+0x0C, up @+0x18, position @+0x24,
				// extents @+0x30 (sub_180678B70 / sub_180729BA0: matrix from point + forward + up, left = up x forward)
				const uintptr_t tv = tvs + (uintptr_t)kv.trigger * 0x44;
				kv.attached = (int16_t)u16(tv + 0x04, 0xFFFF) != -1;
				float f[15];
				if (copy(f, tv + 0x0C, sizeof(f)))
				{
					kv.fwd = { f[0], f[1], f[2] }; kv.up = { f[3], f[4], f[5] }; kv.pos = { f[6], f[7], f[8] }; kv.ext = { f[9], f[10], f[11] };
					kv.left = { kv.up.y * kv.fwd.z - kv.up.z * kv.fwd.y, kv.up.z * kv.fwd.x - kv.up.x * kv.fwd.z, kv.up.x * kv.fwd.y - kv.up.y * kv.fwd.x };
				}
			}
			out.push_back(kv);
		}
		return true;
	}

	uintptr_t vehicleOfUnit(uintptr_t unit, uint32_t* vehicleDatum)
	{
		if (vehicleDatum) *vehicleDatum = 0xFFFFFFFF;
		const uintptr_t hdr = q(base() + rva::OBJ_HEADER);
		uintptr_t obj = unit;
		for (int depth = 0; depth < 4 && okp(obj); ++depth)
		{
			// obj+0x14 = parent object datum, -1 = none (sub_1808D2050 object_attach writes it; sub_1808D46E0 @0x1808D47C0
			// `mov r8d,[rax+14h]; cmp r8d,-1` walks it)
			const uint32_t parent = u32(obj + 0x14);
			if (parent == 0xFFFFFFFF) return 0;
			const uintptr_t entry = datumGet(hdr, parent, true);
			const uintptr_t pobj = entry ? objectFromDatum(parent) : 0;
			if (!entry || !pobj) return 0;
			if (u8(entry + 3) == Vehicle && !(u8(entry + 2) & 0x10))   // header type byte, 0x10 = being deleted
			{
				if (vehicleDatum) *vehicleDatum = parent;
				return pobj;
			}
			obj = pobj;
		}
		return 0;
	}

	uint32_t objectDefinitionTag(uintptr_t obj, uint32_t* group)
	{
		if (group) *group = 0;
		const uint32_t idx = u16(obj, 0xFFFF);   // sub_18070E000: `*v4` (u16 at the object) indexes the tag instance table
		if (idx == 0xFFFF) return idx;
		if (group)
		{
			const uintptr_t meta = q(base() + rva::META_HEADER);
			const int32_t groups = okp(meta) ? i32(meta + 0x04, -1) : -1;
			if (groups >= 0 && groups <= 4096)
				*group = u32(meta + 0x20 + (size_t)groups * 0x0C + (size_t)idx * 0x10 + 0x00, 0);
		}
		return idx;
	}

	// ---- Havok ------------------------------------------------------------------------------------
	const char* motionKindName(Motion::Kind k)
	{
		switch (k)
		{
		case Motion::StabBox: return "stabilized box";
		case Motion::Box: return "box";
		case Motion::StabSphere: return "stabilized sphere";
		case Motion::Sphere: return "sphere";
		case Motion::Keyframed: return "keyframed";
		default: return "?";
		}
	}

	bool readMotion(uintptr_t entity, Motion& m, bool anyDynamic)
	{
		m = Motion{};
		const uintptr_t motion = q(entity + 0x68);
		if (!okp(motion)) return false;
		// Every dynamic hkRigidMotion subclass runs the same integrate (decompiled sub_1809DD250 / 1809DE540 sphere,
		// 1809DE820 stabilized sphere, 1809DEC80 / 1809E02E0 box, 1809E05F0 stabilized box, 1809E0AC0 keyframed):
		//   PREV (+0x70) := CUR (+0x80); CUR += dt * v (+0x50); rotation (+0x40 quat -> +0x90 columns) updated;
		//   origin (+0xC0) = CUR - R * com (+0x30); time (+0x8C) := 1.0
		// and the constructors (sub_1809CEAE0 / 1809DEAD0 / 1809E0960, all `rigid motion` bases) lay out +0x30..+0xC0 identically.
		// Only vInt differs: +0xD0 for box motions (a1+208), +0xF0 for sphere motions (a1+240); keyframed motions keep none.
		const uintptr_t vtab = q(motion), b = base();
		Motion::Kind kind = Motion::None; uintptr_t vintOff = 0;
		if (vtab == b + vt::STAB_BOX_MOTION) { kind = Motion::StabBox; vintOff = 0xD0; }
		else if (anyDynamic)
		{
			if (vtab == b + vt::BOX_MOTION) { kind = Motion::Box; vintOff = 0xD0; }
			else if (vtab == b + vt::STAB_SPHERE_MOTION) { kind = Motion::StabSphere; vintOff = 0xF0; }
			else if (vtab == b + vt::SPHERE_MOTION) { kind = Motion::Sphere; vintOff = 0xF0; }
			else if (vtab == b + vt::KEYFRAMED_MOTION) { kind = Motion::Keyframed; vintOff = 0; }
		}
		if (kind == Motion::None) return false;   // fixed / unknown motions: not a live body
		float buf[0xB0 / 4];
		if (!copy(buf, motion + 0x30, sizeof(buf))) return false;
		auto V = [&](int off) { const int i = (off - 0x30) / 4; return Vec3{ buf[i], buf[i + 1], buf[i + 2] }; };
		m.kind = kind;
		m.com = V(0x30); m.v = V(0x50); m.prev = V(0x70); m.cur = V(0x80); m.origin = V(0xC0);
		m.rc[0] = V(0x90); m.rc[1] = V(0xA0); m.rc[2] = V(0xB0);
		m.vint = m.v;
		if (vintOff)
		{
			float vi[3];
			if (copy(vi, motion + vintOff, sizeof(vi))) m.vint = { vi[0], vi[1], vi[2] };
		}
		m.prevW = buf[(0x7C - 0x30) / 4]; m.time = buf[(0x8C - 0x30) / 4];
		m.ok = std::isfinite(m.cur.x) && std::isfinite(m.cur.y) && std::isfinite(m.cur.z) &&
		       std::isfinite(m.prev.x) && std::isfinite(m.prev.y) && std::isfinite(m.prev.z) &&
		       std::isfinite(m.origin.x) && std::isfinite(m.origin.z) && std::isfinite(m.time);
		if (m.ok && anyDynamic)
		{
			// a rotating body: the rotation columns must be finite and roughly orthonormal
			for (const Vec3& c : m.rc)
			{
				const float l2 = c.x * c.x + c.y * c.y + c.z * c.z;
				if (!std::isfinite(l2) || l2 < 0.8f || l2 > 1.2f) m.ok = false;
			}
		}
		return m.ok;
	}

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

	int havokBodies(uintptr_t obj, uintptr_t out[8])
	{
		const uintptr_t b = base();
		const uint32_t hd = u32(obj + 0xB4);
		if (hd == 0xFFFFFFFF || hd == 0) return 0;
		int32_t elsz = 0;
		const uintptr_t comp = datumGet(q(b + rva::HAVOK_COMPS), hd, false, &elsz);
		if (!comp || elsz != 192) return 0;
		const uintptr_t recs = q(comp + 112);
		const int32_t n = i32(comp + 120, 0);
		if (!okp(recs) || n <= 0 || n > 64) return 0;
		int cnt = 0;
		auto add = [&](int32_t i)
		{
			const uintptr_t e = q(recs + 112 * (uintptr_t)i + 64);
			if (!okp(e) || q(e) != b + vt::RIGID_BODY) return;
			for (int k = 0; k < cnt; ++k) if (out[k] == e) return;
			if (cnt < 8) out[cnt++] = e;
		};
		const int8_t primary = (int8_t)u8(comp + 0x18, 0xFF);   // sub_18070C400 tail: comp+24 = primary record index (-1 = none)
		if (primary >= 0 && primary < n) add(primary);
		for (int32_t i = 0; i < n; ++i) add(i);
		return cnt;
	}

	static inline Vec3 xform(const float R[12], const float t[3], const float p[3])
	{
		// motion transform: three rotation COLUMNS (16-B stride) at motion+0x90/+0xA0/+0xB0, translation at +0xC0
		return { R[0] * p[0] + R[4] * p[1] + R[8] * p[2] + t[0],
		         R[1] * p[0] + R[5] * p[1] + R[9] * p[2] + t[1],
		         R[2] * p[0] + R[6] * p[1] + R[10] * p[2] + t[2] };
	}

	Capsule entityCapsuleLocal(uintptr_t entity)
	{
		Capsule c;
		uintptr_t shape = q(entity + 0x18);                  // collidable (entity+0x18) +0 = shape
		if (!okp(shape)) return c;
		// unwrap hkConvexSweepShape (vtbl 0xB6E468, getType 23): child at +0x40 (sub_18098AA90); then require
		// hkCapsuleShape (vtbl 0xB70230, ctor sub_1809B1350): radius +0x18 (= A.w = B.w), A +0x20, B +0x30
		const uintptr_t b = base();
		if (q(shape) == b + 0xB6E468) shape = q(shape + 0x40);
		// two vtables for the same capsule class: 0xB70230 and 0xB19E50 hold identical method pointers (COMDAT copy);
		// the player's crouch-rebuilt pill came back with 0xB19E50 live (2026-10-13), so accept either
		if (!okp(shape)) return c;
		const uintptr_t svt = q(shape);
		if (svt != b + 0xB70230 && svt != b + 0xB19E50) return c;
		float a[4], bb[4];
		if (!copy(a, shape + 0x20, 16) || !copy(bb, shape + 0x30, 16)) return c;
		float r = a[3];                                      // H2 Havok 2 capsule: radius in A.w (+0x2C)
		if (!(r > 0.02f && r < 2.f)) r = f32(shape + 0x18, 0.f);
		if (!(r > 0.02f && r < 2.f)) return c;
		const float ab2 = (a[0] - bb[0]) * (a[0] - bb[0]) + (a[1] - bb[1]) * (a[1] - bb[1]) + (a[2] - bb[2]) * (a[2] - bb[2]);
		if (!(ab2 < 16.f)) return c;
		c.a = { a[0], a[1], a[2] }; c.b = { bb[0], bb[1], bb[2] }; c.r = r;
		c.ok = std::isfinite(a[0]) && std::isfinite(bb[2]);
		return c;
	}

	Capsule entityCapsule(uintptr_t entity)
	{
		Capsule c = entityCapsuleLocal(entity);
		const uintptr_t motion = q(entity + 0x68);
		if (!c.ok || !okp(motion)) { c.ok = false; return c; }
		float R[12], t[4];
		if (!copy(R, motion + 0x90, 48) || !copy(t, motion + 0xC0, 16)) { c.ok = false; return c; }
		const float a[3] = { c.a.x, c.a.y, c.a.z }, bb[3] = { c.b.x, c.b.y, c.b.z };
		c.a = xform(R, t, a); c.b = xform(R, t, bb);
		c.ok = std::isfinite(c.a.x) && std::isfinite(c.b.z);
		return c;
	}

	AgentKind agentKind(uintptr_t agent)
	{
		if (!okp(agent)) return AgentKind::Unknown;
		const uintptr_t v = q(agent) - base();
		if (v == vt::WELDER_AGENT || v == vt::WELDER_AGENT_S) return AgentKind::Welder;
		if (v == vt::LIST_AGENT || v == vt::LIST_AGENT_S) return AgentKind::List;
		if (v == vt::TRANSFORM_AGENT || v == vt::TRANSFORM_AGENT_S) return AgentKind::Transform;
		// hkBvTreeAgent: same vtable functions as hkMoppAgent (only the RTTI differs), so the same children layout
		if (v == vt::MOPP_AGENT || v == vt::MOPP_AGENT_S || v == vt::BVTREE_AGENT || v == vt::BVTREE_AGENT_S) return AgentKind::Mopp;
		if (v == vt::SWEEP_AGENT || v == vt::GSK_AGENT) return AgentKind::Convex;
		return AgentKind::Unknown;
	}

	const char* agentClassName(uintptr_t agent)
	{
		if (!okp(agent)) return "none";
		const uintptr_t b = base();
		if (agent == b + vt::NULL_AGENT_INST) return "null";
		const uintptr_t v = q(agent) - b;
		static const struct { uintptr_t vt; const char* nm; } names[] = {
			{ vt::WELDER_AGENT, "ConvexConvexWelder" }, { vt::WELDER_AGENT_S, "ConvexConvexWelder(sym)" },
			{ vt::LIST_AGENT, "List" }, { vt::LIST_AGENT_S, "List(sym)" },
			{ vt::TRANSFORM_AGENT, "Transform" }, { vt::TRANSFORM_AGENT_S, "Transform(sym)" },
			{ vt::MOPP_AGENT, "Mopp" }, { vt::MOPP_AGENT_S, "Mopp(sym)" },
			{ vt::SWEEP_AGENT, "ConvexSweepConvex" }, { vt::GSK_AGENT, "GskConvexConvex" },
			{ vt::CAPSULE_CAPSULE_AGENT, "CapsuleCapsule" }, { vt::BOX_BOX_AGENT, "BoxBox" },
			{ vt::BV_AGENT, "Bv" }, { vt::BV_AGENT_S, "Bv(sym)" },
			{ vt::BVTREE_AGENT, "BvTree" }, { vt::BVTREE_AGENT_S, "BvTree(sym)" },
			{ vt::SHAPE_COLL_AGENT, "ShapeCollection" }, { vt::SHAPE_COLL_AGENT_S, "ShapeCollection(sym)" },
			{ vt::CONVEX_WELDER_AGENT, "ConvexWelder" }, { vt::CONVEX_WELDER_AGENT_S, "ConvexWelder(sym)" },
			{ vt::SPHERE_WELDER_AGENT, "SphereConvexWelder" }, { vt::SPHERE_WELDER_AGENT_S, "SphereConvexWelder(sym)" },
			{ vt::MULTISPHERE_AGENT, "MultiSphere" }, { vt::MULTISPHERE_AGENT_S, "MultiSphere(sym)" },
		};
		for (const auto& n : names) if (v == n.vt) return n.nm;
		return "?";
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
					out.kind = agentKind(out.agent);
					out.agentIsWelder = out.kind == AgentKind::Welder;
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

	int findObjectPairs(uintptr_t entity, std::vector<ObjPair>& out)
	{
		out.clear();
		const uintptr_t b = base();
		const uintptr_t world = q(b + rva::WORLD);
		if (!okp(world) || !okp(entity)) return 0;
		const uintptr_t bsp = q(b + rva::BSP_ENTITY), mesh = q(b + rva::MESH_ENTITY);
		const uintptr_t H = entity + 0x38;
		const uintptr_t bspH = okp(bsp) ? bsp + 0x38 : 1, meshH = okp(mesh) ? mesh + 0x38 : 1;
		bool readAny = false;
		auto scan = [&](uintptr_t island)
		{
			const uintptr_t parr = q(island + 0x90);
			const int32_t np = i32(island + 0x98, -1);
			if (!okp(parr) || np <= 0 || np > 8192) return;
			std::vector<uint8_t> blk((size_t)np * 32);
			if (!copy(blk.data(), parr, blk.size())) return;
			readAny = true;
			for (int32_t j = 0; j < np && out.size() < 64; ++j)
			{
				const uint8_t* e = &blk[(size_t)j * 32];
				const uintptr_t a0 = *(const uintptr_t*)e, a1 = *(const uintptr_t*)(e + 8);
				uintptr_t oh = 0;
				if (a0 == H) oh = a1; else if (a1 == H) oh = a0; else continue;
				if (oh == bspH || oh == meshH || !okp(oh)) continue;
				const uintptr_t other = oh - 0x38;
				if (q(other) != b + vt::RIGID_BODY) continue;   // phantoms etc.
				ObjPair p; p.entry = parr + (uintptr_t)j * 32; p.agent = *(const uintptr_t*)(e + 16); p.S = *(const int32_t*)(e + 24); p.other = other;
				out.push_back(p);
			}
		};
		// Every island that could hold the agent: a pair against a keyframed or sleeping body (trams, elevators, doors at
		// rest) is not necessarily in the biped's own island. Candidates: the biped's island, every element of the first
		// three hkArrays of the world (+0x10 active, then the next two), and loose island pointers in world +0x40..+0x100;
		// each accepted only if it points back at the world (island +0x30), and each scanned once.
		std::vector<uintptr_t> islands;
		auto consider = [&](uintptr_t isl)
		{
			if (!okp(isl) || q(isl + 0x30) != world) return;
			if (std::find(islands.begin(), islands.end(), isl) == islands.end() && islands.size() < 2048) islands.push_back(isl);
		};
		consider(q(entity + 0x78));
		for (uintptr_t off : { (uintptr_t)0x10, (uintptr_t)0x20, (uintptr_t)0x30 })
		{
			const uintptr_t arr = q(world + off);
			const int32_t n = i32(world + off + 8, 0);
			if (!okp(arr) || n <= 0 || n > 1024) continue;
			std::vector<uintptr_t> ptrs((size_t)n);
			if (!copy(ptrs.data(), arr, ptrs.size() * 8)) continue;
			for (uintptr_t p : ptrs) consider(p);
		}
		for (uintptr_t off = 0x40; off <= 0x100; off += 8) consider(q(world + off));
		for (uintptr_t isl : islands) { if (out.size() >= 64) break; scan(isl); }
		// the same pair can sit in two scanned arrays (merging islands): keep one entry per other body
		std::vector<ObjPair> uniq;
		for (const auto& p : out)
			if (std::find_if(uniq.begin(), uniq.end(), [&](const ObjPair& u) { return u.other == p.other && u.agent == p.agent; }) == uniq.end()) uniq.push_back(p);
		out.swap(uniq);
		(void)readAny;
		return (int)out.size();
	}

	bool objectOfEntity(uintptr_t entity, ObjectRef& out)
	{
		std::vector<ObjectRef> objs;
		enumerateObjects(objs);
		for (const auto& o : objs)
		{
			if (o.hdrFlags & 0x10) continue;
			uintptr_t bodies[8];
			const int n = havokBodies(o.obj, bodies);
			for (int i = 0; i < n; ++i) if (bodies[i] == entity) { out = o; return true; }
		}
		return false;
	}

	// GSK cache + TIM of a sweep / GSK agent (both lay it out at +0x18): vertex ids +0x18, dims +0x20/+0x21,
	// manifold +0x24, separating normal +0x30, TIM +0x3C
	static bool readGskCache(uintptr_t agent, Child& c)
	{
		uint8_t g[0x28];
		if (!copy(g, agent + 0x18, sizeof(g))) return false;
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
		return std::isfinite(c.tim);
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
		// a welder keeps its children sorted by key (a torn copy breaks that); MOPP / BV-tree agents need not
		if (agentKind(agent) == AgentKind::Welder)
			for (size_t i = 1; i < ents.size(); ++i)
				if (ents[i].key < ents[i - 1].key) return false;
		for (const auto& e : ents)
		{
			Child c; c.key = e.key; c.agent = e.agent;
			const uintptr_t cvt = q(e.agent) - b;
			if (e.agent == b + vt::NULL_AGENT_INST) c.kind = Child::Null;
			else if (cvt == vt::SWEEP_AGENT) c.kind = Child::Sweep;
			else if (cvt == vt::GSK_AGENT) c.kind = Child::Gsk;
			else c.kind = Child::Other;
			if (c.kind == Child::Sweep || c.kind == Child::Gsk) readGskCache(e.agent, c);
			out.push_back(c);
		}
		return true;
	}

	bool readWelderQueryBox(uintptr_t agent, Vec3& mn, Vec3& mx)
	{
		const AgentKind k = agentKind(agent);
		if (k != AgentKind::Welder) return false;
		float b[8];
		if (!copy(b, agent + 0x30, sizeof(b))) return false;
		mn = { b[0], b[1], b[2] }; mx = { b[4], b[5], b[6] };
		for (int i = 0; i < 3; ++i)
			if (!std::isfinite(b[i]) || !std::isfinite(b[4 + i]) || b[4 + i] < b[i] || b[4 + i] - b[i] > 200.f) return false;
		return true;
	}

	float collisionTolerance()
	{
		const uintptr_t world = q(base() + rva::WORLD);
		const uintptr_t input = okp(world) ? q(world + 0x120) : 0;
		const float t = okp(input) ? f32(input + 0x10, 0.f) : 0.f;
		return (t > 0.f && t < 1.f) ? t : 0.f;
	}

	bool broadphaseAabb(uintptr_t entity, Vec3& mn, Vec3& mx)
	{
		const uintptr_t b = base();
		const uintptr_t world = q(b + rva::WORLD);
		const uintptr_t bp = okp(world) ? q(world + 0x110) : 0;
		if (!okp(bp) || q(bp) != b + 0xB764F0) return false;   // hk3AxisSweep
		const uintptr_t nodes = q(bp + 0x40);
		const int32_t n = i32(bp + 0x48, 0);
		if (!okp(nodes) || n <= 0 || n > 65535) return false;
		const uintptr_t handle = entity + 0x38;
		uint32_t idx = u32(handle, 0xFFFFFFFF);
		if (idx >= (uint32_t)n || q(nodes + 24 * (uintptr_t)idx + 0x10) != handle)
		{
			idx = 0xFFFFFFFF;
			std::vector<uint8_t> raw((size_t)n * 24);
			if (!copy(raw.data(), nodes, raw.size())) return false;
			for (int32_t i = 0; i < n; ++i)
				if (*(const uintptr_t*)&raw[(size_t)i * 24 + 0x10] == handle) { idx = (uint32_t)i; break; }
			if (idx == 0xFFFFFFFF) return false;
		}
		uint16_t nd[6];
		if (!copy(nd, nodes + 24 * (uintptr_t)idx, sizeof(nd))) return false;
		float off[3], sc[3];
		if (!copy(off, bp + 0x10, 12) || !copy(sc, bp + 0x30, 12)) return false;
		const uintptr_t ax[3] = { q(bp + 0x50), q(bp + 0x60), q(bp + 0x70) };
		auto ep = [&](int axis, uint16_t i) { return (float)u16(ax[axis] + 4 * (uintptr_t)i, 0) / sc[axis] - off[axis]; };
		for (int a = 0; a < 3; ++a) if (!okp(ax[a]) || !(std::fabs(sc[a]) > 1e-12f)) return false;
		mn = { ep(0, nd[4]), ep(1, nd[0]), ep(2, nd[1]) };
		mx = { ep(0, nd[5]), ep(1, nd[2]), ep(2, nd[3]) };
		return std::isfinite(mn.x) && std::isfinite(mx.z) && mx.x >= mn.x && mx.y >= mn.y && mx.z >= mn.z;
	}

	bool backstepParams(BackstepParams& p)
	{
		p = BackstepParams{};
		const uintptr_t world = q(base() + rva::WORLD);
		if (!okp(world)) return false;
		float v[3];
		if (!copy(v, world + 0x2C4, sizeof(v))) return false;
		for (float f : v) if (!std::isfinite(f) || std::fabs(f) > 10.f) return false;
		p.base = v[0]; p.perS = v[1]; p.timeRes = v[2]; p.ok = true;
		return true;
	}

	bool levelGrids(LevelGrids& g)
	{
		g = LevelGrids{};
		const uintptr_t b = base();
		g.tolerance = collisionTolerance();
		const uintptr_t world = q(b + rva::WORLD);
		const uintptr_t bp = okp(world) ? q(world + 0x110) : 0;
		float off[3], sc[3];
		if (okp(bp) && q(bp) == b + 0xB764F0 && copy(off, bp + 0x10, 12) && copy(sc, bp + 0x30, 12)
			&& sc[0] > 1e-9f && sc[1] > 1e-9f && sc[2] > 1e-9f)
		{
			g.bpMin = { -off[0], -off[1], -off[2] };
			g.bpMax = { -off[0] + 65535.f / sc[0], -off[1] + 65535.f / sc[1], -off[2] + 65535.f / sc[2] };
			g.bpCell = { 2.f / sc[0], 2.f / sc[1], 2.f / sc[2] };
			g.bpOk = true;
		}
		// the level shape: the structure-BSP body's shape, else the MOPP singleton (ctor sub_1807921C0 -> halo2+0x165B430)
		auto isMopp = [&](uintptr_t s)   // Halo's MOPP (0xB3C5B0), hkMoppBvTreeShape (0xB6E990) or its COMDAT copy (0xB1A1A8)
		{
			if (!okp(s)) return false;
			const uintptr_t v = q(s) - b;
			return v == 0xB3C5B0 || v == 0xB6E990 || v == 0xB1A1A8;
		};
		uintptr_t shape = 0;
		const uintptr_t bspEnt = q(b + rva::BSP_ENTITY);
		if (okp(bspEnt)) shape = q(bspEnt + 0x18);
		if (!isMopp(shape)) shape = q(b + 0x165B430);
		if (isMopp(shape))
		{
			const uintptr_t code = q(shape + 0x20);
			float info[4];
			if (okp(code) && copy(info, code, 16) && std::isfinite(info[3]) && info[3] > 1e-6f)
			{
				g.moppOrigin = { info[0], info[1], info[2] }; g.moppScale = info[3]; g.moppOk = true;
			}
		}
		return g.bpOk || g.moppOk;
	}

	bool readConvexAgent(uintptr_t agent, Child& c)
	{
		c = Child{};
		if (!okp(agent)) return false;
		const uintptr_t v = q(agent) - base();
		if (v == vt::SWEEP_AGENT) c.kind = Child::Sweep;
		else if (v == vt::GSK_AGENT) c.kind = Child::Gsk;
		else return false;
		c.agent = agent;
		return readGskCache(agent, c);
	}

	bool writeConvexState(uintptr_t agent, const Child& s)
	{
		Child live;
		if (!readConvexAgent(agent, live) || live.manifold > 0) return false;
		uint8_t cache[10];
		memcpy(cache, s.ids, 8); cache[8] = s.dimA; cache[9] = s.dimB;
		const float n[3] = { s.normal.x, s.normal.y, s.normal.z };
		return mem::put(agent + 0x18, cache, sizeof(cache)) && mem::put(agent + 0x30, n, sizeof(n)) && mem::put(agent + 0x3C, &s.tim, sizeof(float));
	}

	// ---- rigid transforms ---------------------------------------------------------------------------------------
	Vec3 xfPoint(const Xf& x, const Vec3& p)
	{
		return { x.c[0].x * p.x + x.c[1].x * p.y + x.c[2].x * p.z + x.t.x,
		         x.c[0].y * p.x + x.c[1].y * p.y + x.c[2].y * p.z + x.t.y,
		         x.c[0].z * p.x + x.c[1].z * p.y + x.c[2].z * p.z + x.t.z };
	}

	Vec3 xfDir(const Xf& x, const Vec3& d)
	{
		return { x.c[0].x * d.x + x.c[1].x * d.y + x.c[2].x * d.z,
		         x.c[0].y * d.x + x.c[1].y * d.y + x.c[2].y * d.z,
		         x.c[0].z * d.x + x.c[1].z * d.y + x.c[2].z * d.z };
	}

	Xf xfCompose(const Xf& a, const Xf& b)
	{
		Xf o;
		for (int i = 0; i < 3; ++i) o.c[i] = xfDir(a, b.c[i]);
		o.t = xfPoint(a, b.t);
		return o;
	}

	Xf motionPose(const Motion& m)
	{
		Xf x;
		for (int i = 0; i < 3; ++i) x.c[i] = m.rc[i];
		x.t = m.origin;
		return x;
	}

	bool entityPose(uintptr_t entity, Xf& out)
	{
		const uintptr_t motion = q(entity + 0x68);
		if (!okp(motion)) return false;
		float R[12], t[4];
		if (!copy(R, motion + 0x90, 48) || !copy(t, motion + 0xC0, 16)) return false;
		for (int i = 0; i < 3; ++i)
		{
			out.c[i] = { R[i * 4], R[i * 4 + 1], R[i * 4 + 2] };
			const float l2 = out.c[i].x * out.c[i].x + out.c[i].y * out.c[i].y + out.c[i].z * out.c[i].z;
			if (!std::isfinite(l2) || l2 < 0.8f || l2 > 1.2f) return false;
		}
		out.t = { t[0], t[1], t[2] };
		return std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]);
	}

	// ---- shape tree walker (vehicles) ---------------------------------------------------------------------------
	// Shape layouts (all offsets from the constructors, decompiled with Hex-Rays on halo2.dll 1.3528):
	//   hkShape        refcount u16 @+0x0A, user data @+0x10 ;  getType() = vtable slot 5 (+0x28), a `mov eax,imm32 / ret` leaf
	//   hkConvexShape  radius f32 @+0x18                       (sub_1809B0820 box, 1809B12D0 capsule, 1809B2440 convex, 1809B4410 sphere)
	//   sphere (4)     radius @+0x18, centre = leaf-local origin
	//   box (6)        half extents xyz @+0x20 (w = min), radius @+0x18; support point = he * sign(dir), GSK adds the radius
	//   capsule (7)    A xyz @+0x20, B xyz @+0x30, radius @+0x18 (= A.w = B.w)
	//   convex vertices (8)  vertices: 4-wide SoA blocks of 48 B {x[4], y[4], z[4]} at *(+0x40), count @+0xE0 (sub_1809B2990);
	//                        planes {n.xyz, d} 16 B at *(+0xE8), count @+0xF0; n.p + d < 0 inside (ray cast sub_1809B2620)
	//   multi-sphere (9)     count @+0x18, spheres {centre xyz, radius w} 16 B at +0x20 (sub_1809B3800)
	//   list (10)      child array @+0x18 (16 B entries, shape pointer first), count @+0x20 (sub_1809AF870; sub_1809A3950 reads them)
	//   transform (21) child @+0x18, hkTransform (3 rotation columns + translation, 16 B each) @+0x20 (sub_1809AFDD0)
	//   sweep (23)     child @+0x40 (sub_18098AA90)
	//   BV (17) / MOPP (19)  child @+0x18 (sub_18098BC50; the phmo post-process sub_180734350 stores the child there too)
	// Vtable -> type: phmo tags embed shapes whose vtables are COMDAT clones (0xB19DF0.. next to the 0xB70xxx originals), so
	// the type is read from the vtable's own getType leaf instead of comparing vtable addresses.
	namespace
	{
		constexpr int kMaxPrims = 160, kMaxDepth = 8;

		int shapeTypeOf(uintptr_t shape)
		{
			if (!okp(shape)) return -1;
			const uintptr_t b = base();
			const uintptr_t v = q(shape);
			if (v < b || v - b > 0x2000000) return -1;
			const uintptr_t fn = q(v + 0x28);
			if (fn < b || fn - b > 0x2000000) return -1;
			uint8_t c[6];
			if (!copy(c, fn, 6) || c[0] != 0xB8 || c[5] != 0xC3) return -1;     // mov eax, imm32 ; ret
			int32_t t; memcpy(&t, c + 1, 4);
			return t;
		}

		inline bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
		inline Vec3 vsub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline Vec3 vadd(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		inline Vec3 vmul(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		inline float vdot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline Vec3 vcross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline Vec3 vnorm(const Vec3& a) { const float l = std::sqrt(vdot(a, a)); return l > 1e-12f ? vmul(a, 1.f / l) : Vec3{ 0, 0, 1 }; }

		bool readXf(uintptr_t addr, Xf& out)
		{
			float m[16];
			if (!copy(m, addr, sizeof(m))) return false;
			for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e5f) return false;
			out.c[0] = { m[0], m[1], m[2] }; out.c[1] = { m[4], m[5], m[6] }; out.c[2] = { m[8], m[9], m[10] };
			out.t = { m[12], m[13], m[14] };
			for (const Vec3& c : out.c)           // rigid (or near-rigid) only: allow the odd scale wrapper, reject garbage
			{
				const float l2 = vdot(c, c);
				if (!(l2 > 0.01f && l2 < 100.f)) return false;
			}
			return true;
		}

		// unique edges + radius-grown wire vertices of a polytope (verts, faces with outward normals already set)
		void finishPoly(ShapePrim& p)
		{
			std::vector<uint32_t> keys;
			for (const auto& f : p.faces)
				for (size_t i = 0; i < f.ring.size(); ++i)
				{
					uint32_t u = f.ring[i], v = f.ring[(i + 1) % f.ring.size()];
					if (u > v) std::swap(u, v);
					if (u != v) keys.push_back((u << 16) | v);
				}
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			p.edges.clear();
			for (uint32_t k : keys) p.edges.push_back({ (uint16_t)(k >> 16), (uint16_t)(k & 0xFFFF) });
			p.wire = p.verts;
			if (!(p.r > 1e-5f)) return;
			// grow the hull by the convex radius: vertex v -> point on the three (or more) face planes pushed out by r,
			// least squares over the incident faces: (sum n n^T) d = r * sum n
			std::vector<std::vector<int>> inc(p.verts.size());
			for (size_t fi = 0; fi < p.faces.size(); ++fi)
				for (uint16_t vi : p.faces[fi].ring) if (vi < inc.size()) inc[vi].push_back((int)fi);
			for (size_t vi = 0; vi < p.verts.size(); ++vi)
			{
				double A[3][3] = {}, sum[3] = {};
				for (int fi : inc[vi])
				{
					const double n[3] = { p.faces[fi].n.x, p.faces[fi].n.y, p.faces[fi].n.z };
					for (int i = 0; i < 3; ++i) { sum[i] += n[i]; for (int j = 0; j < 3; ++j) A[i][j] += n[i] * n[j]; }
				}
				const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
				Vec3 d = vmul(vnorm(Vec3{ (float)sum[0], (float)sum[1], (float)sum[2] }), p.r);
				if (inc[vi].size() >= 3 && std::fabs(det) > 1e-3)
				{
					const double r[3] = { sum[0] * p.r, sum[1] * p.r, sum[2] * p.r };
					double X[3];
					for (int c = 0; c < 3; ++c)
					{
						double M[3][3]; memcpy(M, A, sizeof(M));
						for (int i = 0; i < 3; ++i) M[i][c] = r[i];
						X[c] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
					}
					const Vec3 dd{ (float)X[0], (float)X[1], (float)X[2] };
					if (finite3(dd) && std::sqrt(vdot(dd, dd)) <= 3.f * p.r) d = dd;
				}
				p.wire[vi] = vadd(p.verts[vi], d);
			}
		}

		// make every face normal point away from the polytope's centroid
		void orientFaces(ShapePrim& p)
		{
			Vec3 cen{ 0, 0, 0 };
			for (const Vec3& v : p.verts) cen = vadd(cen, v);
			cen = vmul(cen, 1.f / (float)std::max<size_t>(1, p.verts.size()));
			for (auto& f : p.faces)
			{
				Vec3 fc{ 0, 0, 0 };
				for (uint16_t vi : f.ring) fc = vadd(fc, p.verts[vi]);
				fc = vmul(fc, 1.f / (float)std::max<size_t>(1, f.ring.size()));
				if (vdot(f.n, vsub(fc, cen)) < 0) f.n = vmul(f.n, -1.f);
			}
		}

		struct Walker
		{
			BodyShape& out;
			int cnt[32] = {};
			explicit Walker(BodyShape& o) : out(o) {}

			void addCapsule(const Vec3& a, const Vec3& b, float r, const Xf& xf, int hkType, bool swept, uintptr_t agent)
			{
				if (out.prims.size() >= (size_t)kMaxPrims) return;
				ShapePrim p; p.capsule = true; p.hkType = hkType; p.swept = swept; p.agent = agent;
				p.a = xfPoint(xf, a); p.b = xfPoint(xf, b); p.r = r;
				if (!finite3(p.a) || !finite3(p.b)) { ++out.skipped; return; }
				out.prims.push_back(std::move(p));
			}

			void addPoly(std::vector<Vec3> lv, std::vector<ShapeFace> faces, float r, const Xf& xf, int hkType, bool swept, uintptr_t agent)
			{
				if (out.prims.size() >= (size_t)kMaxPrims || lv.size() < 3 || faces.empty()) { ++out.skipped; return; }
				ShapePrim p; p.capsule = false; p.hkType = hkType; p.swept = swept; p.agent = agent; p.r = r;
				p.verts.reserve(lv.size());
				for (const Vec3& v : lv) p.verts.push_back(xfPoint(xf, v));
				for (auto& f : faces) { f.n = vnorm(xfDir(xf, f.n)); p.faces.push_back(std::move(f)); }
				orientFaces(p);
				finishPoly(p);
				out.prims.push_back(std::move(p));
			}

			// planes (n.xyz, d; n.p + d = 0, outward n) -> vertex rings, by collecting the vertices lying on each plane
			bool convexFaces(const std::vector<Vec3>& lv, const std::vector<float>& planes, std::vector<ShapeFace>& faces)
			{
				float ext = 1e-4f;
				for (const Vec3& v : lv) ext = std::max({ ext, std::fabs(v.x), std::fabs(v.y), std::fabs(v.z) });
				const float eps = ext * 0.002f + 0.004f;
				std::vector<std::vector<uint16_t>> seen;
				for (size_t f = 0; f * 4 + 3 < planes.size(); ++f)
				{
					const Vec3 n{ planes[f * 4], planes[f * 4 + 1], planes[f * 4 + 2] };
					const float dp = planes[f * 4 + 3];
					const float nl = std::sqrt(vdot(n, n));
					if (!(nl > 0.5f && nl < 1.5f)) continue;
					std::vector<uint16_t> idx; Vec3 cen{ 0, 0, 0 };
					for (size_t k = 0; k < lv.size(); ++k)
					{
						const float d = vdot(n, lv[k]) + dp;
						if (d < eps && d > -eps) { idx.push_back((uint16_t)k); cen = vadd(cen, lv[k]); }
					}
					if (idx.size() < 3) continue;
					cen = vmul(cen, 1.f / (float)idx.size());
					const Vec3 h = std::fabs(n.x) < 0.9f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
					const Vec3 u = vnorm(vcross(n, h)), w = vcross(n, u);
					std::vector<std::pair<float, uint16_t>> ang;
					for (uint16_t k : idx) { const Vec3 d = vsub(lv[k], cen); ang.push_back({ std::atan2(vdot(d, w), vdot(d, u)), k }); }
					std::sort(ang.begin(), ang.end());
					ShapeFace fc; fc.n = vmul(n, 1.f / nl);
					for (auto& a : ang) fc.ring.push_back(a.second);
					std::vector<uint16_t> key = fc.ring; std::sort(key.begin(), key.end());
					bool dup = false;
					for (const auto& s2 : seen) if (s2 == key) { dup = true; break; }
					if (dup) continue;
					seen.push_back(std::move(key));
					faces.push_back(std::move(fc));
				}
				return !faces.empty();
			}

			void leaf(uintptr_t shape, int t, const Xf& xf, bool swept, uintptr_t agent)
			{
				++cnt[t & 31];
				switch (t)
				{
				case 4:   // sphere
				{
					const float r = f32(shape + 0x18, 0.f);
					if (r > 0.f && r < 100.f) addCapsule({ 0, 0, 0 }, { 0, 0, 0 }, r, xf, t, swept, agent); else ++out.skipped;
					break;
				}
				case 7:   // capsule
				{
					float a[4], b[4];
					if (!copy(a, shape + 0x20, 16) || !copy(b, shape + 0x30, 16)) { ++out.skipped; break; }
					float r = a[3];
					if (!(r > 0.001f && r < 100.f)) r = f32(shape + 0x18, 0.f);
					if (r > 0.001f && r < 100.f) addCapsule({ a[0], a[1], a[2] }, { b[0], b[1], b[2] }, r, xf, t, swept, agent); else ++out.skipped;
					break;
				}
				case 9:   // multi-sphere: every sphere is a degenerate capsule sharing the primitive's agent
				{
					const int n = i32(shape + 0x18, 0);
					if (n < 1 || n > 16) { ++out.skipped; break; }
					for (int i = 0; i < n; ++i)
					{
						float s4[4];
						if (!copy(s4, shape + 0x20 + 16 * (uintptr_t)i, 16) || !(s4[3] > 0.001f && s4[3] < 100.f)) continue;
						addCapsule({ s4[0], s4[1], s4[2] }, { s4[0], s4[1], s4[2] }, s4[3], xf, t, swept, agent);
					}
					break;
				}
				case 6:   // box
				{
					float he[3];
					if (!copy(he, shape + 0x20, 12)) { ++out.skipped; break; }
					float r = f32(shape + 0x18, 0.f); if (!(r >= 0.f && r < 5.f)) r = 0.f;
					for (float& h : he) if (!(h > 0.f && h < 1e4f)) { ++out.skipped; return; }
					std::vector<Vec3> lv;
					for (int i = 0; i < 8; ++i) lv.push_back({ (i & 1) ? he[0] : -he[0], (i & 2) ? he[1] : -he[1], (i & 4) ? he[2] : -he[2] });
					static const uint16_t F[6][4] = { {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5} };
					static const Vec3 N[6] = { {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0} };
					std::vector<ShapeFace> faces;
					for (int f = 0; f < 6; ++f) { ShapeFace fc; fc.ring.assign(F[f], F[f] + 4); fc.n = N[f]; faces.push_back(fc); }
					addPoly(std::move(lv), std::move(faces), r, xf, t, swept, agent);
					break;
				}
				case 8:   // convex vertices
				{
					const int nv = i32(shape + 0xE0, -1), np = i32(shape + 0xF0, -1);
					const uintptr_t vdata = q(shape + 0x40), pdata = q(shape + 0xE8);
					float r = f32(shape + 0x18, 0.f); if (!(r >= 0.f && r < 5.f)) r = 0.f;
					if (nv < 3 || nv > 256 || !okp(vdata)) { ++out.skipped; break; }
					const int nb = (nv + 3) / 4;
					std::vector<float> raw((size_t)nb * 12);
					if (!copy(raw.data(), vdata, raw.size() * 4)) { ++out.skipped; break; }
					std::vector<Vec3> lv((size_t)nv);
					for (int k = 0; k < nv; ++k)
					{
						const float* blk = &raw[(size_t)(k / 4) * 12];
						lv[(size_t)k] = { blk[k & 3], blk[4 + (k & 3)], blk[8 + (k & 3)] };
						if (!finite3(lv[(size_t)k])) { ++out.skipped; return; }
					}
					std::vector<ShapeFace> faces;
					if (np >= 1 && np <= 256 && okp(pdata))
					{
						std::vector<float> pl((size_t)np * 4);
						if (copy(pl.data(), pdata, pl.size() * 4)) convexFaces(lv, pl, faces);
					}
					if (faces.empty())   // no usable planes: fall back to the bounding box of the vertices
					{
						Vec3 mn = lv[0], mx = lv[0];
						for (const Vec3& v : lv) { mn = { std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z) }; mx = { std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z) }; }
						lv.clear();
						for (int i = 0; i < 8; ++i) lv.push_back({ (i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z });
						static const uint16_t F[6][4] = { {0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5} };
						static const Vec3 N[6] = { {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0} };
						for (int f = 0; f < 6; ++f) { ShapeFace fc; fc.ring.assign(F[f], F[f] + 4); fc.n = N[f]; faces.push_back(fc); }
					}
					addPoly(std::move(lv), std::move(faces), r, xf, t, swept, agent);
					break;
				}
				default: ++out.skipped; break;
				}
			}

			// Halo collision-bsp collection (vtable COLL_BSP_SHAPE, collision_bsp* @+0x18) -> one BspLeaf
			bool addBsp(uintptr_t coll, const Xf& xf, uintptr_t agent)
			{
				if (!okp(coll) || q(coll) != base() + vt::COLL_BSP_SHAPE) return false;
				const uintptr_t bsp = q(coll + 0x18);
				if (!okp(bsp) || out.bsps.size() >= 32) return false;
				for (const auto& l : out.bsps) if (l.agent == agent && l.bsp == bsp) return true;
				BspLeaf l; l.bsp = bsp; l.agent = agent; l.xf = xf;
				out.bsps.push_back(l);
				return true;
			}

			void walk(uintptr_t shape, uintptr_t agent, const Xf& xf, int depth, bool swept)
			{
				if (depth > kMaxDepth || !okp(shape) || out.prims.size() >= (size_t)kMaxPrims) return;
				const int t = shapeTypeOf(shape);
				const AgentKind ak = agentKind(agent);
				switch (t)
				{
				case 23: ++cnt[23]; walk(q(shape + 0x40), agent, xf, depth + 1, true); break;
				case 21:
				{
					Xf T;
					if (!readXf(shape + 0x20, T)) { ++out.skipped; break; }
					++cnt[21];
					walk(q(shape + 0x18), ak == AgentKind::Transform ? q(agent + 0x18) : 0, xfCompose(xf, T), depth + 1, swept);
					break;
				}
				case 10:
				{
					const int n = i32(shape + 0x20, -1);
					const uintptr_t arr = q(shape + 0x18);
					if (n < 1 || n > 256 || !okp(arr)) { ++out.skipped; break; }
					++cnt[10];
					uintptr_t aarr = 0; int an = 0;
					if (ak == AgentKind::List) { aarr = q(agent + 0x18); an = i32(agent + 0x20, 0); if (!okp(aarr)) an = 0; }
					// a MOPP over the list (sub_1809AAC20): children {key, pad, agent} 16 B, key = the list child index
					// (hkListShape::getChildShape(key) = list[key], sub_1809AFAA0)
					struct MoppEnt { uint32_t key, pad; uintptr_t agent; };
					std::vector<MoppEnt> ments;
					if (ak == AgentKind::Mopp)
					{
						const uintptr_t marr = q(agent + 0x18); const int mn = i32(agent + 0x20, 0);
						if (okp(marr) && mn > 0 && mn <= 512) { ments.resize((size_t)mn); if (!copy(ments.data(), marr, ments.size() * sizeof(MoppEnt))) ments.clear(); }
					}
					for (int i = 0; i < n; ++i)
					{
						uintptr_t ca = 0;
						if (ak == AgentKind::List) ca = i < an ? q(aarr + 8 * (uintptr_t)i) : 0;
						else for (const auto& e : ments) if (e.key == (uint32_t)i) { ca = e.agent; break; }
						walk(q(arr + 16 * (uintptr_t)i), ca, xf, depth + 1, swept);
					}
					break;
				}
				case 19:
					++cnt[t];
					// a welder agent collides the convex body against the MOPP's whole collection itself (the level does
					// this); its children are keyed by the collection's keys
					// (a MOPP / BV-tree agent over the same collection keeps the same {key, agent} children)
					if ((ak == AgentKind::Welder || ak == AgentKind::Mopp) && addBsp(q(shape + 0x18), xf, agent)) break;
					walk(q(shape + 0x18), ak == AgentKind::Mopp ? agent : 0, xf, depth + 1, swept);   // MOPP agent keys the list below
					break;
				case 17: ++cnt[t]; walk(q(shape + 0x18), 0, xf, depth + 1, swept); break;
				case 20:   // hkConvexWelderShape: +0x18 = the MOPP over the collision-bsp collection (sub_18077AFC0)
					++cnt[20];
					walk(q(shape + 0x18), agent, xf, depth + 1, swept);
					break;
				case 2:    // shape collection: only Halo's collision-bsp collection is understood (through a welder agent)
					++cnt[2];
					if (!((ak == AgentKind::Welder || ak == AgentKind::Mopp) && addBsp(shape, xf, agent))) ++out.skipped;
					break;
				case 4: case 6: case 7: case 8: case 9: leaf(shape, t, xf, swept, agent); break;
				default: ++cnt[t >= 0 && t < 32 ? t : 31]; ++out.skipped; break;   // triangle (5), mesh (11), welder (20), unknown
				}
			}
		};
	}

	bool readBodyShape(uintptr_t entity, uintptr_t rootAgent, BodyShape& out)
	{
		out = BodyShape{};
		const uintptr_t shape = q(entity + 0x18);
		if (!okp(shape)) return false;
		out.root = shape; out.rootType = shapeTypeOf(shape);
		Walker w(out);
		w.walk(shape, rootAgent, Xf{}, 0, false);
		char buf[256];
		int n = snprintf(buf, sizeof(buf), "root type %d", out.rootType);
		static const struct { int t; const char* nm; } names[] = { {23, "sweep"}, {21, "transform"}, {10, "list"}, {19, "mopp"}, {17, "bv"}, {4, "sphere"}, {6, "box"}, {7, "capsule"}, {8, "convex"}, {9, "multisphere"} };
		for (const auto& nm : names)
			if (w.cnt[nm.t] && n > 0 && n < (int)sizeof(buf) - 24) n += snprintf(buf + n, sizeof(buf) - n, ", %d %s", w.cnt[nm.t], nm.nm);
		if (out.skipped && n > 0 && n < (int)sizeof(buf) - 24) snprintf(buf + n, sizeof(buf) - n, ", %d skipped", out.skipped);
		out.desc = buf;
		return !out.prims.empty();
	}

	bool readShapeTree(uintptr_t shape, uintptr_t rootAgent, const Xf& xf, BodyShape& out)
	{
		out = BodyShape{};
		if (!okp(shape)) return false;
		out.root = shape; out.rootType = shapeTypeOf(shape);
		Walker w(out);
		w.walk(shape, rootAgent, xf, 0, false);
		char buf[256];
		int n = snprintf(buf, sizeof(buf), "root type %d", out.rootType);
		static const struct { int t; const char* nm; } names[] = { {21, "transform"}, {10, "list"}, {19, "mopp"}, {20, "welder"}, {2, "collection"}, {4, "sphere"}, {6, "box"}, {7, "capsule"}, {8, "convex"}, {9, "multisphere"} };
		for (const auto& nm : names)
			if (w.cnt[nm.t] && n > 0 && n < (int)sizeof(buf) - 24) n += snprintf(buf + n, sizeof(buf) - n, ", %d %s", w.cnt[nm.t], nm.nm);
		if (out.skipped && n > 0 && n < (int)sizeof(buf) - 24) snprintf(buf + n, sizeof(buf) - n, ", %d skipped", out.skipped);
		out.desc = buf;
		return !out.prims.empty() || !out.bsps.empty();
	}

	// ---- baked scenery (level-shape key categories 3 / 4) ---------------------------------------------------------
	// Port of sub_180793760 (halo2.dll 1.3528). Key: low 16 = scenery index, (key >> 16) & 0x1F = model region,
	// (key >> 21) & 0xFF = model permutation, key >> 29 = 3 (physics model) / 4 (collision model).
	//   datum   = u32[*(halo2+0x165B440)][index]; object definition tag = u16 @obj+0
	//   tag data = *(halo2+0x15E4B38) + i32 @(*(halo2+0x15E4B30) + 16 * tagIndex + 8)
	//   hlmt     = object definition +0x38 (tag index); hlmt regions {count +0x70, offset +0x74} stride 16:
	//              collision region i8 +4, physics region i8 +5, permutations {count +8, offset +0xC} stride 8:
	//              collision permutation i8 +5, physics permutation i8 +6
	//   cat 4:   coll = hlmt +0x0C; coll regions {+0x1C, +0x20} stride 12 -> permutations {+4, +8} stride 20 ->
	//            bsp physics {+0x0C, +0x10}; shape = first element + 0x50 (hkConvexWelderShape); transform = node 0
	//   cat 3:   phmo = hlmt +0x24; phmo regions {+0xC0, +0xC4} stride 12 -> permutations {+4, +8} stride 12 ->
	//            rigid body indices {+4, +8} (first u16); rigid bodies {+0x38, +0x3C} stride 0xA0: node i16 +0, shape* +0x38;
	//            transform = that node's matrix (node -1 = the object root; node 0 is used for it here)
	namespace
	{
		uintptr_t tagInstanceData(uint32_t tagIndex)
		{
			if ((tagIndex & 0xFFFF) == 0xFFFF) return 0;
			const uintptr_t b = base();
			const uintptr_t arr = q(b + rva::TAG_INSTANCES), dataBase = q(b + rva::TAG_DATA_BASE);
			if (!okp(arr) || !okp(dataBase)) return 0;
			const int32_t off = i32(arr + 16 * (uintptr_t)(tagIndex & 0xFFFF) + 8, -1);
			if (off == -1) return 0;
			const uintptr_t d = dataBase + (intptr_t)off;
			return okp(d) ? d : 0;
		}

		// element `index` of a tag block {u32 count @blk, i32 offset @blk+4}; 0 when out of range / unreadable
		uintptr_t blockElem(uintptr_t blk, int index, size_t stride, const TagPools& p)
		{
			uintptr_t first = 0; uint32_t count = 0;
			if (index < 0 || !resolveBlock(blk, p, first, count) || (uint32_t)index >= count || count > 100000) return 0;
			return first + (uintptr_t)index * stride;
		}
	}

	bool bakedSceneryShape(uint32_t key, BakedShape& out)
	{
		out = BakedShape{};
		out.category = (int)(key >> 29);
		if (out.category != 3 && out.category != 4) { out.why = "not a scenery key"; return false; }
		const uintptr_t b = base();
		TagPools p;
		if (!tagPools(p)) { out.why = "tag pools"; return false; }
		const uintptr_t table = q(b + rva::SCENERY_TABLE);
		if (!okp(table)) { out.why = "scenery table"; return false; }
		const uint32_t index = key & 0xFFFF, region = (key >> 16) & 0x1F, perm = (key >> 21) & 0xFF;
		out.datum = u32(table + 4 * (uintptr_t)index);
		const uintptr_t entry = datumGet(q(b + rva::OBJ_HEADER), out.datum, true);
		out.obj = entry ? objectFromDatum(out.datum) : 0;
		if (!out.obj) { out.why = "object gone"; return false; }
		out.type = u8(entry + 3, 0xFF);
		const uintptr_t def = tagInstanceData(u16(out.obj, 0xFFFF));
		const uintptr_t hlmt = def ? tagInstanceData(u16(def + 0x38, 0xFFFF)) : 0;
		if (!hlmt) { out.why = "no model tag"; return false; }
		const uintptr_t mreg = blockElem(hlmt + 0x70, (int)region, 16, p);
		const uintptr_t mperm = mreg ? blockElem(mreg + 0x08, (int)perm, 8, p) : 0;
		if (!mperm) { out.why = "model region / permutation out of range"; return false; }
		int node = 0;
		if (out.category == 4)
		{
			const uintptr_t coll = tagInstanceData(u16(hlmt + 0x0C, 0xFFFF));
			const uintptr_t creg = coll ? blockElem(coll + 0x1C, (int)(int8_t)u8(mreg + 4, 0xFF), 12, p) : 0;
			const uintptr_t cperm = creg ? blockElem(creg + 0x04, (int)(int8_t)u8(mperm + 5, 0xFF), 20, p) : 0;
			const uintptr_t phys = cperm ? blockElem(cperm + 0x0C, 0, 1, p) : 0;
			if (!phys) { out.why = "collision model bsp physics missing"; return false; }
			out.shape = phys + 0x50;
		}
		else
		{
			const uintptr_t phmo = tagInstanceData(u16(hlmt + 0x24, 0xFFFF));
			const uintptr_t preg = phmo ? blockElem(phmo + 0xC0, (int)(int8_t)u8(mreg + 5, 0xFF), 12, p) : 0;
			const uintptr_t pperm = preg ? blockElem(preg + 0x04, (int)(int8_t)u8(mperm + 6, 0xFF), 12, p) : 0;
			const uintptr_t rbl = pperm ? blockElem(pperm + 0x04, 0, 2, p) : 0;
			const uintptr_t rb = rbl && phmo ? blockElem(phmo + 0x38, (int)(int16_t)u16(rbl, 0xFFFF), 0xA0, p) : 0;
			if (!rb) { out.why = "physics model rigid body missing"; return false; }
			out.shape = q(rb + 0x38);
			node = (int16_t)u16(rb, 0);
			if (node < 0) node = 0;
		}
		if (!okp(out.shape)) { out.why = "shape pointer"; return false; }
		float m[13];
		if (!objectNodeMatrix(out.obj, (uint16_t)node, m) && !objectNodeMatrix(out.obj, 0, m)) { out.why = "node matrix"; return false; }
		for (float f : m) if (!std::isfinite(f)) { out.why = "node matrix"; return false; }
		out.xf.c[0] = { m[1], m[2], m[3] }; out.xf.c[1] = { m[4], m[5], m[6] }; out.xf.c[2] = { m[7], m[8], m[9] };
		out.xf.t = { m[10], m[11], m[12] };
		return true;
	}
}
