#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. Teleports the player to absolute world coordinates.
//
// SCOPE NOTE: unlike MCC's ForceTeleport there is no "relative to player look direction" mode and no
// "apply to custom object" mode. HCE exposes no player view angle through any chain the reference tool
// reversed (HCM_Evolved's own teleport is absolute-coordinates only), and there is no object-datum path either.
// Shipping a relative mode would mean guessing at a forward vector, which is worse than not shipping one.
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
