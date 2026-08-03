#pragma once
#include "pch.h"
#include "IMCCStateHook.h"

// Halo Campaign Evolved's equivalent of MCCStateHook.
//
// HCE is not MCC: there is no MCC gameEngineIndicator / loadIndicator / menuIndicator to read. Instead the Halo
// simulation lives in HaloSimulation_tag_release.dll, and we derive state from that module directly:
//   - module not loaded            -> MainMenu (the UE5 shell is up but no Halo sim yet)
//   - loaded, tick counter frozen  -> Loading
//   - loaded, tick counter ticking -> Ingame
// currentGameState is always HaloCER while the sim is loaded. HCE is a remake of the Halo CE campaign, so the
// halo1 LevelIDs are the semantically correct mapping for currentLevelID.
//
// Offsets are RVAs into HaloSimulation_tag_release.dll (from the HCM_Evolved RE):
//   tick_counter  0x12944C8      current_level 0xCA2F00      current_bsp 0x9A14E0
//
// Everything is polled on our own thread and guarded, so a torn/unloaded module can never fault the game.
class HCEStateHook : public IMCCStateHook
{
private:
	class HCEStateHookImpl;
	std::unique_ptr<HCEStateHookImpl> pimpl;

public:
	HCEStateHook();
	~HCEStateHook();

	virtual const MCCState& getCurrentMCCState() override;
	virtual bool isGameCurrentlyPlaying(GameState gameToCheck) override;
	virtual std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> getMCCStateChangedEvent() override;

	// Live "is a mouse cursor currently on screen" flag for HCMInternalGUI.
	// HCM gates ALL mouse input to the overlay on this (HCMInternalGUI.cpp: ImGuiWindowFlags_NoInputs when false),
	// so without it you can see the overlay but never click it. MCC exposes its own cursor bool at a known address;
	// HCE has no equivalent we know of, so we derive it from the WIN32 cursor state instead - engine-agnostic and
	// correct for what the flag actually means. Updated by the poll thread; the pointer stays valid for our lifetime.
	bool* getCursorShowingFlag();
};
