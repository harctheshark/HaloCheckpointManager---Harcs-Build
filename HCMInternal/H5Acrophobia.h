#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ACROPHOBIA (boots off the ground) for Halo 5: Forge.
//
// The Halo 1 / CER flight model, applied every frame to the character controller's velocity while jump is
// held. Same write as Force Launch (proxy+0x40) - the only new parts are the per-frame driver and the math.
//
// The model, verbatim from the CER decode:
//     b = aim (unit)                 t = max(0, dot(b, v))
//     s = clamp(t * 0.0625, 0, 1)    a = 9(1-s)(1+2s)
//     steer:  v -= 0.2 * (v - b*t)       remove the component off the aim axis
//     thrust: v += a*dt * b              accelerate along it
// ⚠ The polynomial IS the speed limiter - it reaches zero at t = 16 wu/s, so there is deliberately no
// clamp. Adding one would change the feel of a model that is otherwise bit-for-bit the original.
//
// Hold JUMP to fly along your aim; hold CROUCH to hover (the idle branch scales velocity toward rest,
// which kills the fall as well as the drift). Jump wins when both are held.
//
// ⚠⚠ INPUT IS READ THROUGH IMGUI, which is the only source that has BOTH keyboard and controller.
// Two separate traps sit behind that:
//   * GetAsyncKeyState returns 0x0000 for every key inside the game's AppContainer, so the obvious
//     approach reads nothing at all. HCMExternal polls the real keyboard and forwards it via shared
//     memory, and HCM feeds that into ImGui.
//   * reading that forwarded array directly is KEYBOARD ONLY - the first version did, and Acrophobia
//     simply ignored a controller. The gamepad arrives separately, from HCM's XInput polling into the
//     ImGuiKey_Gamepad* keys.
// ImGui is where the two meet, and this cheat's per-frame callback runs inside the ImGui frame, so
// ImGui::IsKeyDown is valid there.
class H5Acrophobia : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
public:
	H5Acrophobia(GameState game, IDIContainer& dicon);
	~H5Acrophobia();
	std::string_view getName() override { return nameof(H5Acrophobia); }
};
