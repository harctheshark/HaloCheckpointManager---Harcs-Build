#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

// Halo 2 TAA v2 (2026-11-23): PER-OBJECT MOTION for bipeds, vehicles, weapons, equipment, projectiles, machines, crates.
//
// v1 reprojected every pixel with the camera's motion alone, so anything that moves by itself fetched its history from
// where it is NOT and trailed. Halo 2 classic has no velocity buffer, so the motion is rebuilt from what the engine has:
//
//  POSES. The object renderer (sub_1807E4970) draws each object with the node matrices of
//    object_node_transform_interpolate_array (halo2+0x722CB0: (object datum, real_matrix4x3** out, int* count)) - MCC's
//    per-frame interpolation between the previous and the current tick's node transforms (buffers halo2+0x164B2E8 /
//    +0x164B2F0 -> output +0x164B2F8, 2048 slots of 0x340C bytes, matrices at +0x44) - and, when that cache has no entry
//    (interpolation off, cinematics), with the object's own node block (object +0x11A offset / +0x118 size, 52 B each).
//    capture() asks the same function for every live object at the first-person pass entry (the world is drawn, the
//    interpolation fraction is this frame's), so the poses are EXACTLY the ones on screen, and keeps last frame's.
//  SHAPE. Where an object is on screen is not known per pixel. Objects WITH a collision model (coll tag: regions +0x1C /
//    12 B -> permutations +4 / 20 B -> bsps +4 / 0x44 B: node u16 +0, collision bsp +4 - the geometry bullets hit,
//    node-local, hugging the mesh): its TRIANGLES (every surface of every bsp, all permutations, as a fan), each vertex
//    tagged with a slot = which of the definition's nodes it hangs on. The GPU pass draws them with this frame's node
//    matrices and keeps a pixel only where the scene surface is within a tolerance of the triangle (HCM_TAAProxy.hlsl).
//    Objects with NO collision model (weapons, projectiles, equipment): one BOX, the render model's bounds (mode +0x14
//    compression info: x / y / z ranges) taken into node 0's space with node 0's inverse bind matrix (mode nodes +0x48 /
//    0x60 B: real_matrix4x3 +0x28); a pixel whose world position is inside the box is the object's.
//    (2026-11-24: the first version used a box per node for everything. A box around a banshee's wing or a standing biped
//    is mostly air: the floor and the wall inside it took the object's motion - the user's debug-view screenshots.)
//  MOTION. Per node: the rigid transform that takes a point of the node from where it is now to where it was last frame
//    (previous matrix x inverse current matrix). An object is sent when ANY of its nodes moved - then all of them are, the
//    still ones with a zero motion.
//  Left out: objects of which nothing moved, objects off screen, jumps of more than 3 wu in a frame (teleports: that node
//    gets "did not move"), objects the camera is inside (the first-person player's own body is not drawn - it would claim
//    the floor around the feet), hidden / deleted objects, scenery (static), light fixtures, sound scenery.
namespace h2taa
{
	// = HCM_TAAProxy.hlsl's Proxy (9 float4)
	struct GpuProxy
	{
		float rect[4];     // screen rectangle in the view's ndc: x min, y min, x max, y max
		float w2l[12];     // world -> node-local, 3 rows of (x y z 1) coefficients
		float centre[4];   // box centre, node-local
		float half[4];     // box half extents, node-local (grown)
		float delta[12];   // current world position -> last frame's world position, 3 rows
	};
	static_assert(sizeof(GpuProxy) == 144, "GpuProxy = 9 float4");

	// = HCM_TAAProxy.hlsl's Node (6 float4)
	struct GpuNode
	{
		float l2w[12];     // node-local -> world this frame, 3 rows (all zero: the node has no usable pose - its triangles collapse)
		float delta[12];   // current world position -> last frame's world position, 3 rows
	};
	static_assert(sizeof(GpuNode) == 96, "GpuNode = 6 float4");

	// one vertex of a definition's collision triangles: node-local position + which of the definition's slots it hangs on
	struct TriVertex { float pos[3]; uint32_t slot; };
	static_assert(sizeof(TriVertex) == 16, "TriVertex layout");

	// one moving object with collision triangles: draw the mesh of `def` with the nodes starting at `firstNode`
	struct TriDraw { uint32_t def; uint32_t firstNode; };

	struct ObjectFrame
	{
		std::vector<GpuProxy> boxes;
		std::vector<GpuNode> nodes;
		std::vector<TriDraw> draws;
		void clear() { boxes.clear(); nodes.clear(); draws.clear(); }
		bool empty() const { return boxes.empty() && draws.empty(); }
	};

	struct ObjectStats
	{
		int objects = 0;        // live objects of the tracked types this frame
		int interpolated = 0;   // ... whose pose came from the engine's interpolation
		int withShape = 0;      // ... whose pose changed lately and that have collision triangles or a box
		int moving = 0;         // ... sent this frame
		int meshObjects = 0;    // ... of those, with collision triangles
		int nodes = 0;          // nodes sent for them
		int boxes = 0;          // boxes sent (objects with no collision model)
		int cameraInside = 0;   // left out: the camera is inside them
	};

	// The rigid transform taking a world position on a node from where it is NOW to where it was LAST FRAME (previous matrix x
	// inverse current matrix), 3 rows of (x y z 1) coefficients. cur / prev: real_matrix4x3 (13 floats). false = unusable matrix.
	// (Also used for the first-person model's bones, H2TAA.cpp.)
	bool nodeDelta(const float* cur, const float* prev, double out[12]);

	class ObjectMotion
	{
	public:
		// Render thread, at the first-person pass entry, once per resolved view. vpCur: this frame's world -> clip (row vector
		// v * M, un-jittered; only x, y, w are used). cam: camera position. Fills out (within the limits).
		void capture(const double vpCur[16], const float cam[3], ObjectFrame& out, size_t maxBoxes, size_t maxNodes, size_t maxDraws);
		// forget last frame's poses (a frame was skipped, revert, map change)
		void reset() { ++mFrame; ++mFrame; }
		const ObjectStats& stats() const { return mStats; }
		bool engineCallOk() const { return mEngineState > 0; }
		// the collision triangles of a definition (TriDraw::def); nullptr = none. The pointer and its contents stay as they are
		// until generation() changes (another map: every definition is dropped).
		const std::vector<TriVertex>* mesh(uint32_t def) const;
		uint64_t generation() const { return mGeneration; }

	private:
		struct Box { uint16_t node; float c[3]; float h[3]; };
		// boxes: one per node (camera-inside / moved / on-screen tests). tris not empty: the definition has collision triangles,
		// TriVertex::slot = index into boxes (-> its node); else the boxes themselves are the proxies.
		struct Def { std::vector<Box> boxes; std::vector<TriVertex> tris; float extent = 0.f; };   // extent: the largest box diagonal (node-local units)
		struct State { std::vector<float> m[2]; int count[2]{ 0, 0 }; uint64_t frame[2]{ 0, 0 }; uint64_t seen = 0; uint64_t moved = 0; };   // moved: the frame a node of it last moved

		const Def& definition(uintptr_t obj, uint32_t& tag);
		bool buildDefinition(uintptr_t def, Def& out);

		std::unordered_map<uint32_t, Def> mDefs;        // by object definition tag index
		uintptr_t mDefsScenario = 0;                    // the scenario the definitions were read under (map change -> rebuilt)
		uint64_t mGeneration = 1;
		std::unordered_map<uint32_t, State> mStates;    // by object datum
		uint64_t mFrame = 2;
		int mEngineState = 0;                           // 0 not checked, 1 the engine's function is used, -1 not usable (own node blocks)
		ObjectStats mStats;
		bool mLoggedDefs = false;
	};
}
