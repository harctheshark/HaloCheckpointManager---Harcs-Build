#pragma once
#include "pch.h"
#include "IMCCStateHook.h"

// Metro 2033 Redux / Metro Last Light Redux (metro.exe, 2026-11-24). The 4A engine has no MCC-style game / load / menu
// indicators, so this polls the engine's own globals (static RE, Documents\Halo Mod And Tools\Metro Redux\
// BINARY_SURVEY_20261124.md): the level object [metro.exe + 0xFBD820] and its controlled player entity *(level + 56).
//   both present  -> MetroRedux / Ingame
//   otherwise     -> NoGame / MainMenu   (as Halo 5's hook: the GUI then shows only the game-agnostic rows)
// LevelID is a placeholder 0 (Metro levels are not in the byte enum LevelID); the level's name is logged on change and
// available from getLevelName().
// The cursor flag is always true (as Halo 5): HCM draws its own cursor while its window is up.
class MetroStateHook : public IMCCStateHook
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroStateHook();
	~MetroStateHook();

	const MCCState& getCurrentMCCState() override;
	bool isGameCurrentlyPlaying(GameState gameToCheck) override;
	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> getMCCStateChangedEvent() override;

	bool* getCursorShowingFlag();
	std::string getLevelName();
};
