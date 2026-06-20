#include "pch.h"
#include "FarClipDistance.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"

template <GameState::Value gameT>
class FarClipDistanceImpl : public IFarClipDistanceImpl
{
private:
	GameState mGame;

	// fires when the user edits the far-clip value in the UI
	ScopedCallback<eventpp::CallbackList<void(float&)>> mFarClipChangedCallback;
	// fires on MCC state change so we can sync the UI to the live value when a game loads
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> farClipDistancePointer;

	// user changed the value -> write it to game memory
	void onFarClipChanged(float& newValue)
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;

			if (!farClipDistancePointer->writeData(&newValue))
				throw HCMRuntimeException(std::format("Failed to write far clip distance: {}", MultilevelPointer::GetLastError()));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// game (re)loaded -> read the live far clip into the UI so the +/- buttons step from the real value
	void onGameStateChanged(const MCCState& newState)
	{
		try
		{
			if (newState.currentGameState != mGame || newState.currentPlayState != PlayState::Ingame) return;

			float liveValue;
			if (!farClipDistancePointer->readData(&liveValue)) return; // not resolvable yet; leave UI as-is

			lockOrThrow(settingsWeak, settings);
			// set both display and committed value WITHOUT firing the write-back event
			settings->farClipDistance->GetValueDisplay() = liveValue;
			settings->farClipDistance->GetValue() = liveValue;
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	FarClipDistanceImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mFarClipChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->farClipDistance->valueChangedEvent, [this](float& n) { onFarClipChanged(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		farClipDistancePointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(farClipDistance), mGame);
	}
};


FarClipDistance::FarClipDistance(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<FarClipDistanceImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("FarClipDistance not impl for this game");
	}
}

FarClipDistance::~FarClipDistance()
{
	PLOG_DEBUG << "~" << getName();
}
