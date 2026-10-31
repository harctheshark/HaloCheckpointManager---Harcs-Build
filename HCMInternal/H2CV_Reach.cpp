#include "pch.h"
#include "H2CV_Reach.h"
#include "H2CV_Mem.h"
#include <windows.h>
#include <cmath>
#include <cctype>
#include <cstring>
#include <vector>

namespace reach
{
	namespace
	{
		constexpr uint32_t kStamp = 0x68A0EFE1;
		constexpr uintptr_t kCamera = 0xC9FAE0;          // render camera record
		// surface table (Poorboy's kDepthGames row for Reach): table, stride, flags @+0x10; own buffer index (flag 0x100,
		// int @+0x68); paired buffers (flag 8, index x2); per split-screen view (flag 0x20, view index int @ 0x4E38A08);
		// buffers pointer @+0x60, stride 0x88, buffer +0 = ID3D11DepthStencilView; main depth = surface 9
		constexpr uintptr_t kSurfTable = 0xC8E4B0;
		constexpr uint32_t kSurfStride = 0x70, kFlagsAt = 0x10, kOwnFlag = 0x100, kOwnAt = 0x68, kPairFlag = 8, kViewFlag = 0x20;
		constexpr uintptr_t kViewIndex = 0x4E38A08;
		constexpr uint32_t kBuffersAt = 0x60, kBufferStride = 0x88;
		constexpr int kMainDepthSurface = 9;

		uintptr_t sCheckedBase = 0;
		bool sCheckedOk = false;

		float len(const h2::Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
	}

	uintptr_t base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"haloreach.dll")); }

	uintptr_t thisThreadTls()
	{
		const uintptr_t b = base();
		if (!b) return 0;
		const uint32_t index = mem::u32(b + 0xC17B18, 0xFFFFFFFF);
		const uintptr_t slots = (uintptr_t)__readgsqword(0x58);   // TEB.ThreadLocalStoragePointer
		if (index >= 1088 || !slots) return 0;
		return mem::q(slots + 8 * (uintptr_t)index);
	}

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

	h2::Camera readCamera()
	{
		h2::Camera c;
		const uintptr_t b = base();
		if (!b || !buildOk()) return c;
		float v[9];
		if (!mem::copy(v, b + kCamera, sizeof(v))) return c;
		for (float x : v) if (!std::isfinite(x)) return c;
		c.pos = { v[0], v[1], v[2] };
		c.fwd = { v[3], v[4], v[5] };
		c.up = { v[6], v[7], v[8] };
		c.vfov = mem::f32(b + kCamera + 0x28, 0.f);
		c.nearClip = mem::f32(b + kCamera + 0x64, 0.f);
		c.farClip = mem::f32(b + kCamera + 0x68, 0.f);
		if (std::fabs(len(c.fwd) - 1.f) > 1e-2f || std::fabs(len(c.up) - 1.f) > 1e-2f) return c;
		if (!(c.vfov > 0.2f && c.vfov < 2.9f)) return c;
		if (!(c.nearClip > 1e-5f && c.nearClip < 1.f)) c.nearClip = 1.f / 128.f;     // render_near_clip_distance default
		if (!(c.farClip > c.nearClip)) c.farClip = 10240.f;                           // render_far_clip_distance default
		c.ok = true;
		return c;
	}

	bool readViewport(int& width, int& height)
	{
		width = height = 0;
		const uintptr_t b = base();
		if (!b || !buildOk()) return false;
		int16_t r[4];
		if (!mem::copy(r, b + kCamera + 0x38, sizeof(r))) return false;
		width = r[3] - r[1]; height = r[2] - r[0];
		return width > 0 && height > 0;
	}

	namespace
	{
		// the last part of a path-like C string at `at` (Poorboy's rule: only the name itself must be printable), lower case
		std::string nameAt(uintptr_t at)
		{
			char raw[129] = {};
			if (!mem::copy(raw, at, 128)) return "";
			std::string path(raw);
			const size_t cut = path.find_last_of("\\/");
			std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
			if (name.empty() || name.size() > 64) return "";
			for (char& c : name)
			{
				if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return "";
				c = (char)std::tolower((unsigned char)c);
			}
			for (char c : name) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return "";   // goes into a file name
			return name;
		}

		std::string hexAt(uintptr_t at, size_t n)
		{
			uint8_t b[48] = {};
			n = n > sizeof(b) ? sizeof(b) : n;
			if (!mem::copy(b, at, n)) return "unreadable";
			std::string out;
			char h[4];
			for (size_t i = 0; i < n; ++i) { snprintf(h, sizeof(h), "%02X", b[i]); out += h; if (i % 4 == 3) out += ' '; }
			return out;
		}
	}

	std::string mapName()
	{
		const uintptr_t b = base();
		if (!b || !buildOk()) return "";
		std::string n = nameAt(b + 0x2A1F517);           // Poorboy's level path ("...\m50")
		if (n.empty()) n = nameAt(b + 0x2A1F527);         // HCM's currentLevelCode (InternalPointerData.xml, 1.3528)
		return n;
	}

	std::string mapNameDiag()
	{
		const uintptr_t b = base();
		if (!b) return "no module";
		return "+0x2A1F517: " + hexAt(b + 0x2A1F517, 32) + " | +0x2A1F527: " + hexAt(b + 0x2A1F527, 32);
	}

	uint32_t activeBspMask()
	{
		const uintptr_t b = base();
		if (!b || !buildOk()) return 0;
		return mem::u32(b + 0x4E2FB2C, 0);
	}

	namespace
	{
		// the texture behind a depth view of the game's (one reference), when it is a depth view of `device`. Its own
		// function: SEH cannot share a function with objects that need unwinding
		ID3D11Texture2D* depthTextureOf(uintptr_t view, ID3D11Device* device)
		{
			ID3D11Texture2D* tex = nullptr;
			__try
			{
				ID3D11DepthStencilView* dsv = nullptr;
				if (SUCCEEDED(reinterpret_cast<IUnknown*>(view)->QueryInterface(__uuidof(ID3D11DepthStencilView), (void**)&dsv)) && dsv)
				{
					ID3D11Device* owner = nullptr;
					dsv->GetDevice(&owner);
					ID3D11Resource* res = nullptr;
					if (owner == device) dsv->GetResource(&res);
					if (owner) owner->Release();
					dsv->Release();
					if (res)
					{
						if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) tex = nullptr;
						res->Release();
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { tex = nullptr; }
			return tex;
		}
	}

	ID3D11Texture2D* mainDepth(ID3D11Device* device, std::string* why)
	{
		auto fail = [why](std::string w) -> ID3D11Texture2D* { if (why) *why = std::move(w); return nullptr; };
		const uintptr_t b = base();
		if (!b || !buildOk() || !device) return fail("no module / device");
		const uintptr_t entry = b + kSurfTable + (uintptr_t)kMainDepthSurface * kSurfStride;
		const uint32_t flags = mem::u32(entry + kFlagsAt, 0);
		const uintptr_t buffers = mem::q(entry + kBuffersAt);
		char d[200];
		snprintf(d, sizeof(d), "surface %d flags %08X buffers %llX", kMainDepthSurface, flags, (unsigned long long)buffers);
		if (!mem::okp(buffers)) return fail(std::string(d) + ": no buffer array");
		int index = 0;
		if (flags & kOwnFlag) index = mem::i32(entry + kOwnAt, -1);
		if (index < 0) return fail(std::string(d) + ": own index unreadable");
		if (flags & kPairFlag) index *= 2;
		if (flags & kViewFlag) index = mem::i32(b + kViewIndex, 0) + index * 4;
		if (index < 0 || index >= 64) return fail(std::string(d) + ": index " + std::to_string(index) + " out of range");
		const uintptr_t view = mem::q(buffers + (uintptr_t)index * kBufferStride);
		snprintf(d + strlen(d), sizeof(d) - strlen(d), " index %d view %llX", index, (unsigned long long)view);
		if (!mem::okp(view) || !mem::okp(mem::q(view))) return fail(std::string(d) + ": view (or its vtable) unreadable");
		ID3D11Texture2D* tex = depthTextureOf(view, device);
		if (!tex && why) *why = std::string(d) + ": not a depth view of our device";
		else if (why) *why = std::string(d);
		return tex;
	}

	// ================================================================================================================
	// Direct collision read (REPORT_REACH_DIRECT_READ.md). Every address below is a haloreach.dll RVA / an offset into
	// tag or resource data; every read is guarded (mem::).
	// ================================================================================================================
	namespace
	{
		constexpr uintptr_t kScenarioPtr = 0xC1A230;     // *(base+) = scenario tag data
		constexpr uintptr_t kTagTablePtr = 0xC1A600;     // *(base+) = tag instances, 8 B each, u32 tag address at +4
		constexpr uintptr_t kTagSegments = 0x4E39F20;    // 16 qwords: segmented tag-address bases
		constexpr uintptr_t kResGlobal = 0xB93550;       // the resource table root (sub_180014124)
		constexpr uint32_t kScenarioBsps = 0x50, kScenarioBspStride = 0xAC, kScenarioBspTag = 12;
		constexpr uint32_t kSbspResource = 0x50C, kSbspPlacementsRes = 0x520;
		constexpr uint32_t kResSmall = 0x00, kResLarge = 0x0C, kResDefs = 0x18;
		constexpr uint32_t kDefStride = 0x144, kDefCollision = 0x08;
		constexpr uint32_t kPlacementsBlock = 0x48, kPlacementStride = 0x9C;

		struct Block { uint32_t count = 0, addr = 0; };

		// a 32-bit tag address -> pointer (Reach's segmented scheme: base[addr >> 28] + 4 * addr)
		uintptr_t tagAddr(uintptr_t b, uint32_t a)
		{
			if (!a) return 0;
			const uintptr_t seg = mem::q(b + kTagSegments + 8 * (uintptr_t)(a >> 28));
			return mem::okp(seg) ? seg + 4ull * a : 0;
		}
		Block block(uintptr_t at) { Block k; k.count = mem::u32(at, 0); k.addr = mem::u32(at + 4, 0); return k; }

		// haloreach!sub_180014124 re-done from outside: the resource behind a handle (inline, or the TLS-indexed table).
		// ⚠ The table is per THREAD: i32 [TLS+0x338] picks a resource view, and only some views hold the level. Live m50
		// (2026-10-26): the Havok engine-step thread's TLS selects view 1 (empty: every BSP resolved to 0, so the direct read
		// failed and instanced geometry vanished); the main game thread's selects view 2, which resolves. So: the given
		// thread's view first, then the first other live view that resolves this handle.
		uintptr_t resource(uintptr_t b, uintptr_t gameTls, uintptr_t handle)
		{
			if (mem::u32(handle + 0x1C, 0) == 0) return tagAddr(b, mem::u32(handle + 4, 0));
			const int64_t r8 = (int64_t)mem::q(b + kResGlobal);
			const uintptr_t tab = (r8 == -212096) ? 0xD8 : (uintptr_t)(r8 + 0x33C80 + 0x698);
			const uintptr_t t0 = mem::q(tab);
			const uintptr_t index = mem::u16(handle + 0xC, 0);
			auto inView = [&](int32_t view) -> uintptr_t
				{
					if (view < 0 || view > 64) return 0;
					const uintptr_t t1 = mem::q(t0 + 8 * (uintptr_t)view);
					const uintptr_t t2 = mem::okp(t1) ? mem::q(t1 + 0x18) : 0;
					return mem::okp(t2) ? mem::q(t2 + 8 * index) : 0;
				};
			const int32_t mine = mem::okp(gameTls) ? mem::i32(gameTls + 0x338, -1) : -1;
			if (const uintptr_t r = inView(mine)) return r;
			for (int32_t view = 0; view < 16; ++view)
				if (view != mine)
					if (const uintptr_t r = inView(view)) return r;
			return 0;
		}

		// a collision_bsp (Reach header 0x6C = Halo 3's 0x60 with an extra block at index 1) -> polygons
		struct BspHeader { Block surfaces, edges, vertices; };
		bool header(uintptr_t at, BspHeader& h)
		{
			uint8_t raw[0x6C];
			if (!mem::copy(raw, at, sizeof(raw))) return false;
			auto blk = [&](int h3Index) { const size_t o = h3Index == 0 ? 0 : 12 + (size_t)h3Index * 12; Block k; memcpy(&k.count, raw + o, 4); memcpy(&k.addr, raw + o + 4, 4); return k; };
			h.surfaces = blk(5); h.edges = blk(6); h.vertices = blk(7);
			return h.surfaces.count <= 2000000 && h.edges.count <= 4000000 && h.vertices.count <= 4000000;
		}

		struct Xf { bool on = false; float s = 1, r[9]{}, p[3]{}; };
		h2::Vec3 apply(const Xf& x, float a, float b, float c)
		{
			if (!x.on) return { a, b, c };
			return { (a * x.r[0] + b * x.r[3] + c * x.r[6]) * x.s + x.p[0],
			         (a * x.r[1] + b * x.r[4] + c * x.r[7]) * x.s + x.p[1],
			         (a * x.r[2] + b * x.r[5] + c * x.r[8]) * x.s + x.p[2] };
		}

		// the winged-edge walk (h3sb == Halo 3): from the first edge, left side -> v0 / forward, right side -> v1 / reverse
		int emitBsp(uintptr_t b, const BspHeader& h, bool large, const Xf& x, uint8_t inst, uint8_t bsp, Polys& out)
		{
			const size_t ss = large ? 16 : 12, es = large ? 24 : 12, vs = large ? 20 : 16;
			std::vector<uint8_t> S((size_t)h.surfaces.count * ss), E((size_t)h.edges.count * es), V((size_t)h.vertices.count * vs);
			if (S.empty() || E.empty() || V.empty()) return 0;
			if (!mem::copy(S.data(), tagAddr(b, h.surfaces.addr), S.size()) || !mem::copy(E.data(), tagAddr(b, h.edges.addr), E.size())
				|| !mem::copy(V.data(), tagAddr(b, h.vertices.addr), V.size())) return -1;
			auto u16at = [](const std::vector<uint8_t>& a, size_t o) { uint16_t v; memcpy(&v, a.data() + o, 2); return (uint32_t)v; };
			auto u32at = [](const std::vector<uint8_t>& a, size_t o) { uint32_t v; memcpy(&v, a.data() + o, 4); return v; };
			constexpr uint8_t kInvisible = 0x02, kBreakable = 0x08, kInvalid = 0x10;   // = Halo 2 / Halo 3 (and h3sb on Reach)
			auto surfFlags = [&](uint32_t s) -> int { return s < h.surfaces.count ? S[(size_t)s * ss + (large ? 14 : 10)] : -1; };
			const uint32_t none = large ? 0xFFFFFFFFu : 0xFFFFu;
			// lines: the edge table, as Halo 2's classify does it
			for (uint32_t e = 0; e < h.edges.count; ++e)
			{
				const size_t eo = (size_t)e * es;
				const uint32_t v0 = large ? u32at(E, eo) : u16at(E, eo), v1 = large ? u32at(E, eo + 4) : u16at(E, eo + 2);
				const uint32_t l = large ? u32at(E, eo + 16) : u16at(E, eo + 8), r = large ? u32at(E, eo + 20) : u16at(E, eo + 10);
				if (v0 >= h.vertices.count || v1 >= h.vertices.count || v0 == v1) continue;
				const int fL = surfFlags(l), fR = surfFlags(r);
				if ((l != none && (fL < 0 || (fL & kInvalid))) || (r != none && (fR < 0 || (fR & kInvalid)))) continue;
				uint8_t kind = Solid;
				if ((fL >= 0 && (fL & kBreakable)) || (fR >= 0 && (fR & kBreakable))) kind = Breakable;
				else if ((fL >= 0 && (fL & kInvisible)) || (fR >= 0 && (fR & kInvisible))) kind = Invisible;
				float a[3], c[3]; memcpy(a, V.data() + (size_t)v0 * vs, 12); memcpy(c, V.data() + (size_t)v1 * vs, 12);
				out.lineV.push_back(apply(x, a[0], a[1], a[2]));
				out.lineV.push_back(apply(x, c[0], c[1], c[2]));
				out.lineKind.push_back(kind); out.lineInstance.push_back(inst); out.lineBsp.push_back(bsp);
			}
			// occluders: the solid surfaces (invisible / breakable / invalid ones are not visual occluders - Halo 2 the same)
			int made = 0;
			for (uint32_t s = 0; s < h.surfaces.count; ++s)
			{
				const size_t so = (size_t)s * ss;
				const uint8_t flags = S[so + (large ? 14 : 10)];
				if (flags & (kInvalid | kBreakable | kInvisible)) continue;
				const uint32_t first = large ? u32at(S, so + 4) : u16at(S, so + 2);
				uint32_t e = first;
				const size_t at = out.v.size();
				for (int guard = 0; guard < 64 && e < h.edges.count; ++guard)
				{
					const size_t eo = (size_t)e * es;
					uint32_t vtx, next;
					const uint32_t left = large ? u32at(E, eo + 16) : u16at(E, eo + 8), right = large ? u32at(E, eo + 20) : u16at(E, eo + 10);
					if (left == s) { vtx = large ? u32at(E, eo) : u16at(E, eo); next = large ? u32at(E, eo + 8) : u16at(E, eo + 4); }
					else if (right == s) { vtx = large ? u32at(E, eo + 4) : u16at(E, eo + 2); next = large ? u32at(E, eo + 12) : u16at(E, eo + 6); }
					else break;
					if (vtx >= h.vertices.count) break;
					float p[3]; memcpy(p, V.data() + (size_t)vtx * vs, 12);
					out.v.push_back(apply(x, p[0], p[1], p[2]));
					e = next;
					if (e == first) break;
				}
				if (out.v.size() - at >= 3) { out.start.push_back((uint32_t)at); out.instance.push_back(inst); out.bsp.push_back(bsp); ++made; }
				else out.v.resize(at);
			}
			return made > 0 ? made : (int)(h.edges.count ? 1 : 0);      // an instance of only invisible surfaces still counts
		}
	}

	bool readCollision(uintptr_t gameTls, uint32_t mask, Polys& out, std::string& log)
	{
		out = Polys{};
		const uintptr_t b = base();
		if (!b || !buildOk()) { log = "no haloreach.dll 1.3528"; return false; }
		char line[320];
		const uintptr_t scenario = mem::q(b + kScenarioPtr), tags = mem::q(b + kTagTablePtr);
		const Block bsps = block(scenario + kScenarioBsps);
		snprintf(line, sizeof(line), "scenario %llX tags %llX: %u structure bsps, mask %08X, game TLS %llX\n", (unsigned long long)scenario,
			(unsigned long long)tags, bsps.count, mask, (unsigned long long)gameTls);
		log = line;
		if (!mem::okp(scenario) || !mem::okp(tags) || bsps.count == 0 || bsps.count > 32) return false;
		const uintptr_t bspArr = tagAddr(b, bsps.addr);
		for (uint32_t i = 0; i < bsps.count; ++i)
		{
			if (mask && !(mask & (1u << i))) continue;
			const uint16_t tagIndex = mem::u16(bspArr + (uintptr_t)i * kScenarioBspStride + kScenarioBspTag, 0xFFFF);
			if (tagIndex == 0xFFFF) continue;
			const uintptr_t sbsp = tagAddr(b, mem::u32(tags + 8 * (uintptr_t)tagIndex + 4, 0));
			const uintptr_t res = resource(b, gameTls, sbsp + kSbspResource);
			const Block smallBsp = block(res + kResSmall), largeBsp = block(res + kResLarge), defs = block(res + kResDefs);
			const uint8_t bi = (uint8_t)i;
			const Xf none;
			int structure = -2;
			BspHeader h;
			if (smallBsp.count == 1 && header(tagAddr(b, smallBsp.addr), h)) structure = emitBsp(b, h, false, none, 0, bi, out);
			else if (largeBsp.count == 1 && header(tagAddr(b, largeBsp.addr), h)) structure = emitBsp(b, h, true, none, 0, bi, out);
			// instance placements: a separate resource, through a handle built like h3sb does ({u16 index @+0xC, u32 1 @+0x1C})
			int placed = 0, solid = 0, igPolys = 0, renderOnly = 0, noPhysics = 0;
			const uint16_t placeRes = mem::u16(sbsp + kSbspPlacementsRes, 0xFFFF);
			if (placeRes != 0xFFFF && defs.count)
			{
				alignas(8) uint8_t handle[0x20] = {};
				memcpy(handle + 0xC, &placeRes, 2);
				const uint32_t one = 1; memcpy(handle + 0x1C, &one, 4);
				const uintptr_t res2 = resource(b, gameTls, (uintptr_t)handle);
				const Block pl = block(res2 + kPlacementsBlock);
				const uintptr_t plArr = tagAddr(b, pl.addr), defArr = tagAddr(b, defs.addr);
				if (pl.count <= 20000 && mem::okp(plArr) && mem::okp(defArr))
					for (uint32_t k = 0; k < pl.count; ++k)
					{
						uint8_t r[kPlacementStride];
						if (!mem::copy(r, plArr + (uintptr_t)k * kPlacementStride, sizeof(r))) break;
						++placed;
						Xf x; x.on = true;
						memcpy(&x.s, r + 0x00, 4); memcpy(x.r, r + 0x04, 36); memcpy(x.p, r + 0x28, 12);
						int16_t def; memcpy(&def, r + 0x34, 2);
						const uint8_t flags = r[0x36];
						uint32_t physics; memcpy(&physics, r + 0x88, 4);
						if (def < 0 || (uint32_t)def >= defs.count) continue;
						if (flags & 2) { ++renderOnly; continue; }               // render only: no collision in the game
						if (!physics) { ++noPhysics; continue; }                  // no physics block: not collidable
						if (!std::isfinite(x.s) || x.s <= 0.f || x.s > 1000.f) continue;
						BspHeader dh;
						if (!header(defArr + (uintptr_t)def * kDefStride + kDefCollision, dh)) continue;
						const int n = emitBsp(b, dh, false, x, 1, bi, out);
						if (n > 0) { ++solid; igPolys += n; }
					}
			}
			snprintf(line, sizeof(line), "bsp %u: tag %u sbsp %llX res %llX (small %u large %u defs %u): structure %d polygons; instances %d placed, %d solid, %d polygons (%d render only, %d no physics)\n",
				i, tagIndex, (unsigned long long)sbsp, (unsigned long long)res, smallBsp.count, largeBsp.count, defs.count, structure, placed, solid, igPolys, renderOnly, noPhysics);
			log += line;
		}
		out.start.push_back((uint32_t)out.v.size());
		return !out.lineV.empty();
	}
}
