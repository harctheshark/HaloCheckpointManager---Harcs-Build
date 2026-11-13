#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// "One Shot Kill" for the MCC games (Halo 1, 2, 3, ODST, Reach, 4) - 2026-11-11: the engine's own cheat_omnipotent.
// Anything damaged by a player dies in one hit. See OneShotKill.cpp for how each game does it.
class OneShotKill : public IOptionalCheat
{
private:
	class OneShotKillImpl;
	std::unique_ptr<OneShotKillImpl> pimpl;

public:
	OneShotKill(GameState gameImpl, IDIContainer& dicon);
	~OneShotKill();

	virtual std::string_view getName() override { return nameof(OneShotKill); }
};
