#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux: Game Speed.
// Build 113 (2026-11-25): a Cheat Engine-style speedhack. metro.exe's two clock imports (QueryPerformanceCounter and
// timeGetTime) are pointed at detours that return a scaled clock, so everything the game times (scripts, dialogue waits,
// movement, AI, animation, physics) follows the chosen speed. See MetroGameSpeed.cpp.
// Build 112 scaled the engine's static time multiplier 0xFC10F0 instead (the time scale is 0xFC10E8 x 0xFC10EC scripted
// slow-motion x 0xFC10F0; read by the physics substep 0x76C550, the sound pitch 0x6C3E90 / 0x829510; clamped to [0.01, 10]).
// That worked live, but the user found dialogue and scripts kept their own real-time pace.
class MetroGameSpeed : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroGameSpeed(GameState game, IDIContainer& dicon);
	~MetroGameSpeed();
	std::string_view getName() override { return nameof(MetroGameSpeed); }
};
