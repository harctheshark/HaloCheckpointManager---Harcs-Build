#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Sets the player's world-space velocity (physicsEntry + 0x230).
//
// SCOPE NOTE: absolute world-axis velocity only. MCC's ForceLaunch also offers a look-direction-relative mode,
// which needs a player view angle - HCE exposes none through any chain the reference tool reversed. Note also
// that HCE's primitive is a SET, not an add: this replaces the velocity rather than adding to it, matching
// HCM_Evolved player_camera.py::set_player_velocity and MCC's own absolute branch.
class HCEForceLaunch : public IOptionalCheat
{
private:
	class HCEForceLaunchImpl;
	std::unique_ptr<HCEForceLaunchImpl> pimpl;

public:
	HCEForceLaunch(GameState game, IDIContainer& dicon);
	~HCEForceLaunch();
	std::string_view getName() override { return nameof(HCEForceLaunch); }
};
