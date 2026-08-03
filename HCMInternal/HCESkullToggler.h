#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Owns all memory access for the 56-bit HCE skull bitfield at
// *(tls + 0x60) + 0x1EBE0, and re-applies the user's checked skulls after a checkpoint revert wipes them.
// The widget (GUIHCESkullToggle) only reads/writes the shared state in SettingsStateAndEvents.
class HCESkullToggler : public IOptionalCheat
{
private:
	class HCESkullTogglerImpl;
	std::unique_ptr<HCESkullTogglerImpl> pimpl;

public:
	HCESkullToggler(GameState game, IDIContainer& dicon);
	~HCESkullToggler();
	std::string_view getName() override { return nameof(HCESkullToggler); }
};
