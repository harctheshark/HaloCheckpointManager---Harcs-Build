#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 4: Halo 2's own player physics, from a separate mod dll (<HCM folder>\h4h2p\H4Halo2Physics.dll, not part of
// HCM's source). The first time the toggle goes on, the dll is loaded into MCC; it waits for halo4.dll, checks the build
// and hooks the Havok step itself (it logs to h4h2p\H4Halo2Physics.log). From then on the toggle only switches it
// (h4p_enable): off = Halo 4's own physics, the dll stays loaded doing nothing until MCC closes (it cannot be unloaded
// safely while the game thread runs through its hooks). Halo 4 only.
class Halo4Halo2Physics : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	Halo4Halo2Physics(GameState gameImpl, IDIContainer& dicon);
	~Halo4Halo2Physics();

	std::string_view getName() override { return nameof(Halo4Halo2Physics); }
};
