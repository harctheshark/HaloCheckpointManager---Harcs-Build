#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - FREE CAMERA + TELEPORT TO CAMERA.
//
// ★ THIS DRIVES THE ENGINE'S OWN FREE CAMERA. Halo 5 already has one: the director supports 15 camera modes
// and mode 2 is "flying" (the name table is at exe+0x046EC560, and the mode-switch jump table at
// exe+0x006E8F6C has exactly 15 entries in that order). So HCM does not fight the camera system - it asks
// the camera to become the class the engine already uses for a flycam, and then owns its fields.
//
// WHY THIS NEEDS NO ENGINE CALL
// Mode switching in this engine is placement-new: c_director::set_camera_mode (exe+0x006E8C20) overwrites
// the vtable pointer and re-initialises fields in place, and calls NO destructor on the outgoing camera in
// any of the 15 cases. So writing the flying vtable plus its fields IS the mode switch. That matters here:
// in this title every pure memory write has worked and every engine call has faulted.
//
//     simTLS    = the sim thread's TLS block, preferring [simTLS+0x24]&1
//     dirBlock  = [simTLS+0x198]              director = dirBlock + userIndex*0x1E8
//     camera    = director + 0x08             observer = [simTLS+0x1B0] + watchedIdx*0x4A0
//
//     camera+0x00 vtable   (flying = exe+0x0333CEA0, first person = exe+0x0333CD20)
//     camera+0x18 pos[3]   +0x24 yaw   +0x28 pitch   +0x2C prevPos[3]  +0x38/+0x3C prev angles
//     camera+0x40 collision sweep radius   +0x44/+0x48 input ramps   +0x4C flags
//
// ⚠⚠ WITH flags = 0 THE ENGINE LEAVES POSITION AND ANGLES ALONE. The update (exe+0x006EC140) loads its
// working position from +0x18 BEFORE the movement (bit 4) and collision (bit 2) blocks, and both are
// skipped when their bit is clear, so the only net effect of a tick is copying current -> prev. That is
// what makes this race-free: HCM writes the fields, the engine publishes them.
// Use 0x1C instead for the engine's NATIVE flycam (gamepad control + collision), or 0x18 for native
// control with noclip.
//
// ⚠⚠⚠ WRITE ORDER IS LOAD-BEARING AND NOT OBVIOUS. camera+0x18..+0x24 are ALSO the live FIRST-PERSON
// camera's prevFOV / yaw / pitch / control byte - they are NOT spare space, and writing them early makes
// the current camera jump for a frame. So: write the genuinely-inert tail (+0x28..+0x4C) first, then the
// 16 bytes at +0x18 as late as possible, then flip the vtable last.
//
// ⚠⚠ THE VTABLE POINTER IS NOT 8-BYTE ALIGNED (camera & 7 == 4), so an 8-byte store to it is only atomic
// if it does not straddle a 64-byte cache line. We refuse to enable when (camera & 63) > 56.
//
// ⚠ A LUA SCRIPT CAN TAKE THE CAMERA BACK AT ANY TIME. set_camera_mode has a script-facing caller
// (camera_set_mode, exe+0x006E7BD0) and camera_set_flying_cam_at_point calls the flying ctor directly, so
// free camera can be dropped with no watched-player change. We re-check the vtable every tick.
//
// ⚠ NOCLIP IS CONDITIONAL. Clearing bit 2 does not unconditionally disable the collision sweep: the real
// gate at exe+0x006EC8DC..0x006EC8EE force-enters the sweep when the predicate at exe+0x0135F0A0 is true,
// whatever the flag says. That predicate reads false in normal play; we evaluate it read-only and report
// rather than promising noclip we cannot deliver.
//
// ⚠ NOT YET VERIFIED IN GAME. Everything above is static disassembly plus read-only live confirmation.
// Whether flipping this vtable from an HCM thread actually steers the RENDERED view is the one thing no
// amount of static analysis can settle - it is the first thing to check.
// ================================================================================================================
class H5FreeCamera : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5FreeCamera(GameState game, IDIContainer& dicon);
	~H5FreeCamera();
	std::string_view getName() override { return nameof(H5FreeCamera); }
};
