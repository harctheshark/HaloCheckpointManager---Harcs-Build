#include "pch.h"
#include "H5ForceLaunch.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5ForceLaunch.h for which field this writes and why the obvious one (obj+0x248) is wrong.

class H5ForceLaunch::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::atomic<bool> mReady{ false };

	void onForceLaunch()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);

			// The two modes are a radio pair in the GUI but two independent bools underneath, so a config
			// file can have both or neither set. Repair it the way ForceTeleport.cpp does.
			if ((settings->forceLaunchManual->GetValue() ^ settings->forceLaunchForward->GetValue()) == false)
			{
				settings->forceLaunchManual->GetValueDisplay() = false;
				settings->forceLaunchManual->UpdateValueWithInput();
				settings->forceLaunchForward->GetValueDisplay() = true;
				settings->forceLaunchForward->UpdateValueWithInput();
			}

			SimpleMath::Vector3 delta{};
			if (settings->forceLaunchManual->GetValue())
			{
				delta = settings->forceLaunchAbsoluteVec3->GetValue();
			}
			else
			{
				// Relative: expressed in the player's own frame, X forward.
				// ⚠ getPlayerAim() carries pitch. Flattening it (the "ignore vertical" toggle) is what makes
				// a forward launch send you across the map rather than into the floor when you look down.
				const auto offset = settings->forceLaunchRelativeVec3->GetValue();
				auto forward = playerState->getPlayerAim();
				if (forward.LengthSquared() > 0.0001f) forward.Normalize();

				if (settings->forceLaunchForwardIgnoreZ->GetValue())
				{
					forward.z = 0.f;
					if (forward.LengthSquared() > 0.0001f) forward.Normalize();
				}

				// ⚠ GIMBAL LOCK. Looking straight up or straight down makes forward parallel to world up, so
				// forward x up collapses to the zero vector and the whole basis degenerates - a launch that
				// silently does nothing, or goes somewhere arbitrary. Pick an arbitrary right vector there,
				// exactly as MCC's ForceLaunch does.
				SimpleMath::Vector3 right =
					(std::abs(forward.z) > 0.9999f)
					? SimpleMath::Vector3::UnitX
					: forward.Cross(SimpleMath::Vector3::UnitZ);
				right.Normalize();

				// ⚠ UP IS AIM-RELATIVE (right x forward), NOT world up. This used to use world up, which made
				// the Z component mean something different here than it does in every other game's Force Launch.
				SimpleMath::Vector3 up = right.Cross(forward);
				up.Normalize();

				delta = (forward * offset.x) + (right * offset.y) + (up * offset.z);
			}

			// ⚠⚠ ADDITIVE, exactly like MCC's ForceLaunch (`*currentVelocity = *currentVelocity + ...`).
			// This used to REPLACE the velocity, so pressing +50 twice left you at 50 instead of 100 and a
			// launch always wiped whatever momentum you already had. Read-modify-write on the live value.
			SimpleMath::Vector3 result{};
			playerState->modifyPlayerVelocity([&](SimpleMath::Vector3 current)
				{
					result = current + delta;
					return result;
				});

			messagesGUI->addMessage(std::format("Launched by ({:.2f}, {:.2f}, {:.2f}) -> velocity ({:.2f}, {:.2f}, {:.2f})",
				delta.x, delta.y, delta.z, result.x, result.y, result.z));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error force launching: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceLaunchEvent, [this]() { onForceLaunch(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5ForceLaunch only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mCallback.removeCallback();
	}
};

H5ForceLaunch::H5ForceLaunch(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5ForceLaunch::~H5ForceLaunch() { PLOG_VERBOSE << "~" << getName(); }
