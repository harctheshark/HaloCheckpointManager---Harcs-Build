#include "pch.h"
#include "H5ForceTeleport.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5ForceTeleport.h for why this goes through the engine's teleport worker rather than writing a position.

class H5ForceTeleport::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::atomic<bool> mReady{ false };

	void onForceTeleport()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);

			// The two modes are a radio pair in the GUI, but the settings are two independent bools, so a
			// config file (or a half-applied toggle) can have both set or neither. Fix it up the way
			// ForceTeleport.cpp does rather than doing something arbitrary.
			if ((settings->forceTeleportManual->GetValue() ^ settings->forceTeleportForward->GetValue()) == false)
			{
				settings->forceTeleportManual->GetValueDisplay() = false;
				settings->forceTeleportManual->UpdateValueWithInput();
				settings->forceTeleportForward->GetValueDisplay() = true;
				settings->forceTeleportForward->UpdateValueWithInput();
			}

			if (settings->forceTeleportManual->GetValue())
			{
				const auto target = settings->forceTeleportAbsoluteVec3->GetValue();
				playerState->teleportPlayerTo(target);
				messagesGUI->addMessage(std::format("Teleported to ({:.2f}, {:.2f}, {:.2f})",
					target.x, target.y, target.z));
			}
			else
			{
				// Relative: the offset is expressed in the player's own frame, so X is "forward".
				// ⚠ getPlayerAim() has a pitch component. Zeroing it (the "ignore vertical" setting) is what
				// makes a forward teleport keep you at the same height instead of firing you into the floor
				// or the sky whenever you happen to be looking up or down.
				const auto offset = settings->forceTeleportRelativeVec3->GetValue();
				auto forward = playerState->getPlayerAim();
				if (settings->forceTeleportForwardIgnoreZ->GetValue())
				{
					forward.z = 0.f;
					if (forward.LengthSquared() > 0.0001f) forward.Normalize();
				}

				// Right-handed basis from the (possibly flattened) forward, so the Y component means
				// "sideways" and Z means "up" regardless of which way the player faces.
				SimpleMath::Vector3 worldUp(0.f, 0.f, 1.f);
				auto right = forward.Cross(worldUp);
				if (right.LengthSquared() > 0.0001f) right.Normalize();

				const auto delta = (forward * offset.x) + (right * offset.y) + (worldUp * offset.z);
				const auto landed = playerState->teleportPlayerBy(delta);
				messagesGUI->addMessage(std::format("Teleported to ({:.2f}, {:.2f}, {:.2f})",
					landed.x, landed.y, landed.z));
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error force teleporting: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onFillCurrent()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(settingsWeak, settings);

			settings->forceTeleportAbsoluteVec3->GetValueDisplay() = playerState->getPlayerPosition();
			settings->forceTeleportAbsoluteVec3->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error filling current position: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mTeleportCallback;
	ScopedCallback<ActionEvent> mFillCurrentCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mTeleportCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportEvent, [this]() { onForceTeleport(); }),
		mFillCurrentCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsoluteFillCurrent, [this]() { onFillCurrent(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5ForceTeleport only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mTeleportCallback.removeCallback();
		mFillCurrentCallback.removeCallback();
	}
};

H5ForceTeleport::H5ForceTeleport(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5ForceTeleport::~H5ForceTeleport() { PLOG_VERBOSE << "~" << getName(); }
