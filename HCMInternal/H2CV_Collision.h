#pragma once
// Collision geometry cache for Halo 2 MCC: world collision BSP, instanced geometry (IG), breakables, and dynamic
// object collision (scenery / crates / machines). Ported from HCM's wireframe-private CollisionWireframeOverlay
// (the "sorting" by collision type), plus what the new viewer adds: per-surface polygon outlines (for the TIM
// labels) and occluder triangles (for the collision-depth occlusion pass).
//
// collision_bsp layout (dual-pool tag blocks {u32 count, i32 dataOffset}):
//   surfaces @+0x28 (stride 8):  {plane u16, first_edge u16, flags u8, breakable i8, material u16}
//        flags: two_sided 1, invisible 2, climbable 4, breakable 8, invalid 0x10, conveyor 0x20
//   edges    @+0x30 (stride 0xC): {start_vertex, end_vertex, forward_edge, reverse_edge, left_surface, right_surface} u16
//   vertices @+0x38 (stride 0x10): xyz f32
#include "H2CV_Halo2.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace h2cv
{
	using h2::Vec3;

	enum class Cat : uint8_t { Bsp, BspInvisible, Breakable, Instanced, InstancedInvisible, Scenery, Crate, Machine, KillTrigger, Count };
	constexpr int kCatCount = (int)Cat::Count;
	const char* catName(Cat c);

	struct Line { Vec3 a, b; };
	struct Tri { Vec3 a, b, c; };

	// a parsed (copied) collision_bsp
	struct CollBsp
	{
		struct Surface { uint16_t plane, firstEdge; uint8_t flags; int8_t breakable; uint16_t material; };
		struct Edge { uint16_t v0, v1, fwd, rev, left, right; };
		std::vector<Surface> surfaces;
		std::vector<Edge> edges;
		std::vector<Vec3> verts;
		uintptr_t liveSurfaces = 0; // live surfaces array (for breakable "invalid" flag re-reads)
		bool parse(uintptr_t collbsp, const h2::TagPools& p);
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
		const std::vector<Tri>& staticOccluders() const { return mStaticTris; }   // world + IG, non-invisible
		const std::vector<Tri>& objectOccluders() const { return mObjectTris; }   // scenery/crates/machines
		const std::vector<Tri>& barrierFills() const { return mBarrierTris; }     // invisible (*) surfaces, world + IG
		const std::vector<Tri>& killTriggerFills() const { return mKillTris; }    // scenario kill-trigger volume faces

		bool valid() const { return mSbsp != 0; }
		const CollBsp& worldBsp() const { return mWorld; }

		// polygon outline for a Havok BSP collection child (category decoded by the caller)
		bool worldPolygon(uint32_t surface, std::vector<Vec3>& out) const { return mWorld.polygon(surface, out); }
		bool igPolygon(uint32_t instance, uint32_t surface, std::vector<Vec3>& out) const;
		size_t igInstanceCount() const { return mIgInstances.size(); }

		size_t edgeCountTotal() const;

	private:
		void rebuildStatic(uintptr_t sbsp);
		void gatherObjects();
		void classify(const CollBsp& bsp, const float* m, Cat normal, Cat invisible, bool withBreakable,
		              std::vector<Tri>* trisOut, std::vector<Tri>* invisTrisOut);

		uintptr_t mSbsp = 0;
		CollBsp mWorld;
		std::vector<CollBsp> mIgDefs;
		std::vector<IgInstance> mIgInstances;

		struct BreakableEdge { Line l; uint16_t surface; };
		std::vector<BreakableEdge> mBreakable;

		std::vector<Line> mLines[kCatCount];
		std::vector<Tri> mStaticTris, mObjectTris, mBarrierTris, mKillTris;
		void buildKillTriggers();

		// object collision: coll tag data address -> node-local collision bsps
		struct ObjBsp { uint16_t node; std::vector<Line> lines; std::vector<Tri> tris; };
		std::unordered_map<uintptr_t, std::vector<ObjBsp>> mCollCache;
		uint32_t mFrame = 0;
	};
}
