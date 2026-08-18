#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// Inject Checkpoint - Halo Campaign Evolved.
//
// Loads a .bin dumped by HCEDumpCheckpoint back over the game's current checkpoint, so the next revert restores
// it.
//
// WHICH FILE, in this order:
//   1. The checkpoint highlighted on HCMExternal's "Halo Campaign Evolved" tab (ISharedMemory::getInjectInfo) -
//      the same contract MCC's InjectCheckpoint has always had. Guarded on the game index, because HCMExternal
//      publishes ONE selection for whichever tab is open and an MCC checkpoint must never reach this code.
//   2. Otherwise a plain Win32 open dialog, which is what this feature shipped with. ⚠ THIS FALLBACK IS LOAD
//      BEARING, not a leftover: it is what keeps injection working with no shared memory, with an older
//      HCMExternal, with HCMExternal on another game's tab or closed, with nothing selected, and it is still the
//      only way to inject a file that is not in the tab's folder. Every reason the selection is unusable ends up
//      here (see externalSelectedCheckpoint). PresetManager already establishes the in-process file-dialog pattern
//      (ModalDialogGuard + GetOpenFileNameW) and this follows it.
//
// It is still a separate class from MCC's InjectCheckpoint despite now sharing that selection: that one writes
// through a checkpointLocation MultilevelPointer into an in-process buffer the shipped game does not have, stamps
// and zeroes 10 version bytes HaloCER must never carry, and computes its SHA over a different layout.
//
// ⚠⚠⚠ THIS IS THE DANGEROUS ONE, AND THE DANGER IS NOT A CRASH.
// Every failure path in the revert (sub_18019D730) falls through to LABEL_95, which sets word_181357067 and
// byte_181357069, and the checkpoint host at 0x1ADB83 turns that pair into sub_18020C920 - A LEVEL RESTART. A bad
// digest, a checkpoint from another level, or one made on another difficulty does not produce an error message or
// even a crash the user could alt-tab out of: it silently throws away their run. There is also no bypass to hide
// behind - the 0xBB x 20 sentinel the verifier honours is unreachable from the revert, which calls it with
// a2 = 0 (`19DBB8: xor edx, edx`). So:
//
//   * EVERYTHING is validated against the LIVE SESSION HEADER before anything is handed to the game.
//   * The digest is ALWAYS recomputed (HCECheckpointBlob::applySHA1), never trusted from the file.
//   * When a check fails and its toggle is on, the user gets a blocking dialog that says "restart the level" in
//     as many words. If that dialog cannot be shown, the injection is REFUSED rather than pushed through.
//
// The validation target is the live header at the front of the game state block (HCECheckpointBlob::readLiveHeader),
// which is not a stand-in for the engine's test - it IS it. sub_18019EB50's `live` argument is qword_1812945C8,
// the header allocated at offset 0 of that same block, and the session setup at 0x19D21B fills in every field the
// revert compares. So a file that agrees with it field for field is a file the revert accepts. It is also
// available at all times, which is why injection no longer requires a checkpoint to already exist.
//
// ================================================================================================================
// TWO STORAGE PATHS - see HCECheckpointBlob.h. The shipped game takes the second.
//
//   LOCAL    the checkpoint lives in an in-process ring buffer, so injection is a write into it:
//            HCECheckpointBlob::writeCheckpoint, barrier-gated on both sides, rolling the previous checkpoint
//            back if the engine saves underneath it.
//
//   PROVIDER there is no buffer to write. The engine hands its state to an external provider at 0x19D51A, so
//            injection means being present at that instant and redirecting the provider's SOURCE ARGUMENT to our
//            blob - HCECheckpointDetours::injectCheckpointAtHandoff, armed for exactly ONE forced checkpoint,
//            never left armed. It forces that checkpoint itself, so injecting also creates one.
//
//            ⚠⚠ THE ARGUMENT, NOT THE BUFFER. RDX at that site is the LIVE GAME STATE (the block a revert
//            restores into), not a staging copy. Writing our blob into it would corrupt the running session.
//            ★ It also means the engine re-signs the blob for us, via the sub_1803274D0 callback it passes as
//            the 5th argument, so a stale checksum cannot survive the trip.
//
// The three warning toggles deliberately reuse MCC's injectCheckpointLevelCheck / injectCheckpointVersionCheck /
// injectCheckpointDifficultyCheck settings (and injectCheckpointForcesRevert, and the injectCheckpoint hotkey),
// exactly as HCEDumpCheckpoint reuses autonameCheckpoints: HaloCER and the MCC games can never be the same
// process, so only one implementation is ever listening.
//
// Everything that knows the blob format - the slot ring, the publication barrier, the header layout, the ported
// sub_1802096F0 options comparison and the SHA-1 recipe - lives in HCECheckpointBlob, shared with the dump side.
// ================================================================================================================
class HCEInjectCheckpoint : public IOptionalCheat
{
private:
	class HCEInjectCheckpointImpl;
	std::unique_ptr<HCEInjectCheckpointImpl> pimpl;

public:
	HCEInjectCheckpoint(GameState game, IDIContainer& dicon);
	~HCEInjectCheckpoint();
	virtual std::string_view getName() override { return nameof(HCEInjectCheckpoint); }
};
