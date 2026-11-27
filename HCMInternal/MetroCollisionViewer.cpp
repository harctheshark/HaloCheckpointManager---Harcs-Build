#include "pch.h"
#include "MetroCollisionViewer.h"
#include "MetroEngine.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"
#include "DirectXRenderEvent.h"
#include "H2CV_Renderer.h"
#include <thread>

namespace
{
	constexpr uintptr_t kWorldRVA = 0xFBD8A0;      // physics world*
	constexpr uintptr_t kWorldSceneOff = 0xF8;     // PxScene*
	constexpr uintptr_t kCamPosRVA = 0xFC1170;     // float4
	constexpr uintptr_t kCamViewProjRVA = 0xFC1230; // 4x4, row vectors

	// PhysX 3.3 vtable byte offsets (see the header)
	constexpr uintptr_t kSceneGetNbActors = 136, kSceneGetActors = 144, kSceneLockRead = 792, kSceneUnlockRead = 800;
	constexpr uintptr_t kBaseIsKindOf = 32;
	constexpr uintptr_t kActorGetGlobalPose = 160, kActorGetNbShapes = 200, kActorGetShapes = 208;
	constexpr uintptr_t kShapeGetType = 40, kShapeGetBox = 64, kShapeGetSphere = 72, kShapeGetCapsule = 80, kShapeGetConvex = 96, kShapeGetTriMesh = 104, kShapeGetLocalPose = 136;
	constexpr uintptr_t kMeshNbVerts = 40, kMeshVerts = 48, kMeshNbTris = 56, kMeshTris = 64;
	constexpr uintptr_t kConvexNbVerts = 40, kConvexVerts = 48, kConvexIndexBuffer = 56, kConvexNbPolys = 64, kConvexPolyData = 72;
	constexpr uint16_t kRigidStatic = 1, kRigidDynamic = 2;
	enum GeomType { Sphere = 0, Plane = 1, Capsule = 2, Box = 3, Convex = 4, TriMesh = 5, HeightField = 6 };

	constexpr float kCell = 16.f;
	constexpr size_t kMaxStaticTris = 4'000'000, kMaxDynTris = 400'000;
	constexpr uint32_t kMaxActors = 200'000;

	struct V3 { float x, y, z; };
	struct Q4 { float x, y, z, w; };
	struct Xf { Q4 q; V3 p; };
	inline V3 add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3 sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3 mul(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3 rot(Q4 q, V3 v) { const V3 u{ q.x, q.y, q.z }; const V3 t = mul(cross(u, v), 2.f); return add(add(v, mul(t, q.w)), cross(u, t)); }
	inline Q4 conj(Q4 q) { return { -q.x, -q.y, -q.z, q.w }; }
	inline Q4 qmul(Q4 a, Q4 b) { return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z }; }
	inline V3 xf(const Xf& t, V3 v) { return add(rot(t.q, v), t.p); }
	inline Xf compose(const Xf& a, const Xf& b) { return { qmul(a.q, b.q), xf(a, b.p) }; }
	bool finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::abs(v.x) < 1e5f && std::abs(v.y) < 1e5f && std::abs(v.z) < 1e5f; }
	bool okXf(const Xf& t) { const float n = t.q.x * t.q.x + t.q.y * t.q.y + t.q.z * t.q.z + t.q.w * t.q.w; return std::isfinite(n) && std::abs(n - 1.f) < 0.05f && finite(t.p); }

	enum : uint8_t { kFloor, kWall, kCeiling, kDynamic };
	struct Tri { V3 a, b, c; uint8_t kind; };

	// ---- guarded engine calls ----
	bool inModule(uintptr_t p)
	{
		HMODULE m = nullptr;
		return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m) && m;
	}
	using fn5_t = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
	bool sehCall(uintptr_t fn, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t* out)
	{
		__try { *out = ((fn5_t)fn)(a, b, c, d, e); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	// obj->vtbl[off](obj, b, c, d, e); false if the slot is not code in a loaded module or the call faulted
	bool vcall(uintptr_t obj, uintptr_t off, uintptr_t& out, uintptr_t b = 0, uintptr_t c = 0, uintptr_t d = 0, uintptr_t e = 0)
	{
		const uintptr_t vt = metro::readPtr(obj);
		const uintptr_t fn = vt ? metro::readPtr(vt + off) : 0;
		if (!fn || !inModule(fn)) return false;
		return sehCall(fn, obj, b, c, d, e, &out);
	}
	bool vcallU32(uintptr_t obj, uintptr_t off, uint32_t& out, uintptr_t b = 0) { uintptr_t r = 0; if (!vcall(obj, off, r, b)) return false; out = (uint32_t)r; return true; }
	bool vcallBool(uintptr_t obj, uintptr_t off, uintptr_t b) { uintptr_t r = 0; return vcall(obj, off, r, b) && (r & 0xFF) != 0; }
	bool getPose(uintptr_t obj, uintptr_t off, Xf& out)
	{
		alignas(16) Xf t{};
		uintptr_t r = 0;
		if (!vcall(obj, off, r, (uintptr_t)&t)) return false;
		out = t;
		return okXf(t);
	}
	bool copyMem(void* dst, uintptr_t src, size_t n) { return metro::okPtr(src) && metro::detail::sehRead(dst, (const void*)src, n); }

	// ---- shapes -> world triangles ----
	struct Builder
	{
		std::vector<Tri>& out;
		size_t cap;
		uint8_t kindOverride;   // kDynamic, or 0xFF = classify by normal later
		bool full() const { return out.size() >= cap; }
		void tri(V3 a, V3 b, V3 c) { if (!full() && finite(a) && finite(b) && finite(c)) out.push_back({ a, b, c, kindOverride }); }
		void quad(V3 a, V3 b, V3 c, V3 d) { tri(a, b, c); tri(a, c, d); }
	};

	void addBox(Builder& bl, const Xf& w, V3 h)
	{
		V3 p[8];
		for (int i = 0; i < 8; ++i) p[i] = xf(w, { (i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z });
		bl.quad(p[0], p[1], p[3], p[2]); bl.quad(p[4], p[6], p[7], p[5]);
		bl.quad(p[0], p[4], p[5], p[1]); bl.quad(p[2], p[3], p[7], p[6]);
		bl.quad(p[0], p[2], p[6], p[4]); bl.quad(p[1], p[5], p[7], p[3]);
	}

	// a capsule along local X (PhysX), or a sphere when halfHeight == 0
	void addCapsule(Builder& bl, const Xf& w, float r, float hh)
	{
		constexpr int kSeg = 10, kRings = 4;
		auto ring = [&](float x, float rr, int s) { const float a = 6.2831853f * s / kSeg; return xf(w, { x, rr * std::cos(a), rr * std::sin(a) }); };
		for (int side = -1; side <= 1; side += 2)
			for (int k = 0; k < kRings; ++k)
			{
				const float a0 = 1.5707963f * k / kRings, a1 = 1.5707963f * (k + 1) / kRings;
				const float x0 = side * (hh + r * std::sin(a0)), x1 = side * (hh + r * std::sin(a1));
				const float r0 = r * std::cos(a0), r1 = r * std::cos(a1);
				for (int s = 0; s < kSeg; ++s) bl.quad(ring(x0, r0, s), ring(x0, r0, s + 1), ring(x1, r1, s + 1), ring(x1, r1, s));
			}
		if (hh > 0.f) for (int s = 0; s < kSeg; ++s) bl.quad(ring(-hh, r, s), ring(hh, r, s), ring(hh, r, s + 1), ring(-hh, r, s + 1));
	}

	// PxMeshScale {vec3 scale; quat rotation}: v' = R^T S R v
	V3 meshScale(V3 v, V3 s, Q4 q) { V3 a = rot(q, v); a = { a.x * s.x, a.y * s.y, a.z * s.z }; return rot(conj(q), a); }

	bool addTriMesh(Builder& bl, const Xf& w, const uint8_t* geom)
	{
		V3 s; Q4 q; uintptr_t mesh = 0;
		memcpy(&s, geom + 4, 12); memcpy(&q, geom + 16, 16); memcpy(&mesh, geom + 40, 8);
		if (!metro::okPtr(mesh)) return false;
		uint32_t nv = 0, nt = 0; uintptr_t pv = 0, pt = 0;
		if (!vcallU32(mesh, kMeshNbVerts, nv) || !vcallU32(mesh, kMeshNbTris, nt) || !vcall(mesh, kMeshVerts, pv) || !vcall(mesh, kMeshTris, pt)) return false;
		if (nv == 0 || nt == 0 || nv > 10'000'000 || nt > 10'000'000) return false;
		std::vector<V3> verts(nv);
		if (!copyMem(verts.data(), pv, nv * sizeof(V3))) return false;
		// 32- or 16-bit indices: 32-bit only if every index is in range (16-bit pairs read as 32 overflow at once)
		std::vector<uint32_t> idx(size_t(nt) * 3);
		bool ok32 = copyMem(idx.data(), pt, idx.size() * 4);
		if (ok32) for (uint32_t i : idx) if (i >= nv) { ok32 = false; break; }
		if (!ok32)
		{
			std::vector<uint16_t> i16(size_t(nt) * 3);
			if (!copyMem(i16.data(), pt, i16.size() * 2)) return false;
			for (size_t i = 0; i < i16.size(); ++i) { if (i16[i] >= nv) return false; idx[i] = i16[i]; }
		}
		const bool scaled = !(s.x == 1.f && s.y == 1.f && s.z == 1.f);
		for (auto& v : verts) v = xf(w, scaled ? meshScale(v, s, q) : v);
		for (size_t t = 0; t < nt && !bl.full(); ++t) bl.tri(verts[idx[t * 3]], verts[idx[t * 3 + 1]], verts[idx[t * 3 + 2]]);
		return true;
	}

	bool addConvex(Builder& bl, const Xf& w, const uint8_t* geom)
	{
		V3 s; Q4 q; uintptr_t mesh = 0;
		memcpy(&s, geom + 4, 12); memcpy(&q, geom + 16, 16); memcpy(&mesh, geom + 32, 8);
		if (!metro::okPtr(mesh)) return false;
		uint32_t nv = 0, np = 0; uintptr_t pv = 0, pi = 0;
		if (!vcallU32(mesh, kConvexNbVerts, nv) || !vcallU32(mesh, kConvexNbPolys, np) || !vcall(mesh, kConvexVerts, pv) || !vcall(mesh, kConvexIndexBuffer, pi)) return false;
		if (nv == 0 || nv > 256 || np == 0 || np > 256) return false;
		std::vector<V3> verts(nv);
		if (!copyMem(verts.data(), pv, nv * sizeof(V3))) return false;
		for (auto& v : verts) v = xf(w, meshScale(v, s, q));
		for (uint32_t p = 0; p < np; ++p)
		{
			struct { float plane[4]; uint16_t nb, base; } poly{};
			uintptr_t r = 0;   // bool getPolygonData(PxU32 index, PxHullPolygon& out)
			if (!vcall(mesh, kConvexPolyData, r, p, (uintptr_t)&poly) || !(r & 0xFF)) return false;
			if (poly.nb < 3 || poly.nb > 64) continue;
			std::vector<uint8_t> ix(poly.nb);
			if (!copyMem(ix.data(), pi + poly.base, poly.nb)) return false;
			for (uint8_t i : ix) if (i >= nv) return false;
			for (int k = 1; k + 1 < poly.nb; ++k) bl.tri(verts[ix[0]], verts[ix[k]], verts[ix[k + 1]]);
		}
		return true;
	}

	void addShape(Builder& bl, uintptr_t shape, const Xf& actorPose)
	{
		uint32_t type = 0;
		if (!vcallU32(shape, kShapeGetType, type)) return;
		Xf local{};
		if (!getPose(shape, kShapeGetLocalPose, local)) return;
		const Xf w = compose(actorPose, local);
		alignas(16) uint8_t g[64]{};
		uintptr_t r = 0;
		switch (type)
		{
		case Box:      if (vcall(shape, kShapeGetBox, r, (uintptr_t)g) && (r & 0xFF)) { V3 h; memcpy(&h, g + 4, 12); if (finite(h)) addBox(bl, w, h); } break;
		case Sphere:   if (vcall(shape, kShapeGetSphere, r, (uintptr_t)g) && (r & 0xFF)) { float rr; memcpy(&rr, g + 4, 4); if (rr > 0 && rr < 1e3f) addCapsule(bl, w, rr, 0.f); } break;
		case Capsule:  if (vcall(shape, kShapeGetCapsule, r, (uintptr_t)g) && (r & 0xFF)) { float rr, hh; memcpy(&rr, g + 4, 4); memcpy(&hh, g + 8, 4); if (rr > 0 && rr < 1e3f && hh >= 0 && hh < 1e3f) addCapsule(bl, w, rr, hh); } break;
		case Convex:   if (vcall(shape, kShapeGetConvex, r, (uintptr_t)g) && (r & 0xFF)) addConvex(bl, w, g); break;
		case TriMesh:  if (vcall(shape, kShapeGetTriMesh, r, (uintptr_t)g) && (r & 0xFF)) addTriMesh(bl, w, g); break;
		default: break;   // planes and height fields are not drawn
		}
	}

	void addActor(Builder& bl, uintptr_t actor)
	{
		if (!vcallBool(actor, kBaseIsKindOf, (uintptr_t)"PxRigidActor")) return;
		Xf pose{};
		if (!getPose(actor, kActorGetGlobalPose, pose)) return;
		uint32_t ns = 0;
		if (!vcallU32(actor, kActorGetNbShapes, ns) || ns == 0 || ns > 4096) return;
		std::vector<uintptr_t> shapes(ns);
		uintptr_t got = 0;
		if (!vcall(actor, kActorGetShapes, got, (uintptr_t)shapes.data(), ns, 0)) return;
		for (uint32_t i = 0; i < std::min<uint32_t>((uint32_t)got, ns) && !bl.full(); ++i)
			if (metro::okPtr(shapes[i])) addShape(bl, shapes[i], pose);
	}


	// ---- triangles -> deduplicated edges (2026-11-25, user: "skip duplicated geometry like sub divisions") ----
	struct Edge { V3 a, b; uint8_t kind; };

	inline int32_t quant(float v) { return (int32_t)std::lround((double)v * 1000.0); }   // 1 mm
	inline uint64_t mix64(uint64_t x) { x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; x ^= x >> 31; return x; }
	uint64_t pointKey(V3 p) { return mix64(((uint64_t)(uint32_t)quant(p.x) * 0x9E3779B97F4A7C15ull) ^ mix64((uint64_t)(uint32_t)quant(p.y) + 0x632BE59BD9B4E019ull) ^ mix64((uint64_t)(uint32_t)quant(p.z) * 0xD6E8FEB86659FD93ull)); }

	// every edge once; with hideFlat, an edge shared only by triangles lying in one plane (a flat surface's internal seam) is
	// left out entirely, so a flat floor shows as its outline
	std::vector<Edge> buildEdges(const std::vector<Tri>& tris, bool hideFlat)
	{
		struct Rec { uint64_t key; uint32_t tri; uint8_t e; };
		std::vector<Rec> recs;
		recs.reserve(tris.size() * 3);
		std::vector<V3> normals(tris.size());
		for (uint32_t i = 0; i < tris.size(); ++i)
		{
			const Tri& t = tris[i];
			V3 n = cross(sub(t.b, t.a), sub(t.c, t.a));
			const float l = std::sqrt(dot(n, n));
			normals[i] = l > 1e-12f ? mul(n, 1.f / l) : V3{ 0, 0, 0 };
			const V3 v[3] = { t.a, t.b, t.c };
			for (uint8_t e = 0; e < 3; ++e)
			{
				uint64_t k0 = pointKey(v[e]), k1 = pointKey(v[(e + 1) % 3]);
				if (k0 > k1) std::swap(k0, k1);
				recs.push_back({ mix64(k0 ^ mix64(k1 + 0x9E3779B97F4A7C15ull)), i, e });
			}
		}
		std::sort(recs.begin(), recs.end(), [](const Rec& x, const Rec& y) { return x.key < y.key; });
		std::vector<Edge> out;
		out.reserve(recs.size() / 2 + 16);
		for (size_t i = 0; i < recs.size();)
		{
			size_t j = i + 1;
			while (j < recs.size() && recs[j].key == recs[i].key) ++j;
			bool flat = false;
			if (hideFlat && j - i >= 2)
			{
				flat = true;
				const V3 n0 = normals[recs[i].tri];
				const V3 p0 = tris[recs[i].tri].a;
				for (size_t k = i + 1; k < j && flat; ++k)
				{
					const V3 nk = normals[recs[k].tri];
					// same plane: parallel normals (either winding) and the other triangle's corner on this plane (2 mm)
					if (std::fabs(dot(n0, nk)) < 0.9995f || std::fabs(dot(n0, sub(tris[recs[k].tri].a, p0))) > 0.002f) flat = false;
				}
			}
			if (!flat)
			{
				const Tri& t = tris[recs[i].tri];
				const V3 v[3] = { t.a, t.b, t.c };
				out.push_back({ v[recs[i].e], v[(recs[i].e + 1) % 3], t.kind });
			}
			i = j;
		}
		return out;
	}

	constexpr uint32_t kColours[4] = { 0x005AFF5Au, 0x00C8AA96u, 0x005A5AFFu, 0x0032A0FFu };   // 0x00BBGGRR: floor, wall, ceiling, dynamic

	struct Cache
	{
		uintptr_t scene = 0;
		uint32_t nStatic = 0;
		std::shared_ptr<const std::vector<Tri>> staticTris;   // classified
		uint64_t staticVersion = 0;
		std::vector<Tri> dyn;
	};

	// what the renderer draws: line lists (pairs of vertices) built by the worker
	struct Lines
	{
		std::shared_ptr<const std::vector<h2cv::LineVertex>> stat;   // kept on the GPU, re-uploaded only when key changes
		uint64_t key = 0;
		std::vector<h2cv::LineVertex> dyn;    // re-uploaded every frame (small)
		size_t staticEdgesTotal = 0;
	};
}

class MetroCollisionViewer::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::atomic<bool> mReady{ false };

	std::mutex mLinesMutex;
	std::shared_ptr<const Lines> mLines;   // what the renderer draws
	std::atomic<bool> mRun{ false };
	std::thread mWorker;
	std::atomic<bool> mInLevel{ false };
	bool mWarned = false;
	h2cv::Renderer mR;                     // render thread only
	bool mRendererFailed = false;

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	static uintptr_t scene() { const uintptr_t w = metro::readPtr(metro::base() + kWorldRVA); return w ? metro::readPtr(w + kWorldSceneOff) : 0; }

	// the player's own capsule actor (not drawn - it would surround the camera)
	static uintptr_t playerActor()
	{
		const uintptr_t pl = metro::player();
		const uintptr_t mover = pl ? metro::readPtr(pl + 0x4F8) : 0;
		const uintptr_t shell = mover ? metro::readPtr(mover + 8) : 0;
		const uintptr_t ctrl = shell ? metro::readPtr(shell + 200) : 0;
		return ctrl ? metro::readPtr(ctrl + 224) : 0;
	}

	static bool getActors(uintptr_t sc, uint16_t types, std::vector<uintptr_t>& out)
	{
		// 2026-11-25 live probe: PxActorTypeFlags (a PxFlags class with a copy constructor) is passed BY ADDRESS -
		// getNbActors starts `movzx r11d, word ptr [rdx]`. Passing the value faulted, so the viewer listed nothing.
		const uint16_t flags = types;
		uint32_t n = 0;
		if (!vcallU32(sc, kSceneGetNbActors, n, (uintptr_t)&flags) || n > kMaxActors) return false;
		out.assign(n, 0);
		if (n == 0) return true;
		uintptr_t got = 0;
		if (!vcall(sc, kSceneGetActors, got, (uintptr_t)&flags, (uintptr_t)out.data(), n, 0)) return false;
		out.resize(std::min<size_t>((uint32_t)got, n));
		return true;
	}

	// classify a static triangle by its normal; flip = the mesh winding puts normals inward
	static uint8_t classify(const Tri& t, bool flip)
	{
		V3 n = cross(sub(t.b, t.a), sub(t.c, t.a));
		const float l = std::sqrt(dot(n, n));
		if (!(l > 1e-9f)) return kWall;
		const float ny = (flip ? -n.y : n.y) / l;
		return ny > 0.7f ? kFloor : ny < -0.7f ? kCeiling : kWall;
	}

	static bool cameraPos(V3& out)
	{
		float c[4];
		if (!metro::read(metro::base() + kCamPosRVA, c)) return false;
		out = { c[0], c[1], c[2] };
		return finite(out);
	}

	// the static line list for this camera: edges within range, nearest `maxEdges`, alpha fading with distance
	static void filterStatic(const std::vector<Edge>& edges, V3 cam, float range, size_t maxEdges, std::vector<h2cv::LineVertex>& out)
	{
		struct Pick { float d2; uint32_t i; };
		std::vector<Pick> picks;
		picks.reserve(edges.size());
		const float r2 = range * range;
		for (uint32_t i = 0; i < edges.size(); ++i)
		{
			const V3 m = mul(add(edges[i].a, edges[i].b), 0.5f), d = sub(m, cam);
			const float d2 = dot(d, d);
			if (d2 <= r2) picks.push_back({ d2, i });
		}
		if (picks.size() > maxEdges)
		{
			std::nth_element(picks.begin(), picks.begin() + maxEdges, picks.end(), [](const Pick& x, const Pick& y) { return x.d2 < y.d2; });
			picks.resize(maxEdges);
		}
		out.clear();
		out.reserve(picks.size() * 2);
		for (const Pick& p : picks)
		{
			const Edge& e = edges[p.i];
			const float fade = 1.f - std::clamp(std::sqrt(p.d2) / range, 0.f, 1.f);
			const uint32_t rgba = kColours[e.kind & 3] | ((uint32_t)(60.f + 190.f * fade) << 24);
			out.push_back({ e.a.x, e.a.y, e.a.z, rgba });
			out.push_back({ e.b.x, e.b.y, e.b.z, rgba });
		}
	}

	void workerLoop()
	{
		std::shared_ptr<Cache> cur;
		// the static edge set and the last filter inputs
		std::shared_ptr<const std::vector<Tri>> edgesFor;
		bool edgesFlat = true;
		std::vector<Edge> edges;
		V3 lastCam{ 1e9f, 1e9f, 1e9f };
		float lastRange = -1.f; int lastBudget = -1;
		uint64_t lastStaticVersion = ~0ull, versionCounter = 0, lineKey = 0;
		std::shared_ptr<std::vector<h2cv::LineVertex>> statLines = std::make_shared<std::vector<h2cv::LineVertex>>();
		bool allInRange = false;

		while (mRun.load())
		{
			const auto t0 = std::chrono::steady_clock::now();
			auto s = settingsWeak.lock();
			if (mInLevel.load() && s)
			{
				const uintptr_t sc = scene();
				uintptr_t dummy = 0;
				if (sc && vcall(sc, kSceneLockRead, dummy, 0, 0))
				{
					std::vector<uintptr_t> statics, dyns;
					const bool okS = getActors(sc, kRigidStatic, statics);
					const bool okD = getActors(sc, kRigidDynamic, dyns);
					auto next = std::make_shared<Cache>();
					if (cur && cur->scene == sc && okS && cur->nStatic == statics.size())
					{
						next->scene = sc; next->nStatic = cur->nStatic; next->staticTris = cur->staticTris; next->staticVersion = cur->staticVersion;   // statics unchanged
					}
					else if (okS)
					{
						auto tris = std::make_shared<std::vector<Tri>>();
						Builder bl{ *tris, kMaxStaticTris, 0xFF };
						for (uintptr_t a : statics) { if (bl.full()) break; if (metro::okPtr(a)) addActor(bl, a); }
						// winding: the static triangle nearest below the player should face up
						bool flip = false;
						if (const auto pos = metro::playerPosition())
						{
							float best = 1e9f; const Tri* under = nullptr;
							for (const auto& t : *tris)
							{
								const float cy = (t.a.y + t.b.y + t.c.y) / 3.f, dy = pos->y - cy;
								if (dy < -0.3f || dy > 3.f) continue;
								auto side = [&](V3 p, V3 q) { return (q.x - p.x) * (pos->z - p.z) - (q.z - p.z) * (pos->x - p.x); };
								const float s1 = side(t.a, t.b), s2 = side(t.b, t.c), s3 = side(t.c, t.a);
								if (!((s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0))) continue;
								if (dy < best) { best = dy; under = &t; }
							}
							if (under) flip = cross(sub(under->b, under->a), sub(under->c, under->a)).y < 0.f;
						}
						for (auto& t : *tris) t.kind = classify(t, flip);
						next->scene = sc; next->nStatic = (uint32_t)statics.size(); next->staticTris = tris; next->staticVersion = ++versionCounter;
						PLOG_INFO << "Metro collision: " << statics.size() << " static actors, " << tris->size() << " triangles" << (flip ? " (winding flipped)" : "");
					}
					if (okD)
					{
						Builder bl{ next->dyn, kMaxDynTris, kDynamic };
						const uintptr_t me = playerActor();
						for (uintptr_t a : dyns) { if (bl.full()) break; if (metro::okPtr(a) && a != me) addActor(bl, a); }
					}
					vcall(sc, kSceneUnlockRead, dummy);
					if (!okS && !mWarned) { mWarned = true; PLOG_ERROR << "Metro collision: could not list the physics scene's actors"; }
					cur = next;
				}

				if (cur && cur->staticTris)
				{
					const bool flat = s->metroCollisionViewerHideFlatEdges->GetValue();
					// the edge set: once per static build / "Hide flat seams" change
					if (edgesFor != cur->staticTris || edgesFlat != flat)
					{
						const auto e0 = std::chrono::steady_clock::now();
						edges = buildEdges(*cur->staticTris, flat);
						edgesFor = cur->staticTris; edgesFlat = flat;
						lastStaticVersion = ~0ull;   // refilter
						PLOG_INFO << std::format("Metro collision: {} triangles -> {} edges{} in {} ms", cur->staticTris->size(), edges.size(),
							flat ? " (flat seams hidden)" : "", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - e0).count());
					}
					V3 cam{};
					const float range = s->metroCollisionViewerRange->GetValue() * metro::kMetresPerHaloUnit;   // the setting is in wu
					const int budget = s->metroCollisionViewerBudget->GetValue();
					if (cameraPos(cam))
					{
						// re-filter only when something changed: the edge set, range, budget, or the camera moved 10% of the range
						// (1 m at least) - unless everything is already in range, then moving changes nothing but the fade
						const V3 d = sub(cam, lastCam);
						const float moved = std::sqrt(dot(d, d));
						const bool needed = lastStaticVersion != cur->staticVersion || range != lastRange || budget != lastBudget
							|| moved > std::max(1.f, range * (allInRange ? 0.25f : 0.1f));
						if (needed)
						{
							const size_t maxEdges = (size_t)budget * 3 / 2;   // ~1.5 edges per triangle in a closed mesh
							auto fresh = std::make_shared<std::vector<h2cv::LineVertex>>();   // a new vector: the renderer may hold the old one
							filterStatic(edges, cam, range, maxEdges, *fresh);
							statLines = fresh;
							allInRange = statLines->size() / 2 == edges.size();
							lastCam = cam; lastRange = range; lastBudget = budget; lastStaticVersion = cur->staticVersion;
							++lineKey;
						}
					}
					// dynamic objects: every refresh, no dedup (few), within range
					auto lines = std::make_shared<Lines>();
					lines->stat = statLines; lines->key = lineKey; lines->staticEdgesTotal = edges.size();
					const float r2 = range * range;
					for (const auto& t : cur->dyn)
					{
						const V3 c = mul(add(add(t.a, t.b), t.c), 1.f / 3.f), dd = sub(c, cam);
						if (dot(dd, dd) > r2) continue;
						const uint32_t rgba = kColours[kDynamic] | (230u << 24);
						const V3 v[3] = { t.a, t.b, t.c };
						for (int e = 0; e < 3; ++e) { lines->dyn.push_back({ v[e].x, v[e].y, v[e].z, rgba }); lines->dyn.push_back({ v[(e + 1) % 3].x, v[(e + 1) % 3].y, v[(e + 1) % 3].z, rgba }); }
					}
					std::scoped_lock lk(mLinesMutex);
					mLines = lines;
				}
			}
			else if (cur)
			{
				cur.reset(); edgesFor.reset(); edges.clear(); statLines = std::make_shared<std::vector<h2cv::LineVertex>>(); lastStaticVersion = ~0ull;
				std::scoped_lock lk(mLinesMutex); mLines.reset();
			}
			// dynamic objects / camera refresh 4x a second
			while (mRun.load() && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(250)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	void start()
	{
		if (mRun.exchange(true)) return;
		mWorker = std::thread([this]() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); workerLoop(); });
	}
	void stop()
	{
		mRun = false;
		if (mWorker.joinable()) mWorker.join();
		std::scoped_lock lk(mLinesMutex);
		mLines.reset();
	}

	void onToggle(bool on)
	{
		if (on) { start(); say("Collision Viewer on: green = floor, grey-blue = wall, red = ceiling, orange = moving objects"); }
		else { stop(); say("Collision Viewer off"); }
	}

	// 2026-11-25 (user: "work on bringing down the render cost ... i dont want culling"): drawn by the GPU at Present through
	// HCM's D3D11 line renderer (h2cv::Renderer, the Halo collision viewer's): the static lines live in a kept vertex buffer and
	// are re-uploaded only when the worker rebuilds them; per frame only the game's view-projection is set. The CPU no longer
	// projects or clips anything (the renderer's geometry shader clips at the near plane and widens lines to pixels).
	void onRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, SimpleMath::Vector2 screen, ID3D11RenderTargetView* rtv)
	{
		if (!mReady.load(std::memory_order_acquire) || GlobalKill::isKillSet()) return;
		auto s = settingsWeak.lock();
		auto m = mccWeak.lock();
		if (!s || !m || !s->metroCollisionViewerToggle->GetValue()) return;
		const bool inLevel = m->isGameCurrentlyPlaying(mGame);
		mInLevel = inLevel;
		if (!inLevel || !dev || !ctx || !rtv) return;
		std::shared_ptr<const Lines> lines;
		{ std::scoped_lock lk(mLinesMutex); lines = mLines; }
		if (!lines) return;
		if (!mR.ready())
		{
			if (mRendererFailed) return;
			if (!mR.init(dev)) { mRendererFailed = true; PLOG_ERROR << "Metro collision: the D3D11 line renderer could not be created"; return; }
		}

		float vp[16];
		V3 cam{};
		if (!metro::read(metro::base() + kCamViewProjRVA, vp) || !cameraPos(cam)) return;
		for (float f : vp) if (!std::isfinite(f)) return;
		DirectX::XMFLOAT4X4 gvp;
		memcpy(&gvp, vp, sizeof(vp));   // row vectors, as the renderer wants (clip = p * VP, w = view depth)
		if (!mR.beginGame(ctx, rtv, (uint32_t)screen.x, (uint32_t)screen.y, h2::Vec3{ cam.x, cam.y, cam.z }, gvp, 0.05f)) return;
		if (lines->stat && !lines->stat->empty()) mR.drawLinesCached(lines->stat->data(), lines->stat->size(), lines->key, 1.f, 0.f, false, false, 1.f);
		if (!lines->dyn.empty()) mR.drawLines(lines->dyn.data(), lines->dyn.size(), 1.f, 0.f, false, false, 1.f);
		mR.end();
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<DirectXRenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->metroCollisionViewerToggle->valueChangedEvent, [this](bool& on) { onToggle(on); }),
		mRenderCallback(dicon.Resolve<DirectXRenderEvent>().lock(),
			[this](ID3D11Device* d, ID3D11DeviceContext* c, SimpleMath::Vector2 sz, ID3D11RenderTargetView* r) { onRender(d, c, sz, r); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroCollisionViewer is Metro Redux only");
		if (auto s = settingsWeak.lock(); s && s->metroCollisionViewerToggle->GetValue()) start();
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderCallback.removeCallback();
		stop();
		mR.shutdown();
	}
};

MetroCollisionViewer::MetroCollisionViewer(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroCollisionViewer::~MetroCollisionViewer() { PLOG_VERBOSE << "~" << getName(); }
