#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IUncapVisibilityLimitsImpl { public: virtual ~IUncapVisibilityLimitsImpl() = default; };

// Halo 2 only. Raises three of the per-frame "visibility index collection" caps that decide how many things
// can be drawn at once: visible objects, instanced geometry, and building (BSP) meshes. Stock halo2.dll
// sizes these collections at 256 / 128 / 384 (camera collection) via packed maximums in the visibility init
// (sub_180709F90); on large maps these overflow ("overflowed visibility index collection count") and chunks
// of the level stop rendering. We bump each to 4096 (the hard ceiling is 32765). The 4th cap in that init -
// clusters-per-region (128) - is a CHAR_MAX/region-struct limit and is NOT touched here.
// The collections are built at init, so this is "armed": it takes effect on the next level load / MCC
// restart. Fully reverted on disable. Build 1.3528 only.
class UncapVisibilityLimits : public IOptionalCheat
{
private:
	std::unique_ptr<IUncapVisibilityLimitsImpl> pimpl;

public:
	UncapVisibilityLimits(GameState gameImpl, IDIContainer& dicon);
	~UncapVisibilityLimits();

	std::string_view getName() override { return nameof(UncapVisibilityLimits); }
};
