#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - SWITCH ZONE SET.
//
// Lists every zone set the CURRENT scenario declares and switches to the one you pick. Nothing per-level is
// shipped: names come straight out of the loaded scenario tag, so this works on any map.
//
// WHERE THE NAMES COME FROM
//   scenario globals = [exe + 0x05A62538];  array = [globals + 0x264], stride 0x218, count = [globals + 0x274]
//   entry + 0x00  u32    name hash
//   entry + 0x04  char[] INLINE null-terminated ASCII name - no string table, no engine call.
//   Independently confirmed by the engine's own name lookup at exe+0x00A7A8B0, which resolves the 'scnr' tag
//   and then indexes [+0x264] + i*0x218 + 4 exactly as we do.
//
// HOW THE SWITCH HAPPENS - AND WHY IT IS NOT AN ENGINE CALL
//   H5GetPlayerState::requestZoneSetSwitch writes the same three globals the engine's switch_zone_set writes
//   and lets the game's main loop consume them on its next tick (it polls the preparing flag at
//   exe+0x005E7D2F and calls main_switch_structure_bsp itself). See that function for the full derivation.
//   In this title every engine call attempted has faulted and killed the process, and every plain store has
//   worked; this keeps us on the side that works.
//
// ⚠ DIFFERENT FROM THE HALOCER VERSION IN ONE IMPORTANT WAY: HaloCER switches BY NAME through HaloScript, so a
// zone set with no name in the tag can never be switched to and is filtered out of its dropdown. Halo 5
// switches BY INDEX, so EVERY zone set is reachable - including unnamed ones, which are listed under a
// synthesised "zone set N" label. Do not copy HaloCER's filtering here; it would hide working entries.
// ================================================================================================================

// The GUI cannot resolve a cheat - GUIElementConstructor is handed settings and nothing else - so this
// publishes itself through a bridge, exactly as HCEZoneSetBridge does. Disarmed, every entry point reports
// "unavailable" rather than touching anything.
namespace H5ZoneSetBridge
{
	bool isUsable();
	// Snapshot of the current scenario's zone set names, index-aligned with the engine's own indices.
	std::vector<std::string> names();
	// The engine's COMMITTED zone set index, or -1 when none is active / it cannot be read.
	int currentIndex();
	// Requests the switch. false + a reason when it cannot run right now.
	bool switchTo(int index, std::string& outWhy);
	// The dropdown's selection. Lives here rather than in the widget because the BUTTON's event fires on a
	// detached thread with no access to the GUI element that holds it.
	int selection();
	void setSelection(int index);
	// Forces a re-read of the zone set table (level changed).
	void invalidate();
}

class H5SwitchZoneSet : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5SwitchZoneSet(GameState game, IDIContainer& dicon);
	~H5SwitchZoneSet();
	std::string_view getName() override { return nameof(H5SwitchZoneSet); }
};
