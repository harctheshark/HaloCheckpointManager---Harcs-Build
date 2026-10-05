#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// UNCAP RENDER SECTIONS - Halo 2 (MCC halo2.dll build 1.3528), offline.
//
// Raises render_visible_globals.sections from 850 to 4096. With 850, big maps seen at speed (Metropolis) fill the
// list before the objects are added - the objects are appended last - so the player model and far instances flicker
// in and out (live-confirmed cause). Spec, RE and verification: Documents\Halo Mod And Tools\H2 Uncap Render
// Sections\spec.md ("HCM runtime apply / revert", "Risks") and spec.json.
//
// HOW: the three per-section arrays (sections[] 48 B, extern_shader_override u8[], shader_index_indices u8[]) move
// out of .data into ONE zero-filled VirtualAlloc block S (0x32640 bytes = 50 x (4096+32) entries) within +-0x70000000
// of the module, and 60 operands are rewritten IN PLACE (4-byte disp32/imm32 fields - no caves, no jumps, no length
// changes): 28 rip-relative leas, 11 image-base-relative loads, 19 sections-base-relative operands and 1 add-imm that
// re-aim back at the .data fields that stay, plus the cap (written LAST, restored FIRST) and a companion rel8 that
// makes a subpart-pool overflow fall back to "draw all parts" instead of aliasing. The row table is GENERATED from
// spec.json (UncapRenderSectionsTable.h, by ...\hcm\gen_hcm_table.py) - never edit it by hand.
//
// SAFETY (why this is more than one suspend window):
//  * A thread suspended after `lea r15,[sections]` (0x7ED887) or `lea rdx` (0x7EDF84) keeps the OLD base in a
//    register and would run the NEW displacements after resume (a wild write). So apply/revert only write inside a
//    window where no suspended thread has Rip, or any qword of its WHOLE live stack, inside the five writer functions
//    (the quiescence gate; up to 500 tries to apply, 1000 to revert). No fixed scan depth: callees under
//    sub_1807ED850 reach a 0x80068-byte frame. An unreadable stack counts as a hit.
//    A window only counts if it stopped EVERY other thread: the thread snapshot worked, no live thread was missed, and
//    a census inside the window (NtGetNextThread) finds no thread created since the snapshot.
//  * Revert is two-phase: the cap goes back to 850 first; the arrays are copied back (850 entries only) and the rows
//    restored only after a camera-pass reset has been seen and, under suspension, camera count <= 818 and running
//    count <= 850. If the renderer never goes quiet the game simply keeps running at cap 850 on the relocated arrays
//    (stock-equivalent and safe indefinitely) and it finishes by itself at the next Ingame (or the next toggle-off).
//  * S is never freed while any row refers to it. Readers outside the hazard functions can still HOLD a superseded
//    block in a callee-saved register after the revert window (sub_1807F1610, sub_1809786A0), so a superseded block
//    is freed only once every row is verified stock AND its revert is >= 5 s old AND one complete suspend window shows
//    no thread with an address in or near it in any register or on its live stack; otherwise it stays queued.
//  * A guard memcmps all 61 instructions before anything is written: all stock -> apply; a block address (from the
//    lea at 0x7ED887) inside the image -> a baked dll (reported, nothing done, never adopted - this test comes FIRST,
//    since a bake also satisfies every row formula); our own live patch outside the image -> adopt it; else refuse.
//
// THREADING: every apply/revert runs on ONE worker thread owned by the cheat. The toggle and MCC-state handlers only
// record the latest request and wake it (the toggle event also fires on the render thread when a preset loads, and
// the state event on MCC's own thread - neither may run a multi-second gate). A request overtaken by the opposite
// toggle stops waiting at once and leaves a safe state. Every suspend window takes HCM's process-wide suspension lock
// (hcmThreadSuspensionMutex, ScopedThreadSuspender.h) once per attempt, so no other HCM patcher can suspend this
// worker while it is suspending them, and other patchers still get in between attempts.
//
// LIFECYCLE: applied once per halo2.dll load (toggle-on, or MCC reaching Ingame with the toggle on); NOT reverted on
// loading screens (the arrays are rebuilt from zero every camera pass). Ingame with the toggle off finishes a revert
// left pending. Leaving Halo 2 forgets the patch without touching memory (the guard re-adopts it if the module
// persisted). Closing HCM leaves the patch live and leaks the block on purpose (the game keeps using it); the next
// HCM session adopts it at start/Ingame and ticks the checkbox (the toggle is not persisted, so it starts off).
// ================================================================================================================
class UncapRenderSections : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	UncapRenderSections(GameState game, IDIContainer& dicon);
	~UncapRenderSections();
	std::string_view getName() override { return nameof(UncapRenderSections); }
};
