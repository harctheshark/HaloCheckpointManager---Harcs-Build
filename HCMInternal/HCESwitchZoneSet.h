#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) - SWITCH ZONE SET.
//
// Lists every zone set the CURRENT scenario actually declares, and switches to the one you pick. No per-level
// table is shipped: the names come straight out of the loaded scenario tag, so this works on any level,
// including ones that did not exist when this was written.
//
// WHERE THE NAMES COME FROM
//   scenario + 0xD0 is the 'zone sets' block: count, then an encoded tag-block address. Stride is 304 (0x130).
//   +0x004 is a LITERAL char[256] ASCII name - no string_id resolution, no call into the game, no thread
//   considerations. Verified populated across all 13 shipped levels (a50 = set_landing / set_lift_approach,
//   c20 = set_floor_1..4, and so on).
//   ⚠ Every level also ships exactly ONE *internal* zone set whose name is EMPTY and whose flags have bit 2
//   set. An empty name there is NOT a read failure - it is rendered "(internal zone set N)".
//
// HOW THE SWITCH HAPPENS
//   Through HaloScript: `switch_zone_set <name>`, queued on HCEConsoleBridge. That runs it on the simulation
//   thread through the console's existing pump, and the script parser validates arity and argument types
//   BEFORE evaluating anything - so a bad name is a compile error in the game's own sink, never a corrupted
//   stack. Calling sub_1801B0060 directly would mean owning the ABI and the thread affinity ourselves for no
//   benefit.
//
// ⚠ WHAT THE CURRENT-INDEX READ ACTUALLY MEANS. HCM's anchor is named CurrentBSP for config-compatibility
// reasons, but simBase+0x9A14E0 is literally what HaloScript `current_zone_set` returns - it is a ZONE SET
// index, not a BSP. It is also published BEFORE the BSPs finish loading, so it means "switching to", not
// "finished". Shown as the active selection, not relied on for anything else.
// ================================================================================================================

// The GUI cannot resolve a cheat - GUIElementConstructor is handed settings and nothing else - so this
// publishes itself through a bridge, exactly as HCEConsoleBridge does. Disarmed, every entry point reports
// "unavailable" rather than touching anything.
namespace HCEZoneSetBridge
{
	bool isUsable();
	// Snapshot of the current scenario's zone set names, index-aligned with the engine's own indices.
	std::vector<std::string> names();
	// Engine's current zone set index, or -1 when it cannot be read.
	int currentIndex();
	// Queues `switch_zone_set <name>`. false + a reason when it cannot run right now.
	bool switchTo(int index, std::string& outWhy);
	// The dropdown's selection. Lives here rather than in the widget because the BUTTON's event fires on a
	// detached thread with no access to the GUI element that holds it.
	int selection();
	void setSelection(int index);
	// Forces a re-read of the scenario block (level changed, or the user pressed refresh).
	void invalidate();
}

class HCESwitchZoneSet : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HCESwitchZoneSet(GameState game, IDIContainer& dicon);
	~HCESwitchZoneSet();
	std::string_view getName() override { return nameof(HCESwitchZoneSet); }
};
