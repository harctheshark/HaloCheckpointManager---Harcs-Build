#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// ================================================================================================================
// Dump Checkpoint - Halo Campaign Evolved.
//
// Writes the game's CURRENT checkpoint to <name>.bin, byte for byte. It does NOT create a checkpoint - it writes
// the last one the game actually made.
//
// WHERE IT WRITES: the save folder HCMExternal's "Halo Campaign Evolved" tab has selected, i.e.
// <HCM dir>\Saves\Halo Campaign Evolved\... - the same ISharedMemory::getDumpInfo convention the six MCC tabs use,
// so dumps show up in the tab (and in its subfolders) the moment they are written. It falls back to
// <HCM dir>\HaloCER Checkpoints\ - where this feature wrote unconditionally before that tab existed - whenever
// getDumpInfo is unusable: no shared memory, HCMExternal sitting on another game's tab (getDumpInfo throws exactly
// that), or an older HCMExternal with no HaloCER tab at all. ⚠ Files already in the legacy folder are NOT migrated;
// they stay valid and HCEInjectCheckpoint's browse dialog still opens there while they are the only dumps around.
//
// Why this is still a separate class rather than a HaloCER branch inside DumpCheckpoint.h: that one depends on
// IGetMCCVersion for a 10-byte version stamp, which HaloCER has no version resource to provide AND must not have
// stamped into it (see below), and on a checkpointLocation MultilevelPointer into an in-process buffer that the
// shipped game does not have at all.
//
// ⚠ THE FILE IS THE BLOB, VERBATIM - no version stamp, no trailing bytes, no padding. The engine's SHA-1 at
// blob+0x1ED20 covers the WHOLE buffer, so anything written into the dump would make it fail verification on
// revert, and on HaloCER a rejected revert does not fail softly: every failure path in the revert falls through
// to a LEVEL RESTART. (MCC's DumpCheckpoint stamps 10 bytes and MCC's InjectCheckpoint then zeroes them again -
// do not copy that here.) The one thing that IS written is the blob's own SHA-1 over itself - see below.
//
// ================================================================================================================
// TWO STORAGE PATHS, AND THE SHIPPED GAME TAKES THE SECOND ONE - see HCECheckpointBlob.h.
//
//   LOCAL    the checkpoint is sitting in an in-process ring buffer. Dumping is a passive read, gated on the
//            engine's own publication barrier (the digest is computed on a WORKER TASK, so there is a window
//            where the 12 MB buffer has been written but its SHA-1 has not, and a file copied inside that window
//            can never be injected). HCECheckpointBlob::readCurrentCheckpoint waits, copies, then re-checks.
//
//   PROVIDER there is no in-process buffer at all. The only moment a checkpoint blob exists in this process is
//            the instant the engine hands it to the storage provider - so HCM SHADOWS that hand-off. The midhook
//            at 0x19D51A fires on every checkpoint, natural or forced, and while shadowing is armed it copies the
//            blob into an HCM-owned buffer; the dump writes the newest copy.
//
//            (Neither path cares where the file goes - resolveDumpFolder is the only thing that decides that.)
//
//            ⚠ DUMPING DOES NOT CREATE A CHECKPOINT. An earlier version of this file forced one (which is what
//            the reference tool HCM_Evolved does), because forcing was the only way to make a hand-off happen
//            while it was watching. That silently moved the player's checkpoint. It now writes the checkpoint
//            they are already on, and if there isn't one yet it says so - see requireShadowedCheckpoint.
//
//            The shadow is armed on level load, on Force Checkpoint and on the dump itself, and it is gated on
//            settings->hceShadowCheckpoints (default on). The whole cost is one ~12 MB memcpy on the game thread
//            per checkpoint, measured at 0.44 ms median / 0.98 ms worst - see the cost note in
//            HCECheckpointDetours.cpp. With the setting off, nothing is hooked and nothing is copied, and Dump
//            says so rather than pretending.
//
// ⚠ THE DIGEST IS RE-SIGNED ON THE PROVIDER PATH. What the hand-off carries - and so what the shadow holds - is
// the LIVE GAME STATE, whose header digest field still holds whatever the last revert left there; the engine only
// signs the copy the provider is given, via the sub_1803274D0 callback, on bytes HCM does not own. So the dump is
// run through HCECheckpointBlob::applySHA1 to make the FILE self-consistent. That is deterministic and covers the
// whole blob, so the result is a checkpoint the engine's own verifier accepts.
//
// Everything that actually knows the checkpoint layout lives in HCECheckpointBlob, which HCEInjectCheckpoint
// resolves too; this class is only naming, folders, files and messages.
// ================================================================================================================
class HCEDumpCheckpoint : public IOptionalCheat
{
private:
	class HCEDumpCheckpointImpl;
	std::unique_ptr<HCEDumpCheckpointImpl> pimpl;

public:
	HCEDumpCheckpoint(GameState game, IDIContainer& dicon);
	~HCEDumpCheckpoint();
	virtual std::string_view getName() override { return nameof(HCEDumpCheckpoint); }
};
