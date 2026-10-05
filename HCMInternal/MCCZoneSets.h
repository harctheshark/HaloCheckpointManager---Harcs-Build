#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// ZONE SETS FOR THE THIRD-GEN MCC GAMES (Halo 3, ODST, Reach, Halo 4) - read side.
//
// Lists the CURRENT scenario's zone sets by name and reports which one is current and whether it is fully loaded.
// Used by "Show Current Zone Set" (Display 2D Info) and by MCCSwitchZoneSet's dropdown. Everything is plain memory
// reads; no engine call. RE + every address: Documents\Halo Mod And Tools\MCC Zone Sets\<game>_findings.json.
//
// WHERE THE DATA COMES FROM
//   * zone set list   = scenario tag block (scenarioTagDataFields::ZoneSetTagBlock), element stride and fields in
//                       mccZoneSetFields: +0x00 name string_id, bspMask = the structure-BSP mask the engine's own
//                       "fully active" test compares against currentBSPSet (H3/ODST +0x0C, Reach/H4 +0x114).
//   * current         = currentZoneSet (what HaloScript current_zone_set returns).
//   * "fully loaded"  = (bspMask & ~currentBSPSet) == 0 - the engine's current_zone_set_fully_active test, minus its
//                       designer-zone half. ⚠ The current index is published when a switch STARTS (and
//                       prepare_to_switch_to_zone_set sets it too), so on its own it means "switching to".
//
// ⚠⚠ NAMES: THESE ENGINES DO NOT DECODE STRING_IDS. All four keep a runtime hash map string_id -> offset into a
// string pool, rebuilt on every map load (layout identical in all four: header u32 bucketCount at +0, bucket heads
// at +buckets; node +0x00 key, +0x10 next, +0x18 int32 offset, -1 = no name). The HASH differs per game (H3/ODST a
// weighted byte sum, Reach/H4 a signed-shift MurmurHash2A), so instead of re-implementing four hashes that have
// never been checked against a live map, this walks every bucket ONCE per level and picks out the ids it needs.
// ~8 MB of bucket heads and a few tens of thousands of nodes, once per scenario - not per frame.
// If the string map pointers are missing for a build, names fall back to "zone set N" and everything else works.
// ================================================================================================================
class MCCZoneSets : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	MCCZoneSets(GameState game, IDIContainer& dicon);
	~MCCZoneSets();

	// The current scenario's zone sets, index-aligned with the engine's indices. An entry whose name cannot be
	// resolved reads "zone set N". Throws HCMRuntimeException when no scenario is loaded / it cannot be read.
	std::vector<std::string> getZoneSetNames();

	// The engine's current zone set index (-1 = none).
	int getCurrentZoneSet();

	// True once every structure BSP the zone set needs is resident. False while a switch is still loading.
	bool isZoneSetFullyLoaded(int index);

	// "3: set_jungle_walk", plus " (loading)" mid-switch. For the Display 2D Info row.
	std::string describeCurrentZoneSet();

	virtual std::string_view getName() override { return nameof(MCCZoneSets); }
};

// The GUI cannot resolve a cheat (GUIElementConstructor is handed settings and nothing else), so the services publish
// themselves here, keyed by game - MCC keeps all four DLLs loaded and HCM builds one of each per game.
namespace MCCZoneSetBridge
{
	bool isUsable(GameState game);              // a switch service is registered for this game and its info service is alive
	std::vector<std::string> names(GameState game);  // empty when unavailable / no level loaded
	int currentIndex(GameState game);           // -1 when unavailable
	int selection(GameState game);
	void setSelection(GameState game, int index);

	// internal: MCCSwitchZoneSet registers (with the info service it resolved) while it is alive
	void registerSwitch(GameState game, std::weak_ptr<MCCZoneSets> info);
	void unregisterSwitch(GameState game);
}
