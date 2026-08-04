#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Teleports the player, either to absolute world coordinates or by a
// forward/right/up offset relative to their look direction - the same two modes MCC's ForceTeleport offers.
//
// SCOPE NOTE: player only. MCC additionally offers "apply to custom object", which needs a validated
// datum -> object physics resolver; HCE has none, and the eight-write teleport sequence was reversed against
// the player biped specifically.
//
// teleportPlayerTo is public so HCEFreecam can reuse it, mirroring how MCC's FreeCamera calls ForceTeleport.
class HCEForceTeleport : public IOptionalCheat
{
private:
	class HCEForceTeleportImpl;
	std::unique_ptr<HCEForceTeleportImpl> pimpl;

public:
	HCEForceTeleport(GameState game, IDIContainer& dicon);
	~HCEForceTeleport();
	std::string_view getName() override { return nameof(HCEForceTeleport); }

	void teleportPlayerTo(SimpleMath::Vector3 position); // throws HCMRuntimeException
};
