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
			{ const double s2 = dot(v, v); if (s2 > 4.085 * 4.085) v = v * (4.085 / std::sqrt(s2)); }   // the game caps the pill's speed at 4.085 (direction kept) before its solver (2026-11-01)
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
		struct Job
		{
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
			std::atomic<bool> ready{ false };  // surfaces + grid built
			std::thread recipeThread;

			static long long cellkey(int x, int y) { return ((long long)(x + 1000000) << 32) | (unsigned)(y + 1000000); }
			void near_surfs(double x, double y, double r, std::vector<int>& out) const
			{
				out.clear();
				for (int gx = (int)std::floor(x - r); gx <= (int)std::floor(x + r); ++gx)
					for (int gy = (int)std::floor(y - r); gy <= (int)std::floor(y + r); ++gy)
					{
						auto it = grid.find(cellkey(gx, gy));
						if (it == grid.end()) continue;
						for (int i : it->second)
							if (S[i].lo.x - r <= x && x <= S[i].hi.x + r && S[i].lo.y - r <= y && y <= S[i].hi.y + r) out.push_back(i);
					}
				std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
			}
			int landing_contacts(V3 feet, V3 v_pre, const std::vector<int>& local, Row* rows, int maxr) const
			{
				V3 core[4]; int nc = 2;
				core[0] = { feet.x, feet.y, feet.z + CAP_LO }; core[1] = { feet.x, feet.y, feet.z + CAP_HI };
				if (len(v_pre) > SWEEP_MIN) { const V3 sw = v_pre * DT; core[2] = core[0] + sw; core[3] = core[1] + sw; nc = 4; }
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
				const int n = landing_contacts(feet - v_pre * DT, len(v_prev) > SWEEP_MIN ? v_pre : v_prev, local, prev, 32);
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
				FILE* f = nullptr;
				if (fopen_s(&f, name, "r") != 0 || !f) return false;
				char line[256];
				while (fgets(line, sizeof(line), f))
				{
					if (line[0] == '#') continue;
					Spot s;
					if (sscanf_s(line, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &s.x, &s.y, &s.z, &s.best, &s.share20, &s.share35, &s.fall, &s.driftX, &s.driftY) >= 6) found.push_back(s);
				}
				fclose(f);
				return true;
			}
			// the exact approach for one spot: a dense sweep at its landing point
			void recipe(Spot& s)
			{
				std::vector<int> local;
				near_surfs(s.x, s.y, 1.0, local);
				// the REFINE of the spot (2026-11-01): a 3x3 patch 0.05 apart, 16 start heights, falls 4-5 (starting bounces) and
				// 6..50, LEGIT drift only (the biped's horizontal speed is at most 2.25) - the scan's sparse table misses precise
				// approaches (headlong lightpost: scan 25.0, refined 29.0, the user's record 28-29)
				const int NH = 24, NPH = 16, NM = 10;
				static const double mags[NM] = { 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 };
				std::vector<double> vzs = { 4.0, 4.5, 5.0 }; for (double vz = 6; vz <= 50.01; vz += 2) vzs.push_back(vz);
				static const double PX[9] = { 0, -0.05, 0.05, 0, 0, -0.05, 0.05, -0.05, 0.05 }, PY[9] = { 0, 0, 0, -0.05, 0.05, -0.05, -0.05, 0.05, 0.05 };
				struct R { double best = 0; V3 bv{}; long long n20 = 0, n35 = 0, cases = 0; double feet = 0; int pt = 0; };
				std::vector<R> res(vzs.size() * NH * NM);
				Row rows[32], rr[32];
				for (size_t i = 0; i < res.size() && !stop; ++i)
				{
					const int vi = (int)(i / (NH * NM)), h = (int)((i / NM) % NH), mi = (int)(i % NM);
					const double vz = vzs[vi], ang = h * 6.283185307179586 / NH, m = mags[mi];
					const V3 v_pre{ m * std::cos(ang), m * std::sin(ang), -vz };
					R& o = res[i];
					const double side = len(v_pre) > SWEEP_MIN ? 1.0 : -1.0;   // a slow (unswept) fall is found sunk in, under the surface
					for (int pt = 0; pt < 9; ++pt)
					for (int ph = 0; ph < NPH; ++ph)
					{
						const double fh = side * (ph + 0.5) / NPH * vz * DT;
						const V3 at{ s.x + PX[pt], s.y + PY[pt], s.z + fh };
						if (!reachable(at, v_pre, local)) continue;
						const int n = landing_contacts(at, v_pre, local, rows, 32);
						if (n == 0) continue;
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
				if (stop) return;
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
						const double fh = (len(v_pre) > SWEEP_MIN ? 1.0 : -1.0) * (ph + 0.5) / NPH * vzs[bvi] * DT;
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
			void recipes()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				while (!stop)
				{
					if (!ready) { Sleep(100); continue; }
					// the nearest spot within 30 wu without a recipe (one at a time: the list grows while the scan runs)
					int pick = -1; Spot copy;
					{
						std::scoped_lock lk(m);
						float bd = 30.f * 30.f;
						for (int i = 0; i < (int)found.size(); ++i)
						{
							const Spot& s = found[i];
							if (s.recipe) continue;   // every spot gets a recipe, narrow windows too (2026-11-01: all predictions are shown)
							const float dx = s.x - px, dy = s.y - py, d2 = dx * dx + dy * dy;
							if (d2 < bd && std::fabs(s.z - pz) < 15.f) { bd = d2; pick = i; }
						}
						if (pick >= 0) copy = found[pick];
					}
					if (pick < 0) { Sleep(250); continue; }
					recipe(copy);
					std::scoped_lock lk(m);
					if (pick < (int)found.size() && found[pick].x == copy.x && found[pick].y == copy.y) found[pick] = copy;
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

		// the job's detached owner thread joins its workers; stopping only asks them to finish their current place
		void stop_job(const std::shared_ptr<Job>& j) { if (j) j->stop = true; }
	}

	int tick() { return (int)std::lround(1.0 / DT); }

	bool landingLaunch(float fx, float fy, float fz, float vx, float vy, float vz, float launch[3])
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j || !j->ready) return false;
		std::vector<int> local;
		j->near_surfs(fx, fy, 1.0, local);
		const V3 feet{ fx, fy, fz }, v_pre{ vx, vy, vz };
		if (!j->reachable(feet, v_pre, local)) return false;
		Row rows[32];
		const int n = j->landing_contacts(feet, v_pre, local, rows, 32);
		if (n == 0) return false;
		bool hard = false; for (int k = 0; k < n; ++k) if (!rows[k].soft) { hard = true; break; }
		if (!hard) return false;
		const V3 v = solve(rows, n, v_pre);
		launch[0] = (float)v.x; launch[1] = (float)v.y; launch[2] = (float)v.z;
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
				if (!g_job->loaded) { std::scoped_lock lk2(g_job->m); g_job->found.clear(); g_job->loaded = g_job->load(); }
				return;
			}
			old = g_job; g_job.reset();
		}
		stop_job(old);
		auto j = std::make_shared<Job>();
		j->key = key; j->tick = tick > 0 ? tick : 30;
		DT = 1.0 / j->tick;
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

	float progress()
	{
		std::shared_ptr<Job> j;
		{ std::scoped_lock lk(g_m); j = g_job; }
		if (!j) return 0.f;
		if (j->loaded) return 1.f;
		const size_t n = j->total.load();
		return n ? (float)j->done.load() / (float)n : 0.f;
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
}
