#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Changes the player's world-space velocity (physicsEntry + 0x230).
//
// Same two modes as MCC's ForceLaunch, including MCC's deliberate asymmetry: the absolute mode SETS the
// velocity, the look-relative mode ADDS to it.
//
// SCOPE NOTE: player only. MCC additionally offers "apply to custom object", which needs a validated
// datum -> object physics resolver; HCE has none, and the velocity field offset was reversed against the
// player biped.
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
