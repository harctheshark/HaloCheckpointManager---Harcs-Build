#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Metro Redux (2026-11-24): "Display Player Info" - position and speed in Halo world units (1 wu = 3.048 m, Z up) and in
// metres, health and stamina, the level's name. Reuses the 2D info display's settings (toggle, corner, font, colour) like
// Halo 5's. Speed is the change of position over real time (the controller's own velocity is not located yet).
class MetroDisplayInfo : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MetroDisplayInfo(GameState game, IDIContainer& dicon);
	~MetroDisplayInfo();
	std::string_view getName() override { return nameof(MetroDisplayInfo); }
};
