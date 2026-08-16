#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved ONLY - CAMERA ROLL (banking the free camera about its own forward axis).
//
// ⚠ THIS IS NOT "Gimbal Lock Bypass". That feature already exists (HCEFreecamExtras) and does something else
// entirely: it removes the camera's PITCH CLAMP by patching four code sites. This one writes a single float and
// patches nothing.
//
// WHERE THE NUMBER COMES FROM (established, do not re-derive):
//
//   * The camera entry is the one HCEGetPlayerState::getActiveCameraEntry() already returns - the first non-null
//     of (*(tls + 0x148) + slot * 0x1AC) over slots 0..3.
//   * Its layout: +0x20 position (3 floats, the same three Freecam's "teleport to camera" reads), +0x2C yaw
//     (radians), +0x30 pitch (radians), +0x34 ROLL (radians), +0x40 flags.
//   * Proof for roll specifically: sim rva 0x3A5D40 is a two-instruction setter, "vmovss [rcx+0x2C], xmm1 ; ret",
//     reached with this == E + 8 - i.e. it writes E + 0x34. Nothing on the per-frame camera update path writes
//     roll at all, which is exactly why a written value STAYS written.
//
// ⚠ A CAMERA-MODE CHANGE ZEROES IT. Entering a cutscene, or toggling freecam off and on, resets the field, so the
// value has to be re-asserted. That is done on the same throttled 250 ms cadence HCEFreecam::onRenderEvent uses to
// re-assert the freecam toggle itself, and for the same reason.
//
// SCOPE: the roll is applied ONLY while the engine's own free camera is on (the +0x9C8 master byte reads 1).
// Writing it during normal play would tilt the player's view with nothing to level it back out, and the engine
// would fight us every frame anyway.
//
// INPUT: the roll can be driven from the slider or from cameraRollLeftBinding / cameraRollRightBinding - the
// hotkeys MCC's free camera already uses. They are polled HERE rather than through UserCameraInputReader, which
// is MCC-only, templated per MCC game, and hard-depends on analog-input pointer data that HaloCER does not have.
// Polling two already-defined bindings costs nothing and leaves every line of shared MCC input code untouched.
// ================================================================================================================
class HCECameraRoll : public IOptionalCheat
{
private:
	class HCECameraRollImpl;
	std::unique_ptr<HCECameraRollImpl> pimpl;

public:
	HCECameraRoll(GameState game, IDIContainer& dicon);
	~HCECameraRoll();
	std::string_view getName() override { return nameof(HCECameraRoll); }
};
