#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): Kill Scripts. 4A level logic is a graph of trigger / logic / action nodes that pass their output
// events on through ONE function, the signal emit 0x735D60 (~500 call sites, all node classes; callers ignore its result).
// While on, its first byte is a `ret`: no node can activate the next, so a timer may still count but its "kill the player"
// output goes nowhere. AI, physics, the player and rendering do not go through it. Off = the original byte back.
// Static RE: Documents\Halo Mod And Tools\Metro Redux\binary_survey\agent_scripts\.
// Side effects: everything scripted stops - doors, cutscenes, spawns, objectives (and possibly the death screen).
class MetroKillScripts : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroKillScripts(GameState game, IDIContainer& dicon);
	~MetroKillScripts();
	std::string_view getName() override { return nameof(MetroKillScripts); }
};
