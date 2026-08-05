#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include <vector>

// Halo Campaign Evolved ONLY (GameState::Value::HaloCER).
//
// Owns EVERY code hook Halo Campaign Evolved needs around checkpoints, and drives four features from them:
//   "Force Checkpoint"            (settings->forceCheckpointEvent)
//   "Disable Natural Checkpoints" (settings->naturalCheckpointDisable)
//   "Dump Checkpoint"             (HCEDumpCheckpoint,   via armShadowCapture / readShadowCheckpoint)
//   "Inject Checkpoint"           (HCEInjectCheckpoint, via injectCheckpointAtHandoff)
//
// They share one class because they share hook sites and, more importantly, shared STATE: the dump/inject swap at
// the provider hand-off is only allowed to fire inside a checkpoint HCM itself forced, and "is this checkpoint
// forced" is a fact only the save_checkpoint call hook knows. Splitting them would mean two owners for one byte of
// game code and a flag passed between them. See HCECheckpointDetours.cpp for the full detour description.
//
// HCEDumpCheckpoint and HCEInjectCheckpoint reach this through resolveDependentCheat(HCECheckpointDetours), so
// constructing either of them installs nothing on its own - the hooks still attach lazily on first real use.
class HCECheckpointDetours : public IOptionalCheat
{
private:
	class HCECheckpointDetoursImpl;
	std::unique_ptr<HCECheckpointDetoursImpl> pimpl;

public:
	HCECheckpointDetours(GameState gameImpl, IDIContainer& dicon);
	~HCECheckpointDetours();

	std::string_view getName() override { return nameof(HCECheckpointDetours); }

	// ============================================================================================================
	// THE PASSIVE SHADOW - what Dump Checkpoint reads.
	//
	// The hand-off hook fires on EVERY checkpoint, natural or forced. While shadowing is armed it copies the blob
	// the engine is about to hand its storage provider into an HCM-owned buffer, so a dump writes the checkpoint
	// the player ALREADY HAS. Dumping therefore no longer creates a checkpoint.
	//
	// ⚠ THE SHADOW ONLY EVER HOLDS CHECKPOINTS THAT HAPPENED WHILE IT WAS ARMED. It is invalidated whenever the
	// level/game state changes and whenever the feature is turned off, so it can never hand out a checkpoint from
	// a previous level. "Nothing yet" is a normal, expected answer - see ShadowStatus::haveCheckpoint.
	// ============================================================================================================
	struct ShadowStatus
	{
		bool     wanted = false;         // the user's "Keep last checkpoint ready to dump" setting
		bool     armed = false;          // shadowing is actually running (hooks installed, buffer published)
		bool     haveCheckpoint = false; // a COMPLETE blob is sitting in the shadow right now
		bool     handoffSeen = false;    // this process has reached the provider hand-off at least once
		uint32_t length = 0;             // bytes in the shadow
		uint32_t captureCount = 0;       // completed captures since HCM attached
		uint32_t overlongLength = 0;     // non-zero: the game handed over MORE bytes than the shadow buffer holds
	};

	// Never throws. Safe to call at any time from any thread but a hook.
	ShadowStatus getShadowStatus();

	// Installs the hooks and publishes the shadow buffer, if the setting is on and it is not already running.
	// Idempotent and cheap on every call after the first. A NO-OP while the setting is off - that is what makes
	// "off" cost literally nothing, hooks included. Throws HCMRuntimeException with a user-facing message (no
	// level loaded, unrecognised build, ...). Caller's thread, never a hook.
	void armShadowCapture();

	// A coherent copy of the newest shadowed checkpoint. Uses the game thread's own sequence counter to detect a
	// checkpoint landing mid-copy and retries, exactly like the local path's publication barrier - a torn blob
	// would be a file that fails its SHA-1, and on HaloCER a rejected revert RESTARTS THE LEVEL.
	// Throws HCMRuntimeException if nothing has been shadowed yet. Caller's thread, never a hook.
	std::vector<byte> readShadowCheckpoint();


	// ============================================================================================================
	// THE PROVIDER HAND-OFF, WRITE SIDE. Installs the hooks if needed, arms a ONE SHOT action, forces a checkpoint,
	// and blocks the CALLING thread (never the game thread) until the game thread has consumed the arming or the
	// deadline expires. Always disarms before returning or throwing. Throws HCMRuntimeException with a user-facing
	// message; not safe to call from inside a hook.
	//
	// blob.size() must equal the engine's own state length, which the caller reads out of hceCheckpointStateLength.
	// The hook re-checks it against the R8D the engine actually passes and refuses on a mismatch rather than
	// handing over the wrong number of bytes.
	// ============================================================================================================

	// Redirects the storage provider's source argument at the hand-off to a copy of `blob`, for exactly one forced
	// checkpoint. Does NOT write into the game's own buffers - see the ⚠⚠ note in HCECheckpointDetours.cpp.
	void injectCheckpointAtHandoff(const std::vector<byte>& blob, int timeoutMilliseconds);
};
