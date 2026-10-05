#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// SWITCH ZONE SET - Halo 3, ODST, Reach, Halo 4. Replaces the old "Switch BSP Set", which wrote a raw BSP mask.
//
// Does what HaloScript `switch_zone_set` does, with TWO PLAIN STORES and no engine call: the request is a pair of
// module globals (main_globals) that the engine's own main loop polls every iteration, on its own thread. It then runs
// the real switch (unload/load, designer zones, the commit of currentZoneSet/currentBSPSet) and clears both itself.
//   1. int32  zoneSetSwitchIndex = index
//   2. uint8  zoneSetSwitchFlag  = 1        ⚠ FLAG LAST - the consumer reads the flag, then the index.
// ⚠ NOT an engine call on purpose: calling into these engines from a foreign thread faults (thread-affine tag-resource
// tables; see the "engine calls need the sim thread" note), and a store the engine consumes itself cannot.
//
// The engine's own request function also checks: scenario loaded, 0 <= index < count, and "already current and fully
// active" - replicated here because the raw stores skip it. Two side effects are skipped too: a per-player "loading"
// HUD message (cosmetic; the engine still posts the closing one) and, in a networked session, the matching player
// event. A pending request is silently dropped by a revert or a map load - that is engine behaviour.
// Addresses per build: InternalPointerData.xml; RE in Documents\Halo Mod And Tools\MCC Zone Sets\.
// ================================================================================================================
class MCCSwitchZoneSet : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MCCSwitchZoneSet(GameState game, IDIContainer& dicon);
	~MCCSwitchZoneSet();

	virtual std::string_view getName() override { return nameof(MCCSwitchZoneSet); }
};
