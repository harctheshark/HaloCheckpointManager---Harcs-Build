#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): Force Launch. The MCC forceLaunch* settings, event and hotkey, in Halo world units per second
// (Z up, 1 wu/s = 3.048 m/s): relative mode ADDS velocity along the look direction, absolute mode REPLACES it.
// The velocity write is metro::move (MetroMove.h).
class MetroForceLaunch : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroForceLaunch(GameState game, IDIContainer& dicon);
	~MetroForceLaunch();
	std::string_view getName() override { return nameof(MetroForceLaunch); }
};
