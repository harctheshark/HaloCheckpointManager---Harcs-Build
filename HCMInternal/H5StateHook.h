#pragma once
#include "pch.h"
#include "IMCCStateHook.h"

// Halo 5: Forge's equivalent of MCCStateHook (and the direct analogue of HCEStateHook).
//
// Halo 5 is not MCC: there is no gameEngineIndicator / loadIndicator / menuIndicator to read. The whole engine
// lives in halo5forge.exe, and state is derived from the simulation thread's TLS block:
//
//   no thread has a TLS block with player globals   -> MainMenu (the shell is up, no simulation yet)
//   player globals present, player datum valid      -> Ingame
//   player globals present, datum == 0xFFFFFFFF     -> Ingame ANYWAY, see below
//
// ⚠ A -1 datum is NOT "loading". It is the normal state while the player is dead, respawning, or mid-level-
// transition, and it recurs constantly during play. HaloCER learned the equivalent lesson the expensive way
// (reporting Loading for a PAUSED game switched every cheat off mid-session), so Halo 5 does not repeat it:
// once the globals are addressable we stay Ingame and let individual cheats fail transiently on the datum.
//
// currentGameState is always Halo5Forge while the simulation is addressable.
//
// ⚠ currentLevelID: Halo 5's levels are not in HCM's LevelID enum at all, so there is nothing honest to
// report. We pin it to a fixed placeholder rather than inventing a mapping - anything level-keyed simply will
// not match, which is the correct outcome. Do NOT map these onto halo1 IDs the way HCE does: HCE is a remake
// of the Halo CE campaign so that mapping is semantically real, and Halo 5's is not.
//
// Everything is polled on our own thread and SEH-guarded, so a torn or unmapped TLS block can never fault the
// game.
class H5StateHook : public IMCCStateHook
{
private:
	class H5StateHookImpl;
	std::unique_ptr<H5StateHookImpl> pimpl;

public:
	H5StateHook();
	~H5StateHook();

	const MCCState& getCurrentMCCState() override;
	bool isGameCurrentlyPlaying(GameState gameToCheck) override;
	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> getMCCStateChangedEvent() override;

	// isCursorShowing gates ALL mouse input to the overlay, so it is load-bearing rather than cosmetic.
	// MCC exposes its own bool at a known address; Halo 5 has no equivalent we know of, so - exactly as
	// HCEStateHook does - this is derived from the live WIN32 cursor state (GetCursorInfo/CURSOR_SHOWING),
	// which is engine-agnostic and true to what the flag means.
	bool* getCursorShowingFlag();
};
