#pragma once
// Drawing / geometry helpers shared by Viewer.cpp (Halo 2) and Viewer3.cpp (Halo 3 / ODST): category colours, vector maths, point / segment /
// polygon distances, capsule and primitive wireframes, the 2D label queue and the JSON helpers of the desync logger.
// (Extracted unchanged from Viewer.cpp's anonymous namespaces.)
#include "H2CV_Collision.h"
#include "H2CV_Renderer.h"
#include "H2CV_Halo2.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace h2cv::vu
{
	using h2::Vec3;

	constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
	inline const uint32_t kCatColor[kCatCount] = {
		rgba(0, 230, 255),    // BSP: cyan
		rgba(255, 50, 50),    // BSP invisible: red
		rgba(255, 140, 0),    // breakable: orange
		rgba(70, 120, 255),   // instanced: blue
		rgba(180, 70, 255),   // instanced invisible: purple
		rgba(255, 0, 255),    // scenery: magenta
		rgba(0, 255, 90),     // crates: green
		rgba(255, 130, 190),  // machines: pink
		rgba(255, 90, 0),     // kill triggers: red-orange
		rgba(255, 235, 120),  // soft ceilings / soft kills / slip surfaces (Halo 3 / ODST): pale yellow
	};

	inline Vec3 sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline Vec3 add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline Vec3 mul(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3 cross(const Vec3& a, const Vec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	inline float len(const Vec3& a) { return std::sqrt(dot(a, a)); }
	inline Vec3 norm(const Vec3& a) { const float l = len(a); return l > 1e-9f ? mul(a, 1.f / l) : Vec3{ 0, 0, 1 }; }

	inline float pointSegDist2(const Vec3& p, const Vec3& a, const Vec3& b)
	{
		const Vec3 ab = sub(b, a);
		const float l2 = dot(ab, ab);
		const float t = l2 > 0 ? std::clamp(dot(sub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
		const Vec3 d = sub(p, add(a, mul(ab, t)));
		return dot(d, d);
	}

	// closest distance^2 between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9)
	inline float segSegDist2(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2)
	{
		const Vec3 d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
		const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
		float s, t;
		if (a <= 1e-12f && e <= 1e-12f) return dot(r, r);
		if (a <= 1e-12f) { s = 0; t = std::clamp(f / e, 0.f, 1.f); }
		else
		{
			const float c = dot(d1, r);
			if (e <= 1e-12f) { t = 0; s = std::clamp(-c / a, 0.f, 1.f); }
			else
			{
				const float b = dot(d1, d2), den = a * e - b * b;
				s = den != 0 ? std::clamp((b * f - c * e) / den, 0.f, 1.f) : 0.f;
				t = (b * s + f) / e;
				if (t < 0) { t = 0; s = std::clamp(-c / a, 0.f, 1.f); }
				else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.f, 1.f); }
			}
		}
		const Vec3 d = sub(add(p1, mul(d1, s)), add(p2, mul(d2, t)));
		return dot(d, d);
	}

	// distance from segment AB to a planar convex polygon (0 if it crosses)
	inline float segPolyDist(const Vec3& A, const Vec3& B, const std::vector<Vec3>& poly)
	{
		if (poly.size() < 3) return 1e9f;
		const Vec3 n = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
		auto inside = [&](const Vec3& p) {
			for (size_t i = 0; i < poly.size(); ++i)
			{
				const Vec3& a = poly[i]; const Vec3& b = poly[(i + 1) % poly.size()];
				if (dot(cross(sub(b, a), sub(p, a)), n) < -1e-6f) return false;
			}
			return true; };
		const float da = dot(sub(A, poly[0]), n), db = dot(sub(B, poly[0]), n);
		if ((da > 0) != (db > 0) && std::fabs(da - db) > 1e-9f)
		{
			const Vec3 x = add(A, mul(sub(B, A), da / (da - db)));
			if (inside(x)) return 0.f;
		}
		float best = 1e18f;
		for (const Vec3* p : { &A, &B })
		{
			const float h = dot(sub(*p, poly[0]), n);
			const Vec3 proj = sub(*p, mul(n, h));
			if (inside(proj)) best = std::min(best, h * h);
		}
		for (size_t i = 0; i < poly.size(); ++i)
			best = std::min(best, segSegDist2(A, B, poly[i], poly[(i + 1) % poly.size()]));
		return std::sqrt(best);
	}

	struct Label { float x, y; uint32_t col; char text[40]; };
	inline std::vector<Label> gLabels;

	inline void addLine(std::vector<LineVertex>& v, const Vec3& a, const Vec3& b, uint32_t c)
	{
		v.push_back({ a.x, a.y, a.z, c });
		v.push_back({ b.x, b.y, b.z, c });
	}

	// capsule wireframe (rings + meridians) and triangles
	inline void capsuleWire(std::vector<LineVertex>& v, const h2::Capsule& c, uint32_t col)
	{
		const Vec3 axis = norm(sub(c.b, c.a));
		const Vec3 t1 = norm(std::fabs(axis.z) < 0.9f ? cross(axis, Vec3{ 0, 0, 1 }) : cross(axis, Vec3{ 1, 0, 0 }));
		const Vec3 t2 = cross(axis, t1);
		// same tessellation as Cartographer's xlive pill (CollisionLines.cpp pill_build_wireframe): 24-segment rings at
		// each hemisphere's equator and at 45 degrees, 8 meridians running pole to pole with 6-segment quarter arcs
		constexpr int kRing = 24, kMeridians = 8, kArc = 6;
		auto surf = [&](const Vec3& centre, float sgn, float ang, float lat) {
			const Vec3 dir = add(mul(t1, std::cos(ang)), mul(t2, std::sin(ang)));
			return add(centre, add(mul(dir, std::cos(lat) * c.r), mul(axis, sgn * std::sin(lat) * c.r))); };
		for (int top = 0; top < 2; ++top)
		{
			const Vec3& centre = top ? c.b : c.a;
			const float sgn = top ? 1.f : -1.f;
			for (int ri = 0; ri < 2; ++ri)                          // equator, then 45 degrees
			{
				const float lat = 0.78539816f * ri;
				for (int si = 0; si < kRing; ++si)
				{
					const float a0 = 6.2831853f * si / kRing, a1 = 6.2831853f * (si + 1) / kRing;
					addLine(v, surf(centre, sgn, a0, lat), surf(centre, sgn, a1, lat), col);
				}
			}
		}
		for (int mi = 0; mi < kMeridians; ++mi)
		{
			const float ang = 6.2831853f * mi / kMeridians;
			addLine(v, surf(c.a, -1, ang, 0.f), surf(c.b, 1, ang, 0.f), col);   // the straight side
			for (int ai = 0; ai < kArc; ++ai)
			{
				const float l0 = 1.5707963f * ai / kArc, l1 = 1.5707963f * (ai + 1) / kArc;
				addLine(v, surf(c.b, 1, ang, l0), surf(c.b, 1, ang, l1), col);
				addLine(v, surf(c.a, -1, ang, l0), surf(c.a, -1, ang, l1), col);
			}
		}
	}

	inline void capsuleTris(std::vector<Tri>& out, const h2::Capsule& c)
	{
		const Vec3 axis = norm(sub(c.b, c.a));
		const Vec3 t1 = norm(std::fabs(axis.z) < 0.9f ? cross(axis, Vec3{ 0, 0, 1 }) : cross(axis, Vec3{ 1, 0, 0 }));
		const Vec3 t2 = cross(axis, t1);
		constexpr int N = 12, M = 4;
		auto pt = [&](int i, int j) -> Vec3 {
			// j in [-M, M]: latitude rings; j<0 bottom hemisphere around A, j>=0 top around B
			const float ang = 6.2831853f * i / N;
			const float lat = 1.5707963f * (float)std::abs(j) / M;
			const Vec3 radial = add(mul(t1, std::cos(ang)), mul(t2, std::sin(ang)));
			const Vec3 centre = j < 0 ? c.a : c.b;
			const float s = j < 0 ? -1.f : 1.f;
			return add(centre, add(mul(radial, std::cos(lat) * c.r), mul(axis, s * std::sin(lat) * c.r)));
		};
		for (int j = -M; j < M; ++j)
			for (int i = 0; i < N; ++i)
			{
				int ja = j, jb = j + 1;
				Vec3 p00 = pt(i, ja), p10 = pt(i + 1, ja), p01 = pt(i, jb), p11 = pt(i + 1, jb);
				if (j == -1) { p01 = pt(i, 0); p11 = pt(i + 1, 0); } // cylinder band between the hemispheres
				out.push_back({ p00, p10, p11 });
				out.push_back({ p00, p11, p01 });
			}
	}


	// ---- ghost subject geometry (biped capsule or vehicle shape primitives, body-local + pose) ----------------
	inline Vec3 xfp(const h2::Xf& x, const Vec3& p) { return h2::xfPoint(x, p); }

	// wireframe of one primitive at `pose`, translated by `shift` (believed / previous / halo / rewind copies)
	inline void primWire(std::vector<LineVertex>& v, const h2::ShapePrim& p, const h2::Xf& pose, const Vec3& shift, uint32_t col)
	{
		if (p.capsule)
		{
			h2::Capsule c; c.a = add(xfp(pose, p.a), shift); c.b = add(xfp(pose, p.b), shift); c.r = p.r; c.ok = true;
			capsuleWire(v, c, col);
			return;
		}
		std::vector<Vec3> w(p.wire.size());
		for (size_t i = 0; i < w.size(); ++i) w[i] = add(xfp(pose, p.wire[i]), shift);
		for (const auto& e : p.edges)
			if (e.first < w.size() && e.second < w.size()) addLine(v, w[e.first], w[e.second], col);
	}

	// closest point of a convex polygon to p
	inline Vec3 closestPointPoly(const Vec3& p, const std::vector<Vec3>& poly)
	{
		const Vec3 n = norm(cross(sub(poly[1], poly[0]), sub(poly[2], poly[0])));
		const Vec3 q = sub(p, mul(n, dot(sub(p, poly[0]), n)));
		bool inside = true;
		for (size_t i = 0; i < poly.size(); ++i)
		{
			const Vec3 a = poly[i], b = poly[(i + 1) % poly.size()];
			if (dot(cross(sub(b, a), sub(q, a)), n) < 0) { inside = false; break; }
		}
		if (inside) return q;
		Vec3 best = poly[0]; float bd = 1e30f;
		for (size_t i = 0; i < poly.size(); ++i)
		{
			const Vec3 a = poly[i], ab = sub(poly[(i + 1) % poly.size()], a);
			const float l2 = dot(ab, ab);
			const float t = l2 > 1e-12f ? std::clamp(dot(sub(p, a), ab) / l2, 0.f, 1.f) : 0.f;
			const Vec3 c = add(a, mul(ab, t));
			const float d = dot(sub(p, c), sub(p, c));
			if (d < bd) { bd = d; best = c; }
		}
		return best;
	}

	inline Vec3 primCentre(const h2::ShapePrim& p, const h2::Xf& pose)
	{
		if (p.capsule) return mul(add(xfp(pose, p.a), xfp(pose, p.b)), 0.5f);
		Vec3 c{ 0, 0, 0 };
		for (const Vec3& v : p.verts) c = add(c, xfp(pose, v));
		return p.verts.empty() ? c : mul(c, 1.f / (float)p.verts.size());
	}

	// Distance from the primitive's SURFACE (core minus its convex radius) to a planar convex polygon; 0 - r when they
	// intersect. Capsule / sphere: exact. Polytope (box, convex vertices): exact for two convex sets that do not
	// interpenetrate = min over (polytope edges x polygon) and (polygon edges x polytope faces) -- every closest feature
	// pair of two convex polytopes is vertex/edge against face/edge, and segPolyDist handles segment-vs-planar-polygon
	// including the boundary edges. Penetration depth is not computed (clamped at -r).
	inline float primSurfaceDist(const h2::ShapePrim& p, const h2::Xf& pose, const std::vector<Vec3>& poly)
	{
		if (poly.size() < 3) return 1e9f;
		if (p.capsule) return segPolyDist(xfp(pose, p.a), xfp(pose, p.b), poly) - p.r;
		std::vector<Vec3> w(p.verts.size());
		for (size_t i = 0; i < w.size(); ++i) w[i] = xfp(pose, p.verts[i]);
		float best = 1e9f;
		for (const auto& e : p.edges)
			if (e.first < w.size() && e.second < w.size()) best = std::min(best, segPolyDist(w[e.first], w[e.second], poly));
		if (best > 0.f)
		{
			std::vector<Vec3> ring;
			for (const auto& f : p.faces)
			{
				ring.clear();
				for (uint16_t vi : f.ring) if (vi < w.size()) ring.push_back(w[vi]);
				if (ring.size() < 3) continue;
				for (size_t i = 0; i < poly.size() && best > 0.f; ++i)
					best = std::min(best, segPolyDist(poly[i], poly[(i + 1) % poly.size()], ring));
			}
		}
		return best - p.r;
	}


	inline uint32_t timColor(float over, bool skipping, int manifold)
	{
		if (manifold > 0) return rgba(255, 255, 255);
		if (!skipping) return rgba(150, 150, 150);
		if (over > 0.02f) return rgba(255, 30, 30);    // over-credited: Havok believes it is much farther than it is
		if (over > 0.005f) return rgba(255, 170, 0);
		return rgba(255, 255, 0);                       // skipping honestly
	}

	inline std::string jv(const h2::Vec3& v)
	{
		char b[96]; snprintf(b, sizeof(b), "[%.5f,%.5f,%.5f]", v.x, v.y, v.z); return b;
	}
	inline std::string jf(float f) { char b[32]; snprintf(b, sizeof(b), "%.6g", std::isfinite(f) ? f : 0.f); return b; }
}
