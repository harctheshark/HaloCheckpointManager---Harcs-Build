#include "pch.h"
#include "HCESkullToggler.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCESkullEnum.h"

// ================================================================================================================
// Halo Campaign Evolved skulls. Ported from HCM_Evolved gameplay.py (toggle_skull, _skull_flags_address and the
// skull half of maintain_enabled_toggles).
//
// Bit maths, verbatim from the reference:
//     byteAddress = flagsBase + bitIndex / 8
//     mask        = 1 << (bitIndex % 8)
// A toggle is a read-modify-write of ONE byte. The re-apply pass is a batched read-modify-write of all 7.
//
// The re-apply pass is OR-ONLY and compare-before-write, exactly like the reference. That is deliberate:
//   - OR-only means it never fights the user un-checking a skull (which the immediate write already did), and
//     never clears a skull the game itself set.
//   - compare-before-write means a bake-in that changes nothing costs one 7-byte read every 250 ms.
// Without it, every skull silently turns itself off on the next death or checkpoint reload, because the skull
// bitfield lives in the game-state TLS block that a checkpoint snapshots and restores.
// ================================================================================================================

// SettingsStateAndEvents::hceSkullEngineState is a fixed std::array shared with GUIHCESkullToggle. If the skull
// table ever grows, that array has to grow with it - catch the mismatch here rather than at index 56.
static_assert(std::tuple_size_v<std::remove_reference_t<decltype(std::declval<SettingsStateAndEvents&>().hceSkullEngineState)>> == kHCESkullCount,
	"SettingsStateAndEvents::hceSkullEngineState must have one entry per HCE skull");

class HCESkullToggler::HCESkullTogglerImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };

	// User intent. Only bits the user explicitly checked go in here, and only these get re-applied.
	// Not persisted to disk - the reference tool deliberately excludes toggle state from its config too.
	std::array<bool, kHCESkullCount> mWanted{};
	std::atomic<bool> mAnyWanted{ false };

	std::chrono::steady_clock::time_point mLastMaintain{};

	// Reads all 7 flag bytes. Returns false (silently) whenever the chain is legitimately down.
	bool readFlagBytes(uint8_t(&out)[kHCESkullByteCount], uintptr_t& flagsAddressOut)
	{
		auto playerState = playerStateWeak.lock();
		if (!playerState) return false;
		try
		{
			flagsAddressOut = playerState->getSkullFlagsAddress();
		}
		catch (HCMRuntimeException)
		{
			return false;
		}
		return HCEGetPlayerState::tryReadRaw(flagsAddressOut, out, sizeof(out));
	}

	// GUI -> here: refresh the shared engine-state array just before the widget renders it.
	void onUpdateEvent(GameState game)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER) return;

		auto settings = settingsWeak.lock();
		if (!settings) return;

		uint8_t bytes[kHCESkullByteCount]{};
		uintptr_t flagsAddress = 0;
		if (!readFlagBytes(bytes, flagsAddress))
		{
			settings->hceSkullStateValid = false;
			return;
		}

		for (size_t i = 0; i < kHCESkullCount; i++)
			settings->hceSkullEngineState[i] = (bytes[i / 8] & (1u << (i % 8))) != 0;

		settings->hceSkullStateValid = true;
	}

	// GUI -> here: the user clicked a checkbox.
	void onSetEvent(int bitIndex, bool enabled)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (bitIndex < 0 || bitIndex >= (int)kHCESkullCount) return;

		try
		{
			lockOrThrow(playerStateWeak, playerState);

			const uintptr_t flagsAddress = playerState->getSkullFlagsAddress();
			const uintptr_t byteAddress = flagsAddress + (bitIndex / 8);
			const uint8_t mask = (uint8_t)(1u << (bitIndex % 8));

			uint8_t current = 0;
			if (!HCEGetPlayerState::tryReadRaw(byteAddress, &current, sizeof(current)))
				throw HCMRuntimeException(std::format("{} failed: skull state is unavailable", kHCESkulls[bitIndex].displayName));

			const uint8_t updated = enabled ? (uint8_t)(current | mask) : (uint8_t)(current & ~mask);
			if (updated != current && !HCEGetPlayerState::tryWriteRaw(byteAddress, &updated, sizeof(updated)))
				throw HCMRuntimeException(std::format("{} failed: memory write failed", kHCESkulls[bitIndex].displayName));

			mWanted[bitIndex] = enabled;
			refreshAnyWanted();

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("{} {}.", kHCESkulls[bitIndex].displayName, enabled ? "enabled" : "disabled"));
		}
		catch (HCMRuntimeException ex)
		{
			// The next onUpdateEvent re-reads the truth from memory, so the checkbox corrects itself.
			runtimeExceptions->handleMessage(ex);
		}
	}

	void refreshAnyWanted()
	{
		bool any = false;
		for (bool b : mWanted) if (b) { any = true; break; }
		mAnyWanted.store(any, std::memory_order_release);
	}

	// Throttled OR-only re-assert of every wanted bit. Silent by design.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mAnyWanted.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		if (mLastMaintain.time_since_epoch().count() != 0 && (now - mLastMaintain) < std::chrono::milliseconds(250)) return;
		mLastMaintain = now;

		auto mccStateHook = mccStateHookWeak.lock();
		if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

		uint8_t current[kHCESkullByteCount]{};
		uintptr_t flagsAddress = 0;
		if (!readFlagBytes(current, flagsAddress)) return;

		uint8_t desired[kHCESkullByteCount];
		memcpy(desired, current, sizeof(desired));
		for (size_t i = 0; i < kHCESkullCount; i++)
			if (mWanted[i]) desired[i / 8] |= (uint8_t)(1u << (i % 8));

		if (memcmp(desired, current, sizeof(desired)) != 0)
			HCEGetPlayerState::tryWriteRaw(flagsAddress, desired, sizeof(desired));
	}

	// Declared LAST, see HCECheckpointDetours.cpp.
	ScopedCallback<eventpp::CallbackList<void(GameState)>> mUpdateCallback;
	ScopedCallback<eventpp::CallbackList<void(int, bool)>> mSetCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCESkullTogglerImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mUpdateCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceSkullUpdateEvent, [this](GameState g) { onUpdateEvent(g); }),
		mSetCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceSkullSetEvent, [this](int b, bool v) { onSetEvent(b, v); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCESkullToggler only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCESkullTogglerImpl()
	{
		mReady.store(false, std::memory_order_release);
		mUpdateCallback.removeCallback();
		mSetCallback.removeCallback();
		mRenderEventCallback.removeCallback();

		if (auto settings = settingsWeak.lock()) settings->hceSkullStateValid = false;
	}
};


HCESkullToggler::HCESkullToggler(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCESkullTogglerImpl>(game, dicon))
{
}

HCESkullToggler::~HCESkullToggler()
{
	PLOG_VERBOSE << "~" << getName();
}
