#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// FORCE TELEPORT for Halo 5: Forge.
//
// The hard part already exists: H5GetPlayerState::teleportPlayerTo calls the engine's own teleport worker
// (exe+0x0088FC10) rather than writing coordinates anywhere. See the contract on that declaration - in
// particular r8 is a FORWARD DIRECTION, not a second position, and arg6 must be 0 or the vector in r8 is
// written into the unit's facing fields.
//
// ⚠⚠ WHY THE ENGINE CALL AND NOT A POSITION WRITE.
//
// Writing obj+0x224 directly "works" for one frame and then snaps back - observed as "my camera flashed for
// 1 frame, but im still on the ground". obj+0x224 is the PUBLISHED position, not the authority.
//
// ⚠ CORRECTION (the previous version of this comment got this wrong, and the error is dangerous):
// the OTHER user report - "i was desynced and camera flashed between where i was and the TPed location, then
// the game straight up killed me" - does NOT belong to obj+0x224. It belongs to writing the HAVOK MOTION
// STATE translation, from the game thread, single-writer (mshold.py up 100). That is one level BELOW
// obj+0x224, and it is the exact thing an author would try next after reading "obj+0x224 is only published
// output". It has already been tried and it killed the player.
//
// So: do not read this comment as "go one level deeper". Both levels have been tried. The reason the deeper
// write fails is that the collision proxy / character controller which actually holds the player has never
// been located, so moving the transform only desynchronises the player from whatever still owns them.
// See the memory note halo5-havok-motion-state-chain for the full record and the read-only work that would
// have to come back clean first.
//
// Deliberately reuses forceTeleportEvent, RebindableHotkeyEnum::forceTeleport and the existing
// forceTeleport* settings instead of introducing Halo-5-specific copies - the same approach the HaloCER and
// Halo 5 checkpoint/revert cheats take. Only one title exists in a process at a time, so exactly one
// listener is ever constructed.
class H5ForceTeleport : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
public:
	H5ForceTeleport(GameState game, IDIContainer& dicon);
	~H5ForceTeleport();
	std::string_view getName() override { return nameof(H5ForceTeleport); }
};
