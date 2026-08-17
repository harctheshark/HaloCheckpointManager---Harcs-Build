#include "pch.h"
#include "HCEFieldOfView.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"
#include "ControlServiceContainer.h"
#include "HotkeyDefinitions.h"
#include <algorithm>
#include <cmath>
#include <mutex>

// See HCEFieldOfView.h for the derivation of LockedFOV and for why POV.FOV is deliberately not written.

namespace
{
	// APlayerCameraManager::LockedFOV. HORIZONTAL degrees; <= 0 means "not locked".
	constexpr int64_t kLockedFovOffset = 0x2F4;

	// 0.0f is the engine's own "unlocked" value - UnlockFOV writes exactly this.
	constexpr float kUnlocked = 0.f;

	// Must match the slider's range in GUIElementConstructor, or a held hotkey would wind the value somewhere
	// the slider cannot represent and the two would disagree.
	constexpr float kFovMinDegrees = 50.f;
	constexpr float kFovMaxDegrees = 140.f;

	// What "restore default" puts back. The engine's own default sits at DefaultFOV (camera manager + 0x2F0), so
	// that is read live rather than hard-coded here; this is only the fallback when the read fails.
	constexpr float kFovFallbackDefault = 78.f;

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
	std::weak_ptr<ControlServiceContainer> controlServicesWeak;

	// Shared with MCC's free camera and already defined and defaulted - we only borrow references. HaloCER and
	// the MCC games can never be in one process, so there is nothing to collide with.
	std::shared_ptr<RebindableHotkey> mFovIncreaseBinding;
	std::shared_ptr<RebindableHotkey> mFovDecreaseBinding;

	std::atomic<bool> mReady{ false };
	std::atomic<bool> mWantLock{ false };

	// Guards everything below it. Taken on the render thread (the poll) and on the hotkey / GUI threads (the
	// toggle), so it must never be held across anything that can block - it is not.
	std::mutex mMutex;
	bool mHookRequested = false;          // our half of HCEGetCameraData's reference count
	uintptr_t mLockedCameraManager = 0;   // the PCM currently holding OUR value, 0 = none
	uintptr_t mValidatedCameraManager = 0;// a PCM that passed looksLikeCameraManager, latched. 0 = not resolved yet
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

	// ⚠⚠ WHY THIS IS NOT JUST getPlayerCameraManager().
	//
	// That helper computes gPreferredDestination - 0x14B0, and gPreferredDestination is the CAMERA ELECTION's
	// current winner: the FMinimalViewInfo destination that has been written most in the last window. The midhook
	// sits on FMinimalViewInfo::operator=, so it sees EVERY view-info assignment in the process, and the election
	// re-elects roughly once a second, cycling among about six addresses (measured in the user's own logs).
	//
	// Only ONE of those is a POV embedded in an APlayerCameraManager. Subtract 0x14B0 from any of the others and
	// the result is a pointer into unrelated memory - and this cheat then writes four bytes at +0x2F4 into it.
	// That is a live memory corruption, and it is the reported crash. It is also why locking the FOV worked only
	// sometimes: when the election sat elsewhere we locked the wrong object, the real camera manager's LockedFOV
	// stayed 0, and the engine's own FOV won.
	//
	// So the candidate is VALIDATED before it is ever written to, and the first one that passes is LATCHED. The
	// real APlayerCameraManager is stable for the session, so following the election afterwards buys nothing and
	// risks everything.
	static constexpr int64_t kDefaultFovOffset = 0x2F0;   // reflected DefaultFOV, sits just before LockedFOV

	// A real APlayerCameraManager has a vtable inside the exe image and a sane DefaultFOV. A stray
	// FMinimalViewInfo offset back by 0x14B0 satisfies neither except by coincidence.
	bool looksLikeCameraManager(uintptr_t candidate) const
	{
		if (candidate < 0x10000) return false;

		uintptr_t vtable = 0;
		if (!HCEGetPlayerState::tryReadRaw(candidate, &vtable, sizeof(vtable)) || vtable < 0x10000) return false;

		const HMODULE exe = GetModuleHandleW(nullptr);
		MODULEINFO mi{};
		if (!exe || !GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi))) return false;
		const uintptr_t exeBase = (uintptr_t)mi.lpBaseOfDll;
		if (vtable < exeBase || vtable >= exeBase + mi.SizeOfImage) return false;

		float defaultFov = 0.f;
		if (!HCEGetPlayerState::tryReadRaw(candidate + kDefaultFovOffset, &defaultFov, sizeof(defaultFov)))
			return false;

		return std::isfinite(defaultFov) && defaultFov > 20.f && defaultFov < 170.f;
	}

	// The APlayerCameraManager the render camera belongs to. Throws rather than returning 0, because every caller
	// wants the reason, and "the midhook has not fired yet" is a completely different fault from "the cheat is
	// not wired up" - see the counters HCEGetCameraData keeps. CALLER MUST HOLD mMutex.
	uintptr_t resolveCameraManager() // throws HCMRuntimeException
	{
		// Keep using a camera manager we already proved, as long as it still looks like one. A level transition
		// is what invalidates it, and that shows up here as the validation failing.
		if (mValidatedCameraManager != 0)
		{
			if (looksLikeCameraManager(mValidatedCameraManager)) return mValidatedCameraManager;
			PLOG_DEBUG << "HCEFieldOfView: latched camera manager 0x" << std::hex << mValidatedCameraManager
				<< std::dec << " no longer validates; re-resolving";
			mValidatedCameraManager = 0;
		}

		lockOrThrow(cameraDataWeak, cameraData);

		const uintptr_t cameraManager = cameraData->getPlayerCameraManager();
		if (cameraManager == 0)
			throw HCMRuntimeException(std::format(
				"No Halo Campaign Evolved render camera is available yet (DoUpdateCamera's midhook has fired {} "
				"time(s) and the hook is {}installed). Load into a level and try again.",
				cameraData->getHookFireCount(), cameraData->isHookInstalled() ? "" : "NOT "));

		if (!looksLikeCameraManager(cameraManager))
			throw HCMRuntimeException(std::format(
				"The Halo Campaign Evolved render camera HCM is currently tracking (0x{:X}) is not a player camera "
				"manager, so its field of view cannot be locked safely. This is normal for a moment after a level "
				"loads or during a cutscene - try again once you are in control.", cameraManager));

		mValidatedCameraManager = cameraManager;
		PLOG_DEBUG << "HCEFieldOfView: latched camera manager 0x" << std::hex << cameraManager << std::dec;
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
	// Winds the FOV slider while a key is held, exactly as HCECameraRoll does for tilt. Only meaningful while the
	// lock is on - the value has nowhere to go otherwise, and silently moving a slider the game is ignoring
	// would just look broken.
	static constexpr float kFovDegreesPerSecond = 40.f;

	// Button and hotkey share one event. "Default" is read LIVE out of the engine rather than hard-coded: the
	// camera manager's own DefaultFOV at +0x2F0 is exactly what GetFOVAngle falls back to when LockedFOV is
	// unset, so it is right for this level even if the game varies it. kFovFallbackDefault is only for when the
	// read fails.
	//
	// This restores the VALUE, it does not switch the lock off - so pressing it while locked snaps you back to
	// the game's framing without also handing the field of view back mid-look. Switch the toggle off for that.
	void onResetFov()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			auto settings = settingsWeak.lock();
			if (!settings) return;

			float engineDefault = kFovFallbackDefault;
			{
				std::scoped_lock lock(mMutex);
				try
				{
					const uintptr_t manager = resolveCameraManager();
					float candidate = 0.f;
					if (HCEGetPlayerState::tryReadRaw(manager + kDefaultFovOffset, &candidate, sizeof(candidate))
						&& std::isfinite(candidate) && candidate >= kFovMinDegrees && candidate <= kFovMaxDegrees)
					{
						engineDefault = candidate;
					}
				}
				catch (HCMRuntimeException)
				{
					// No camera resolved yet - the fallback is still a sane thing to put in the slider.
				}
			}

			auto& setting = settings->hceFieldOfViewDegrees;
			setting->GetValueDisplay() = engineDefault;
			setting->UpdateValueWithInput();   // fires onValueChanged, which writes it if the lock is on

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("Field of view reset to {:.1f} degrees.", engineDefault));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	bool pollFovHotkeys()
	{
		if (!mFovIncreaseBinding || !mFovDecreaseBinding) return false;

		// Respect the SAME disabler HotkeyManager::pollInput respects, so holding the binding while rebinding a
		// key or typing in a text box cannot wind the field of view.
		if (auto controls = controlServicesWeak.lock())
			if (controls->hotkeyDisabler && controls->hotkeyDisabler->serviceIsRequested()) return false;

		const bool up = mFovIncreaseBinding->isCurrentlyDown();
		const bool down = mFovDecreaseBinding->isCurrentlyDown();
		if (up == down) return false;   // neither, or both cancelling out

		auto settings = settingsWeak.lock();
		if (!settings) return false;

		const float frameDelta = ImGui::GetIO().DeltaTime;   // clamped by ImGui, so a stall cannot jump
		auto& setting = settings->hceFieldOfViewDegrees;
		const float next = std::clamp(setting->GetValue() + kFovDegreesPerSecond * frameDelta * (up ? 1.f : -1.f),
			kFovMinDegrees, kFovMaxDegrees);

		setting->GetValueDisplay() = next;
		setting->UpdateValueWithInput();   // fires the value-changed handler, which does the write
		return true;
	}

	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantLock.load(std::memory_order_acquire)) return;

		pollFovHotkeys();

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
	ScopedCallback<ActionEvent> mResetCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEFieldOfViewImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		cameraDataWeak(resolveDependentCheat(HCEGetCameraData)),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		controlServicesWeak(dicon.Resolve<ControlServiceContainer>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFieldOfViewToggle->valueChangedEvent, [this](bool& n) { onToggleChanged(n); }),
		mValueCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFieldOfViewDegrees->valueChangedEvent, [this](float& n) { onValueChanged(n); }),
		mResetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFieldOfViewResetEvent, [this]() { onResetFov(); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFieldOfView only supports Halo Campaign Evolved");

		// Same borrow HCECameraRoll does for its tilt keys: these are MCC free-camera bindings that already
		// exist and are already defaulted, and the two games can never share a process. find() rather than at(),
		// so a missing definition surfaces as an HCMInitException instead of std::out_of_range.
		auto& hkd = dicon.Resolve<HotkeyDefinitions>().lock()->getAllRebindableHotkeys();
		auto increase = hkd.find(RebindableHotkeyEnum::cameraFOVIncreaseBinding);
		auto decrease = hkd.find(RebindableHotkeyEnum::cameraFOVDecreaseBinding);
		if (increase == hkd.end() || decrease == hkd.end())
			throw HCMInitException("The camera field-of-view hotkeys are missing from the hotkey definitions");
		mFovIncreaseBinding = increase->second;
		mFovDecreaseBinding = decrease->second;

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
