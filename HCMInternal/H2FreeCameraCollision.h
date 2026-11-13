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
		if (!available() || gFaults.load(std::memory_order_relaxed) >= 3) return false;
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
				gFaults.fetch_add(1);
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
				if (!testSegment(pos, end, h)) { gFaults.fetch_add(1); pass = 2; break; }
				if (!h.hit || !h.hasNormal) continue;
				const float cosn = -(dirs[k][0] * h.n[0] + dirs[k][1] * h.n[1] + dirs[k][2] * h.n[2]);
				if (cosn <= 1e-3f) continue;
				const float gap = h.t * R * cosn;   // the centre's distance from that surface's plane
				float push = R - gap;
				if (push <= 1e-4f) continue;

				const float pend[3] = { pos[0] + h.n[0] * (push + kSkin), pos[1] + h.n[1] * (push + kSkin), pos[2] + h.n[2] * (push + kSkin) };
				Hit h2;
				if (!testSegment(pos, pend, h2)) { gFaults.fetch_add(1); pass = 2; break; }
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
}
