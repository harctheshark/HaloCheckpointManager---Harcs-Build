#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// FORCE LAUNCH for Halo 5: Forge.
//
// Writes the character controller's velocity (proxy+0x40) - the same structure Force Teleport writes the
// position of. One 12-byte store, no engine call, no pump, no code patch.
//
// ⚠⚠ DO NOT WRITE obj+0x248 INSTEAD. It looks like the velocity and even reads back plausibly, but it is
// a PUBLISHED MIRROR: forced to (0,0,8) it held that value for three seconds while the player stood
// perfectly still, because nothing in the engine reads it. That dead end cost a full debugging cycle and
// is the origin of the report "All this is doing is teleporting me, im not 'moving' with velocity".
//
// The real field was found by scoring every float triple in the proxy element against measured
// d(position)/dt over 52.68 wu of walking - +0x40 scored 0.150, everything else 1.000 - and confirmed by
// writing (0,0,12), which produced a textbook ballistic arc: 12.7 wu of rise with gravity decaying the
// velocity to zero and then negative, no crash and no desync.
//
// Reuses forceLaunchEvent and the existing forceLaunch* settings rather than adding parallel Halo-5 ones,
// exactly as the Halo 5 checkpoint / revert / teleport cheats do.
class H5ForceLaunch : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
public:
	H5ForceLaunch(GameState game, IDIContainer& dicon);
	~H5ForceLaunch();
	std::string_view getName() override { return nameof(H5ForceLaunch); }
};
