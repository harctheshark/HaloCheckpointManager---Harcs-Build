#pragma once
#include "GameState.h"
#include "DIContainer.h"
#include "IOptionalCheat.h"

// Halo 3 MCC only. Interpolates the Theater render between engine ticks so a watched player turning
// no longer steps the world. See Halo3TheaterInterpCave.h for what is patched and where the cave came
// from, and Fixes/Halo3_Theater_Interp_FIXES.md for the reverse engineering behind it.
class Halo3TheaterInterp : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	Halo3TheaterInterp(GameState game, IDIContainer& dicon);
	~Halo3TheaterInterp();
	virtual std::string_view getName() override { return nameof(Halo3TheaterInterp); }
};
