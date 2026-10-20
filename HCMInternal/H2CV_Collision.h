#pragma once
// Collision geometry cache for Halo 2 MCC: world collision BSP, instanced geometry (IG), breakables, and dynamic
// object collision (scenery / crates / machines). Ported from HCM's wireframe-private CollisionWireframeOverlay
// (the "sorting" by collision type), plus what the new viewer adds: per-surface polygon outlines (for the TIM
// labels) and occluder triangles (for the collision-depth occlusion pass).
//
// Halo 3 / ODST (Collision3.cpp): the same winged-edge collision_bsp in two variants (16-bit and 32-bit indices, see CollBsp::parse3),
// one per ACTIVE structure bsp, plus the instanced-geometry definitions from the same resource.
//
// collision_bsp layout, Halo 2 (dual-pool tag blocks {u32 count, i32 dataOffset}):
//   surfaces @+0x28 (stride 8):  {plane u16, first_edge u16, flags u8, breakable i8, material u16}
//        flags: two_sided 1, invisible 2, climbable 4, breakable 8, invalid 0x10, conveyor 0x20
//   edges    @+0x30 (stride 0xC): {start_vertex, end_vertex, forward_edge, reverse_edge, left_surface, right_surface} u16
//   vertices @+0x38 (stride 0x10): xyz f32
#include "H2CV_Halo2.h"
#include "H2CV_Halo3.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace h2cv
{
	using h2::Vec3;

	// SoftCeiling: Halo 3 / ODST only (structure design soft ceilings / soft kills / slip surfaces); always empty for Halo 2
	enum class Cat : uint8_t { Bsp, BspInvisible, Breakable, Instanced, InstancedInvisible, Scenery, Crate, Machine, KillTrigger, SoftCeiling, Count };
	constexpr int kCatCount = (int)Cat::Count;
	const char* catName(Cat c);

	struct Line { Vec3 a, b; };
	struct Tri { Vec3 a, b, c; };

	// Uniform-grid index over a static line list (built for the large Halo 3 / ODST sets): query() visits every line that touches a cell
	// within `r` of the point - a superset of the lines within r, the caller applies the exact distance test. Halo 2 never builds one.
	class LineGrid
	{
	public:
		void build(const std::vector<Line>& lines, float cell);
		bool ready() const { return mBuilt; }
		template <class F> void query(const std::vector<Line>& lines, const Vec3& p, float r, F&& fn) const
		{
			if (mStamp.size() != lines.size()) mStamp.assign(lines.size(), 0);
			if (++mTick == 0) { std::fill(mStamp.begin(), mStamp.end(), 0u); mTick = 1; }
			const float r2 = r * r;
			for (const Cell& c : mCells)
			{
				const float x0 = c.x * mCell, y0 = c.y * mCell, z0 = c.z * mCell;
				const float dx = p.x < x0 ? x0 - p.x : (p.x > x0 + mCell ? p.x - x0 - mCell : 0.f);
				const float dy = p.y < y0 ? y0 - p.y : (p.y > y0 + mCell ? p.y - y0 - mCell : 0.f);
				const float dz = p.z < z0 ? z0 - p.z : (p.z > z0 + mCell ? p.z - z0 - mCell : 0.f);
				if (dx * dx + dy * dy + dz * dz > r2) continue;
				for (uint32_t k = 0; k < c.count; ++k)
				{
					const uint32_t i = mItems[c.first + k];
					if (mStamp[i] != mTick) { mStamp[i] = mTick; fn(lines[i]); }
				}
			}
			for (uint32_t i : mLong) if (mStamp[i] != mTick) { mStamp[i] = mTick; fn(lines[i]); }
		}
	private:
		struct Cell { int x, y, z; uint32_t first, count; };
		std::vector<Cell> mCells;
		std::vector<uint32_t> mItems, mLong;
		float mCell = 32.f; bool mBuilt = false;
		mutable std::vector<uint32_t> mStamp; mutable uint32_t mTick = 0;
	};

	// a parsed (copied) collision_bsp
	struct CollBsp
	{
		static constexpr uint32_t kNone = 0xFFFFFFFFu;   // "no surface" in edge left / right (Halo 2's 0xFFFF)
		// plane = plane index with the flip flag split out; breakable = Halo 2 breakable bit index; breakBit / breakGroup = Halo 3 / ODST
		// (bit within the group row; for Halo 2 breakBit mirrors `breakable` and group is 0)
		struct Surface { uint32_t plane, firstEdge; bool flip; uint8_t flags; int8_t breakable; uint16_t material; uint16_t breakBit, breakGroup; };
		struct Edge { uint32_t v0, v1, fwd, rev, left, right; };
		std::vector<Surface> surfaces;
		std::vector<Edge> edges;
		std::vector<Vec3> verts;
		std::vector<float> planes;   // planes (stride 0x10): {i, j, k, d}; Surface::flip = plane is flipped
		bool parse(uintptr_t collbsp, const h2::TagPools& p);
		// Halo 3 / ODST: `bsp` = collision_bsp struct (8 blocks of {count, offset, def} at +0 nodes, +0x0C planes, +0x18 leaves, +0x24 bsp2d refs,
		// +0x30 bsp2d nodes, +0x3C surfaces, +0x48 edges, +0x54 vertices). Small: surface 12 B / edge 12 B (u16) / vertex 16 B;
		// large: surface 16 B / edge 24 B (u32) / vertex 20 B. Plane = 16 B in both.
		bool parse3(uintptr_t bsp, bool large);
		// front-facing unit normal of a surface (its plane, negated when the plane index is flagged); false if malformed
		bool normal(uint32_t surface, Vec3& out) const;
		// ordered polygon outline of a surface (Halo edge-ring walk); false if malformed
		bool polygon(uint32_t surface, std::vector<Vec3>& out) const;
	};

	// IG instance placement: [0]=scale, [1..3]=forward, [4..6]=left, [7..9]=up, [10..12]=position
	struct IgInstance { float m[13]; int def; };
	Vec3 igTransform(const float* m, const Vec3& v);

	class CollisionWorld
	{
	public:
		// call once per frame from the render thread; cheap when nothing changed
		void update(bool wantObjects, bool wantBreakableLive);
		void invalidate() { mSbsp = 0; }

		const std::vector<Line>& lines(Cat c) const { return mLines[(int)c]; }
		// visit the lines of a category that may lie within r of p (Halo 3 / ODST static sets use a grid; everything else is a plain scan)
		template <class F> void forNear(Cat c, const Vec3& p, float r, F&& fn) const
		{
			const int ci = (int)c;
			if (mGrid[ci].ready()) mGrid[ci].query(mLines[ci], p, r, fn);
			else for (const Line& l : mLines[ci]) fn(l);
		}
		const std::vector<Tri>& staticOccluders() const { return mStaticTris; }   // world + IG, non-invisible
		const std::vector<Tri>& objectOccluders() const { return mObjectTris; }   // scenery/crates/machines
		const std::vector<Tri>& barrierFills() const { return mBarrierTris; }     // invisible (*) surfaces, world + IG
		const std::vector<Tri>& killTriggerFills() const { return mKillTris; }    // scenario kill-trigger volume faces

		bool valid() const { return mSbsp != 0; }
		uintptr_t sbspId() const { return mSbsp; }   // changes on every BSP (re)load
		const CollBsp& worldBsp() const { return mWorld; }

		// polygon outline for a Havok BSP collection child (category decoded by the caller)
		bool worldPolygon(uint32_t surface, std::vector<Vec3>& out) const { return mWorld.polygon(surface, out); }
		bool igPolygon(uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const;
		size_t igInstanceCount() const { return mIgInstances.size(); }
		size_t igSurfaceCount(uint32_t instance) const;   // surfaces in that instance's definition bsp (0 if unparsed)

		// ---- Halo 3 / ODST (Collision3.cpp): one entry per active structure bsp ----
		struct Bsp3
		{
			int index = 0; uintptr_t sbsp = 0; std::string how;
			CollBsp world;
			std::vector<CollBsp> igDefs; std::vector<uint8_t> igOk;   // by definition index
			std::vector<IgInstance> igInst;
		};
		const std::vector<Bsp3>& bsps3() const { return mB3; }
		const Bsp3* bsp3(int index) const;
		bool worldPolygon3(int bsp, uint32_t surface, std::vector<Vec3>& out) const;
		bool igPolygon3(int bsp, uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const;
		void setWantCreases(bool b) { mWantCreases = b; }
		const std::string& lastWhy() const { return mWhy3; }   // why the last bsp resolve failed (log / HUD)
		int objectShapesSkipped() const { return mObjSkipped3; }

		size_t edgeCountTotal() const;

		// "desyncable creases": two collision surfaces sharing an edge whose fronts face each other at less than 90
		// degrees (concave AND nA.nB < 0). Ghosting model: a head-on bump into A rewinds along -nA, which over-credits
		// the TIM of any nearby polygon B with nB.(-nA) > 0, i.e. nA.nB < 0 - a flat floor meeting a vertical wall
		// (nA.nB = 0) can't be charged, a floor sloping down into the wall can. sharp = -nA.nB (0..1).
		struct Crease { Line edge; float sharp; };
		const std::vector<Crease>& creases() const { return mCreases; }
		const std::vector<Line>& creaseFaces() const { return mCreaseFaces; }   // outlines of both faces of each crease
		int planeOrientationAgree() const { return mPlaneAgree; }               // diagnostics: winding vs signed plane
		int planeOrientationDisagree() const { return mPlaneDisagree; }

	private:
		void rebuildStatic(uintptr_t sbsp);
		void gatherObjects();
		void updateH3(bool wantObjects, bool wantBreakableLive);
		void rebuildStatic3(const std::vector<h3::BspRes>& act);
		void buildKillTriggers3();
		void buildSoftCeilings3(const std::vector<h3::BspRes>& act);
		void gatherObjects3();
		std::vector<Bsp3> mB3;
		uint64_t mSig3 = 0;
		bool mWantCreases = true, mCreasesBuilt = false;
		std::string mWhy3;
		int mObjSkipped3 = 0;
		void classify(const CollBsp& bsp, const float* m, Cat normal, Cat invisible, bool withBreakable,
		              std::vector<Tri>* trisOut, std::vector<Tri>* invisTrisOut, int slot = 0);

		uintptr_t mSbsp = 0;
		CollBsp mWorld;
		std::vector<CollBsp> mIgDefs;
		std::vector<IgInstance> mIgInstances;

		// edge bordering a breakable collision surface; left/right are surface indices (0xFFFF = none)
		struct BreakableEdge { Line l; uint32_t left, right; int slot; };   // slot = index into mB3 (Halo 3), 0 for Halo 2
		std::vector<BreakableEdge> mBreakable;

		std::vector<Crease> mCreases;
		std::vector<Line> mCreaseFaces;
		int mPlaneAgree = 0, mPlaneDisagree = 0;
		void findCreases(const CollBsp& bsp, const float* m);   // m = IG placement, nullptr for the world bsp

		std::vector<Line> mLines[kCatCount];
		LineGrid mGrid[kCatCount];
		std::vector<Tri> mStaticTris, mObjectTris, mBarrierTris, mKillTris;
		void buildKillTriggers();

		// object collision, parsed from the engine's own walk (sub_180734FB0):
		//   coll tag: regions {count @+0x1C, offset @+0x20} stride 12 -> permutations {count @+4, offset @+8} stride 20
		//   -> bsps {count @+4, offset @+8} stride 0x44 {node_index u16 @0, collision_bsp @+4}
		struct ObjBsp { uint16_t node; std::vector<Line> lines; std::vector<Tri> tris; };
		struct ObjPerm { std::vector<ObjBsp> bsps; };
		struct ObjRegion { std::vector<ObjPerm> perms; };
		struct ObjColl { std::vector<ObjRegion> regions; };
		std::unordered_map<uintptr_t, ObjColl> mCollCache;     // coll tag data address -> geometry
		// model (hlmt) regions {count @+0x70, offset @+0x74} stride 16: collision_region_index i8 @+4, permutations
		// {count @+8, offset @+0xC} stride 8, collision_permutation_index i8 @+5 (sub_180793C40 / sub_180793760)
		struct ModelRegion { int8_t collRegion; std::vector<int8_t> collPerm; };
		struct ModelMap { std::vector<ModelRegion> regions; bool anyMapped = false; };
		std::unordered_map<uintptr_t, ModelMap> mModelCache;   // hlmt tag data address -> region mapping
		const ObjColl* loadColl(uintptr_t coll, const h2::TagPools& p);
		const ModelMap* loadModelMap(uintptr_t hlmt, const h2::TagPools& p);
		uint32_t mFrame = 0;
	};
}
