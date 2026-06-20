#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IRocketLauncherAnimationFixImpl { public: virtual ~IRocketLauncherAnimationFixImpl() = default; };

// Halo 2 only. Scans decompressed tag data (writable, non-executable heap) for a known animation-tag header
// signature and patches a table of offsets to fix the first-person rocket launcher animation pop. Re-applies
// on a background timer because the tag data is re-created on map/BSP loads. Reverts the patches when disabled.
class RocketLauncherAnimationFix : public IOptionalCheat
{
private:
	std::unique_ptr<IRocketLauncherAnimationFixImpl> pimpl;

public:
	RocketLauncherAnimationFix(GameState gameImpl, IDIContainer& dicon);
	~RocketLauncherAnimationFix();

	std::string_view getName() override { return nameof(RocketLauncherAnimationFix); }
};
