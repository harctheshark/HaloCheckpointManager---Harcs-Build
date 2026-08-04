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
// It renders through ImGui's background draw list rather than HCM's IRenderer3D. That is not laziness: HCM's 3D
// renderer is DirectXTK-11 all the way down (IRenderer3D::updateCameraData takes an ID3D11Device*), and HaloCER
// is a D3D12 title - Render3DEventProvider throws "not impl yet" for it. Everything drawn on an ImGui draw list
// works unchanged on the D3D12 backend, which is the same reason HCEDisplayInfo uses RenderTextHelper.
//
// The cost of that choice: there is no depth buffer, so volumes are not occluded by walls and cannot be filled
// convincingly (faces would sort by emission order). Wireframe only, deliberately.
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
