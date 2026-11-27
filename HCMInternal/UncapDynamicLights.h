#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IUncapDynamicLightsImpl { public: virtual ~IUncapDynamicLightsImpl() = default; };

// Halo 2 only (halo2.dll 1.3528). Stock, only the 3 most important dynamic lights of a view are allowed to shine: the game
// gathers up to 128 visible lights a frame, keeps the best 32 in a per-view fade list and lets just the first 3 of that list
// fade in. This moves the lights to HCM's own 128-entry lists (H2FadeListHooks) and lets "Max Lights" of them shine. See the .cpp.
class UncapDynamicLights : public IOptionalCheat
{
private:
	std::unique_ptr<IUncapDynamicLightsImpl> pimpl;

public:
	UncapDynamicLights(GameState gameImpl, IDIContainer& dicon);
	~UncapDynamicLights();

	std::string_view getName() override { return nameof(UncapDynamicLights); }
};
