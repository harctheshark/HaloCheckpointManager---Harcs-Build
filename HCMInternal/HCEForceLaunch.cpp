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
// Reuses forceLaunchEvent / forceLaunch* settings and the forceLaunch hotkey; HaloCER and MCC can never share a
// process, so there is only ever one listener.
//
// Two modes, mirroring MCC exactly - including the asymmetry, which is deliberate on MCC's side and kept here:
//   * Manual   - REPLACES the velocity with the absolute world-axis vector.
//   * Relative - ADDS forward/right/up relative to the player's look direction, with an optional "ignore
//                vertical look angle". Basis from HCEGetPlayerState::lookRelativeOffset; the view angle is in
//                the same world frame as the velocity field (see the comment on that function).
//
// NOT ported: "apply to custom object" - see the same note in HCEForceTeleport.cpp.

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

			// Shared with MCC, so the pair can arrive invalid (neither set, or both). XOR is true exactly when
			// the state is valid. Same repair MCC's ForceLaunch does.
			if ((settings->forceLaunchManual->GetValue() ^ settings->forceLaunchForward->GetValue()) == false)
			{
				settings->forceLaunchManual->GetValueDisplay() = false;
				settings->forceLaunchManual->UpdateValueWithInput();
				settings->forceLaunchForward->GetValueDisplay() = true;
				settings->forceLaunchForward->UpdateValueWithInput();
				lockOrThrow(messagesGUIWeak, messagesGUI);
				messagesGUI->addMessage("Force Launch radio button was in invalid state. \nHCM has set it to a valid state (relative)");
			}

			SimpleMath::Vector3 velocity;
			if (settings->forceLaunchManual->GetValue())
			{
				velocity = settings->forceLaunchAbsoluteVec3->GetValue();
				playerState->setPlayerVelocity(velocity);
			}
			else
			{
				const auto viewAngle = playerState->getPlayerViewAngle();
				const auto delta = HCEGetPlayerState::lookRelativeOffset(
					viewAngle,
					settings->forceLaunchRelativeVec3->GetValue(),
					settings->forceLaunchForwardIgnoreZ->GetValue());

				velocity = playerState->addPlayerVelocity(delta);
			}

			lockOrThrow(messagesGUIWeak, messagesGUI);
			// "now", not "set to" - the relative branch ADDS, so the number below is the resulting velocity.
			messagesGUI->addMessage(std::format("Velocity now {:.2f}, {:.2f}, {:.2f}", velocity.x, velocity.y, velocity.z));
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
