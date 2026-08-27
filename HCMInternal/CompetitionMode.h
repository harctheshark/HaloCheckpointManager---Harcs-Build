#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Competition Mode - a broadcast scoreboard overlay for Halo 2 Classic multiplayer.
//
// Intended for an observer flying free camera at a tournament: red team down the left of the screen, blue down
// the right, each with the team total and a per-player breakdown. Gametype aware - the score column relabels
// itself and switches to MM:SS for the time-based engines, mirroring what the game's own formatter does.
//
// Halo 2 Classic (GameState::Halo2) only. See H2CompetitionData.h for the memory layout and its provenance.
class CompetitionModeUntemplated { public: virtual ~CompetitionModeUntemplated() = default; };
class CompetitionMode : public IOptionalCheat
{
private:
	std::unique_ptr<CompetitionModeUntemplated> pimpl;

public:
	CompetitionMode(GameState gameImpl, IDIContainer& dicon);
	~CompetitionMode();

	std::string_view getName() override { return nameof(CompetitionMode); }
};
