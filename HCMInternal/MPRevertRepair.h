#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 3 multiplayer (2026-11-05): after a checkpoint revert, repairs the network simulation's entity bookkeeping so a revert
// past an object that spawned and was then deleted no longer crashes the game. See MPRevertRepair.cpp.
class MPRevertRepair : public IOptionalCheat
{
private:
	class MPRevertRepairImpl;
	std::unique_ptr<MPRevertRepairImpl> pimpl;

public:
	MPRevertRepair(GameState gameImpl, IDIContainer& dicon);
	~MPRevertRepair();

	virtual std::string_view getName() override { return nameof(MPRevertRepair); }
};
