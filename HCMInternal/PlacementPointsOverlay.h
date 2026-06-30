#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Visualises Halo 3's respawn / vehicle-exit placement search (RE'd from H3EK sapien -> retail halo3.dll).
// The engine, when placing a biped (respawn or after a blocked vehicle exit), tries a fixed set of directions
// (a 27-entry cube sampling table @ halo3.dll+0x7FB090: center + 6 faces + 12 edges + 8 corners) at expanding
// radii, testing each candidate against collision until one is clear. This overlay replicates the candidate
// generation around the player so the search pattern is visible at all times.
//   - 18 base points (center + horizontal + UP) = the respawn set.
//   - +9 downward points = the VEHICLE-only extended set (gated behind its own toggle).

class PlacementPointsOverlayUntemplated { public: virtual ~PlacementPointsOverlayUntemplated() = default; };
class PlacementPointsOverlay : public IOptionalCheat
{
private:
	std::unique_ptr<PlacementPointsOverlayUntemplated> pimpl;

public:
	PlacementPointsOverlay(GameState gameImpl, IDIContainer& dicon);
	~PlacementPointsOverlay();

	std::string_view getName() override { return nameof(PlacementPointsOverlay); }
};
