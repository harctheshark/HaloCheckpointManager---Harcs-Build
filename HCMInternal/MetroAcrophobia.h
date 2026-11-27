#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-25): Acrophobia - hold JUMP (Space / A) to fly along your look direction, hold CROUCH (Left Ctrl /
// L3) to hover. The Halo 1 / CER flight model (see H5Acrophobia.h), run on the game's thread inside the character
// controller's move (MetroMove.cpp), so walls and floors still stop you. Shares the Halo 5 Acrophobia setting and hotkey.
// This class only feeds the toggle and the held keys (read through ImGui, which has keyboard AND controller).
class MetroAcrophobia : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroAcrophobia(GameState game, IDIContainer& dicon);
	~MetroAcrophobia();
	std::string_view getName() override { return nameof(MetroAcrophobia); }
};
