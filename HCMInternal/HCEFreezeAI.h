#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Writes the game thread's TLS ai_enabled byte (HCM_Evolved gameplay.py:
// toggle_ai_freeze). 0 = AI frozen, 1 = AI running - note the sense is inverted relative to the "Freeze AI" label.
//
// A checkpoint revert restores the value, so this also re-asserts the byte on a throttled render callback,
// which is what the reference tool's maintain_enabled_toggles does at 250 ms.
class HCEFreezeAI : public IOptionalCheat
{
private:
	class HCEFreezeAIImpl;
	std::unique_ptr<HCEFreezeAIImpl> pimpl;

public:
	HCEFreezeAI(GameState game, IDIContainer& dicon);
	~HCEFreezeAI();
	std::string_view getName() override { return nameof(HCEFreezeAI); }
};
