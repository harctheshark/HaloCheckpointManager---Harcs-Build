#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-24): Invincibility and Infinite Ammo through the engine's own cheat bits. metro.exe's
// `ps_actor_flags` dword (RVA 0xF2E0F0 - the value of the g_toggle_aim / g_show_hitmarks / g_input_hand console variables,
// which are bit masks over it) still holds the stripped cheat commands' bits, and the game code still tests them:
//   bit 1 (g_god)            - the player takes no damage    (fn 0x1E3360 / 0x2427E0 family)
//   bit 8 (g_unlimitedammo)  - ammo is not used up            (tested in 0x24BDA0, 0x2AECD0, 0x4E4820, 0x4E4960)
// (MetroDeveloper restores the same commands; bit values from its RestoreCommands.cpp, the tests read in the IDB.)
// Only OUR bits are ever changed - the word also carries the player's own settings (aim toggle, hit markers, hand). They
// are re-applied 4x a second while on (a load or config_load may rewrite the word), and cleared when switched off and when
// HCM closes. Uses the shared Invulnerability / Infinite Ammo settings and hotkeys.
class MetroActorFlags : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroActorFlags(GameState game, IDIContainer& dicon);
	~MetroActorFlags();
	std::string_view getName() override { return nameof(MetroActorFlags); }
};
