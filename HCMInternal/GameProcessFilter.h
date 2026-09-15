#pragma once
#include "GameState.h"
#include "IGetMCCVersion.h"

// Which GameStates can possibly exist inside THIS host process?
//
// HCM is injected into exactly one title at a time, and the titles are mutually exclusive processes:
// MCC (Steam/WinStore), Halo Campaign Evolved, and Halo 5: Forge. Building an MCC game's cheats
// inside HaloCER produced ~428 bogus "failed service" reports plus a lot of pointless work, which is
// why this filter exists at all.
//
// ⚠ THIS USED TO BE A BOOLEAN - `isHaloCER != isCampaignEvolvedProcess` - written out longhand in
// TWO places (OptionalCheatManager::createCheats and GUIElementConstructor). That form silently
// breaks the moment a THIRD title exists: with Halo 5: Forge added, "not HaloCER" is true for both
// MCC games and Halo5Forge games, so each would have been built inside the other's process. Keep the
// decision here, once, as a mapping rather than a negation.
//
// The two callers must agree exactly: if they ever disagree you get phantom failures (element built,
// cheat wasn't) or silently missing UI (cheat built, element wasn't).
inline bool gameBelongsToProcess(GameState::Value game, MCCProcessType proc)
{
	// NoGame (255) is the GAME-AGNOSTIC bucket - global settings that belong to no title. Always
	// exempt: a naive per-title test drops every one of them and takes the global UI with it.
	if (game == GameState::Value::NoGame) return true;

	switch (proc)
	{
	case MCCProcessType::CampaignEvolved: return game == GameState::Value::HaloCER;
	case MCCProcessType::Halo5Forge:      return game == GameState::Value::Halo5Forge;
	case MCCProcessType::Steam:
	case MCCProcessType::WinStore:
	default:
		// Everything that is not one of the standalone titles is an MCC game.
		return game != GameState::Value::HaloCER && game != GameState::Value::Halo5Forge;
	}
}
