#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IModel.h"

// ================================================================================================================
// Halo 5: Forge - HAVOK COLLISION READER.
//
// Walks the live physics world and produces world-space triangles for the static level collision, the
// instanced geometry placed on top of it, and (secondarily) dynamic object shapes.
//
// ⚠⚠ HALO 5 USES **hknp**, THE NEXT-GEN HAVOK API - NOT the classic hkp one.
// Both sets of classes are present in the binary (~1051 hkp reflection names, plenty of hkp vtables), but
// no hkp object exists in the live world: the world is hknpWorld, its simulation hknpMultithreadedSimulation,
// its broadphase hknpHybridBroadPhase, and every shape reachable from every body is an hknp* class. Do not
// spend time on hkpBvCompressedMeshShape or MOPP trees - wrong API for this title.
//
// THE CHAIN
//     world  = [exe + 0x05FB1CD8]                       // hknpWorld*, held directly, no indirection
//     bodies = hkArray at world+0x20                    // {void* data; u32 size; u32 capacityAndFlags}
//     body   = bodies.data + i * 0x90                   // hknpBody
//
//     body + 0x00/0x10/0x20   rotation COLUMNS (hkVector4 each; w ignored)
//     body + 0x30             translation
//     body + 0x40  u32        flags
//     body + 0x44  u32        collisionFilterInfo       // the collision LAYER
//     body + 0x48  hknpShape* shape                     // ⚠ NULL means the slot is unused
//
// ⚠⚠ hkTransform IS COLUMN-MAJOR: world = col0*v.x + col1*v.y + col2*v.z + translation. Treating the three
// stored vectors as ROWS instead was tested against a completely independent quantity - the orientation
// quaternion at motion+0x10 - across all 84 dynamic bodies: columns reproduce it with max error 0.000000,
// rows give mean error 1.576. (An earlier check against the compound's own AABB could not tell them apart,
// because every instance rotation in that sample was axis-aligned. Classic symmetry trap.)
//
// ⚠⚠ AN UNUSED BODY SLOT IS **NOT** ZEROED. 3981 of the 4096 slots carry a free-list chain in their first
// dword. `shape == 0` is the only valid emptiness test.
//
// SHAPE TYPE IS THE VTABLE, not a field. hknpShape has no type enum - only dispatchType (1 convex /
// 2 composite) and numShapeKeyBits - so the vtable pointer is the discriminator. RVAs in the .cpp.
//
// STATIC WORLD COLLISION: 16 bodies, each an hknpStaticCompoundShape at identity holding exactly one
// hknpCompressedMeshShape. 2,371,357 triangles across 3 collision layers (filter 0x1D = 6 bodies and
// 2,270,990 triangles, 0x1C = 6 bodies, 0x1B = 4 bodies).
//
// ⚠ THE PAYLOAD IS BIG BUT CHEAP TO CULL. Each mesh is split into Sections that each carry their own AABB,
// with a mean of 78 primitives and a mean diagonal of 1.35 world units. A 30 WU sphere around the player
// keeps roughly 2% of them, so decoding visible sections per frame is trivial - decoding ALL 2.27M
// triangles of the main layer took 0.8 s in pure Python, so a C++ pass over 2% is nothing.
//
// UNITS: Z is up, 1 world unit = 10 feet = 3.048 m. Established end to end - the highest decoded collision
// triangle directly under the player sits 0.600 WU below the eye position from the separate TLS chain,
// i.e. a 6 ft eye height, and the player's own physics capsule stands on that same triangle to 0.004 WU.
// ================================================================================================================
class H5GetHavokData : public IOptionalCheat
{
public:
	enum class ShapeKind
	{
		Unknown,
		CompressedMesh,     // the static level collision
		ConvexPolytope,     // hulls - directly drawable polygons
		Sphere,
		Capsule,
		Compound,           // static or dynamic; holds instances
	};

	// One drawable piece of collision, already transformed into world space.
	struct Piece
	{
		ShapeKind kind = ShapeKind::Unknown;
		uint32_t bodyIndex = 0;
		uint32_t collisionFilter = 0;   // the layer, from body+0x44
		bool isStaticWorld = false;     // body has no motion and an identity transform
		VertexCollection verts;
		IndexCollection edges;          // collision reads best as wireframe
		IndexCollection triangles;
	};

	H5GetHavokData(GameState game, IDIContainer& dicon);
	~H5GetHavokData();
	std::string_view getName() override { return nameof(H5GetHavokData); }

	// Decode everything within `radius` world units of `around`. Re-walks the world each call; the section
	// AABBs make that cheap. Throws when no physics world is loaded.
	// `maxTriangles` is a hard budget - decoding stops once it is hit, so a bad cull radius degrades into
	// "draws less" rather than "drops the frame".
	void collect(const SimpleMath::Vector3& around, float radius, int maxTriangles,
		bool includeStaticWorld, bool includeInstances, bool includeObjects,
		std::vector<Piece>& out);

	// Cheap summary for the UI: body count, static mesh count, total declared triangles.
	struct Summary { int bodies = 0; int staticMeshes = 0; int64_t declaredTriangles = 0; };
	Summary summarise();

private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
};
