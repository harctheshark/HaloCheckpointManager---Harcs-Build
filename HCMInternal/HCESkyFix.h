#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// Sky Fix - Halo Campaign Evolved.
//
// SYMPTOM: going out of bounds makes the sky stop rendering and the world go dark. It returns when the player
// comes back. HCM's freecam does not affect it, so it is keyed on the PLAYER's position.
//
// ⚠ THE BLAM SIM IS NOT INVOLVED - do not go looking there again. Established over several rounds INCLUDING
// in-game tests: the scenario has no `skies` block; nothing in either binary indexes the 84-byte cluster
// records (the exe's 43.8M instructions contain not one `cmp byte ptr [reg+0x1A], 0FFh`); the exe never reads
// the player's cluster attachment out of the sim; and holding that attachment (both object+0x08 and
// player+0x3B4) changed nothing - both read a CONSTANT 0x0001 in bounds and out, so neither even tracks
// position. render_sky / render_cluster_enabled / debug_structure_cluster_skies are all compiled out.
//
// THE ACTUAL MECHANISM is UE5 World Partition data layers. (The game is World Partition, not classic streaming
// sub-levels: the cook layout is <Map>/<MapName>/_Generated_/<hash>.umap, 15,063 generated packages under
// Levels/Halo1 alone.)
//
// AHaloWorldStreamingAreaActor::RemoveOccupant (exe rva 0x095B1350) runs when the player pawn's last local
// overlap with a streaming area ends:
//
//     095B1376  sub dword ptr [rdi+2D8h], 1      ; occupant refcount--
//     095B137D  jne  1495B140B                   ; NON-ZERO -> the whole teardown is SKIPPED
//     095B139D  mov  byte ptr [rcx+101h], 0      ; StreamingSourceComponent->bStreamingSourceEnabled = false
//     095B13D6  call 1495B2410                   ; ReleaseDataLayerRef, per AffectedDataLayers element
//     095B1406  call 1495B29D0                   ; unregister camera grids
//
// ReleaseDataLayerRef demotes a layer from Activated(2) to Loaded(1), and it has EXACTLY ONE CALL SITE IN THE
// ENTIRE 177 MB .text - the one above. So preventing that 1->0 transition blocks 100% of the module's
// demotion path. RemoveOccupant is not virtual and has only two call sites, so one hook covers every exit.
//
// THE FIX is therefore a single clamp with no code patch: hold the refcount at >= 2 so the game's own `sub`
// lands on >= 1, ZF=0, and the teardown never runs. RCX at the hook site IS the actor - the game just called a
// member function on it - so there is no pointer chain, no UWorld walk, and nothing to resolve.
//
// ⚠ CLAMP, DO NOT INCREMENT. The game's `sub` consumes the bump, so clamping converges on exactly 1 ("one
// pawn inside") however many exit events occur. A blind ++ drifts upward by one per enter/exit cycle.
//
// KNOWN LIMITS, all documented in the GUI tooltip:
//   * Turning it ON while already out of bounds cannot restore layers that are already demoted - the refcount
//     genuinely reached 0 before we engaged. Walk back in once and the pin holds from then on.
//   * Turning it OFF leaves each pinned area believing the player is inside; the next REAL exit drains it.
//   * Leaving it on for a whole level keeps every visited area's WP streaming source enabled, so cells
//     accumulate - expect memory growth and possible streaming hitches on large levels (B40 has 3482 runtime
//     cells). Default OFF for that reason.
//
// ⚠ The ONE crash-class risk is +0x2D8 not being the refcount, so the hook refuses to install unless the
// site's first 51 bytes match - and that signature literally contains `83 AF D8 02 00 00 01`, the decrement
// itself. HCE reports version 0.0.0.0 for every build, so the byte guard is the only protection against a game
// update. DO NOT REMOVE IT.
// ================================================================================================================
class HCESkyFix : public IOptionalCheat
{
private:
	class HCESkyFixImpl;
	std::unique_ptr<HCESkyFixImpl> pimpl;

public:
	HCESkyFix(GameState game, IDIContainer& dicon);
	~HCESkyFix();
	virtual std::string_view getName() override { return nameof(HCESkyFix); }
};
