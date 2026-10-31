#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 2 (MCC halo2.dll 1.3528): an overlay listing the player's Havok simulation island and the four nearest others, with
// each island's ticks until Havok puts it to sleep (deactivates it) if nothing in it moves, its quiet-round count and the
// ticks to its next short / long check, and per body whether it would pass the check now (or what keeps it awake).
// Ported from Cartographer's havok_sleep.cpp (Halo 2 Vista); the MCC x64 layout is in HavokSleepCountdown.cpp.
class HavokSleepCountdown : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HavokSleepCountdown(GameState game, IDIContainer& dicon);
	~HavokSleepCountdown();
	std::string_view getName() override { return nameof(HavokSleepCountdown); }
};
