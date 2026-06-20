#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class ISeason7PhysicsImpl { public: virtual ~ISeason7PhysicsImpl() = default; };

// Halo 2 only. Toggles the physics/tickrate scalar instruction between the "Season 7" constant (ON) and
// the game's default constant (OFF) by rewriting the mulss instruction's bytes in .text.
class Season7Physics : public IOptionalCheat
{
private:
	std::unique_ptr<ISeason7PhysicsImpl> pimpl;

public:
	Season7Physics(GameState gameImpl, IDIContainer& dicon);
	~Season7Physics();

	std::string_view getName() override { return nameof(Season7Physics); }
};
