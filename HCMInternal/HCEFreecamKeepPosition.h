#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// KEEP THE FREECAM WHERE IT IS ACROSS A CHECKPOINT REVERT.
//
// THE PROBLEM
// -----------
// Revert while in freecam and the camera snaps back to wherever it was standing when the checkpoint was taken,
// which makes the freecam useless for lining a shot up and re-running an attempt into it.
//
// WHY IT HAPPENS — and it is NOT a camera reset
// ---------------------------------------------
// There is no "reset the camera" call anywhere on the revert path; looking for one is a dead end (an earlier
// comment in HCEFreecam.cpp pointed at sim 0x26D385 for this, which is off by 0x1B and is actually the `04`
// operand byte of a `cmp eax, 4` — not a function, not even an instruction boundary).
//
// What actually happens is simpler. "observer globals" (TLS +0x4E8), "director globals" (TLS +0x148) and
// "player control globals" (TLS +0xB8) are ALL allocated out of game-state arena 4 — each registers with
// `ecx = 4` — and a revert restores that whole 0xC10000 block bit-for-bit. The freecam's pose is therefore
// just checkpoint payload, restored along with everything else. So is the freecam's own on/off byte, which is
// why HCEFreecam already has to re-assert it.
//
// The observer/director/player-control POINTERS live in sim TLS and are NOT restored, so addresses stay valid
// across a revert — only the contents change.
//
// ⚠⚠⚠ THE OBSERVER IS NOT THE CAMERA — THREE EARLIER VERSIONS OF THIS FILE GOT THIS WRONG.
// -----------------------------------------------------------------------------------------
// The nine observer floats (+0x154 position, +0x17C forward, +0x188 up) are the LAST STAGE of a four-stage
// derivation that the engine rebuilds FROM SCRATCH every simulation frame:
//
//     cameraEntry+0x20  (the authority — input is integrated into it by sub_1803A5DB0)
//       -> sub_1802194B0 publishes a 0x104-byte camera block at cameraEntry+0x78
//       -> observer+0x10 points at that block, sub_180233FC0 memcpys it to observer+0x1C8
//       -> sub_180235D80 / sub_180236540 recompute observer+0x154 / +0x17C / +0x188
//
// All four stages run inside `call sub_1801AE530`, i.e. BEFORE HCM's pump hook at sim 0x1AF2BD. And
// `sub_180232A30` is NOT an integrator — that claim is REFUTED; it passes position through and only
// re-orthonormalises. Writing the observer therefore produced exactly the three symptoms that were reported:
// it looked like it worked (it is the final cache the renderer reads), it ate all camera input (input had
// already been folded into the authority upstream), and the moment we stopped re-asserting it, the next frame
// re-derived the cache from the checkpoint-restored authority and the camera snapped back. There is no frame
// position at which writing the observer preserves input — the fight was unwinnable at any cadence.
//
// THE POSE — the authority, at cameraEntry = *(tls+0x148) + slot*0x1AC
//     +0x20  position x,y,z
//     +0x2C  yaw    (radians)
//     +0x30  pitch  (radians)
//     +0x34  roll   (radians)  — deliberately left alone so HCECameraRoll keeps ownership
// sub_1803A5DB0 ADDS this frame's input to those fields, so one write after the revert is preserved and flown
// onward from: no input lock, and nothing to re-assert per frame.
//
// ⚠⚠ AND THE TRAP THAT WOULD CORRUPT A VPTR: cameraEntry+0x08 onward is a UNION over 13 camera-mode classes,
// and +0x20 is only a position under mode 2 (the flying camera) and mode 5. Under mode 7, +0x28 is a VTABLE
// POINTER and +0x20 is a word — mode 7 keeps its pose at +0x50 instead, which is also why HCM's notes record
// cameraEntry+0x50 as "written once, no readers". This is the same union that once sent Teleport-to-Camera to
// a bogus world point. So the pose is NEVER read or written without first checking
//     *(cameraEntry + 0x08) == simBase + 0x882318      // the mode-2 vtable
// which is one 8-byte read and never calls into a possibly-garbage object. If that address ever moves the
// comparison simply fails and the feature does nothing, which is the correct way for this to break.
//
// ⚠⚠ WHY THIS NEVER TOUCHES THE CAMERA ELECTION. HCEFieldOfView documents that writing through
// APlayerCameraManager — the election that re-elects roughly once a second — CRASHED this game. Nothing here
// goes near it: every address is the sim-TLS chain HCEGetPlayerState already owns, and the pattern is the
// validate-then-latch one from HCEFieldOfView with the throttled re-assert from HCECameraRoll.
//
// ⚠ WHY THE CAMERA ENTRY IS RE-RESOLVED RATHER THAN CACHED. The director globals are a 0x6F0-byte arena-4
// allocation, so the revert can bring back a different active slot AND a different camera mode. Reusing a
// latched address across the very event this feature exists for is exactly how it would write into the wrong
// object. It re-resolves and re-checks the mode vtable before every write — and because a revert can restore
// a non-flying mode whose mode-2 constructor then re-seeds the pose from the observer, it also writes again
// on the first frame the mode comes back.
//
// ⚠ AND THE ONE BUG THAT WOULD MAKE THIS SILENTLY DO NOTHING: do not snapshot while a restore is pending.
// The first pass after a revert would otherwise overwrite the saved pose with the checkpoint's, and the
// feature would look like it had simply never run.
//
// Detection is the tick counter going BACKWARDS, which is already shipped and already documented as the
// revert signal. No new hooks, and nothing on the game's critical path.
// ================================================================================================================

class HCEFreecamKeepPosition : public IOptionalCheat
{
private:
	class HCEFreecamKeepPositionImpl;
	std::unique_ptr<HCEFreecamKeepPositionImpl> pimpl;

public:
	HCEFreecamKeepPosition(GameState game, IDIContainer& dicon);
	~HCEFreecamKeepPosition();
	virtual std::string_view getName() override { return nameof(HCEFreecamKeepPosition); }
};
