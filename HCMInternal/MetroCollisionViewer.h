#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): Collision Viewer - the PhysX scene's collision (the level's static triangle meshes, boxes,
// spheres, capsules, convex hulls of dynamic objects) drawn as a wireframe on HCM's overlay, coloured by surface:
// green = floor you can stand on, blue-grey = wall, red = ceiling; dynamic objects orange.
//
// Static RE: Documents\Halo Mod And Tools\Metro Redux\binary_survey\agent_collision\. A worker thread reads the scene
// under PhysX's own read lock (never the write lock): world *(0xFBD8A0), PxScene at +0xF8; PhysX 3.3 vtable slots
// (each bracketed by slots seen used in the exe): scene getNbActors +136 / getActors +144 / lockRead +792 / unlockRead +800,
// actor isKindOf +32, getGlobalPose +160, getNbShapes +200, getShapes +208; shape getGeometryType +40, box/sphere/capsule/
// convex/trimesh geometry +64/+72/+80/+96/+104, getLocalPose +136; triangle mesh getNbVertices +40, getVertices +48,
// getNbTriangles +56, getTriangles +64; convex mesh getNbVertices +40, getVertices +48, getIndexBuffer +56,
// getNbPolygons +64, getPolygonData +72. Every mesh is range-checked before use. The camera is the engine's main camera
// view-projection at 0xFC1230 (row vectors, D3D) and position at 0xFC1170.
class MetroCollisionViewer : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroCollisionViewer(GameState game, IDIContainer& dicon);
	~MetroCollisionViewer();
	std::string_view getName() override { return nameof(MetroCollisionViewer); }
};
