#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): Force Teleport. The MCC forceTeleport* settings, event and hotkey, in Halo world units (Z up,
// 1 wu = 3.048 m), relative to the look direction or to manual coordinates. The move itself is metro::move (MetroMove.h).
class MetroForceTeleport : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroForceTeleport(GameState game, IDIContainer& dicon);
	~MetroForceTeleport();
	std::string_view getName() override { return nameof(MetroForceTeleport); }
};
