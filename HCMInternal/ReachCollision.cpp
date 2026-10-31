#include "pch.h"
#include "ReachCollision.h"
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// Graph (haloreach.dll 1.3528, RVAs from the image base):
//   havok components header  *(base+0xC519B8) = H; components = H + *(u32)(H+0x60), stride 0xC0, count *(i32)(H+0x40);
//                            slot in use if *(u16)comp != 0; rigid bodies: array *(comp+0x30), count *(i32)(comp+0x38),
//                            body[k] = *(array + 0x40 + k*0x70)
//   rigid body               shape *(body+0x20); world transform at +0x170 (rotation columns +0x170/+0x180/+0x190,
//                            position +0x1A0); world = *(body+0x10)
//   hkpWorld                 fixed island *(world+0x30); its entities *(island+0x60), count *(u32)(island+0x68)
//   structure                MOPP (vtable 0xA2D3A8) -> hkpBvCompressedMeshShape *(mopp+0x70) (magic 123456789 at +0x10)
//                            -> shape container at bvcms+0x20 (vtable 0xA2D308): [2] getFirstKey, [3] getNextKey,
//                            [5] getChildShape(key, buffer). Children of vtable 0xA2D558 are flat world-space convex
//                            polygons: vertex count u8 at +0x2E, vertices at +0x40 (stride 16).
//   simple shapes            box 0xA2CB58 (half extents +0x30), convex vertices 0xA2CAC8 (vertices SoA at *(+0x50),
//                            count +0x60, planes *(+0x70) count +0x78), convex transform 0xA2CD08 (child +0x30,
//                            matrix +0x40..+0x70), convex translate 0xA2CC78 (child +0x30, offset +0x40),
//                            lists 0xA2CA60 / 0xA7F438 (children *(+0x30) stride 0x20, count +0x38)
// Instanced geometry (rocks, placed pieces): baked into the same container as ~11k convex pieces, each an
// hkpConvexTransformShape (vtable 0xA2CFE0, same methods as the stock one at 0xA2CD08: child +0x30, transform columns
// +0x40/+0x50/+0x60, translation +0x70) around a scaled convex (0xA2D1D0). Rather than decode that stack (the Havok
// Debugger notes the placement as unsolved), each piece is asked for its own shape through Havok's virtual
// getSupportingVertex (vtable slot 14 = +0x70; verified on the box shape, 0x1808861F0, and on the transform shape,
// 0x180882A60, which maps the direction into its local space, asks its child and maps the vertex back): the support
// points in 26 directions are exactly on the piece's hull, wherever and however the game places it.

namespace ReachCollision
{
	namespace
	{
		constexpr uintptr_t kCompHeader = 0xC519B8;
		constexpr uintptr_t vtStructMopp = 0xA2D3A8, vtContainer = 0xA2D308, vtWorldPoly = 0xA2D558;
		constexpr uintptr_t vtBox = 0xA2CB58, vtConvexVerts = 0xA2CAC8, vtConvexXform = 0xA2CD08, vtConvexTranslate = 0xA2CC78;
		constexpr uintptr_t vtList = 0xA2CA60, vtHkList = 0xA7F438;
		constexpr uintptr_t vtInstPiece = 0xA2CFE0, vtInstScaled = 0xA2D1D0;
		int gInstPieces = 0;

		struct Xf { float r[9]; float o[3]; };

		uintptr_t gBase = 0, gEnd = 0;
		float* gTri = nullptr; int gCap = 0, gN = 0;
		uintptr_t gObjBodies[8192]; int gObjN = 0;
		uintptr_t gSig = 0; int gRetryTick = 0; bool gObjects = false;

		std::mutex gLock;
		std::vector<float> gPublished;
		long gGen = 0;
		char gStatus[200] = "Reach collision: waiting for the game";

		inline bool okp(uintptr_t p) { return p >= 0x10000ull && p < 0x800000000000ull && (p & 7) == 0; }
		inline bool ok4(uintptr_t p) { return p >= 0x10000ull && p < 0x800000000000ull; }
		inline bool inMod(uint64_t p) { return p >= gBase && p < gEnd; }
		inline uint64_t vtOf(uintptr_t p) { return *(uint64_t*)p; }
		inline bool isVt(uint64_t v, uintptr_t rva) { return v == gBase + rva; }

		inline void xp(const Xf& X, float x, float y, float z, float* o)
		{
			o[0] = X.o[0] + X.r[0] * x + X.r[3] * y + X.r[6] * z;
			o[1] = X.o[1] + X.r[1] * x + X.r[4] * y + X.r[7] * z;
			o[2] = X.o[2] + X.r[2] * x + X.r[5] * y + X.r[8] * z;
		}
		Xf compose(const Xf& P, const float cr[9], const float ct[3])
		{
			Xf R;
			for (int j = 0; j < 3; ++j)
			{
				const float* ic = cr + j * 3; float* cc = R.r + j * 3;
				cc[0] = P.r[0] * ic[0] + P.r[3] * ic[1] + P.r[6] * ic[2];
				cc[1] = P.r[1] * ic[0] + P.r[4] * ic[1] + P.r[7] * ic[2];
				cc[2] = P.r[2] * ic[0] + P.r[5] * ic[1] + P.r[8] * ic[2];
			}
			xp(P, ct[0], ct[1], ct[2], R.o);
			return R;
		}
		inline void tri(const float* a, const float* b, const float* c)
		{
			if (gN >= gCap) return;
			float* t = gTri + (size_t)gN * 9;
			memcpy(t, a, 12); memcpy(t + 3, b, 12); memcpy(t + 6, c, 12);
			++gN;
		}

		// ---- simple solid shapes ----
		void boxTris(const Xf& X, float hx, float hy, float hz)
		{
			float c[8][3]; int i = 0;
			for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2) xp(X, sx * hx, sy * hy, sz * hz, c[i++]);
			static const int q[6][4] = { {0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5} };
			for (int f = 0; f < 6; ++f) { tri(c[q[f][0]], c[q[f][1]], c[q[f][2]]); tri(c[q[f][0]], c[q[f][2]], c[q[f][3]]); }
		}
		void convexTris(uintptr_t s, const Xf& X)
		{
			const uintptr_t vd = *(uintptr_t*)(s + 0x50); int nv = *(int*)(s + 0x60);
			const uintptr_t pd = *(uintptr_t*)(s + 0x70); int np = *(int*)(s + 0x78);
			if (!okp(vd) || nv < 4 || nv > 128 || !okp(pd) || np < 4 || np > 256) return;
			float lv[128][3], wv[128][3], ext = 1e-3f;
			for (int k = 0; k < nv; ++k)
			{
				const float* b = (const float*)(vd + (uintptr_t)(k / 4) * 48); const int l = k & 3;
				lv[k][0] = b[l]; lv[k][1] = b[4 + l]; lv[k][2] = b[8 + l];
				xp(X, lv[k][0], lv[k][1], lv[k][2], wv[k]);
				for (int z = 0; z < 3; ++z) if (fabsf(lv[k][z]) > ext) ext = fabsf(lv[k][z]);
			}
			const float eps = ext * 0.01f + 0.05f;
			const float* P = (const float*)pd;
			for (int i = 0; i < np; ++i)
			{
				float nx = P[i * 4], ny = P[i * 4 + 1], nz = P[i * 4 + 2], dd = P[i * 4 + 3];
				const float nl = nx * nx + ny * ny + nz * nz; if (nl < 1e-6f) continue;
				const float inv = 1.f / sqrtf(nl); nx *= inv; ny *= inv; nz *= inv; dd *= inv;
				int idx[128], cnt = 0; float cx = 0, cy = 0, cz = 0;
				for (int k = 0; k < nv; ++k)
					if (fabsf(nx * lv[k][0] + ny * lv[k][1] + nz * lv[k][2] + dd) < eps) { idx[cnt++] = k; cx += lv[k][0]; cy += lv[k][1]; cz += lv[k][2]; }
				if (cnt < 3) continue;
				cx /= cnt; cy /= cnt; cz /= cnt;
				const float hx = fabsf(nx) < 0.9f ? 1.f : 0.f, hz = fabsf(nx) < 0.9f ? 0.f : 1.f;
				float ux = ny * hz, uy = nz * hx - nx * hz, uz = -ny * hx; const float ul = sqrtf(ux * ux + uy * uy + uz * uz); if (ul < 1e-6f) continue;
				ux /= ul; uy /= ul; uz /= ul;
				const float wx = ny * uz - nz * uy, wy = nz * ux - nx * uz, wz = nx * uy - ny * ux;
				float ang[128];
				for (int q = 0; q < cnt; ++q) { const float dx = lv[idx[q]][0] - cx, dy = lv[idx[q]][1] - cy, dz = lv[idx[q]][2] - cz; ang[q] = atan2f(dx * wx + dy * wy + dz * wz, dx * ux + dy * uy + dz * uz); }
				for (int q = 1; q < cnt; ++q) { const float ka = ang[q]; const int ki = idx[q]; int j = q - 1; while (j >= 0 && ang[j] > ka) { ang[j + 1] = ang[j]; idx[j + 1] = idx[j]; --j; } ang[j + 1] = ka; idx[j + 1] = ki; }
				for (int q = 1; q + 1 < cnt; ++q) tri(wv[idx[0]], wv[idx[q]], wv[idx[q + 1]]);
			}
		}
		void solidTris(uintptr_t s, const Xf& X, int depth)
		{
			if (!okp(s) || depth > 8) return;
			const uint64_t vt = vtOf(s);
			if (isVt(vt, vtBox)) { const float* he = (const float*)(s + 0x30); boxTris(X, he[0], he[1], he[2]); }
			else if (isVt(vt, vtConvexVerts)) convexTris(s, X);
			else if (isVt(vt, vtConvexXform))
			{
				const float* c0 = (const float*)(s + 0x40), * c1 = (const float*)(s + 0x50), * c2 = (const float*)(s + 0x60), * t = (const float*)(s + 0x70);
				const float cr[9] = { c0[0],c0[1],c0[2],c1[0],c1[1],c1[2],c2[0],c2[1],c2[2] }, ct[3] = { t[0],t[1],t[2] };
				solidTris(*(uintptr_t*)(s + 0x30), compose(X, cr, ct), depth + 1);
			}
			else if (isVt(vt, vtConvexTranslate))
			{
				const float* t = (const float*)(s + 0x40);
				const float cr[9] = { 1,0,0,0,1,0,0,0,1 }, ct[3] = { t[0],t[1],t[2] };
				solidTris(*(uintptr_t*)(s + 0x30), compose(X, cr, ct), depth + 1);
			}
			else if (isVt(vt, vtList) || isVt(vt, vtHkList))
			{
				const uintptr_t data = *(uintptr_t*)(s + 0x30); const int n = *(int*)(s + 0x38);
				if (okp(data) && n >= 0 && n < 8192) for (int k = 0; k < n; ++k) solidTris(*(uintptr_t*)(data + 0x20 * (uintptr_t)k), X, depth + 1);
			}
		}

		// ---- convex piece from its own support function (instanced geometry) ----
		typedef void(__fastcall* FnSupport)(void*, const float*, float*);
		bool supportSafe(FnSupport f, void* shape, const float* dir, float* out) { __try { f(shape, dir, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

		void pieceTris(uintptr_t shp, const Xf& X)
		{
			void** vt = *(void***)shp;
			const FnSupport sup = (FnSupport)vt[14];
			if (!inMod((uint64_t)sup)) return;
			float radius = *(float*)(shp + 0x20);
			if (!(radius >= 0.f && radius < 1.f)) radius = 0.f;
			static const float D[26][3] = {
				{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
				{1,1,0},{1,-1,0},{-1,1,0},{-1,-1,0},{1,0,1},{1,0,-1},{-1,0,1},{-1,0,-1},{0,1,1},{0,1,-1},{0,-1,1},{0,-1,-1},
				{1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1},{-1,1,1},{-1,1,-1},{-1,-1,1},{-1,-1,-1} };
			float P[26][3]; int n = 0;
			for (int k = 0; k < 26; ++k)
			{
				const float l = 1.f / sqrtf(D[k][0] * D[k][0] + D[k][1] * D[k][1] + D[k][2] * D[k][2]);
				const float wx = D[k][0] * l, wy = D[k][1] * l, wz = D[k][2] * l;
				// world direction -> body-local (transpose of the body rotation)
				alignas(16) float dl[4] = { X.r[0] * wx + X.r[1] * wy + X.r[2] * wz, X.r[3] * wx + X.r[4] * wy + X.r[5] * wz, X.r[6] * wx + X.r[7] * wy + X.r[8] * wz, 0.f };
				alignas(16) float out[4] = { 0, 0, 0, 0 };
				if (!supportSafe(sup, (void*)shp, dl, out)) return;
				if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2])) return;
				float w[3]; xp(X, out[0], out[1], out[2], w);
				w[0] += wx * radius; w[1] += wy * radius; w[2] += wz * radius;
				bool dup = false;
				for (int q = 0; q < n && !dup; ++q) dup = fabsf(P[q][0] - w[0]) + fabsf(P[q][1] - w[1]) + fabsf(P[q][2] - w[2]) < 1e-3f;
				if (!dup) { P[n][0] = w[0]; P[n][1] = w[1]; P[n][2] = w[2]; ++n; }
			}
			if (n < 4) return;
			++gInstPieces;
			// hull faces: every plane through three points with all others on one side (n <= 26)
			float ext = 1e-3f;
			for (int k = 1; k < n; ++k) for (int z = 0; z < 3; ++z) ext = fmaxf(ext, fabsf(P[k][z] - P[0][z]));
			const float eps = ext * 0.002f + 0.002f;
			float fn[64][4]; int nf = 0;
			for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b) for (int c = b + 1; c < n; ++c)
			{
				const float e1[3] = { P[b][0] - P[a][0], P[b][1] - P[a][1], P[b][2] - P[a][2] }, e2[3] = { P[c][0] - P[a][0], P[c][1] - P[a][1], P[c][2] - P[a][2] };
				float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
				const float nl = nx * nx + ny * ny + nz * nz; if (nl < 1e-12f) continue;
				const float inv = 1.f / sqrtf(nl); nx *= inv; ny *= inv; nz *= inv;
				float dd = nx * P[a][0] + ny * P[a][1] + nz * P[a][2];
				int pos = 0, neg = 0;
				for (int k = 0; k < n; ++k) { const float t = nx * P[k][0] + ny * P[k][1] + nz * P[k][2] - dd; if (t > eps) ++pos; else if (t < -eps) ++neg; }
				if (pos && neg) continue;
				if (pos) { nx = -nx; ny = -ny; nz = -nz; dd = -dd; }
				bool dup = false;
				for (int f = 0; f < nf && !dup; ++f) dup = fn[f][0] * nx + fn[f][1] * ny + fn[f][2] * nz > 0.999f && fabsf(fn[f][3] - dd) < eps * 2.f;
				if (dup || nf >= 64) continue;
				fn[nf][0] = nx; fn[nf][1] = ny; fn[nf][2] = nz; fn[nf][3] = dd; ++nf;
				int idx[26], cnt = 0; float cx = 0, cy = 0, cz = 0;
				for (int k = 0; k < n; ++k) if (fabsf(nx * P[k][0] + ny * P[k][1] + nz * P[k][2] - dd) <= eps * 2.f) { idx[cnt++] = k; cx += P[k][0]; cy += P[k][1]; cz += P[k][2]; }
				if (cnt < 3) continue;
				cx /= cnt; cy /= cnt; cz /= cnt;
				const float hx = fabsf(nx) < 0.9f ? 1.f : 0.f, hz = fabsf(nx) < 0.9f ? 0.f : 1.f;
				float ux = ny * hz, uy = nz * hx - nx * hz, uz = -ny * hx; const float ul = sqrtf(ux * ux + uy * uy + uz * uz); if (ul < 1e-6f) continue;
				ux /= ul; uy /= ul; uz /= ul;
				const float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
				float ang[26];
				for (int q = 0; q < cnt; ++q) { const float dx = P[idx[q]][0] - cx, dy = P[idx[q]][1] - cy, dz = P[idx[q]][2] - cz; ang[q] = atan2f(dx * vx + dy * vy + dz * vz, dx * ux + dy * uy + dz * uz); }
				for (int q = 1; q < cnt; ++q) { const float ka = ang[q]; const int ki = idx[q]; int j = q - 1; while (j >= 0 && ang[j] > ka) { ang[j + 1] = ang[j]; idx[j + 1] = idx[j]; --j; } ang[j + 1] = ka; idx[j + 1] = ki; }
				for (int q = 1; q + 1 < cnt; ++q) tri(P[idx[0]], P[idx[q]], P[idx[q + 1]]);
			}
		}

		// ---- structure mesh through the engine's getChildShape (game thread only) ----
		typedef uint32_t(__fastcall* FnFirstKey)(void*);
		typedef uint32_t(__fastcall* FnNextKey)(void*, uint32_t);
		typedef uintptr_t(__fastcall* FnChildShape)(void*, uint32_t, void*);

		// one bad key must not end the whole walk
		uintptr_t childSafe(FnChildShape f, void* c, uint32_t k, void* buf) { __try { return f(c, k, buf); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
		uint32_t nextSafe(FnNextKey f, void* c, uint32_t k) { __try { return f(c, k); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; } }

		void structureTris(uintptr_t mopp, const Xf& X)
		{
			uintptr_t bv = *(uintptr_t*)(mopp + 0x70);
			if (!(okp(bv) && *(uint32_t*)(bv + 0x10) == 123456789u))
			{
				bv = 0;
				for (int o = 0x08; o <= 0x100 && !bv; o += 8) { const uintptr_t p = *(uintptr_t*)(mopp + o); if (okp(p) && *(uint32_t*)(p + 0x10) == 123456789u) bv = p; }
			}
			if (!bv) return;
			const uintptr_t cont = bv + 0x20;
			if (!isVt(vtOf(cont), vtContainer)) return;
			void** vt = *(void***)cont;
			const FnFirstKey first = (FnFirstKey)vt[2]; const FnNextKey next = (FnNextKey)vt[3]; const FnChildShape child = (FnChildShape)vt[5];
			if (!inMod((uint64_t)first) || !inMod((uint64_t)next) || !inMod((uint64_t)child)) return;
			alignas(16) unsigned char buf[512];
			uint32_t key = first((void*)cont);
			for (int guard = 0; key != 0xFFFFFFFFu && guard < 2000000 && gN < gCap; ++guard)
			{
				const uintptr_t shp = childSafe(child, (void*)cont, key, buf);
				if (okp(shp) && (isVt(vtOf(shp), vtInstPiece) || isVt(vtOf(shp), vtInstScaled)))
					pieceTris(shp, X);
				else if (okp(shp) && isVt(vtOf(shp), vtWorldPoly))
				{
					const int nv = *(unsigned char*)(shp + 0x2E);
					if (nv >= 3 && nv <= 64)
					{
						float w[64][3];
						for (int v = 0; v < nv; ++v) { const float* p = (const float*)(shp + 0x40 + (uintptr_t)v * 16); xp(X, p[0], p[1], p[2], w[v]); }
						for (int v = 1; v + 1 < nv; ++v) tri(w[0], w[v], w[v + 1]);
					}
				}
				key = nextSafe(next, (void*)cont, key);
			}
		}

		bool bodyXf(uintptr_t body, Xf& X)
		{
			const float* T = (const float*)(body + 0x170);
			X.r[0] = T[0]; X.r[1] = T[1]; X.r[2] = T[2]; X.r[3] = T[4]; X.r[4] = T[5]; X.r[5] = T[6]; X.r[6] = T[8]; X.r[7] = T[9]; X.r[8] = T[10];
			X.o[0] = T[12]; X.o[1] = T[13]; X.o[2] = T[14];
			for (float f : X.r) if (!std::isfinite(f)) return false;
			return std::isfinite(X.o[0]) && std::isfinite(X.o[1]) && std::isfinite(X.o[2]);
		}
		bool isRigidBody(uintptr_t b) { return okp(b) && inMod(vtOf(b)) && okp(*(uintptr_t*)(b + 0x20)); }

		int cmpU(const void* a, const void* b) { const uintptr_t x = *(const uintptr_t*)a, y = *(const uintptr_t*)b; return x < y ? -1 : x > y ? 1 : 0; }
		bool isObjectBody(uintptr_t b)
		{
			int lo = 0, hi = gObjN - 1;
			while (lo <= hi) { const int m = (lo + hi) >> 1; if (gObjBodies[m] == b) return true; if (gObjBodies[m] < b) lo = m + 1; else hi = m - 1; }
			return false;
		}

		// the object (component) bodies, and the world from any of them
		uintptr_t scanComponents()
		{
			gObjN = 0;
			const uintptr_t H = *(uintptr_t*)(gBase + kCompHeader);
			if (!ok4(H)) return 0;
			const uintptr_t base = H + *(uint32_t*)(H + 0x60); const int count = *(int*)(H + 0x40);
			if (!ok4(base) || count < 0 || count > 262144) return 0;
			uintptr_t world = 0;
			for (int i = 0; i < count; ++i)
			{
				const uintptr_t comp = base + (uintptr_t)i * 0xC0;
				if (*(unsigned short*)comp == 0) continue;
				const uintptr_t rb = *(uintptr_t*)(comp + 0x30); const int rc = *(int*)(comp + 0x38);
				if (!ok4(rb) || rc <= 0 || rc >= 64) continue;
				for (int k = 0; k < rc; ++k)
				{
					const uintptr_t b = *(uintptr_t*)(rb + 0x40 + (uintptr_t)k * 0x70);
					if (!okp(b)) continue;
					if (gObjN < 8192) gObjBodies[gObjN++] = b;
					if (!world && isRigidBody(b)) { const uintptr_t w = *(uintptr_t*)(b + 0x10); if (okp(w)) world = w; }
				}
			}
			if (gObjN > 1) qsort(gObjBodies, (size_t)gObjN, sizeof(uintptr_t), cmpU);
			return world;
		}

		// one SEH-guarded pass (POD only in here); returns false if it faulted
		bool gather(uintptr_t world, bool objects, int& structureBodies, int& otherBodies)
		{
			__try
			{
				gN = 0; structureBodies = 0; otherBodies = 0; gInstPieces = 0;
				const uintptr_t fi = *(uintptr_t*)(world + 0x30);
				if (!okp(fi)) return true;
				const uintptr_t ents = *(uintptr_t*)(fi + 0x60); const uint32_t n = *(uint32_t*)(fi + 0x68);
				if (!okp(ents) || n > 100000) return true;
				for (uint32_t j = 0; j < n && gN < gCap; ++j)
				{
					const uintptr_t body = *(uintptr_t*)(ents + 8ull * j);
					if (!isRigidBody(body)) continue;
					Xf X; if (!bodyXf(body, X)) continue;
					const uintptr_t shp = *(uintptr_t*)(body + 0x20);
					if (isVt(vtOf(shp), vtStructMopp)) { structureTris(shp, X); ++structureBodies; }
					else if (!isObjectBody(body) || objects) { solidTris(shp, X, 0); ++otherBodies; }
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		uintptr_t scanGuarded() { __try { return scanComponents(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
		uint32_t fixedCount(uintptr_t world)
		{
			__try { const uintptr_t fi = *(uintptr_t*)(world + 0x30); return okp(fi) ? *(uint32_t*)(fi + 0x68) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		void publish()
		{
			std::scoped_lock lk(gLock);
			gPublished.assign(gTri, gTri + (size_t)gN * 9);
			++gGen;
		}
		void setStatus(const char* s) { std::scoped_lock lk(gLock); strncpy_s(gStatus, s, _TRUNCATE); }
	}

	void tick(uintptr_t haloreachBase, bool includeObjects)
	{
		if (!haloreachBase) return;
		if (haloreachBase != gBase)
		{
			gBase = haloreachBase;
			const IMAGE_DOS_HEADER* d = (const IMAGE_DOS_HEADER*)gBase;
			const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(gBase + d->e_lfanew);
			gEnd = gBase + nt->OptionalHeader.SizeOfImage;
			gSig = 0;
		}
		if (!gTri)
		{
			gCap = 1500000;   // 54 MB of triangles; a Reach level's structure is well under this
			gTri = (float*)malloc((size_t)gCap * 9 * sizeof(float));
			if (!gTri) { gCap = 0; setStatus("Reach collision: OFF - out of memory"); return; }
		}
		if (gRetryTick > 0) { --gRetryTick; return; }
		const uintptr_t world = scanGuarded();
		if (!world) { setStatus("Reach collision: waiting for the Havok world (no physics bodies yet)"); gRetryTick = 30; return; }
		const uintptr_t sig = world ^ ((uintptr_t)fixedCount(world) * 2654435761u) ^ (includeObjects ? 1u : 0u);
		if (sig == gSig) return;
		int sb = 0, ob = 0;
		const bool ok = gather(world, includeObjects, sb, ob);
		char msg[200];
		if (!ok) { snprintf(msg, sizeof(msg), "Reach collision: the world walk faulted - retrying"); setStatus(msg); gRetryTick = 90; return; }
		gSig = sig;
		gObjects = includeObjects;
		publish();
		snprintf(msg, sizeof(msg), "Reach collision: %d triangles (%d structure bodies, %d rocks/placed pieces, %d other fixed bodies)%s", gN, sb, gInstPieces, ob,
			gN == 0 ? " - none found" : "");
		setStatus(msg);
		if (gN == 0) { gSig = 0; gRetryTick = 90; }
	}

	bool snapshot(std::vector<float>& out, long& gen)
	{
		std::scoped_lock lk(gLock);
		if (gen == gGen) return false;
		out = gPublished;
		gen = gGen;
		return true;
	}

	std::string status()
	{
		std::scoped_lock lk(gLock);
		return gStatus;
	}
}
