#include "pch.h"
#include "HCEForceTeleport.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"

// ================================================================================================================
// Halo Campaign Evolved Force Teleport. Ported from HCM_Evolved player_camera.py::teleport_player.
//
// The eight-write sequence that actually moves the player lives in HCEGetPlayerState::teleportPlayerTo, because
// HCEFreecam's "teleport to camera" needs the identical sequence with a different target.
//
// Deliberately reuses the MCC forceTeleportEvent / forceTeleportAbsoluteVec3 settings and the forceTeleport
// hotkey: HaloCER and the MCC games can never be in the same process (OptionalCheatManager filters the pair list
// both ways), so there is only ever one listener. Same reasoning as HCECheckpointDetours.
// ================================================================================================================

class HCEForceTeleport::HCEForceTeleportImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };

	void onForceTeleportEvent()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(settingsWeak, settings);
			const auto target = settings->forceTeleportAbsoluteVec3->GetValue();

			teleportPlayerTo(target);

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("Teleported to {:.3f}, {:.3f}, {:.3f}", target.x, target.y, target.z));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error teleporting player: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	void fillPosition()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(settingsWeak, settings);
			lockOrThrow(playerStateWeak, playerState);

			settings->forceTeleportAbsoluteVec3->GetValueDisplay() = playerState->getPlayerPosition();
			settings->forceTeleportAbsoluteVec3->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void copyPosition()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			settings->forceTeleportAbsoluteVec3->serialiseToClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void pastePosition()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			settings->forceTeleportAbsoluteVec3->deserialiseFromClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mForceTeleportEventCallback;
	ScopedCallback<ActionEvent> mFillCurrentCallback;
	ScopedCallback<ActionEvent> mCopyCallback;
	ScopedCallback<ActionEvent> mPasteCallback;

public:
	HCEForceTeleportImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mForceTeleportEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportEvent, [this]() { onForceTeleportEvent(); }),
		mFillCurrentCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsoluteFillCurrent, [this]() { fillPosition(); }),
		mCopyCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsoluteCopy, [this]() { copyPosition(); }),
		mPasteCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsolutePaste, [this]() { pastePosition(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEForceTeleport only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEForceTeleportImpl()
	{
		mReady.store(false, std::memory_order_release);
		mForceTeleportEventCallback.removeCallback();
		mFillCurrentCallback.removeCallback();
		mCopyCallback.removeCallback();
		mPasteCallback.removeCallback();
	}

	void teleportPlayerTo(SimpleMath::Vector3 position) // throws HCMRuntimeException
	{
		lockOrThrow(playerStateWeak, playerState);
		playerState->teleportPlayerTo(position);
	}
};


HCEForceTeleport::HCEForceTeleport(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEForceTeleportImpl>(game, dicon))
{
}

HCEForceTeleport::~HCEForceTeleport()
{
	PLOG_VERBOSE << "~" << getName();
}

void HCEForceTeleport::teleportPlayerTo(SimpleMath::Vector3 position) { pimpl->teleportPlayerTo(position); }
