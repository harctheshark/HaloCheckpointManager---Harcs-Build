#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - INVINCIBILITY.
//
// Sets the engine's OWN per-object damage flags rather than rewriting health, so nothing has to fight the
// damage code and there is no per-frame value to keep resetting.
//
//     object + 0x138   u32 damage flags
//         bit  7  (0x00000080)  cannot_take_damage   - script fn `object_cannot_take_damage`
//         bit 20  (0x00100000)  cannot_die           - script fn `object_cannot_die`
//         bit 21  (0x00200000)  cannot_die EXCEPT kill volumes  ⚠ deliberately NOT used - it lets kill
//                                                      volumes through, which is the opposite of what we want
//         bit  2  (0x00000004)  already dead         ⚠ NEVER touch; it makes the health/shield getters
//                                                      return 0 and the engine's own setters no-op
//
// ⚠⚠⚠ BOTH BITS ARE REQUIRED - bit 7 ALONE DOES NOT MAKE YOU UNKILLABLE.
// Bit 7 short-circuits the damage applier (exe+0x028D87B0 tests it at +0x028D8BF0 and jumps past the whole
// damage block), and via the veto at exe+0x028D81F0 it also stops the kill call *inside* that funnel. But
// the kill routine exe+0x028D9AE0 has NINE callers and only one of them sits behind that gate. It never
// reads bit 7 at all - it consults the predicate at exe+0x02828A40, which is vetoed by bit 20. So
// `unit_kill` and friends (exe+0x02858A60 -> 0x02806D10 -> 0x0285CEF0 -> 0x028D9AE0) walk straight past
// bit 7. Shipping bit 7 alone leaves the player killable by scripted kills and, near-certainly, by kill
// volumes. The mask is 0x00100080.
//
// ⚠⚠ THE FLAG IS LOST ON RESPAWN, so this re-applies on a tick rather than writing once:
//   * exe+0x0285CBFA (`mov dword [r14+0x138], ebx` with ebx = 0) zeroes the whole word on a vitality reset,
//     reached from the revive/respawn wrapper at exe+0x0282CC30;
//   * exe+0x028839D8 clears it on death/despawn.
//
// ⚠⚠ ALWAYS READ-MODIFY-WRITE, AND RE-READ ON DISABLE. Never cache the word at enable time and restore it
// later: the engine zeroes the whole word on respawn, including bit 28 which is live on the player, so a
// stale write-back would resurrect a bit the engine had just cleared.
//
// ⚠ The read-modify-write cannot be atomic against the simulation thread, which performs its own or/and on
// this same dword (exe+0x0285E966, +0x0285E972, +0x0285AAEB, +0x028839D8). The window is narrow, losing a
// bit is not fatal, and re-applying every tick is self-healing - but this is not claimed to be atomic.
//
// ⚠ Unlike the engine's own setters (exe+0x0285AAD0 / +0x0285E8F0), this does not propagate the bits to the
// chained object at [obj+0x2C]. Irrelevant for keeping the player alive; relevant if anything attached to
// the player is ever expected to be protected too.
//
// ⚠ NOT YET VERIFIED IN GAME. Static control flow plus read-only live confirmation only - the object and
// the flags word were read live (0x10000000 on the player), but no write has been performed. Falling damage
// has its own system (`unit_falling_damage_disable`, exe+0x02803700) that was NOT traced, and the
// multiplayer/host-authoritative path is untested. Treat as campaign/Forge until proven otherwise.
// ================================================================================================================
class H5Invincibility : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5Invincibility(GameState game, IDIContainer& dicon);
	~H5Invincibility();
	std::string_view getName() override { return nameof(H5Invincibility); }
};
