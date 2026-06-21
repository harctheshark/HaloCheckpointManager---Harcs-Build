#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IMasterTickrateImpl { public: virtual ~IMasterTickrateImpl() = default; };

// Halo 2 only. A button that flips the master simulation tickrate between 60 and 30 Hz (and the matching
// seconds-per-tick), writing the game-time-globals tickrate (int16) and dt (float).
class MasterTickrate : public IOptionalCheat
{
private:
	std::unique_ptr<IMasterTickrateImpl> pimpl;

public:
	MasterTickrate(GameState gameImpl, IDIContainer& dicon);
	~MasterTickrate();

	std::string_view getName() override { return nameof(MasterTickrate); }
};
