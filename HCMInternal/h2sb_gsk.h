// h2sb_gsk.h - Havok's convex-pair search as Halo 2 runs it for the SWEPT pill against one level polygon, up to the point where
// its penetration query starts expanding a hull.  One question is answered: does the game take the polygon's own plane instead of
// the least-overlap direction?  (2026-11-11)
//
// Why: for overlapping cores the penetration query (Xbox 0x309b50 = MCC halo2.dll+0x9CB600) first tests whether the starting
// simplex is flat - with an UNNORMALISED triangle normal N (|N| = twice the triangle's area):
//     |(a1 - a0) . N| <= 0.001          (the pill's segment against the polygon's feature triangle)
//     reach of the pair's difference beyond the triangle, times |N|, < 0.001
// On a small face both hold, and the query returns the triangle's plane ("separated") with the features unchanged: the contact
// is the face's normal - pointing from the pill's first feature (the swept leading end) to the other side - with the FULL depth
// of that end.  On a big face neither holds, the hull is expanded and the result is the least overlap (the scanner's SAT).
// Recordings (rec_20261101, 2364 contact points on swept ticks): every deep point that was not least overlap is this case.
//
// Sources: halo2-decompiled (Xbox v1.0) 0x3048a0 search, 0x303dd0 penetration seed, 0x309b50 query, 0x303360 / 0x3037a0 /
// 0x308590 / 0x303540 / 0x303110 feature helpers, 0x2fefd0 start; MCC 1.3528: 0x9BF590 (same search), 0x9BAAA0 (start),
// the shapes' own functions -
//   pill    hkConvexSweepShape (+B6E468) over a capsule (+B19E50): support = capsule vertex B (id 16) when (B-A).d >= 0 else A
//           (id 0), plus the sweep when sweep.d > 0;  vertex(id) = capsule vertex + sweep ALWAYS;  centre = vertex B;  no
//           feature limit.  A = the capsule's top, B = its bottom.
//   polygon Halo's surface shape (+B3C4B8, built by 0x791FB0 in the surface's ring order): support = the FIRST vertex with the
//           largest d.v;  vertex(id) = vertex[id];  centre = vertex 0;  no feature limit.
//   hkConvexSweepConvexAgent (0x989590) restarts the pair's search state on every call while the sweep is active, so the answer
//   depends on this tick's geometry alone.  (Without a sweep the state carries over from tick to tick: not modelled - callers
//   use this for the swept core only.)
//
// Self-contained (no includes), templated on the number type, usable from CUDA (H2D).
#pragma once
#include <math.h>
#ifndef H2D
#define H2D
#endif

namespace h2gsk {

template<class T> struct P { T x, y, z; int id; };
template<class T> H2D inline P<T> mk(T x, T y, T z, int id = 0) { P<T> p; p.x = x; p.y = y; p.z = z; p.id = id; return p; }
template<class T> H2D inline T dt(const P<T>& a, const P<T>& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
template<class T> H2D inline P<T> sb(const P<T>& a, const P<T>& b) { return mk<T>(a.x - b.x, a.y - b.y, a.z - b.z); }
template<class T> H2D inline P<T> ad(const P<T>& a, const P<T>& b) { return mk<T>(a.x + b.x, a.y + b.y, a.z + b.z); }
template<class T> H2D inline P<T> ml(const P<T>& a, T s) { return mk<T>(a.x * s, a.y * s, a.z * s); }
template<class T> H2D inline P<T> ng(const P<T>& a) { return mk<T>(-a.x, -a.y, -a.z); }
template<class T> H2D inline P<T> cr(const P<T>& a, const P<T>& b) { return mk<T>(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
template<class T> H2D inline T ab(T v) { return v < T(0) ? -v : v; }
template<class T> H2D inline bool same(const P<T>& a, const P<T>& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
template<class T> H2D inline void swp(P<T>& a, P<T>& b) { const P<T> t = a; a = b; b = t; }

// the two shapes
template<class T> struct Pair
{
	P<T> A, B, sw;        // capsule vertex A (top, id 0), vertex B (bottom, id 16), the sweep
	const T* pv; int nv;  // the polygon: nv vertices, 3 numbers each, in the surface's ring order
};
template<class T> H2D inline P<T> sup_a(const Pair<T>& q, const P<T>& d)
{
	P<T> v = dt(sb(q.B, q.A), d) >= T(0) ? q.B : q.A;
	if (dt(q.sw, d) > T(0)) { v.x += q.sw.x; v.y += q.sw.y; v.z += q.sw.z; }
	return v;
}
template<class T> H2D inline P<T> vert_a(const Pair<T>& q, int id)
{
	P<T> v = id == 16 ? q.B : q.A;
	v.x += q.sw.x; v.y += q.sw.y; v.z += q.sw.z;
	return v;
}
template<class T> H2D inline P<T> sup_b(const Pair<T>& q, const P<T>& d)
{
	T best = T(-3.4028234663852886e38); int k = 0;
	for (int i = 0; i < q.nv; ++i)
	{
		const T x = d.x * q.pv[3 * i] + d.y * q.pv[3 * i + 1] + d.z * q.pv[3 * i + 2];
		if (x > best) { best = x; k = i; }
	}
	return mk<T>(q.pv[3 * k], q.pv[3 * k + 1], q.pv[3 * k + 2], k);
}

// the search's working state (0x3048a0's stack object)
template<class T> struct St
{
	P<T> a[6], b[6];
	int ca, cb, prevb, restricted;
	P<T> normal;
	T ed[3];
};

// 0x308590: closest points of two segments; the flags say which ends were clamped (1 / 2 first at its end / start, 4 / 8 second)
template<class T> H2D inline int seg_flags(const P<T>& p1, const P<T>& d1, const P<T>& p2, const P<T>& d2, T& dist2)
{
	const P<T> between = sb(p2, p1);
	const T directions = dt(d1, d2), first_along = dt(d1, between), second_along = dt(d2, between);
	const T l1 = dt(d1, d1), l2 = dt(d2, d2);
	const T lengths = l2 * l1, dsq = directions * directions;
	const T det = ab(lengths - dsq);
	T num = l2 * first_along - second_along * directions;
	int flags; T f1;
	if (num * det >= det * det) { f1 = T(1); flags = 1; }
	else if (num <= T(0)) { f1 = T(0); flags = 2; }
	else if ((dsq + lengths) * T(9.5367431640625e-7) < det) { f1 = num / det; flags = 0; }
	else { f1 = T(1); flags = 1; }
	T f2 = directions * f1 - second_along;
	bool clamped = true;
	if (f2 >= l2) { f2 = T(1); flags = 4; }
	else if (f2 <= T(0)) { f2 = T(0); flags = 8; }
	else { f2 = f2 / l2; clamped = false; }
	if (clamped)
	{
		num = directions * f2 + first_along;
		if (num <= T(0)) { f1 = T(0); flags |= 2; }
		else if (num >= l1) { f1 = T(1); flags |= 1; }
		else f1 = num / l1;
	}
	const P<T> off = sb(ad(p1, ml(d1, f1)), ad(p2, ml(d2, f2)));
	dist2 = dt(off, off);
	return flags;
}

// 0x303360: which sides of a triangle's edges a point lies inside (7 = inside all three); with orient, keeps the triangle's
// normal and turns the triangle round so that the normal points from the first shape's feature to the second's
template<class T> H2D inline int tri_sides(St<T>& s, const P<T>& point, P<T>* tri, bool orient)
{
	const P<T> a = tri[0], b = tri[1], c = tri[2];
	const P<T> cb = sb(c, b), ac = sb(a, c), ba = sb(b, a);
	const P<T> n = cr(cb, ac);
	const T d0 = dt(cr(sb(point, b), cb), n), d1 = dt(cr(sb(point, c), ac), n), d2 = dt(cr(sb(point, a), ba), n);
	const int outside = (d0 < T(0) ? 1 : 0) | (d1 < T(0) ? 2 : 0) | (d2 < T(0) ? 4 : 0);
	s.ed[0] = d0; s.ed[1] = d1; s.ed[2] = d2;
	if (orient)
	{
		s.normal = n;
		if (outside == 7 && dt(n, sb(s.b[0], s.a[0])) < T(0))
		{
			s.normal = ng(n);
			swp(tri[0], tri[1]);
			const T t = s.ed[0]; s.ed[0] = s.ed[1]; s.ed[1] = t;
		}
	}
	return outside;
}

// 0x3037a0: a segment against a triangle. 1 = it crosses the triangle inside, 0 = one end of the segment dropped, 2 = one corner
template<class T> H2D inline int seg_tri(St<T>& s, bool seg_is_a, bool own_plane, int value)
{
	P<T>* seg = seg_is_a ? s.a : s.b;
	P<T>* tri = seg_is_a ? s.b : s.a;
	int& cseg = seg_is_a ? s.ca : s.cb;
	int& ctri = seg_is_a ? s.cb : s.ca;
	if (own_plane)
	{
		const P<T> second_edge = sb(tri[1], tri[2]), first_edge = sb(tri[0], tri[2]);
		const P<T> plane = cr(second_edge, first_edge);
		const T sh = dt(plane, sb(seg[0], tri[0])), eh = dt(plane, sb(seg[1], tri[0]));
		if ((eh < T(0)) != (sh < T(0)))
		{
			const T fr = sh / (sh - eh);
			const P<T> crossing = ad(ml(seg[0], T(1) - fr), ml(seg[1], fr));
			if (tri_sides(s, crossing, tri, false) == 7) return 1;
		}
		else
		{
			if (ab(sh) > ab(eh)) swp(seg[0], seg[1]);
			if (tri_sides(s, seg[0], tri, true) == 7) { --cseg; return 0; }
		}
		const P<T> direction = sb(seg[1], seg[0]);
		T ds, df;
		seg_flags(seg[0], direction, tri[2], second_edge, ds);
		seg_flags(seg[0], direction, tri[2], first_edge, df);
		tri[ds > df ? 1 : 0] = tri[2];
		--ctri;
		return 2;
	}
	const T o0 = s.ed[0], o1 = s.ed[1], o2 = s.ed[2];
	const P<T> corner = tri[0];
	const T eh = dt(s.normal, sb(seg[1], corner)), sh = dt(s.normal, sb(seg[0], corner));
	if (sh * eh < T(0))
	{
		const T fr = sh / (sh - eh);
		const P<T> crossing = ad(ml(seg[0], T(1) - fr), ml(seg[1], fr));
		if (tri_sides(s, crossing, tri, false) == 7) return 1;
	}
	const int inside = tri_sides(s, seg[1], tri, false);
	if (inside == 7) { --cseg; seg[0] = seg[1]; s.restricted = value; return 0; }
	int drop = 0;
	if (inside == 6) drop = 0;
	else if (inside == 5) drop = 1;
	else if (inside == 3) drop = 2;
	else if (inside == 1) drop = (s.ed[1] - o1) * o2 > (s.ed[2] - o2) * o1 ? 2 : 1;
	else if (inside == 2) drop = (s.ed[2] - o2) * o0 > (s.ed[0] - o0) * o2 ? 0 : 2;
	else if (inside == 4) drop = (s.ed[0] - o0) * o1 > (s.ed[1] - o1) * o0 ? 1 : 0;
	--ctri;
	if (drop == 0) tri[0] = tri[2];
	else if (drop == 1) tri[1] = tri[2];
	return 2;
}

// 0x303110 / 0x303540: a point against a tetrahedron: the corner to drop, or -1 when the point is inside
template<class T> H2D inline int tet_tie(const P<T>* v, const P<T>& point, T* scores)
{
	const int cyc[5] = { 2, 0, 1, 2, 0 };
	for (int face = 0; face < 3; ++face)
	{
		const int nxt = cyc[face + 2], prv = cyc[face];
		if (scores[nxt] >= T(0) && scores[prv] >= T(0))
		{
			const P<T> base = v[face];
			const P<T> en = sb(v[nxt], base), ep = sb(v[prv], base), qq = sb(point, base), apex = sb(v[3], base);
			if (dt(qq, en) * dt(apex, ep) < dt(qq, ep) * dt(apex, en)) scores[prv] = T(-1);
			else scores[nxt] = T(-1);
		}
	}
	if (scores[0] > T(0)) return 0;
	if (scores[1] > T(0)) return 1;
	if (scores[2] > T(0)) return 2;
	return -1;
}
template<class T> H2D inline int tet_drop(const P<T>* v, const P<T>& point, T orientation)
{
	const P<T> a = v[0], b = v[1], c = v[2], d = v[3];
	const P<T> ba = sb(b, a), ac = sb(a, c), da = sb(d, a), db = sb(d, b), dc = sb(d, c), cb = sb(c, b);
	const P<T> normals[3] = { cr(cb, db), cr(ac, dc), cr(ba, da) };
	const P<T> query = ml(sb(point, d), orientation);
	T dots[3], dist[3];
	for (int i = 0; i < 3; ++i)
	{
		dots[i] = dt(normals[i], query);
		const T L = dt(normals[i], normals[i]);
		dist[i] = L != T(0) ? dots[i] * ab(dots[i]) / L : T(3.4028234663852886e38);
	}
	int best = 1;
	if (dist[0] > dist[1]) { const T t = dist[0]; dist[0] = dist[1]; dist[1] = t; best = 0; }
	if (dist[1] > dist[2])
	{
		const T t = dist[1]; dist[1] = dist[2]; dist[2] = t;
		if (dist[0] > dist[1]) dist[1] = dist[0];
	}
	else best = 2;
	if (dist[2] < T(1.0000001111620804e-6)) return -1;
	if (dist[2] > T(1.1) * dist[1]) return best;
	return tet_tie(v, point, dots);
}

// the feature reduction (search_reduce): 1 = the features overlap, 0 = s.normal separates them
template<class T> H2D inline int reduce(St<T>& s)
{
	const int cyc[5] = { 2, 0, 1, 2, 0 };
	const int corner_of[8] = { -1, 0, 1, -6, 2, -7, -8, -1 };
	s.restricted = 0;
	for (int guard = 0; guard < 32; ++guard)
	{
		int key = (s.ca << 3) | s.cb;
		if (key == 9) { s.normal = sb(s.b[0], s.a[0]); s.prevb = s.cb; return 0; }
		if (key == 19 || key == 26)
		{
			const int r = s.ca == 2 ? seg_tri(s, true, s.prevb == 2, 1) : seg_tri(s, false, s.prevb == 2, -1);
			if (r == 1) return 1;
			if (r != 2) { s.prevb = s.cb; return 0; }
			key = 18;
		}
		if (key == 18)
		{
			const P<T> da = sb(s.a[1], s.a[0]), db = sb(s.b[1], s.b[0]);
			T d2;
			const int flags = seg_flags(s.a[0], da, s.b[0], db, d2);
			if (flags)
			{
				if (flags & 1) { s.a[0] = s.a[1]; s.ca = 1; }
				else if (flags & 2) s.ca = 1;
				if (flags & 4) { s.b[0] = s.b[1]; s.cb = 1; }
				else if (flags & 8) s.cb = 1;
				continue;
			}
			s.normal = cr(da, db);
			if (!(dt(s.normal, sb(s.a[0], s.b[0])) < T(0))) s.normal = ng(s.normal);
			s.prevb = s.cb; return 0;
		}
		if (key == 12 || key == 33)
		{
			const bool second = s.cb == 4;
			P<T>* corners = second ? s.b : s.a;
			const P<T> point = second ? s.a[0] : s.b[0];
			const int index = tet_drop(corners, point, second ? T(-1) : T(1));
			if (index < 0) return 1;
			if (second) --s.cb; else --s.ca;
			corners[index] = corners[3];
			key = 11;
		}
		if (key == 11 || key == 25)
		{
			const bool second = s.cb == 3;
			P<T>* corners = second ? s.b : s.a;
			const P<T> point = second ? s.a[0] : s.b[0];
			const int inside = tri_sides(s, point, corners, true);
			if (inside == 7) { s.prevb = s.cb; return 0; }
			int index = corner_of[inside];
			if (index >= 0)
			{
				const int prv = cyc[index]; int nxt = cyc[index + 2];
				const P<T> delta = sb(point, corners[index]), before = sb(corners[prv], corners[index]), after = sb(corners[nxt], corners[index]);
				int count = 2;
				if (dt(delta, before) < T(0))
				{
					corners[prv] = corners[2];
					if (dt(delta, after) < T(0))
					{
						if (nxt == count) nxt = prv;
						--count;
						corners[nxt] = corners[count];
					}
				}
				else corners[nxt] = corners[2];
				if (second) s.cb = count; else s.ca = count;
				continue;
			}
			index += 8;
			if (index > 3) index = 1;
			if (second) --s.cb; else --s.ca;
			corners[index] = corners[2];
			key = 10;
		}
		if (key == 10 || key == 17)
		{
			const bool second = s.cb == 2;
			P<T>* corners = second ? s.b : s.a;
			const P<T> point = second ? s.a[0] : s.b[0];
			const P<T> direction = sb(corners[1], corners[0]);
			const P<T> start = sb(corners[0], point), end = sb(corners[1], point);
			const bool end_sign = dt(end, direction) < T(0);
			if (end_sign != (dt(start, direction) < T(0)))
			{
				const P<T> across = cr(end, start);
				s.normal = second ? cr(across, direction) : cr(direction, across);
				s.prevb = s.cb; return 0;
			}
			if (end_sign) corners[0] = corners[1];
			if (second) --s.cb; else --s.ca;
			s.normal = sb(s.b[0], s.a[0]); s.prevb = s.cb; return 0;
		}
		return 1;
	}
	return 1;
}

template<class T> H2D inline bool new_point(const P<T>* c, int count, const P<T>& direction, T threshold)
{
	T p = dt(sb(c[count], c[0]), direction);
	if (!(ab(p) * p > threshold)) return false;
	if (count == 3)
	{
		p = dt(sb(c[count], c[2]), direction);
		if (ab(p) * p < threshold) return false;
	}
	if (count == 2 || count == 3)
	{
		p = dt(sb(c[count], c[1]), direction);
		if (ab(p) * p < threshold) return false;
	}
	return true;
}

// a support point of the pair (first shape along d, second against it) and their difference
template<class T> struct Sup { P<T> fa, fb, diff; };
template<class T> H2D inline Sup<T> pair_sup(const Pair<T>& q, const P<T>& d)
{
	Sup<T> r; r.fa = sup_a(q, d); r.fb = sup_b(q, ng(d)); r.diff = sb(r.fa, r.fb); return r;
}
// 0x3093e0: the better of the two opposite support points along a direction, and how far it lies from origin along it
template<class T> H2D inline Sup<T> scored_sup(const Pair<T>& q, const P<T>& direction, const P<T>& origin, T& dist)
{
	const Sup<T> al = pair_sup(q, direction), ag = pair_sup(q, ng(direction));
	const T adist = dt(sb(al.diff, origin), direction), gdist = dt(sb(ag.diff, origin), ng(direction));
	if (adist >= gdist) { dist = adist; return al; }
	dist = gdist; return ag;
}

// 0x309b50 up to the hull: true when the query ends on "the triangle's plane is the direction" (N returned unnormalised).
// Every other ending (the hull expansion, a collapsed simplex, a failure) returns false.
template<class T> H2D inline bool flat_plane(const Pair<T>& q, P<T>* sa, P<T>* sbv, P<T>* diffs, int count, P<T>& n_out)
{
	int grown = -1;
	int state = count;   // 1 point, 2 segment, 3 triangle, 4 tetrahedron; 5 grow point, 6 grow segment, 7 grow triangle
	if (count < 1 || count > 4) return false;
	P<T> normal = mk<T>(0, 0, 0);
	for (int guard = 0; guard < 24; ++guard)
	{
		if (state == 4)
		{
			normal = cr(sb(diffs[0], diffs[1]), sb(diffs[1], diffs[2]));
			const T height = dt(sb(diffs[3], diffs[0]), normal);
			if (!(ab(height) <= T(0.001))) return false;      // -> the hull expansion
			if (grown >= 3) return false;
			--count; state = 3;
		}
		if (state == 3)
		{
			normal = cr(sb(diffs[0], diffs[1]), sb(diffs[1], diffs[2]));
			if (dt(normal, normal) != T(0)) state = 7;
			else
			{
				if (grown >= 2) return false;                   // collapsed simplex
				--count; state = 2;
			}
		}
		if (state == 2)
		{
			const P<T> along = sb(diffs[0], diffs[1]);
			if (!(dt(along, along) <= T(0.001 * 0.001))) state = 6;
			else
			{
				if (grown >= 2) return false;
				--count; state = 1;
			}
		}
		if (state == 1)
		{
			if (!(dt(diffs[0], diffs[0]) <= T(0.001 * 0.001))) state = 5;
			else return false;                                  // the simplex is at the origin: the query restarts from the bodies' offset
		}
		if (state == 5)
		{
			grown = 1;
			const Sup<T> al = pair_sup(q, diffs[0]), ag = pair_sup(q, ng(diffs[0]));
			const P<T> ao = sb(al.diff, diffs[0]), go = sb(ag.diff, diffs[0]);
			const Sup<T>& w = dt(ao, ao) >= dt(go, go) ? al : ag;
			diffs[count] = w.diff; sa[count] = w.fa; sbv[count] = w.fb; ++count;
			state = 2; continue;
		}
		if (state == 6)
		{
			grown = 2;
			const P<T> edge = sb(diffs[0], diffs[1]);
			const T e[3] = { edge.x, edge.y, edge.z };
			int smallest = 0, second = 1, third = 2; T least = ab(e[0]);
			if (ab(e[1]) < least) { second = 0; least = ab(e[1]); smallest = 1; }
			if (ab(e[2]) < least) { third = smallest; smallest = 2; }
			T sq[3] = { T(0), T(0), T(0) };
			sq[smallest] = T(0); sq[second] = e[third]; sq[third] = -e[second];
			P<T> square = mk<T>(sq[0], sq[1], sq[2]);
			P<T> crs = cr(edge, square);
			// unit length (the game: reciprocal square root refined once; here the plain division - only the comparison below uses them)
			T l = dt(square, square); if (l > T(0)) square = ml(square, T(1) / (T)sqrt((double)l));
			l = dt(crs, crs); if (l > T(0)) crs = ml(crs, T(1) / (T)sqrt((double)l));
			T d1, d2;
			const Sup<T> f1 = scored_sup(q, square, diffs[0], d1), f2 = scored_sup(q, crs, diffs[0], d2);
			const Sup<T>& w = d1 > d2 ? f1 : f2;
			diffs[count] = w.diff; sa[count] = w.fa; sbv[count] = w.fb; ++count;
			state = 3; continue;
		}
		if (state == 7)
		{
			grown = 3;
			T dist;
			const Sup<T> w = scored_sup(q, normal, diffs[0], dist);
			if (dist < T(0.001)) { n_out = normal; return true; }
			diffs[count] = w.diff; sa[count] = w.fa; sbv[count] = w.fb; ++count;
			state = 4; continue;
		}
	}
	return false;
}

// THE QUESTION.  core = the swept pill core as the scanner keeps it (bottom, top, bottom + sweep, top + sweep); poly = nv vertices
// (3 numbers each) in the surface's ring order.  true: the game's contact is the polygon's plane - n (unit, the push on the pill)
// and w = the cores' signed distance along it (the contact depth is w - both radii; nearly always <= 0, but the query can also
// end on a plane of a simplex it grew itself, with w > 0 - the caller keeps the contact only while that depth is inside the
// contact tolerance).  false: use the least overlap.
template<class T> H2D inline bool face_rule(const T* bottom, const T* top, const T* sweep, const T* poly, int nv, T* n_out, T& w_out)
{
	if (nv < 3) return false;
	Pair<T> q;
	q.A = mk<T>(top[0], top[1], top[2], 0); q.B = mk<T>(bottom[0], bottom[1], bottom[2], 16); q.sw = mk<T>(sweep[0], sweep[1], sweep[2]);
	q.pv = poly; q.nv = nv;
	St<T> s;
	s.prevb = 0; s.restricted = 0; s.normal = mk<T>(0, 0, 0); s.ed[0] = s.ed[1] = s.ed[2] = T(0);
	// start (0x2fefd0): the pill's support vertex towards the polygon's first vertex, the polygon's back towards that
	const P<T> cb = mk<T>(poly[0], poly[1], poly[2]);
	const P<T> va = sup_a(q, sb(cb, q.B));
	const P<T> vb = sup_b(q, ng(sb(cb, va)));
	s.ca = 1; s.cb = 1;
	s.a[0] = vert_a(q, va.id); s.b[0] = vb;
	T tol = T(1.0e-10);
	for (int iteration = 1; iteration <= 48; ++iteration)
	{
		tol += tol;
		const int overlap = reduce(s);
		const T l2 = dt(s.normal, s.normal), sep = dt(sb(s.b[0], s.a[0]), s.normal);
		if (overlap || tol * T(1.1920928955078125e-7) * l2 >= sep * sep * sep)
		{
			// 0x303dd0: the penetration query's starting simplex from the features
			P<T> sa[6], sbv[6], diffs[6]; int count = 4;
			sa[0] = s.a[0]; sbv[0] = s.b[0]; diffs[0] = sb(s.a[0], s.b[0]);
			if (s.cb >= 3)
			{
				for (int i = 1; i < 3; ++i) { sa[i] = s.a[0]; sbv[i] = s.b[i]; diffs[i] = sb(s.a[0], s.b[i]); }
				if (s.cb > 3) { sa[3] = s.a[0]; sbv[3] = s.b[3]; diffs[3] = sb(s.a[0], s.b[3]); }
				else if (s.ca > 1) { sa[3] = s.a[1]; sbv[3] = s.b[0]; diffs[3] = sb(s.a[1], s.b[0]); }
				else count = 3;
			}
			else if (s.ca >= 3)
			{
				for (int i = 1; i < 3; ++i) { sa[i] = s.a[i]; sbv[i] = s.b[0]; diffs[i] = sb(s.a[i], s.b[0]); }
				if (s.ca > 3) { sa[3] = s.a[3]; sbv[3] = s.b[0]; diffs[3] = sb(s.a[3], s.b[0]); }
				else if (s.cb > 1) { sa[3] = s.a[0]; sbv[3] = s.b[1]; diffs[3] = sb(s.a[0], s.b[1]); }
				else count = 3;
			}
			else count = 1;
			P<T> N;
			if (!flat_plane(q, sa, sbv, diffs, count, N)) return false;
			const T l = dt(N, N);
			if (!(l > T(0))) return false;
			const P<T> n = ml(N, T(1) / (T)sqrt((double)l));
			// the features the query hands back: its first three points, duplicates dropped; the distance is taken between the first of each
			for (int i = 0; i < 3; ++i) { s.a[i] = sa[i]; s.b[i] = sbv[i]; }
			s.ca = s.cb = 3;
			if (same(s.a[1], s.a[2]) || same(s.a[0], s.a[2])) s.ca = 2;
			if (same(s.a[0], s.a[1])) { --s.ca; s.a[0] = s.a[s.ca]; }
			if (same(s.b[1], s.b[2]) || same(s.b[0], s.b[2])) --s.cb;
			if (s.cb >= 2 && same(s.b[0], s.b[1])) { --s.cb; s.b[0] = s.b[s.cb]; }
			if (s.ca + s.cb <= 4) reduce(s);
			const T w = dt(sb(s.a[0], s.b[0]), n);
			n_out[0] = n.x; n_out[1] = n.y; n_out[2] = n.z; w_out = w;
			return true;
		}
		const T threshold = l2 * tol;
		if ((iteration & 1) && s.restricted != 1 && s.ca < 5)
		{
			s.a[s.ca] = sup_a(q, s.normal);
			if (new_point(s.a, s.ca, s.normal, threshold)) { ++s.ca; continue; }
		}
		if (s.restricted != -1 && s.cb < 5)
		{
			const P<T> direction = ng(s.normal);
			s.b[s.cb] = sup_b(q, direction);
			if (new_point(s.b, s.cb, direction, threshold)) { ++s.cb; continue; }
		}
		if (!(iteration & 1) && s.restricted != 1 && s.ca < 5)
		{
			s.a[s.ca] = sup_a(q, s.normal);
			if (new_point(s.a, s.ca, s.normal, threshold)) { ++s.ca; continue; }
		}
		return false;   // the search ended with the features apart
	}
	return false;
}

} // namespace h2gsk
