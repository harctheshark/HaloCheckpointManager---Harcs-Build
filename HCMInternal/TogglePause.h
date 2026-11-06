#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ControlServiceContainer.h"

class TogglePause : public IOptionalCheat
{
private:
	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback <ToggleEvent>mTogglePauseCallbackHandle;
	ScopedCallback <ToggleEvent>mFreezeEffectsCallbackHandle;

	// injected services
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<PauseGame> pauseGameServiceWeak; // actual implementation is over here

	// data
	std::shared_ptr<SharedRequestToken> pauseRequest;





public:
	TogglePause(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mTogglePauseCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->togglePause->valueChangedEvent, [this](bool& newValue) { onTogglePauseChanged(newValue); }),
		mFreezeEffectsCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->pauseAlsoFreezesEffects->valueChangedEvent, [this](bool& newValue) { onFreezeEffectsChanged(newValue); }),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto control = dicon.Resolve<ControlServiceContainer>().lock();
		if (control->pauseGameServiceFailure.has_value()) throw control->pauseGameServiceFailure.value();
		if (control->pauseGameService.value()->getServiceFailures().contains(gameImpl)) throw control->pauseGameService.value()->getServiceFailures().at(gameImpl);
		pauseGameServiceWeak = control->pauseGameService.value();
		bool freeze = dicon.Resolve<SettingsStateAndEvents>().lock()->pauseAlsoFreezesEffects->GetValue();
		onFreezeEffectsChanged(freeze);
	}

	// Halo 3 / ODST: whether the pause also holds the game speed at 0 (particles / decorators freeze, as MCC's own pause)
	void onFreezeEffectsChanged(bool& newValue)
	{
		if (auto svc = pauseGameServiceWeak.lock()) svc->setFreezeEffects(newValue);
	}

	~TogglePause()
	{
		PLOG_VERBOSE << "~" << getName();
	}

	std::string_view getName() override { return nameof(TogglePause); }

	// primary event callback
	// public so AdvanceTicks can access
	void onTogglePauseChanged(bool& newValue)
	{
		try
		{
			if (newValue)
			{
				lockOrThrow(pauseGameServiceWeak, pauseGameService)
				pauseRequest = pauseGameService->makeScopedRequest();
			}
			else
			{
				if (pauseRequest)
					pauseRequest.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		
	}

};