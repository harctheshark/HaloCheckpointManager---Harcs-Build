#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// H2A SHADER PRE-WARM - Halo 2 campaign, Anniversary graphics (the Saber3D renderer inside halo2.dll), MCC 1.3528.
//
// WHY. Every traversal hitch in Anniversary graphics is the game creating a shader variant the first time something
// that needs it is drawn: its bytecode cache hits, but the Direct3D 11 Create*Shader call then makes the graphics
// driver compile it (1-36 ms each, measured), one at a time behind a single lock, while the frame waits. Variants
// only live for the session - the table is empty at every level start - so the stutter comes back every time.
// (Measured with the read-only probe in Documents\Halo Mod And Tools\H2A Stutter Research\, 2026-11-09.)
//
// WHAT. While the toggle is on, HCM
//   1. RECORDS which variants a level ends up using (shader type, stage, 32-byte define mask - the same key the
//      game's own cache files are indexed by) into <HCM folder>\H2AShaderPrewarm\level_NNN.bin, and
//   2. at the next start of that level, RE-CREATES all of them up front through the game's own two functions
//      (find-or-allocate a variant slot, then make the variant), so nothing is left to compile during play.
// The first playthrough of a level still stutters while HCM learns it.
//
// HOW. One inline hook on the shader manager's once-per-frame housekeeping call (sub_180115AF0: it evicts old
// variants and bumps the manager's frame number, i.e. the one place the game itself rewrites the variant table).
// Our work runs right after it, on the same thread, holding the manager lock across "find slot + make variant" so
// the game's purge / eviction (which take that lock first) can never run between the two.
// Nothing in the game's code or data is patched besides that hook; with the toggle off the detour is a no-op.
//
// CLASSIC. Staying in Classic graphics does NOT throw variants away (45 visits up to 28 s measured, none did).
// What does hurt is a level STARTED in Classic: the Anniversary renderer draws nothing, so nothing is created, and the
// first switch to Anniversary has to create every visible shader at once - the long freeze people report in co-op.
// So a second hook on the game's graphics-mode handler (sub_1800511E0) gives us a place to run while Classic is
// showing; there the level's list is created a few milliseconds per frame, in the background.
//
// LIMITS. Build 1.3528 only (entry bytes of the hooked and called functions are checked before a hook is armed).
// If the game empties the table under a level that is still up (restart / revert from the pause menu), the list is
// re-created a few milliseconds per frame instead of in one block.
// ================================================================================================================
class H2AShaderPrewarm : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H2AShaderPrewarm(GameState game, IDIContainer& dicon);
	~H2AShaderPrewarm();
	std::string_view getName() override { return nameof(H2AShaderPrewarm); }
};
