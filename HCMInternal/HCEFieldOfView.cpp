#include "pch.h"
#include "HCEFieldOfView.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include <cmath>
#include <mutex>

// See HCEFieldOfView.h for the derivation of LockedFOV and for why POV.FOV is deliberately not written.

namespace
{
	// APlayerCameraManager::LockedFOV. HORIZONTAL degrees; <= 0 means "not locked".
	constexpr int64_t kLockedFovOffset = 0x2F4;

	// 0.0f is the engine's own "unlocked" value - UnlockFOV writes exactly this.
	constexpr float kUnlocked = 0.f;

	// Cheap enough at 4 Hz to be free, frequent enough that a level transition re-locks before the player has
	// finished loading in. See the header for why a "one-shot" write still needs this.
	constexpr auto kPollInterval = std::chrono::milliseconds(250);

	// How long the pass may fail to resolve or write before it stops being "transient" and becomes something the
	// user is told about. A level load easily takes several seconds, hence the generous window.
	constexpr auto kFailureGraceInterval = std::chrono::seconds(8);

	// What we are willing to believe is a LockedFOV field before we write to it for the first time. The engine
	// leaves it at exactly 0 in normal play; anything else finite and inside a sane FOV range is still plausible
	// (somebody, including us in a previous session, locked it). Anything ELSE means the pointer arithmetic is
	// wrong for this build and we must not write.
	bool plausibleLockedFov(float value)
	{
		return std::isfinite(value) && value >= 0.f && value <= 180.f;
	}
}


class HCEFieldOfView::HCEFieldOfViewImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetCameraData> cameraDataWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	std::atomic<bool> mReady{ false };
	std::atomic<bool> mWantLock{ false };

	// Guards everything below it. Taken on the render thread (the poll) and on the hotkey / GUI threads (the
	// toggle), so it must never be held across anything that can block - it is not.
	std::mutex mMutex;
	bool mHookRequested = false;          // our half of HCEGetCameraData's reference count
	uintptr_t mLockedCameraManager = 0;   // the PCM currently holding OUR value, 0 = none
	float mLockedValue = 0.f;             // what we last wrote there
	std::chrono::steady_clock::time_point mLastPoll{};
	std::chrono::steady_clock::time_point mFailingSince{};
	bool mReportedFailure = false;

	float wantedFov()
	{
		auto settings = settingsWeak.lock();
		if (!settings) return 0.f;
		return settings->hceFieldOfViewDegrees->GetValue();
	}

	// The APlayerCameraManager the render camera belongs to. Throws rather than returning 0, because every caller
	// wants the reason, and "the midhook has not fired yet" is a completely different fault from "the cheat is
	// not wired up" - see the counters HCEGetCameraData keeps.
	uintptr_t resolveCameraManager() // throws HCMRuntimeException
	{
		lockOrThrow(cameraDataWeak, cameraData);

		const uintptr_t cameraManager = cameraData->getPlayerCameraManager();
		if (cameraManager == 0)
			throw HCMRuntimeException(std::format(
				"No Halo Campaign Evolved render camera is available yet (DoUpdateCamera's midhook has fired {} "
				"time(s) and the hook is {}installed). Load into a level and try again.",
				cameraData->getHookFireCount(), cameraData->isHookInstalled() ? "" : "NOT "));

		return cameraManager;
	}

	// Writes LockedFOV. `value` of 0 unlocks. CALLER MUST HOLD mMutex.
	void writeLockedFov(uintptr_t cameraManager, float value, bool firstWriteToThisCamera) // throws
	{
		const uintptr_t address = cameraManager + kLockedFovOffset;

		// ⚠ LOOK BEFORE WRITING. This offset has never been confirmed in game. A read costs nothing, is made
		// harmless by sehCopy, and turns "HCM wrote a float into the middle of some unrelated object" into a
		// message that names the address and the value it found.
		float existing = 0.f;
		if (!HCEGetPlayerState::tryReadRaw(address, &existing, sizeof(existing)))
			throw HCMRuntimeException(std::format(
				"Could not read Halo Campaign Evolved's LockedFOV at 0x{:X} (camera manager 0x{:X}). The camera "
				"pointer is stale, or this build's APlayerCameraManager layout is different.", address, cameraManager));

		if (firstWriteToThisCamera && !plausibleLockedFov(existing))
			throw HCMRuntimeException(std::format(
				"Refusing to write the Halo Campaign Evolved field of view: LockedFOV at 0x{:X} (camera manager "
				"0x{:X}) reads back {}, which is not a field-of-view value. That means the +0x2F4 offset is wrong "
				"for this build of the game, and writing there would corrupt something else.",
				address, cameraManager, existing));

		if (!HCEGetPlayerState::tryWriteRaw(address, &value, sizeof(value)))
			throw HCMRuntimeException(std::format(
				"Could not write the Halo Campaign Evolved field of view at 0x{:X} (camera manager 0x{:X})",
				address, cameraManager));

		// ⚠ THE LINES A LIVE TEST NEEDS. Only on a FIRST write to a given camera manager, so the 4 Hz poll cannot
		// bury them. Everything needed to check the RE by hand is here: the elected POV, the camera manager it
		// was derived from, the exact address written, and what was there before.
		if (firstWriteToThisCamera)
		{
			auto cameraData = cameraDataWeak.lock();
			const uintptr_t pov = cameraData ? cameraData->getElectedPovAddress() : 0;
			PLOG_DEBUG << "HCEFieldOfView: elected POV 0x" << std::hex << pov << " - 0x"
				<< HCEGetCameraData::kPovInCameraManagerOffset << " = camera manager 0x" << cameraManager
				<< "; LockedFOV at 0x" << address << std::dec << " was " << existing << ", wrote " << value
				<< (value == kUnlocked ? " (unlocked)" : " (locked, horizontal degrees)");
		}
	}

	// CALLER MUST HOLD mMutex. Hands the previously locked camera back to the engine, if there is one.
	void unlockPrevious()
	{
		if (mLockedCameraManager == 0) return;
		try
		{
			writeLockedFov(mLockedCameraManager, kUnlocked, false);
			PLOG_DEBUG << "HCEFieldOfView: unlocked camera manager 0x" << std::hex << mLockedCameraManager << std::dec;
		}
		catch (HCMRuntimeException& ex)
		{
			// The old camera is usually gone by now (that is WHY we are moving), so this is expected and must not
			// stop us locking the new one.
			PLOG_VERBOSE << "HCEFieldOfView: could not unlock the previous camera manager: " << ex.what();
		}
		catch (...) {}
		mLockedCameraManager = 0;
		mLockedValue = 0.f;
	}

	// CALLER MUST HOLD mMutex.
	void applyLocked(uintptr_t cameraManager, float value) // throws
	{
		// "First write" gates the plausibility check and the log line, so it has to be decided BEFORE
		// unlockPrevious() clears mLockedCameraManager.
		const bool firstWriteToThisCamera = (cameraManager != mLockedCameraManager);
		if (firstWriteToThisCamera) unlockPrevious();

		writeLockedFov(cameraManager, value, firstWriteToThisCamera);
		mLockedCameraManager = cameraManager;
		mLockedValue = value;
	}

	void onToggleChanged(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			mWantLock.store(newValue, std::memory_order_release);

			// The midhook lives on the EXE, which is always loaded, so this is safe whether or not a level is up.
			// It is reference counted per requester, so turning this off cannot rip the hook out from under the
			// trigger overlay or the 3D renderer.
			// ⚠ NOT called from the render thread - hotkey events fire on their own thread (HotkeyManager) and
			// GUI toggles fire on a detached thread (GUISimpleToggle), which is what makes this legal here.
			lockOrThrow(cameraDataWeak, cameraData);
			{
				std::scoped_lock lock(mMutex);
				if (newValue != mHookRequested)
				{
					// ⚠ RECORD THE REQUEST BEFORE MAKING IT. setHookWanted registers the requester and THEN
					// applies the hook state, so it can throw with us already in its requester set. Recording
					// afterwards would lose that, and the destructor would never balance the reference count.
					mHookRequested = newValue;
					cameraData->setHookWanted(this, newValue);
				}
			}

			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (!newValue)
			{
				std::scoped_lock lock(mMutex);
				unlockPrevious();
				mReportedFailure = false;
				mFailingSince = {};
				messagesGUI->addMessage("Field of view restored to the game's.");
				return;
			}

			// ⚠ TURNING IT ON AT A MENU IS NOT A FAILURE. The render camera does not exist until a level is up, so
			// throwing an on-screen error here would fire every time somebody armed this from the main menu. The
			// grace window below starts now instead, and the poll surfaces a REAL failure (once) if the camera
			// never turns up. What is never allowed is saying nothing at all - hence the two messages.
			const float wanted = wantedFov();
			bool applied = false;
			{
				std::scoped_lock lock(mMutex);
				mFailingSince = std::chrono::steady_clock::now();   // start the grace window
				mReportedFailure = false;
				try
				{
					applyLocked(resolveCameraManager(), wanted);
					mFailingSince = {};
					applied = true;
				}
				catch (HCMRuntimeException& ex)
				{
					PLOG_DEBUG << "HCEFieldOfView: could not lock at toggle time, leaving it to the poll: " << ex.what();
				}
			}
			messagesGUI->addMessage(applied
				? std::format("Field of view locked to {:.1f} degrees.", wanted)
				: std::format("Field of view will lock to {:.1f} degrees once the camera is live.", wanted));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Field of view: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onValueChanged(float&)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantLock.load(std::memory_order_acquire)) return;
		try
		{
			const float wanted = wantedFov();
			std::scoped_lock lock(mMutex);
			applyLocked(resolveCameraManager(), wanted);
		}
		catch (HCMRuntimeException& ex)
		{
			// Dragging a slider while the camera is down would otherwise throw a message per frame. The poll owns
			// reporting a persistent failure; this path only owns the log.
			PLOG_VERBOSE << "HCEFieldOfView: value change could not be applied yet: " << ex.what();
		}
	}

	// Throttled READ that writes only when the camera manager changed under us or the field no longer holds our
	// value. Never installs or touches the hook - it runs on the render thread, where that is forbidden.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantLock.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		{
			std::scoped_lock lock(mMutex);
			if (mLastPoll.time_since_epoch().count() != 0 && (now - mLastPoll) < kPollInterval) return;
			mLastPoll = now;
		}

		auto mccStateHook = mccStateHookWeak.lock();
		if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

		try
		{
			const uintptr_t cameraManager = resolveCameraManager();
			const float wanted = wantedFov();

			std::scoped_lock lock(mMutex);

			if (cameraManager == mLockedCameraManager && wanted == mLockedValue)
			{
				// Same camera, same value - only rewrite if something else cleared the field (a level's own
				// UnlockFOV call, most likely).
				float current = 0.f;
				if (HCEGetPlayerState::tryReadRaw(cameraManager + kLockedFovOffset, &current, sizeof(current))
					&& current == wanted)
				{
					mFailingSince = {};
					mReportedFailure = false;
					return;
				}
			}

			applyLocked(cameraManager, wanted);
			mFailingSince = {};
			mReportedFailure = false;
		}
		catch (HCMRuntimeException ex)
		{
			// ⚠ NOT SILENT. A cheat whose toggle is ON and which is doing nothing must eventually say so - the
			// alternative is a user staring at an unchanged FOV with nothing in the message feed and nothing in
			// the log. Transient failures (loads, menus, camera changes) are swallowed for the grace window; a
			// persistent one is reported ONCE, and reporting re-arms as soon as a pass succeeds.
			std::scoped_lock lock(mMutex);
			if (mFailingSince.time_since_epoch().count() == 0) mFailingSince = now;
			if (mReportedFailure || (now - mFailingSince) < kFailureGraceInterval) return;

			mReportedFailure = true;
			ex.prepend("Field of view has been unable to apply: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mValueCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEFieldOfViewImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		cameraDataWeak(resolveDependentCheat(HCEGetCameraData)),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFieldOfViewToggle->valueChangedEvent, [this](bool& n) { onToggleChanged(n); }),
		mValueCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFieldOfViewDegrees->valueChangedEvent, [this](float& n) { onValueChanged(n); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFieldOfView only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEFieldOfViewImpl()
	{
		mReady.store(false, std::memory_order_release);
		mWantLock.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mValueCallback.removeCallback();
		mRenderEventCallback.removeCallback();

		// ⚠ HAND THE FOV BACK. Skipping this leaves the game permanently zoomed with HCM gone and nothing to
		// blame. See the header.
		try
		{
			std::scoped_lock lock(mMutex);
			unlockPrevious();
			if (mHookRequested)
			{
				if (auto cameraData = cameraDataWeak.lock()) cameraData->setHookWanted(this, false);
				mHookRequested = false;
			}
		}
		catch (...) {}
	}
};


HCEFieldOfView::HCEFieldOfView(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEFieldOfViewImpl>(game, dicon))
{
}

HCEFieldOfView::~HCEFieldOfView()
{
	PLOG_VERBOSE << "~" << getName();
}
