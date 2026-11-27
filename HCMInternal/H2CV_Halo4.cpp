#include "pch.h"
#include "H2CV_Halo4.h"
#include "H2CV_Reach.h"     // the shared collision_bsp parser and seam-face rule
#include "H2CV_Mem.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

// See H2CV_Halo4.h. Every address is a halo4.dll 1.3528 RVA, found in the halo4.dll IDB and confirmed in a live session
// (2026-11-21, user's Halo 4 map with 2 structure bsps):
//   resource getter sub_180044424 (Reach's sub_180014124 twin), structure body builder sub_1800EE43C, bsp set-up sub_1800ED48C.
namespace h4
{
	namespace
	{
		constexpr uint32_t kStamp = 0x68A0E7BF;
		constexpr uintptr_t kScenarioPtr = 0x4967A30;    // q = scenario tag data
		constexpr uintptr_t kTagTablePtr = 0x107C0B0;    // q = tag instances, 8 B each, u32 tag address at +4
		constexpr uintptr_t kTagSegments = 0x496A180;    // 16 qwords: segmented tag-address bases (base[a >> 28] + 4a)
		constexpr uintptr_t kResGlobal = 0xED4F00;       // the resource table root (sub_180044424)
		constexpr uintptr_t kBspMask = 0x4967A18;
		constexpr uintptr_t kTick = 0x487C6F0;
		constexpr uintptr_t kLevelCode = 0x2AFF89F;
		constexpr uintptr_t kTlsIndex = 0x1057218;
		constexpr uint32_t kTlsView = 0x270;
		constexpr uintptr_t kPlayerDatumPtr = 0x4969D40;  // q + 0xA4 = the player's biped datum
		constexpr uintptr_t kObjectsPtr = 0x27C5F70;      // q + 0x1589E0 = object headers (0x18 each: salt u16, object q @+0x10)
		constexpr uintptr_t kObjectsOff = 0x1589E0;
		constexpr uintptr_t kCameraOff = 0x47BED4;        // q(kObjectsPtr) + this = the camera record
		constexpr uint32_t kScenarioBspCount = 0xA4, kScenarioBsps = 0xA8, kScenarioBspStride = 0x150, kScenarioBspTag = 12;
		constexpr uint32_t kSbspResource = 0x588;         // structure_bsp_tag_resources: +0 collision bsp, +0xC large, +0x24 Havok
		constexpr uint32_t kSbspMaterials = 0x78, kMaterialStride = 0x1C, kMaterialGlobal = 0x14;
		constexpr uint8_t kFlagInvalid = 0x10;

		uintptr_t sCheckedBase = 0;
		bool sCheckedOk = false;

		float len(const h2::Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

		uintptr_t tagAddr(uintptr_t b, uint32_t a)
		{
			if (!a) return 0;
			const uintptr_t seg = mem::q(b + kTagSegments + 8 * (uintptr_t)(a >> 28));
			return mem::okp(seg) ? seg + 4ull * a : 0;
		}

		// halo4!sub_180044424 from outside: inline (u32 h+0x1C == 0: the tag address at h+4), else the per-thread view table -
		// the given thread's view first, then the first view that resolves (only some views hold the level; live: view 2)
		uintptr_t resource(uintptr_t b, uintptr_t tls, uintptr_t handle)
		{
			if (mem::u32(handle + 0x1C, 0) == 0) return tagAddr(b, mem::u32(handle + 4, 0));
			const uintptr_t r = mem::q(b + kResGlobal);
			const uintptr_t tab = (r + 0x460C0 == 0) ? 0x108 : r + 0x460C0 + 0x8758;
			const uintptr_t t0 = mem::q(tab);
			const uintptr_t index = mem::u16(handle + 0xC, 0);
			auto inView = [&](int32_t view) -> uintptr_t
				{
					if (view < 0 || view > 64 || !mem::okp(t0)) return 0;
					const uintptr_t t1 = mem::q(t0 + 8 * (uintptr_t)view);
					const uintptr_t t2 = mem::okp(t1) ? mem::q(t1 + 0x18) : 0;
					return mem::okp(t2) ? mem::q(t2 + 8 * index) : 0;
				};
			const int32_t mine = mem::okp(tls) ? mem::i32(tls + kTlsView, -1) : -1;
			if (const uintptr_t x = inView(mine)) return x;
			for (int32_t view = 0; view < 16; ++view)
				if (view != mine)
					if (const uintptr_t x = inView(view)) return x;
			return 0;
		}
	}

	uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"halo4.dll")); }

	bool buildOk()
	{
		const uintptr_t b = base();
		if (!b) return false;
		if (b == sCheckedBase) return sCheckedOk;
		sCheckedBase = b;
		const uint32_t pe = mem::u32(b + 0x3C, 0);
		sCheckedOk = pe && mem::u32(b + pe + 8, 0) == kStamp;
		return sCheckedOk;
	}

	// The RENDER camera record (2026-11-21, the ODST / Reach route - found live: halo4+0x10BEFCC, Reach's layout): position, forward
	// +0xC, up +0x18, VERTICAL field of view +0x28 (1.1451 with MCC's FOV 100 = horizontal 100 at 16:9 -> ~66 vertical), viewport
	// i16[4] +0x38 (top, left, bottom, right), near / far +0x5C / +0x60. The player camera record below (readCamera(aspect)) was
	// 0.18 wu off the eye and its field of view is not the drawn one: lines came out off on the user's 21:9 either way.
	h2::Camera readCamera()
	{
		h2::Camera c;
		const uintptr_t b = base();
		if (!b || !buildOk()) return c;
		// three copies of the record (live 2026-11-21): 0x10BEFCC stops updating while zoomed (a scope), 0x10BEE80 then carries
		// the zoomed view (once seen with fov 0.786). Use whichever changed most recently; 0x10BEFCC on a tie.
		static const uintptr_t kRecords[3] = { 0x10BEFCC, 0x10BEE80, 0x10DAFE0 };
		static float s_last[3][11] = {};
		static uint64_t s_changed[3] = {};
		static uint64_t s_frame = 0;
		++s_frame;
		uintptr_t kRenderCamera = kRecords[0];
		uint64_t best = 0;
		for (int i = 0; i < 3; ++i)
		{
			float r[11];
			if (!mem::copy(r, b + kRecords[i], sizeof(r))) continue;
			if (memcmp(r, s_last[i], sizeof(r)) != 0) { memcpy(s_last[i], r, sizeof(r)); s_changed[i] = s_frame; }
			if (s_changed[i] > best) { best = s_changed[i]; kRenderCamera = kRecords[i]; }
		}
		float v[11];
		if (!mem::copy(v, b + kRenderCamera, sizeof(v))) return c;
		for (float x : v) if (!std::isfinite(x)) return c;
		c.pos = { v[0], v[1], v[2] }; c.fwd = { v[3], v[4], v[5] }; c.up = { v[6], v[7], v[8] };
		c.vfov = v[10];                                        // +0x28
		c.nearClip = mem::f32(b + kRenderCamera + 0x5C, 0.f);
		c.farClip = mem::f32(b + kRenderCamera + 0x60, 0.f);
		if (std::fabs(len(c.fwd) - 1.f) > 1e-2f || std::fabs(len(c.up) - 1.f) > 1e-2f) return c;
		if (!(c.vfov > 0.05f && c.vfov < 2.9f)) return c;
		if (!(c.nearClip > 1e-5f && c.nearClip < 1.f)) c.nearClip = 1.f / 128.f;
		if (!(c.farClip > c.nearClip)) c.farClip = 10240.f;
		c.ok = true;
		return c;
	}

	h2::Camera readCamera(float aspect)
	{
		h2::Camera c;
		const uintptr_t b = base();
		if (!b || !buildOk()) return c;
		const uintptr_t g = mem::q(b + kObjectsPtr);
		if (!mem::okp(g)) return c;
		const uintptr_t cam = g + kCameraOff;
		float p[3], f[3], u[3];
		if (!mem::copy(p, cam, 12) || !mem::copy(f, cam + 0x28, 12) || !mem::copy(u, cam + 0x34, 12)) return c;
		for (float x : p) if (!std::isfinite(x)) return c;
		c.pos = { p[0], p[1], p[2] }; c.fwd = { f[0], f[1], f[2] }; c.up = { u[0], u[1], u[2] };
		if (std::fabs(len(c.fwd) - 1.f) > 1e-2f || std::fabs(len(c.up) - 1.f) > 1e-2f) return c;
		const float hfov = mem::f32(cam + 0x78, 0.f);
		if (!(hfov > 0.2f && hfov < 3.0f)) return c;
		// the field of view is HORIZONTAL across the actual window (2026-11-21, user on 21:9: taking it as 16:9's squeezed every
		// line toward the crosshair, as ODST's observer fov once did): vertical = 2 atan(tan(h / 2) / aspect)
		if (!(aspect > 0.5f && aspect < 6.f)) aspect = 16.f / 9.f;
		c.vfov = 2.f * std::atan(std::tan(hfov * 0.5f) / aspect);
		c.nearClip = 1.f / 128.f; c.farClip = 10240.f;
		c.ok = true;
		return c;
	}

	uint32_t gameTick() { const uintptr_t b = base(); return (b && buildOk()) ? mem::u32(b + kTick, 0) : 0; }
	uint32_t activeBspMask() { const uintptr_t b = base(); return (b && buildOk()) ? mem::u32(b + kBspMask, 0) : 0; }

	std::string mapName()
	{
		const uintptr_t b = base();
		if (!b || !buildOk()) return "";
		char raw[129] = {};
		if (!mem::copy(raw, b + kLevelCode, 128)) return "";
		std::string path(raw);
		const size_t cut = path.find_last_of("\\/");
		std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
		if (name.empty() || name.size() > 64) return "";
		for (char& c : name)
		{
			if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return "";
			c = (char)std::tolower((unsigned char)c);
		}
		for (char c : name) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return "";
		return name;
	}

	namespace
	{
		uintptr_t playerObject()
		{
			const uintptr_t b = base();
			if (!b || !buildOk()) return 0;
			const uintptr_t pd = mem::q(b + kPlayerDatumPtr);
			if (!mem::okp(pd)) return 0;
			const uint32_t datum = mem::u32(pd + 0xA4, 0xFFFFFFFFu);
			if (datum == 0xFFFFFFFFu) return 0;
			const uintptr_t g = mem::q(b + kObjectsPtr);
			if (!mem::okp(g)) return 0;
			const uintptr_t entry = g + kObjectsOff + 0x18 * (uintptr_t)(datum & 0xFFFF);
			if (mem::u16(entry, 0) != (uint16_t)(datum >> 16)) return 0;
			const uintptr_t obj = mem::q(entry + 0x10);
			return mem::okp(obj) ? obj : 0;
		}
	}

	bool readPlayer(h2::Vec3& feet, h2::Vec3& vel, bool* inVehicle)
	{
		const uintptr_t obj = playerObject();
		float p[3], v[3];
		if (!obj || !mem::copy(p, obj + 0x64, 12) || !mem::copy(v, obj + 0x88, 12)) return false;
		for (int i = 0; i < 3; ++i) if (!std::isfinite(p[i]) || !std::isfinite(v[i])) return false;
		feet = { p[0], p[1], p[2] - kFeetDrop };
		vel = { v[0], v[1], v[2] };
		if (inVehicle) *inVehicle = mem::u32(obj + 0x24, 0xFFFFFFFFu) != 0xFFFFFFFFu;   // bipedDataFields currentVehicleDatum
		return true;
	}

	uint32_t playerDatum()
	{
		const uintptr_t b = base();
		if (!b || !buildOk()) return 0xFFFFFFFFu;
		const uintptr_t pd = mem::q(b + kPlayerDatumPtr);
		return mem::okp(pd) ? mem::u32(pd + 0xA4, 0xFFFFFFFFu) : 0xFFFFFFFFu;
	}

	float crouchAmount()
	{
		const uintptr_t obj = playerObject();
		if (!obj) return -1.f;
		const float c = mem::f32(obj + 0x7E4, -1.f);
		return std::isfinite(c) && c >= -0.01f && c <= 1.01f ? c : -1.f;
	}

	bool writePlayerVelocity(const h2::Vec3& vel)
	{
		const uintptr_t obj = playerObject();
		if (!obj) return false;
		const float v[3] = { vel.x, vel.y, vel.z };
		// HCM's Force Launch path for Halo 4 (GetObjectPhysicsHavokVisualObject, live-checked 2026-11-21): the Havok anchor of the
		// object's havok component - object +0xF0 datum -> table [halo4+0x10BE960] + u32 [its +0x60], 0xE0 per entry -> anchor
		// q(q(component +0x30) +0x40) +0x1C0, velocity anchor +0x70. Writing only the biped's +0x88 (h4_bounce_55) did nothing:
		// Halo 4 copies the body's velocity over it.
		bool ok = false;
		const uint32_t hc = mem::u32(obj + 0xF0, 0xFFFFFFFFu);
		const uintptr_t hdr = mem::q(base() + 0x10BE960);
		if (hc != 0xFFFFFFFFu && mem::okp(hdr))
		{
			const uintptr_t comp = hdr + mem::u32(hdr + 0x60, 0) + 0xE0 * (uintptr_t)(hc & 0xFFFF);
			const uintptr_t a1 = mem::q(comp + 0x30);
			const uintptr_t a2 = mem::okp(a1) ? mem::q(a1 + 0x40) : 0;
			if (mem::okp(a2)) ok = mem::put(a2 + 0x1C0 + 0x70, v, sizeof(v));
		}
		mem::put(obj + 0x88, v, sizeof(v));
		return ok;
	}

	uintptr_t gameTls()
	{
		const uintptr_t b = base();
		if (!b) return 0;
		const uint32_t index = mem::u32(b + kTlsIndex, 0xFFFFFFFF);
		const uintptr_t slots = (uintptr_t)__readgsqword(0x58);   // TEB.ThreadLocalStoragePointer
		if (index >= 1088 || !slots) return 0;
		return mem::q(slots + 8 * (uintptr_t)index);              // resource() tries the other views when this one has no level
	}

	bool readCollisionBsps(uintptr_t tls, uint32_t mask, std::vector<h2cv::CollisionWorld::Bsp3>& out, std::string& log, BspStats* stats)
	{
		out.clear();
		BspStats st;
		const uintptr_t b = base();
		if (!b || !buildOk()) { log = "no halo4.dll 1.3528"; return false; }
		char line[400];
		const uintptr_t scenario = mem::q(b + kScenarioPtr), tags = mem::q(b + kTagTablePtr);
		const uint32_t count = mem::u32(scenario + kScenarioBspCount, 0), arrAddr = mem::u32(scenario + kScenarioBsps, 0);
		snprintf(line, sizeof(line), "scenario %llX tags %llX: %u structure bsps, mask %08X, TLS %llX\n", (unsigned long long)scenario,
			(unsigned long long)tags, count, mask, (unsigned long long)tls);
		log = line;
		if (!mem::okp(scenario) || !mem::okp(tags) || count == 0 || count > 32) return false;
		const uintptr_t bspArr = tagAddr(b, arrAddr);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (mask && !(mask & (1u << i))) continue;
			const uint16_t tagIndex = mem::u16(bspArr + (uintptr_t)i * kScenarioBspStride + kScenarioBspTag, 0xFFFF);
			if (tagIndex == 0xFFFF) continue;
			const uintptr_t sbsp = tagAddr(b, mem::u32(tags + 8 * (uintptr_t)tagIndex + 4, 0));
			if (!mem::okp(sbsp)) continue;
			const uintptr_t res = resource(b, tls, sbsp + kSbspResource);
			if (!mem::okp(res)) { log += "bsp " + std::to_string(i) + ": resource did not resolve\n"; continue; }
			const uint32_t smallN = mem::u32(res + 0x00, 0), smallA = mem::u32(res + 0x04, 0), largeN = mem::u32(res + 0x0C, 0), largeA = mem::u32(res + 0x10, 0);
			h2cv::CollisionWorld::Bsp3 b3;
			b3.index = (int)i; b3.sbsp = sbsp;
			if (smallN == 1 && reach::parseCollisionBspSegmented(b, kTagSegments, tagAddr(b, smallA), false, b3.world)) b3.how = "small (16-bit) collision bsp";
			else if (largeN == 1 && reach::parseCollisionBspSegmented(b, kTagSegments, tagAddr(b, largeA), true, b3.world)) b3.how = "large (32-bit) collision bsp";
			else
			{
				snprintf(line, sizeof(line), "bsp %u: tag %u sbsp %llX res %llX (small %u large %u): the collision bsp did not parse\n", i, tagIndex, (unsigned long long)sbsp, (unsigned long long)res, smallN, largeN);
				log += line;
				continue;
			}
			// non-collidable collision materials (sbsp +0x78, stride 0x1C, global material index i16 @+0x14 < 0), guarded like Reach's
			size_t dropped = 0; const char* matNote = "";
			{
				const uint32_t mn = mem::u32(sbsp + kSbspMaterials, 0), ma = mem::u32(sbsp + kSbspMaterials + 4, 0);
				std::vector<uint8_t> mraw;
				if (mn && mn <= 65535) { mraw.resize((size_t)mn * kMaterialStride); if (!mem::copy(mraw.data(), tagAddr(b, ma), mraw.size())) mraw.clear(); }
				if (!mraw.empty())
				{
					std::vector<uint8_t> badRow(mn, 0);
					size_t badRows = 0;
					for (uint32_t m = 0; m < mn; ++m) { int16_t g; memcpy(&g, &mraw[(size_t)m * kMaterialStride + kMaterialGlobal], 2); if (g < 0) { badRow[m] = 1; ++badRows; } }
					size_t would = 0;
					for (const auto& s : b3.world.surfaces) if (s.material < mn && badRow[s.material] && !(s.flags & kFlagInvalid)) ++would;
					if (would * 4 > b3.world.surfaces.size() || badRows * 2 > mn) matNote = " (non-collidable fix-up SKIPPED: implausibly many)";
					else
						for (auto& s : b3.world.surfaces) if (s.material < mn && badRow[s.material] && !(s.flags & kFlagInvalid)) { s.flags |= kFlagInvalid; ++dropped; }
				}
				else matNote = " (materials unreadable: nothing left out)";
			}
			st.bsps++; st.surfaces += b3.world.surfaces.size(); st.nonCollidable += dropped;
			snprintf(line, sizeof(line), "bsp %u: tag %u sbsp %llX res %llX, %s: %zu surfaces %zu edges %zu verts; %zu non-collidable material surfaces left out%s; instanced geometry not read (Havok only in Halo 4)\n",
				i, tagIndex, (unsigned long long)sbsp, (unsigned long long)res, b3.how.c_str(), b3.world.surfaces.size(), b3.world.edges.size(), b3.world.verts.size(), dropped, matNote);
			log += line;
			out.push_back(std::move(b3));
		}
		std::vector<size_t> seams;
		st.seamLeftOut = reach::markSeamFacesBetweenBsps(out, seams);
		for (size_t i = 0; i < out.size(); ++i)
		{
			snprintf(line, sizeof(line), "bsp %d: %zu seam faces (borders with other loaded BSPs) left out\n", out[i].index, seams[i]);
			log += line;
		}
		if (stats) *stats = st;
		return !out.empty();
	}

	// ---- instanced geometry from the Havok world (see H2CV_Halo4.h) ----
	namespace
	{
		constexpr uintptr_t kHavokWorldPtr = 0x4968CB0;   // q = hkpWorld (rcx of game_tick's step call)
		constexpr uintptr_t kBspBodies = 0xED7A20;        // q[32 * 5]: the structure bsps' fixed rigid bodies (sub_1800EE43C)
		constexpr uintptr_t kHavokComponents = 0x10BE960; // q = the Havok component table (object +0xF0 datum, 0xE0 each)
		constexpr int kKeysPerTick = 2500;                // shape keys walked per game tick
		constexpr size_t kMaxTris = 1500000;

		// the four calls, each in its own SEH frame (no objects to unwind)
		int sehContainer(uintptr_t shape, uintptr_t* out)
		{
			__try
			{
				const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(shape);
				*out = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vt + 0x70))(shape);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		int sehFirstKey(uintptr_t c, uint32_t* key)
		{
			__try
			{
				const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
				*key = reinterpret_cast<uint32_t(__fastcall*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vt + 0x10))(c);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		int sehNextKey(uintptr_t c, uint32_t key, uint32_t* next)
		{
			__try
			{
				const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
				*next = reinterpret_cast<uint32_t(__fastcall*)(uintptr_t, uint32_t)>(*reinterpret_cast<const uintptr_t*>(vt + 0x18))(c, key);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// 1 a triangle (tri = its 3 vertices), 0 another kind of shape (*type), -1 a fault
		int sehChildTriangle(uintptr_t c, uint32_t key, void* buffer, float* tri, uint8_t* type)
		{
			__try
			{
				const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(c);
				const uintptr_t s = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uint32_t, void*)>(*reinterpret_cast<const uintptr_t*>(vt + 0x28))(c, key, buffer);
				if (!s) { *type = 0xFF; return 0; }
				*type = *reinterpret_cast<const uint8_t*>(s + 0x10);
				if (*type != 2) return 0;                    // hkcdShapeType TRIANGLE: m_vertexA / B / C at +0x30 / +0x40 / +0x50
				for (int k = 0; k < 3; ++k) memcpy(tri + 3 * k, reinterpret_cast<const void*>(s + 0x30 + 0x10 * k), 12);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		struct IgBody { int bsp = 0, slot = 0, layer = 0; uintptr_t body = 0; };
		struct IgGather
		{
			// in progress (game thread only)
			uint64_t sig = 0;              // of the bodies being walked (mask + body / shape pointers)
			uint32_t mask = 0;
			std::vector<IgBody> bodies;
			size_t bi = 0;                 // the body being walked
			uintptr_t cont = 0;
			uint32_t key = 0xFFFFFFFFu;
			bool started = false, done = false, failed = false;
			std::vector<float> tris;
			size_t bodyTris = 0, others = 0;
			std::string log;
			// published (under mu)
			std::mutex mu;
			uint32_t pubMask = 0;
			uint64_t pubVersion = 0, pubHash = 0;
			std::vector<float> pubTris;
			std::string pubLog;
		};
		IgGather gIg;

		bool layerCollides(uintptr_t filter, int a, int b)
		{
			if (!mem::okp(filter) || mem::u32(filter + 0x38, 0) != 2) return true;   // not a group filter: assume it does
			return (mem::u32(filter + 0x4C + 4 * (uintptr_t)(a & 31), 0xFFFFFFFFu) >> (b & 31)) & 1;
		}

		// the player's Havok layer (its rigid body's filter info), 11 when unreadable
		int playerLayer()
		{
			const uintptr_t b = base();
			const uintptr_t obj = playerObject();
			if (!obj) return 11;
			const uint32_t hc = mem::u32(obj + 0xF0, 0xFFFFFFFFu);
			const uintptr_t hdr = mem::q(b + kHavokComponents);
			if (hc == 0xFFFFFFFFu || !mem::okp(hdr)) return 11;
			const uintptr_t comp = hdr + mem::u32(hdr + 0x60, 0) + 0xE0 * (uintptr_t)(hc & 0xFFFF);
			const uintptr_t a1 = mem::q(comp + 0x30);
			const uintptr_t body = mem::okp(a1) ? mem::q(a1 + 0x40) : 0;
			return mem::okp(body) ? (int)(mem::u32(body + 0x4C, 11) & 31) : 11;
		}
	}

	void igGatherTick()
	{
		const uintptr_t b = base();
		if (!b || !buildOk()) return;
		IgGather& G = gIg;
		// what is loaded now: the bodies of the loaded bsps the player's layer collides with (not body 0: the structure)
		const uint32_t mask = mem::u32(b + kBspMask, 0);
		const uintptr_t world = mem::q(b + kHavokWorldPtr);
		uint64_t sig = 1469598103934665603ull ^ mask;
		for (int i = 0; i < 32 * 5; ++i)
		{
			const uintptr_t body = mem::q(b + kBspBodies + 8 * (uintptr_t)i);
			sig = (sig ^ body ^ (body ? mem::q(body + 0x20) : 0)) * 1099511628211ull;
		}
		if (sig != G.sig)
		{
			// a new set (map load, bsp switch): start over
			G.sig = sig; G.mask = mask; G.bodies.clear(); G.bi = 0; G.cont = 0; G.key = 0xFFFFFFFFu;
			G.started = false; G.done = false; G.failed = false; G.tris.clear(); G.bodyTris = 0; G.others = 0; G.log.clear();
			const uintptr_t filter = mem::okp(world) ? mem::q(world + 0xC0) : 0;
			const int pl = playerLayer();
			char line[200];
			snprintf(line, sizeof(line), "Havok world %llX, filter %llX, the player's layer %d\n", (unsigned long long)world, (unsigned long long)filter, pl);
			G.log = line;
			for (int bsp = 0; bsp < 32; ++bsp)
			{
				if (!(mask & (1u << bsp))) continue;
				for (int n = 1; n < 5; ++n)
				{
					const uintptr_t body = mem::q(b + kBspBodies + 8 * (uintptr_t)(bsp * 5 + n));
					if (!mem::okp(body)) continue;
					const int layer = (int)(mem::u32(body + 0x4C, 0) & 31);
					const bool inWorld = mem::q(body + 0x10) == world;
					const bool hits = layerCollides(filter, pl, layer);
					snprintf(line, sizeof(line), "bsp %d body %d: layer %d%s%s\n", bsp, n, layer, inWorld ? "" : " (not in the world)", hits ? "" : " (the player does not collide with it: left out)");
					G.log += line;
					if (inWorld && hits) G.bodies.push_back(IgBody{ bsp, n, layer, body });
				}
			}
		}
		if (G.done || G.failed) return;
		alignas(16) static uint8_t buffer[512];   // hkpShapeBuffer (game thread only)
		int budget = kKeysPerTick;
		while (budget > 0 && G.bi < G.bodies.size())
		{
			const IgBody& body = G.bodies[G.bi];
			if (!G.started)
			{
				const uintptr_t shape = mem::q(body.body + 0x20);   // hkpCdBody m_shape
				uintptr_t c = 0; uint32_t k = 0xFFFFFFFFu;
				if (!mem::okp(shape) || !sehContainer(shape, &c) || !mem::okp(c) || !sehFirstKey(c, &k)) { G.failed = true; G.log += "a shape container call faulted: gather stopped\n"; break; }
				G.cont = c; G.key = k; G.started = true; G.bodyTris = 0;
			}
			while (budget > 0 && G.key != 0xFFFFFFFFu)
			{
				--budget;
				float t[9]; uint8_t type = 0;
				const int r = sehChildTriangle(G.cont, G.key, buffer, t, &type);
				if (r < 0) { G.failed = true; G.log += "getChildShape faulted: gather stopped\n"; break; }
				bool finite = r == 1;
				for (int k = 0; finite && k < 9; ++k) finite = std::isfinite(t[k]) && std::fabs(t[k]) < 100000.f;
				if (finite && G.tris.size() / 9 < kMaxTris) { G.tris.insert(G.tris.end(), t, t + 9); ++G.bodyTris; }
				else if (r == 0) ++G.others;
				uint32_t next = 0xFFFFFFFFu;
				if (!sehNextKey(G.cont, G.key, &next)) { G.failed = true; G.log += "getNextKey faulted: gather stopped\n"; break; }
				G.key = next;
			}
			if (G.failed) break;
			if (G.key == 0xFFFFFFFFu)
			{
				char line[160];
				snprintf(line, sizeof(line), "bsp %d body %d (layer %d): %zu triangles\n", body.bsp, body.slot, body.layer, G.bodyTris);
				G.log += line;
				++G.bi; G.started = false;
			}
		}
		if (!G.failed && G.bi >= G.bodies.size())
		{
			G.done = true;
			char line[160];
			snprintf(line, sizeof(line), "done: %zu triangles, %zu other child shapes left out\n", G.tris.size() / 9, G.others);
			G.log += line;
		}
		if (G.done || G.failed)
		{
			// the version moves only when the triangles changed (a revert can rebuild the bodies with the same shapes)
			uint64_t h = 1469598103934665603ull ^ G.mask;
			for (float f : G.tris) { h ^= (uint64_t)(int64_t)std::lround(f * 1000.f); h *= 1099511628211ull; }
			std::scoped_lock lk(G.mu);
			if (h != G.pubHash || G.mask != G.pubMask || !G.pubVersion) { G.pubHash = h; ++G.pubVersion; G.pubTris = G.tris; }
			G.pubMask = G.mask; G.pubLog = G.log;
		}
	}

	uint64_t igVersion(uint32_t mask)
	{
		IgGather& G = gIg;
		std::scoped_lock lk(G.mu);
		return G.pubMask == mask ? G.pubVersion : 0;
	}

	void igToCollBsps(const std::vector<float>& tris, const std::vector<h2cv::CollisionWorld::Bsp3>& structure,
		std::vector<h2cv::CollBsp>& out, std::string& log)
	{
		using h2cv::CollBsp;
		out.clear();
		struct V { float x, y, z; };
		auto sub = [](V a, V b) { return V{ a.x - b.x, a.y - b.y, a.z - b.z }; };
		auto cross = [](V a, V b) { return V{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };
		auto dot = [](V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
		auto qk = [](float f) { return (int64_t)std::lround(f * 1000.f); };   // 1 mm
		const size_t n = tris.size() / 9;

		// the structure's surfaces on an xy grid (4 wu), to leave out triangles lying on one (the same face twice)
		struct SP { V n; float d; std::vector<V> poly; };
		std::vector<SP> sp;
		std::unordered_map<int64_t, std::vector<uint32_t>> sgrid;
		auto cell = [](float x, float y) { return ((int64_t)std::floor(x / 4.f) << 32) ^ (int64_t)(uint32_t)(int32_t)std::floor(y / 4.f); };
		std::vector<h2cv::Vec3> poly;
		for (const auto& b3 : structure)
			for (uint32_t s = 0; s < b3.world.surfaces.size(); ++s)
			{
				if (b3.world.surfaces[s].flags & 0x10) continue;
				h2cv::Vec3 nn;
				if (!b3.world.normal(s, nn) || !b3.world.polygon(s, poly) || poly.size() < 3) continue;
				SP e; e.n = V{ nn.x, nn.y, nn.z };
				for (const auto& p : poly) e.poly.push_back(V{ p.x, p.y, p.z });
				e.d = dot(e.n, e.poly[0]);
				float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
				for (const V& p : e.poly) { lx = (std::min)(lx, p.x); ly = (std::min)(ly, p.y); hx = (std::max)(hx, p.x); hy = (std::max)(hy, p.y); }
				const uint32_t id = (uint32_t)sp.size();
				sp.push_back(std::move(e));
				if (hx - lx > 400.f || hy - ly > 400.f) continue;   // implausibly big: never matched
				for (float x = std::floor(lx / 4.f) * 4.f; x <= hx; x += 4.f)
					for (float y = std::floor(ly / 4.f) * 4.f; y <= hy; y += 4.f) sgrid[cell(x, y)].push_back(id);
			}
		auto onStructure = [&](V c, V nrm) -> bool
		{
			const auto it = sgrid.find(cell(c.x, c.y));
			if (it == sgrid.end()) return false;
			for (uint32_t id : it->second)
			{
				const SP& s = sp[id];
				if (std::fabs(dot(nrm, s.n)) < 0.999f || std::fabs(dot(s.n, c) - s.d) > 0.01f) continue;
				int sign = 0; bool in = true;
				for (size_t k = 0; k < s.poly.size() && in; ++k)
				{
					const float w = dot(cross(sub(s.poly[(k + 1) % s.poly.size()], s.poly[k]), sub(c, s.poly[k])), s.n);
					if (std::fabs(w) < 1e-5f) continue;
					const int g = w > 0 ? 1 : -1;
					if (sign && g != sign) in = false;
					sign = g;
				}
				if (in) return true;
			}
			return false;
		};

		// weld, drop degenerate / duplicate / on-structure triangles
		std::unordered_map<int64_t, uint32_t> vmap;   // quantised position hash -> vertex (collisions resolved by the exact key below)
		std::map<std::tuple<int64_t, int64_t, int64_t>, uint32_t> vexact;
		std::vector<V> verts;
		auto vid = [&](V p) -> uint32_t
		{
			const auto key = std::make_tuple(qk(p.x), qk(p.y), qk(p.z));
			const auto it = vexact.find(key);
			if (it != vexact.end()) return it->second;
			const uint32_t id = (uint32_t)verts.size();
			verts.push_back(p);
			vexact.emplace(key, id);
			return id;
		};
		struct T { uint32_t v[3]; };
		std::vector<T> ts;
		std::set<std::array<uint32_t, 3>> seen;
		size_t degenerate = 0, duplicate = 0, onStruct = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const float* f = &tris[i * 9];
			const V a{ f[0], f[1], f[2] }, b{ f[3], f[4], f[5] }, c{ f[6], f[7], f[8] };
			const V nn = cross(sub(b, a), sub(c, a));
			const float l = std::sqrt(dot(nn, nn));
			if (!(l > 1e-6f)) { ++degenerate; continue; }
			const V nu{ nn.x / l, nn.y / l, nn.z / l };
			const V cen{ (a.x + b.x + c.x) / 3.f, (a.y + b.y + c.y) / 3.f, (a.z + b.z + c.z) / 3.f };
			if (onStructure(cen, nu)) { ++onStruct; continue; }
			T t{ { vid(a), vid(b), vid(c) } };
			if (t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[0] == t.v[2]) { ++degenerate; continue; }
			std::array<uint32_t, 3> key{ t.v[0], t.v[1], t.v[2] };
			std::sort(key.begin(), key.end());
			if (!seen.insert(key).second) { ++duplicate; continue; }
			ts.push_back(t);
		}

		// connected pieces (shared edges), each wound outward by its signed volume (a closed solid's is positive when wound
		// outward); an open piece (|volume| tiny next to its area) keeps Havok's winding
		std::vector<uint32_t> parent(ts.size());
		for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
		std::function<uint32_t(uint32_t)> find = [&](uint32_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
		std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeFirst;
		for (uint32_t i = 0; i < ts.size(); ++i)
			for (int k = 0; k < 3; ++k)
			{
				uint32_t a = ts[i].v[k], b = ts[i].v[(k + 1) % 3];
				if (a > b) std::swap(a, b);
				const auto it = edgeFirst.find({ a, b });
				if (it == edgeFirst.end()) edgeFirst[{ a, b }] = i;
				else parent[find(i)] = find(it->second);
			}
		std::unordered_map<uint32_t, std::vector<uint32_t>> pieces;
		for (uint32_t i = 0; i < ts.size(); ++i) pieces[find(i)].push_back(i);
		size_t flipped = 0, open = 0;
		for (auto& [root, list] : pieces)
		{
			const V ref = verts[ts[list[0]].v[0]];
			double vol = 0, area = 0;
			for (uint32_t i : list)
			{
				const V a = sub(verts[ts[i].v[0]], ref), b = sub(verts[ts[i].v[1]], ref), c = sub(verts[ts[i].v[2]], ref);
				vol += dot(a, cross(b, c));
				const V nn = cross(sub(b, a), sub(c, a));
				area += std::sqrt(dot(nn, nn)) * 0.5;
			}
			if (std::fabs(vol) < 1e-3 * area) { ++open; continue; }
			if (vol < 0) { ++flipped; for (uint32_t i : list) std::swap(ts[i].v[1], ts[i].v[2]); }
		}

		// chunks of whole pieces, at most 4095 surfaces each (a bigger piece is cut)
		std::vector<std::vector<uint32_t>> chunks(1);
		for (auto& [root, list] : pieces)
		{
			if (!chunks.back().empty() && chunks.back().size() + list.size() > 4095) chunks.emplace_back();
			for (uint32_t i : list)
			{
				if (chunks.back().size() >= 4095) chunks.emplace_back();
				chunks.back().push_back(i);
			}
		}
		size_t seamEdges = 0;
		for (const auto& ch : chunks)
		{
			if (ch.empty()) continue;
			CollBsp cb;
			std::unordered_map<uint32_t, uint32_t> local;
			auto lv = [&](uint32_t g) -> uint32_t
			{
				const auto it = local.find(g);
				if (it != local.end()) return it->second;
				const uint32_t id = (uint32_t)cb.verts.size();
				cb.verts.push_back(h2cv::Vec3{ verts[g].x, verts[g].y, verts[g].z });
				local.emplace(g, id);
				return id;
			};
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> open1;   // directed edge (v0, v1) owned by a left surface, no right yet
			for (const uint32_t ti : ch)
			{
				const uint32_t s = (uint32_t)cb.surfaces.size();
				const uint32_t v[3] = { lv(ts[ti].v[0]), lv(ts[ti].v[1]), lv(ts[ti].v[2]) };
				const V a = verts[ts[ti].v[0]], b = verts[ts[ti].v[1]], c = verts[ts[ti].v[2]];
				V nn = cross(sub(b, a), sub(c, a));
				const float l = std::sqrt(dot(nn, nn));
				nn = V{ nn.x / l, nn.y / l, nn.z / l };
				cb.planes.insert(cb.planes.end(), { nn.x, nn.y, nn.z, dot(nn, a) });
				uint32_t e[3]; bool right[3];
				for (int k = 0; k < 3; ++k)
				{
					const uint32_t p = v[k], q = v[(k + 1) % 3];
					const auto it = open1.find({ q, p });   // a neighbour walked it the other way: this surface is its right side
					if (it != open1.end()) { e[k] = it->second; right[k] = true; cb.edges[e[k]].right = s; open1.erase(it); ++seamEdges; }
					else
					{
						e[k] = (uint32_t)cb.edges.size(); right[k] = false;
						cb.edges.push_back(CollBsp::Edge{ p, q, CollBsp::kNone, CollBsp::kNone, s, CollBsp::kNone });
						open1[{ p, q }] = e[k];
					}
				}
				for (int k = 0; k < 3; ++k)
				{
					// the ring: left side -> v0 then fwd, right side -> v1 then rev (CollBsp::polygon)
					if (right[k]) cb.edges[e[k]].rev = e[(k + 1) % 3];
					else cb.edges[e[k]].fwd = e[(k + 1) % 3];
				}
				CollBsp::Surface sf{};
				sf.plane = s; sf.firstEdge = e[0]; sf.flip = false; sf.flags = 0; sf.breakable = -1; sf.material = 0; sf.breakBit = 0; sf.breakGroup = 0;
				cb.surfaces.push_back(sf);
			}
			out.push_back(std::move(cb));
		}
		char line[320];
		snprintf(line, sizeof(line), "instanced geometry: %zu triangles gathered -> %zu kept (%zu on a structure surface, %zu duplicates, %zu degenerate left out), "
			"%zu pieces (%zu wound the other way round, %zu open), %zu shared edges, %zu chunks",
			n, ts.size(), onStruct, duplicate, degenerate, pieces.size(), flipped, open, seamEdges, out.size());
		log = line;
	}

	bool igTriangles(uint32_t mask, std::vector<float>& out, uint64_t* version, std::string* log)
	{
		IgGather& G = gIg;
		std::scoped_lock lk(G.mu);
		if (version) *version = G.pubVersion;
		if (log) *log = G.pubLog;
		if (!G.pubVersion || G.pubMask != mask) return false;
		out = G.pubTris;
		return true;
	}
}
