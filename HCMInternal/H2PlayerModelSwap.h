#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 2 (MCC 1.3528), 2026-11-24. The user's words: "a way to 'override' the player model, ie, if i start the game as an
// elite, swap to be a Spartan, and have spartan values 1:1 ... It will also need all the *dependencies* such as animation
// graphs, shaders, etc." and "The body swap needs the tag injection, because your first person arms/legs dont come with".
// Injects the tags an Arbiter level lacks (from a prebuilt pack) into the loaded map's tag table as the level loads, then
// points the chief's player row and the weapons' first-person entries at them. See H2PlayerModelSwap.cpp and
// Documents\Halo Mod And Tools\H2 Player Model Swap Research\STAGE_B_DESIGN.md.
class H2PlayerModelSwap : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H2PlayerModelSwap(GameState gameImpl, IDIContainer& dicon);
	~H2PlayerModelSwap();

	virtual std::string_view getName() override { return nameof(H2PlayerModelSwap); }
};
