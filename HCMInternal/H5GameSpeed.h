#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// GAME SPEED (Halo 5: Forge) - scales the SIMULATION by writing the engine's own speed field.
//
// Same approach as HCEGameSpeed, and for the same reason: HCM's general Speedhack fakes the process-wide clock
// by hooking QueryPerformanceCounter / GetTickCount / GetTickCount64 / timeGetTime, which drags the graphics
// driver, the audio stack, the Steam overlay and HCM's own UI along with it. Halo 5 exposes the real knob, so
// there is no reason to pay that.
//
// THE STRUCT
// ----------
// The game time globals hang off the simulation thread's TLS block at +0x1538:
//
//     G = *(uintptr_t*)(tls + 0x1538)
//
//     G + 0x04  int16   ticks_per_second
//     G + 0x10  float   speed              1.0   <- THIS
//     G + 0x20  float   ramp_elapsed        \
//     G + 0x24  float   ramp_duration        |   a scripted time-scale ramp
//     G + 0x28  float   ramp_from            |
//     G + 0x2C  float   ramp_to             /
//
// Established by reading the engine, not by sampling:
//   * exe+0x0131AD32 `movss [rcx+0x10], xmm0` - the engine writes this field itself, through the same
//     TLS+0x1538 chain.
//   * exe+0x0131AE0A reads it and gates on `> 0`, then exe+0x0131AE6B re-reads it, does `maxss` with 1.0
//     (exe+0x03308174) and compares against 15.0 (exe+0x033200BC) - the per-frame catch-up budget clamp.
//   * exe+0x0131ABA0 is the engine's own set_game_speed(float): it clamps against a small epsilon at
//     exe+0x03317A68 and stores.
//
// ⚠ THE SCRIPTED RAMP WILL FIGHT US. At exe+0x0131AD65 the update checks `ramp_duration > 0` and, while it is,
// recomputes speed as (1-t)*ramp_from + t*ramp_to with t = ramp_elapsed / ramp_duration, then calls the setter
// (exe+0x0131AD99..0x0131ADB3). A scripted slow-motion effect would therefore stomp our value on the next tick.
// We zero ramp_duration alongside every write, so a scripted ramp is SUPPRESSED while Game Speed is on. That is
// a deliberate trade and the reason the write is re-applied every tick rather than once on toggle.
//
// ⚠ THE ENGINE CLAMPS ITS CATCH-UP BUDGET AT 15. Past that the simulation stops keeping up rather than going
// faster, so higher values are accepted but will not deliver a proportional speedup.
//
// ⚠ ONLY THE SIMULATION SCALES. Rendering, presentation and the frame pacer keep real time - which is normally
// what is wanted for practice and routing, and is a real behavioural difference from the Speedhack.
//
// ⚠ RESTORE IS A PLAIN WRITE OF 1.0, NOT A SAVED VALUE. Caching "what it was" and restoring that would re-apply
// a stale speed over whatever the engine set while we were on. 1.0 is the engine's own default and the value it
// writes at level load.
// ================================================================================================================
class H5GameSpeed : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5GameSpeed(GameState game, IDIContainer& dicon);
	~H5GameSpeed();
	std::string_view getName() override { return nameof(H5GameSpeed); }
};
