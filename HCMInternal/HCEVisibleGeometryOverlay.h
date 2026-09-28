#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) VISIBLE GEOMETRY overlay.
//
// Draws the collision of the INSTANCED geometry that IS rendered - rocks, trees, crates, pipes, every placed prop
// the player can stand on - so its collision can be compared with what the game draws. It is the complement of
// HCEInvisibleGeometryOverlay: a solid instanced definition lands in exactly one of the two. Instanced geometry
// only; the structure BSP is HCEBspOverlay's job.
//
// Same data path and the same tag constants as HCEInvisibleGeometryOverlay (see that header for the layout and the
// measurements behind it). A definition is VISIBLE here when none of its collision surfaces carries the INVISIBLE
// flag, it is not the pathfinding navmesh, and it is not a zero-part render mesh the invisible overlay's guarded
// fallback already draws. Placements flagged 'render only' are skipped: the engine gives them no collision.
//
// BUDGET - the same model as Halo 5's Havok overlay, so a large radius degrades to "draws less" rather than stalling:
//   radius          only collision within this many world units of the camera; pieces are CUT at the radius, triangle
//                   by triangle and edge by edge (a big rock touching a 1 wu radius shows only its near part)
//   triangle budget nearest placements first; selection STOPS at this many collision triangles
//   refresh         the selection is redone at most this often, and only when the camera or a setting changed
//
// Unlike the invisible overlay there can be tens of thousands of these placements, so nothing is transformed up
// front. A worker thread keeps a per-level cache (placements and each definition's class), decodes a definition's
// collision the first time one of its placements comes within range, and re-selects around the camera. The render
// thread only draws the published result, merged into a few spatial batches, and never blocks.
// ================================================================================================================
class HCEVisibleGeometryOverlay : public IOptionalCheat
{
private:
	class HCEVisibleGeometryOverlayImpl;
	std::unique_ptr<HCEVisibleGeometryOverlayImpl> pimpl;

public:
	HCEVisibleGeometryOverlay(GameState game, IDIContainer& dicon);
	~HCEVisibleGeometryOverlay();
	std::string_view getName() override { return nameof(HCEVisibleGeometryOverlay); }
};
