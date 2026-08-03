#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY (GameState::Value::HaloCER).
//
// Owns the three code hooks that Halo Campaign Evolved needs for checkpoint control, and drives BOTH
// "Force Checkpoint" (settings->forceCheckpointEvent) and "Disable Natural Checkpoints"
// (settings->naturalCheckpointDisable) from them.
//
// The two features share one class because they share a hook site: the save_checkpoint call. Two separate
// cheats would both want to own the bytes at that address, and whichever attached second would either fail
// or corrupt the other. See HCECheckpointDetours.cpp for the full detour description.
class HCECheckpointDetours : public IOptionalCheat
{
private:
	class HCECheckpointDetoursImpl;
	std::unique_ptr<HCECheckpointDetoursImpl> pimpl;

public:
	HCECheckpointDetours(GameState gameImpl, IDIContainer& dicon);
	~HCECheckpointDetours();

	std::string_view getName() override { return nameof(HCECheckpointDetours); }
};
