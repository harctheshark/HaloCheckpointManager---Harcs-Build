#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// AI SQUAD OVERLAY - labels every live AI above its head with the squad it belongs to, and whether killing it
// actually gates script progression.
//
// WHITE  - this unit does not gate anything. Killing it is optional.
// RED    - its squad is named by a BLOCKING script gate with no timeout and no alternative. It has to die.
// AMBER  - gated, but with a way out: a timeout, or an or-gate whose other branch is a volume you can walk
//          into. Amber carries a live COUNTDOWN when the gate is timed.
//
// ⚠⚠⚠ WHY THIS CANNOT BE A STATIC NAME TABLE, unlike HCECheckpointGrantTriggers.
// c10's finale is gated on `sq_gauntlet_bloodgate`, and membership of that squad is created AT RUNTIME by
// `ai_migrate` the moment a biped crosses a trigger volume (c10.hsc:1642-1657) - one-way, with no reverse
// migration anywhere. So a unit that is white when it spawns must turn RED mid-fight the instant it walks into
// the tower plaza, and no amount of tag data can predict which ones will. That is exactly why the overlay reads
// the actor's LIVE squad every tick and compares it against a resolved squad index, rather than matching a name
// list. It is also the single most useful thing the overlay does: nothing in game marks bloodgate members, and
// stragglers that reached the tower and were never killed are what stall the level ending.
//
// ================================================================================================================
// THE RUNTIME CHAIN. Every offset below is read by a real instruction in the sim dll, not inferred.
//
//   gate:     *(u8*)(tls + 0x14) != 0        every engine function checks this before touching sim globals
//   objHdr  = *(void**)(tls + 0x20)
//   entries = *(void**)(objHdr + 0x50)   bound = objHdr+0x44 (high water)   stride = *(u64*)(objHdr+0x20) == 0x18
//     entry + 0x00  u16 salt   (0 = free slot)
//     entry + 0x04  u8  type   -> KEEP IF (1 << type) & 0x1003, which is the ENGINE'S OWN "has a team" mask:
//                                 biped(0) | vehicle(1) | giant(12).  ⚠ NOT creature(11).
//     entry + 0x10  object*
//   object + 0x20   float3 world position   (the ORIGIN, i.e. at the FEET - hence trigger volumes' "z sink")
//   object + 0x128  bit 2 SET = DEAD        the engine's own flag; object_get_health short-circuits on it
//   object + 0x1BA  i8 team                 global_campaign_team_enum
//   object + 0x1AC  i32 actor handle        -1 = not an AI actor (the player, or a scripted prop)
//   aiHdr   = *(void**)(tls + 0x28);  actors = *(void**)(aiHdr + 0x50);  stride 0xD10, records stored INLINE
//     actor + 0x3C  i32 CURRENT squad index
//     actor + 0x40  i32 ORIGINATING squad index
//
// ★★★ +0x3C AND +0x40 ARE DIFFERENT FIELDS AND THAT DIFFERENCE IS THE FEATURE. Colour on +0x40, because that
// is the identity scripts key on (`ai_place sq_foo` then a gate on `sq_foo`). When +0x3C != +0x40 the actor has
// MIGRATED, which is precisely the bloodgate case - so the overlay can show both and flag the move.
//
// SQUAD AND GROUP NAMES come from the live scenario tag rather than a shipped table, so this works on every
// level with no per-level data at all:
//   scenario + 0x33C  squad groups block, stride 40   (+0x00 char[32] name, +0x20 i16 PARENT GROUP - they NEST)
//   scenario + 0x348  squads block,       stride 108  (+0x00 char[32] name, +0x24 i16 team, +0x26 i16 parent group)
// Both offsets come from walking the binary's own scenario struct definition, which CLOSES EXACTLY at its
// declared 1780 bytes and is pinned by five independently code-verified offsets (structure bsps 0x60, zone sets
// 0xD0, soft ceilings 0x248, trigger volumes 0x278, zone set switch 0x29C).
// ⚠ Squad names are TRUNCATED AT 31 CHARS in the tag (char[32] + NUL), and their case differs from the script
// source (script `sq_gr_lastwave` vs tag `sq_gr_lastWave`), so any match against script-derived data must be
// case-insensitive and prefix-tolerant.
//
// ⚠⚠ NEVER FILTER ON THE SQUAD TAG'S TEAM (squads +0x24). It is `default` for 1506 of 2406 squads game-wide,
// and for 197 of 197 on d20 - a squad-team filter would put every unit on that level in one bucket. The real
// faction resolves from the CHARACTER tag when the unit spawns, so the filter reads the LIVE unit team at
// object+0x1BA. The squad's authored team is kept only as a display extra, and a disagreement between the two
// is itself interesting (it is the a15 `sq_flyby_air1R` faction-mixed pathology).
//
// ⚠ COST: with ~100 labelled actors against an 800-2048 slot table this is ~200 guarded reads and ~64 KB per
// tick IF the entry array is bulk-copied once and filtered in local memory - versus ~1500 reads field by field.
// The SEH __try frame is the cost, not the bytes. The snapshot is therefore built on the SIMULATION PUMP, once
// per sim tick, and the render path only formats and draws what the snapshot already holds.
// ================================================================================================================

class HCEAISquadOverlay : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HCEAISquadOverlay(GameState game, IDIContainer& dicon);
	~HCEAISquadOverlay();
	virtual std::string_view getName() override { return nameof(HCEAISquadOverlay); }
};
