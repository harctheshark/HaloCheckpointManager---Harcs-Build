#include "pch.h"
#include "HCEFreecamKeepPosition.h"
#include "HCEGameThreadPump.h"
#include "HCEGetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See HCEFreecamKeepPosition.h for the mechanism. The short version, and the thing three earlier attempts
// got wrong: THE OBSERVER IS NOT THE CAMERA.

namespace
{
	constexpr int64_t kCameraEntriesTls = 0x148;
	constexpr int64_t kCameraEntryStride = 0x1AC;
	constexpr int64_t kPlayerControlTls = 0xB8;
	constexpr int64_t kFreecamMaster = 0x9C8;
	constexpr int64_t kGameTimeGlobalsTls = 0x98;
	constexpr int64_t kGameTick = 0x0C;

	// ---- THE AUTHORITY -----------------------------------------------------------------------------
	// The camera entry at *(tls+0x148) + slot*0x1AC embeds a camera-MODE object at +0x08, and that object
	// is a union over 13 different mode classes. Under mode 2 - the flying camera, which is what HCM's
	// freecam turns on - the pose lives directly in it:
	//     +0x20  position, 3 floats
	//     +0x2C  yaw   (radians)
	//     +0x30  pitch (radians)
	//     +0x34  roll  (radians)   <- deliberately NOT touched; HCECameraRoll owns it
	// sub_1803A5DB0 (the mode-2 update) ADDS this frame's input to those fields, so a value written here is
	// preserved and flown onward from - which is why one write is enough and why input keeps working.
	constexpr int64_t kModeObject = 0x08;    // the vtable pointer of the camera-mode object
	constexpr int64_t kFlyPose = 0x20;       // pos[3] + yaw + pitch, contiguous
	constexpr uintptr_t kFlyingCameraVtableRva = 0x882318;   // mode 2

	// ⚠⚠ NEVER WRITE +0x20 WITHOUT CHECKING THE MODE FIRST.
	// +0x08 onward is a union. Under mode 7, +0x28 is a VTABLE POINTER and +0x20 is a word - writing a
	// float3 there corrupts a vptr. (Mode 7 keeps its own pose at +0x50 instead, which is also why HCM's
	// notes record cameraEntry+0x50 as "written once, no readers": it is mode 7's private cache, dead under
	// mode 2.) Comparing the vptr is one 8-byte read and never calls into a possibly-garbage object.
	// If the vtable address ever moves, the comparison simply fails and the feature does nothing - which is
	// the correct way for this to break.
	struct FlyPose
	{
		float position[3];
		float yaw;
		float pitch;
	};

	std::atomic<bool> gActive{ false };
	std::atomic<uintptr_t> gFlyingVtable{ 0 };   // simBase + kFlyingCameraVtableRva, resolved at arm time

	// Sim thread only.
	FlyPose gSaved{};
	FlyPose gCheckpointPose{};
	bool gHaveSnapshot = false;
	bool gHaveCheckpointPose = false;
	bool gRestoring = false;
	bool gWasFlying = false;
	int  gFramesLeft = 0;
	int32_t gLastTick = 0;
	bool gHaveTick = false;

	// Short, because writing the authority actually works. The old 900-frame window existed only because we
	// were fighting a per-frame rebuild we could never win.
	constexpr int kWatchFrames = 60;   // ~1 s at 60 Hz

	inline bool rdPtr(uintptr_t a, uintptr_t& v) { return HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)) && v != 0; }

	bool finitePose(const FlyPose& p) noexcept
	{
		const float* f = &p.position[0];
		for (int i = 0; i < 5; ++i)
			if (!std::isfinite(f[i]) || std::abs(f[i]) > 1.0e6f) return false;
		return true;
	}

	bool samePose(const FlyPose& a, const FlyPose& b) noexcept
	{
		return a.position[0] == b.position[0] && a.position[1] == b.position[1] && a.position[2] == b.position[2]
			&& a.yaw == b.yaw && a.pitch == b.pitch;
	}

	// The camera entry for the active camera. First live slot wins, same rule the engine uses.
	uintptr_t activeCameraEntry(uintptr_t tls) noexcept
	{
		uintptr_t entries = 0;
		if (!rdPtr(tls + kCameraEntriesTls, entries)) return 0;
		for (int slot = 0; slot < 4; ++slot)
		{
			const uintptr_t candidate = entries + (uintptr_t)slot * kCameraEntryStride;
			uintptr_t probe = 0;
			if (rdPtr(candidate, probe)) return candidate;
		}
		return 0;
	}

	bool isFlyingCamera(uintptr_t cameraEntry) noexcept
	{
		const uintptr_t want = gFlyingVtable.load(std::memory_order_relaxed);
		if (!want) return false;
		uintptr_t vptr = 0;
		return rdPtr(cameraEntry + kModeObject, vptr) && vptr == want;
	}

	// ================================================================================================
	// SIM THREAD, once per simulation frame, after the whole simulation update.
	//
	// ⚠⚠ WHY THIS WRITES THE CAMERA ENTRY AND NOT THE OBSERVER - THREE ATTEMPTS GOT THIS WRONG.
	//
	// The observer's nine floats (observer+0x154 position, +0x17C forward, +0x188 up) are NOT the camera.
	// They are the last stage of a four-stage derivation that is rebuilt FROM SCRATCH every simulation
	// frame: the mode object integrates input into cameraEntry+0x20, publishes a 0x104-byte camera block
	// into cameraEntry+0x78, observer+0x10 is pointed at that block, and observers_update recomputes the
	// nine floats from it. All of that happens inside `call sub_1801AE530`, before this hook runs.
	//
	// That is exactly why the earlier versions behaved as they did, and all three symptoms fall out of it:
	//   * writing the observer LOOKED like it worked - it is the final cache the renderer reads;
	//   * it ate all camera input - input had already been folded into the authority upstream;
	//   * and the moment we stopped, the next frame re-derived the cache from the checkpoint-restored
	//     authority and the camera snapped back. There was never any winning that fight, at any cadence.
	//
	// Writing the authority instead needs ONE write. sub_1803A5DB0 adds input to those same fields, so the
	// very next frame the user is already flying from the restored pose.
	// ================================================================================================
	void keepPositionPump(uintptr_t tls) noexcept
	{
		if (!gActive.load(std::memory_order_acquire)) return;

		// Revert detection: the game tick going backwards. game_time_globals is in game-state arena 4, which
		// a revert restores wholesale.
		uintptr_t timeGlobals = 0;
		if (rdPtr(tls + kGameTimeGlobalsTls, timeGlobals))
		{
			uint8_t initialised = 0;
			int32_t tick = 0;
			if (HCEGetPlayerState::tryReadRaw(timeGlobals, &initialised, sizeof(initialised)) && initialised
				&& HCEGetPlayerState::tryReadRaw(timeGlobals + kGameTick, &tick, sizeof(tick)))
			{
				if (gHaveTick && gHaveSnapshot && tick < gLastTick && !gRestoring)
				{
					gRestoring = true;
					gFramesLeft = kWatchFrames;
					gHaveCheckpointPose = false;
				}
				gLastTick = tick;
				gHaveTick = true;
			}
		}

		uintptr_t playerControl = 0;
		uint8_t master = 0;
		if (!rdPtr(tls + kPlayerControlTls, playerControl)) return;
		if (!HCEGetPlayerState::tryReadRaw(playerControl + kFreecamMaster, &master, sizeof(master))) return;
		if (master != 1) { gWasFlying = false; return; }

		const uintptr_t entry = activeCameraEntry(tls);
		if (!entry) { gWasFlying = false; return; }

		// The mode gate. Not the freecam mode - touch nothing, and remember that so we can write again on
		// the frame it comes back (a revert can restore a different mode, and the mode-2 constructor then
		// re-seeds the pose from the observer).
		if (!isFlyingCamera(entry))
		{
			gWasFlying = false;
			return;
		}
		const bool justBecameFlying = !gWasFlying;
		gWasFlying = true;

		FlyPose current{};
		if (!HCEGetPlayerState::tryReadRaw(entry + kFlyPose, &current, sizeof(current))) return;
		if (!finitePose(current)) return;

		if (gRestoring)
		{
			// Same selective rule as before, three links further upstream: only undo the engine's own
			// restore, never the user's flying. The first pass records what the revert put back; after that
			// we rewrite only while the authority still equals it. The instant the user moves, it stops
			// matching and we let go - so there is no input lock.
			if (!gHaveCheckpointPose)
			{
				gCheckpointPose = current;
				gHaveCheckpointPose = true;
				HCEGetPlayerState::tryWriteRaw(entry + kFlyPose, &gSaved, sizeof(gSaved));
			}
			else if (justBecameFlying || samePose(current, gCheckpointPose))
			{
				HCEGetPlayerState::tryWriteRaw(entry + kFlyPose, &gSaved, sizeof(gSaved));
			}

			if (--gFramesLeft <= 0)
			{
				gRestoring = false;
				gHaveCheckpointPose = false;
			}
			return;   // ⚠ never snapshot while restoring, or we would save the checkpoint's pose over ours
		}

		gSaved = current;
		gHaveSnapshot = true;
	}
}

class HCEFreecamKeepPosition::HCEFreecamKeepPositionImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::shared_ptr<HCEGetPlayerState> mPlayerState;
	std::shared_ptr<HCEGameThreadPumpHost> mPump;

	bool mRegistered = false;
	std::atomic<bool> mReady{ false };

	void setRegistered(bool wanted)
	{
		if (wanted && !mRegistered)
		{
			if (!HCEGameThreadPump::add(&keepPositionPump))
				throw HCMRuntimeException("Freecam Keep Position: the simulation-thread pump table is full");
			mRegistered = true;
		}
		else if (!wanted && mRegistered)
		{
			HCEGameThreadPump::remove(&keepPositionPump);
			mRegistered = false;
		}
	}

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (newValue)
			{
				// Resolve the mode-2 vtable now. If the sim module is not up we simply have no gate, and the
				// pump does nothing rather than writing into an unknown mode object.
				uintptr_t simBase = 0;
				try { simBase = mPlayerState->getSimModuleBase(); }
				catch (HCMRuntimeException&) { simBase = 0; }
				if (!simBase)
					throw HCMRuntimeException("Freecam Keep Position: the simulation module is not loaded yet");
				gFlyingVtable.store(simBase + kFlyingCameraVtableRva, std::memory_order_relaxed);

				gHaveSnapshot = false;
				gRestoring = false;
				gHaveTick = false;
				gWasFlying = false;
				setRegistered(true);
				gActive.store(true, std::memory_order_release);
			}
			else
			{
				gActive.store(false, std::memory_order_release);
				setRegistered(false);
			}

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Freecam will keep its position through a revert."
					: "Freecam will return to the checkpoint's camera again.");
		}
		catch (HCMRuntimeException ex)
		{
			gActive.store(false, std::memory_order_release);
			try { setRegistered(false); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	HCEFreecamKeepPositionImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFreecamKeepPositionToggle->valueChangedEvent, [this](bool& n) { onToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFreecamKeepPosition only supports Halo Campaign Evolved");

		mPlayerState = resolveDependentCheat(HCEGetPlayerState);
		mPump = resolveDependentCheat(HCEGameThreadPumpHost);
		mReady.store(true, std::memory_order_release);
	}

	~HCEFreecamKeepPositionImpl()
	{
		mReady.store(false, std::memory_order_release);
		gActive.store(false, std::memory_order_release);
		try { setRegistered(false); } catch (...) {}
		mToggleCallback.removeCallback();
	}
};


HCEFreecamKeepPosition::HCEFreecamKeepPosition(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEFreecamKeepPositionImpl>(game, dicon))
{
}

HCEFreecamKeepPosition::~HCEFreecamKeepPosition()
{
	PLOG_VERBOSE << "~" << getName();
}
