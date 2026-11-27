#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux: Save Checkpoint / Revert on the game's own checkpoint slot "auto_save".
//   Save Checkpoint = console "gamesave auto_save" (console object [0xFBD770], vtable +72 deferred queue, game thread).
//   Revert (build 114, default) = an IN-PLACE restore: the engine's load core 0x672030(game, "auto_save") is called on the
//     game thread from a hook on the per-frame save/load tick 0x675CC0; for a save of the current level the engine restores
//     every entity from the save chunks without a level reload (see MetroCheckpoint.cpp).
//   Revert with "Revert with a level reload" ticked (setting metroUseGameSavesToggle) = console "gameload auto_save", the
//     stock path (the same-level case calls the level loader 0x69CEC0 = loading screen).
// Builds 95-113 had an HCM player-only snapshot (MetroMove.h take/restore) as the default; the user wanted a real revert.
// Uses the shared Force Checkpoint / Force Revert events and hotkeys.
class MetroCheckpoint : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroCheckpoint(GameState game, IDIContainer& dicon);
	~MetroCheckpoint();
	std::string_view getName() override { return nameof(MetroCheckpoint); }
};
