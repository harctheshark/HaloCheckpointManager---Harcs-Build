#pragma once
// Halo 2 (MCC 1.3528, halo2.dll) free-camera collision.
//
// Why this exists: HCM's Free Camera hooks halo2.dll+0x6F1BAC, which is INSIDE the observer update sub_1806F1A60, right
// after sub_1806F0250 (the observer's final stage, which already did the engine's camera collision) - so the position the
// free camera writes there is never collision-tested by the engine. This file re-does the engine's own camera collision for
// the free camera, calling the very same engine functions sub_1806F0250 uses, on the same (game) thread, in the same frame:
//
//     loc  = sub_1806782B0(&loc, newPos)                       // scenario location of the destination point
//     mask = sub_1806783D0(&loc, newPos, 0) ? 0x80100D : 0x80100F
//     hit  = sub_18075B2C0(mask, oldPos, newPos, -1, -1, &result)   // collision_test_vector: world BSP + instanced
//                                                                    // geometry + the object types the camera collides with
//     (result: +0 hit kind, +4 fraction along the segment, +0x28 front-facing plane {nx, ny, nz, d})
//
// The engine then pulls the camera back to oldPos + 0.9 * t * (newPos - oldPos). Here we instead stop a small radius short
// of the surface and slide along it (up to 3 segments), which is what a free camera should do.
//
// Everything is optional: if any of the three functions' entry bytes are not the ones the RE was done on (another build, or
// already hooked by something else), or the module is missing, or a call faults, the free camera falls back to no collision.
// Only POD in functions that contain __try.
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <atomic>

namespace h2fcc
{
	// halo2.dll RVAs (IDA EA - 0x180000000) for 1.3528
	constexpr uintptr_t kRvaCollisionTestVector = 0x75B2C0;   // sub_18075B2C0(int mask, float* from, float* to, int ignoreA, int ignoreB, void* result)
	constexpr uintptr_t kRvaLocationFromPoint = 0x6782B0;     // sub_1806782B0(void* outLocation8, float* point)
	constexpr uintptr_t kRvaLocationFlag = 0x6783D0;          // sub_1806783D0(void* location8, float* point, uint16_t* out) -> char

	constexpr int kMaskA = 0x80100F;   // 8424463, what the engine uses by default
	constexpr int kMaskB = 0x80100D;   // 8424461, what it uses when sub_1806783D0 answers true

	// Default distance (world units) the camera stays off a surface (the freeCameraCollisionRadius setting overrides it).
	constexpr float kRadius = 0.25f;

	// What the free camera collides with (flag bits of sub_18075B850, read 2026-11-11):
	//   0x1 structure BSP, 0x4 instanced geometry, 0x8 objects, object type t = bit (4 + t) (none set = all types),
	//   0x800000 front-facing surfaces only, 0x4000000 skip INVISIBLE surfaces (collision surface flag 2 = the "*" walls),
	//   0x8000000 skip breakable surfaces, 0x2 media (water) planes - not used: the player does not collide with water.
	// Objects: every type except weapon (2), equipment (3), garbage (4), projectile (5) = what a player bumps into.
	constexpr int kMaskPlayerLike = 0x1 | 0x4 | 0x8 | 0x1FC30 | 0x800000 | 0x4000000;
	constexpr int kMaxSlideSegments = 3;

	using TestVectorFn = uint8_t(__fastcall*)(int mask, const float* from, const float* to, int ignoreA, int ignoreB, void* result);
	using LocationFn = int64_t(__fastcall*)(void* outLocation, const float* point);
	using LocationFlagFn = char(__fastcall*)(void* location, const float* point, uint16_t* out);

	struct Api
	{
		TestVectorFn testVector = nullptr;
		LocationFn location = nullptr;
		LocationFlagFn locationFlag = nullptr;
	};

	// 0 = not resolved yet, 1 = ready, 2 = unavailable (permanent for this process)
	inline std::atomic<int> gState{ 0 };
	inline std::atomic<int> gFaults{ 0 };
	// 2026-11-21 (user: "free cam collision isnt working"): three faulted engine calls used to switch collision off for the
	// rest of the session, silently - the user's live HCM had gFaults = 3. A burst of faults is what a map load does to a
	// free camera that stays on (the collision BSP is briefly gone, e.g. Map Launcher's mid-game Play). Now a burst only
	// pauses collision: 3 s after the last fault the count resets and collision is tried again. Each burst is logged.
	inline std::atomic<uint64_t> gLastFaultMs{ 0 };
	inline void noteFault()
	{
		const uint64_t now = GetTickCount64();
		const uint64_t last = gLastFaultMs.exchange(now);
		const int n = gFaults.fetch_add(1) + 1;
		if (n == 1 || now - last > 3000) PLOG_WARNING << "free camera collision: an engine collision call faulted (count " << n << ") - collision pauses after 3 until 3 s pass without one";
	}
	inline bool faultedOut()
	{
		if (gFaults.load(std::memory_order_relaxed) < 3) return false;
		if (GetTickCount64() - gLastFaultMs.load(std::memory_order_relaxed) > 3000)
		{
			gFaults.store(0);
			PLOG_INFO << "free camera collision: resuming after faults";
			return false;
		}
		return true;
	}
	inline Api gApi;

	inline bool bytesMatch(uintptr_t at, const uint8_t* expect, size_t n)
	{
		uint8_t got[32] = {};
		if (n > sizeof(got)) return false;
		__try { memcpy(got, reinterpret_cast<const void*>(at), n); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return memcmp(got, expect, n) == 0;
	}

	// Verifies the three entry points (first instructions, up to the first RIP-relative operand) and fills gApi.
	inline bool resolve()
	{
		int s = gState.load(std::memory_order_acquire);
		if (s != 0) return s == 1;

		bool ok = false;
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"halo2.dll"));
		if (base)
		{
			// sub_18075B2C0: sub rsp,48h; movss xmm0,[r8]; subss xmm0,[rdx]; movss xmm1,[r8+4]; mov rax,[rsp+78h]
			static const uint8_t b1[] = { 0x48, 0x83, 0xEC, 0x48, 0xF3, 0x41, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x5C, 0x02,
				0xF3, 0x41, 0x0F, 0x10, 0x48, 0x04, 0x48, 0x8B, 0x44, 0x24, 0x78 };
			// sub_1806782B0: push rbx; sub rsp,20h; cmp word ptr [rip+..],-1  (displacement skipped) ; mov rbx,rcx; jnz
			static const uint8_t b2a[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x83, 0x3D };
			static const uint8_t b2b[] = { 0xFF, 0x48, 0x8B, 0xD9, 0x75, 0x1C };
			// sub_1806783D0: push rbp; movzx r11d,word ptr [rcx+4]; mov rbp,rdx; cmp r11w,-1
			static const uint8_t b3[] = { 0x40, 0x55, 0x44, 0x0F, 0xB7, 0x59, 0x04, 0x48, 0x8B, 0xEA, 0x66, 0x41, 0x83, 0xFB, 0xFF };

			ok = bytesMatch(base + kRvaCollisionTestVector, b1, sizeof(b1))
				&& bytesMatch(base + kRvaLocationFromPoint, b2a, sizeof(b2a))
				&& bytesMatch(base + kRvaLocationFromPoint + 13, b2b, sizeof(b2b))
				&& bytesMatch(base + kRvaLocationFlag, b3, sizeof(b3));
			if (ok)
			{
				gApi.testVector = reinterpret_cast<TestVectorFn>(base + kRvaCollisionTestVector);
				gApi.location = reinterpret_cast<LocationFn>(base + kRvaLocationFromPoint);
				gApi.locationFlag = reinterpret_cast<LocationFlagFn>(base + kRvaLocationFlag);
			}
		}
		gState.store(ok ? 1 : 2, std::memory_order_release);
		return ok;
	}

	inline bool available() { return resolve(); }

	struct Hit
	{
		bool hit = false;        // the engine reported a collision
		float t = 1.f;           // fraction of (to - from)
		bool hasNormal = false;  // front-facing unit normal read from the result and sane
		float n[3] = { 0.f, 0.f, 0.f };
	};

	// One engine collision query along from -> to. Returns false if the call faulted (out is then unusable).
	// POD only (SEH).
	inline bool testSegment(const float* from, const float* to, Hit& out)
	{
		out = Hit{};
		alignas(16) uint8_t location[16] = {};
		alignas(16) uint8_t result[256] = {};
		float f[3] = { from[0], from[1], from[2] };
		float t3[3] = { to[0], to[1], to[2] };
		uint8_t r = 0;
		__try
		{
			r = gApi.testVector(kMaskPlayerLike, f, t3, -1, -1, result);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		if (!r) return true;   // no hit

		float t = 0.f, n[4] = {};
		memcpy(&t, result + 4, 4);
		memcpy(n, result + 40, 16);
		if (!std::isfinite(t)) return true;   // garbage: treat as no hit rather than act on it
		out.hit = true;
		out.t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
		const float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
		if (std::isfinite(len2) && std::fabs(len2 - 1.f) < 0.1f)
		{
			out.hasNormal = true;
			out.n[0] = n[0]; out.n[1] = n[1]; out.n[2] = n[2];
		}
		return true;
	}

	// 26 unit directions (faces, edges and corners of a cube): the probe rays that give the camera a radius.
	inline const float (*probeDirs())[3]
	{
		static float d[26][3];
		static bool made = false;
		if (!made)
		{
			int n = 0;
			for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z)
			{
				if (!x && !y && !z) continue;
				const float l = std::sqrt((float)(x * x + y * y + z * z));
				d[n][0] = x / l; d[n][1] = y / l; d[n][2] = z / l; ++n;
			}
			made = true;   // game thread only
		}
		return d;
	}

	// Moves the camera from `from` towards `to` as a SPHERE of the given radius (in/out: `to` becomes the allowed position).
	// Returns false if collision could not be evaluated at all (caller then keeps the unmodified target = no collision).
	//
	// Two stages, all with the engine's own ray test:
	//   1. MOVE: the centre's path, looked at `radius` further than it travels. A surface in the way stops the centre where
	//      the sphere would touch its plane (travel = hit distance - radius / cos), then the rest of the motion slides
	//      along that plane. Up to kMaxSlideSegments planes per frame.
	//   2. CLEARANCE: 26 probe rays of length `radius` from where the centre ended up. Any surface one of them reaches is
	//      closer than the radius allows (walls the centre was sliding past, corners, things approached sideways): the
	//      centre is pushed straight out of its plane until it is `radius` away. Each push is itself ray-tested so it
	//      cannot carry the centre through something behind it. Twice, so a corner's two walls both get their turn.
	// Stage 2 is what the first version lacked: with only the centre ray the camera could end up a hair from a wall (and
	// see through it - the near clip plane), and the next frame's ray then started on the wrong side of it.
	//
	// A camera that starts inside geometry is never held: only front faces are tested, so moving within or out of a solid
	// reports no hit.
	inline bool sweep(const float* from, float* to, float radius)
	{
		if (!available() || faultedOut()) return false;
		for (int i = 0; i < 3; ++i) if (!std::isfinite(from[i]) || !std::isfinite(to[i])) return false;
		if (!std::isfinite(radius)) radius = kRadius;
		const float R = radius < 0.05f ? 0.05f : (radius > 2.f ? 2.f : radius);

		float pos[3] = { from[0], from[1], from[2] };
		float target[3] = { to[0], to[1], to[2] };
		{
			const float m[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
			if (m[0] * m[0] + m[1] * m[1] + m[2] * m[2] < 1e-12f) return true;   // not moving: nothing to do (to == from)
		}

		// ---- 1. move
		for (int seg = 0; seg < kMaxSlideSegments; ++seg)
		{
			const float d[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
			const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (len < 1e-5f) break;
			const float dir[3] = { d[0] / len, d[1] / len, d[2] / len };
			const float reach = len + R;
			const float end[3] = { pos[0] + dir[0] * reach, pos[1] + dir[1] * reach, pos[2] + dir[2] * reach };

			Hit h;
			if (!testSegment(pos, end, h))
			{
				noteFault();
				if (seg == 0) return false;   // nothing evaluated: behave as without collision
				break;                        // later segment faulted: stay where the safe part ended
			}
			if (!h.hit)
			{
				pos[0] = target[0]; pos[1] = target[1]; pos[2] = target[2];
				break;
			}

			const float along = h.t * reach;
			float cosn = 0.f;
			if (h.hasNormal) cosn = -(dir[0] * h.n[0] + dir[1] * h.n[1] + dir[2] * h.n[2]);
			const bool usable = h.hasNormal && cosn > 1e-3f;   // we must be approaching the front face

			// how far the centre may travel before the sphere touches the plane of what was hit
			float adv = along - (usable ? R / (cosn > 0.02f ? cosn : 0.02f) : R);
			if (adv < 0.f) adv = 0.f;
			if (adv >= len)
			{
				pos[0] = target[0]; pos[1] = target[1]; pos[2] = target[2];   // the surface is beyond what this frame needs
				break;
			}
			pos[0] += dir[0] * adv; pos[1] += dir[1] * adv; pos[2] += dir[2] * adv;

			if (!usable) break;   // dead stop

			// slide: keep the part of the remaining motion that is parallel to the surface
			float rem[3] = { target[0] - pos[0], target[1] - pos[1], target[2] - pos[2] };
			const float remN = rem[0] * h.n[0] + rem[1] * h.n[1] + rem[2] * h.n[2];
			if (remN < 0.f) { rem[0] -= h.n[0] * remN; rem[1] -= h.n[1] * remN; rem[2] -= h.n[2] * remN; }
			target[0] = pos[0] + rem[0]; target[1] = pos[1] + rem[1]; target[2] = pos[2] + rem[2];
		}

		// ---- 2. clearance
		const float (*dirs)[3] = probeDirs();
		constexpr float kSkin = 0.02f;   // a push stops this far short of anything behind it
		for (int pass = 0; pass < 2; ++pass)
		{
			bool pushed = false;
			for (int k = 0; k < 26; ++k)
			{
				const float end[3] = { pos[0] + dirs[k][0] * R, pos[1] + dirs[k][1] * R, pos[2] + dirs[k][2] * R };
				Hit h;
				if (!testSegment(pos, end, h)) { noteFault(); pass = 2; break; }
				if (!h.hit || !h.hasNormal) continue;
				const float cosn = -(dirs[k][0] * h.n[0] + dirs[k][1] * h.n[1] + dirs[k][2] * h.n[2]);
				if (cosn <= 1e-3f) continue;
				const float gap = h.t * R * cosn;   // the centre's distance from that surface's plane
				float push = R - gap;
				if (push <= 1e-4f) continue;

				const float pend[3] = { pos[0] + h.n[0] * (push + kSkin), pos[1] + h.n[1] * (push + kSkin), pos[2] + h.n[2] * (push + kSkin) };
				Hit h2;
				if (!testSegment(pos, pend, h2)) { noteFault(); pass = 2; break; }
				if (h2.hit)
				{
					const float room = h2.t * (push + kSkin) - kSkin;
					if (room < push) push = room;
				}
				if (push <= 1e-4f) continue;
				pos[0] += h.n[0] * push; pos[1] += h.n[1] * push; pos[2] += h.n[2] * push;
				pushed = true;
			}
			if (!pushed) break;
		}

		to[0] = pos[0]; to[1] = pos[1]; to[2] = pos[2];
		return true;
	}

	// ============================================================================================================================
	// SPHERE MOVE THROUGH THE ENGINE'S COLLISION FEATURES (2026-11-13) - how Halo 3's flying camera does it.
	//
	// Halo 3 (halo3.dll 1.3528, c_flying_camera update sub_180215648): when camera collision is on it never ray-casts. It calls
	// sub_1801FFB64(flags, position, displacement, height, radius, ...), which is
	//     sub_1801FE800 = collision features in a sphere around the path: every surface, edge and vertex near it, in one buffer
	//     sub_1801FEF30 = move the sphere through those features, up to 3 slides, giving the new position AND what is left of
	//                     the displacement - so the camera keeps its sideways speed along a wall.
	// Halo 2 kept the first half (sub_18075A250, same buffer: 256 vertices, 256 edges, 256 surfaces) but not the mover - its
	// bipeds went to Havok. So the gather is the engine's and the move is done here, against the gathered SURFACES (each a plane
	// plus a convex polygon of up to 8 vertices): after every small step the sphere is pushed out of each polygon it overlaps,
	// along the line from the polygon's closest point to the centre. That is exact for faces, edges and corners, has no
	// "stop then slide" stage and loses none of the motion along the surface.
	//
	// sub_18075A250(flags, point, radius [xmm2], height [xmm3], 0, ignoreObjectA, ignoreObjectB, features) - flags as
	// kMaskPlayerLike above (0x1 structure, 0x4 instanced geometry, 0x8 objects by type bit).
	// features: +0 u16 vertex count, +2 u16 edge count, +4 u16 surface count; surfaces at +19464, 108 bytes each:
	//   +12 u8 collision surface flags (1 two-sided, 2 INVISIBLE), +16 plane (n, d: n.p - d = 0), +36 i16 dominant axis,
	//   +38 u8 (normal[axis] > 0), +40 i32 vertex count, +44 + 8*i the vertices projected along the axis (2 floats each);
	//   which coordinate each of the two is = the engine's table at halo2+0xB187E0 [6*axis + 3*sign + {0,1}].
	// ============================================================================================================================
	constexpr uintptr_t kRvaFeaturesInSphere = 0x75A250;
	constexpr uintptr_t kRvaProjectionTable = 0xB187E0;
	constexpr size_t kFeaturesSurfaces = 19464, kFeaturesSurfaceSize = 108, kFeaturesMaxSurfaces = 256;

	using FeaturesFn = bool(__fastcall*)(unsigned int flags, const float* point, float radius, float height, int zero, int ignoreA, int ignoreB, void* features);

	struct Poly { float n[3]; float d; int nv; float v[8][3]; bool twoSided; };

	inline std::atomic<int> gFeatState{ 0 };   // 0 not resolved, 1 ready, 2 unavailable
	inline FeaturesFn gFeatures = nullptr;
	inline int16_t gProj[18] = {};
	alignas(16) inline uint8_t gFeatureBuf[64 * 1024];   // the engine writes 47112 bytes at most
	inline Poly gPolys[kFeaturesMaxSurfaces];
	inline int gPolyCount = 0;

	inline bool featuresResolve()
	{
		const int s = gFeatState.load(std::memory_order_acquire);
		if (s != 0) return s == 1;
		bool ok = false;
		const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"halo2.dll"));
		if (base)
		{
			// push rdi; mov eax,2140h; call _alloca_probe (rel32 skipped); sub rsp,rax; mov rdi,[rsp+2188h]
			static const uint8_t a[] = { 0x40, 0x57, 0xB8, 0x40, 0x21, 0x00, 0x00, 0xE8 };
			static const uint8_t b[] = { 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0xBC, 0x24, 0x88, 0x21, 0x00, 0x00 };
			ok = bytesMatch(base + kRvaFeaturesInSphere, a, sizeof(a)) && bytesMatch(base + kRvaFeaturesInSphere + 12, b, sizeof(b));
			if (ok)
			{
				__try { memcpy(gProj, reinterpret_cast<const void*>(base + kRvaProjectionTable), sizeof(gProj)); }
				__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
			}
			// every row must name the two OTHER axes
			for (int ax = 0; ok && ax < 3; ++ax) for (int sg = 0; sg < 2; ++sg)
			{
				const int u = gProj[6 * ax + 3 * sg], v = gProj[6 * ax + 3 * sg + 1];
				if (u < 0 || u > 2 || v < 0 || v > 2 || u == v || u == ax || v == ax) ok = false;
			}
			if (ok) gFeatures = reinterpret_cast<FeaturesFn>(base + kRvaFeaturesInSphere);
		}
		gFeatState.store(ok ? 1 : 2, std::memory_order_release);
		return ok;
	}

	// Fills gPolys with the collision surfaces within `radius` of `centre`. -1 = the call faulted. POD only (SEH).
	inline int gather(const float* centre, float radius)
	{
		gPolyCount = 0;
		float c[3] = { centre[0], centre[1], centre[2] };
		__try
		{
			gFeatures(static_cast<unsigned int>(kMaskPlayerLike), c, radius, 0.f, 0, -1, -1, gFeatureBuf);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
		uint16_t count = 0;
		memcpy(&count, gFeatureBuf + 4, 2);
		if (count > kFeaturesMaxSurfaces) count = kFeaturesMaxSurfaces;
		for (int i = 0; i < count; ++i)
		{
			const uint8_t* r = gFeatureBuf + kFeaturesSurfaces + kFeaturesSurfaceSize * i;
			const uint8_t flags = r[12];
			if (flags & 0x02) continue;   // invisible ("*") surface: blocks the player, not the camera
			Poly& p = gPolys[gPolyCount];
			float plane[4]; memcpy(plane, r + 16, 16);
			int16_t axis = 0; memcpy(&axis, r + 36, 2);
			const int sign = r[38] ? 1 : 0;
			int32_t nv = 0; memcpy(&nv, r + 40, 4);
			if (axis < 0 || axis > 2 || nv < 3) continue;
			if (nv > 8) nv = 8;
			const float na = plane[axis];
			if (!std::isfinite(na) || std::fabs(na) < 1e-4f) continue;
			const int iu = gProj[6 * axis + 3 * sign], iv = gProj[6 * axis + 3 * sign + 1];
			bool good = true;
			for (int j = 0; j < nv; ++j)
			{
				float uv[2]; memcpy(uv, r + 44 + 8 * j, 8);
				p.v[j][iu] = uv[0]; p.v[j][iv] = uv[1];
				p.v[j][axis] = (plane[3] - plane[iu] * uv[0] - plane[iv] * uv[1]) / na;
				if (!std::isfinite(p.v[j][0]) || !std::isfinite(p.v[j][1]) || !std::isfinite(p.v[j][2])) good = false;
			}
			if (!good) continue;
			p.n[0] = plane[0]; p.n[1] = plane[1]; p.n[2] = plane[2]; p.d = plane[3];
			p.nv = nv; p.twoSided = (flags & 0x01) != 0;
			++gPolyCount;
		}
		return count;
	}

	// closest point of a convex polygon to c
	inline void closestOnPoly(const Poly& p, const float* c, float* q)
	{
		const float s = p.n[0] * c[0] + p.n[1] * c[1] + p.n[2] * c[2] - p.d;
		const float pr[3] = { c[0] - p.n[0] * s, c[1] - p.n[1] * s, c[2] - p.n[2] * s };
		bool pos = false, neg = false;
		for (int i = 0; i < p.nv; ++i)
		{
			const float* a = p.v[i]; const float* b = p.v[(i + 1) % p.nv];
			const float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float w[3] = { pr[0] - a[0], pr[1] - a[1], pr[2] - a[2] };
			const float cr = (e[1] * w[2] - e[2] * w[1]) * p.n[0] + (e[2] * w[0] - e[0] * w[2]) * p.n[1] + (e[0] * w[1] - e[1] * w[0]) * p.n[2];
			if (cr > 1e-9f) pos = true; else if (cr < -1e-9f) neg = true;
		}
		if (!(pos && neg)) { q[0] = pr[0]; q[1] = pr[1]; q[2] = pr[2]; return; }   // inside (either winding)
		float best = 3.4e38f;
		for (int i = 0; i < p.nv; ++i)
		{
			const float* a = p.v[i]; const float* b = p.v[(i + 1) % p.nv];
			const float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float l2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
			float t = l2 > 1e-12f ? ((c[0] - a[0]) * e[0] + (c[1] - a[1]) * e[1] + (c[2] - a[2]) * e[2]) / l2 : 0.f;
			t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
			const float x[3] = { a[0] + e[0] * t, a[1] + e[1] * t, a[2] + e[2] * t };
			const float d2 = (c[0] - x[0]) * (c[0] - x[0]) + (c[1] - x[1]) * (c[1] - x[1]) + (c[2] - x[2]) * (c[2] - x[2]);
			if (d2 < best) { best = d2; q[0] = x[0]; q[1] = x[1]; q[2] = x[2]; }
		}
	}

	// pushes the sphere out of every gathered polygon it overlaps
	inline void depenetrate(float* pos, float R)
	{
		for (int pass = 0; pass < 4; ++pass)
		{
			bool pushed = false;
			for (int i = 0; i < gPolyCount; ++i)
			{
				const Poly& p = gPolys[i];
				const float s = p.n[0] * pos[0] + p.n[1] * pos[1] + p.n[2] * pos[2] - p.d;
				if (s < 0.f && !p.twoSided) continue;   // behind a one-sided surface: a camera inside geometry is not held
				if (std::fabs(s) >= R) continue;
				float q[3]; closestOnPoly(p, pos, q);
				const float v[3] = { pos[0] - q[0], pos[1] - q[1], pos[2] - q[2] };
				const float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
				if (d2 >= R * R) continue;
				if (d2 > 1e-10f)
				{
					const float dist = std::sqrt(d2), k = (R - dist) / dist;
					pos[0] += v[0] * k; pos[1] += v[1] * k; pos[2] += v[2] * k;
				}
				else
				{
					const float sg = s >= 0.f ? R : -R;
					pos[0] += p.n[0] * sg; pos[1] += p.n[1] * sg; pos[2] += p.n[2] * sg;
				}
				pushed = true;
			}
			if (!pushed) break;
		}
	}

	// Moves a sphere from `from` towards `to` (in/out). false = could not be evaluated (caller falls back).
	inline bool moveSphere(const float* from, float* to, float radius)
	{
		if (!featuresResolve() || faultedOut()) return false;
		for (int i = 0; i < 3; ++i) if (!std::isfinite(from[i]) || !std::isfinite(to[i])) return false;
		if (!std::isfinite(radius)) radius = kRadius;
		const float R = radius < 0.05f ? 0.05f : (radius > 2.f ? 2.f : radius);

		float pos[3] = { from[0], from[1], from[2] };
		const float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
		float remaining = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (remaining < 1e-6f)
		{
			// standing still: only make sure nothing has moved into the sphere
			if (gather(pos, R + 0.1f) < 0) { noteFault(); return false; }
			depenetrate(pos, R);
			to[0] = pos[0]; to[1] = pos[1]; to[2] = pos[2];
			return true;
		}
		const float dir[3] = { d[0] / remaining, d[1] / remaining, d[2] / remaining };
		const float maxStep = R * 0.5f;   // a step never carries the centre through a surface the sphere was clear of

		for (int chunk = 0; chunk < 16 && remaining > 1e-6f; ++chunk)
		{
			float len = remaining < 0.75f ? remaining : 0.75f;
			// everything the sphere can reach during this chunk (pushes can add to the travel, hence the 1.5)
			int got = gather(pos, len * 1.5f + R + 0.1f);
			if (got >= static_cast<int>(kFeaturesMaxSurfaces) && len > 0.2f) { len = 0.2f; got = gather(pos, len * 1.5f + R + 0.1f); }
			if (got < 0) { noteFault(); if (chunk == 0) return false; break; }

			int steps = static_cast<int>(std::ceil(len / maxStep));
			if (steps < 1) steps = 1;
			const float stepLen = len / steps;
			for (int s = 0; s < steps; ++s)
			{
				pos[0] += dir[0] * stepLen; pos[1] += dir[1] * stepLen; pos[2] += dir[2] * stepLen;
				if (gPolyCount) depenetrate(pos, R);
			}
			remaining -= len;
		}

		to[0] = pos[0]; to[1] = pos[1]; to[2] = pos[2];
		return true;
	}

	// The free camera's collision: the sphere move, or the ray version when the features routine is not the expected code.
	inline bool collide(const float* from, float* to, float radius)
	{
		float t[3] = { to[0], to[1], to[2] };
		if (moveSphere(from, t, radius)) { to[0] = t[0]; to[1] = t[1]; to[2] = t[2]; return true; }
		return sweep(from, to, radius);
	}
}
