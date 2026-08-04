#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) "Disable Barriers" - soft ceilings.
//
// Unlike MCC's DisableBarriers (a 2-byte EB jump patch over the barrier collision check), this patches NO code.
// HCE keeps a per-soft-ceiling enable BITMASK in the game state, one bit per scenario soft ceiling, and the
// engine's own soft_ceiling_enable HaloScript function does nothing but flip bits in it. Zero the mask and every
// soft ceiling stops existing, through the engine's own supported path.
//
// Reuses the MCC disableBarriersToggle setting and hotkey - HaloCER and the MCC games can never share a process.
// ================================================================================================================
class HCEDisableBarriers : public IOptionalCheat
{
private:
	class HCEDisableBarriersImpl;
	std::unique_ptr<HCEDisableBarriersImpl> pimpl;

public:
	HCEDisableBarriers(GameState game, IDIContainer& dicon);
	~HCEDisableBarriers();
	std::string_view getName() override { return nameof(HCEDisableBarriers); }
};
