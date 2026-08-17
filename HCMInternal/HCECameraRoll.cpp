#include "pch.h"
#include "HCECameraRoll.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HotkeyDefinitions.h"
#include "ControlServiceContainer.h"
#include "imgui.h"
#include <cmath>

// See HCECameraRoll.h for where every offset comes from and why this is NOT the gimbal lock bypass.

namespace
{
	// Camera entry layout. Position (+0x20) is already used by HCEFreecam's "teleport to camera"; roll is the
	// third of the three angle floats that follow it.
	constexpr int64_t kCameraRollOffset = 0x34;   // float, RADIANS

	// How fast the roll hotkeys wind the value, in DEGREES PER SECOND. Deliberately frame-rate independent - the
	// same key held for the same wall-clock time gives the same roll at 60 fps and at 240.
	constexpr float kRollDegreesPerSecond = 60.f;

	// Same cadence as HCEFreecam's freecam re-assert, for the same reason: a camera-mode change zeroes the field
	// behind our back and there is no event that tells us it happened.
	constexpr auto kReassertInterval = std::chrono::milliseconds(250);

	// Keep the value inside the slider's own range, so winding past the end with a hotkey wraps rather than
	// piling up against a validator that would silently reject it.
	float wrapDegrees(float degrees)
	{
		if (!std::isfinite(degrees)) return 0.f;
		while (degrees > 180.f) degrees -= 360.f;
		while (degrees < -180.f) degrees += 360.f;
		return degrees;
	}
}


class HCECameraRoll::HCECameraRollImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	// Only the reset action needs this: the slider moving is its own feedback, but a hotkey press with the menu
	// closed would otherwise be silent.
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<ControlServiceContainer> controlServicesWeak;

	// The two bindings MCC's free camera already owns. Held directly rather than going through
	// UserCameraInputReader - see the header.
	std::shared_ptr<RebindableHotkey> mRollLeftBinding;
	std::shared_ptr<RebindableHotkey> mRollRightBinding;

	std::atomic<bool> mReady{ false };

	// Touched only by the render thread.
	std::chrono::steady_clock::time_point mLastReassert{};

	// Written by the render thread's throttled pass and read by the GUI / hotkey threads, hence atomic. A stale
	// read costs at most one redundant TLS walk or one duplicate log line, never a bad write.
	std::atomic<bool> mFreecamOnCached{ false };
	std::atomic<uintptr_t> mLastLoggedEntry{ 0 };   // logging only - see the PLOG_DEBUG in writeRoll
	std::atomic<bool> mEverApplied{ false };

	// Is the ENGINE's free camera currently on? Reads the master byte of the trio HCEFreecam documents at
	// +0x9C8. Deliberately reads the engine rather than HCM's own toggle: the player can turn the camera on with
	// the game's own key bind, and a checkpoint revert can turn it off under both of us.
	bool freecamIsOn() // throws HCMRuntimeException
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t address = playerState->getFreecamToggleAddress();
		uint8_t master = 0;
		if (!HCEGetPlayerState::tryReadRaw(address, &master, sizeof(master)))
		{
			playerState->invalidateCache();
			throw HCMRuntimeException("Could not read the Halo Campaign Evolved camera state");
		}
		return master == 1;
	}

	// Cheap variant for paths that can run every frame. A cached TRUE was verified within the last 250 ms by the
	// throttled pass and is good enough; a cached FALSE is re-checked, because the alternative is a slider that
	// does nothing for a quarter of a second after the camera comes on.
	bool freecamIsOnCachedOrFresh() // throws HCMRuntimeException
	{
		if (mFreecamOnCached.load(std::memory_order_acquire)) return true;
		const bool on = freecamIsOn();
		mFreecamOnCached.store(on, std::memory_order_release);
		return on;
	}

	void writeRoll(float degrees) // throws HCMRuntimeException
	{
		lockOrThrow(playerStateWeak, playerState);

		const uintptr_t cameraEntry = playerState->getActiveCameraEntry();
		const uintptr_t rollAddress = cameraEntry + kCameraRollOffset;
		const float radians = DirectX::XMConvertToRadians(degrees);

		if (!HCEGetPlayerState::tryWriteRaw(rollAddress, &radians, sizeof(radians)))
		{
			playerState->invalidateCache();
			throw HCMRuntimeException(std::format(
				"Could not write the Halo Campaign Evolved camera roll at 0x{:X} (camera entry 0x{:X})",
				rollAddress, cameraEntry));
		}

		mEverApplied.store(true, std::memory_order_release);

		// ⚠ THE ONE LINE A LIVE TEST NEEDS. Neither of this feature's offsets has ever been confirmed in game, so
		// the log has to say exactly what was written and where - "the roll slider did nothing" is unactionable,
		// "wrote 0.5236 rad to 0x1F2A3B4C34 (entry 0x1F2A3B4C00)" is not. Logged when the camera entry CHANGES
		// rather than every pass, so the 4 Hz re-assert cannot bury the log.
		if (mLastLoggedEntry.exchange(cameraEntry, std::memory_order_acq_rel) != cameraEntry)
		{
			PLOG_DEBUG << "HCECameraRoll: camera entry 0x" << std::hex << cameraEntry << ", roll field 0x"
				<< rollAddress << std::dec << " (entry + 0x34); wrote " << degrees << " degrees (" << radians
				<< " radians)";
		}
	}

	// The current slider value, or 0 if the settings object has gone.
	float wantedRoll()
	{
		auto settings = settingsWeak.lock();
		if (!settings) return 0.f;
		return wrapDegrees(settings->hceCameraRollDegrees->GetValue());
	}

	// Fired by the slider AND by the roll hotkeys below (both go through UpdateValueWithInput, so there is one
	// path into the game and one place that can fail).
	void onRollChanged(float&)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			// Roll applies to the free camera only - see the header. Silently doing nothing here is correct and
			// is NOT the "silent no-op" the brief forbids: the feature is simply not in scope right now, and the
			// tooltip says so.
			if (!freecamIsOnCachedOrFresh()) return;

			writeRoll(wantedRoll());
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Camera roll failed: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Winds the setting while a roll key is held. Runs on the render thread inside ImGui's frame, which is the
	// only place ImGui's key state and frame delta are meaningful.
	// Returns true if the value was changed (in which case onRollChanged has already run and written it).
	// Button and hotkey both land here - they share one event, so there is nothing to keep in sync. Zero is the
	// real default: the engine initialises roll to zero and every camera-mode change resets it there.
	void onResetRoll()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			auto settings = settingsWeak.lock();
			if (!settings) return;

			auto& setting = settings->hceCameraRollDegrees;
			setting->GetValueDisplay() = 0.f;
			setting->UpdateValueWithInput();   // fires onRollChanged, which writes it

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage("Camera tilt reset.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	bool pollRollHotkeys()
	{
		if (!mRollLeftBinding || !mRollRightBinding) return false;

		// Respect the SAME disabler HotkeyManager::pollInput respects, so rebinding a key or typing in a text box
		// cannot roll the camera. Without this, holding the default binding (G / T) while typing a trigger-name
		// filter would spin the view.
		if (auto controls = controlServicesWeak.lock())
			if (controls->hotkeyDisabler && controls->hotkeyDisabler->serviceIsRequested()) return false;

		const bool left = mRollLeftBinding->isCurrentlyDown();
		const bool right = mRollRightBinding->isCurrentlyDown();
		if (left == right) return false;   // neither, or both cancelling out

		auto settings = settingsWeak.lock();
		if (!settings) return false;

		// ImGui's delta is clamped and never zero, so a stall cannot produce a huge jump.
		const float frameDelta = ImGui::GetIO().DeltaTime;
		const float step = kRollDegreesPerSecond * frameDelta * (right ? 1.f : -1.f);

		auto& setting = settings->hceCameraRollDegrees;
		setting->GetValueDisplay() = wrapDegrees(setting->GetValue() + step);
		setting->UpdateValueWithInput();   // fires onRollChanged, which does the write
		return true;
	}

	// Throttled re-assert, silent by design - exactly the shape of HCEFreecam::onRenderEvent, and there for the
	// same reason: a camera-mode change (cutscene, freecam toggled off and on, a revert) zeroes the roll field
	// with no event to tell us.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;

		try
		{
			auto mccStateHook = mccStateHookWeak.lock();
			if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			// ⚠ THE FREECAM CHECK IS CACHED, AND IT GATES THE HOTKEYS TOO. Answering "is the free camera on"
			// costs a TLS walk, and the hotkey poll runs EVERY frame, so asking per frame would put that walk on
			// the render thread hundreds of times a second for a byte that changes about once a minute. Refreshed
			// on the same 250 ms tick as the re-assert. The only cost is that the roll keys can take up to a
			// quarter second to start responding after freecam comes on - which is also what stops them silently
			// winding the slider during normal play, when nothing would apply it.
			const auto now = std::chrono::steady_clock::now();
			const bool due = mLastReassert.time_since_epoch().count() == 0 || (now - mLastReassert) >= kReassertInterval;
			if (due)
			{
				mLastReassert = now;
				const bool wasOn = mFreecamOnCached.exchange(false, std::memory_order_acq_rel);
				const bool isOn = freecamIsOn();   // a throw here leaves the cache FALSE, which is the safe answer
				mFreecamOnCached.store(isOn, std::memory_order_release);
				// The engine zeroes roll on its way out of freecam, so forget the camera we were writing - the
				// next one earns its own log line.
				if (wasOn && !isOn) mLastLoggedEntry.store(0, std::memory_order_release);
			}

			if (!mFreecamOnCached.load(std::memory_order_acquire)) return;

			// Hotkeys next: if one moved the value it has already been written through onRollChanged, so the
			// re-assert below has nothing left to do this frame.
			if (pollRollHotkeys()) return;
			if (!due) return;

			const float wanted = wantedRoll();
			if (wanted == 0.f && !mEverApplied) return;   // nothing to hold, and nothing of ours to restore

			// Only write when the field has actually drifted from what we want. A read is far cheaper than the
			// write plus its failure handling, and it keeps us off the bus four times a second for nothing.
			auto playerState = playerStateWeak.lock();
			if (!playerState) return;
			const uintptr_t rollAddress = playerState->getActiveCameraEntry() + kCameraRollOffset;
			float current = 0.f;
			if (HCEGetPlayerState::tryReadRaw(rollAddress, &current, sizeof(current)))
			{
				const float wantedRadians = DirectX::XMConvertToRadians(wanted);
				if (std::isfinite(current) && std::abs(current - wantedRadians) < 1e-5f) return;
			}

			writeRoll(wanted);
		}
		catch (HCMRuntimeException)
		{
			// Expected constantly - at a menu, mid-load, and while the player is dead. The user-facing path is
			// onRollChanged; a throttled background pass must not spam the message feed.
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<eventpp::CallbackList<void(float&)>> mRollChangedCallback;
	ScopedCallback<ActionEvent> mResetCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCECameraRollImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		controlServicesWeak(dicon.Resolve<ControlServiceContainer>()),
		mRollChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceCameraRollDegrees->valueChangedEvent, [this](float& n) { onRollChanged(n); }),
		mResetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceCameraRollResetEvent, [this]() { onResetRoll(); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCECameraRoll only supports Halo Campaign Evolved");

		// The bindings are shared with MCC's free camera and are already defined and defaulted (G / T) - we only
		// borrow references to them. at() would throw std::out_of_range rather than an HCMInitException, so ask
		// first and convert.
		auto& hkd = dicon.Resolve<HotkeyDefinitions>().lock()->getAllRebindableHotkeys();
		auto left = hkd.find(RebindableHotkeyEnum::cameraRollLeftBinding);
		auto right = hkd.find(RebindableHotkeyEnum::cameraRollRightBinding);
		if (left == hkd.end() || right == hkd.end())
			throw HCMInitException("The camera roll hotkeys are missing from the hotkey definitions");
		mRollLeftBinding = left->second;
		mRollRightBinding = right->second;

		mReady.store(true, std::memory_order_release);
	}

	~HCECameraRollImpl()
	{
		mReady.store(false, std::memory_order_release);
		mRollChangedCallback.removeCallback();
		mRenderEventCallback.removeCallback();

		// Level the camera back out. The engine would do it on the next camera-mode change anyway, but leaving a
		// tilted view behind after HCM unloads reads as the GAME being broken - the same reasoning that keeps
		// Disable Fade From Black out of the serialised settings.
		try
		{
			if (mEverApplied && freecamIsOn()) writeRoll(0.f);
		}
		catch (...) {}
	}
};


HCECameraRoll::HCECameraRoll(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCECameraRollImpl>(game, dicon))
{
}

HCECameraRoll::~HCECameraRoll()
{
	PLOG_VERBOSE << "~" << getName();
}
