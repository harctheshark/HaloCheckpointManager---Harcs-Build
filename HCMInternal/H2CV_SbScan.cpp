#include "pch.h"
#include "H2CV_SbScan.h"
// The model (memory h2-monster-bounce-contact-solver-re; halo2.dll, verified on recordings and a live probe):
//  * the landing tick collides BEFORE integrating: the pill at its start-of-tick origin as a convex sweep shape = the
//    capsule (axis feet+0.175 .. feet+0.55, r 0.175) extended by sweep = pre-solve velocity * dt (only when |v| > 5.4465)
//  * contacts against the flat surface polygons: least overlap (SAT) when the cores overlap -> depth -(overlap + r + 0.01);
//    else closest points -> depth (distance - r - 0.01), kept when < 0.0333; one contact per surface
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

namespace h2cv::sbscan
{
	namespace
	{
		struct V3 { double x = 0, y = 0, z = 0; };
		inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		inline V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
		inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		inline double len(V3 a) { return std::sqrt(dot(a, a)); }
		inline bool norm(V3 a, V3& out) { const double l = len(a); if (l < 1e-12) return false; out = a * (1.0 / l); return true; }

		constexpr double G = 4.1712, SWEEP_MIN = 5.4465, R_TRI = 0.01, TOL = 0.0333, TAU = 0.6, DAMP = 1.0, COUPLE = 0.99;
		constexpr int PASSES = 8, MAX_ROWS = 6, PHASES = 8;
		constexpr double CAP_LO = 0.175, CAP_HI = 0.55, CAP_R = 0.175;
		double DT = 1.0 / 30.0;   // the loaded job's tick rate (one job at a time)
		// the sweep threshold is a distance per tick (the swept shape's +52: 5.44650 at 30 tick, 10.89300 at 60 - read live 2026-11-01)
		inline double sweep_min() { return SWEEP_MIN * ((1.0 / 30.0) / DT); }

		struct Surf { uint32_t key; V3 n; std::vector<V3> poly, edges; V3 lo, hi; bool soft = false; };   // soft: two-sided / invisible
		struct Row { V3 n; double d; bool pair; bool soft; };
		struct Pt { double x, y, zc, dist2; };
		struct Fall { double vz, dx, dy; };

		bool point_in_poly(V3 p, const std::vector<V3>& poly, V3 n)
		{
			for (size_t i = 0; i < poly.size(); ++i)
				if (dot(cross(poly[(i + 1) % poly.size()] - poly[i], p - poly[i]), n) < -1e-9) return false;
			return true;
		}
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

		bool contact(const V3* core, int nc, const Surf& s, V3& n_out, double& d_out)
		{
			V3 axes[64]; int na = 0;
			axes[na++] = s.n;
			V3 cd[2]; int ncd = 0;
			cd[ncd++] = core[1] - core[0];
			if (nc == 4) { cd[ncd++] = core[2] - core[0]; V3 pn; if (norm(cross(cd[0], cd[1]), pn)) axes[na++] = pn; }
			for (int i = 0; i < ncd; ++i)
				for (const V3& e : s.edges) { V3 ax; if (na < 64 && norm(cross(cd[i], e), ax)) axes[na++] = ax; }
			double best = 1e30; V3 bn{};
			for (int i = 0; i < na; ++i)
				for (int sg = -1; sg <= 1; sg += 2)
				{
					const V3 n = axes[i] * (double)sg;
					double mp = -1e30, mc = 1e30;
					for (const V3& y : s.poly) mp = (std::max)(mp, dot(n, y));
					for (int k = 0; k < nc; ++k) mc = (std::min)(mc, dot(n, core[k]));
					if (mp - mc < best) { best = mp - mc; bn = n; }
				}
			if (best > 0) { n_out = bn; d_out = -(best + CAP_R + R_TRI); return true; }
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
					const std::vector<V3> quad = { core[0], core[1], core[3], core[2] };
					for (const V3& v : s.poly)
					{
						const V3 q = v - pn * (dot(pn, v) - dot(pn, core[0]));
						if (point_in_poly(q, quad, pn) || point_in_poly(q, quad, pn * -1.0)) consider(q, v);
					}
				}
			}
			const double dist = std::sqrt(bd) - CAP_R - R_TRI;
			if (dist >= TOL) return false;
			if (!norm(ba - bb, n_out)) n_out = s.n;
			d_out = dist;
			return true;
		}

		V3 solve(const Row* rows, int n, V3 v0)
		{
			V3 v = v0, vsum{};
			// the landing rule (Halo's contact-added listener sub_180989B40, traced 2026-11-01): a new contact while the pill moves faster than
			// its sweep threshold (5.4465) sets the speed to 0.75 x the threshold (4.08488), direction kept; slower: left alone
			{ const double sm = sweep_min(); const double s2 = dot(v, v); if (s2 > sm * sm) v = v * (sm * 0.75 / std::sqrt(s2)); }
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
			void near_surfs_cached(NearCache& c, double x, double y, double r, std::vector<int>& out, double zlo, double zhi) const
			{
				constexpr double M = 0.5, ZM = 2.0;
				if (std::isnan(zlo) || std::isnan(zhi)) { near_surfs(x, y, r, out, zlo, zhi); return; }   // never cache a NaN band
				if (c.job != this || c.serial != serial || c.r != r || std::fabs(x - c.cx) > M || std::fabs(y - c.cy) > M || zlo < c.zlo || zhi > c.zhi)
				{
					c.job = this; c.serial = serial; c.cx = x; c.cy = y; c.r = r; c.zlo = zlo - ZM; c.zhi = zhi + ZM;
					near_surfs(x, y, r + M, c.ids, c.zlo, c.zhi);
					c.box.resize(c.ids.size());
					for (size_t k = 0; k < c.ids.size(); ++k) { const Surf& s = S[c.ids[k]]; c.box[k] = { s.lo.x, s.hi.x, s.lo.y, s.hi.y, s.lo.z, s.hi.z }; }
				}
				out.clear();
				for (size_t k = 0; k < c.ids.size(); ++k)
				{
					const NearBox& b = c.box[k];   // the same test as near_surfs, on the same doubles
					if (b.lx - r <= x && x <= b.hx + r && b.ly - r <= y && y <= b.hy + r && b.hz >= zlo && b.lz <= zhi) out.push_back(c.ids[k]);
				}
			}
			int landing_contacts(V3 feet, V3 v_pre, const std::vector<int>& local, Row* rows, int maxr) const
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
				const double mg = CAP_R + 0.05;
				int n = 0;
				for (int i : local)
				{
					const Surf& s = S[i];
					if (s.hi.x < lo.x - mg || s.lo.x > hi.x + mg || s.hi.y < lo.y - mg || s.lo.y > hi.y + mg || s.hi.z < lo.z - mg || s.lo.z > hi.z + mg) continue;
					V3 nn; double d;
					if (!contact(core, nc, s, nn, d)) continue;
					if (n >= maxr) break;
					rows[n++] = { nn, d, false, s.soft };
				}
				// Havok's order on the landing tick: deepest first, and each point coupled with the one before it unless that one
				// already is (verified on 954 recorded landings: 952 deepest-first, 948 alternating) - no lottery
				std::stable_sort(rows, rows + n, [](const Row& a, const Row& b) { return a.d < b.d; });
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
				char name[96]; snprintf(name, sizeof(name), "%s", fileName(key, tick).c_str());
				FILE* f = nullptr;
				if (fopen_s(&f, name, "w") != 0 || !f) return;
				fprintf(f, "# predicted monster / dreadnought spots (HCM scanner): x,y,z,ceiling u/s,share 20+,share 35+,fall u/s,drift x,drift y\n");
				for (const Spot& s : found) fprintf(f, "%.3f,%.3f,%.3f,%.2f,%.4f,%.4f,%.0f,%.2f,%.2f\n", s.x, s.y, s.z, s.best, s.share20, s.share35, s.fall, s.driftX, s.driftY);
				fclose(f);
			}
			bool load()
			{
				char name[96]; snprintf(name, sizeof(name), "%s", fileName(key, tick).c_str());
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
					Surf o; o.key = s.key; o.n = { s.n[0], s.n[1], s.n[2] }; o.soft = (s.flags & 3) != 0;
					for (size_t i = 0; i + 2 < s.poly.size(); i += 3) o.poly.push_back({ s.poly[i], s.poly[i + 1], s.poly[i + 2] });
					o.lo = o.hi = o.poly[0];
					for (const V3& v : o.poly)
					{
						o.lo = { (std::min)(o.lo.x, v.x), (std::min)(o.lo.y, v.y), (std::min)(o.lo.z, v.z) };
						o.hi = { (std::max)(o.hi.x, v.x), (std::max)(o.hi.y, v.y), (std::max)(o.hi.z, v.z) };
					}
					for (size_t i = 0; i < o.poly.size(); ++i) o.edges.push_back(o.poly[(i + 1) % o.poly.size()] - o.poly[i]);
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
		std::string g_map_name;   // setMapFile (under g_m)
		std::string g_map_prefix = "gen3";   // setMapFile: "gen3" (Halo 3 / ODST) or "reach" -> superbounce_predicted_<prefix>_<map>[_t<tick>].csv
		std::string map_file(int tick) { return g_map_name.empty() ? std::string() : (tick == 30 ? "superbounce_predicted_" + g_map_prefix + "_" + g_map_name + ".csv" : "superbounce_predicted_" + g_map_prefix + "_" + g_map_name + "_t" + std::to_string(tick) + ".csv"); }

		// the job's detached owner thread joins its workers; stopping only asks them to finish their current place
		void stop_job(const std::shared_ptr<Job>& j) { if (j) j->stop = true; }
	}

	int tick() { return (int)std::lround(1.0 / DT); }

	bool landingLaunch(float fx, float fy, float fz, float vx, float vy, float vz, float launch[3])
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j || !j->ready) return false;
		// a pure function of its six inputs and the level (2026-11-03): the viewer repeats the exact same calls every tick while
		// you stand still, and the seams' straight-down falls are the same for every seam - remembered per job, bit-exact keys
		struct MemoKey { uint32_t b[6]; bool operator==(const MemoKey& o) const { return std::memcmp(b, o.b, sizeof(b)) == 0; } };
		struct MemoHash { size_t operator()(const MemoKey& k) const { uint64_t h = 1469598103934665603ull; for (uint32_t v : k.b) { h ^= v; h *= 1099511628211ull; } return (size_t)h; } };
		struct MemoVal { bool ok; float l[3]; };
		thread_local std::unordered_map<MemoKey, MemoVal, MemoHash> memo;
		thread_local uint64_t memoSerial = 0;
		if (memoSerial != j->serial || memo.size() > 65536) { memo.clear(); memoSerial = j->serial; }
		MemoKey mk;
		{ const float in[6] = { fx, fy, fz, vx, vy, vz }; std::memcpy(mk.b, in, sizeof(in)); }
		if (const auto it = memo.find(mk); it != memo.end())
		{
			if (it->second.ok) { launch[0] = it->second.l[0]; launch[1] = it->second.l[1]; launch[2] = it->second.l[2]; }
			return it->second.ok;
		}
		MemoVal& mv = memo[mk];
		mv.ok = false;
		// the heights both landing_contacts calls can reach: this tick's cores (feet + CAP_LO .. CAP_HI, swept by a tick of v) and
		// reachable's tick before (a tick higher, swept by v or v + a tick of gravity), widened past their own margin (CAP_R + 0.05)
		const double span = 2.0 * (std::fabs((double)vz) + G * DT) * DT;
		thread_local std::vector<int> local;
		thread_local NearCache cache;
		j->near_surfs_cached(cache, fx, fy, 1.0, local,(double)fz + CAP_LO - span - CAP_R - 0.25, (double)fz + CAP_HI + span + CAP_R + 0.25);
		const V3 feet{ fx, fy, fz }, v_pre{ vx, vy, vz };
		if (!j->reachable(feet, v_pre, local)) return false;
		Row rows[32];
		const int n = j->landing_contacts(feet, v_pre, local, rows, 32);
		if (n == 0) return false;
		bool hard = false; for (int k = 0; k < n; ++k) if (!rows[k].soft) { hard = true; break; }
		if (!hard) return false;
		const V3 v = solve(rows, n, v_pre);
		launch[0] = (float)v.x; launch[1] = (float)v.y; launch[2] = (float)v.z;
		mv.ok = true; mv.l[0] = launch[0]; mv.l[1] = launch[1]; mv.l[2] = launch[2];
		return true;
	}

	std::string fileName(uint64_t key, int tick)
	{
		char name[96];
		if (tick == 30) snprintf(name, sizeof(name), "superbounce_predicted_v3_%016llx.csv", (unsigned long long)key);
		else snprintf(name, sizeof(name), "superbounce_predicted_v3_t%d_%016llx.csv", tick, (unsigned long long)key);
		return name;
	}

	void start(uint64_t key, std::vector<Surface>&& surfaces, float nearX, float nearY, int tick)
	{
		std::shared_ptr<Job> old;
		{
			std::scoped_lock lk(g_m);
			if (g_job && g_job->key == key && g_job->tick == tick)
			{
				// the same map again (the toggle went off / on): retry the file if it was missing (2026-10-31: no auto scan)
				const std::string mf = map_file(tick);
				if (!g_job->loaded || g_job->mapFile != mf) { std::scoped_lock lk2(g_job->m); g_job->mapFile = mf; g_job->found.clear(); g_job->loaded = g_job->load(); }
				return;
			}
			old = g_job; g_job.reset();
		}
		stop_job(old);
		auto j = std::make_shared<Job>();
		j->key = key; j->tick = tick > 0 ? tick : 30;
		DT = 1.0 / j->tick;
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
		g_job = j;
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
		{ std::scoped_lock lk(g_m); j = g_job; g_job.reset(); }
		if (j) j->stop = true;      // its threads finish their current place and exit; the detached owner joins them
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
		std::scoped_lock lk(g_m);
		return g_job && g_job->ready ? g_job->serial : 0;
	}
}
