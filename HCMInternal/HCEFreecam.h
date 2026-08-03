#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved ONLY. The engine already owns a free camera; HCM just flips its byte.
//
// SCOPE NOTE - this is deliberately NOT the MCC FreeCamera cheat. That one is 822 lines templated per game and
// hard-requires GetGameCameraData, GetPlayerViewAngle, UpdateGameCameraData, UserCameraInputReader, Speedhack and
// FreeCameraFOVOverride, plus a midhook on the engine's camera-write function so it can drive the camera matrix
// every frame. None of that exists for HCE, and reversing HCE's camera write path is far more work than the
// reference tool provides. HCE's freecam is one byte - *( *(tls + 0xB8) + 0x9C8 ) = 1 - after which the ENGINE
// flies the camera itself. So this ships the reference tool's two features and nothing more: the toggle, and
// "teleport the player to wherever the camera ended up".
//
// The toggle byte lives in the game-state TLS block, so a checkpoint revert clears it; there is a throttled
// re-assert, same as Freeze AI and the skulls.
// ================================================================================================================
class HCEFreecam : public IOptionalCheat
{
private:
	class HCEFreecamImpl;
	std::unique_ptr<HCEFreecamImpl> pimpl;

public:
	HCEFreecam(GameState game, IDIContainer& dicon);
	~HCEFreecam();
	std::string_view getName() override { return nameof(HCEFreecam); }
};
