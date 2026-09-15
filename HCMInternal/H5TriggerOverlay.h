#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - TRIGGER VOLUME OVERLAY.
//
// Draws the scenario's trigger volumes in 3D, filtered by category. Geometry and categories come from
// H5GetTriggerData; this class is only rendering, filtering and settings.
//
// FOUR CATEGORIES, each independently filterable and independently coloured:
//   Regular / Kill / Begin Zone Set / Zone Set Commit
//
// ⚠ FIELD OF VIEW IS A SETTING, NOT A READ. Renderer3DImplD3D12 builds its own projection from position,
// forward, up and a HORIZONTAL fov, and no FOV field has been located in Halo 5 yet. The overlay therefore
// takes the angle from h5TriggerOverlayFov and the boxes will only line up with the world once that matches
// the game's actual FOV. This is deliberately exposed rather than hard-coded, because guessing it silently
// produces an overlay that looks subtly broken with no way for the user to fix it.
//
// ⚠ SECTOR VOLUMES DRAW AS THEIR BOUNDING BOX. 173 of 379 volumes in the test scenario are sector type -
// arbitrary geometry, not boxes - and that mesh is not decoded yet. They are drawn as bounds and can be
// hidden with h5TriggerOverlayShowSectors so they are never mistaken for exact shapes.
// ================================================================================================================
class H5TriggerOverlay : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5TriggerOverlay(GameState game, IDIContainer& dicon);
	~H5TriggerOverlay();
	std::string_view getName() override { return nameof(H5TriggerOverlay); }
};
