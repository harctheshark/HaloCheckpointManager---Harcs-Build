#include "pch.h"
#include "LaunchTrajectory.h"
#include "SettingsStateAndEvents.h"
#include "Render3DEventProvider.h"
#include "GameTickEventHook.h"
#include "IMakeOrGetCheat.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "RenderTextHelper.h"
#include "ScopedAtomicBool.h"
#include "H2CV_Halo2.h"
#include "H2CV_Collision.h"
#include "H2CV_Engine.h"
#include "H2CV_Mem.h"
#include <deque>
#include <unordered_map>
#include <limits>
#include <filesystem>
#include <fstream>
#include "DirPathContainer.h"
#include "GetPlayerViewAngle.h"
#include "GetPlayerDatum.h"
#include "GetObjectPhysics.h"
#include "GetObjectAddress.h"
#include "GetSoftCeilingData.h"
#include "ReachCollision.h"
#include <functional>
#include <unordered_set>
#include <intrin.h>

// ============================================================================================================
// Engine facts this overlay relies on (halo2.dll 1.3528, PE timestamp 0x68A0F0F2 - the build h2::buildOk() checks).
//
// Gravity  - qword at RVA 0x1652740 points at the physics constants block; +0x0 is gravity in wu/s^2.
//            Initialised by sub_1807613A0 from the constant 4.1712594 (RVA 0xB1BBE4), possibly overridden by the
//            game-settings vtable (+0x130). physics_set_gravity (sub_180761430) writes scale * 4.1712594.
//            Getter sub_180761310. Read every frame, so script / skull / settings changes are followed.
// Tick     - qword at RVA 0x15FE008 -> game time: +2 i16 ticks per second, +4 float seconds per tick (sub_180706760).
// Per tick - sub_180712850, for each rigid body of a Havok component:
//              sub_180711C00(&v, 100.0)   if |v| > 100 scale v to 100      (linear; angular is capped at 50)
//              sub_180713130              v = motion.v (+0x50) + (0, 0, -gravity * secondsPerTick), setLinearVelocity
//            Havok then integrates p += v * dt (semi-implicit Euler).
// State    - the player's Havok body: motion = entity +0x68, v @ +0x50, centre of mass (CUR) @ +0x80. This is the
//            same velocity HCM's Display 2D Info shows for Halo 2, so "stored momentum" is visible there too.
// ============================================================================================================

namespace launchtraj
{
	using V3 = SimpleMath::Vector3;
	using V4 = SimpleMath::Vector4;

	constexpr uintptr_t kPhysicsConstantsRva = 0x1652740;
	constexpr uintptr_t kGameTimeRva = 0x15FE008;
	constexpr float kDefaultGravity = 4.17125940f;
	constexpr float kGridCell = 8.f;
	constexpr int kMaxSteps = 7200;

	// ---- Halo: Reach (haloreach.dll, MCC 1.3528, PE timestamp 0x68A0EFE1) ----
	// Reach keeps its per-game globals in the game thread's TLS block (gs:0x58[_tls_index]):
	//   +0x330 -> physics constants: +0 gravity (initialised to 4.1712594 = 0x40857AF5 in sub_18014FD94, the same constant
	//            and the same {gravity, 1.0, 0.0011, vector} layout as Halo 2; physics_set_gravity sub_18019C604 writes
	//            scale * 4.1712594)
	//   +0xA0  -> game time: +6 i16 ticks per second (sub_180126C3A), +8 float seconds per tick (multiplied into
	//            gravity * dt by the per-tick gravity code, e.g. sub_18007542E)
	constexpr uintptr_t kReachTlsIndexRva = 0xC17B18;
	constexpr uintptr_t kReachPhysicsTlsOffset = 0x330;
	constexpr uintptr_t kReachGameTimeTlsOffset = 0xA0;
	constexpr uint32_t kReachTimestamp = 0x68A0EFE1;
	const SimpleMath::Vector4 kDeathColour{ 1.f, 0.12f, 0.12f, 1.f };

	inline V3 v3(const h2::Vec3& v) { return V3(v.x, v.y, v.z); }
	inline bool finite3(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

	// Moller-Trumbore, two-sided. Segment a + d*s, s in [0,1]. Returns s of the hit.
	inline bool segmentTriangle(const V3& a, const V3& d, const h2cv::Tri& t, float& sOut)
	{
		const V3 p0 = v3(t.a), p1 = v3(t.b), p2 = v3(t.c);
		const V3 e1 = p1 - p0, e2 = p2 - p0;
		const V3 h = d.Cross(e2);
		const float det = e1.Dot(h);
		if (std::fabs(det) < 1e-9f) return false;
		const float inv = 1.f / det;
		const V3 s = a - p0;
		const float u = s.Dot(h) * inv;
		if (u < 0.f || u > 1.f) return false;
		const V3 q = s.Cross(e1);
		const float v = d.Dot(q) * inv;
		if (v < 0.f || u + v > 1.f) return false;
		const float r = e2.Dot(q) * inv;
		if (r < 0.f || r > 1.f) return false;
		sOut = r;
		return true;
	}

	struct Hit { float s = 2.f; V3 n{ 0, 0, 1 }; };   // s = fraction along the queried segment

	// Uniform hash grid over a static triangle list, for segment queries.
	class TriGrid
	{
	public:
		void clear() { mTris.clear(); mCells.clear(); mBig.clear(); mStamp.clear(); mTick = 0; }
		bool empty() const { return mTris.empty(); }
		size_t size() const { return mTris.size(); }

		void build(const std::vector<h2cv::Tri>* a, const std::vector<h2cv::Tri>* b, float cell)
		{
			clear();
			mCell = cell; mInv = 1.f / cell;
			if (a) mTris.insert(mTris.end(), a->begin(), a->end());
			if (b) mTris.insert(mTris.end(), b->begin(), b->end());
			addAll();
		}

		void buildFrom(std::vector<h2cv::Tri>&& tris, float cell)
		{
			clear();
			mCell = cell; mInv = 1.f / cell;
			mTris = std::move(tris);
			addAll();
		}

		// Earliest hit along a -> b. Only replaces `hit` if this hit is earlier than the one it already holds, so several
		// grids / rays can be folded into one Hit. The normal is oriented against the direction of travel (collision
		// surfaces are tested two-sided).
		bool raycast(const V3& a, const V3& b, Hit& hit) const
		{
			if (mTris.empty()) return false;
			if (++mTick == 0) { std::fill(mStamp.begin(), mStamp.end(), 0u); mTick = 1; }
			float best = std::min(hit.s, 2.f);
			int64_t bestIdx = -1;
			const V3 d = b - a;
			auto test = [&](uint32_t i)
				{
					if (mStamp[i] == mTick) return;
					mStamp[i] = mTick;
					float s;
					if (segmentTriangle(a, d, mTris[i], s) && s < best) { best = s; bestIdx = i; }
				};
			for (uint32_t i : mBig) test(i);

			const float len = d.Length();
			if (!std::isfinite(len)) return false;
			const int pieces = std::clamp((int)std::ceil(len / mCell), 1, 4096);
			for (int k = 0; k < pieces; ++k)
			{
				const float s0 = (float)k / pieces, s1 = (float)(k + 1) / pieces;
				if (best <= s0) break; // anything found later along the segment cannot beat it
				const V3 p0 = a + d * s0, p1 = a + d * s1;
				const int x0 = cellOf(std::min(p0.x, p1.x)), x1 = cellOf(std::max(p0.x, p1.x));
				const int y0 = cellOf(std::min(p0.y, p1.y)), y1 = cellOf(std::max(p0.y, p1.y));
				const int z0 = cellOf(std::min(p0.z, p1.z)), z1 = cellOf(std::max(p0.z, p1.z));
				for (int x = x0; x <= x1; ++x)
					for (int y = y0; y <= y1; ++y)
						for (int z = z0; z <= z1; ++z)
						{
							auto it = mCells.find(key(x, y, z));
							if (it == mCells.end()) continue;
							for (uint32_t i : it->second) test(i);
						}
			}
			if (bestIdx < 0 || best > 1.f) return false;
			const h2cv::Tri& t = mTris[(size_t)bestIdx];
			const V3 p0 = v3(t.a);
			V3 n = (v3(t.b) - p0).Cross(v3(t.c) - p0);
			const float nLen = n.Length();
			n = nLen > 1e-12f ? n * (1.f / nLen) : V3(0, 0, 1);
			if (n.Dot(d) > 0.f) n = n * -1.f;
			hit.s = best;
			hit.n = n;
			return true;
		}

		// Earliest contact of a sphere of radius R whose centre moves c0 -> c0 + d (Reach's rounded body bottom). Like
		// raycast() it only replaces `hit` if this contact is earlier. hit.n = direction from the touched point to the
		// sphere centre - on a crease or an edge that is a blend, not the steepest facet's normal.
		bool sweepSphere(const V3& c0, const V3& d, float R, Hit& hit) const
		{
			if (mTris.empty()) return false;
			if (++mTick == 0) { std::fill(mStamp.begin(), mStamp.end(), 0u); mTick = 1; }
			float best = std::min(hit.s, 1.f);
			bool found = false;
			auto test = [&](uint32_t i)
				{
					if (mStamp[i] == mTick) return;
					mStamp[i] = mTick;
					float t;
					if (sphereTriangle(c0, d, R, mTris[i], best, t)) { best = t; found = true; }
				};
			for (uint32_t i : mBig) test(i);
			const V3 e = c0 + d;
			const int x0 = cellOf(std::min(c0.x, e.x) - R), x1 = cellOf(std::max(c0.x, e.x) + R);
			const int y0 = cellOf(std::min(c0.y, e.y) - R), y1 = cellOf(std::max(c0.y, e.y) + R);
			const int z0 = cellOf(std::min(c0.z, e.z) - R), z1 = cellOf(std::max(c0.z, e.z) + R);
			if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) return false;
			for (int x = x0; x <= x1; ++x)
				for (int y = y0; y <= y1; ++y)
					for (int z = z0; z <= z1; ++z)
					{
						auto it = mCells.find(key(x, y, z));
						if (it == mCells.end()) continue;
						for (uint32_t i : it->second) test(i);
					}
			if (!found) return false;
			// normal at the moment of contact: from the closest point of the nearest triangle to the centre
			const V3 c = c0 + d * best;
			float bestD2 = std::numeric_limits<float>::max();
			V3 n(0, 0, 1);
			for (int x = cellOf(c.x - R); x <= cellOf(c.x + R); ++x)
				for (int y = cellOf(c.y - R); y <= cellOf(c.y + R); ++y)
					for (int z = cellOf(c.z - R); z <= cellOf(c.z + R); ++z)
					{
						auto it = mCells.find(key(x, y, z));
						if (it == mCells.end()) continue;
						for (uint32_t i : it->second)
						{
							const V3 q = closestOnTriangle(c, mTris[i]);
							const float d2 = (c - q).Dot(c - q);
							if (d2 < bestD2) { bestD2 = d2; n = c - q; }
						}
					}
			for (uint32_t i : mBig)
			{
				const V3 q = closestOnTriangle(c, mTris[i]);
				const float d2 = (c - q).Dot(c - q);
				if (d2 < bestD2) { bestD2 = d2; n = c - q; }
			}
			const float nl = n.Length();
			n = nl > 1e-6f ? n * (1.f / nl) : V3(0, 0, 1);
			hit.s = best;
			hit.n = n;
			return true;
		}

		// indices of triangles with a point within r of c (for the launch logs' geometry dump); `seen` dedupes across calls
		void nearTriangles(const V3& c, float r, std::vector<uint32_t>& out, std::unordered_set<uint32_t>& seen, size_t cap) const
		{
			auto take = [&](uint32_t i)
				{
					if (out.size() >= cap || seen.count(i)) return;
					const V3 q = closestOnTriangle(c, mTris[i]);
					if ((q - c).Length() <= r) { seen.insert(i); out.push_back(i); }
				};
			for (uint32_t i : mBig) take(i);
			for (int x = cellOf(c.x - r); x <= cellOf(c.x + r); ++x)
				for (int y = cellOf(c.y - r); y <= cellOf(c.y + r); ++y)
					for (int z = cellOf(c.z - r); z <= cellOf(c.z + r); ++z)
					{
						auto it = mCells.find(key(x, y, z));
						if (it == mCells.end()) continue;
						for (uint32_t i : it->second) take(i);
					}
		}

		const h2cv::Tri& tri(uint32_t i) const { return mTris[i]; }

		static V3 closestOnTriangle(const V3& p, const h2cv::Tri& t)
		{
			// Ericson, Real-Time Collision Detection 5.1.5
			const V3 a = v3(t.a), b = v3(t.b), c = v3(t.c);
			const V3 ab = b - a, ac = c - a, ap = p - a;
			const float d1 = ab.Dot(ap), d2 = ac.Dot(ap);
			if (d1 <= 0.f && d2 <= 0.f) return a;
			const V3 bp = p - b; const float d3 = ab.Dot(bp), d4 = ac.Dot(bp);
			if (d3 >= 0.f && d4 <= d3) return b;
			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) return a + ab * (d1 / (d1 - d3));
			const V3 cp = p - c; const float d5 = ab.Dot(cp), d6 = ac.Dot(cp);
			if (d6 >= 0.f && d5 <= d6) return c;
			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) return a + ac * (d2 / (d2 - d6));
			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
			const float den = 1.f / (va + vb + vc);
			return a + ab * (vb * den) + ac * (vc * den);
		}

		// earliest t in [0, limit) at which the moving sphere touches the triangle (face, edges or corners)
		static bool sphereTriangle(const V3& c0, const V3& d, float R, const h2cv::Tri& tri, float limit, float& tOut)
		{
			const V3 a = v3(tri.a), b = v3(tri.b), c = v3(tri.c);
			V3 n = (b - a).Cross(c - a);
			const float nl = n.Length();
			if (nl < 1e-9f) return false;
			n = n * (1.f / nl);
			float s0 = n.Dot(c0 - a);
			float dn = n.Dot(d);
			if (s0 < 0.f) { n = n * -1.f; s0 = -s0; dn = -dn; }   // two-sided: the side the centre is on
			// already touching at the start: only a contact if moving further in (so a body resting on a floor does
			// not register its own floor every tick)
			auto inside = [&](const V3& q)
				{
					const V3 c1 = (b - a).Cross(q - a), c2 = (c - b).Cross(q - b), c3 = (a - c).Cross(q - c);
					return c1.Dot(n) >= -1e-6f && c2.Dot(n) >= -1e-6f && c3.Dot(n) >= -1e-6f;
				};
			float best = limit;
			bool hitAny = false;
			if (dn < 0.f)
			{
				const float t = (s0 - R) / -dn;
				if (t >= -1e-4f && t < best)
				{
					const V3 q = c0 + d * std::max(0.f, t) - n * R;
					if (inside(q)) { best = std::max(0.f, t); hitAny = true; }
				}
				else if (t < 0.f && s0 <= R && inside(c0 - n * s0)) { best = 0.f; hitAny = true; }
			}
			if (hitAny) { tOut = best; return true; }
			// edges (moving point vs cylinder of radius R) and corners (vs sphere of radius R)
			const float dd = d.Dot(d);
			if (dd < 1e-12f) return false;
			auto sphereT = [&](const V3& p, float& t) -> bool
				{
					const V3 m = c0 - p;
					const float B = m.Dot(d), C = m.Dot(m) - R * R;
					if (C <= 0.f) return B < 0.f ? (t = 0.f, true) : false;
					const float disc = B * B - dd * C;
					if (disc < 0.f || B >= 0.f) return false;
					t = (-B - std::sqrt(disc)) / dd;
					return true;
				};
			auto edgeT = [&](const V3& p, const V3& q, float& t) -> bool
				{
					const V3 e = q - p; const float ee = e.Dot(e);
					if (ee < 1e-12f) return false;
					const V3 m = c0 - p;
					const float md = m.Dot(e), nd = d.Dot(e);
					const float A = dd * ee - nd * nd, B = ee * m.Dot(d) - md * nd, C = ee * (m.Dot(m) - R * R) - md * md;
					if (std::fabs(A) < 1e-12f) return false;
					const float disc = B * B - A * C;
					if (disc < 0.f) return false;
					const float tt = (-B - std::sqrt(disc)) / A;
					if (tt < 0.f) return false;
					const float u = (md + tt * nd) / ee;
					if (u < 0.f || u > 1.f) return false;
					t = tt;
					return true;
				};
			float t;
			if (edgeT(a, b, t) && t < best) { best = t; hitAny = true; }
			if (edgeT(b, c, t) && t < best) { best = t; hitAny = true; }
			if (edgeT(c, a, t) && t < best) { best = t; hitAny = true; }
			if (sphereT(a, t) && t < best) { best = t; hitAny = true; }
			if (sphereT(b, t) && t < best) { best = t; hitAny = true; }
			if (sphereT(c, t) && t < best) { best = t; hitAny = true; }
			if (hitAny) tOut = best;
			return hitAny;
		}

	private:
		std::vector<h2cv::Tri> mTris;
		std::unordered_map<uint64_t, std::vector<uint32_t>> mCells;
		std::vector<uint32_t> mBig; // triangles too large to bucket: always tested
		mutable std::vector<uint32_t> mStamp;
		mutable uint32_t mTick = 0;
		float mCell = kGridCell, mInv = 1.f / kGridCell;

		int cellOf(float f) const { return (int)std::floor(std::clamp(f, -1.0e6f, 1.0e6f) * mInv); }
		static uint64_t key(int x, int y, int z)
		{
			return ((uint64_t)(x & 0x1FFFFF) << 42) | ((uint64_t)(y & 0x1FFFFF) << 21) | (uint64_t)(z & 0x1FFFFF);
		}

		void addAll()
		{
			mStamp.assign(mTris.size(), 0u);
			for (uint32_t i = 0; i < (uint32_t)mTris.size(); ++i)
			{
				const V3 a = v3(mTris[i].a), b = v3(mTris[i].b), c = v3(mTris[i].c);
				if (!finite3(a) || !finite3(b) || !finite3(c)) continue;
				const int x0 = cellOf(std::min({ a.x, b.x, c.x })), x1 = cellOf(std::max({ a.x, b.x, c.x }));
				const int y0 = cellOf(std::min({ a.y, b.y, c.y })), y1 = cellOf(std::max({ a.y, b.y, c.y }));
				const int z0 = cellOf(std::min({ a.z, b.z, c.z })), z1 = cellOf(std::max({ a.z, b.z, c.z }));
				const int64_t n = (int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
				if (n > 2048) { mBig.push_back(i); continue; }
				for (int x = x0; x <= x1; ++x)
					for (int y = y0; y <= y1; ++y)
						for (int z = z0; z <= z1; ++z)
							mCells[key(x, y, z)].push_back(i);
			}
		}
	};

	// One tick-aligned reading of the subject's Havok state (taken on the simulation thread).
	struct Sample
	{
		bool valid = false;
		bool vehicle = false;
		bool stored = false;
		uint32_t tick = 0;
		V3 pos{}, vel{};        // centre of mass, linear velocity
		V3 offLo{}, offHi{};    // capsule bottom / top relative to pos (bipeds)
		bool hasExtents = false;
		float dt = 1.f / 30.f;
		float yaw = std::numeric_limits<float>::quiet_NaN();   // view yaw (radians) on this tick, when known
		V3 vint{};              // Halo 2: the box motion's second velocity copy (motion +0xD0), logged for the release kick
		bool hasVint = false;
		// Halo 2: gap between the body's bottom and the top of a movable object's physics shape right under it (crate /
		// bowl you store on), NaN when there is none
		float objGap = std::numeric_limits<float>::quiet_NaN();
		// Reach: the game's own character physics (haloreach.dll 1.3528, decoded from Sapien's
		// character_physics_mode_ground.cpp): mode byte at biped + 0xA6E (1 = ground), and the ground mode's
		// m_ground_plane at biped + 0xAD8 (= character physics + 8 + 0x64). Logged to fit the release to it.
		int gameMode = -1;
		V3 gamePlane{};
		uintptr_t objBase = 0;
	};

	// Halo: Reach crouch storage. The stored velocity points almost straight down into the floor (vz -11 to -41 in 11
	// logged launches); tapping releases it as speed ALONG THE FLOOR: release tick v at ~26 deg below horizontal, then
	// the floor turns the vertical part into horizontal speed. Horizontal speed once on the ground: 0.75 - 0.86 of the
	// stored speed (mean ~0.82). The direction does NOT follow your aim or the tap: 9 releases with the view at 334, 341,
	// 13, 346, 358, 9, 343, 47 and even 194 deg all went to -11.3 deg - exactly opposite the stored velocity's small
	// horizontal part (168.5 deg), i.e. down the slope you are stored on. Only when that horizontal part is ~0 (flat
	// floor: direction unknown) does the overlay fall back to your aim.
	inline bool reachDownwardStorage(const Sample& s)
	{
		const float sp = s.vel.Length();
		return s.stored && sp > 1.f && s.vel.z < -0.7f * sp;
	}
	inline Sample reachReleaseToward(const Sample& s, float ratio, float aimYaw, bool* usedAim = nullptr)
	{
		Sample r = s;
		const float sp = s.vel.Length();
		const float hx = s.vel.x, hy = s.vel.y, hl = std::sqrt(hx * hx + hy * hy);
		float dx, dy;
		const bool fromSlope = hl > 0.02f * sp;
		if (fromSlope) { dx = -hx / hl; dy = -hy / hl; }
		else { dx = std::cos(aimYaw); dy = std::sin(aimYaw); }
		if (usedAim) *usedAim = !fromSlope;
		const float h = ratio * sp;
		r.vel = V3(h * dx, h * dy, 0.f);
		r.stored = false;   // released: the Halo 2 release ticks do not apply in Reach
		return r;
	}
	// Reach: the release tick usually dips into the floor first (vz ~ -0.39 x stored speed, then the floor flattens it).
	// Every release whose dip was 13.4 wu/s or more killed you on that tick (stored 34.6 / 40.9 / 41.4); dips of 11.4
	// and less never did (stored 28.5 and below). Two releases at 44.8 and 50.2 went straight onto the floor without
	// the dip and survived. Which kind you get is not predictable from the stored state, so above ~31 it is a risk.

	// A soft ceiling triangle (Reach), with its normal pointing to the side the player is allowed on.
	struct SoftTri { V3 a, b, c, n, centre; float radius = 0.f; };

	struct SimParams
	{
		float gravity = kDefaultGravity;
		float maxSpeed = 100.f;     // <= 0: no cap
		float damping = 0.f;        // 1/s
		float duration = 20.f;      // s, upper bound on the simulation
		bool collide = true;
		bool useExtents = true;
		int ignoreFirstTicks = 1;
		// Halo 2: the biped steers its velocity toward the stick's desired velocity (zero with no input) at the globals'
		// player information "run acceleration" on the ground and "airborne acceleration" in the air - 9.6 and 1.05 in
		// globals.globals, exactly the slow-downs the launch logs measured
		float groundFriction = 9.6f; // wu/s^2 of deceleration on walkable ground, no stick input (run acceleration)
		float airFriction = 1.05f;   // wu/s^2 of HORIZONTAL deceleration while airborne, no stick input (airborne acceleration)
		float groundSlideMaxSpeed = 6.f;   // above this horizontal speed a grounded body only gets airFriction (measured: friction
		                                   // at <= 3.4 wu/s, none at 11.9 wu/s - the exact switch point is not pinned down yet)
		bool releaseModel = true;          // model the release tick and the post-release ticks (below) for a stored launch
		bool releaseBump = true;           // release tick: +1 tick of ground friction on the horizontal speed
		float releaseKick = 0.f;           // upward speed added on the release tick of a stored launch (forward-tap releases: 2-5)
		// > 0: the line ends where falling speed passes this (mid-air fall death); 0 = off. Halo 2: sqrt(2 x 3.2086613 x
		// "maximum falling distance" 14) = 9.4785 (Halo 2 Sapien 0x696E9C turns the globals' falling distances into speeds)
		float fallDeathSpeed = 0.f;
		float releaseFriction = 9.3f;      // wu/s^2 along the whole velocity for the 4 ticks after release (fit: 9.3)
		bool wallBackstep = true;    // hard impacts cut the velocity to sqrt(2 g * 2 wu) = 4.085 wu/s (see the move below)
		float restitution = 0.f;     // 0 = no bounce
		float maxSlopeDeg = 50.f;    // steeper than this is not walkable: you slide down it (Halo 2 bipeds: "maximum slope angle" 50)
		// Halo: Reach ground impacts: the velocity is turned INTO the surface's plane keeping most of its speed (a crouch-
		// storage release pointing straight down at a slope came off it along the slope at 0.90 of the stored speed),
		// instead of Halo 2's 4.085 cut.
		bool reachImpacts = false;
		float reachImpactKeep = 0.85f;   // speed kept by a head-on (perpendicular) Reach impact; glancing ones keep more
		// Reach body height above its lowest point (the object position is not the feet): measured against the logged
		// collision geometry, the position stays 0.343 above the floor while sliding on flat ground (0.36 - 0.41 measured
		// vertically on the release slopes - the same clearance perpendicular to them). One height for crouched and
		// standing: the logs show no stand-up step.
		float reachStandHeight = 0.345f;
		float reachCrouchHeight = 0.345f;
		// > 0: sweep a sphere this size as the body's bottom instead of a ray. Off: against the logged geometry the
		// sphere caught the steep wall behind the storage spot at once, while the game clearly reacts to the facet right
		// under the body (its rebound angle below matches that facet at both logged spots)
		float reachRadius = 0.f;
		// The logged releases keep the crouched height through the release ticks (standing up at once put the line a
		// constant 0.27 wu high); the body stands up later, on the ground, where the 0.57 height was measured
		bool reachStandUpAtRelease = false;
		int reachStickyTicks = 5;
		float reachStickyGravity = 13.9f;
		float reachStickyFriction = 10.5f;
		float reachWalkableDeg = 35.f;
		float reachStickyGap = 0.15f;
		int reachReboundRecentTicks = 2;
		// Optional ray provider replacing the triangle grids (Reach: the game's own collision_test_vector, called on the game
		// thread). Same contract as TriGrid::raycast: only replaces `hit` with an earlier one.
		const std::function<bool(const V3&, const V3&, Hit&)>* rayFn = nullptr;
		// Reach soft ceilings ("Acceleration" type, applying to bipeds) and the force the game applies inside them -
		// decoded from Sapien (scenario_soft_ceilings.cpp, the soft ceiling force at 0x140793F30; constants from the
		// globals tag's soft ceiling block: biped spring constant, normal damping, tangent damping, min tangent damp
		// velocity). Per tick, with d = how far the body's position is behind the ceiling and n its outward normal:
		//   v += n * (K d dt);  then  v_n *= (1 - Cn dt),  and  v_t *= (1 - Ct dt) only while |v_t| > vmin
		// Fitted to three logged barrier hits: K 45, Cn 10, Ct 2, vmin 2 (rms 0.006 wu/s per tick).
		std::shared_ptr<const std::vector<SoftTri>> softTris;
		float softK = 45.f, softCn = 10.f, softCt = 2.f, softVmin = 2.f;
		float releaseDownToHorizontal = 0.28f;   // Halo 2: share of a stored downward speed that comes out horizontal on release
		// Halo 2 ground mode, from the Xbox decompilation (src/unknown_1f03e0.cpp 0x1f0870 / 0x1f2a80):
		//   h2GroundPush    - each tick the velocity change also gets -0.234 x the ground normal (pushed into the ground)
		//   h2GroundGravity - gravity is applied in ground mode on the ticks the acceleration limit was hit (still slowing)
		//   h2CreaseSupport - the ground normal from up to 3 contacts: one -> it; two -> the bisector unless the velocity
		//                     favours the first, or the crease direction when one faces down; three -> up
		float h2GroundPush = 0.f;
		bool h2GroundGravity = false;
		bool h2CreaseSupport = false;
		// Halo 2: the release tick's velocity change taken from a matching earlier launch (see the release memory)
		bool hasReleaseDv = false;
		V3 releaseDv{};
		float reachBounceGap = 0.03f;       // above the surface by at least this much when the tick began
		float reachBounceMaxDepth = 0.2f;   // and not driven deeper than this into it within the tick   // the release slopes (38.6 / 47 / 50 deg) never left you "standing"; 28 deg ground did
		// Reach rebound: a hit more than this far into a surface leaves it along the surface, tilted this far AWAY from it
		// (measured: crouch releases came off a 38.6 deg facet at 26.5 deg and off a 47.1 deg one at 34.8 - both 12.1 -
		// 12.3 deg out of the surface - and the slide then met 8 deg ground and left it at +3.5, again 12 deg out)
		float reachReboundDeg = 12.f;
		float reachReboundMinDeg = 15.f;   // 15.2 / 15.7 deg hits at 4.3 - 6.9 wu/s rebounded; one 15.6 deg one did not
		float reachReboundMinSpeed = 3.3f;   // a 3.15 wu/s hit slid, 3.5+ bounced
	};

	struct SimResult
	{
		std::vector<V3> pts;        // one point per tick, pts[0] = start
		std::vector<V3> vels;       // velocity after each tick (same indexing as pts)
		std::vector<uint8_t> groundedAt;
		bool hit = false;           // touched anything at all
		V3 firstContact{};
		V3 firstNormal{}, firstRawNormal{};   // contact normal used at the first contact / the triangle's own
		float firstContactTime = 0.f;
		int contacts = 0;
		bool atRest = false;        // came to a stop (otherwise the time limit ran out)
		bool wedged = false;        // stopped without standing on walkable ground
		float startFloorDepth = std::numeric_limits<float>::quiet_NaN();   // feet below (+) / above (-) the floor under the start
		float predictedKick = 0.f;  // vertical speed added on the release tick by releaseKick
		bool died = false;          // ended at a fall death
		V3 deathPos{};
		float deathTime = 0.f;
		float flightTime = 0.f, apex = 0.f, horizontal = 0.f, drop = 0.f;
	};

	// Steps the engine's model forward from s until the body comes to rest (or the time limit).
	//   airborne : v capped, v.z -= g dt, move
	//   contact  : move to the surface, remove the velocity going into it (plus restitution), keep the rest - so you
	//              slide along walls and steep slopes; a walkable surface (normal within maxSlope of up) grounds you
	//   grounded : velocity kept in the ground plane, slowed by groundFriction (no stick input), gravity held by the
	//              ground; a short probe under the feet follows slopes and drops you off ledges
	// Halo 2's ground normal from several contacts (Xbox decompilation src/unknown_1f03e0.cpp, 0x1f2a80): the surfaces
	// within contact range of the feet (one normal per distinct plane, closest first, at most 3; normals facing the body),
	// then: 1 -> its normal; 2 -> if one faces down (-0.999 < z < 0, the lowest such) the crease direction built from it,
	// else the bisector unless the velocity favours the first normal more; 3 -> up minus the downward one's part, else up
	inline V3 h2Support(const V3& feet, const V3& vel, const TriGrid* world, const TriGrid* objects, const V3& fallback)
	{
		struct C { V3 n; float d; };
		std::vector<C> cs;
		auto gather = [&](const TriGrid* g)
			{
				if (!g) return;
				std::vector<uint32_t> idx; std::unordered_set<uint32_t> seen;
				g->nearTriangles(feet, 0.4f, idx, seen, 256);
				for (uint32_t i : idx)
				{
					const h2cv::Tri& t = g->tri(i);
					const V3 a(t.a.x, t.a.y, t.a.z), b(t.b.x, t.b.y, t.b.z), c(t.c.x, t.c.y, t.c.z);
					V3 n = (b - a).Cross(c - a);
					const float nl = n.Length();
					if (nl < 1e-8f) continue;
					n *= 1.f / nl;
					const V3 cp = TriGrid::closestOnTriangle(feet, t);
					const float d = (feet - cp).Length();
					if (d > 0.2f) continue;                                   // contact range (0x1f2e60: up to 0.2 above)
					if ((feet - cp).Dot(n) < 0.f) n *= -1.f;                   // facing the body
					if (n.z <= -0.999f) continue;
					bool dup = false;
					for (C& e : cs) if (e.n.Dot(n) > 0.9998f) { dup = true; e.d = std::min(e.d, d); }
					if (!dup) cs.push_back({ n, d });
				}
			};
		gather(world); gather(objects);
		if (cs.empty()) return fallback;
		std::sort(cs.begin(), cs.end(), [](const C& x, const C& y) { return x.d < y.d; });
		if (cs.size() > 3) cs.resize(3);
		int sel = -1; float lowest = 0.f;
		for (size_t i = 0; i < cs.size(); ++i) if (lowest > cs[i].n.z && cs[i].n.z > -0.999f) { lowest = cs[i].n.z; sel = (int)i; }
		V3 out(0, 0, 1);
		if (cs.size() == 1) out = cs[0].n;
		else if (cs.size() == 2)
		{
			const V3 a = cs[0].n, b = cs[1].n;
			if (sel >= 0)
			{
				const V3 axis = a.Cross(b);
				out = sel == 0 ? axis.Cross(a) : b.Cross(axis);
			}
			else
			{
				V3 avg = a + b;
				const float al = avg.Length();
				if (al > 1e-6f) avg *= 1.f / al;
				out = vel.Dot(avg) > vel.Dot(a) ? a : avg;
			}
		}
		else if (sel >= 0)
		{
			const V3 n = cs[sel].n; const float k = -n.z;
			out = V3(n.x * k, n.y * k, n.z * k + 1.f);
		}
		const float ol = out.Length();
		return ol > 1e-4f ? out * (1.f / ol) : V3(0, 0, 1);
	}

	inline SimResult simulate(const Sample& s, const SimParams& p, const TriGrid* world, const TriGrid* objects)
	{
		SimResult r;
		V3 pos = s.pos, vel = s.vel;
		const float dt = (s.dt > 0.001f && s.dt < 0.2f) ? s.dt : (1.f / 30.f);
		const int steps = std::clamp((int)std::ceil(p.duration / dt), 1, kMaxSteps);
		const float dampMul = std::max(0.f, 1.f - p.damping * dt);
		const float cosWalk = std::cos(std::clamp(p.maxSlopeDeg, 0.f, 89.f) * 3.14159265f / 180.f);
		V3 offs[3] = { V3::Zero, s.offLo, s.offHi };
		bool reachCrouched = false;
		if (p.reachImpacts && s.hasExtents)
		{
			// Reach: the lowest ray is the body's bottom - crouched while storing, standing otherwise
			reachCrouched = s.stored;
			offs[1] = V3(0.f, 0.f, -(reachCrouched ? p.reachCrouchHeight : p.reachStandHeight));
			offs[2] = V3(0.f, 0.f, 0.3f);
		}
		const int nRays = (p.useExtents && s.hasExtents) ? 3 : 1;
		const int feet = nRays == 3 ? 1 : 0;
		constexpr float kBackOff = 0.01f;     // stay this far off a surface after touching it
		constexpr float kProbe = 0.3f;        // how far under the feet the ground is looked for
		constexpr float kRestSpeed = 0.05f;   // wu/s

		// Reach: the body's bottom is a sphere (swept), the rest rays; Halo 2: rays only
		auto sphereCentre = [&](const V3& at) { return at + offs[1] + V3(0.f, 0.f, p.reachRadius); };
		auto castBody = [&](const V3& from, const V3& to, Hit& h)
			{
				if (p.rayFn)
				{
					// game rays are dearer: the body's bottom and its centre
					(*p.rayFn)(from + offs[1], to + offs[1], h);
					(*p.rayFn)(from, to, h);
					return h.s <= 1.f;
				}
				for (int k = 0; k < nRays; ++k)
				{
					if (p.reachImpacts && p.reachRadius > 0.f && k == 1)
					{
						const V3 c0 = sphereCentre(from), d = to - from;
						if (world) world->sweepSphere(c0, d, p.reachRadius, h);
						if (objects) objects->sweepSphere(c0, d, p.reachRadius, h);
						continue;
					}
					if (world) world->raycast(from + offs[k], to + offs[k], h);
					if (objects) objects->raycast(from + offs[k], to + offs[k], h);
				}
				return h.s <= 1.f;
			};
		auto castOne = [&](const V3& from, const V3& to, Hit& h)
			{
				if (p.rayFn) { (*p.rayFn)(from, to, h); return h.s <= 1.f; }
				if (world) world->raycast(from, to, h);
				if (objects) objects->raycast(from, to, h);
				return h.s <= 1.f;
			};

		r.pts.reserve(std::min(steps + 1, 4096));
		r.pts.push_back(pos);
		r.vels.push_back(vel);
		r.groundedAt.push_back(0);
		float apexZ = pos.z;
		bool grounded = false;
		V3 groundN(0, 0, 1);
		int stuckTicks = 0;
		std::vector<float> softSide;   // +1 / -1 per soft ceiling: which way its stored normal faces the allowed side
		if (p.softTris && p.reachImpacts)
		{
			softSide.resize(p.softTris->size());
			for (size_t t = 0; t < softSide.size(); ++t)
			{
				const SoftTri& st = (*p.softTris)[t];
				softSide[t] = (s.pos - st.a).Dot(st.n) >= 0.f ? 1.f : -1.f;
			}
		}
		int stickyTicks = 0;   // Reach: ticks of "still grounded" after leaving walkable ground (see reachStickyTicks)
		int stickyCheck = 0;   // Reach: just left the ground - decided at the end of the NEXT tick (counts down to 0)
		int ticksSinceContact = 0;   // Reach: a bounce needs a recent contact (see reachReboundRecentTicks)
		bool touchedThisTick = false;
		bool wasGrounded = false;

		// Stored launch (measured from 13 logged launches): the release tick adds back one tick of ground friction to the
		// horizontal speed and applies no gravity; then for 4 ticks the body is still treated as standing on the ground it was
		// stored on - ~9.3 wu/s^2 of slow-down along its whole velocity, plus gravity - and only then flies normally.
		// How deep the feet are in the floor right under the start. Storing momentum leaves the body embedded, and the extra
		// upward kick on release (+3 to +19 wu/s logged) is suspected to be the push-out from that: kick ~ depth / tick.
		if (world || objects || p.rayFn)
		{
			const V3 top = pos + (s.hasExtents ? s.offHi : V3(0, 0, 0.35f));
			const float feetZ = pos.z + (s.hasExtents ? s.offLo.z : -0.3425f);
			Hit h;
			if (castOne(top, top - V3(0, 0, 4.f), h)) r.startFloorDepth = (top.z - 4.f * h.s) - feetZ;
		}

		// Every logged storage release behaves as a release from the ground (bump, 4 friction ticks, kick), including ones
		// where the collision probe under the feet found nothing within 0.3 wu - so it is not gated on that probe.
		const bool releaseOnGround = s.stored && p.releaseModel;

		for (int i = 0; i < steps; ++i)
		{
			const bool collideNow = p.collide && i >= p.ignoreFirstTicks;
			const V3 before = pos;

			if (p.maxSpeed > 0.f)
			{
				const float sp = vel.Length();
				if (sp > p.maxSpeed) vel *= p.maxSpeed / sp;
			}
			if (s.stored && p.releaseModel && i == 0 && p.hasReleaseDv)
			{
				// learned: exactly what this stored state did on its release tick before (Halo 2 releases are deterministic)
				vel = s.vel + p.releaseDv;
				r.predictedKick = p.releaseDv.z;
			}
			else if (s.stored && p.releaseModel && i == 0)
			{
				// release tick: horizontal speed +1 tick of ground friction, no gravity
				const float hs = std::sqrt(vel.x * vel.x + vel.y * vel.y);
				if (releaseOnGround && p.releaseBump && hs > 1e-6f) { const float k = (hs + p.groundFriction * dt) / hs; vel.x *= k; vel.y *= k; }
				// Stored pressing DOWN into the ground (rocket storage): the release drops the downward part - the
				// kick starts from zero vertical speed, not from the stored downward speed (otherwise a fast rocket
				// storage "dies" from falling on its first tick) - and about 0.28 of that downward speed comes out
				// as extra horizontal speed (4 logged rocket releases: 0.26 - 0.35; vertical after the release
				// 0.7 - 2.9 wu/s, inside the usual kick spread)
				if (releaseOnGround && vel.z < 0.f)
				{
					const float down = -vel.z;
					const float h = std::sqrt(vel.x * vel.x + vel.y * vel.y);
					if (h > 1e-6f) { const float k = (h + p.releaseDownToHorizontal * down) / h; vel.x *= k; vel.y *= k; }
					vel.z = 0.f;
				}
				if (releaseOnGround && p.releaseKick > 0.f)
				{
					r.predictedKick = p.releaseKick;
					vel.z += r.predictedKick;
				}
			}
			else if (releaseOnGround && i >= 1 && i <= 4)
			{
				const float sp = vel.Length();
				if (sp > 1e-6f) vel *= std::max(0.f, sp - p.releaseFriction * dt) / sp;
				vel.z -= p.gravity * dt;
			}
			else if (grounded)
			{
				vel -= groundN * vel.Dot(groundN);
				const float sp = vel.Length();
				const float decel = sp > p.groundSlideMaxSpeed ? p.airFriction : p.groundFriction;
				const float ns = std::max(0.f, sp - decel * dt);
				const bool limited = sp > decel * dt;   // still slowing down this tick (didn't reach the target)
				vel = sp > 1e-6f ? vel * (ns / sp) : V3::Zero;
				if (!p.reachImpacts)
				{
					if (p.h2GroundPush > 0.f) vel -= groundN * p.h2GroundPush;
					if (p.h2GroundGravity && limited) vel.z -= p.gravity * dt;
				}
			}
			else if (p.reachImpacts && stickyTicks > 0)
			{
				// Reach: just off walkable ground the body is still treated as standing on it for a few ticks - pulled down
				// at ~3.33 g and slowed ~10.5 wu/s^2 horizontally (logged: -13.90 wu/s^2 and 10.47 for 5 ticks after the slide
				// bounced off the flatter ground), then it flies normally
				--stickyTicks;
				vel.z -= p.reachStickyGravity * dt;
				const float hs = std::sqrt(vel.x * vel.x + vel.y * vel.y);
				if (hs > 1e-6f)
				{
					const float k = std::max(0.f, hs - p.reachStickyFriction * dt) / hs;
					vel.x *= k; vel.y *= k;
				}
			}
			else
			{
				// measured: gravity on z, and a constant-magnitude slow-down of the horizontal speed
				vel.z -= p.gravity * dt;
				const float hs = std::sqrt(vel.x * vel.x + vel.y * vel.y);
				if (hs > 1e-6f)
				{
					const float k = std::max(0.f, hs - p.airFriction * dt) / hs;
					vel.x *= k; vel.y *= k;
				}
			}
			vel *= dampMul;

			// Reach soft ceilings: spring push-back plus damping while the body is behind one
			if (softSide.size())
			{
				float bestDepth = 0.f; const SoftTri* best = nullptr; V3 bestN;
				for (size_t t = 0; t < softSide.size(); ++t)
				{
					const SoftTri& st = (*p.softTris)[t];
					if ((pos - st.centre).Length() > st.radius + 3.f) continue;
					const V3 nOut = st.n * softSide[t];
					const float depth = -(pos - st.a).Dot(nOut);
					if (depth <= 0.f || depth > 3.f) continue;
					// inside the triangle's outline (projected along its normal), with a small margin
					const V3 q = pos + nOut * depth;
					const V3 e0 = st.b - st.a, e1 = st.c - st.b, e2 = st.a - st.c;
					const float s0 = e0.Cross(q - st.a).Dot(st.n), s1 = e1.Cross(q - st.b).Dot(st.n), s2 = e2.Cross(q - st.c).Dot(st.n);
					const bool inside = (s0 >= -0.05f && s1 >= -0.05f && s2 >= -0.05f) || (s0 <= 0.05f && s1 <= 0.05f && s2 <= 0.05f);
					if (!inside) continue;
					if (depth > bestDepth) { bestDepth = depth; best = &st; bestN = nOut; }
				}
				if (best)
				{
					vel += bestN * (p.softK * bestDepth * dt);
					const float vn = vel.Dot(bestN);
					V3 vt = vel - bestN * vn;
					if (vt.Length() > p.softVmin) vt *= (1.f - p.softCt * dt);
					vel = vt + bestN * (vn * (1.f - p.softCn * dt));
				}
			}

			// Move. Measured from 40+ impacts in the launch logs:
			//   hard impact  : if the tick would run into a surface at more than sqrt(2 g * 2 wu) = 4.085 wu/s (into the
			//                  surface), the whole velocity is cut to 4.085 wu/s in the SAME direction and the tick is
			//                  re-run with it - walls, ceilings, objects and hard landings alike (exactly 4.085 after 30 of
			//                  them; 4.085 * |vz|/|v| left vertically when the surface removes the rest). The body then creeps
			//                  in at that speed until it touches.
			//   soft contact : the velocity into the surface is removed, the rest is kept (land and slide, slide along walls)
			{
				V3 delta = vel * dt;
				Hit h;
				bool touching = collideNow && castBody(pos, pos + delta, h);
				touchedThisTick = touching;
				// (Reach: the same cut for walls - a 25 wu/s launch into the wall at the far end of the logged path bled to
				// ~4 wu/s and then slid down it at a steady ~4 wu/s for seconds)
				if (touching && (p.wallBackstep || (p.reachImpacts && h.n.z < 0.2f)))
				{
					const float impact = std::sqrt(4.f * std::max(0.f, p.gravity));
					const float sp = vel.Length();
					if (-vel.Dot(h.n) > impact && sp > impact)
					{
						vel *= impact / sp;
						if (!r.hit) { r.hit = true; r.firstContact = pos + delta * h.s; r.firstContactTime = (i + h.s) * dt; }
						++r.contacts;
						delta = vel * dt;
						h = Hit{};
						touching = castBody(pos, pos + delta, h);
					}
				}
				if (!touching)
				{
					pos += delta;
				}
				else
				{
					pos += delta * h.s + h.n * kBackOff;
					const V3 n = h.n;   // Reach: from the swept sphere (blends creases and edges like the game's rounded body)
					const bool walkable = n.z >= cosWalk;
					const float into = vel.Dot(n);
					bool redirected = false, rebounded = false;
					// (Reach: on any floor-like surface, not only walkable ones - the second logged spot's facet is 47 deg)
					if (p.reachImpacts && n.z > 0.2f && into < -0.5f)
					{
						// keeps more of its speed the more glancing the hit: measured 0.86 - 0.91 for a crouch-storage
						// release into the slope (~56 deg into it) and 0.967 when that slide then met the flatter ground
						// (~30 deg into it): keep = 1 - 0.17 (into / speed)^2
						const float sp = vel.Length();
						const float f = sp > 1e-6f ? -into / sp : 0.f;
						const float keep = std::clamp(1.f - (1.f - p.reachImpactKeep) * 1.42f * f * f, 0.f, 1.f);
						const V3 t = vel - n * into;
						const float tl = t.Length();
						const float rb = p.reachReboundDeg * 3.14159265f / 180.f;
						if (tl > 1e-4f)
						{
							const V3 td = t * (1.f / tl);
							// Bounce or slide. The release tick (crouch storage pressed into the slope) always bounces off a
							// steep enough hit. After that, what decides it is how the contact happens (25 later contacts
							// checked against the logged geometry): a body that was clearly above the surface when the
							// tick began and hits it partway through bounces 12 deg out of it (gap >= 0.03 wu: 14 of
							// 15); one that is already resting on / sliding along it just slides (gap ~0: 9 of 9).
							// Very deep hits (> 0.2 wu into it within the tick) and steep ones (41 deg; 30.6 still bounced)
							// were absorbed.
							const float alongN = -delta.Dot(n);             // this tick's motion into the surface
							const float gapN = h.s * alongN, depthN = (1.f - h.s) * alongN;
							bool rebound;
							if (i == 0)
								rebound = f > std::sin(p.reachReboundMinDeg * 3.14159265f / 180.f) && -into > p.reachReboundMinSpeed;
							else
								rebound = gapN >= p.reachBounceGap && depthN <= p.reachBounceMaxDepth
									&& f >= std::sin(12.f * 3.14159265f / 180.f) && f <= std::sin(31.f * 3.14159265f / 180.f)
									&& n.z >= 0.8f;   // floors only: hits on rock faces (54 deg) deflected without lifting you
							rebounded = rebound;
							const V3 dir = rebound ? td * std::cos(rb) + n * std::sin(rb) : td;
							vel = dir * (sp * keep);
						}
						else vel = n * (sp * keep * std::sin(rb));
						redirected = true;
					}
					else if (into < 0.f) vel -= n * (into * (1.f + p.restitution));
					if (p.reachImpacts && n.z < 0.2f)
					{
						// pressed against a Reach wall: no faster than sqrt(4 g) (the steady ~4 wu/s slide down it)
						const float cap = std::sqrt(4.f * std::max(0.f, p.gravity));
						const float sp = vel.Length();
						if (sp > cap) vel *= cap / sp;
					}
					if (!r.hit) { r.firstNormal = n; r.firstRawNormal = h.n; }
					if (redirected)
					{
						// (any bounce: slow releases off the steep release facet also kept sliding with friction while the
						// slope stayed right under them)
						if (rebounded) { stickyTicks = 0; stickyCheck = 2; }
						if (reachCrouched && p.reachStandUpAtRelease)
						{
							// stands up on release: the position rises to standing height above the same bottom point
							reachCrouched = false;
							pos.z += p.reachStandHeight - p.reachCrouchHeight;
							offs[1] = V3(0.f, 0.f, -p.reachStandHeight);
						}
						// Reach keeps moving for the rest of the tick with the new velocity (measured: the release tick's
						// position is ~11% of the old velocity plus ~89% of the new one)
						// ...and if that runs into another surface within the same tick (fast releases cross the release facet
						// and meet the flatter ground below in one tick, leaving it at +3.5 deg), that is a contact of its own
						float remFrac = 1.f - h.s;
						for (int sub = 0; sub < 2; ++sub)
						{
							const V3 rest = vel * (dt * remFrac);
							Hit h2;
							if (!castBody(pos, pos + rest, h2)) { pos += rest; break; }
							pos += rest * h2.s + h2.n * kBackOff;
							const V3 n2 = h2.n;
							const float into2 = vel.Dot(n2);
							if (!(n2.z > 0.2f && into2 < -0.5f)) { if (into2 < 0.f) vel -= n2 * into2; break; }
							const float sp2 = vel.Length();
							const float f2 = sp2 > 1e-6f ? -into2 / sp2 : 0.f;
							const float keep2 = std::clamp(1.f - (1.f - p.reachImpactKeep) * 1.42f * f2 * f2, 0.f, 1.f);
							const V3 t2 = vel - n2 * into2;
							const float tl2 = t2.Length();
							if (tl2 < 1e-4f) break;
							const float along2 = -rest.Dot(n2);
							const float gap2 = h2.s * along2, depth2 = (1.f - h2.s) * along2;
							const float rb = p.reachReboundDeg * 3.14159265f / 180.f;
							const bool rebound2 = (i == 0)
								? (f2 > std::sin(p.reachReboundMinDeg * 3.14159265f / 180.f) && -into2 > p.reachReboundMinSpeed)
								: (gap2 >= p.reachBounceGap && depth2 <= p.reachBounceMaxDepth
									&& f2 >= std::sin(12.f * 3.14159265f / 180.f) && f2 <= std::sin(31.f * 3.14159265f / 180.f) && n2.z >= 0.8f);
							const V3 td2 = t2 * (1.f / tl2);
							vel = (rebound2 ? td2 * std::cos(rb) + n2 * std::sin(rb) : td2) * (sp2 * keep2);
							if (rebound2) { stickyTicks = 0; stickyCheck = 2; }
							++r.contacts;
							remFrac *= (1.f - h2.s);
						}
					}
					if (!r.hit) { r.hit = true; r.firstContact = pos; r.firstContactTime = (i + h.s) * dt; }
					++r.contacts;
					if (walkable && vel.Dot(n) < 0.5f && !(redirected && vel.Dot(n) > 0.05f))
					{
						grounded = true;
						groundN = n;
						vel -= groundN * vel.Dot(groundN);
						if (reachCrouched)
						{
							// back on the ground after the release: standing up (the slides were logged 0.57 above it)
							reachCrouched = false;
							pos.z += p.reachStandHeight - p.reachCrouchHeight;
							offs[1] = V3(0.f, 0.f, -p.reachStandHeight);
						}
					}
				}
			}

			// keep standing on the ground: follow slopes, fall off ledges
			if (collideNow && grounded)
			{
				const V3 f = pos + offs[feet];
				Hit h;
				bool probed;
				if (p.reachImpacts && p.reachRadius > 0.f)
				{
					const V3 c0 = sphereCentre(pos), d(0.f, 0.f, -kProbe);
					if (world) world->sweepSphere(c0, d, p.reachRadius, h);
					if (objects) objects->sweepSphere(c0, d, p.reachRadius, h);
					probed = h.s <= 1.f;
				}
				else probed = castOne(f, f - V3(0, 0, kProbe), h);
				if (probed && h.n.z >= cosWalk)
				{
					if (p.reachImpacts)
					{
						// Reach: a slope change turns the velocity onto the new ground keeping its speed (21.6 -> 23.4 wu/s
						// horizontal when the release slope flattened out); moving away from the ground (e.g. off the top
						// of a rise) leaves it - you fly on with air friction instead of sliding with ground friction
						const float sp = vel.Length();
						const V3 t = vel - h.n * vel.Dot(h.n);
						const float tl = t.Length();
						if (vel.Dot(h.n) > 0.05f) { grounded = false; stickyCheck = 2; }
						else
						{
							if (tl > 1e-4f) vel = t * (sp / tl);
							groundN = h.n;
							pos.z -= std::max(0.f, h.s * kProbe - kBackOff);
						}
					}
					else
					{
						groundN = (p.h2CreaseSupport && (world || objects)) ? h2Support(pos + offs[feet], vel, world, objects, h.n) : h.n;
						pos.z -= std::max(0.f, h.s * kProbe - kBackOff);
					}
				}
				else
				{
					grounded = false;
					if (p.reachImpacts) stickyCheck = 2;   // ran off an edge: maybe still "standing" for a few ticks
				}
			}

			ticksSinceContact = (touchedThisTick || grounded) ? 0 : ticksSinceContact + 1;

			// Reach keeps you "standing" for a few more ticks only if the ground is still right under you one tick after
			// leaving it (logged one tick after the bounce: 0.087 / 0.103 wu below the feet -> still standing for 5 ticks;
			// 0.194 / 0.245 / 0.278 -> flying)
			if (p.reachImpacts && stickyCheck > 0 && --stickyCheck == 0 && !grounded && collideNow)
			{
				const V3 f = pos + offs[feet];
				Hit hs;
				stickyTicks = castOne(f, f - V3(0, 0, p.reachStickyGap), hs) ? p.reachStickyTicks : 0;
			}

			// the kick pushes the feet exactly up to the floor surface; keep them 1 cm off it like any other contact, so the
			// next tick's rays do not start on the surface
			if (i == 0 && r.predictedKick > 0.f && std::isfinite(r.startFloorDepth) && r.startFloorDepth > 0.f) pos.z += kBackOff;
			if (!finite3(pos)) { pos = before; break; }
			r.pts.push_back(pos);
			r.vels.push_back(vel);
			r.groundedAt.push_back(grounded ? 1 : 0);
			apexZ = std::max(apexZ, pos.z);
			r.flightTime = (i + 1) * dt;

			// Fall death (measured: two deaths, both on the tick vertical speed passed ~9.5 wu/s downward - last alive at
			// -9.378 / -9.409, dead by -9.52 / -9.55; a fall with HCM invulnerability on passed -30 and lived)
			// Reach: the globals' "terminal velocity" (13 wu/s), while actually in the air. In 369 Reach logs, 47 launches
			// ended within one tick of falling past -13 wu/s in free flight; the only 2 that went past it and lived were
			// sliding along the slope at the time (in contact with it, so not falling)
			const bool fallingFree = !grounded && !(p.reachImpacts && touchedThisTick);
			if (p.fallDeathSpeed > 0.f && fallingFree && vel.z < -p.fallDeathSpeed)
			{
				r.died = true; r.deathPos = pos; r.deathTime = (i + 1) * dt;
				break;
			}
			if (grounded && vel.Length() < kRestSpeed) { r.atRest = true; break; }
			// pinned in a corner or on an unwalkable crease: not moving, not grounded
			stuckTicks = (collideNow && (pos - before).Length() < 1e-3f) ? stuckTicks + 1 : 0;
			if (stuckTicks >= 15) { r.atRest = true; r.wedged = true; break; }
		}

		const V3 d = r.pts.back() - s.pos;
		r.horizontal = std::sqrt(d.x * d.x + d.y * d.y);
		r.drop = d.z;
		r.apex = apexZ - s.pos.z;
		return r;
	}

	// From the recorded actual path: the deceleration while sliding to a stop after the first landing, for
	// calibrating Ground Friction. Landing = the tick vertical speed goes from falling to ~0.
	inline std::optional<float> measureGroundDecel(const std::vector<V3>& trace, float dt)
	{
		if (trace.size() < 6 || dt <= 0.f) return std::nullopt;
		const size_t n = trace.size() - 1;
		std::vector<float> h(n), vz(n);
		for (size_t i = 0; i < n; ++i)
		{
			const V3 d = trace[i + 1] - trace[i];
			h[i] = std::sqrt(d.x * d.x + d.y * d.y) / dt;
			vz[i] = d.z / dt;
		}
		for (size_t L = 1; L < n; ++L)
		{
			if (!(vz[L - 1] < -1.f && std::fabs(vz[L]) < 0.5f)) continue;
			size_t E = L;
			while (E + 1 < n && std::fabs(vz[E + 1]) < 0.5f && h[E + 1] <= h[E] + 0.05f && h[E] > 0.1f) ++E;
			if (E - L < 3 || h[L] - h[E] < 0.5f) return std::nullopt;
			return (h[L] - h[E]) / ((E - L) * dt);
		}
		return std::nullopt;
	}

	// Halo 2: movable objects' physics shapes around the player on one tick (sim thread) - kept for the ticks around the
	// start of a storage, since the sword knocks the object you store on away (about as hard as it pushes you)
	struct ObjSnap
	{
		uint32_t tick = 0;
		std::vector<h2cv::Tri> tris;
		std::vector<std::string> info;   // per object: type, position, body velocity, shape
	};

	// One logged launch: the state the prediction starts from, the ticks just before it, and the ticks that followed.
	struct LaunchCapture
	{
		std::vector<ObjSnap> storeSnaps;   // Halo 2: objects around the player when the storage started, and at release
		// Halo 2: what the overlay's release prediction used for this launch (fixed at the release, before it is remembered)
		std::vector<float> charRaw;   // Reach: 0x100 bytes of the biped's character physics at the release (biped + 0xA6C)
		bool overTopFace = false;
		bool reach = false;
		V3 memDp{};
		bool hasMemDv = false;
		V3 memDv{};
		std::string trigger;           // "stored-release" or "speed-up"
		std::string benchInfo;         // Launch Test Bench run and input timing, when the bench made this launch
		Sample start;
		std::vector<Sample> pre;       // ticks before start (context: the storage itself)
		std::vector<Sample> actual;    // start.tick + 1 onwards, as the game played them
		std::string endReason;
		int restTicks = 0;
	};

	inline void growBox(V3& mn, V3& mx, const V3& p) { mn = V3::Min(mn, p); mx = V3::Max(mx, p); }
}
using namespace launchtraj;


class LaunchTrajectory::Impl
{
private:
	// injected services
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<IMCCStateHook> mccStateWeak;
	std::weak_ptr<Render3DEventProvider> render3DEventProviderWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::shared_ptr<ObservedEvent<GameTickEvent>> mTickEvent;

	// callbacks
	ScopedCallback<ToggleEvent> mToggleCallback;
	std::unique_ptr<ScopedCallback<Render3DEvent>> mRenderCallback;
	std::unique_ptr<ScopedCallback<GameTickEvent>> mTickCallback;
	std::atomic_bool mEnabled = false;
	std::atomic_bool renderingMutex = false;
	std::atomic_int mSimInFlight = 0;

	// simulation-thread state, guarded by mStateMutex
	std::mutex mStateMutex;
	Sample mLatest;
	Sample mLastStored;
	Sample mHeld;               // the last stored-momentum state before a launch
	bool mHeldValid = false;
	uint64_t mHeldSinceMs = 0;
	bool mPinned = false;          // momentum storage in progress (see onSimTick)
	int mPinCandidate = 0;
	int mSlowTicks = 0;
	V3 mAnchor{};
	std::deque<V3> mTrace;      // actual positions, one per tick

	// settings mirrored for the simulation thread (written on toggle / render thread)
	std::atomic<float> mMinSpeedForSim = 5.f;
	std::atomic<float> mDurationForSim = 8.f;
	std::atomic_bool mLogForSim = false;
	std::atomic_bool mLogSpeedUpsForSim = true;

	// last hit (melee, sword, explosion, ...) seen on the simulation thread - guarded by mStateMutex
	// fromStoredRelease: the hit WAS the release of stored momentum (e.g. a rocket at your feet). Logged rocket releases
	// keep the stored horizontal velocity plus a push, but REPLACE a downward stored vz: release-tick vz came out
	// 2.86 / 2.81 / 2.84 / 2.87 from stored vz -2.25 / -2.25 / -10.54 / -12.55. So dv.z is stored as
	// "vz after - max(vz before, 0)" and applied the same way; dv.xy (which already includes the release bump) is added.
	struct LastHit { bool valid = false; V3 dv{}; float yaw = 0.f; bool hasYaw = false; uint64_t whenMs = 0; bool fromStoredRelease = false; };
	LastHit mLastHit;
	std::weak_ptr<GetPlayerViewAngle> mViewAngleWeak;

	// launch logging (simulation thread fills, render thread writes)
	std::deque<Sample> mRecent;               // last few ticks, for the pre-launch context rows
	std::optional<LaunchCapture> mCapture;    // launch being recorded right now
	std::deque<LaunchCapture> mFinished;      // recorded, waiting to be written
	std::string mLogDir;

	// render-thread only
	h2cv::CollisionWorld mWorld;
	TriGrid mWorldGrid, mObjectGrid;
	uintptr_t mBuiltSbsp = 0;
	bool mBuiltWithBarriers = false;
	bool mWarnedBuild = false;
	bool mLogAnnounced = false;
	uintptr_t mCheckedBase = 0;
	std::vector<std::string> mPendingMessages;   // simulation thread -> message log (guarded by mStateMutex)

	// which game this instance serves (HCM builds one per game)
	GameState::Value mGame = GameState::Value::Halo2;
	bool mIsReach = false;
	std::weak_ptr<GetPlayerDatum> mPlayerDatumWeak;      // Reach: the player's object
	std::weak_ptr<GetObjectPhysics> mObjectPhysicsWeak;  // Reach: its position / velocity
	std::weak_ptr<GetObjectAddress> mObjectAddressWeak;  // Reach: the biped object (character physics state)
	// Reach: the game's physics mode / ground plane on the latest tick, for the on-screen readout
	std::atomic<int> mGameMode = -1;
	std::atomic<float> mGamePlaneX = 0.f, mGamePlaneY = 0.f, mGamePlaneZ = 0.f;
	std::weak_ptr<GetSoftCeilingData> mSoftCeilingWeak;  // Reach: soft ceilings (HCM's own reader)
	std::shared_ptr<SoftCeilingDataProvider> mSoftProvider;
	std::shared_ptr<const std::vector<SoftTri>> mSoftTris;
	size_t mSoftSourceCount = 0;

	// render thread: the biped soft ceilings of type "Acceleration" (the ones that push you back)
	void updateSoftCeilings()
	{
		try
		{
			if (!mSoftProvider)
			{
				auto g = mSoftCeilingWeak.lock();
				if (!g) return;
				mSoftProvider = g->getSoftCeilingDataProvider();
			}
			auto lock = mSoftProvider->getSoftCeilings();
			if (!lock) return;
			const auto& all = *lock.value().get();
			if (all.size() == mSoftSourceCount && mSoftTris) return;
			auto tris = std::make_shared<std::vector<SoftTri>>();
			for (const auto& sc : all)
			{
				if (sc.softCeilingType != SoftCeilingType::Acceleration || !sc.appliesToBiped()) continue;
				const auto& vs = sc.getTriangleVertices();
				if (vs.size() < 3) continue;
				SoftTri t;
				// (VertexCollection = std::vector<DirectX::VertexPosition>: the point is its .position)
				t.a = V3(vs[0].position.x, vs[0].position.y, vs[0].position.z);
				t.b = V3(vs[1].position.x, vs[1].position.y, vs[1].position.z);
				t.c = V3(vs[2].position.x, vs[2].position.y, vs[2].position.z);
				V3 n = (t.b - t.a).Cross(t.c - t.a);
				const float nl = n.Length();
				if (nl < 1e-6f) continue;
				t.n = n * (1.f / nl);
				t.centre = (t.a + t.b + t.c) * (1.f / 3.f);
				t.radius = std::max({ (t.a - t.centre).Length(), (t.b - t.centre).Length(), (t.c - t.centre).Length() });
				tris->push_back(t);
			}
			mSoftTris = tris;
			mSoftSourceCount = all.size();
		}
		catch (...) {}
	}
	std::atomic<float> mReachGravity = kDefaultGravity;  // Reach: read from game-thread TLS each tick
	std::atomic<float> mReachDt = 1.f / 60.f;

	bool isActiveGame()
	{
		auto mcc = mccStateWeak.lock();
		return mcc && mcc->isGameCurrentlyPlaying(mGame);
	}

	uintptr_t moduleBase() const
	{
		return mIsReach ? reinterpret_cast<uintptr_t>(GetModuleHandleW(L"haloreach.dll")) : h2::base();
	}

	static uint32_t peTimestamp(uintptr_t base)
	{
		const uint32_t peOff = mem::get<uint32_t>(base + 0x3C, 0);
		return (peOff && peOff < 0x1000) ? mem::get<uint32_t>(base + peOff + 8, 0) : 0;
	}

	bool engineReady() const
	{
		const uintptr_t b = moduleBase();
		if (!b) return false;
		return mIsReach ? peTimestamp(b) == kReachTimestamp : h2::buildOk();
	}

	// Reach: gravity and tick length from the game thread's TLS (call on the simulation thread only)
	void readReachTls()
	{
		const uintptr_t base = moduleBase();
		const uint32_t idx = mem::get<uint32_t>(base + kReachTlsIndexRva, 0xFFFFFFFFu);
		if (idx > 1088) return;
		const uintptr_t tlsArray = (uintptr_t)__readgsqword(0x58);
		const uintptr_t slot = mem::q(tlsArray + 8ull * idx);
		if (!mem::okp(slot)) return;
		const uintptr_t phys = mem::q(slot + kReachPhysicsTlsOffset);
		const uintptr_t gt = mem::q(slot + kReachGameTimeTlsOffset);
		const float g = mem::okp(phys) ? mem::f32(phys, std::numeric_limits<float>::quiet_NaN()) : std::numeric_limits<float>::quiet_NaN();
		const float dt = mem::okp(gt) ? mem::f32(gt + 8, 0.f) : 0.f;
		if (std::isfinite(g) && std::fabs(g) < 1000.f) mReachGravity = g;
		if (dt > 0.001f && dt < 0.2f) mReachDt = dt;
	}

	bool readSubjectReach(Sample& out)
	{
		auto pd = mPlayerDatumWeak.lock();
		auto op = mObjectPhysicsWeak.lock();
		if (!pd || !op) return false;
		try
		{
			const Datum d = pd->getPlayerDatum();
			if (d.isNull()) return false;
			const SimpleMath::Vector3* pos = op->getObjectPosition(d);
			const SimpleMath::Vector3* vel = op->getObjectVelocity(d);
			if (!pos || !vel) return false;
			float pv[3], vv[3];
			if (!mem::copy(pv, reinterpret_cast<uintptr_t>(pos), sizeof(pv)) || !mem::copy(vv, reinterpret_cast<uintptr_t>(vel), sizeof(vv))) return false;
			out.pos = V3(pv[0], pv[1], pv[2]);
			out.vel = V3(vv[0], vv[1], vv[2]);
			// no Havok capsule read for Reach: rays at the object position, its bottom and above it (heights measured
			// from launch logs, see SimParams::reachStandHeight)
			out.offLo = V3(0.f, 0.f, -0.345f);   // the body's bottom (measured against the logged geometry: 0.343 above the floor while sliding)
			out.offHi = V3(0.f, 0.f, 0.3f);
			out.hasExtents = true;
			if (auto oa = mObjectAddressWeak.lock())
			{
				try
				{
					const uintptr_t base = oa->getObjectAddress(d);
					if (mem::okp(base))
					{
						out.objBase = base;
						uint8_t mode = 0xFF;
						float pl[3];
						if (mem::copy(&mode, base + 0xA6E, 1)) out.gameMode = mode;
						if (mem::copy(pl, base + 0xAD8, sizeof(pl))) out.gamePlane = V3(pl[0], pl[1], pl[2]);
						mGameMode = out.gameMode;
						mGamePlaneX = out.gamePlane.x; mGamePlaneY = out.gamePlane.y; mGamePlaneZ = out.gamePlane.z;
					}
				}
				catch (...) {}
			}
			return finite3(out.pos) && finite3(out.vel);
		}
		catch (HCMRuntimeException&) { return false; }
	}

	// Reach collision (ReachCollision.cpp): walked on the game thread in onSimTick, copied into a grid here when it changes
	TriGrid mReachGrid;
	long mReachGen = 0;
	std::atomic<bool> mReachWanted = false, mReachObjectsWanted = false;
	std::string mReachStatus = "Reach collision: not requested";
	void updateReachGrid(bool includeObjects)
	{
		mReachWanted = true;
		mReachObjectsWanted = includeObjects;
		mReachStatus = ReachCollision::status();
		std::vector<float> flat;
		if (!ReachCollision::snapshot(flat, mReachGen)) return;
		std::vector<h2cv::Tri> tris;
		tris.reserve(flat.size() / 9);
		for (size_t i = 0; i + 8 < flat.size(); i += 9)
		{
			const float* t = flat.data() + i;
			if (!std::isfinite(t[0]) || !std::isfinite(t[4]) || !std::isfinite(t[8])) continue;
			tris.push_back({ { t[0], t[1], t[2] }, { t[3], t[4], t[5] }, { t[6], t[7], t[8] } });
		}
		{
			// release memory level key (see the Halo 2 one): from the level geometry
			uint64_t h = 1469598103934665603ull ^ (uint64_t)tris.size();
			for (size_t i = 0; i < tris.size() && i < 64; ++i)
				for (const float f : { tris[i].a.x, tris[i].a.y, tris[i].a.z, tris[i].c.x, tris[i].c.y, tris[i].c.z })
					h = (h ^ (uint64_t)(int64_t)std::lround(f * 100.f)) * 1099511628211ull;
			mLevelKey = h ? h : 1;
		}
		mReachGrid.buildFrom(std::move(tris), kGridCell);
	}

	// ---- Halo: Reach - the game's own collision test (collision_test_vector) -----------------------------------------
	// haloreach.dll 1.3528 +0x12969C is the internal of collision_test_vector, found through the Reach mod tools' Sapien
	// (same engine source, asserts kept: physics\collisions.cpp, the "TEST_FLAG(flags.collision_flags,
	// _collision_test_structure_bit) || ..." check at line 1407; Sapien 0x140454BA0, wrapped by the public
	// collision_test_vector at 0x140454B60). Same 8 arguments in both:
	//   (u64 flags, bool, const real_point3d* point, const real_vector3d* vector, datum ignore1, datum ignore2,
	//    datum ignore3, collision_result* result) -> bool hit
	// flags: low dword = collision flags (bit 0 structure, 1 water, 3 instanced geometry, 11 front-facing surfaces,
	// 12 back-facing), high dword = object flags (bit 0 objects, then 1 << (object type + 1): scenery 7, machine 8,
	// crate 11, effect scenery 14) - mapped from Sapien's collision_debug_flag_* globals.
	// result: +0 type, +4 t (0..1 along the vector), +8 point, +0x2C plane (normal + d).
	// It reads game state through the game thread's TLS, so it only runs on the simulation thread: the render thread
	// posts what it wants predicted and draws the result computed on the previous tick.
	static constexpr uintptr_t kReachCollisionTestVectorRva = 0x12969C;
	typedef bool(__fastcall* CollisionTestVectorFn)(uint64_t, bool, const float*, const float*, uint32_t, uint32_t, uint32_t, void*);
	static bool callCollisionTestVector(CollisionTestVectorFn fn, uint64_t flags, const float* pt, const float* v, uint32_t ignore, void* res)
	{
		__try { return fn(flags, false, pt, v, ignore, 0xFFFFFFFFu, 0xFFFFFFFFu, res); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	struct GameSimRequest { uint64_t id = 0; Sample s; SimParams p; };
	std::mutex mGameSimMutex;
	std::unordered_map<std::string, GameSimRequest> mGameSimRequests;                      // render -> simulation thread
	std::unordered_map<std::string, std::pair<uint64_t, SimResult>> mGameSimResults;      // simulation -> render thread
	uint64_t mGameSimNextId = 0;
	std::atomic<bool> mUseGameRays = false, mGameRaysObjects = false;
	std::atomic<int> mGameRayCallsLastTick = 0;
	std::optional<LaunchCapture> mPendingLog;   // Reach: waiting for its prediction from the simulation thread
	uint64_t mPendingLogId = 0;
	SimParams mPendingLogParams;
	bool mPendingLogBarriers = false, mPendingLogObjects = false;

	uint64_t postGameSim(const std::string& key, const Sample& s, const SimParams& p)
	{
		std::scoped_lock lk(mGameSimMutex);
		GameSimRequest& r = mGameSimRequests[key];
		r.s = s; r.p = p; r.p.rayFn = nullptr; r.id = ++mGameSimNextId;
		return r.id;
	}

	// what the render thread calls instead of simulate(): in Reach with the game's collision, the newest result the
	// simulation thread computed for this line (falls back to the triangle grids until the first one is ready)
	SimResult simulateFor(const std::string& key, const Sample& s, const SimParams& p, const TriGrid* w, const TriGrid* o)
	{
		if (!(mIsReach && mUseGameRays.load() && p.collide)) return simulate(s, p, w, o);
		postGameSim(key, s, p);
		{
			std::scoped_lock lk(mGameSimMutex);
			auto it = mGameSimResults.find(key);
			if (it != mGameSimResults.end()) return it->second.second;
		}
		return simulate(s, p, w, o);
	}

	// simulation thread: run every posted prediction against the game's collision
	void runGameSims()
	{
		std::vector<std::pair<std::string, GameSimRequest>> reqs;
		{
			std::scoped_lock lk(mGameSimMutex);
			for (auto& kv : mGameSimRequests) reqs.emplace_back(kv.first, kv.second);
			mGameSimRequests.clear();
		}
		if (reqs.empty()) return;
		const auto fn = (CollisionTestVectorFn)(moduleBase() + kReachCollisionTestVectorRva);
		uint32_t ignore = 0xFFFFFFFFu;
		if (auto pd = mPlayerDatumWeak.lock()) { try { ignore = (uint32_t)pd->getPlayerDatum(); } catch (...) {} }
		uint64_t flags = (1ull << 0) | (1ull << 3) | (1ull << 11);   // structure, instanced geometry, front faces
		if (mGameRaysObjects.load())
			flags |= (uint64_t)((1u << 0) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 14)) << 32;   // scenery, machines, crates, effect scenery
		int calls = 0;
		constexpr int kBudget = 6000;   // rays per tick for all lines together; past this a line just stops colliding
		const std::function<bool(const V3&, const V3&, Hit&)> ray = [&](const V3& a, const V3& b, Hit& h) -> bool
			{
				if (calls >= kBudget) return false;
				++calls;
				alignas(16) float pt[4] = { a.x, a.y, a.z, 0.f };
				alignas(16) float v[4] = { b.x - a.x, b.y - a.y, b.z - a.z, 0.f };
				alignas(16) unsigned char res[0x200] = {};
				if (!callCollisionTestVector(fn, flags, pt, v, ignore, res)) return false;
				const float t = *(const float*)(res + 0x4);
				const float* pl = (const float*)(res + 0x2C);
				if (!(t >= 0.f && t <= 1.f) || t >= h.s) return false;
				V3 n(pl[0], pl[1], pl[2]);
				const float nl = n.Length();
				if (!(nl > 0.5f && nl < 1.5f)) return false;
				n = n * (1.f / nl);
				if (n.Dot(V3(v[0], v[1], v[2])) > 0.f) n = n * -1.f;
				h.s = t; h.n = n;
				return true;
			};
		for (auto& [key, rq] : reqs)
		{
			SimParams p2 = rq.p;
			p2.rayFn = &ray;
			SimResult r = simulate(rq.s, p2, nullptr, nullptr);
			std::scoped_lock lk(mGameSimMutex);
			mGameSimResults[key] = { rq.id, std::move(r) };
		}
		mGameRayCallsLastTick = calls;
	}

	float gravityNow()
	{
		return mIsReach ? mReachGravity.load() : readGravity();
	}

	// ---------------------------------------------------------------- simulation thread

	static float readSecondsPerTick()
	{
		const uintptr_t gt = mem::q(h2::base() + kGameTimeRva);
		const float dt = mem::okp(gt) ? mem::f32(gt + 4, 0.f) : 0.f;
		return (dt > 0.001f && dt < 0.2f) ? dt : (1.f / 30.f);
	}

	static bool readSubject(Sample& out)
	{
		const uintptr_t unit = h2::localPlayerUnitObject();
		if (!unit) return false;

		h2::Motion m;
		uint32_t vehicleDatum = 0;
		const uintptr_t vehicle = h2::vehicleOfUnit(unit, &vehicleDatum);
		if (vehicle)
		{
			uintptr_t bodies[8];
			const int nb = h2::havokBodies(vehicle, bodies);
			bool found = false;
			for (int i = 0; i < nb && !found; ++i) found = h2::readMotion(bodies[i], m, true);
			if (!found) return false;
			out.vehicle = true;
		}
		else
		{
			const uintptr_t ent = h2::havokEntityForObject(unit);
			if (!ent) return false;
			if (!h2::readMotion(ent, m) && !h2::readMotion(ent, m, true)) return false;

			const h2::Capsule cap = h2::entityCapsule(ent);
			if (cap.ok)
			{
				const V3 a = v3(cap.a), b = v3(cap.b), com = v3(m.cur);
				const V3 lo = (a.z < b.z ? a : b), hi = (a.z < b.z ? b : a);
				constexpr float skin = 0.f; // measured: a standing biped's capsule bottom rests 0.01 wu (= kBackOff) above the floor
				out.offLo = lo - V3(0, 0, cap.r - skin) - com;
				out.offHi = hi + V3(0, 0, cap.r - skin) - com;
				out.hasExtents = finite3(out.offLo) && finite3(out.offHi) && out.offLo.Length() < 4.f && out.offHi.Length() < 4.f;
			}
		}
		if (!m.ok) return false;
		out.pos = v3(m.cur);
		out.vel = v3(m.v);
		out.vint = v3(m.vint);
		out.hasVint = finite3(out.vint);
		return finite3(out.pos) && finite3(out.vel);
	}

	// ---- Halo 2 release memory ---------------------------------------------------------------------------------------
	// Halo 2 releases are fully deterministic in the stored state (identical states gave identical release ticks to the
	// thousandth in every bench run), but the release kick depends on the Havok contact between your body and the object
	// you store on (top face vs rim, stored velocity sign, speed) in ways not modelled yet. So every logged stored-release
	// is remembered - level, stored position, stored velocity -> the release tick's velocity change - in
	// LaunchLogs\ReleaseMemory_H2.csv, and a storage that matches one (position within 0.03 wu, velocity within
	// 0.5 wu/s) uses that release instead of the kick model.
	struct ReleaseRec { uint64_t level = 0; V3 pos, vel, dv; V3 dp{}; bool hasDp = false; };   // dp: position change over the release tick
	bool overTopFace(const Sample& st) const
	{
		return !mIsReach && st.stored && std::isfinite(st.objGap) && st.objGap > -0.05f && st.objGap < 0.6f;
	}
	std::atomic<uint64_t> mLegacyLevelKey = 0;   // the first release memory build keyed levels by the bsp address
	std::mutex mRelMutex;
	std::vector<ReleaseRec> mRelMem;
	bool mRelLoaded = false;
	std::atomic<uint64_t> mLevelKey = 0;   // render thread sets it (structure bsp id)

	std::filesystem::path releaseMemoryPath() const
	{
		return std::filesystem::path(mLogDir.empty() ? std::string(".") : mLogDir) / "LaunchLogs" / (mIsReach ? "ReleaseMemory_Reach.csv" : "ReleaseMemory_H2.csv");
	}
	void loadReleaseMemory()
	{
		std::scoped_lock lk(mRelMutex);
		if (mRelLoaded) return;
		mRelLoaded = true;
		try
		{
			std::ifstream in(releaseMemoryPath());
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#') continue;
				ReleaseRec r; unsigned long long lv = 0;
				const int got = sscanf_s(line.c_str(), "%llu,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &lv, &r.pos.x, &r.pos.y, &r.pos.z, &r.vel.x, &r.vel.y, &r.vel.z,
					&r.dv.x, &r.dv.y, &r.dv.z, &r.dp.x, &r.dp.y, &r.dp.z);
				if (got == 10 || got == 13)
				{
					r.level = lv;
					r.hasDp = got == 13;
					mRelMem.push_back(r);
				}
			}
		}
		catch (...) {}
	}
	// simulation thread: a stored-release's first tick
	void rememberRelease(const Sample& start, const Sample& first)
	{
		if (first.tick != start.tick + 1) return;
		const V3 dv = first.vel - start.vel;
		const float s0 = start.vel.Length(), s1 = first.vel.Length();
		if (s0 < 1.f || (s1 < 4.f && s1 < s0 * 0.33f) || dv.Length() > 60.f) return;   // jumps / cancels
		ReleaseRec r; r.level = mLevelKey.load(); r.pos = start.pos; r.vel = start.vel; r.dv = dv; r.dp = first.pos - start.pos; r.hasDp = true;
		{
			std::scoped_lock lk(mRelMutex);
			for (const ReleaseRec& e : mRelMem)
				if (e.level == r.level && (e.pos - r.pos).Length() < 1e-3f && (e.vel - r.vel).Length() < 1e-3f) return;   // known
			mRelMem.push_back(r);
		}
		try
		{
			const auto path = releaseMemoryPath();
			std::filesystem::create_directories(path.parent_path());
			const bool fresh = !std::filesystem::exists(path);
			std::ofstream o(path, std::ios::app);
			if (fresh) o << "# level,stored_pos_x,y,z,stored_vel_x,y,z,release_dv_x,y,z,release_dpos_x,y,z  (" << (mIsReach ? "Reach" : "Halo 2") << " release memory - delete to forget)\n";
			o << r.level << ',' << f6(r.pos.x) << ',' << f6(r.pos.y) << ',' << f6(r.pos.z) << ',' << f6(r.vel.x) << ',' << f6(r.vel.y) << ',' << f6(r.vel.z)
				<< ',' << f6(r.dv.x) << ',' << f6(r.dv.y) << ',' << f6(r.dv.z) << ',' << f6(r.dp.x) << ',' << f6(r.dp.y) << ',' << f6(r.dp.z) << "\n";
		}
		catch (...) {}
	}
	// render thread: the remembered release for this stored state, if one matches
	// Reach: the release tick's velocity scales with the stored speed along a fixed direction (41 bench releases at
	// the second spot: 15 - 41 wu/s all left at heading -34.4 in-plane, 0.78 of the speed), except where it flips to
	// another branch (22, 44 wu/s) - so a storage in the same spot and stored DIRECTION (within 2 deg) takes the
	// remembered release of the nearest stored speed, scaled to this one. Returns the velocity after the release tick.
	struct ReachRecall { V3 v1, dp; };
	std::optional<ReachRecall> recallReachRelease(const Sample& st)
	{
		if (!mIsReach || !st.stored) return std::nullopt;
		const float s0 = st.vel.Length();
		if (s0 < 1.f) return std::nullopt;
		std::scoped_lock lk(mRelMutex);
		const uint64_t lv = mLevelKey.load();
		const ReleaseRec* best = nullptr; float bestDs = 1e9f;
		for (const ReleaseRec& e : mRelMem)
		{
			if (e.level != lv || (e.pos - st.pos).Length() > 0.03f) continue;
			const float es = e.vel.Length();
			if (es < 1.f) continue;
			const float cosA = e.vel.Dot(st.vel) / (es * s0);
			if (cosA < 0.99939f) continue;                       // 2 deg
			const float ds = std::fabs(es - s0);
			if (ds < bestDs) { bestDs = ds; best = &e; }
		}
		if (!best || bestDs > 3.f) return std::nullopt;          // nearest remembered speed within 3 wu/s
		const float k = s0 / best->vel.Length();
		const V3 v1 = (best->vel + best->dv) * k;
		// where the release tick left the body (older records: ~0.84 of a tick at the new velocity, as measured)
		const V3 dp = best->hasDp ? best->dp * k : v1 * (0.84f * st.dt);
		return ReachRecall{ v1, dp };
	}

	// Reach: predict from the state AFTER a remembered release tick (that tick's contact with the slope is what the
	// memory replaces - re-running it would redo the release), then put the stored state back in front so tick k of
	// the result is still tick k after the release
	static Sample afterRelease(const Sample& start, const ReachRecall& rr)
	{
		Sample s1 = start;
		s1.pos = start.pos + rr.dp;
		s1.vel = rr.v1;
		s1.stored = false;
		s1.tick = start.tick + 1;
		return s1;
	}
	SimResult simulateAfterRelease(const Sample& start, const ReachRecall& rr, const SimParams& p, const TriGrid* world, const TriGrid* objects,
		const char* gameSimKey = nullptr)
	{
		const Sample s1 = afterRelease(start, rr);
		SimResult r = gameSimKey ? simulateFor(gameSimKey, s1, p, world, objects) : simulate(s1, p, world, objects);
		r.pts.insert(r.pts.begin(), start.pos);
		r.vels.insert(r.vels.begin(), start.vel);
		if (!r.groundedAt.empty()) r.groundedAt.insert(r.groundedAt.begin(), (uint8_t)0);
		const float dt = start.dt;
		r.flightTime += dt;
		if (r.hit) r.firstContactTime += dt;
		if (r.died) r.deathTime += dt;
		return r;
	}

	std::optional<V3> recallRelease(const Sample& st, int* matches = nullptr)
	{
		if (mIsReach || !st.stored) return std::nullopt;
		std::scoped_lock lk(mRelMutex);
		const uint64_t lv = mLevelKey.load(), legacy = mLegacyLevelKey.load();
		const ReleaseRec* best = nullptr; float bestScore = 1e9f; int n = 0;
		for (const ReleaseRec& e : mRelMem)
		{
			if (e.level != lv && (legacy == 0 || e.level != legacy)) continue;
			const float dp = (e.pos - st.pos).Length(), dvv = (e.vel - st.vel).Length();
			if (dp > 0.03f || dvv > 0.5f) continue;
			++n;
			const float score = dvv + 10.f * dp;
			if (score < bestScore) { bestScore = score; best = &e; }
		}
		if (matches) *matches = n;
		if (!best) return std::nullopt;
		return best->dv;
	}

	// ---- Launch Test Bench ------------------------------------------------------------------------------------------
	// Repeats a storage launch automatically from the player's own checkpoint: revert, press the storage inputs (crouch
	// spam, optional melee) with the real keyboard keys, wait until the storage is detected, tap the release key, let
	// the game play the launch out (it is logged like any other), revert, and go again with every input timing shifted
	// by a step, so each run arrives a little differently. Keys are sent with SendInput, so MCC must have focus.
	// Runs on the simulation thread, one decision per game tick.
	struct BenchConfig
	{
		// The storage inputs are CYCLES (Halo 2 sword storage repeats them): each cycle spams the attack key for
		// attackTicks ticks from its start (0 = no attack - Reach), then spams crouch for crouchTicks ticks starting
		// crouchDelay ticks after the cycle start (the frame the attack lands). Cycle c starts at
		// firstCrouch + run x step + c x cycleLength ticks after the revert.
		int runs = 50, firstCrouch = 5, crouchTicks = 30, step = 1, releaseDelay = 10, holdTicks = 2;
		int cycles = 1, cycleLength = 40, attackTicks = 0, crouchDelay = 0;
		int crouchKey = 0xA2, meleeKey = 'Q', releaseKey = 'W';
		float flightTimeout = 12.f;
		// Reach: sweep the STORED speed itself - while stored, the bench sets the speed to speedStart + run x speedStep
		// (direction kept) through the object velocity HCM can write. 0 = leave the stored speed alone.
		float speedStart = 0.f, speedStep = 0.f;
	};
	std::atomic<bool> mBenchWanted = false;
	bool mBenchWantedLast = false;         // render thread: edge detection on the setting
	std::mutex mBenchMutex;
	BenchConfig mBenchCfg;                 // copied from the settings by the render thread
	enum class BenchState { Idle, Reverting, Sequence, Flight } mBench = BenchState::Idle;
	int mBenchRun = 0, mBenchT = 0, mBenchPinnedTicks = 0, mBenchReleaseLeft = 0, mBenchWait = 0;
	uint32_t mBenchLastTick = 0;
	bool mBenchCaptureSeen = false;
	std::atomic<bool> mBenchMenuOpen = false;   // HCM's menu is open (render thread sets it)
	bool mBenchResume = false; int mBenchResumeRun = 0;
	std::atomic<bool> mBenchRevertRequest = false;   // fired on the render thread (HCM events are GUI-side)
	std::atomic<bool> mBenchFinished = false;        // tells the render thread to switch the setting off
	bool mBenchKeyDown[3] = { false, false, false };   // crouch, melee, release
	std::atomic<int> mBenchRunShown = 0;
	std::atomic<int> mBenchLogsAtStart = 0, mBenchLogged = 0;
	std::string mBenchStatus;              // guarded by mBenchMutex

	static void benchSendKey(int vk, bool down)
	{
		if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
		{
			// mouse buttons (1 left, 2 right, 4 middle, 5 / 6 side buttons)
			INPUT m{};
			m.type = INPUT_MOUSE;
			if (vk == VK_LBUTTON) m.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
			else if (vk == VK_RBUTTON) m.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
			else if (vk == VK_MBUTTON) m.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
			else { m.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; m.mi.mouseData = vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2; }
			SendInput(1, &m, sizeof(INPUT));
			return;
		}
		INPUT in{};
		in.type = INPUT_KEYBOARD;
		in.ki.wVk = (WORD)vk;
		in.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
		in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
		if (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END
			|| vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)
			in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
		SendInput(1, &in, sizeof(INPUT));
	}
	void benchKey(int which, int vk, bool down)
	{
		if (vk <= 0 || mBenchKeyDown[which] == down) return;
		benchSendKey(vk, down);
		mBenchKeyDown[which] = down;
	}
	void benchReleaseAll(const BenchConfig& c)
	{
		benchKey(0, c.crouchKey, false); benchKey(1, c.meleeKey, false); benchKey(2, c.releaseKey, false);
	}
	void benchSetStatus(std::string st) { std::scoped_lock lk(mBenchMutex); mBenchStatus = std::move(st); }

	// simulation thread: scale the player's stored velocity to `speed`, keeping its direction
	void benchSetStoredSpeed(float speed)
	{
		if (!mIsReach)
		{
			// Halo 2: the stored velocity lives in the player's Havok rigid motion (q(entity + 0x68)): linear velocity at
			// +0x50 and, for box motions (the player's stabilized box), the integrator's copy at +0xD0 - both scaled
			try
			{
				const uintptr_t unit = h2::localPlayerUnitObject();
				const uintptr_t ent = unit ? h2::havokEntityForObject(unit) : 0;
				const uintptr_t motion = ent ? mem::q(ent + 0x68) : 0;
				if (!mem::okp(motion)) return;
				for (uintptr_t off : { (uintptr_t)0x50, (uintptr_t)0xD0 })
				{
					float v[3];
					if (!mem::copy(v, motion + off, sizeof(v))) continue;
					const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					if (!(len > 0.01f && len < 1000.f)) continue;
					for (float& x : v) x *= speed / len;
					mem::put(motion + off, v, sizeof(v));
				}
			}
			catch (...) {}
			return;
		}
		auto op = mObjectPhysicsWeak.lock();
		auto pd = mPlayerDatumWeak.lock();
		if (!op || !pd) return;
		try
		{
			const Datum d = pd->getPlayerDatum();
			if (d.isNull()) return;
			SimpleMath::Vector3* v = op->getObjectVelocityMutable(d);
			if (!v) return;
			const float len = v->Length();
			if (len > 0.01f) *v *= speed / len;
		}
		catch (...) {}
	}

	void benchTick(uint32_t tick)
	{
		BenchConfig c;
		{ std::scoped_lock lk(mBenchMutex); c = mBenchCfg; }
		if (!mBenchWanted.load())
		{
			if (mBench != BenchState::Idle) { benchReleaseAll(c); mBench = BenchState::Idle; benchSetStatus("stopped"); }
			return;
		}
		const bool tickJumpedBack = mBenchLastTick != 0 && tick < mBenchLastTick;
		mBenchLastTick = tick;
		// keys sent while the HCM menu has focus never reach the game: wait for it to close (a run that was in progress
		// is started over from its revert)
		if (mBenchMenuOpen.load())
		{
			benchReleaseAll(c);
			if (mBench != BenchState::Idle) { mBenchResumeRun = mBenchRun; mBenchResume = true; mBench = BenchState::Idle; }
			benchSetStatus("waiting - close the HCM menu to start");
			return;
		}
		auto revert = [&]()
			{
				benchReleaseAll(c);
				mBenchRevertRequest = true;
				mBench = BenchState::Reverting; mBenchWait = 0;
			};
		switch (mBench)
		{
		case BenchState::Idle:
			mBenchRun = mBenchResume ? mBenchResumeRun : 0; mBenchRunShown = mBenchRun; mBenchResume = false;
			benchSetStatus("reverting to your checkpoint");
			revert();
			break;
		case BenchState::Reverting:
			// the revert has happened when the game tick jumps back; give up waiting after ~5 s
			if (tickJumpedBack || ++mBenchWait > 150)
			{
				mBench = BenchState::Sequence; mBenchT = 0; mBenchPinnedTicks = 0; mBenchReleaseLeft = 0;
				benchSetStatus(std::format("run {}/{}: storing", mBenchRun + 1, c.runs));
			}
			break;
		case BenchState::Sequence:
		{
			const int t = mBenchT++;
			const int shift = mBenchRun * c.step;
			// cycles: attack spam, then crouch spam from the frame the attack lands (both pressed on even ticks only)
			const int cs = c.firstCrouch + shift;
			const int nCycles = std::max(1, c.cycles);
			bool crouchDown = false, attackDown = false;
			for (int k = 0; k < nCycles; ++k)
			{
				const int c0 = cs + k * c.cycleLength;
				if (c.attackTicks > 0 && t >= c0 && t < c0 + c.attackTicks && (t - c0) % 2 == 0) attackDown = true;
				const int cc = c0 + c.crouchDelay;
				if (t >= cc && t < cc + c.crouchTicks && (t - cc) % 2 == 0) crouchDown = true;
			}
			const int seqEnd = cs + (nCycles - 1) * c.cycleLength + std::max(c.attackTicks, c.crouchDelay + c.crouchTicks);
			const bool crouching = t < seqEnd;
			benchKey(0, c.crouchKey, crouchDown);
			benchKey(1, c.meleeKey, attackDown);
			// once stored for releaseDelay ticks (and the spam is over), tap the release key
			mBenchPinnedTicks = mPinned ? mBenchPinnedTicks + 1 : 0;
			if (mPinned && c.speedStart > 0.f && mBenchReleaseLeft == 0) benchSetStoredSpeed(c.speedStart + mBenchRun * c.speedStep);
			if (mBenchReleaseLeft > 0)
			{
				if (--mBenchReleaseLeft == 0) { benchKey(2, c.releaseKey, false); mBench = BenchState::Flight; mBenchWait = 0; mBenchCaptureSeen = false; benchSetStatus(std::format("run {}/{}: flying", mBenchRun + 1, c.runs)); }
			}
			else if (!crouching && mBenchPinnedTicks >= c.releaseDelay)
			{
				benchKey(0, c.crouchKey, false);
				benchKey(1, c.meleeKey, false);
				benchKey(2, c.releaseKey, true);
				mBenchReleaseLeft = std::max(1, c.holdTicks);
			}
			else if (t > seqEnd + 90)
			{
				// never stored: count it and go again
				benchSetStatus(std::format("run {}/{}: no storage - next", mBenchRun + 1, c.runs));
				if (++mBenchRun >= c.runs) { benchReleaseAll(c); mBenchWanted = false; mBench = BenchState::Idle; mBenchFinished = true; benchSetStatus("done"); }
				else revert();
				mBenchRunShown = mBenchRun;
			}
			break;
		}
		case BenchState::Flight:
		{
			// the launch is logged when its capture ends (rest, death, length cap); then the next run. No capture
			// within a second means the tap did not launch - go again.
			++mBenchWait;
			if (mCapture) mBenchCaptureSeen = true;
			const bool over = (mBenchCaptureSeen && !mCapture) || (!mBenchCaptureSeen && mBenchWait > 30)
				|| mBenchWait > (int)(c.flightTimeout * 30.f);
			if (over)
			{
				if (++mBenchRun >= c.runs) { benchReleaseAll(c); mBenchWanted = false; mBench = BenchState::Idle; mBenchFinished = true; benchSetStatus("done"); }
				else revert();
				mBenchRunShown = mBenchRun;
			}
			break;
		}
		}
	}

	void onSimTick(uint32_t tick)
	{
		++mSimInFlight;
		struct Done { std::atomic_int& c; ~Done() { --c; } } done{ mSimInFlight };
		if (!mEnabled.load() || !isActiveGame() || !engineReady()) return;
		benchTick(tick);
		if (mIsReach)
		{
			readReachTls();
			// Reach collision: walked here, on the game thread (the structure mesh is decoded through the engine's own
			// getChildShape, which needs this thread's Havok TLS)
			if (mReachWanted.load()) ReachCollision::tick(moduleBase(), mReachObjectsWanted.load());
			if (mUseGameRays.load()) runGameSims();
		}

		Sample s;
		s.tick = tick;
		const bool ok = mIsReach ? readSubjectReach(s) : readSubject(s);
		s.dt = mIsReach ? mReachDt.load() : readSecondsPerTick();
		if (auto va = mViewAngleWeak.lock())
		{
			try { s.yaw = va->getPlayerViewAngle().x; }
			catch (...) {}
		}
		s.valid = ok;

		const float minSpeed = mMinSpeedForSim.load();
		const size_t maxTrace = (size_t)std::clamp((int)std::ceil(mDurationForSim.load() / s.dt), 2, kMaxSteps);

		std::scoped_lock lk(mStateMutex);
		const Sample prev = mLatest;
		if (ok && !mIsReach)
		{
			mObjSnaps.push_back(snapObjects(s.pos, tick));
			while (mObjSnaps.size() > 6) mObjSnaps.pop_front();
			// highest physics-shape surface straight under the body
			float top = -1e9f;
			for (const h2cv::Tri& t : mObjSnaps.back().tris)
			{
				const V3 a = v3(t.a), b = v3(t.b), c = v3(t.c);
				const V3 e0 = b - a, e1 = c - a;
				const float d = e0.x * e1.y - e1.x * e0.y;
				if (std::fabs(d) < 1e-9f) continue;
				const float px = s.pos.x - a.x, py = s.pos.y - a.y;
				const float u = (px * e1.y - e1.x * py) / d, v = (e0.x * py - px * e0.y) / d;
				if (u < -1e-5f || v < -1e-5f || u + v > 1.f + 1e-5f) continue;
				const float z = a.z + u * e0.z + v * e1.z;
				if (z <= s.pos.z + 0.5f) top = std::max(top, z);
			}
			if (top > -1e8f) s.objGap = (s.pos.z + (s.hasExtents ? s.offLo.z : -0.3625f)) - top;
		}

		if (!ok)
		{
			finishCapture("subject lost (death, cutscene or no Havok body)");
			mLatest = Sample{};
			mPinned = false; mPinCandidate = 0;
			mRecent.clear();
			return;
		}

		// Momentum storage, as seen in the launch logs: the body is PINNED - its horizontal position does not change at all
		// (it only bobs ~0.11 wu vertically every other tick) - while its velocity is several wu/s and keeps changing
		// (growing, then dropping back, over and over). So storage is tracked by position, not by speed: once pinned,
		// a dip in speed does not end it; only the body actually leaving the spot is a launch.
		const bool consecutive = prev.valid && prev.vehicle == s.vehicle && s.tick == prev.tick + 1;
		bool launched = false;
		if (!prev.valid || s.tick <= prev.tick || s.tick > prev.tick + 5 || prev.vehicle != s.vehicle)
		{
			// revert, load, teleport-by-checkpoint or getting in / out of a vehicle
			finishCapture("tick discontinuity (revert / load / vehicle change)");
			mTrace.clear();
			mRecent.clear();
			mPinned = false; mPinCandidate = 0;
		}
		else if (consecutive)
		{
			const float speed = s.vel.Length();
			const float moved = (s.pos - prev.pos).Length();
			const float expected = speed * s.dt;
			if (moved > std::max(5.f, 4.f * expected + 1.f))
			{
				finishCapture("teleported");
				mTrace.clear();
				mRecent.clear();
				mPinned = false; mPinCandidate = 0;
			}
			else if (!mPinned)
			{
				mPinCandidate = (speed >= minSpeed && moved < 0.5f * expected) ? mPinCandidate + 1 : 0;
				if (mPinCandidate >= 2)
				{
					mPinned = true;
					mStoreSnaps.assign(mObjSnaps.begin(), mObjSnaps.end());   // the hit and the first stored ticks
					mAnchor = s.pos;
					mLastStored = s;
					mLastStored.stored = true;
					mSlowTicks = 0;
					mHeldValid = false; // a new storage replaces the previous launch's held line
				}
				else if (mLogForSim.load() && mLogSpeedUpsForSim.load() && !mCapture
					&& prev.vel.Length() < minSpeed && speed >= minSpeed
					&& (s.pos - prev.pos - s.vel * s.dt).Length() < 0.5f * expected)   // really travelling along its velocity (not the storage bob)
				{
					// a launch with no storage phase (explosion, vehicle hit, ...): predict from the first fast tick
					startCapture("speed-up", s);
				}
			}
			else
			{
				const V3 off = s.pos - mAnchor;
				const bool stillPinned = std::sqrt(off.x * off.x + off.y * off.y) < 0.05f && std::fabs(off.z) < 0.3f;
				if (stillPinned)
				{
					mLastStored = s;
					mLastStored.stored = true;
					mSlowTicks = speed < 0.5f ? mSlowTicks + 1 : 0;
					if (mSlowTicks >= 15) { mPinned = false; mPinCandidate = 0; } // storage fizzled out without a launch
				}
				else
				{
					mPinned = false;
					mPinCandidate = 0;
					launched = mLastStored.valid;
				}
			}
		}

		// A hit (sword, melee, rocket, grenade ...): a velocity change gravity / friction / contacts cannot explain.
		//   while pinned in storage : velocity drifts ~0.3 wu/s per tick and sometimes drops to ~40% in the same direction;
		//                             a change > 4 wu/s that also TURNS the velocity is a hit, even if it takes speed away
		//   on the release tick     : a forward-tap release only kicks upward (+1.4 to +8.4) plus ~0.3 horizontally; a
		//                             horizontal change > 4, or any change > 9, means the release was a hit (rocket
		//                             releases logged at 12.1 / 13.1 / 15.0, mostly upward)
		//   in free flight          : contacts only take speed away, so it must ADD > 1 wu/s of speed as well
		if (consecutive)
		{
			V3 dv = s.vel - prev.vel;
			dv.z += gravityNow() * s.dt;
			const float hdv = std::sqrt(dv.x * dv.x + dv.y * dv.y);
			bool isHit = false;
			// Reach crouch storage: the stored speed points down into the floor and grows with every crouch, and its
			// release turns it along the slope - neither is a hit
			auto downward = [](const V3& v) { const float sp = v.Length(); return sp > 0.5f && v.z < -0.7f * sp; };
			const bool reachCrouch = mIsReach && (downward(prev.vel) || (prev.stored && prev.vel.Length() < 1.f));
			if (reachCrouch) isHit = false;
			else if (launched) isHit = hdv > 4.f || dv.Length() > 9.f;   // forward-tap kicks are <= ~8.4; rocket releases were 12-15
			else if (prev.stored)
			{
				// storage itself periodically drops the velocity back to ~40% WITHOUT turning it (seen in the first logs):
				// that is not a hit, so a pinned change only counts if it also turns the velocity
				const float s0 = prev.vel.Length(), s1 = s.vel.Length();
				const float cosTurn = (s0 > 0.5f && s1 > 0.5f) ? prev.vel.Dot(s.vel) / (s0 * s1) : 0.f;
				isHit = dv.Length() > 4.f && cosTurn < 0.98f;
			}
			else isHit = dv.Length() > 4.f && s.vel.Length() > prev.vel.Length() + 1.f;
			if (isHit)
			{
				mLastHit.valid = true;
				mLastHit.dv = dv;
				mLastHit.fromStoredRelease = launched;
				if (launched)
				{
					// the release tick applies no gravity (see the release model), and replaces a downward stored vz
					mLastHit.dv = V3(s.vel.x - prev.vel.x, s.vel.y - prev.vel.y, s.vel.z - std::max(prev.vel.z, 0.f));
				}
				mLastHit.whenMs = GetTickCount64();
				mLastHit.hasYaw = false;
				if (auto va = mViewAngleWeak.lock())
				{
					try { mLastHit.yaw = va->getPlayerViewAngle().x; mLastHit.hasYaw = std::isfinite(mLastHit.yaw); }
					catch (...) {}
				}
				mPendingMessages.push_back(std::format("Launch Trajectory: hit recorded ({:.1f} wu/s velocity change{}) - one-more-hit line updated",
					dv.Length(), prev.stored ? ", during storage" : ""));
			}
		}

		if (launched)
		{
			// launch: keep the prediction made from the last pinned tick, and start the actual-path trace from there
			mHeld = mLastStored;
			mHeldValid = true;
			mHeldSinceMs = GetTickCount64();
			mTrace.clear();
			mTrace.push_back(mLastStored.pos);
			if (mLogForSim.load()) startCapture("stored-release", mLastStored);
		}

		s.stored = mPinned;
		mLatest = s;
		mTrace.push_back(s.pos);
		while (mTrace.size() > maxTrace) mTrace.pop_front();

		if (mCapture && s.tick > mCapture->start.tick)
		{
			mCapture->actual.push_back(s);
			if (mCapture->actual.size() == 1 && mCapture->trigger == "stored-release" && mLogForSim.load()) rememberRelease(mCapture->start, s);
			mCapture->restTicks = s.vel.Length() < 0.05f ? mCapture->restTicks + 1 : 0;
			if (mCapture->restTicks >= 15) finishCapture("at rest");
			else if (mCapture->actual.size() >= maxTrace) finishCapture("reached Max Prediction Length");
		}
		mRecent.push_back(s);
		while (mRecent.size() > 30) mRecent.pop_front();
	}

	// ---- launch capture (simulation thread, mStateMutex held) ----

	void startCapture(const char* trigger, const Sample& start)
	{
		finishCapture("next launch started");
		LaunchCapture c;
		c.trigger = trigger;
		c.start = start;
		for (const Sample& r : mRecent) if (r.tick < start.tick) c.pre.push_back(r);
		mPendingMessages.push_back(std::format("Launch Trajectory: recording launch ({}, {:.1f} wu/s)...", trigger, start.vel.Length()));
		if (mBenchWanted.load() && mBench != BenchState::Idle)
		{
			BenchConfig bc; { std::scoped_lock lk(mBenchMutex); bc = mBenchCfg; }
			const int shift = mBenchRun * bc.step;
			c.benchInfo = std::format("run,{},of,{},start_tick,{},crouch_ticks,{},attack_cycles,{},release_after_stored_ticks,{},step,{},set_stored_speed,{:.3f}",
				mBenchRun + 1, bc.runs, bc.firstCrouch + shift, bc.crouchTicks, bc.attackTicks > 0 ? bc.cycles : 0, bc.releaseDelay, bc.step,
				bc.speedStart > 0.f ? bc.speedStart + mBenchRun * bc.speedStep : 0.f);
		}
		if (!mIsReach && std::string(trigger) == "stored-release")
		{
			c.storeSnaps = mStoreSnaps;
			for (const ObjSnap& sn : mObjSnaps) c.storeSnaps.push_back(sn);   // and the last ticks up to the release
		}
		if (!mIsReach && std::string(trigger) == "stored-release")
		{
			c.overTopFace = overTopFace(start);
			if (const auto dv = recallRelease(start)) { c.hasMemDv = true; c.memDv = *dv; }
		}
		c.reach = mIsReach;
		if (mIsReach && std::string(trigger) == "stored-release")
			if (const auto rr = recallReachRelease(start)) { c.hasMemDv = true; c.memDv = rr->v1 - start.vel; c.memDp = rr->dp; }
		if (mIsReach && mem::okp(start.objBase))
		{
			c.charRaw.resize(64);
			if (!mem::copy(c.charRaw.data(), start.objBase + 0xA6C, 64 * sizeof(float))) c.charRaw.clear();
		}
		mCapture = std::move(c);
	}

	void finishCapture(const char* why)
	{
		if (!mCapture) return;
		mCapture->endReason = why;
		if (!mCapture->actual.empty())
		{
			mFinished.push_back(std::move(*mCapture));
			while (mFinished.size() > 4) mFinished.pop_front();
		}
		else mPendingMessages.push_back(std::format("Launch Trajectory: launch recording dropped ({}) before any tick was recorded", why));
		mCapture.reset();
	}

	// ---------------------------------------------------------------- render thread

	static float readGravity()
	{
		const uintptr_t pc = mem::q(h2::base() + kPhysicsConstantsRva);
		const float g = mem::okp(pc) ? mem::f32(pc, std::numeric_limits<float>::quiet_NaN()) : std::numeric_limits<float>::quiet_NaN();
		return (std::isfinite(g) && std::fabs(g) < 1000.f) ? g : kDefaultGravity;
	}

	void updateWorldGrid(bool includeBarriers, bool includeObjects)
	{
		h2cv::setGame(h2cv::Game::Halo2); // process-global selector; the H3 collision viewer may have left it elsewhere
		mWorld.update(includeObjects, false);
		if (!mWorld.valid())
		{
			mWorldGrid.clear();
			mBuiltSbsp = 0;
			return;
		}
		if (mWorld.sbspId() != mBuiltSbsp || includeBarriers != mBuiltWithBarriers || mWorldGrid.empty())
		{
			mWorldGrid.build(&mWorld.staticOccluders(), includeBarriers ? &mWorld.barrierFills() : nullptr, kGridCell);
			mBuiltSbsp = mWorld.sbspId();
			mBuiltWithBarriers = includeBarriers;
			// release memory level key: from the level geometry itself (stable between sessions, unlike the bsp address)
			{
				uint64_t h = 1469598103934665603ull ^ (uint64_t)mWorld.staticOccluders().size();
				const auto& tris = mWorld.staticOccluders();
				for (size_t i = 0; i < tris.size() && i < 64; ++i)
					for (const float f : { tris[i].a.x, tris[i].a.y, tris[i].a.z, tris[i].c.x, tris[i].c.y, tris[i].c.z })
						h = (h ^ (uint64_t)(int64_t)std::lround(f * 100.f)) * 1099511628211ull;
				mLevelKey = h ? h : 1;
			}
			PLOG_INFO << "[LaunchTrajectory] collision grid built: " << mWorldGrid.size() << " triangles";
		}
	}

	// Objects move, so only the ones near this frame's paths are bucketed: the free-flight arc plus the path against the
	// world alone (which is where the slide after the first contact goes).
	size_t mPhysicsShapeTris = 0;   // Halo 2: triangles from movable objects' Havok shapes in the object grid
	struct ObjDiag { int type = 0; V3 pos; int bodies = 0; int shapes = 0; std::string desc; };
	std::mutex mObjDiagMutex;
	std::vector<ObjDiag> mObjDiag;   // Halo 2: objects near the predictions, for the launch logs
	void updateObjectGrid(const std::vector<const Sample*>& subjects, const SimParams& p, const TriGrid* worldGrid)
	{
		constexpr float big = std::numeric_limits<float>::max();
		V3 mn(big, big, big), mx(-big, -big, -big);
		SimParams free = p;
		free.collide = false;
		bool any = false;
		for (const Sample* s : subjects)
		{
			const SimResult r = simulate(*s, free, nullptr, nullptr);
			for (const V3& pt : r.pts) { growBox(mn, mx, pt); any = true; }
			if (worldGrid)
			{
				const SimResult rw = simulate(*s, p, worldGrid, nullptr);
				for (const V3& pt : rw.pts) growBox(mn, mx, pt);
			}
		}
		if (!any) { mObjectGrid.clear(); return; }
		const V3 pad(5.f, 5.f, 5.f);
		mn -= pad; mx += pad;

		std::vector<h2cv::Tri> nearTris;
		for (const h2cv::Tri& t : mWorld.objectOccluders())
		{
			const V3 a = v3(t.a), b = v3(t.b), c = v3(t.c);
			const V3 tmn = V3::Min(a, V3::Min(b, c)), tmx = V3::Max(a, V3::Max(b, c));
			if (tmx.x < mn.x || tmn.x > mx.x || tmx.y < mn.y || tmn.y > mx.y || tmx.z < mn.z || tmn.z > mx.z) continue;
			nearTris.push_back(t);
		}
		appendPhysicsShapes(nearTris, mn, mx);
		mObjectGrid.buildFrom(std::move(nearTris), kGridCell);
	}

	// Halo 2: the Havok shapes of movable objects (crates, vehicles, machines). The player's body collides with these
	// physics shapes, not with the objects' collision models above - e.g. bowl_tall's physics model is a 0.33 x 0.33 x
	// 0.87 box and civilian_cov_crate's an octagonal prism, and the sword / rocket storage spots sit on them. Read live
	// from each object's Havok bodies (H2CV's shape walker: boxes, convex hulls, capsules, spheres), at their current pose.
	// one Havok primitive (body-local) -> world triangles at `pose`: polytopes face by face, pushed out by the convex radius;
	// capsules / spheres as an octagonal prism with pointed caps. Implausibly large primitives (> 12 wu on an axis) are
	// skipped: a 17 x 17 x 18 wu hull showed up in the logs that the player flew straight through (a 6 wu limit also
	// threw away a real 3.5 x 3.5 x 6 wu object that sword launches hit at (-157, -16.5, 26.8)).
	static size_t primToTris(const h2::ShapePrim& pr, const h2::Xf& pose, std::vector<h2cv::Tri>& out)
	{
		auto W = [&](const h2::Vec3& v) { return h2::xfPoint(pose, v); };
		{
			float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
			auto grow = [&](const h2::Vec3& v) { lo[0] = std::min(lo[0], v.x); lo[1] = std::min(lo[1], v.y); lo[2] = std::min(lo[2], v.z); hi[0] = std::max(hi[0], v.x); hi[1] = std::max(hi[1], v.y); hi[2] = std::max(hi[2], v.z); };
			if (pr.capsule) { grow(pr.a); grow(pr.b); } else for (const auto& v : pr.verts) grow(v);
			if (hi[0] - lo[0] > 12.f || hi[1] - lo[1] > 12.f || hi[2] - lo[2] > 12.f) return 0;
		}
		size_t added = 0;
		if (!pr.capsule && !pr.faces.empty())
		{
			for (const h2::ShapeFace& f : pr.faces)
			{
				if (f.ring.size() < 3) continue;
				bool okIdx = true;
				for (uint16_t i : f.ring) if (i >= pr.verts.size()) okIdx = false;
				if (!okIdx) continue;
				const h2::Vec3 off{ f.n.x * pr.r, f.n.y * pr.r, f.n.z * pr.r };
				auto P = [&](uint16_t i) { const h2::Vec3& v = pr.verts[i]; return W(h2::Vec3{ v.x + off.x, v.y + off.y, v.z + off.z }); };
				for (size_t k = 1; k + 1 < f.ring.size(); ++k) { out.push_back({ P(f.ring[0]), P(f.ring[k]), P(f.ring[k + 1]) }); ++added; }
			}
		}
		else
		{
			const V3 a(pr.a.x, pr.a.y, pr.a.z), b(pr.b.x, pr.b.y, pr.b.z);
			V3 ax = b - a; const float L = ax.Length();
			ax = L > 1e-5f ? ax * (1.f / L) : V3(0, 0, 1);
			V3 u = std::fabs(ax.z) < 0.9f ? ax.Cross(V3(0, 0, 1)) : ax.Cross(V3(1, 0, 0));
			{ const float ul = u.Length(); u = ul > 1e-6f ? u * (1.f / ul) : V3(1, 0, 0); }
			const V3 w = ax.Cross(u);
			const float r = pr.r;
			auto T = [&](const V3& v) { return W(h2::Vec3{ v.x, v.y, v.z }); };
			const V3 tipA = a - ax * r, tipB = b + ax * r;
			for (int k = 0; k < 8; ++k)
			{
				const float t0 = k * 0.785398f, t1 = (k + 1) * 0.785398f;
				const V3 d0 = u * std::cos(t0) + w * std::sin(t0), d1 = u * std::cos(t1) + w * std::sin(t1);
				const V3 a0 = a + d0 * r, a1 = a + d1 * r, b0 = b + d0 * r, b1 = b + d1 * r;
				out.push_back({ T(a0), T(b0), T(b1) }); out.push_back({ T(a0), T(b1), T(a1) });
				out.push_back({ T(tipA), T(a1), T(a0) }); out.push_back({ T(tipB), T(b0), T(b1) });
				added += 4;
			}
		}
		return added;
	}

	std::deque<ObjSnap> mObjSnaps;        // last few ticks (sim thread)
	std::vector<ObjSnap> mStoreSnaps;     // copied when a storage starts: the ticks of the hit and the first stored tick

	ObjSnap snapObjects(const V3& c, uint32_t tick)
	{
		ObjSnap sn; sn.tick = tick;
		try
		{
			std::vector<h2::ObjectRef> objs;
			h2::enumerateObjects(objs);
			const uintptr_t me = h2::localPlayerUnitObject();
			const uintptr_t myVehicle = me ? h2::vehicleOfUnit(me) : 0;
			for (const auto& o : objs)
			{
				if (o.type != h2::Crate && o.type != h2::Vehicle && o.type != h2::Machine) continue;
				if ((o.hdrFlags & 0x10) || o.obj == me || o.obj == myVehicle) continue;
				float loc[3] = {};
				if (!mem::copy(loc, o.obj + 0x64, sizeof(loc))) continue;
				const V3 opos(loc[0], loc[1], loc[2]);
				if (!(std::fabs(opos.x - c.x) < 4.f && std::fabs(opos.y - c.y) < 4.f && std::fabs(opos.z - c.z) < 4.f)) continue;
				uintptr_t bodies[8];
				const int nb = h2::havokBodies(o.obj, bodies);
				for (int bi = 0; bi < nb; ++bi)
				{
					h2::Xf pose;
					if (!h2::entityPose(bodies[bi], pose)) continue;
					h2::BodyShape bs;
					const bool shaped = h2::readBodyShape(bodies[bi], 0, bs);
					h2::Motion m;
					const bool moving = h2::readMotion(bodies[bi], m, true);
					sn.info.push_back(std::format("type,{},pos,{:.4f},{:.4f},{:.4f},body,{},origin,{:.4f},{:.4f},{:.4f},vel,{:.4f},{:.4f},{:.4f},shape,{}",
						(int)o.type, opos.x, opos.y, opos.z, bi, pose.t.x, pose.t.y, pose.t.z,
						moving ? m.v.x : 0.f, moving ? m.v.y : 0.f, moving ? m.v.z : 0.f, shaped ? bs.desc : std::string("none")));
					if (shaped) for (const h2::ShapePrim& pr : bs.prims) primToTris(pr, pose, sn.tris);
				}
			}
		}
		catch (...) {}
		return sn;
	}

	void appendPhysicsShapes(std::vector<h2cv::Tri>& out, const V3& mn, const V3& mx)
	{
		try
		{
			std::vector<h2::ObjectRef> objs;
			h2::enumerateObjects(objs);
			const uintptr_t me = h2::localPlayerUnitObject();
			const uintptr_t myVehicle = me ? h2::vehicleOfUnit(me) : 0;
			size_t added = 0;
			std::vector<ObjDiag> diag;
			for (const auto& o : objs)
			{
				if (o.type == h2::Biped || o.type == h2::Weapon || o.type == h2::Equipment) continue;
				if (o.obj == me || o.obj == myVehicle) continue;
				float loc[3] = {};
				mem::copy(loc, o.obj + 0x64, sizeof(loc));   // object root position
				uintptr_t bodies[8];
				const int nb = (o.hdrFlags & 0x10) ? 0 : h2::havokBodies(o.obj, bodies);
				const V3 opos(loc[0], loc[1], loc[2]);
				ObjDiag dg; dg.type = o.type; dg.pos = opos; dg.bodies = nb;
				const bool wanted = (o.type == h2::Crate || o.type == h2::Vehicle || o.type == h2::Machine) && !(o.hdrFlags & 0x10);
				ObjDiag* dgp = nullptr;
				if (std::isfinite(opos.x) && opos.x > mn.x - 10.f && opos.x < mx.x + 10.f && opos.y > mn.y - 10.f && opos.y < mx.y + 10.f && opos.z > mn.z - 10.f && opos.z < mx.z + 10.f)
				{
					diag.push_back(dg);
					dgp = &diag.back();
				}
				if (!wanted) continue;
				for (int bi = 0; bi < nb; ++bi)
				{
					h2::Xf pose;
					if (!h2::entityPose(bodies[bi], pose)) continue;
					const h2::Vec3 org = h2::xfPoint(pose, h2::Vec3{ 0, 0, 0 });
					// quick reject: body origin far outside the area the predictions cover
					if (org.x < mn.x - 10.f || org.x > mx.x + 10.f || org.y < mn.y - 10.f || org.y > mx.y + 10.f || org.z < mn.z - 10.f || org.z > mx.z + 10.f) continue;
					h2::BodyShape bs;
					if (!h2::readBodyShape(bodies[bi], 0, bs)) { if (dgp) dgp->desc += " [no shape]"; continue; }
					if (dgp) { dgp->desc += " [" + bs.desc + "]"; dgp->shapes += (int)bs.prims.size(); }
					for (const h2::ShapePrim& pr : bs.prims)
					{
						if (out.size() > 200000) return;
						added += primToTris(pr, pose, out);
					}
				}
			}
			mPhysicsShapeTris = added;
			{ std::scoped_lock lk(mObjDiagMutex); mObjDiag = std::move(diag); }
		}
		catch (...) {}
	}

	static void drawPath(IRenderer3D* renderer, const SimResult& r, const V4& colour, bool tickMarks, float dt)
	{
		for (size_t i = 1; i < r.pts.size(); ++i)
			renderer->drawEdge(r.pts[i - 1], r.pts[i], colour);

		if (tickMarks && dt > 0.f)
		{
			const int every = std::max(1, (int)std::lround(1.f / dt)); // one marker per second of flight
			for (size_t i = every; i < r.pts.size(); i += every)
				renderer->renderSphere(r.pts[i], colour, 0.06f, true);
		}
	}

	static void drawTrace(IRenderer3D* renderer, const std::vector<V3>& trace, const V4& colour)
	{
		for (size_t i = 1; i < trace.size(); ++i)
			renderer->drawEdge(trace[i - 1], trace[i], colour);
	}

	static void drawLabel(IRenderer3D* renderer, const V3& world, const std::string& text, const V4& colour, float scale, float yOffset = 0.f)
	{
		if (renderer->pointBehindCamera(world)) return;
		const V3 sp = renderer->worldPointToScreenPosition(world);
		RenderTextHelper::drawOutlinedText(text, { sp.x + 10.f, sp.y + yOffset }, ImGui::ColorConvertFloat4ToU32(colour), scale);
	}

	static std::string describe(const SimResult& r)
	{
		const char* how = r.died ? "FALL DEATH" : r.wedged ? "wedged" : r.atRest ? "at rest" : "still moving (raise Max Prediction Length)";
		return std::format("{} after {:.2f} s  |  {:.1f} wu out  |  apex {:+.1f}  |  height {:+.1f}  |  {} contact{}",
			how, r.flightTime, r.horizontal, r.apex, r.drop, r.contacts, r.contacts == 1 ? "" : "s");
	}

	// ---- launch log ----------------------------------------------------------------------------------------------
	// One CSV per launch in <HCM folder>\LaunchLogs: the prediction the overlay makes from the launch state, next to what
	// the game actually did, tick by tick, plus the model constants and quick fits measured from the actual path. This
	// is the raw data for refining the model.
	static float median(std::vector<float> v)
	{
		if (v.empty()) return std::numeric_limits<float>::quiet_NaN();
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		return v[v.size() / 2];
	}

	static std::string f6(float f) { return std::isfinite(f) ? std::format("{:.6f}", f) : std::string("nan"); }

	static SimParams logParams(const LaunchCapture& c, const SimParams& p)
	{
		const float dt = (c.start.dt > 0.001f && c.start.dt < 0.2f) ? c.start.dt : (1.f / 30.f);
		SimParams lp = p;
		lp.duration = std::max(p.duration, (c.actual.size() + 2) * dt);
		if (c.hasMemDv && !c.reach) { lp.hasReleaseDv = true; lp.releaseDv = c.memDv; }
		else if (c.overTopFace) lp.releaseKick = 0.3f;
		return lp;
	}

	void writeLaunchLog(const LaunchCapture& c, const SimParams& p, bool includeBarriers, bool includeObjects, const SimResult* precomputed = nullptr)
	{
		try
		{
			const float dt = (c.start.dt > 0.001f && c.start.dt < 0.2f) ? c.start.dt : (1.f / 30.f);

			// the prediction, from the launch state, against the same collision the overlay draws with
			const TriGrid* worldGrid = nullptr;
			const TriGrid* objectGrid = nullptr;
			if (p.collide && mIsReach)
			{
				updateReachGrid(includeObjects);
				worldGrid = mReachGrid.empty() ? nullptr : &mReachGrid;
			}
			else if (p.collide)
			{
				updateWorldGrid(includeBarriers, includeObjects);
				worldGrid = mWorldGrid.empty() ? nullptr : &mWorldGrid;
				if (includeObjects)
				{
					updateObjectGrid({ &c.start }, p, worldGrid);
					objectGrid = mObjectGrid.empty() ? nullptr : &mObjectGrid;
				}
			}
			const SimParams lp = logParams(c, p);
			const SimResult r = precomputed ? *precomputed
				: (mIsReach && c.hasMemDv) ? simulateAfterRelease(c.start, ReachRecall{ c.start.vel + c.memDv, c.memDp }, lp, worldGrid, objectGrid)
				: simulate(c.start, lp, worldGrid, objectGrid);

			// actual sequence with the start state as index 0
			std::vector<Sample> act;
			act.reserve(c.actual.size() + 1);
			act.push_back(c.start);
			for (const Sample& a : c.actual) act.push_back(a);

			// ---- stick input in the air (logging only - the prediction never uses it) ----
			// A free-flight tick (gravity alone on z, moved by its own velocity) whose horizontal velocity changed by more
			// than the 1.05 wu/s^2 no-input slow-down explains (> 0.15 wu/s^2 off) had stick input. Flagged per row and left
			// out of the fits below, so input never feeds into the model.
			std::vector<uint8_t> airInput(c.actual.size() + 1, 0);
			int airInputTicks = 0;
			for (size_t k = 1; k < act.size(); ++k)
			{
				const Sample& a0 = act[k - 1];
				const Sample& a1 = act[k];
				if (a1.tick != a0.tick + 1 || k < 6) continue;   // the release ticks are modelled separately
				if (std::fabs((a1.vel.z - a0.vel.z) + p.gravity * dt) > 0.01f) continue;
				if ((a1.pos - a0.pos - a1.vel * dt).Length() > 1e-3f) continue;
				const float hs = std::sqrt(a0.vel.x * a0.vel.x + a0.vel.y * a0.vel.y);
				if (hs < 0.05f) continue;
				const float k1 = std::max(0.f, hs - p.airFriction * dt) / hs;
				const float rx = a1.vel.x - a0.vel.x * k1, ry = a1.vel.y - a0.vel.y * k1;
				if (std::sqrt(rx * rx + ry * ry) / dt > 0.15f) { airInput[k] = 1; ++airInputTicks; }
			}

			// ---- quick fits from the actual path ----
			// airborne-looking ticks: vertical acceleration within 50% of the model's gravity, horizontal velocity barely changed
			std::vector<float> gEst, dragEst, airDecelEst, semiImplicitErr, explicitErr;
			float maxActualSpeed = 0.f;
			for (size_t k = 1; k < act.size(); ++k)
			{
				const Sample& a0 = act[k - 1];
				const Sample& a1 = act[k];
				maxActualSpeed = std::max(maxActualSpeed, a1.vel.Length());
				if (a1.tick != a0.tick + 1) continue;
				const float az = (a1.vel.z - a0.vel.z) / dt;
				const V3 h0(a0.vel.x, a0.vel.y, 0), h1(a1.vel.x, a1.vel.y, 0);
				const float hAccel = (h1 - h0).Length() / dt;
				const float hSpeed0 = h0.Length();
				// contacts change velocity by far more than this; real air drag of up to 0.2/s still passes
				const bool airborne = std::fabs(az + p.gravity) < 0.5f * std::max(0.5f, std::fabs(p.gravity)) && hAccel < 3.f;   // air slow-down is ~1.05 wu/s^2; ground friction (9.6) and contacts are far above this
				if (!airborne || airInput[k]) continue;
				gEst.push_back(-az);
				if (hSpeed0 > 1.f) dragEst.push_back((hSpeed0 - h1.Length()) / (hSpeed0 * dt));
				if (hSpeed0 > 0.2f) airDecelEst.push_back((hSpeed0 - h1.Length()) / dt);
				// which velocity moved the body this tick: the new one (semi-implicit) or the old one (explicit)?
				const V3 moved = a1.pos - a0.pos;
				semiImplicitErr.push_back((moved - a1.vel * dt).Length());
				explicitErr.push_back((moved - a0.vel * dt).Length());
			}
			std::vector<V3> actPos;
			for (const Sample& a : act) actPos.push_back(a.pos);
			const auto groundDecel = measureGroundDecel(actPos, dt);

			// ---- per-tick comparison ----
			float maxErr1s = 0.f, maxErr3s = 0.f, maxErr = 0.f;
			const size_t n1s = (size_t)std::lround(1.f / dt), n3s = (size_t)std::lround(3.f / dt);
			for (size_t k = 0; k < act.size(); ++k)
			{
				const V3 pp = r.pts[std::min(k, r.pts.size() - 1)];
				const float e = (act[k].pos - pp).Length();
				maxErr = std::max(maxErr, e);
				if (k <= n1s) maxErr1s = std::max(maxErr1s, e);
				if (k <= n3s) maxErr3s = std::max(maxErr3s, e);
			}
			const V3 actualEnd = act.back().pos, predEnd = r.pts.back();
			// same tick as the last actual sample; and, separately, where each one came to rest
			const float errEnd = (actualEnd - r.pts[std::min(act.size() - 1, r.pts.size() - 1)]).Length();
			const float restGap = (actualEnd - predEnd).Length();

			// ---- write ----
			namespace fs = std::filesystem;
			fs::path dir = fs::path(mLogDir.empty() ? std::string(".") : mLogDir) / "LaunchLogs";
			std::error_code ec;
			fs::create_directories(dir, ec);
			SYSTEMTIME st; GetLocalTime(&st);
			const std::string name = std::format("LaunchLog_{:04}{:02}{:02}_{:02}{:02}{:02}_{}.csv",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, c.trigger);
			const fs::path file = dir / name;
			std::ofstream o(file, std::ios::out | std::ios::trunc);
			if (!o) throw HCMRuntimeException(std::format("Launch Trajectory: could not write {}", file.string()));

			o << (mIsReach ? "# HCM Launch Trajectory log (Halo: Reach, haloreach.dll 1.3528)\n" : "# HCM Launch Trajectory log (Halo 2, halo2.dll 1.3528)\n");
			o << "# trigger," << c.trigger << "\n";
			o << "# end_reason," << c.endReason << "\n";
			o << "# vehicle," << (c.start.vehicle ? 1 : 0) << "\n";
			o << "# start_tick," << c.start.tick << "\n";
			o << "# seconds_per_tick," << f6(dt) << "\n";
			o << "# model_gravity," << f6(p.gravity) << "\n";
			o << "# model_max_speed," << f6(p.maxSpeed) << "\n";
			o << "# model_damping," << f6(p.damping) << "\n";
			o << "# model_ground_friction," << f6(p.groundFriction) << "\n";
			o << "# model_restitution," << f6(p.restitution) << "\n";
			o << "# model_air_friction," << f6(p.airFriction) << ",wall_backstep," << (p.wallBackstep ? 1 : 0) << "\n";
			o << "# model_h2_decomp_ground,push," << f6(p.h2GroundPush) << ",gravity_while_slowing," << (int)p.h2GroundGravity << ",crease_support," << (int)p.h2CreaseSupport << "\n";
			o << "# model_ground_slide_max_speed," << f6(p.groundSlideMaxSpeed) << ",impact_speed," << f6(std::sqrt(4.f * std::max(0.f, p.gravity)))
				<< ",release_model," << (p.releaseModel ? 1 : 0) << ",release_friction," << f6(p.releaseFriction) << ",start_stored," << (c.start.stored ? 1 : 0) << "\n";
			{
				const float kickZ = c.actual.empty() ? std::numeric_limits<float>::quiet_NaN() : c.actual[0].vel.z - c.start.vel.z;
				o << "# start_floor_depth," << f6(r.startFloorDepth) << ",release_kick_z," << f6(kickZ) << ",kick_times_tick," << f6(kickZ * dt)
					<< ",predicted_kick," << f6(r.predictedKick) << "\n";
				o << "# fall_death_speed," << f6(p.fallDeathSpeed) << ",pred_died," << (r.died ? 1 : 0) << ",pred_death_time," << f6(r.died ? r.deathTime : std::numeric_limits<float>::quiet_NaN())
					<< ",actual_last_tick_time," << f6((float)c.actual.size() * dt) << ",actual_last_vz," << f6(c.actual.empty() ? 0.f : c.actual.back().vel.z) << "\n";
			}
			o << "# release_tick_velocity_change," << (c.actual.empty() ? std::string("nan") : std::format("{},{},{}", f6(c.actual[0].vel.x - c.start.vel.x), f6(c.actual[0].vel.y - c.start.vel.y), f6(c.actual[0].vel.z - c.start.vel.z))) << "\n";
			o << "# model_max_slope_deg," << f6(p.maxSlopeDeg) << "\n";
			if (!c.benchInfo.empty()) o << "# test_bench," << c.benchInfo << "\n";
			if (mIsReach)
			{
				// the game's own ground state around the release (see Sample::gamePlane)
				auto gline = [&](const char* tag, long k, const Sample& sm)
					{
						o << "# game_ground," << tag << ',' << k << ",mode," << sm.gameMode << ",plane," << f6(sm.gamePlane.x) << ',' << f6(sm.gamePlane.y) << ',' << f6(sm.gamePlane.z)
							<< ",vel," << f6(sm.vel.x) << ',' << f6(sm.vel.y) << ',' << f6(sm.vel.z) << "\n";
					};
				for (size_t k = c.pre.size() >= 4 ? c.pre.size() - 4 : 0; k < c.pre.size(); ++k) gline("pre", (long)c.pre[k].tick - (long)c.start.tick, c.pre[k]);
				gline("start", 0, c.start);
				for (size_t k = 0; k < c.actual.size() && k < 6; ++k) gline("after", (long)c.actual[k].tick - (long)c.start.tick, c.actual[k]);
				if (!c.charRaw.empty())
				{
					o << "# game_char_physics_raw (floats from biped+0xA6C)";
					for (float f : c.charRaw) o << ',' << f6(f);
					o << "\n";
				}
			}
			// Halo 2: the movable objects around you from the ticks the storage started on, and at release (the sword
			// knocks the object you store on away): positions, body velocities, shapes, and their triangles per tick
			for (const ObjSnap& sn : c.storeSnaps)
			{
				const long rel = (long)sn.tick - (long)c.start.tick;
				o << "# storage_snapshot,tick," << sn.tick << ",ticks_from_release," << rel << ",bodies," << sn.info.size() << ",triangles," << sn.tris.size() << "\n";
				for (const std::string& in : sn.info) o << "# storage_object,ticks_from_release," << rel << ',' << in << "\n";
				for (const h2cv::Tri& t : sn.tris)
					o << "# stri," << rel << ',' << f6(t.a.x) << ',' << f6(t.a.y) << ',' << f6(t.a.z) << ',' << f6(t.b.x) << ',' << f6(t.b.y) << ',' << f6(t.b.z)
						<< ',' << f6(t.c.x) << ',' << f6(t.c.y) << ',' << f6(t.c.z) << "\n";
			}
			// Halo 2: the rigid motion keeps a second velocity (+0xD0, box motions) next to the linear velocity (+0x50).
			// Bench runs that overwrite BOTH with the same vector release with no kick at all, natural storages kick -
			// so the difference between the two is the prime suspect for the kick
			if (c.trigger == "stored-release")
				o << "# release_prediction," << (c.hasMemDv ? "remembered release" : (c.overTopFace ? "kick 0.3 (top face of a movable object under you)" : "kick setting")) << "\n";
			if (!mIsReach)
				o << "# start_object_gap," << (std::isfinite(c.start.objGap) ? f6(c.start.objGap) : std::string("none"))
				<< " (body bottom above the top of a movable object straight under it at release, none = no top face under the body)\n";
			if (c.start.hasVint)
			{
				const V3 d = c.start.vint - c.start.vel;
				o << "# start_vint," << f6(c.start.vint.x) << ',' << f6(c.start.vint.y) << ',' << f6(c.start.vint.z)
					<< ",vint_minus_v," << f6(d.x) << ',' << f6(d.y) << ',' << f6(d.z) << "\n";
				for (size_t k = c.pre.size() >= 4 ? c.pre.size() - 4 : 0; k < c.pre.size(); ++k)
					if (c.pre[k].hasVint)
						o << "# pre_vint,tick," << c.pre[k].tick << ",v," << f6(c.pre[k].vel.x) << ',' << f6(c.pre[k].vel.y) << ',' << f6(c.pre[k].vel.z)
						<< ",vint," << f6(c.pre[k].vint.x) << ',' << f6(c.pre[k].vint.y) << ',' << f6(c.pre[k].vint.z)
						<< ",z," << f6(c.pre[k].pos.z) << "\n";
			}
			if (mIsReach) o << "# soft_ceilings_used," << (p.softTris ? p.softTris->size() : 0) << " (Acceleration type, bipeds)\n";
			if (mIsReach) o << "# " << mReachStatus << (precomputed ? "  |  prediction: the game's own collision test" : "  |  prediction: triangle copy") << "\n";
			o << "# model_collide," << (p.collide ? 1 : 0) << ",use_body_extents," << (p.useExtents ? 1 : 0)
				<< ",ignore_first_ticks," << p.ignoreFirstTicks << ",barriers," << (includeBarriers ? 1 : 0) << ",objects," << (includeObjects ? 1 : 0) << "\n";
			o << "# capsule_offset_lo," << f6(c.start.offLo.x) << "," << f6(c.start.offLo.y) << "," << f6(c.start.offLo.z)
				<< ",hi," << f6(c.start.offHi.x) << "," << f6(c.start.offHi.y) << "," << f6(c.start.offHi.z) << ",valid," << (c.start.hasExtents ? 1 : 0) << "\n";
			o << "# start_pos," << f6(c.start.pos.x) << "," << f6(c.start.pos.y) << "," << f6(c.start.pos.z) << "\n";
			o << "# start_vel," << f6(c.start.vel.x) << "," << f6(c.start.vel.y) << "," << f6(c.start.vel.z) << ",speed," << f6(c.start.vel.Length()) << "\n";
			o << "# fit_gravity_median," << f6(median(gEst)) << ",ticks," << gEst.size() << "\n";
			o << "# fit_air_damping_per_s_median," << f6(median(dragEst)) << ",ticks," << dragEst.size() << "\n";
			o << "# fit_air_horizontal_decel_median," << f6(median(airDecelEst)) << ",ticks," << airDecelEst.size() << "\n";
			o << "# fit_integration_err_semi_implicit," << f6(median(semiImplicitErr)) << ",explicit," << f6(median(explicitErr)) << "\n";
			o << "# fit_ground_decel," << (groundDecel ? f6(*groundDecel) : std::string("nan")) << "\n";
			o << "# actual_max_speed_after_start," << f6(maxActualSpeed) << "\n";
			o << "# pred_rest," << (r.atRest ? 1 : 0) << ",wedged," << (r.wedged ? 1 : 0) << ",contacts," << r.contacts
				<< ",first_contact_time," << f6(r.hit ? r.firstContactTime : std::numeric_limits<float>::quiet_NaN()) << ",time," << f6(r.flightTime) << "\n";
			// the geometry along the actual and the predicted path, so the whole launch can be replayed offline (both games:
			// for Halo 2 it also lets the storage spot be checked - how deep the body's capsule sits in the floor - against
			// the release kick)
			if (worldGrid)
			{
				std::vector<uint32_t> idx; std::unordered_set<uint32_t> seen;
				worldGrid->nearTriangles(c.start.pos, 2.5f, idx, seen, 30000);
				for (const Sample& a : c.actual) worldGrid->nearTriangles(a.pos, 2.0f, idx, seen, 30000);
				for (size_t k = 0; k < r.pts.size(); k += 2) worldGrid->nearTriangles(r.pts[k], 1.5f, idx, seen, 30000);
				o << "# near_triangles," << idx.size() << " (within 2 wu of the actual path and 1.5 of the prediction: ax,ay,az,bx,by,bz,cx,cy,cz)\n";
				for (uint32_t i : idx)
				{
					const h2cv::Tri& t = worldGrid->tri(i);
					o << "# tri," << f6(t.a.x) << ',' << f6(t.a.y) << ',' << f6(t.a.z) << ',' << f6(t.b.x) << ',' << f6(t.b.y) << ',' << f6(t.b.z)
						<< ',' << f6(t.c.x) << ',' << f6(t.c.y) << ',' << f6(t.c.z) << "\n";
				}
			}
			// objects (crates, scenery, the thing the Halo 2 sword storage is pinned on) near the storage spot and the path
			if (objectGrid)
			{
				std::vector<uint32_t> idx; std::unordered_set<uint32_t> seen;
				objectGrid->nearTriangles(c.start.pos, 3.0f, idx, seen, 20000);
				for (const Sample& a : c.actual) objectGrid->nearTriangles(a.pos, 2.0f, idx, seen, 20000);
				{
					std::scoped_lock lk(mObjDiagMutex);
					for (const ObjDiag& dg : mObjDiag)
						if ((dg.pos - c.start.pos).Length() < 4.f)
							o << "# object_near_start,type," << dg.type << ",pos," << f6(dg.pos.x) << ',' << f6(dg.pos.y) << ',' << f6(dg.pos.z)
							<< ",havok_bodies," << dg.bodies << ",shape_prims," << dg.shapes << "," << dg.desc << "\n";
				}
				o << "# near_object_triangles," << idx.size() << " (objects within 3 wu of the start and 2 of the actual path; Havok shapes of crates / vehicles / machines included: "
					<< mPhysicsShapeTris << " triangles)\n";
				for (uint32_t i : idx)
				{
					const h2cv::Tri& t = objectGrid->tri(i);
					o << "# otri," << f6(t.a.x) << ',' << f6(t.a.y) << ',' << f6(t.a.z) << ',' << f6(t.b.x) << ',' << f6(t.b.y) << ',' << f6(t.b.z)
						<< ',' << f6(t.c.x) << ',' << f6(t.c.y) << ',' << f6(t.c.z) << "\n";
				}
			}
			if (r.hit) o << "# first_contact_normal_used," << f6(r.firstNormal.x) << ',' << f6(r.firstNormal.y) << ',' << f6(r.firstNormal.z)
				<< ",triangle_normal," << f6(r.firstRawNormal.x) << ',' << f6(r.firstRawNormal.y) << ',' << f6(r.firstRawNormal.z) << "\n";
			o << "# pred_end," << f6(predEnd.x) << "," << f6(predEnd.y) << "," << f6(predEnd.z) << "\n";
			o << "# actual_end," << f6(actualEnd.x) << "," << f6(actualEnd.y) << "," << f6(actualEnd.z) << ",ticks," << (act.size() - 1) << "\n";
			o << "# err_max_1s," << f6(maxErr1s) << ",err_max_3s," << f6(maxErr3s) << ",err_max," << f6(maxErr) << ",err_last_tick," << f6(errEnd) << ",rest_point_gap," << f6(restGap) << (c.endReason == "at rest" && r.atRest ? "" : ",(rest gap only meaningful when both came to rest)") << "\n";

			o << "# air_input_ticks," << airInputTicks << " (flagged in the air_input column; excluded from the fits)\n";
			o << "k,tick,t,stored,act_x,act_y,act_z,act_vx,act_vy,act_vz,act_speed,pred_x,pred_y,pred_z,pred_vx,pred_vy,pred_vz,pred_speed,pred_grounded,pred_ended,err_pos,err_vel,air_input,yaw\n";
			for (const Sample& a : c.pre)
			{
				const long long k = (long long)a.tick - (long long)c.start.tick;
				o << k << ',' << a.tick << ',' << f6(k * dt) << ',' << (a.stored ? 1 : 0) << ','
					<< f6(a.pos.x) << ',' << f6(a.pos.y) << ',' << f6(a.pos.z) << ','
					<< f6(a.vel.x) << ',' << f6(a.vel.y) << ',' << f6(a.vel.z) << ',' << f6(a.vel.Length())
					<< ",,,,,,,,,,,,," << f6(a.yaw) << "\n";   // 12 empty prediction columns, then yaw
			}
			for (size_t k = 0; k < act.size(); ++k)
			{
				const Sample& a = act[k];
				const bool ended = k >= r.pts.size();
				const size_t i = std::min(k, r.pts.size() - 1);
				const V3 pp = r.pts[i];
				const V3 pv = ended ? V3::Zero : r.vels[i];
				const long long kk = (long long)a.tick - (long long)c.start.tick;
				o << kk << ',' << a.tick << ',' << f6(kk * dt) << ',' << (a.stored ? 1 : 0) << ','
					<< f6(a.pos.x) << ',' << f6(a.pos.y) << ',' << f6(a.pos.z) << ','
					<< f6(a.vel.x) << ',' << f6(a.vel.y) << ',' << f6(a.vel.z) << ',' << f6(a.vel.Length()) << ','
					<< f6(pp.x) << ',' << f6(pp.y) << ',' << f6(pp.z) << ','
					<< f6(pv.x) << ',' << f6(pv.y) << ',' << f6(pv.z) << ',' << f6(pv.Length()) << ','
					<< (int)r.groundedAt[i] << ',' << (ended ? 1 : 0) << ','
					<< f6((a.pos - pp).Length()) << ',' << f6((a.vel - pv).Length()) << ',' << (int)airInput[k] << ',' << f6(a.yaw) << '\n';
			}
			o.close();

			PLOG_INFO << "[LaunchTrajectory] wrote " << file.string();
			if (auto m = messagesWeak.lock())
				m->addMessage(std::format("Launch logged ({}, {} ticks, {}): max error {:.2f} wu in 1 s, {:.2f} wu overall, {:.2f} wu at the last tick{}. Measured g {:.3f}. -> LaunchLogs\\{}",
					c.trigger, act.size() - 1, c.endReason, maxErr1s, maxErr, errEnd,
					(c.endReason == "at rest" && r.atRest) ? std::format(", rest points {:.2f} wu apart", restGap) : std::string{},
					median(gEst), name));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		catch (const std::exception& ex)
		{
			PLOG_ERROR << "[LaunchTrajectory] log write failed: " << ex.what();
		}
	}

	void onRender(GameState game, IRenderer3D* renderer)
	{
		if (game.operator GameState::Value() != mGame) return;
		if (!mEnabled.load()) return;

		try
		{
			ScopedAtomicBool lockRender(renderingMutex);
			lockOrThrow(settingsWeak, settings);

			// the game dll is only mapped while that game is loaded: nothing to do (and nothing wrong) until it is
			const uintptr_t base = moduleBase();
			if (!base) return;
			if (base != mCheckedBase) { mCheckedBase = base; mWarnedBuild = false; }
			if (!engineReady())
			{
				if (!mWarnedBuild)
				{
					mWarnedBuild = true;
					if (auto m = messagesWeak.lock())
						m->addMessage(std::format("Launch Trajectory: {} (PE timestamp 0x{:08X}) is not the build the overlay was reverse engineered on (MCC 1.3528, 0x{:08X}) - disabled.",
							mIsReach ? "haloreach.dll" : "halo2.dll", peTimestamp(base), mIsReach ? kReachTimestamp : 0x68A0F0F2u));
				}
				return;
			}

			// logging switched on: create the folder now and say where it is
			const bool logOn = settings->launchTrajectoryLogToFile->GetValue();
			if (logOn && !mLogAnnounced)
			{
				mLogAnnounced = true;
				std::error_code ec;
				const std::filesystem::path dir = std::filesystem::path(mLogDir.empty() ? std::string(".") : mLogDir) / "LaunchLogs";
				std::filesystem::create_directories(dir, ec);
				if (auto m = messagesWeak.lock())
					m->addMessage(ec ? std::format("Launch Trajectory: could not create {} ({})", dir.string(), ec.message())
						: std::format("Launch Trajectory: logging launches to {}", std::filesystem::absolute(dir, ec).string()));
			}
			if (!logOn) mLogAnnounced = false;

			// messages queued by the simulation thread
			{
				std::vector<std::string> msgs;
				{
					std::scoped_lock lk(mStateMutex);
					msgs.swap(mPendingMessages);
				}
				if (auto m = messagesWeak.lock()) for (auto& t : msgs) m->addMessage(t);
			}

			// settings
			SimParams p;
			p.gravity = gravityNow();
			p.maxSpeed = settings->launchTrajectoryMaxSpeed->GetValue();
			p.damping = settings->launchTrajectoryDamping->GetValue();
			p.duration = settings->launchTrajectoryDuration->GetValue();
			p.collide = settings->launchTrajectoryStopAtCollision->GetValue();
			p.useExtents = settings->launchTrajectoryUseBodyExtents->GetValue();
			p.ignoreFirstTicks = settings->launchTrajectoryIgnoreFirstTicks->GetValue();
			p.groundFriction = settings->launchTrajectoryGroundDecel->GetValue();
			p.restitution = settings->launchTrajectoryRestitution->GetValue();
			p.airFriction = settings->launchTrajectoryAirFriction->GetValue();
			p.wallBackstep = settings->launchTrajectoryWallBackstep->GetValue();
			p.groundSlideMaxSpeed = settings->launchTrajectoryGroundSlideMaxSpeed->GetValue();
			p.releaseModel = settings->launchTrajectoryReleaseModel->GetValue();
			p.releaseFriction = settings->launchTrajectoryReleaseFriction->GetValue();
			p.releaseKick = settings->launchTrajectoryReleaseKick->GetValue();
			{
				// Halo 2 ground-mode details from the Xbox decompilation (see SimParams); off by default - replayed against
				// 766 Halo 2 logs they changed no prediction by more than 0.3 wu (except one), slightly more often for the worse
				const bool decomp = settings->launchTrajectoryH2DecompGround->GetValue();
				p.h2GroundPush = decomp ? 0.234f : 0.f;
				p.h2GroundGravity = decomp;
				p.h2CreaseSupport = decomp;
			}
			// with HCM invulnerability on there is no fall death, so the whole line is drawn
			p.fallDeathSpeed = (settings->launchTrajectoryFallDeath->GetValue() && !settings->invulnerabilityToggle->GetValue())
				? settings->launchTrajectoryFallDeathSpeed->GetValue() : 0.f;
			p.maxSlopeDeg = settings->launchTrajectoryMaxSlope->GetValue();
			if (mIsReach)
			{
				// Halo: Reach. Only the flight model carries over: live gravity and tick length are read from haloreach.dll.
				// Everything else above was measured on Halo 2 (air / ground friction, the release ticks and kick, the
				// 4.085 impact cut, the fall-death speed, the 100 wu/s cap) and is not assumed for Reach; collision needs
				// Reach's collision BSP, which the Collision Viewer code does not read. The launch logs record Reach
				// flights so those can be measured the same way.
				// Collision: the triangles the Havok Debugger's world gather collects for Reach (structure, plus static objects
				// when Include Objects is on). Ground: measured ~7.7 wu/s^2 slide friction at any speed (19 releases).
				p.useExtents = true;
				p.ignoreFirstTicks = 0;
				// globals.globals player information (Reach): run acceleration 8.5, airborne acceleration 1.05 - exactly
				// the slow-downs the launch logs measured on the ground and in the air
				p.groundFriction = 8.5f;
				p.groundSlideMaxSpeed = 1000.f;
				p.reachImpacts = true;
				p.airFriction = 1.05f;
				p.maxSpeed = 0.f;
				p.wallBackstep = false;
				p.releaseModel = false;
				p.releaseKick = 0.f;
				// globals damage "terminal velocity" 13 wu/s (see the fall-death check in simulate); off with invulnerability
				p.fallDeathSpeed = settings->invulnerabilityToggle->GetValue() ? 0.f : 13.f;
				p.maxSlopeDeg = 50.f;   // spartans.biped ground physics: maximum slope angle 50
			}
			const float minSpeed = settings->launchTrajectoryMinSpeed->GetValue();
			const bool onlyWhenStored = settings->launchTrajectoryOnlyWhenStored->GetValue();
			const bool keepAfterLaunch = settings->launchTrajectoryKeepAfterLaunch->GetValue();
			const bool showActual = settings->launchTrajectoryShowActualPath->GetValue();
			const bool tickMarks = settings->launchTrajectoryShowTickMarks->GetValue();
			const bool labels = settings->launchTrajectoryShowLabels->GetValue();
			const bool includeObjects = settings->launchTrajectoryIncludeObjects->GetValue();
			const bool includeBarriers = settings->launchTrajectoryIncludeBarriers->GetValue();
			const float labelScale = settings->launchTrajectoryLabelScale->GetValue();
			const V4 lineColour = settings->launchTrajectoryLineColor->GetValue();
			const V4 storedColour = settings->launchTrajectoryStoredColor->GetValue();
			const V4 heldColour = settings->launchTrajectoryHeldColor->GetValue();
			const V4 actualColour = settings->launchTrajectoryActualColor->GetValue();
			const V4 impactColour = settings->launchTrajectoryImpactColor->GetValue();
			mMinSpeedForSim = minSpeed;
			mDurationForSim = p.duration;
			// ---- Launch Test Bench: settings -> simulation thread, and the GUI-side jobs it asks for ----
			{
				const bool benchOn = settings->launchTrajectoryBenchRun->GetValue();
				if (mBenchFinished.exchange(false) && benchOn)
				{
					settings->launchTrajectoryBenchRun->GetValueDisplay() = false;
					settings->launchTrajectoryBenchRun->UpdateValueWithInput();
				}
				else if (benchOn != mBenchWantedLast)
				{
					mBenchWantedLast = benchOn;
					if (benchOn)
					{
						BenchConfig c;
						c.runs = std::max(1, settings->launchTrajectoryBenchRuns->GetValue());
						c.firstCrouch = std::max(0, settings->launchTrajectoryBenchFirstCrouch->GetValue());
						c.crouchTicks = std::max(0, settings->launchTrajectoryBenchCrouchTicks->GetValue());
						c.cycles = std::max(1, settings->launchTrajectoryBenchCycles->GetValue());
						c.cycleLength = std::max(1, settings->launchTrajectoryBenchCycleLength->GetValue());
						c.attackTicks = std::max(0, settings->launchTrajectoryBenchAttackTicks->GetValue());
						c.crouchDelay = std::max(0, settings->launchTrajectoryBenchCrouchDelay->GetValue());
						c.step = settings->launchTrajectoryBenchStep->GetValue();
						c.releaseDelay = std::max(1, settings->launchTrajectoryBenchReleaseDelay->GetValue());
						c.crouchKey = settings->launchTrajectoryBenchCrouchKey->GetValue();
						c.meleeKey = settings->launchTrajectoryBenchMeleeKey->GetValue();
						c.releaseKey = settings->launchTrajectoryBenchReleaseKey->GetValue();
						c.speedStart = std::max(0.f, settings->launchTrajectoryBenchSpeedStart->GetValue());
						c.speedStep = settings->launchTrajectoryBenchSpeedStep->GetValue();
						{ std::scoped_lock lk(mBenchMutex); mBenchCfg = c; mBenchStatus = "starting"; }
					}
					mBenchWanted = benchOn;
				}
				if (mBenchRevertRequest.exchange(false)) settings->forceRevertEvent->operator()();
				mBenchMenuOpen = settings->GUIWindowOpen->GetValue();
				if (!mIsReach) mLegacyLevelKey = (uint64_t)mWorld.sbspId();
				if (!mRelLoaded) loadReleaseMemory();
			}
			mLogForSim = settings->launchTrajectoryLogToFile->GetValue() || mBenchWanted.load();
			if (mIsReach && mGameMode.load() >= 0)
			{
				// readout of the game's own ground state (checks the decoded offsets: flat ground should read mode 1,
				// plane (0, 0, 1), 0 deg)
				const float gx = mGamePlaneX.load(), gy = mGamePlaneY.load(), gz = mGamePlaneZ.load();
				const float glen = std::sqrt(gx * gx + gy * gy + gz * gz);
				const ImVec2 ds = ImGui::GetIO().DisplaySize;
				RenderTextHelper::drawCenteredOutlinedText(std::format("Reach ground: mode {}  plane ({:.3f}, {:.3f}, {:.3f})  {:.1f} deg{}", mGameMode.load(), gx, gy, gz,
					glen > 0.5f ? std::acos(std::clamp(gz / glen, -1.f, 1.f)) * 57.29578f : 0.f, (glen < 0.98f || glen > 1.02f) ? "  (not a unit vector - offsets wrong?)" : ""),
					{ ds.x * 0.5f, ds.y * 0.95f }, 0xFFFFFFFFu, std::max(13.f, ds.y * 0.016f), 1.f);
			}
			if (mBenchWanted.load())
			{
				std::string st; { std::scoped_lock lk(mBenchMutex); st = mBenchStatus; }
				const ImVec2 ds = ImGui::GetIO().DisplaySize;
				RenderTextHelper::drawCenteredOutlinedText("LAUNCH TEST BENCH  " + st, { ds.x * 0.5f, ds.y * 0.08f },
					0xFF40E0FFu, std::max(16.f, ds.y * 0.022f), 1.f);
			}
			mUseGameRays = mIsReach && settings->launchTrajectoryReachGameCollision->GetValue();
			if (mIsReach && p.collide)
			{
				updateSoftCeilings();
				p.softTris = mSoftTris;
			}
			mGameRaysObjects = includeObjects;
			mLogSpeedUpsForSim = settings->launchTrajectoryLogSpeedUps->GetValue();

			// write any launch the simulation thread finished recording
			{
				std::optional<LaunchCapture> done;
				{
					std::scoped_lock lk(mStateMutex);
					if (!mFinished.empty()) { done = std::move(mFinished.front()); mFinished.pop_front(); }
				}
				// not a launch: the "release" was a jump or the momentum was cancelled (speed below Min Speed at the start,
				// or under 4 wu/s and a third of the stored speed within the first 3 ticks) - nothing to predict, no log
				if (done && done->trigger == "stored-release")
				{
					const float s0 = done->start.vel.Length();
					bool cancelled = s0 < mMinSpeedForSim.load();
					for (size_t k = 0; k < done->actual.size() && k < 3 && !cancelled; ++k)
					{
						const float sk = done->actual[k].vel.Length();
						cancelled = sk < 4.f && sk < s0 * 0.33f;
					}
					if (cancelled) done.reset();
				}
				if (done)
				{
					if (mIsReach && mUseGameRays.load() && p.collide && !mPendingLog)
					{
						mPendingLogParams = logParams(*done, p);
						mPendingLogBarriers = includeBarriers; mPendingLogObjects = includeObjects;
						// a remembered release: the game-collision prediction starts after the release tick (see simulateAfterRelease)
						mPendingLogId = postGameSim("log", done->hasMemDv ? afterRelease(done->start, ReachRecall{ done->start.vel + done->memDv, done->memDp }) : done->start,
							mPendingLogParams);
						mPendingLog = std::move(*done);
					}
					else writeLaunchLog(*done, p, includeBarriers, includeObjects);
				}
				if (mPendingLog)
				{
					std::optional<SimResult> ready;
					{
						std::scoped_lock lk(mGameSimMutex);
						auto it = mGameSimResults.find("log");
						if (it != mGameSimResults.end() && it->second.first >= mPendingLogId) { ready = it->second.second; mGameSimResults.erase(it); }
					}
					if (ready)
					{
						if (mPendingLog->hasMemDv)
						{
							// put the stored state back in front (same as simulateAfterRelease)
							ready->pts.insert(ready->pts.begin(), mPendingLog->start.pos);
							ready->vels.insert(ready->vels.begin(), mPendingLog->start.vel);
							if (!ready->groundedAt.empty()) ready->groundedAt.insert(ready->groundedAt.begin(), (uint8_t)0);
							const float dt0 = mPendingLog->start.dt;
							ready->flightTime += dt0;
							if (ready->hit) ready->firstContactTime += dt0;
							if (ready->died) ready->deathTime += dt0;
						}
						writeLaunchLog(*mPendingLog, mPendingLogParams, mPendingLogBarriers, mPendingLogObjects, &*ready);
						mPendingLog.reset();
					}
				}
			}

			// snapshot the simulation-thread state
			Sample latest, held;
			bool heldValid = false;
			uint64_t heldSince = 0;
			std::vector<V3> trace;
			LastHit lastHit;
			{
				std::scoped_lock lk(mStateMutex);
				lastHit = mLastHit;
				latest = mLatest;
				held = mHeld;
				heldValid = mHeldValid;
				heldSince = mHeldSinceMs;
				if (showActual) trace.assign(mTrace.begin(), mTrace.end());
			}

			const bool showLive = latest.valid && (latest.stored || (!onlyWhenStored && latest.vel.Length() >= minSpeed));

			// "one more hit": the current state plus the velocity change of the last hit, optionally turned to where you
			// are looking now (a melee / sword hit pushes along your aim)
			const bool showNextHit = settings->launchTrajectoryNextHitToggle->GetValue() && lastHit.valid && latest.valid;
			Sample nextHit = latest;
			if (showNextHit)
			{
				V3 dv = lastHit.dv;
				if (settings->launchTrajectoryNextHitRotateWithView->GetValue() && lastHit.hasYaw)
				{
					if (auto va = mViewAngleWeak.lock())
					{
						try
						{
							const float a = va->getPlayerViewAngle().x - lastHit.yaw;
							const float c = std::cos(a), sn = std::sin(a);
							dv = V3(dv.x * c - dv.y * sn, dv.x * sn + dv.y * c, dv.z);
						}
						catch (...) {}
					}
				}
				if (lastHit.fromStoredRelease && latest.stored)
				{
					nextHit.vel.x += dv.x;
					nextHit.vel.y += dv.y;
					nextHit.vel.z = std::max(nextHit.vel.z, 0.f) + dv.z;
				}
				else nextHit.vel += dv;
			}
			// a release-type hit already contains the whole release tick: no extra kick or bump on top of it
			SimParams pNext = p;
			if (lastHit.fromStoredRelease && latest.stored) { pNext.releaseKick = 0.f; pNext.releaseBump = false; }
			const uint64_t holdMs = (uint64_t)(p.duration * 1000.f) + 3000;
			const bool showHeld = keepAfterLaunch && heldValid && held.valid && (GetTickCount64() - heldSince) < holdMs;

			const TriGrid* worldGrid = nullptr;
			const TriGrid* objectGrid = nullptr;
			if (p.collide && mIsReach && (showLive || showHeld || showNextHit))
			{
				updateReachGrid(includeObjects);
				worldGrid = mReachGrid.empty() ? nullptr : &mReachGrid;
			}
			else if (p.collide && (showLive || showHeld || showNextHit))
			{
				updateWorldGrid(includeBarriers, includeObjects);
				worldGrid = mWorldGrid.empty() ? nullptr : &mWorldGrid;
				if (includeObjects)
				{
					std::vector<const Sample*> subjects;
					if (showLive) subjects.push_back(&latest);
					if (showHeld) subjects.push_back(&held);
					if (showNextHit) subjects.push_back(&nextHit);
					updateObjectGrid(subjects, p, worldGrid);
					objectGrid = mObjectGrid.empty() ? nullptr : &mObjectGrid;
				}
			}

			if (mIsReach && p.collide && labels && latest.valid)
				drawLabel(renderer, latest.pos, mUseGameRays.load()
					? std::format("Reach collision: the game's own collision test ({} rays last tick)", mGameRayCallsLastTick.load())
					: mReachStatus, (mUseGameRays.load() || worldGrid) ? impactColour : kDeathColour, labelScale * 0.8f, -labelScale * 3.f);

			// actual path (drawn first so the predictions sit on top)
			if (showActual && trace.size() > 1)
			{
				drawTrace(renderer, trace, actualColour);
				if (labels)
				{
					if (const auto decel = measureGroundDecel(trace, latest.valid ? latest.dt : (1.f / 30.f)))
						drawLabel(renderer, trace.back(), std::format("actual ground slow-down {:.1f} wu/s^2 (Ground Friction is {:.1f})", *decel, p.groundFriction), actualColour, labelScale, -labelScale * 1.5f);
				}
			}

			// Reach crouch storage: predict the release toward your aim (the stored velocity itself points into the floor)
			// (only without Reach collision: with it, the stored velocity is simply run into the slope like the game does)
			const bool reachRelease = mIsReach && settings->launchTrajectoryReachCrouchRelease->GetValue() && worldGrid == nullptr;
			const float reachRatio = settings->launchTrajectoryReachReleaseRatio->GetValue();
			float aimYaw = std::numeric_limits<float>::quiet_NaN();
			if (reachRelease)
				if (auto va = mViewAngleWeak.lock()) { try { aimYaw = va->getPlayerViewAngle().x; } catch (...) {} }
			bool liveIsReachRelease = false, liveUsedAim = false;
			float reachStoredSpeed = 0.f;
			// Reach: a remembered release for this storage replaces the release model (the velocity after the release tick)
			bool reachRemembered = false;
			std::optional<ReachRecall> liveRecall = recallReachRelease(latest), heldRecall = recallReachRelease(held);
			reachRemembered = liveRecall.has_value();
			if (!reachRemembered && reachRelease && reachDownwardStorage(latest))
			{
				reachStoredSpeed = latest.vel.Length();
				latest = reachReleaseToward(latest, reachRatio, std::isfinite(aimYaw) ? aimYaw : 0.f, &liveUsedAim);
				latest.stored = true;          // still shown in the Stored colour
				liveIsReachRelease = true;
			}
			if (!heldRecall && reachRelease && reachDownwardStorage(held))
				held = reachReleaseToward(held, reachRatio, std::isfinite(held.yaw) ? held.yaw : 0.f);

			// Halo 2 sword storage on a movable object (bowl_tall). From 138 bench releases with the object's shape recorded
			// every tick: standing squarely on its top face released with almost no kick (+0.06, 38 releases, the top
			// 0.001 wu under the body), and so did releasing after it had dropped away below you (+0.04 to +1.65, 79
			// releases); standing over its edge - the body resting on the rounded rim, no top face straight under it -
			// kicked +5.85 (21 releases), like the 3 - 9 kicks of earlier storages. So: a movable object's top face
			// straight under the body -> the small kick; otherwise your usual kick setting.
			auto kickFor = [&](const Sample& st, float usual) -> float { return overTopFace(st) ? 0.3f : usual; };

			// a remembered release for this exact storage beats the kick model
			auto applyMemory = [&](const Sample& st, SimParams& pp)
				{
					if (const auto dv = recallRelease(st)) { pp.hasReleaseDv = true; pp.releaseDv = *dv; return true; }
					return false;
				};

			// prediction held from the moment before the launch
			if (showHeld)
			{
				SimParams ph = p;
				ph.releaseKick = kickFor(held, p.releaseKick);
				applyMemory(held, ph);
				const SimResult r = heldRecall ? simulateAfterRelease(held, *heldRecall, ph, worldGrid, objectGrid, "held")
					: simulateFor("held", held, ph, worldGrid, objectGrid);
				drawPath(renderer, r, heldColour, tickMarks, held.dt);
				if (r.hit) renderer->renderSphere(r.firstContact, heldColour, 0.12f, true);
				renderer->renderSphere(r.pts.back(), r.died ? kDeathColour : heldColour, r.died ? 0.45f : 0.25f, true);
				if (labels) drawLabel(renderer, r.pts.back(), "pre-launch: " + describe(r), heldColour, labelScale, labelScale * 1.5f);
			}

			// live prediction from the current state
			if (showLive)
			{
				const V4 colour = (latest.stored || reachRemembered) ? storedColour : lineColour;
				// The release kick is the one thing the stored state cannot tell us (2-6 wu/s across forward-tap releases);
				// with it known, every logged flight matches to ~0.1-0.3 wu and the fall death to within a tick. So while
				// storing, also draw the paths for the low and high end of the kick, dimmed, to show the spread.
				const float kickRange = settings->launchTrajectoryReleaseKickRange->GetValue();
				const float liveKick = kickFor(latest, p.releaseKick);
				SimParams pl = p;
				pl.releaseKick = liveKick;
				const bool remembered = applyMemory(latest, pl);   // a known release: no spread to show
				if (latest.stored && p.releaseModel && kickRange > 0.f && !remembered)
				{
					const V4 dim(colour.x * 0.55f, colour.y * 0.55f, colour.z * 0.55f, colour.w);
					for (const float kk : { liveKick - kickRange, liveKick + kickRange })
					{
						SimParams pk = p;
						pk.releaseKick = std::max(0.f, kk);
						const SimResult rk = simulate(latest, pk, worldGrid, objectGrid);
						// dashed (every other tick) so the spread lines cannot be mistaken for the main line
						for (size_t i = 1; i < rk.pts.size(); i += 2)
							renderer->drawEdge(rk.pts[i - 1], rk.pts[i], dim);
						renderer->renderSphere(rk.pts.back(), rk.died ? kDeathColour : dim, 0.15f, true);
						if (labels)
							drawLabel(renderer, rk.pts.back(), std::format("if the kick is {:.1f}: {}", pk.releaseKick, describe(rk)),
								rk.died ? kDeathColour : dim, labelScale * 0.75f, labelScale * 1.2f);
					}
				}
				if (liveIsReachRelease) pl.releaseModel = false;
				const SimResult r = liveRecall ? simulateAfterRelease(latest, *liveRecall, pl, worldGrid, objectGrid, "live")
					: simulateFor("live", latest, pl, worldGrid, objectGrid);
				drawPath(renderer, r, colour, tickMarks, latest.dt);
				if (r.hit) renderer->renderSphere(r.firstContact, impactColour, 0.12f, true);
				renderer->renderSphere(r.pts.back(), r.died ? kDeathColour : impactColour, r.died ? 0.45f : 0.3f, true);
				if (labels)
				{
					drawLabel(renderer, r.pts.back(), describe(r), r.died ? kDeathColour : impactColour, labelScale);
					if (r.hit && (r.firstContact - r.pts.back()).Length() > 1.f)
						drawLabel(renderer, r.firstContact, std::format("first contact {:.2f} s", r.firstContactTime), impactColour, labelScale * 0.85f);
					const float shown = p.maxSpeed > 0.f ? std::min(latest.vel.Length(), p.maxSpeed) : latest.vel.Length();
					drawLabel(renderer, latest.pos,
						std::format("{}{:.1f} wu/s{}   g {:.3f}{}", latest.stored ? "STORED " : "", latest.vel.Length(),
							shown < latest.vel.Length() ? std::format(" (capped to {:.0f})", shown) : std::string{}, p.gravity,
							liveIsReachRelease ? std::format("   (crouch storage {:.1f} wu/s: release {} at {:.0f}% of it{})", reachStoredSpeed,
								liveUsedAim ? "toward your aim (flat floor: direction unknown)" : "down the slope", reachRatio * 100.f,
								"")
							: (p.fallDeathSpeed > 0.f || mIsReach) ? std::string{} : std::string("   (no fall death: invulnerable or off)")),
						colour, labelScale);
				}

				// HUD read-out: in first person the label at your feet is off screen, so while momentum is stored show the
				// stored speed and where the prediction ends, centred under the crosshair
				if (latest.stored && settings->launchTrajectoryHudStored->GetValue())
				{
					const ImVec2 ds = ImGui::GetIO().DisplaySize;
					const float big = std::max(18.f, ds.y * 0.032f) * settings->launchTrajectoryHudScale->GetValue();
					const float y = ds.y * settings->launchTrajectoryHudHeight->GetValue();
					const float speed = latest.vel.Length();
					const uint32_t c = ImGui::ColorConvertFloat4ToU32(colour);
					RenderTextHelper::drawCenteredOutlinedText(std::format("STORED  {:.1f} wu/s", speed), { ds.x * 0.5f, y }, c, big, 1.f);
					std::string sub;
					if (r.died && mIsReach && r.deathTime <= 0.1f) sub = "DEATH ON RELEASE (falls past 13 wu/s - Reach terminal velocity)";
					else if (r.died) sub = std::format("fall death after {:.1f} s", r.deathTime);
					else sub = std::format("{} {:.1f} wu out after {:.1f} s", r.atRest ? "rest" : (r.wedged ? "wedged" : "still moving"), r.horizontal, r.flightTime);
					if (remembered || reachRemembered) sub += "   |   release known from an earlier launch";
					RenderTextHelper::drawCenteredOutlinedText(sub, { ds.x * 0.5f, y + big * 1.25f },
						r.died ? ImGui::ColorConvertFloat4ToU32(kDeathColour) : c, big * 0.62f, 1.f);
				}
			}

			// one more hit
			if (settings->launchTrajectoryNextHitToggle->GetValue() && !lastHit.valid && latest.valid && labels)
				drawLabel(renderer, latest.pos, "one-more-hit: no hit seen yet this session", settings->launchTrajectoryNextHitColor->GetValue(), labelScale * 0.85f, labelScale * 1.5f);
			if (showNextHit)
			{
				const V4 hitColour = settings->launchTrajectoryNextHitColor->GetValue();
				const SimResult r = simulateFor("next", nextHit, pNext, worldGrid, objectGrid);
				drawPath(renderer, r, hitColour, tickMarks, nextHit.dt);
				renderer->renderSphere(r.pts.back(), r.died ? kDeathColour : hitColour, r.died ? 0.45f : 0.25f, true);
				if (labels)
				{
					const V3 dv = nextHit.vel - latest.vel;
					drawLabel(renderer, r.pts.back(), std::format("with one more hit (+{:.1f} wu/s, last hit {:.0f} s ago): {}",
						dv.Length(), (GetTickCount64() - lastHit.whenMs) / 1000.0, describe(r)), r.died ? kDeathColour : hitColour, labelScale, labelScale * 3.f);
				}
			}
		}
		catch (HCMRuntimeException ex)
		{
			PLOG_ERROR << "Launch Trajectory rendering error: " << ex.what();
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onToggle(bool& on)
	{
		if (on)
		{
			try
			{
				lockOrThrow(render3DEventProviderWeak, render3DEventProvider);
				if (!mRenderCallback)
					mRenderCallback = render3DEventProvider->getRender3DEvent()->subscribe([this](GameState g, IRenderer3D* r) { onRender(g, r); });
				if (mTickEvent && !mTickCallback)
					mTickCallback = mTickEvent->subscribe([this](uint32_t t) { onSimTick(t); });
				{
					std::scoped_lock lk(mStateMutex);
					mLatest = Sample{}; mHeldValid = false; mPinned = false; mPinCandidate = 0; mTrace.clear();
				}
				mEnabled = true;
			}
			catch (HCMRuntimeException ex)
			{
				runtimeExceptions->handleMessage(ex);
			}
		}
		else
		{
			stop();
		}

		if (auto m = messagesWeak.lock()) m->addMessage(on ? "Launch Trajectory enabled." : "Launch Trajectory disabled.");
	}

	void stop()
	{
		mEnabled = false;
		mTickCallback.reset();
		for (int i = 0; i < 400 && mSimInFlight.load() > 0; ++i) Sleep(1);
		if (renderingMutex) renderingMutex.wait(true);
		mRenderCallback.reset();
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		mccStateWeak(dicon.Resolve<IMCCStateHook>()),
		render3DEventProviderWeak(resolveDependentCheat(Render3DEventProvider)),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mTickEvent(resolveDependentCheat(GameTickEventHook)->getGameTickEvent()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->launchTrajectoryToggle->valueChangedEvent, [this](bool& on) { onToggle(on); })
	{
		mGame = game.operator GameState::Value();
		mIsReach = mGame == GameState::Value::HaloReach;
		if (auto dp = dicon.Resolve<DirPathContainer>().lock()) mLogDir = dp->dirPath;
		if (mIsReach)
		{
			// Reach reads the player through HCM's own services (throws HCMInitException if their pointer data is missing)
			mPlayerDatumWeak = resolveDependentCheat(GetPlayerDatum);
			try { mSoftCeilingWeak = resolveDependentCheat(GetSoftCeilingData); }
			catch (...) {}   // optional: without it the predictions just ignore soft ceilings
			mObjectPhysicsWeak = resolveDependentCheat(GetObjectPhysics);
			try { mObjectAddressWeak = resolveDependentCheat(GetObjectAddress); }
			catch (...) {}   // optional: only logs the game's ground plane
		}
		// only used to turn the next-hit push with your aim; the overlay works without it
		try { mViewAngleWeak = resolveDependentCheat(GetPlayerViewAngle); }
		catch (...) { PLOG_ERROR << "[LaunchTrajectory] GetPlayerViewAngle unavailable - next-hit line will not rotate with your view"; }
		if (auto s = settingsWeak.lock(); s && s->launchTrajectoryToggle->GetValue())
		{
			bool on = true;
			onToggle(on);
		}
	}

	~Impl()
	{
		stop();
	}
};


LaunchTrajectory::LaunchTrajectory(GameState gameImpl, IDIContainer& dicon)
{
	if (gameImpl.operator GameState::Value() != GameState::Value::Halo2 && gameImpl.operator GameState::Value() != GameState::Value::HaloReach)
		throw HCMInitException(std::format("{} is only implemented for Halo 2 and Halo: Reach", nameof(LaunchTrajectory)));
	pimpl = std::make_unique<Impl>(gameImpl, dicon);
}

LaunchTrajectory::~LaunchTrajectory()
{
	PLOG_VERBOSE << "~" << getName();
}
