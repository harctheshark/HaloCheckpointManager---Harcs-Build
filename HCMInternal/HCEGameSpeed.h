#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// GAME SPEED - scales Halo Campaign Evolved's SIMULATION by writing the engine's own speed field.
//
// WHY THIS EXISTS INSTEAD OF THE SPEEDHACK
// ----------------------------------------
// HCM's general Speedhack works by inline-hooking QueryPerformanceCounter, GetTickCount, GetTickCount64 and
// timeGetTime and returning a fabricated clock. That is the only option on titles where nothing better is
// exposed, but it is a bad deal on this one:
//
//   * It is PROCESS-WIDE. Every caller gets the fake clock - the graphics driver, the frame-generation pacer,
//     the audio stack, the Steam overlay, HCM's own UI.
//   * Those four exports are the cheapest calls in Windows. GetTickCount and GetTickCount64 are plain reads of
//     KUSER_SHARED_DATA with no syscall at all. Hooking them replaces a few nanoseconds with a trampoline, a
//     call through to the original, a seqlock read and a double multiply - and UE5's task graph calls them
//     constantly, from every worker.
//   * MEASURED on this title: the game sat at 15.6 cores with 73% of that in KERNEL time and 633,000 context
//     switches per second. (HCMSpeedhack now installs its hooks lazily for that reason, so the cost is only
//     paid by someone who actually uses it - but on HaloCER there is no reason to pay it at all.)
//
// THE ENGINE ALREADY HAS THE KNOB
// -------------------------------
// s_game_time_globals is a 0x3C-byte struct reached through the sim module's TLS slot +0x98:
//
//     +0x06 int16  ticks_per_second     60
//     +0x08 float  seconds_per_tick     1/60
//     +0x0C        tick counter          <- what game_tick_get returns
//     +0x10 float  speed                1.0   <- THIS
//     +0x14 float  leftover_ticks       the fixed-timestep accumulator's fraction
//     +0x18 float  ramp_elapsed          \
//     +0x1C float  ramp_duration          |  a scripted time-scale ramp
//     +0x20 float  ramp_from              |
//     +0x24 float  ramp_to               /
//
// main_game_time_update (sub_180210050) is a fixed-timestep accumulator that does, at 0x180210315:
//     vmulss     xmm7, xmm8, [rcx+10h]        ; dt * speed
//     vfmadd213ss xmm6, xmm7, [rcx+14h]       ; acc = rate*(dt*speed) + leftover
// so `speed` is exactly a simulation-rate multiplier, and the engine writes that same field itself at
// 0x1802107B2 (`P.speed = 60 / new_rate`). This is a supported path, not a poke at a spare float.
//
// ⚠ SPEED, NOT TICKS_PER_SECOND. Changing the RATE would desync HaloScript: its time unit is 30 Hz by
// convention, and game_ticks_from_seconds / game_seconds_from_ticks hardcode 30.0f and never consult the live
// rate. Changing SPEED leaves every tick nominally 1/60 s and only changes how quickly real time is converted
// into ticks, so script timing stays consistent with itself.
//
// ⚠ THE SCRIPTED RAMP WILL FIGHT US. At 0x18021010C the update checks `ramp_duration > 0` and, while it is,
// recomputes speed by interpolating ramp_from -> ramp_to. A scripted slow-motion effect would therefore stomp
// our value on the next tick. We zero ramp_duration alongside every write, which means a scripted ramp is
// suppressed while Game Speed is on - a deliberate trade, and the reason the write is re-applied EVERY FRAME
// from the game thread rather than once on toggle.
//
// ⚠ THE ENGINE CLAMPS ITS CATCH-UP BUDGET. At 0x1802102C8 the per-frame tick budget is
// min(rate*5, 4*clamp(speed,1,5)), so past roughly 5x the simulation simply stops keeping up rather than
// going faster. Values above that are accepted but will not deliver a proportional speedup.
//
// ⚠ ONLY THE SIMULATION SCALES. Rendering, presentation and the frame pacer keep real time. That is usually
// what is wanted for practice and routing, and it is a real behavioural difference from the Speedhack, which
// drags everything along with it.
// ================================================================================================================

class HCEGameSpeed : public IOptionalCheat
{
private:
	class HCEGameSpeedImpl;
	std::unique_ptr<HCEGameSpeedImpl> pimpl;

public:
	HCEGameSpeed(GameState game, IDIContainer& dicon);
	~HCEGameSpeed();
	virtual std::string_view getName() override { return nameof(HCEGameSpeed); }
};
