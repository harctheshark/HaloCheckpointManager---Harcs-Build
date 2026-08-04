#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) trigger volume overlay.
//
// Draws every scenario trigger volume as a world-space wireframe with a name label. Boxes are drawn as oriented
// boxes and SECTOR volumes as their real extruded polygon prism (the external CER tool drew sectors as boxes,
// which is wrong for the L-shaped and curved ones).
//
// TWO DRAWING PATHS.
//
// PRIMARY: HCM's own IRenderer3D, through Render3DEventProvider. On HaloCER that is Renderer3DImplD3D12 - a real
// D3D12 renderer that records depth-tested triangles and lines into the command list D3D12Hook already owns. Solid
// faces are properly triangulated (sector caps are often CONCAVE, which ImGui's convex-polygon fill cannot handle),
// and the player's trigger test point is a real sphere. As on MCC, volumes depth-sort against EACH OTHER only - the
// renderer owns its depth buffer and clears it every frame; nothing is occluded by world geometry.
//
// FALLBACK: the original ImGui background-draw-list implementation, kept intact. It takes over automatically
// whenever the 3D path has not drawn recently - the D3D12 renderer failing to initialise, Render3DEventProvider
// failing to construct, or the graphics hook not being up yet. So the overlay is never lost, only downgraded.
// ================================================================================================================
class HCETriggerOverlay : public IOptionalCheat
{
private:
	class HCETriggerOverlayImpl;
	std::unique_ptr<HCETriggerOverlayImpl> pimpl;

public:
	HCETriggerOverlay(GameState game, IDIContainer& dicon);
	~HCETriggerOverlay();
	std::string_view getName() override { return nameof(HCETriggerOverlay); }
};
