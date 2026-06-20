#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IFarClipDistanceImpl { public: virtual ~IFarClipDistanceImpl() = default; };

// Halo 2 only. Writes the renderer far-clip-distance float in game memory from a UI value, and syncs the
// UI to the live value when a game loads so the +/- 512 buttons step from the real current distance.
class FarClipDistance : public IOptionalCheat
{
private:
	std::unique_ptr<IFarClipDistanceImpl> pimpl;

public:
	FarClipDistance(GameState gameImpl, IDIContainer& dicon);
	~FarClipDistance();

	std::string_view getName() override { return nameof(FarClipDistance); }
};
