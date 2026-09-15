#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// HAVOK BROADPHASE DELETION BYPASS (Halo 5: Forge) - fly out of bounds forever without being removed.
//
// ⚠⚠ THERE ARE **TWO** INDEPENDENT KILLERS OUT THERE, and disabling either one alone is not enough. They live
// in different subsystems, fire on different timers and have completely different mechanisms. Both were read
// out of the engine rather than inferred.
//
// ---------------------------------------------------------------------------------------------------------------
// KILLER 1 - "went outside of the world"  (blofeld/game/players.cpp, exe+0x0133E3C0)
// ---------------------------------------------------------------------------------------------------------------
// The per-player update tests bit 13 of the object's flags word and counts consecutive ticks spent outside:
//
//     exe+0x0133E4B7   mov  ecx, [object + 0x18]
//     exe+0x0133E4BA   shr  rcx, 0xd
//     exe+0x0133E4BE   test cl, 1                  ; bit 13 = "outside the world"
//     exe+0x0133E4C1   je   0x0133E52D             ; inside -> reset the counter to 0
//     exe+0x0133E4C3   inc  byte [player + 0x89]   ; outside -> ++counter
//     exe+0x0133E4C9   movss xmm0, [exe+0x03309314]  ; 0.5 seconds
//     exe+0x0133E4DD   cmp  ebx, eax               ; counter vs ticks(0.5s)
//     exe+0x0133E4DF   jl   0x0133E534             ; not long enough yet
//                      ... player killed, logs "players: player %d went outside of the world. Now he is dead."
//
// So this one is pure data and needs NO code patch: we write 0 to `player + 0x89` every tick. The counter can
// never reach the threshold, so the branch is never taken. This is the same shape as H5Invincibility - starve
// the condition rather than patch the consequence. `player` here is exactly what
// H5GetPlayerState::getPlayerArray() returns; the engine reads the object datum from `player + 0x24` in that
// very function, which is the field that getPlayerDatum() reads.
//
// ---------------------------------------------------------------------------------------------------------------
// KILLER 2 - the actual Havok broadphase exit  (blofeld/physics/havok.cpp, exe+0x014CC6F0)
// ---------------------------------------------------------------------------------------------------------------
// When a rigid body leaves the broadphase AABB, Havok raises an exit event and the engine walks the list of
// exited bodies and DELETES the owning object, logging
//     "rigid body has exited broadphase at (%f %f %f).  The associated object has been deleted. %s"
//
//     exe+0x014CC91B   mov  ecx, [rsp + 0x50]      ; the object datum
//     exe+0x014CC91F   call 0x028258F0             ; <- the delete
//     exe+0x014CC924   add  rdi, 4                 ; next exited body
//
// This one cannot be starved from data. The engine DOES have an exemption immediately above
// (exe+0x014CC887 calls exe+0x020EEF70 and skips the body entirely when it returns true) but that predicate
// only passes for Forge/map-variant placed objects - it requires `object + 0x104 != -1` - so it is no use for
// the player. We therefore NOP the 5-byte call. The exit event still fires and still logs; the object simply
// survives it.
//
// ⚠ THE PATCH IS FOUND BY SIGNATURE, NOT BY RVA, so a game update cannot silently turn it into a write into
// the middle of some unrelated function:
//     8B 4C 24 50  E8 ?? ?? ?? ??  48 83 C7 04  49 3B FF  0F 85
// Exactly one match in the image. The call displacement is wildcarded. We additionally re-verify the five
// bytes we are about to overwrite really are `E8 CC 8F 35 01`-shaped (an E8 call) before touching anything,
// and we restore the ORIGINAL bytes we read - never a hardcoded copy.
//
// ⚠ LEAVING THE BROADPHASE IS STILL A REAL PHYSICS STATE. The body is outside the broadphase, so it collides
// with nothing out there. That is exactly what "fly OOB forever" wants, but it does mean the world will not
// catch you again until you come back inside.
// ================================================================================================================
class H5OutOfBoundsBypass : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5OutOfBoundsBypass(GameState game, IDIContainer& dicon);
	~H5OutOfBoundsBypass();
	std::string_view getName() override { return nameof(H5OutOfBoundsBypass); }
};
