#include "pch.h"
#include "H2CV_SbScan.h"
// The model (memory h2-monster-bounce-contact-solver-re; halo2.dll, verified on recordings and a live probe):
//  * the landing tick collides BEFORE integrating: the pill at its start-of-tick origin as a convex sweep shape = the
//    capsule (axis feet+0.175 .. feet+0.55, r 0.175) extended by sweep = pre-solve velocity * dt (only when |v| > 5.4465)
//  * contacts against the flat surface polygons: least overlap (SAT) when the cores overlap -> depth -(overlap + r + 0.01);
//    else closest points -> depth (distance - r - 0.01), kept when < 0.0328; one contact per surface
//  * which contacts are solved as coupled pairs depends on their history (a lottery): every pairing is evaluated
//  * solver: rhs = min(0.05|d|, dt) * 8/dt; 8 passes, tau 0.6, damp 1, coupled 2x2 with 0.99 n0.n1, pushes only,
//    rhs -= n.v after each pass; the body leaves with the AVERAGE of the 8 pass velocities
//  * a slab launch needs a fall above the sweep speed (it passes the slab in one tick) and some sideways drift (tilts the
//    sideways pushes up)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "h2sb_gsk.h"   // the game's own convex search up to the hull: when a small face answers with its own plane (2026-11-11)

namespace h2cv::sbscan
{
	namespace
	{
		struct V3 { double x = 0, y = 0, z = 0; };
		static_assert(sizeof(V3) == 3 * sizeof(double), "h2sb_gsk.h reads V3 arrays as packed triples");
		inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline double len(V3 a) { return std::sqrt(dot(a, a)); }
		inline bool norm(V3 a, V3& out) { const double l = len(a); if (l < 1e-12) return false; out = a * (1.0 / l); return true; }

		constexpr double G = 4.1712, SWEEP_MIN = 5.4465, R_TRI = 0.01, TOL = 0.0328, TAU = 0.6, DAMP = 1.0, COUPLE = 0.99;
		constexpr int PASSES = 8, MAX_ROWS = 6, PHASES = 8;
		constexpr double CAP_LO = 0.175, CAP_HI = 0.55, CAP_R = 0.175;
		double g_job_dt = 1.0 / 30.0;   // the loaded job's tick rate (one job at a time): the tick its PREDICTION FILE is for
		// 2026-11-16: the live viewer's landings (landingLaunch) run at the GAME's tick rate, whatever the job's is - the job's is 30
		// unless "scan at the current tick" is on, so at 60 tick the seams' physics used 30-tick rules. A caller's tick length, per
		// thread, for the length of its call; 0 = the job's (the scan / recipe threads).
		thread_local double t_call_dt = 0.0;
		inline double dt_now() { return t_call_dt > 0.0 ? t_call_dt : g_job_dt; }
		#define DT (dt_now())
		// the sweep threshold is a distance per tick (the swept shape's +52: 5.44650 at 30 tick, 10.89300 at 60 - read live 2026-11-01)
		// 2026-11-16: the threshold is NOT tied to the tick rate. It is 0.18155 x the collision "tickrate scalar" (halo2+0x70DBFA, the
		// pill's swept-shape builder): stock MCC (Season 8+) uses 30.0 at ANY tick rate -> 5.4465 u/s even at 60 tick; Season 7
		// physics uses the tick rate -> 10.893 at 60. A live caller passes the value it read from the pill's own shape (+52);
		// 0 = the old tick-matched value (the scan / recipe threads, Halo 3 / ODST / Reach's h3sb body).
		thread_local double t_call_sweep = 0.0;
		inline double sweep_min() { return t_call_sweep > 0.0 ? t_call_sweep : SWEEP_MIN * ((1.0 / 30.0) / DT); }

		struct Surf
		{
			uint32_t key; V3 n; std::vector<V3> poly, edges; V3 lo, hi; bool soft = false;   // soft: two-sided / invisible
			bool scen = false;   // a baked scenery object's triangle (Surface::flags 0x80): found late - see launch_contacts
			// contact()'s early "no contact" tests (2026-11-16, prepare_surfaces): the polygon's extent along its own normal
			// (min / max of dot(n, vertex), the very sums contact() made per call), the normal's length, and how far outside the
			// polygon's hull the closest-points part can place a point (its point_in_poly allows 1e-9; vertices off the plane)
			double nmin = 0, nmax = 0, nlen = 1, slack = 1e30;
		};
		// face: |polygon normal . contact normal| (1 = a face contact). sep / edge / pt: for the welder's cull - the cores were apart
		// (not a penetration), the polygon's closest point is on one of its edges or corners, and that point
		struct Row { V3 n; double d; bool pair; bool soft; double face = 1.0; bool sep = false, edge = false; V3 pt{}; };
		struct Pt { double x, y, zc, dist2; };
		struct Fall { double vz, dx, dy; };

		bool point_in_poly(V3 p, const V3* poly, size_t np, V3 n)
		{
			for (size_t i = 0; i < np; ++i)
				if (dot(cross(poly[(i + 1) % np] - poly[i], p - poly[i]), n) < -1e-9) return false;
			return true;
		}
		inline bool point_in_poly(V3 p, const std::vector<V3>& poly, V3 n) { return point_in_poly(p, poly.data(), poly.size(), n); }
		void seg_seg(V3 p1, V3 q1, V3 p2, V3 q2, V3& c1, V3& c2)
		{
			const V3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
			const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
			double s, t;
			if (a < 1e-12 && e < 1e-12) { c1 = p1; c2 = p2; return; }
			if (a < 1e-12) { s = 0; t = std::clamp(f / e, 0.0, 1.0); }
			else
			{
				const double cc = dot(d1, r);
				if (e < 1e-12) { t = 0; s = std::clamp(-cc / a, 0.0, 1.0); }
				else
				{
					const double b = dot(d1, d2), den = a * e - b * b;
					s = den > 1e-12 ? std::clamp((b * f - cc * e) / den, 0.0, 1.0) : 0.0;
					t = (b * s + f) / e;
					if (t < 0) { t = 0; s = std::clamp(-cc / a, 0.0, 1.0); }
					else if (t > 1) { t = 1; s = std::clamp((b - cc) / a, 0.0, 1.0); }
				}
			}
			c1 = p1 + d1 * s; c2 = p2 + d2 * t;
		}

		// 2026-11-16 (cost; the same answers bit for bit - H2 Monster Bounce Model\review_20261116\C_cost):
		//  * each axis is projected ONCE for both of its signs: dot(-n, y) is exactly -dot(n, y), so the - sign's "most of the
		//    polygon" is minus the + sign's least, and (-a) - (-b) rounds as b - a. Same values, compared in the same order
		//    (axis by axis, - before +, at most 64 axes)
		//  * the polygon's extent along its own normal comes precomputed (Surf::nmin / nmax)
		//  * NO CONTACT, EARLY: the closest-points part below returns false unless the shapes are closer than REACH (TOL + both
		//    radii). An axis that keeps them further apart than that proves it, and that part (two thirds of this function's
		//    time) is not run. The slack covers the points that part accepts just outside the two shapes: its point-in-polygon
		//    tests allow 1e-9 (cslack: the swept core's quad, from landing_contacts; Surf::slack: the polygon), + 1e-6
		bool contact(const V3* core, int nc, const Surf& s, double cslack, V3& n_out, double& d_out, bool* sep_out = nullptr, V3* pt_out = nullptr, bool* edge_out = nullptr)
		{
			if (sep_out) *sep_out = false;
			constexpr double REACH = TOL + CAP_R + R_TRI;
			double best = 1e30; V3 bn{};
			int na = 0;
			auto axis = [&](const V3& ax, double pmin, double pmax)
			{
				double cmin = 1e30, cmax = -1e30;
				for (int k = 0; k < nc; ++k) { const double c = dot(ax, core[k]); cmin = (std::min)(cmin, c); cmax = (std::max)(cmax, c); }
				const double on = cmax - pmin, op = pmax - cmin;   // the - sign's overlap, then the + sign's
				if (on < best) { best = on; bn = ax * -1.0; }
				if (op < best) { best = op; bn = ax; }
				++na;
			};
			auto poly_axis = [&](const V3& ax)
			{
				double pmin = 1e30, pmax = -1e30;
				for (const V3& y : s.poly) { const double p = dot(ax, y); pmin = (std::min)(pmin, p); pmax = (std::max)(pmax, p); }
				axis(ax, pmin, pmax);
			};
			axis(s.n, s.nmin, s.nmax);
			if (best < -(REACH + 1e-6 + cslack) * s.nlen) return false;   // the whole core is further than a contact from the polygon's plane
			V3 cd[2]; int ncd = 0;
			cd[ncd++] = core[1] - core[0];
			if (nc == 4) { cd[ncd++] = core[2] - core[0]; V3 pn; if (norm(cross(cd[0], cd[1]), pn)) poly_axis(pn); }
			for (int i = 0; i < ncd; ++i)
				for (const V3& e : s.edges) { V3 ax; if (na < 64 && norm(cross(cd[i], e), ax)) poly_axis(ax); }
			if (best < -(REACH + 1e-6 + cslack + s.slack) * (std::max)(1.0, s.nlen)) return false;   // an axis keeps them further apart than a contact
			if (best > 0)
			{
				// 2026-11-11: the game's penetration query tests its starting simplex for flatness with an UNNORMALISED normal, so on a small
				// face it never expands a hull - it answers with the face's own plane and the full depth of the pill's leading end (h2sb_gsk.h,
				// the same rule as the offline scanner's contact()). Swept core only.
				if (nc == 4 && s.poly.size() >= 3)
				{
					const V3 sw = core[2] - core[0];
					double fn[3], fw, fpb[3]; int fnb = 3;
					if (h2gsk::face_rule<double>(&core[0].x, &core[1].x, &sw.x, &s.poly[0].x, (int)s.poly.size(), fn, fw, &fnb, fpb) && fw - CAP_R - R_TRI < TOL)
					{
						// 2026-11-18: the search state's polygon-side feature size after the query (0x303dd0's end, matched source) is
						// what the welder's cull flag reads - an edge / a corner can be dropped like any other contact; and the point
						// is the engine's own (it was the polygon's support vertex along the normal, a stand-in)
						n_out = { fn[0], fn[1], fn[2] }; d_out = fw - CAP_R - R_TRI;
						if (pt_out) *pt_out = V3{ fpb[0], fpb[1], fpb[2] } + n_out * R_TRI;
						if (edge_out) *edge_out = fnb == 1 || fnb == 2;
						return true;
					}
				}
				n_out = bn; d_out = -(best + CAP_R + R_TRI); if (pt_out) { double mx = -1e300; for (const V3& y : s.poly) { const double t = dot(n_out, y); if (t > mx) { mx = t; *pt_out = y; } } } return true;
			}
			double bd = 1e30; V3 ba{}, bb{};
			auto consider = [&](V3 pa, V3 pb) { const V3 d = pa - pb; const double dd = dot(d, d); if (dd < bd) { bd = dd; ba = pa; bb = pb; } };
			const double pd = dot(s.n, s.poly[0]);
			for (int k = 0; k < nc; ++k) { const V3 q = core[k] - s.n * (dot(s.n, core[k]) - pd); if (point_in_poly(q, s.poly, s.n)) consider(core[k], q); }
			static const int ce[4][2] = { {0, 1}, {2, 3}, {0, 2}, {1, 3} };
			const int nce = nc == 4 ? 4 : 1;
			for (size_t i = 0; i < s.poly.size(); ++i)
				for (int j = 0; j < nce; ++j) { V3 a, b; seg_seg(core[ce[j][0]], core[ce[j][1]], s.poly[i], s.poly[(i + 1) % s.poly.size()], a, b); consider(a, b); }
			if (nc == 4)
			{
				V3 pn;
				if (norm(cross(core[1] - core[0], core[2] - core[0]), pn))
				{
					const V3 quad[4] = { core[0], core[1], core[3], core[2] };   // (an array: this ran a heap allocation per swept contact test)
					for (const V3& v : s.poly)
					{
						const V3 q = v - pn * (dot(pn, v) - dot(pn, core[0]));
						if (point_in_poly(q, quad, 4, pn) || point_in_poly(q, quad, 4, pn * -1.0)) consider(q, v);
					}
				}
			}
			const double dist = std::sqrt(bd) - CAP_R - R_TRI;
			if (dist >= TOL) return false;
			if (!norm(ba - bb, n_out)) n_out = s.n;
			d_out = dist;
			if (sep_out)
			{
				// for the welder's cull: the polygon's closest point, and whether it lies on an edge or a corner (within 1e-4)
				*sep_out = true; *pt_out = bb;
				bool on_edge = false;
				for (size_t i = 0; i < s.poly.size() && !on_edge; ++i)
				{
					const V3 a = s.poly[i], e = s.edges[i], w = bb - a;
					const double l2 = dot(e, e), t = l2 > 1e-24 ? std::clamp(dot(w, e) / l2, 0.0, 1.0) : 0.0;
					const V3 q = w - e * t;
					on_edge = dot(q, q) < 1e-8;
				}
				*edge_out = on_edge;
				// 2026-11-17: the engine's welder (Xbox 0x2fa2f0, keep test 0x2f97e0) asks the pair's SEARCH STATE which feature of
				// the polygon the closest point is on (a corner or an edge = may be dropped; a face = always stays). Where the
				// pill's closest point sits right on a border shared by two polygons - falling down a wall made of several
				// polygons - the geometry cannot tell: the search's own sign tests decide, and usually call ONE of the
				// polygons a face and the rest edges, sometimes none. So the search itself is run (h2gsk::features: the same
				// port face_rule uses) and its answer taken; its contact point too (0x3032c0, + the polygon's radius).
				const V3 fsw = nc == 4 ? core[2] - core[0] : V3{ 0, 0, 0 };
				int fa = 0, fb = 0; double fnn[3], fw = 0, fp[3];
				if (h2gsk::features<double>(&core[0].x, &core[1].x, &fsw.x, &s.poly[0].x, (int)s.poly.size(), fa, fb, fnn, fw, fp))
				{
					*edge_out = fb == 1 || fb == 2;
					*pt_out = V3{ fp[0], fp[1], fp[2] } + n_out * R_TRI;
				}
			}
			return true;
		}

		V3 solve(const Row* rows, int n, V3 v0)
		{
			V3 v = v0, vsum{};
			// the landing rule (Halo's contact-added listener sub_180989B40, traced 2026-11-01): a new contact while the pill moves faster than
			// its sweep threshold (5.4465) sets the speed to 0.75 x the threshold (4.08488), direction kept; slower: left alone
			// 2026-11-16 (review agents A + E, read in sub_18078D890 / sub_18078DB80 and measured on ~3,800 first-contact ticks): the
			// engine only cuts the speed for a contact the pill is approaching faster than the threshold, or an edge-type contact
			// (|face normal . contact normal| < 0.577). Applied here in its safest part only: when NOTHING opposes the motion and
			// every contact is a face contact - the pill passing a polygon from behind (falling through a floor's back: recorded,
			// velocity untouched) or sliding along a face - the speed is left alone. Everything else keeps the cut as before
			// (the full rule disagreed with the recordings on some launch ticks, so it is not used there).
			{
				const double sm = sweep_min(); const double s2 = dot(v, v);
				if (s2 > sm * sm)
				{
					bool cut = false;
					for (int i = 0; i < n && !cut; ++i) cut = -dot(rows[i].n, v) > 0.0 || rows[i].face < 0.577;
					if (cut) v = v * (sm * 0.75 / std::sqrt(s2));
				}
			}
			double rhs[32];
			for (int i = 0; i < n; ++i) rhs[i] = rows[i].d < 0 ? (std::min)(0.05 * std::fabs(rows[i].d), DT) * 8.0 / DT : 0.0;
			for (int pass = 0; pass < PASSES; ++pass)
			{
				for (int i = 0; i < n;)
				{
					if (i + 1 < n && rows[i + 1].pair)
					{
						const V3 a = rows[i].n, b = rows[i + 1].n;
						const double cp = COUPLE * dot(a, b);
						const double d0 = TAU * rhs[i] - DAMP * dot(a, v), d1 = TAU * rhs[i + 1] - DAMP * dot(b, v);
						const double det = 1.0 - cp * cp;
						const double l0 = (d0 - cp * d1) / det;
						if (l0 <= 0) { if (d1 > 0) v = v + b * d1; }
						else
						{
							const double l1 = (d1 - cp * d0) / det;
							if (l1 > 0) v = v + a * l0 + b * l1;
							else if (d0 > 0) v = v + a * d0;
						}
						i += 2;
					}
					else
					{
						const double d = TAU * rhs[i] - DAMP * dot(rows[i].n, v);
						if (d > 0) v = v + rows[i].n * d;
						++i;
					}
				}
				for (int i = 0; i < n; ++i) rhs[i] -= dot(rows[i].n, v);
				vsum = vsum + v;
			}
			return vsum * (1.0 / PASSES);
		}

		using Matching = std::vector<std::pair<int, int>>;
		void matchings(const int* idx, int n, std::vector<Matching>& out, Matching& cur)
		{
			if (n == 0) { out.push_back(cur); return; }
			const int a = idx[0];
			cur.push_back({ a, -1 }); matchings(idx + 1, n - 1, out, cur); cur.pop_back();
			for (int j = 1; j < n; ++j)
			{
				int rest[16]; int m = 0;
				for (int k = 1; k < n; ++k) if (k != j) rest[m++] = idx[k];
				cur.push_back({ a, idx[j] }); matchings(rest, m, out, cur); cur.pop_back();
			}
		}

		// ---- one scan job ----
		std::atomic<uint64_t> g_job_serial{ 0 };
		struct Job;
		// near_surfs' cells around the last query, gathered + sorted once and reused while queries stay inside (2026-11-03)
		// ids + their boxes side by side: reading lo / hi out of each Surf (vectors in between) missed the CPU cache per surface
		struct NearBox { double lx, hx, ly, hy, lz, hz; };
		struct NearCache { const Job* job = nullptr; uint64_t serial = 0; double cx = 0, cy = 0, r = -1, zlo = 1e300, zhi = -1e300; std::vector<int> ids; std::vector<NearBox> box; };
		struct Job
		{
			const uint64_t serial = ++g_job_serial;   // tells a NearCache this job from an earlier one at the same address
			uint64_t key = 0;
			uint64_t objSig = 0;   // which set of objects its surfaces hold (start(): another set = another job)
			int tick = 30;
			std::vector<Surf> S;
			std::unordered_map<long long, std::vector<int>> grid;
			std::vector<Pt> pts;
			std::vector<std::vector<Matching>> match;
			std::vector<Fall> falls;
			std::atomic<size_t> next{ 0 }, done{ 0 }, total{ 0 };   // total: set once the candidate places are ready
			std::atomic<bool> stop{ false };
			std::mutex m;
			std::vector<Spot> found;          // clustered
			std::vector<std::thread> threads;
			bool loaded = false;               // from the cache file
			std::string mapFile;               // Halo 3 / ODST: the per-map offline scan, read with the key file (setMapFile)
			std::atomic<bool> ready{ false };  // surfaces + grid built
			std::thread recipeThread;

			static long long cellkey(int x, int y) { return ((long long)(x + 1000000) << 32) | (unsigned)(y + 1000000); }
			// zlo / zhi (2026-11-02): only surfaces whose height range meets [zlo, zhi] - the in-game physics check steps whole falls
			// tick by tick through landingLaunch, and next to tall walls every call gathered (and sorted) the whole column, which
			// took the viewer's frame rate down in those spots. The order of what is kept is unchanged (ascending index).
			void near_surfs(double x, double y, double r, std::vector<int>& out, double zlo = -1e300, double zhi = 1e300) const
			{
				out.clear();
				for (int gx = (int)std::floor(x - r); gx <= (int)std::floor(x + r); ++gx)
					for (int gy = (int)std::floor(y - r); gy <= (int)std::floor(y + r); ++gy)
					{
						auto it = grid.find(cellkey(gx, gy));
						if (it == grid.end()) continue;
						for (int i : it->second)
							if (S[i].lo.x - r <= x && x <= S[i].hi.x + r && S[i].lo.y - r <= y && y <= S[i].hi.y + r && S[i].hi.z >= zlo && S[i].lo.z <= zhi) out.push_back(i);
					}
				std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
			}
			// the same list as near_surfs (exact: the same test, ascending), from a cached superset - the in-game physics check and
			// debounce finder call landingLaunch thousands of times a tick within a wu or two, and gathering + sorting each time was
			// ~60% of HCM's render thread (live profile, 2026-11-03). The cache: every surface within r + M of a centre (near_surfs'
			// own list there, ascending). A query within M of the centre (in x and y) only needs surfaces within r of it, and
			// those are all within r + M of the centre, so filtering the cache with the exact test gives near_surfs' list.
			// z-sliced (2026-11-05, Reach): the cache holds only the surfaces whose height range meets [zlo - 2, zhi + 2] of the query
			// that built it, and is reused while a query's [zlo, zhi] stays inside that band (and within M in x / y). Any surface a
			// query keeps meets its [zlo, zhi], so it meets the band and is in the cache: still near_surfs' exact list, ascending.
			// Before, the cache held whole columns (every height), and Reach's dense instance meshes put hundreds to thousands of
			// surfaces in them, rescanned on each of the ~700-1800 calls a tick (offline bench on m50: lists identical on 23,488 of
			// 23,488 recorded calls, the gather 8.1 -> 3.5 us a call; HCM Reach Bounce Perf\bench.cpp).
			// the cache part of near_surfs_cached on its own: after it, c holds every surface a query at x, y, [zlo, zhi] keeps
			// (the caller has ruled out a NaN band)
			void cache_fit(NearCache& c, double x, double y, double r, double zlo, double zhi) const
			{
				constexpr double M = 0.5, ZM = 2.0;
				if (c.job != this || c.serial != serial || c.r != r || std::fabs(x - c.cx) > M || std::fabs(y - c.cy) > M || zlo < c.zlo || zhi > c.zhi)
				{
					c.job = this; c.serial = serial; c.cx = x; c.cy = y; c.r = r; c.zlo = zlo - ZM; c.zhi = zhi + ZM;
					near_surfs(x, y, r + M, c.ids, c.zlo, c.zhi);
					c.box.resize(c.ids.size());
					for (size_t k = 0; k < c.ids.size(); ++k) { const Surf& s = S[c.ids[k]]; c.box[k] = { s.lo.x, s.hi.x, s.lo.y, s.hi.y, s.lo.z, s.hi.z }; }
				}
			}
			void near_surfs_cached(NearCache& c, double x, double y, double r, std::vector<int>& out, double zlo, double zhi) const
			{
				if (std::isnan(zlo) || std::isnan(zhi)) { near_surfs(x, y, r, out, zlo, zhi); return; }   // never cache a NaN band
				cache_fit(c, x, y, r, zlo, zhi);
				out.clear();
				for (size_t k = 0; k < c.ids.size(); ++k)
				{
					const NearBox& b = c.box[k];   // the same test as near_surfs, on the same doubles
					if (b.lx - r <= x && x <= b.hx + r && b.ly - r <= y && y <= b.hy + r && b.hz >= zlo && b.lz <= zhi) out.push_back(c.ids[k]);
				}
			}
			// contact()'s slack for the swept core (2026-11-16): its closest-points part takes a polygon vertex's projection onto
			// the core's quad when point_in_poly(quad) allows it, and that allows 1e-9 - at the quad's corners up to
			// 1e-9 (|a| + |b|) / |a x b| outside it (a, b: the quad's sides; much when the sweep is nearly along the pill's axis)
			static double core_slack(const V3* core, int nc)
			{
				if (nc != 4) return 0.0;
				const V3 a = core[1] - core[0], b = core[2] - core[0];
				const double l = len(cross(a, b));
				return l < 1e-12 ? 0.0 : 2e-9 * (len(a) + len(b)) / l;   // under 1e-12 contact() does not use the quad at all
			}
			// THE WELDER'S SEAM CULL (2026-11-16; hkConvexConvexWelderAgent sub_1809AC680 and its predicate sub_1809ABDB0, read in
			// the code and checked against the recordings - review_20261116\F_contact_timing\tim_model.py). After a tick's new
			// points are sorted deepest first, a point is thrown away when a DEEPER one that was kept stands as its reference and
			// all of these hold: its polygon's closest point is on an edge or a corner (face contacts and penetrations always
			// stay), it is not deeper than 0.1, its normal is more than ~5.7 deg from the reference's (dot < 0.995), and it does
			// not sit above the reference's plane (by more than 0.01). A penetration has no closest point, so it is never a
			// reference. In the recordings the game keeps 99.4% of face contacts but only 23% of edge and 20% of corner ones;
			// without this the rules made contacts on polygons the game had dropped - shallow side contacts that paired with a
			// deep one into launches that never happened (95 false launches in 1,518 recorded Headlong falls). With it the
			// tick's set of contacts equals the game's on 74% of recorded ticks at 30 tick (49% before), 90% at 60 (73%).
			// Only the live landings (landingLaunch) use it; the map scan's candidates are as before. rows: sorted, deepest first.
			// 2026-11-16 (evening), scored on 1,518 recorded Headlong falls (s8_physics/hcmstate/score.py, each fall stepped on its real
			// tick states): as first ported, with only SEPARATED contacts as references, it changed nothing (100 false launches, 1
			// missed) - the false launches' deep point is a PENETRATION. With a penetration as a reference too (its point: the
			// polygon's support vertex along the normal - an approximation, the engine's own point was not read) and a contact
			// pushed along its own face normal never culled: 38 false launches, 1 missed (1,479 of 1,518 right, was 1,417).
			// 2026-11-17, from the engine's own source (Xbox decomp: welder 0x2fa2f0, keep test 0x2f97e0, world setup 0x1c2d40 -
			// weld limits 0.01 / 0.995, contact tolerance 0.0328): the 38 were all one thing. "Edge or corner" is not where
			// the closest point falls - it is the polygon-side feature size the pair's SEARCH ended on (search state byte 9).
			// Falling beside a wall built of several polygons the closest point sits on their shared border; the search calls
			// one of them a face (kept) or all of them edges (all dropped), and the old "face normal = face contact" guess
			// kept one every time. Now contact() runs that search (h2gsk::features) and Row::edge is its answer; the face-
			// normal exemption is gone; TOL is the engine's 0.0328 (was 0.0333: two contacts at +0.033 the game never made).
			// Scoreboard: 1,517 of 1,518 right, 0 false launches, 1 missed (a lamp-post sliver - the penetration query's case).
			static int weld_cull(Row* rows, int n)
			{
#ifndef H2SB_NO_WELD
				bool gone[32] = {};
				for (int i = 0; i < n && i < 32; ++i)
				{
					if (gone[i]) continue;   // (a penetration is a reference too: its point = the polygon's support vertex along the normal)
					for (int j = i + 1; j < n && j < 32; ++j)
					{
						if (gone[j] || !rows[j].edge) continue;   // (edge: the engine search's own word - see contact(); 2026-11-18: also after a penetration query that ended on the polygon's plane)
						if (rows[j].d < -0.1) continue;
						if (dot(rows[i].n, rows[j].n) >= 0.995) continue;
						if (dot(rows[j].pt - rows[i].pt, rows[i].n) > 0.01) continue;
						gone[j] = true;
					}
				}
				int m = 0;
				for (int i = 0; i < n; ++i) if (i >= 32 || !gone[i]) rows[m++] = rows[i];
				return m;
#else
				return n;
#endif
			}
			int landing_contacts(V3 feet, V3 v_pre, const std::vector<int>& local, Row* rows, int maxr, bool weld = false) const
			{
				V3 core[4]; int nc = 2;
				core[0] = { feet.x, feet.y, feet.z + CAP_LO }; core[1] = { feet.x, feet.y, feet.z + CAP_HI };
				if (len(v_pre) > sweep_min()) { const V3 sw = v_pre * DT; core[2] = core[0] + sw; core[3] = core[1] + sw; nc = 4; }
				V3 lo = core[0], hi = core[0];
				for (int k = 0; k < nc; ++k)
				{
					lo = { (std::min)(lo.x, core[k].x), (std::min)(lo.y, core[k].y), (std::min)(lo.z, core[k].z) };
					hi = { (std::max)(hi.x, core[k].x), (std::max)(hi.y, core[k].y), (std::max)(hi.z, core[k].z) };
				}
				const double mg = CAP_R + 0.05, cslack = core_slack(core, nc);
				int n = 0;
				for (int i : local)
				{
					const Surf& s = S[i];
					if (s.hi.x < lo.x - mg || s.lo.x > hi.x + mg || s.hi.y < lo.y - mg || s.lo.y > hi.y + mg || s.hi.z < lo.z - mg || s.lo.z > hi.z + mg) continue;
					V3 nn, pt{}; double d; bool sep = false, edge = false;
					if (!contact(core, nc, s, cslack, nn, d, &sep, &pt, &edge)) continue;
					if (n >= maxr) break;
					rows[n++] = { nn, d, false, s.soft, std::fabs(dot(s.n, nn)), sep, edge, pt };
				}
				// Havok's order on the landing tick: deepest first, and each point coupled with the one before it unless that one
				// already is (verified on 954 recorded landings: 952 deepest-first, 948 alternating) - no lottery
				std::stable_sort(rows, rows + n, [](const Row& a, const Row& b) { return a.d < b.d; });
				if (weld) n = weld_cull(rows, n);
				bool prev_paired = true;
				for (int k = 0; k < n; ++k) { rows[k].pair = k > 0 && !prev_paired; prev_paired = rows[k].pair; }
				return n;
			}
			// reachable: on the tick before, the pill's swept path down to this start was clear (no penetrating contact) - else
			// it would have hit something earlier (e.g. a block's rim above the surface the landing is aimed at)
			bool reachable(V3 feet, V3 v_pre, const std::vector<int>& local) const
			{
				Row prev[32];
				// the tick before swept only if ITS speed (a tick of gravity less) was over SWEEP_MIN: a slow fall sinks in unswept,
				// and the faster landing tick's sweep must not make this check see the floor it had already reached (2026-11-01)
				const V3 v_prev{ v_pre.x, v_pre.y, v_pre.z + G * DT };
				const int n = landing_contacts(feet - v_pre * DT, len(v_prev) > sweep_min() ? v_pre : v_prev, local, prev, 32);
				for (int k = 0; k < n; ++k) if (prev[k].d < -0.001) return false;
				return true;
			}
			// ---- landingLaunch's own two passes (2026-11-16, cost; the same answers as reachable() + landing_contacts()) ----
			// Both read the near cache's boxes directly: a surface is tested when it passes near_surfs' test AND the pass's own box
			// test - exactly the surfaces landing_contacts tested out of near_surfs_cached's list, in the same (ascending) order,
			// without building that list or reading each fat Surf for its box.
			struct Cores { V3 c[4]; int nc = 2; double lx, hx, ly, hy, lz, hz, cslack; };
			static Cores cores_of(V3 feet, V3 v_pre)
			{
				Cores q;
				q.c[0] = { feet.x, feet.y, feet.z + CAP_LO }; q.c[1] = { feet.x, feet.y, feet.z + CAP_HI };
				if (len(v_pre) > sweep_min()) { const V3 sw = v_pre * DT; q.c[2] = q.c[0] + sw; q.c[3] = q.c[1] + sw; q.nc = 4; }
				V3 lo = q.c[0], hi = q.c[0];
				for (int k = 0; k < q.nc; ++k)
				{
					lo = { (std::min)(lo.x, q.c[k].x), (std::min)(lo.y, q.c[k].y), (std::min)(lo.z, q.c[k].z) };
					hi = { (std::max)(hi.x, q.c[k].x), (std::max)(hi.y, q.c[k].y), (std::max)(hi.z, q.c[k].z) };
				}
				const double mg = CAP_R + 0.05;
				q.lx = lo.x - mg; q.hx = hi.x + mg; q.ly = lo.y - mg; q.hy = hi.y + mg; q.lz = lo.z - mg; q.hz = hi.z + mg;
				q.cslack = core_slack(q.c, q.nc);
				return q;
			}
			// reachable(), stopping at the first contact that blocks: reachable() gathers up to 32 contacts in list order and then
			// asks whether any is deeper than 0.001 - the first such one among the first 32 answers that
			bool clear_before(const NearCache& c, double x, double y, double r, double zlo, double zhi, V3 feet, V3 v_pre) const
			{
				const V3 v_prev{ v_pre.x, v_pre.y, v_pre.z + G * DT };
				const Cores q = cores_of(feet - v_pre * DT, len(v_prev) > sweep_min() ? v_pre : v_prev);
				int n = 0;
				const size_t nk = c.ids.size();
				for (size_t k = 0; k < nk; ++k)
				{
					const NearBox& b = c.box[k];
					if (!(b.lx - r <= x && x <= b.hx + r && b.ly - r <= y && y <= b.hy + r && b.hz >= zlo && b.lz <= zhi)) continue;   // near_surfs' test
					if (b.hx < q.lx || b.lx > q.hx || b.hy < q.ly || b.ly > q.hy || b.hz < q.lz || b.lz > q.hz) continue;            // landing_contacts' box test
					V3 nn; double d;
					if (!contact(q.c, q.nc, S[c.ids[k]], q.cslack, nn, d)) continue;
					if (n >= 32) break;
					++n;
					if (d < -0.001) return false;
				}
				return true;
			}
			// landing_contacts(), with the tick before asked about only once this tick has a contact at all (blocked: it was not
			// clear -> landingLaunch's answer is false, as when reachable() was asked first; without a contact it is false anyway)
			int launch_contacts(const NearCache& c, double x, double y, double r, double zlo, double zhi, V3 feet, V3 v_pre, Row* rows, int maxr, bool& blocked, bool gate) const
			{
				blocked = false;
				const Cores q = cores_of(feet, v_pre);
				int n = 0;
				const size_t nk = c.ids.size();
				// SCENERY IS FOUND LATE (2026-11-16; the user's Season 8+ rock bounces, superbounce_log sessions with key 8000006e).
				// The pill's sweep vector is written by its polygon children as they run (hkConvexSweepConvexAgent, sub_180989590) -
				// with no polygon child there is no sweep, and the box the pill looks for polygons with is its UNSWEPT one. A baked
				// scenery object is ONE child of the level (a nested collection) whose own polygons only become children once that
				// unswept box (grown by 1.5 x the tolerance) reaches them. Recorded: falling onto a rock at 8.9-9.2 u/s with no level
				// polygon near, the sweep stays 0 for 5 ticks beside the object's helper, the pill ends one tick 0.08 above the
				// surface and starts the next 0.07 inside - only then a polygon child exists, the sweep switches on and the first
				// contact is deep (the launches). With level polygons within reach (sweep already on) the same object is met early
				// and shallow, as level floors are (0 of 39 recorded level landings at that speed launched).
				// So: on a swept tick, when no LEVEL surface is within reach of the unswept pill, a scenery triangle only counts
				// if the unswept pill's box reaches it. Read in the code + fitted to those recordings; not verified beyond them.
				const double um = CAP_R + 1.5 * 0.0328;
				const double ulx = feet.x - um, uhx = feet.x + um, uly = feet.y - um, uhy = feet.y + um, ulz = feet.z + CAP_LO - um, uhz = feet.z + CAP_HI + um;
				int armed = -1;   // -1 not asked yet; 1: a level surface is within reach of the unswept pill (the sweep is on)
				for (size_t k = 0; k < nk; ++k)
				{
					const NearBox& b = c.box[k];
					if (!(b.lx - r <= x && x <= b.hx + r && b.ly - r <= y && y <= b.hy + r && b.hz >= zlo && b.lz <= zhi)) continue;
					if (b.hx < q.lx || b.lx > q.hx || b.hy < q.ly || b.ly > q.hy || b.hz < q.lz || b.lz > q.hz) continue;
					const Surf& s = S[c.ids[k]];
					if (s.scen && q.nc == 4)
					{
						if (armed < 0)
						{
							armed = 0;
							const double g = 0.1;   // level polygons' bounds in the level's tree are loose: counted a little further out
							for (size_t k2 = 0; k2 < nk && !armed; ++k2)
							{
								const NearBox& b2 = c.box[k2];
								if (b2.hx < ulx - g || b2.lx > uhx + g || b2.hy < uly - g || b2.ly > uhy + g || b2.hz < ulz - g || b2.lz > uhz + g) continue;
								if (!S[c.ids[k2]].scen) armed = 1;
							}
						}
						if (!armed && (b.hx < ulx || b.lx > uhx || b.hy < uly || b.ly > uhy || b.hz < ulz || b.lz > uhz)) continue;   // not found yet
					}
					V3 nn, pt{}; double d; bool sep = false, edge = false;
					if (!contact(q.c, q.nc, s, q.cslack, nn, d, &sep, &pt, &edge)) continue;
					if (n == 0 && gate && !clear_before(c, x, y, r, zlo, zhi, feet, v_pre)) { blocked = true; return 0; }
					if (n >= maxr) break;
					rows[n++] = { nn, d, false, s.soft, std::fabs(dot(s.n, nn)), sep, edge, pt };
				}
				std::stable_sort(rows, rows + n, [](const Row& a, const Row& b) { return a.d < b.d; });
				n = weld_cull(rows, n);
				bool prev_paired = true;
				for (int k = 0; k < n; ++k) { rows[k].pair = k > 0 && !prev_paired; prev_paired = rows[k].pair; }
				return n;
			}
			void add_found(const Spot& s)
			{
				std::scoped_lock lk(m);
				for (Spot& f : found)
					if (std::fabs(f.x - s.x) < 0.4f && std::fabs(f.y - s.y) < 0.4f && std::fabs(f.z - s.z) < 1.f)
					{
						if (s.best > f.best) { const float sh20 = (std::max)(f.share20, s.share20), sh35 = (std::max)(f.share35, s.share35); f = s; f.share20 = sh20; f.share35 = sh35; }
						else { f.share20 = (std::max)(f.share20, s.share20); f.share35 = (std::max)(f.share35, s.share35); }
						return;
					}
				found.push_back(s);
			}
			void work()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				std::vector<int> local; Row rows[32], rr[32];
				size_t i;
				while (!stop && (i = next.fetch_add(1)) < pts.size())
				{
					const Pt& p = pts[i];
					near_surfs(p.x, p.y, 1.0, local);
					double zland = p.zc; bool got = false;
					for (int si : local)
					{
						const Surf& s = S[si];
						if (s.n.z < 0.3 || p.x < s.lo.x || p.x > s.hi.x || p.y < s.lo.y || p.y > s.hi.y) continue;
						const double z = s.poly[0].z - (s.n.x * (p.x - s.poly[0].x) + s.n.y * (p.y - s.poly[0].y)) / s.n.z;
						if (z > p.zc + 2 || z < p.zc - 3) continue;
						if (point_in_poly({ p.x, p.y, z }, s.poly, s.n) && (!got || z > zland)) { zland = z; got = true; }
					}
					double best = 0; long long cases = 0, n20 = 0, n35 = 0; Fall bf{};
					for (const Fall& f : falls)
						for (int ph = 0; ph < PHASES; ++ph)
						{
							const V3 v_pre{ f.dx, f.dy, -f.vz };
							const V3 feet{ p.x, p.y, zland + (ph + 0.5) / PHASES * f.vz * DT };
							if (!reachable(feet, v_pre, local)) continue;
							const int n = landing_contacts(feet, v_pre, local, rows, 32);
							if (n == 0) continue;
							const V3 v = solve(rows, n, v_pre);
							++cases;
							if (v.z > 4)
							{
								const double sp = len(v);
								if (sp >= lineMonster(tick)) ++n20;
								if (sp >= lineDreadnought(tick)) ++n35;
								if (sp > best) { best = sp; bf = f; }
							}
						}
					if (best >= lineMonster(tick) && cases > 0)
						add_found({ (float)p.x, (float)p.y, (float)zland, (float)best, (float)n20 / cases, (float)n35 / cases, (float)bf.vz, (float)bf.dx, (float)bf.dy });
					++done;
				}
			}
			void save() const
			{
				char name[200]; snprintf(name, sizeof(name), "%s", fileName(key, tick).c_str());
				FILE* f = nullptr;
				if (fopen_s(&f, name, "w") != 0 || !f) return;
				fprintf(f, "# predicted monster / dreadnought spots (HCM scanner): x,y,z,ceiling u/s,share 20+,share 35+,fall u/s,drift x,drift y\n");
				for (const Spot& s : found) fprintf(f, "%.3f,%.3f,%.3f,%.2f,%.4f,%.4f,%.0f,%.2f,%.2f\n", s.x, s.y, s.z, s.best, s.share20, s.share35, s.fall, s.driftX, s.driftY);
				fclose(f);
			}
			bool load()
			{
				char name[200]; snprintf(name, sizeof(name), "%s", fileName(key, tick).c_str());
				bool any = false;
				for (const std::string& path : { std::string(name), mapFile })
				{
					if (path.empty()) continue;
					FILE* f = nullptr;
					if (fopen_s(&f, path.c_str(), "r") != 0 || !f) continue;
					char line[256];
					while (fgets(line, sizeof(line), f))
					{
						if (line[0] == '#') continue;
						Spot s;
						if (sscanf_s(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &s.x, &s.y, &s.z, &s.best, &s.share20, &s.share35, &s.fall, &s.driftX, &s.driftY) >= 6) found.push_back(s);
					}
					fclose(f);
					any = true;
				}
				if (any) loadRecipes();
				return any;
			}

			// HCM addition (2026-11-03): recipes are SAVED - appended to <prediction file>.recipes.csv as each is worked out, and
			// applied when the predictions load, so a spot's approach and launch direction is computed once (~900k landings each)
			// instead of every time the map loads. One file per job (the per-map file's for Halo 3 / ODST, else the key file's).
			// The first line names the sweep: a file from a different sweep is set aside (.old) and the recipes are redone.
			static constexpr const char* kRecipeHeader = "# HCM superbounce recipes v1 (refine: 3x3 patch 0.05 apart, 16 start heights, falls 4-50, drift 0-2.25 x 24 headings)";
			std::string recipeFile() const
			{
				const std::string base = mapFile.empty() ? fileName(key, tick) : mapFile;
				const size_t dot = base.rfind(".csv");
				return (dot == std::string::npos ? base : base.substr(0, dot)) + ".recipes.csv";
			}
			// a spot by its position to the centimetre (the files print millimetres)
			static long long spotKey(float x, float y, float z)
			{
				return ((long long)std::lround(x * 100.f) * 73856093LL) ^ ((long long)std::lround(y * 100.f) * 19349663LL) ^ ((long long)std::lround(z * 100.f) * 83492791LL);
			}
			static void copyRecipe(Spot& d, const Spot& r)
			{
				d.rBest = r.rBest; d.rShare20 = r.rShare20; d.rShare35 = r.rShare35; d.rFall = r.rFall; d.rDriftX = r.rDriftX; d.rDriftY = r.rDriftY; d.rFeet = r.rFeet;
				d.rLaunchX = r.rLaunchX; d.rLaunchY = r.rLaunchY; d.rLaunchZ = r.rLaunchZ; d.rHeadLo = r.rHeadLo; d.rHeadHi = r.rHeadHi;
				d.rFallLo = r.rFallLo; d.rFallHi = r.rFallHi; d.rDriftLo = r.rDriftLo; d.rDriftHi = r.rDriftHi;
				d.rFeetLo = r.rFeetLo; d.rFeetHi = r.rFeetHi; d.rHeights = r.rHeights; d.rAtX = r.rAtX; d.rAtY = r.rAtY;
				d.recipe = true;
			}
			void loadRecipes()
			{
				const std::string path = recipeFile();
				FILE* f = nullptr;
				if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
				char line[512];
				if (!fgets(line, sizeof(line), f) || strncmp(line, kRecipeHeader, strlen(kRecipeHeader)) != 0)
				{
					fclose(f);
					MoveFileExA(path.c_str(), (path + ".old").c_str(), MOVEFILE_REPLACE_EXISTING);   // another sweep's: redone
					return;
				}
				std::unordered_map<long long, Spot> got;
				while (fgets(line, sizeof(line), f))
				{
					if (line[0] == '#') continue;
					Spot r;
					if (sscanf_s(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%f,%f", &r.x, &r.y, &r.z, &r.rBest, &r.rShare20, &r.rShare35,
						&r.rFall, &r.rDriftX, &r.rDriftY, &r.rFeet, &r.rLaunchX, &r.rLaunchY, &r.rLaunchZ, &r.rHeadLo, &r.rHeadHi, &r.rFallLo, &r.rFallHi,
						&r.rDriftLo, &r.rDriftHi, &r.rFeetLo, &r.rFeetHi, &r.rHeights, &r.rAtX, &r.rAtY) == 24)
						got[spotKey(r.x, r.y, r.z)] = r;   // the last line for a spot wins
				}
				fclose(f);
				for (Spot& s : found)
				{
					const auto it = got.find(spotKey(s.x, s.y, s.z));
					if (it != got.end()) copyRecipe(s, it->second);
				}
			}
			void saveRecipe(const Spot& s) const { saveRecipes(&s, 1); }
			// n recipes in one append (the GPU merge hands over hundreds at once)
			void saveRecipes(const Spot* sp, size_t n) const
			{
				if (n == 0) return;
				const std::string path = recipeFile();
				const bool fresh = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
				FILE* f = nullptr;
				if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
				if (fresh) fprintf(f, "%s\n# x,y,z,best,share20,share35,fall,drift x,drift y,feet,launch x,y,z,heading lo,hi,fall lo,hi,drift lo,hi,feet lo,hi,heights,at x,y\n", kRecipeHeader);
				for (size_t i = 0; i < n; ++i)
				{
					const Spot& s = sp[i];
					fprintf(f, "%.3f,%.3f,%.3f,%.2f,%.4f,%.4f,%.1f,%.3f,%.3f,%.4f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.4f,%.4f,%d,%.3f,%.3f\n",
						s.x, s.y, s.z, s.rBest, s.rShare20, s.rShare35, s.rFall, s.rDriftX, s.rDriftY, s.rFeet, s.rLaunchX, s.rLaunchY, s.rLaunchZ,
						s.rHeadLo, s.rHeadHi, s.rFallLo, s.rFallHi, s.rDriftLo, s.rDriftHi, s.rFeetLo, s.rFeetHi, s.rHeights, s.rAtX, s.rAtY);
				}
				fclose(f);
			}
			// the exact approach for one spot: a dense sweep at its landing point
			void recipe(Spot& s)
			{
				// the heights the sweep can touch (2026-11-03, speed): start heights up to a tick of the fastest fall (50) above the spot, the
				// tick before (reachable) one more tick up, the capsule (to 0.55) and landing_contacts' own margin (0.225) - 5 above / 1.5 below
				// covers it at 30 and 60 tick. Next to tall walls the full column made every one of the ~900k landings slow.
				std::vector<int> local;
				near_surfs(s.x, s.y, 1.0, local, (double)s.z - 1.5, (double)s.z + 5.0);
				// the REFINE of the spot (2026-11-01): a 3x3 patch 0.05 apart, 16 start heights, falls 4-5 (starting bounces) and
				// 6..50, LEGIT drift only (the biped's horizontal speed is at most 2.25) - the scan's sparse table misses precise
				// approaches (headlong lightpost: scan 25.0, refined 29.0, the user's record 28-29)
				const int NH = 24, NPH = 16, NM = 10;
				static const double mags[NM] = { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 };
				std::vector<double> vzs = { 4.0, 4.5, 5.0 }; for (double vz = 6; vz <= 50.01; vz += 2) vzs.push_back(vz);
				static const double PX[9] = { 0, -0.05, 0.05, 0, 0, -0.05, 0.05, -0.05, 0.05 }, PY[9] = { 0, 0, 0, -0.05, 0.05, -0.05, -0.05, 0.05, 0.05 };
				struct R { double best = 0; V3 bv{}; long long n20 = 0, n35 = 0, cases = 0; double feet = 0; int pt = 0; };
				std::vector<R> res(vzs.size() * NH * NM);
				// split over up to 8 threads (2026-11-03, speed): the ~6000 approaches are independent; each thread fills its own slice
				const unsigned nth = (std::max)(1u, (std::min)(8u, std::thread::hardware_concurrency() / 2));
				auto sweep = [&](size_t i0, size_t i1) {
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				Row rows[32];
				for (size_t i = i0; i < i1 && !stop; ++i)
				{
					const int vi = (int)(i / (NH * NM)), h = (int)((i / NM) % NH), mi = (int)(i % NM);
					const double vz = vzs[vi], ang = h * 6.283185307179586 / NH, m = mags[mi];
					const V3 v_pre{ m * std::cos(ang), m * std::sin(ang), -vz };
					R& o = res[i];
					const double side = len(v_pre) > sweep_min() ? 1.0 : -1.0;   // a slow (unswept) fall is found sunk in, under the surface
					for (int pt = 0; pt < 9; ++pt)
					for (int ph = 0; ph < NPH; ++ph)
					{
						const double fh = side * (ph + 0.5) / NPH * vz * DT;
						const V3 at{ s.x + PX[pt], s.y + PY[pt], s.z + fh };
						const int n = landing_contacts(at, v_pre, local, rows, 32);
						if (n == 0) continue;   // touches nothing: no case (tested before reachable, which needs a second contact test)
						if (!reachable(at, v_pre, local)) continue;
						++o.cases;
						bool hard = false; for (int k = 0; k < n; ++k) if (!rows[k].soft) { hard = true; break; }
						if (!hard) continue;   // two-sided / invisible only: falls through (the scan's rule, 2026-10-30)
						const V3 v = solve(rows, n, v_pre);
						if (v.z > 4)
						{
							const double sp = len(v);
							if (sp >= lineMonster(tick)) ++o.n20;
							if (sp >= lineDreadnought(tick)) ++o.n35;
							if (sp > o.best) { o.best = sp; o.bv = v; o.feet = fh; o.pt = pt; }
						}
					}
				}
				};
				{
				std::vector<std::thread> pool;
				const size_t chunk = (res.size() + nth - 1) / nth;
				for (unsigned t = 1; t < nth; ++t) { const size_t a = t * chunk, b = (std::min)(res.size(), a + chunk); if (a < b) pool.emplace_back(sweep, a, b); }
				sweep(0, (std::min)(res.size(), chunk));
				for (auto& th : pool) th.join();
				}
				if (stop) return;
				Row rows[32];
				size_t bi = 0;
				for (size_t i = 0; i < res.size(); ++i) if (res[i].best > res[bi].best || (res[i].best == res[bi].best && res[i].n20 > res[bi].n20)) bi = i;
				const R& B = res[bi];
				const int bvi = (int)(bi / (NH * NM)), bh = (int)((bi / NM) % NH), bmi = (int)(bi % NM);
				const double good = (std::max)((double)lineMonster(tick), 0.85 * B.best);
				auto at = [&](int vi, int h, int mi) -> const R& { return res[((size_t)vi * NH + ((h % NH + NH) % NH)) * NM + mi]; };
				auto headBest = [&](int h) { double b = 0; for (int vi = 0; vi < (int)vzs.size(); ++vi) for (int mi = 0; mi < NM; ++mi) b = (std::max)(b, at(vi, h, mi).best); return b; };
				int hl = 0, hh = 0;
				while (hl > -NH + 1 && headBest(bh + hl - 1) >= good) --hl;
				while (hh < NH - 1 && headBest(bh + hh + 1) >= good) ++hh;
				int fl = bvi, fh2 = bvi;
				while (fl > 0 && at(fl - 1, bh, bmi).best >= good) --fl;
				while (fh2 + 1 < (int)vzs.size() && at(fh2 + 1, bh, bmi).best >= good) ++fh2;
				int ml = bmi, mh = bmi;
				while (ml > 0 && at(bvi, bh, ml - 1).best >= good) --ml;
				while (mh + 1 < NM && at(bvi, bh, mh + 1).best >= good) ++mh;
				const double ang = bh * 6.283185307179586 / NH;
				// the start heights of the best approach that launch 20+ (deterministic: the tick-based sink depth decides it)
				double flo = 1e9, fhi = -1e9; int nh = 0;
				{
					const V3 v_pre{ mags[bmi] * std::cos(ang), mags[bmi] * std::sin(ang), -vzs[bvi] };
					for (int ph = 0; ph < NPH; ++ph)
					{
						const double fh = (len(v_pre) > sweep_min() ? 1.0 : -1.0) * (ph + 0.5) / NPH * vzs[bvi] * DT;
						const V3 at{ s.x + PX[B.pt], s.y + PY[B.pt], s.z + fh };
						if (!reachable(at, v_pre, local)) continue;
						const int n = landing_contacts(at, v_pre, local, rows, 32);
						if (n == 0) continue;
						bool hard = false; for (int k = 0; k < n; ++k) if (!rows[k].soft) { hard = true; break; }
						if (!hard) continue;
						const V3 v = solve(rows, n, v_pre);
						if (v.z > 4 && len(v) >= lineMonster(tick)) { flo = (std::min)(flo, fh); fhi = (std::max)(fhi, fh); ++nh; }
					}
				}
				std::scoped_lock lk(m);
				s.rFeetLo = nh ? (float)flo : 0.f; s.rFeetHi = nh ? (float)fhi : 0.f; s.rHeights = nh;
				s.rBest = (float)B.best; s.rShare20 = (float)B.n20 / (float)(std::max)(1LL, B.cases); s.rShare35 = (float)B.n35 / (float)(std::max)(1LL, B.cases);
				s.rFall = (float)vzs[bvi]; s.rDriftX = (float)(mags[bmi] * std::cos(ang)); s.rDriftY = (float)(mags[bmi] * std::sin(ang)); s.rFeet = (float)B.feet;
				s.rLaunchX = (float)B.bv.x; s.rLaunchY = (float)B.bv.y; s.rLaunchZ = (float)B.bv.z;
				s.rAtX = (float)(s.x + PX[B.pt]); s.rAtY = (float)(s.y + PY[B.pt]);
				s.rHeadLo = (float)((bh + hl) * 360.0 / NH); s.rHeadHi = (float)((bh + hh) * 360.0 / NH);
				s.rFallLo = (float)vzs[fl]; s.rFallHi = (float)vzs[fh2]; s.rDriftLo = (float)mags[ml]; s.rDriftHi = (float)mags[mh];
				s.recipe = true;
			}
			std::atomic<float> px{ 0 }, py{ 0 }, pz{ 0 };
			// recipes run ONLY on request (user, 2026-11-03: "never have it auto run that step") - the "Work out approaches" button:
			// every spot within reqR (x / y) and 30 wu (z) of where it was pressed, nearest first, then the request ends
			std::atomic<bool> reqOn{ false };
			std::atomic<float> reqX{ 0 }, reqY{ 0 }, reqZ{ 0 }, reqR{ 0 };
			std::atomic<int> reqTotal{ 0 }, reqDone{ 0 };
			bool inRequest(const Spot& s) const
			{
				const float dx = s.x - reqX, dy = s.y - reqY, r = reqR;
				return dx * dx + dy * dy < r * r && std::fabs(s.z - reqZ) < 30.f;
			}
			void recipes()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				while (!stop)
				{
					if (!ready || !reqOn) { Sleep(100); continue; }
					// the nearest spot within 30 wu without a recipe (one at a time: the list grows while the scan runs)
					int pick = -1; Spot copy;
					{
						std::scoped_lock lk(m);
						float bd = 1e30f;
						for (int i = 0; i < (int)found.size(); ++i)
						{
							const Spot& s = found[i];
							if (s.recipe || !inRequest(s)) continue;
							const float dx = s.x - reqX, dy = s.y - reqY, d2 = dx * dx + dy * dy;
							if (d2 < bd) { bd = d2; pick = i; }
						}
						if (pick >= 0) copy = found[pick];
					}
					if (pick < 0) { reqOn = false; continue; }   // the request is done
					recipe(copy);
					if (!copy.recipe) continue;   // stopped mid-sweep
					std::scoped_lock lk(m);
					if (pick < (int)found.size() && found[pick].x == copy.x && found[pick].y == copy.y) found[pick] = copy;
					saveRecipe(copy);   // kept for the next time this map loads
					++reqDone;
				}
			}
			void prepare_surfaces(std::vector<Surface>&& in)
			{
				for (Surface& s : in)
				{
					if (s.poly.size() < 9) continue;
					Surf o; o.key = s.key; o.n = { s.n[0], s.n[1], s.n[2] }; o.soft = (s.flags & 3) != 0; o.scen = (s.flags & 0x80) != 0;
					for (size_t i = 0; i + 2 < s.poly.size(); i += 3) o.poly.push_back({ s.poly[i], s.poly[i + 1], s.poly[i + 2] });
					o.lo = o.hi = o.poly[0];
					for (const V3& v : o.poly)
					{
						o.lo = { (std::min)(o.lo.x, v.x), (std::min)(o.lo.y, v.y), (std::min)(o.lo.z, v.z) };
						o.hi = { (std::max)(o.hi.x, v.x), (std::max)(o.hi.y, v.y), (std::max)(o.hi.z, v.z) };
					}
					for (size_t i = 0; i < o.poly.size(); ++i) o.edges.push_back(o.poly[(i + 1) % o.poly.size()] - o.poly[i]);
					// contact()'s early tests (2026-11-16): the extent along the normal - the same sums, in the same order, as contact()'s
					// own loop over the polygon made on every call
					o.nmin = 1e30; o.nmax = -1e30;
					for (const V3& y : o.poly) { const double p = dot(o.n, y); o.nmin = (std::min)(o.nmin, p); o.nmax = (std::max)(o.nmax, p); }
					o.nlen = len(o.n);
					{
						// how far outside the polygon point_in_poly (which allows 1e-9) lets a point of its plane be: at a corner with
						// inside angle t between sides a and b, (1e-9 / the shorter side / |n|) / sin(t / 2); doubled. Plus how far off
						// the plane (through vertex 0) the vertices are. A polygon with a very short side or a needle corner: no early test.
						double lat = 0; const size_t np = o.edges.size();
						for (size_t i = 0; i < np && o.nlen > 1e-6; ++i)
						{
							const V3 a = o.edges[i], b = o.edges[(i + 1) % np];
							const double la = len(a), lb = len(b), lm = (std::min)(la, lb);
							if (lm < 1e-4) { lat = 1e30; break; }
							const double sh = std::sqrt((std::max)(0.0, 0.5 * (1.0 + dot(a, b) / (la * lb))));   // sin(t / 2): cos t = -(a . b) / (|a| |b|)
							lat = (std::max)(lat, sh > 1e-6 ? 2e-9 / (lm * o.nlen * sh) : 1e30);
						}
						o.slack = o.nlen > 1e-6 ? lat + 2.0 * (o.nmax - o.nmin) / o.nlen : 1e30;
					}
					S.push_back(std::move(o));
				}
				std::sort(S.begin(), S.end(), [](const Surf& a, const Surf& b) { return a.key < b.key; });
				for (int i = 0; i < (int)S.size(); ++i)
					for (int gx = (int)std::floor(S[i].lo.x); gx <= (int)std::floor(S[i].hi.x); ++gx)
						for (int gy = (int)std::floor(S[i].lo.y); gy <= (int)std::floor(S[i].hi.y); ++gy)
							grid[cellkey(gx, gy)].push_back(i);
				match.resize(MAX_ROWS + 1);
				for (int n = 0; n <= MAX_ROWS; ++n) { int idx[16]; for (int i = 0; i < n; ++i) idx[i] = i; Matching cur; matchings(idx, n, match[n], cur); }
				// 30 / 35 / 40: Force Launch plunges (2026-10-30: a user-found dreadnought on a headlong post works only at ~30-40)
				for (double vz : { 10.0, 22.0, 30.0, 35.0, 40.0 })
				{
					falls.push_back({ vz, 0, 0 });
					for (double mm : { 1.0, 2.25 })
						for (int a = 0; a < 8; ++a) falls.push_back({ vz, mm * std::cos(a * 3.14159265358979 / 4), mm * std::sin(a * 3.14159265358979 / 4) });
				}
			}
			void prepare_points(double nx, double ny)
			{
				// candidate places: between two nearby surfaces whose normals are within ~10 deg of opposite
				std::unordered_map<long long, int> seen;
				std::vector<int> nb;
				for (int i = 0; i < (int)S.size(); ++i)
				{
					const Surf& a = S[i];
					for (int gx = (int)std::floor(a.lo.x - 1); gx <= (int)std::floor(a.hi.x + 1); ++gx)
						for (int gy = (int)std::floor(a.lo.y - 1); gy <= (int)std::floor(a.hi.y + 1); ++gy)
						{
							auto it = grid.find(cellkey(gx, gy));
							if (it == grid.end()) continue;
							for (int j : it->second) if (j > i) nb.push_back(j);
						}
					std::sort(nb.begin(), nb.end()); nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
					for (int j : nb)
					{
						const Surf& b = S[j];
						if (dot(a.n, b.n) > -0.985) continue;
						if (a.hi.x < b.lo.x - 1 || b.hi.x < a.lo.x - 1 || a.hi.y < b.lo.y - 1 || b.hi.y < a.lo.y - 1 || a.hi.z < b.lo.z - 1 || b.hi.z < a.lo.z - 1) continue;
						const double lx = (std::max)(a.lo.x, b.lo.x) - 0.2, hx = (std::min)(a.hi.x, b.hi.x) + 0.2;
						const double ly = (std::max)(a.lo.y, b.lo.y) - 0.2, hy = (std::min)(a.hi.y, b.hi.y) + 0.2;
						if (lx > hx || ly > hy) continue;
						const double zc = (std::min)(a.hi.z, b.hi.z);
						for (double x = lx; x <= hx + 1e-9; x += 0.05)
							for (double y = ly; y <= hy + 1e-9; y += 0.05)
							{
								const long long k = ((long long)std::llround(x / 0.05) << 40) ^ ((long long)std::llround(y / 0.05) << 20) ^ (long long)std::llround(zc / 0.5);
								if (seen.emplace(k, 1).second) pts.push_back({ x, y, zc, (x - nx) * (x - nx) + (y - ny) * (y - ny) });
							}
					}
					nb.clear();
				}
				std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) { return a.dist2 < b.dist2; });   // nearest first
			}
		};

		std::mutex g_m;
		std::shared_ptr<Job> g_job;
		// 2026-11-16: landingLaunch / readySerial took g_m on every call - thousands a tick from the viewer's worker threads
		// (BELOW_NORMAL). A worker descheduled while holding it stalled the render thread's own calls until it ran again: hitches.
		// Now each thread keeps the job it last saw and only takes the lock when the job changed (g_job_gen: bumped under g_m with
		// every change of g_job).
		std::atomic<uint64_t> g_job_gen{ 1 };
		thread_local std::shared_ptr<Job> tj;
		thread_local uint64_t tgen = 0;
		Job* job_fast()
		{
			if (tgen != g_job_gen.load(std::memory_order_acquire))
			{
				std::scoped_lock lk(g_m);
				tj = g_job; tgen = g_job_gen.load(std::memory_order_relaxed);
			}
			return tj.get();   // kept alive by this thread's own reference until its next call
		}
		thread_local bool t_memo = true;   // landingLaunch's per-thread memo (off for a thread whose calls never repeat)
		std::string g_map_name;   // setMapFile (under g_m)
		std::string g_map_prefix = "gen3";   // setMapFile: "gen3" (Halo 3 / ODST) or "reach" -> superbounce_predicted_<prefix>_<map>[_t<tick>].csv
		std::string map_file(int tick) { return g_map_name.empty() ? std::string() : dataPath(tick == 30 ? "superbounce_predicted_" + g_map_prefix + "_" + g_map_name + ".csv" : "superbounce_predicted_" + g_map_prefix + "_" + g_map_name + "_t" + std::to_string(tick) + ".csv"); }

		// the job's detached owner thread joins its workers; stopping only asks them to finish their current place
		void stop_job(const std::shared_ptr<Job>& j) { if (j) j->stop = true; }
	}

	std::string dataPath(const std::string& name)
	{
		static const char* const kDir = "HCM Bounce Viewer";
		static std::once_flag once;
		std::call_once(once, []
		{
			CreateDirectoryA(kDir, nullptr);
			// the files earlier builds left loose in the game folder: moved in once (one already there under that name wins)
			WIN32_FIND_DATAA fd{};
			std::vector<std::string> names;
			HANDLE h = FindFirstFileA("superbounce_*", &fd);
			if (h != INVALID_HANDLE_VALUE) { do { names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
			for (const std::string& n : names) MoveFileA(n.c_str(), (std::string(kDir) + "\\" + n).c_str());
		});
		return std::string(kDir) + "\\" + name;
	}

	int tick() { return (int)std::lround(1.0 / DT); }

	bool landingLaunch(float fx, float fy, float fz, float vx, float vy, float vz, float launch[3], float dt, bool gate, float sweepMin)
	{
		Job* const j = job_fast();
		if (!j || !j->ready) return false;
		// the caller's tick length and sweep threshold for everything below (restored on every way out)
		struct DtScope { double prev, prevS; DtScope(double d, double s) : prev(t_call_dt), prevS(t_call_sweep) { t_call_dt = d; t_call_sweep = s; } ~DtScope() { t_call_dt = prev; t_call_sweep = prevS; } }
			dtScope(dt > 0.f ? (double)dt : 0.0, sweepMin > 0.f ? (double)sweepMin : 0.0);
		// a pure function of its six inputs and the level (2026-11-03): the viewer repeats the exact same calls every tick while
		// you stand still, and the seams' straight-down falls are the same for every seam - remembered per job, bit-exact keys
		struct MemoKey { uint32_t b[9]; bool operator==(const MemoKey& o) const { return std::memcmp(b, o.b, sizeof(b)) == 0; } };   // + the tick length, the gate, the threshold
		struct MemoHash { size_t operator()(const MemoKey& k) const { uint64_t h = 1469598103934665603ull; for (uint32_t v : k.b) { h ^= v; h *= 1099511628211ull; } return (size_t)h; } };
		struct MemoVal { MemoKey k; uint32_t state; float l[3]; };   // state: 0 empty, 1 answered false, 2 answered true
		// 2026-11-16 (cost): a fixed table, one slot per hash (a newer call takes the slot over). The unordered_map allocated a node
		// per new call and freed all 16k of them at once every time it filled - in a fall no call repeats, so that was a
		// malloc per call and a burst of frees on the render thread every few ticks. A memo only ever returns what the same
		// seven inputs computed before, so what it forgets changes no answer.
		constexpr size_t MEMO_N = 16384;   // a power of two
		thread_local std::vector<MemoVal> memo;
		thread_local uint64_t memoSerial = 0;
		const bool use_memo = t_memo;
		MemoKey mk;
		{ const float in[9] = { fx, fy, fz, vx, vy, vz, (float)DT, gate ? 1.f : 0.f, (float)sweep_min() }; std::memcpy(mk.b, in, sizeof(in)); }
		MemoVal unkept{};
		MemoVal* mvp = &unkept;
		if (use_memo)
		{
			if (memo.size() != MEMO_N || memoSerial != j->serial) { memo.assign(MEMO_N, MemoVal{}); memoSerial = j->serial; }
			const uint64_t h = MemoHash()(mk);
			mvp = &memo[(size_t)(h ^ (h >> 29)) & (MEMO_N - 1)];
			if (mvp->state != 0 && mvp->k == mk)
			{
				if (mvp->state == 2) { launch[0] = mvp->l[0]; launch[1] = mvp->l[1]; launch[2] = mvp->l[2]; }
				return mvp->state == 2;
			}
			mvp->k = mk;
		}
		MemoVal& mv = *mvp;
		mv.state = 1;
		// the heights both contact passes can reach: this tick's cores (feet + CAP_LO .. CAP_HI, swept by a tick of v) and
		// the tick before's (a tick higher, swept by v or v + a tick of gravity), widened past their own margin (CAP_R + 0.05)
		const double span = 2.0 * (std::fabs((double)vz) + G * DT) * DT;
		const double zlo = (double)fz + CAP_LO - span - CAP_R - 0.25, zhi = (double)fz + CAP_HI + span + CAP_R + 0.25;
		thread_local NearCache cache;
		const V3 feet{ fx, fy, fz }, v_pre{ vx, vy, vz };
		Row rows[32];
		int n;
		if (std::isnan(zlo) || std::isnan(zhi))
		{
			// a NaN band is never cached: the plain way
			thread_local std::vector<int> local;
			j->near_surfs(fx, fy, 1.0, local, zlo, zhi);
			if (gate && !j->reachable(feet, v_pre, local)) return false;
			n = j->landing_contacts(feet, v_pre, local, rows, 32, true);
		}
		else
		{
			// 2026-11-16 (cost): this tick's contacts FIRST, and the tick before (reachable) only once there is one - most calls
			// of a fall touch nothing, and each paid for a second full contact pass whose answer could not matter (no contact is
			// false whatever the tick before says). Every false is the same false: the answers are unchanged.
			j->cache_fit(cache, fx, fy, 1.0, zlo, zhi);
			bool blocked = false;
			// gate (2026-11-16): "the tick before was clear" belongs to the map scan, whose landings start from nowhere. A caller
			// stepping a fall tick by tick has already asked about the tick before; there the gate only hid the real landing after
			// a brush (11 of 12 recorded launches the rules missed on Headlong were dropped by it).
			n = j->launch_contacts(cache, fx, fy, 1.0, zlo, zhi, feet, v_pre, rows, 32, blocked, gate);
			if (blocked) return false;
		}
		if (n == 0) return false;
		bool hard = false; for (int k = 0; k < n; ++k) if (!rows[k].soft) { hard = true; break; }
		if (!hard)
		{
			// only two-sided / invisible surfaces touched. The map scan counts that as "no launch here" (false). A fall being
			// STEPPED must not treat it as empty air (2026-11-16, the user's "it says I can monster but it is barely not right",
			// live probe on Headlong at (3.63, -16.90, 7.19): the game stopped a -16.3 u/s fall on the two-sided invisible slope
			// 200007a8 - speed cut, out at -0.5 u/s - while the stepped fall flew on through it into the floor and walls behind
			// and reported a monster). So for a stepped fall this tick IS a contact: the solved velocity, never as a launch
			// (0 of 65 recorded launches came off such surfaces alone).
			if (gate) return false;
			const V3 vs = solve(rows, n, v_pre);
			launch[0] = (float)vs.x; launch[1] = (float)vs.y; launch[2] = (float)(std::min)(vs.z, 0.0);
			mv.state = 2; mv.l[0] = launch[0]; mv.l[1] = launch[1]; mv.l[2] = launch[2];
			return true;
		}
		const V3 v = solve(rows, n, v_pre);
		launch[0] = (float)v.x; launch[1] = (float)v.y; launch[2] = (float)v.z;
		mv.state = 2; mv.l[0] = launch[0]; mv.l[1] = launch[1]; mv.l[2] = launch[2];
		return true;
	}

	std::string fileName(uint64_t key, int tick)
	{
		char name[96];
		if (tick == 30) snprintf(name, sizeof(name), "superbounce_predicted_v3_%016llx.csv", (unsigned long long)key);
		else snprintf(name, sizeof(name), "superbounce_predicted_v3_t%d_%016llx.csv", tick, (unsigned long long)key);
		return dataPath(name);
	}

	void start(uint64_t key, std::vector<Surface>&& surfaces, float nearX, float nearY, int tick, uint64_t objSig)
	{
		std::shared_ptr<Job> old;
		{
			std::scoped_lock lk(g_m);
			if (g_job && g_job->key == key && g_job->tick == tick && g_job->objSig == objSig)
			{
				// the same map again (the toggle went off / on): retry the file if it was missing (2026-10-31: no auto scan)
				const std::string mf = map_file(tick);
				if (!g_job->loaded || g_job->mapFile != mf) { std::scoped_lock lk2(g_job->m); g_job->mapFile = mf; g_job->found.clear(); g_job->loaded = g_job->load(); }
				return;
			}
			old = g_job; g_job.reset(); ++g_job_gen;
		}
		stop_job(old);
		auto j = std::make_shared<Job>();
		j->key = key; j->tick = tick > 0 ? tick : 30; j->objSig = objSig;
		g_job_dt = 1.0 / j->tick;
		{ std::scoped_lock lk(g_m); j->mapFile = map_file(j->tick); }
		j->loaded = j->load();
		j->recipeThread = std::thread([j]() { j->recipes(); });
		j->recipeThread.detach();
		// the preparation (surfaces, candidate places) also runs off the game thread
		std::thread([j, s = std::move(surfaces), nearX, nearY]() mutable {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			j->prepare_surfaces(std::move(s));
			j->ready = true;
			return;   // 2026-10-31 (user): NO automatic scan - the map is scanned only by the "scan around me" button (the external scanner)
			j->prepare_points(nearX, nearY);
			j->total = j->pts.size();
			if (j->stop) return;
			const unsigned hw = (std::max)(2u, std::thread::hardware_concurrency());
			const unsigned nthreads = (std::min)(6u, hw > 4 ? hw / 4 : 1);   // inside the game: at most 6 (24 made MCC unplayable, 2026-10-30)
			for (unsigned t = 0; t < nthreads; ++t) j->threads.emplace_back([j]() { j->work(); });
			for (auto& t : j->threads) t.join();
			if (!j->stop) j->save();
		}).detach();
		std::scoped_lock lk(g_m);
		g_job = j; ++g_job_gen;
	}

	void setMapFile(const std::string& mapName, const char* prefix)
	{
		std::string name;
		for (char c : mapName) if (std::isalnum((unsigned char)c) || c == '_' || c == '-') name += c;   // a file name part only
		std::string pre;
		for (const char* p = prefix ? prefix : "gen3"; *p; ++p) if (std::isalnum((unsigned char)*p)) pre += *p;
		if (pre.empty()) pre = "gen3";
		std::scoped_lock lk(g_m);
		if (name == g_map_name && pre == g_map_prefix) return;
		g_map_name = name; g_map_prefix = pre;
		// the map name can arrive after the job started: read the predictions again with (or without) the map's file
		if (g_job) { std::scoped_lock lk2(g_job->m); g_job->mapFile = map_file(g_job->tick); g_job->found.clear(); g_job->loaded = g_job->load(); }
	}

	void reload()
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j) return;
		std::scoped_lock lk(j->m);
		j->found.clear();
		j->loaded = j->load();
	}

	void cancel()
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; g_job.reset(); ++g_job_gen; }
		if (j) j->stop = true;     // its threads finish their current place and exit; the detached owner joins them
	}

	void spots(std::vector<Spot>& out)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		out.clear();
		if (!j) return;
		std::scoped_lock lk(j->m);
		out = j->found;
	}

	void spots(std::vector<Spot>& out, float x, float y, float r, size_t& total)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		out.clear(); total = 0;
		if (!j) return;
		std::scoped_lock lk(j->m);
		total = j->found.size();
		for (const Spot& s : j->found)
		{
			const float dx = s.x - x, dy = s.y - y;
			if (dx * dx + dy * dy < r * r) out.push_back(s);
		}
	}

	float progress()
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j) return 0.f;
		if (j->loaded) return 1.f;
		const size_t n = j->total.load();
		return n ? (float)j->done.load() / (float)n : 0.f;
	}

	void requestRecipes(float x, float y, float z, float r)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j) return;
		int n = 0;
		{
			std::scoped_lock lk(j->m);
			j->reqX = x; j->reqY = y; j->reqZ = z; j->reqR = r;
			for (const Spot& s : j->found) if (!s.recipe && j->inRequest(s)) ++n;
		}
		j->reqTotal = n; j->reqDone = 0;
		j->reqOn = n > 0;
	}

	void cancelRecipes()
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (j) j->reqOn = false;
	}

	std::vector<Spot> recipeTargets(float x, float y, float z, float r, bool all)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		std::vector<Spot> out;
		if (!j) return out;
		{
			std::scoped_lock lk(j->m);
			for (const Spot& s : j->found)
			{
				const float dx = s.x - x, dy = s.y - y;
				if ((all || !s.recipe) && dx * dx + dy * dy < r * r && std::fabs(s.z - z) < 30.f) out.push_back(s);
			}
		}
		std::sort(out.begin(), out.end(), [x, y](const Spot& a, const Spot& b) {
			return (a.x - x) * (a.x - x) + (a.y - y) * (a.y - y) < (b.x - x) * (b.x - x) + (b.y - y) * (b.y - y); });
		return out;
	}

	int mergeRecipes(const std::string& path, bool overwrite, int run)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j) return 0;
		// shared: the GPU tool may still be writing it (rows are merged while it runs); only whole lines count
		FILE* f = _fsopen(path.c_str(), "r", _SH_DENYNO);
		if (!f) return 0;
		std::unordered_map<long long, Spot> got;
		char line[512];
		while (fgets(line, sizeof(line), f))
		{
			if (line[0] == '#' || !strchr(line, '\n')) continue;
			Spot r;
			if (sscanf_s(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%d,%f,%f", &r.x, &r.y, &r.z, &r.rBest, &r.rShare20, &r.rShare35,
				&r.rFall, &r.rDriftX, &r.rDriftY, &r.rFeet, &r.rLaunchX, &r.rLaunchY, &r.rLaunchZ, &r.rHeadLo, &r.rHeadHi, &r.rFallLo, &r.rFallHi,
				&r.rDriftLo, &r.rDriftHi, &r.rFeetLo, &r.rFeetHi, &r.rHeights, &r.rAtX, &r.rAtY) == 24)
				got[Job::spotKey(r.x, r.y, r.z)] = r;
		}
		fclose(f);
		std::vector<Spot> merged;
		{
			std::scoped_lock lk(j->m);
			// a redo run's file is merged once a second as it grows: each of its rows replaces a spot's recipe ONCE
			static std::unordered_map<long long, bool> s_replaced;   // spot key -> replaced by the current redo file
			static int s_run = -1;
			if (!overwrite || s_run != run) { s_replaced.clear(); s_run = overwrite ? run : -1; }
			for (Spot& s : j->found)
			{
				const long long k = Job::spotKey(s.x, s.y, s.z);
				const auto it = got.find(k);
				if (it == got.end()) continue;
				if (s.recipe && !(overwrite && !s_replaced.count(k))) continue;
				Job::copyRecipe(s, it->second);
				merged.push_back(s);
				if (overwrite) s_replaced[k] = true;
			}
		}
		j->saveRecipes(merged.data(), merged.size());   // into the map's recipe cache, like the CPU path (one append)
		return (int)merged.size();
	}

	bool recipeStatus(int& done, int& total)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		done = total = 0;
		if (!j) return false;
		done = j->reqDone; total = j->reqTotal;
		return j->reqOn;
	}

	void setPlayer(float x, float y, float z)
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (j) { j->px = x; j->py = y; j->pz = z; }
	}

	uint64_t mapKey()
	{
		std::scoped_lock lk(g_m);
		return g_job ? g_job->key : 0;
	}

	uint64_t readySerial()
	{
		const Job* const j = job_fast();
		return j && j->ready ? j->serial : 0;
	}

	void setThreadMemo(bool on) { t_memo = on; }
	// a worker with nothing to do lets go of the job it last saw (else an idle thread keeps a whole level's surfaces alive)
	void dropThreadJob() { tj.reset(); tgen = 0; }
}
