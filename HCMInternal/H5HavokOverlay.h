#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - HAVOK DEBUGGER.
//
// Draws the live physics collision: the static level mesh first, then instanced geometry, then dynamic object
// shapes. Geometry comes from H5GetHavokData; this class is rendering, culling and settings only.
//
// ⚠ THE LEVEL HAS 2.37 MILLION COLLISION TRIANGLES. Drawing them all is neither possible nor useful, so the
// reader culls per SECTION (each carries its own AABB, mean diagonal 1.35 world units) against a radius
// around the camera, and a hard triangle budget stops decoding rather than dropping the frame. Turn the
// radius up for a wider view and expect the budget to bite.
//
// ⚠ Collision reads far better as WIREFRAME than as solid. A solid 2M-triangle mesh is an opaque wall; the
// wireframe shows you the shape of what you are standing on. Fill is available but off by default.
//
// ⚠ NOT YET VERIFIED IN GAME. The decoder itself is strongly validated offline - it reproduces the engine's
// own declared triangle count EXACTLY on all 16 static meshes (2,371,357) and places a floor triangle
// 0.0005 world units under the player's feet as derived from the completely independent TLS/object chain -
// but this C++ port of it has never been run, and nothing here has been seen on screen.
// ================================================================================================================
class H5HavokOverlay : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5HavokOverlay(GameState game, IDIContainer& dicon);
	~H5HavokOverlay();
	std::string_view getName() override { return nameof(H5HavokOverlay); }
};
