#include "pch.h"
#include "HCEForceLaunch.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"

// Halo Campaign Evolved Force Launch. One 12-byte write at physicsEntry + 0x230
// (HCM_Evolved player_camera.py::set_player_velocity). No re-apply - it is a one-shot, like MCC's.
//
// Reuses forceLaunchEvent / forceLaunchAbsoluteVec3 and the forceLaunch hotkey; HaloCER and MCC can never share a
// process, so there is only ever one listener.

class HCEForceLaunch::HCEForceLaunchImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };

	void onForceLaunchEvent()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(settingsWeak, settings);
			lockOrThrow(playerStateWeak, playerState);

			const auto velocity = settings->forceLaunchAbsoluteVec3->GetValue();
			playerState->setPlayerVelocity(velocity);

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("Velocity set to {:.2f}, {:.2f}, {:.2f}", velocity.x, velocity.y, velocity.z));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error launching player: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mForceLaunchEventCallback;

public:
	HCEForceLaunchImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mForceLaunchEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceLaunchEvent, [this]() { onForceLaunchEvent(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEForceLaunch only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEForceLaunchImpl()
	{
		mReady.store(false, std::memory_order_release);
		mForceLaunchEventCallback.removeCallback();
	}
};


HCEForceLaunch::HCEForceLaunch(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEForceLaunchImpl>(game, dicon))
{
}

HCEForceLaunch::~HCEForceLaunch()
{
	PLOG_VERBOSE << "~" << getName();
}
