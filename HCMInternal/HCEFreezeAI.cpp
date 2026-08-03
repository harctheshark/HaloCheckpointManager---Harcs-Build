#include "pch.h"
#include "HCEFreezeAI.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"

// ================================================================================================================
// Halo Campaign Evolved Freeze AI.
//
// The whole feature is one byte: *( *(tls + 0x40) ) = 0 freezes, 1 resumes.
//
// The re-apply pass matters as much as the write. ai_enabled lives inside the game-state TLS block, which is
// exactly the memory a checkpoint snapshots and restores. Every revert, death and natural checkpoint reload puts
// the engine's value back, so a write-once implementation appears to work and then silently un-freezes.
// HCM_Evolved solves this with a 250 ms polling loop; here it hangs off BackgroundRenderEvent (which the D3D12
// present path fires - see ImGuiManager::onPresentHookEventD3D12) with the same 250 ms throttle. Like the
// reference, the pass only ever ASSERTS the enabled state and compares before writing, so it never fights the
// user turning the feature off and never writes when nothing changed.
// ================================================================================================================

class HCEFreezeAI::HCEFreezeAIImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };
	std::atomic<bool> mWantFrozen{ false };
	std::chrono::steady_clock::time_point mLastMaintain{};

	void applyFreeze(bool frozen) // throws HCMRuntimeException
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t address = playerState->getAiEnabledAddress();
		const uint8_t value = frozen ? 0 : 1;
		if (!HCEGetPlayerState::tryWriteRaw(address, &value, sizeof(value)))
		{
			playerState->invalidateCache();
			throw HCMRuntimeException("Could not write the Halo Campaign Evolved AI state");
		}
	}

	void onToggleChange(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			mWantFrozen.store(newValue, std::memory_order_release);

			// Not in game yet? The maintain pass will apply it as soon as the chain resolves.
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			applyFreeze(newValue);
			messagesGUI->addMessage(newValue ? "AI frozen." : "AI unfrozen.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Throttled re-assert. Deliberately silent: at a menu, mid-load or while dead the chain is legitimately
	// unresolvable and there is nothing wrong, so this must never throw, message or log-spam.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantFrozen.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		if (mLastMaintain.time_since_epoch().count() != 0 && (now - mLastMaintain) < std::chrono::milliseconds(250)) return;
		mLastMaintain = now;

		try
		{
			auto mccStateHook = mccStateHookWeak.lock();
			if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			auto playerState = playerStateWeak.lock();
			if (!playerState) return;

			const uintptr_t address = playerState->getAiEnabledAddress();
			uint8_t current = 1;
			if (!HCEGetPlayerState::tryReadRaw(address, &current, sizeof(current))) return;
			if (current == 0) return; // already frozen, nothing to do

			const uint8_t frozen = 0;
			HCEGetPlayerState::tryWriteRaw(address, &frozen, sizeof(frozen));
		}
		catch (HCMRuntimeException)
		{
			// expected while the chain is down
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor, and cheats are built on detached
	// threads while the hotkey thread is already live. See the same note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallbackHandle;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEFreezeAIImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->aiFreezeToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		// explicit cast - GameState has operator==(GameState) AND operator Value(), so a direct compare is C2666
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFreezeAI only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEFreezeAIImpl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallbackHandle.removeCallback();
		mRenderEventCallback.removeCallback();
	}
};


HCEFreezeAI::HCEFreezeAI(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEFreezeAIImpl>(game, dicon))
{
}

HCEFreezeAI::~HCEFreezeAI()
{
	PLOG_VERBOSE << "~" << getName();
}
