#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 5: Forge checkpoint control.
//
// Both of these deliberately reuse the EXISTING forceCheckpointEvent / forceRevertEvent settings (and so the
// existing hotkeys), exactly as the HaloCER equivalents do: the three titles can never coexist in one process,
// so there is only ever one listener per event.

// FORCE CHECKPOINT. A pure data write - no engine call, nothing to deadlock:
//     *(uint32*)(*(tls + 0x15A8)) = mode
// The engine consumes the request and zeroes the field, which is how we know it landed.
class H5ForceCheckpoint : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
public:
	H5ForceCheckpoint(GameState game, IDIContainer& dicon);
	~H5ForceCheckpoint();
	std::string_view getName() override { return nameof(H5ForceCheckpoint); }
};

// FORCE REVERT. Two static writes, IN THIS ORDER - the gate first:
//     [exe + 0x50B0251] |= 0x80     the gate
//     [exe + 0x50B03AC] |= 0x0C     the request
//
// ⚠⚠ THE GATE IS NOT OPTIONAL AND ITS ORDER MATTERS. It is the engine's own non-client path: game_revert
// calls an is-distributed-client check and takes this branch only when it is the simulation authority.
// Measured 2/2 with the gate set first, 0/2 without it - the request is simply never consumed.
//
// ⚠⚠⚠ CAMPAIGN ONLY. In a distributed-client session this tears the player down with nothing to restore.
// Guarded by H5GetPlayerState::isLocalSimulation() (the engine's own kind string at exe+0x5A23920 reading
// "local"), and it REFUSES rather than warns.
class H5ForceRevert : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
public:
	H5ForceRevert(GameState game, IDIContainer& dicon);
	~H5ForceRevert();
	std::string_view getName() override { return nameof(H5ForceRevert); }
};
