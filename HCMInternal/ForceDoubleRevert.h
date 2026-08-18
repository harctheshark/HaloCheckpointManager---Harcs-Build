#pragma once
#include "HCEAnchors.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"

class ForceDoubleRevert : public IOptionalCheat
{
private:
	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback<ActionEvent> mForceDoubleRevertCallbackHandle;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	//data
	std::shared_ptr<MultilevelPointer> doubleRevertFlag;

	// primary event callback
	void onForceDoubleRevert()
	{

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;
			PLOG_DEBUG << "Force DoubleRevert called";

			// read the flag
			byte currentFlag = 1;
			// Cross-checked against an independent byte signature - see HCEAnchors.h. HaloCER cannot report
			// its build, so this is the only thing standing between a game update and a write to a stale
			// address. Throws only if the two derivations disagree.
			if (mGame.operator GameState::Value() == GameState::Value::HaloCER)
			{
				uintptr_t doubleRevertAddress = 0;
				if (!doubleRevertFlag->resolve(&doubleRevertAddress) || !doubleRevertAddress)
					throw HCMRuntimeException(std::format("Could not resolve the HaloCER double-revert flag: {}", MultilevelPointer::GetLastError()));
				HCEAnchors::crossCheck(HCEAnchors::Anchor::DoubleRevertFlag, doubleRevertAddress, "Force Double Revert");
			}

			if (!doubleRevertFlag->readData(&currentFlag)) throw HCMRuntimeException(std::format("Failed to read doubleRevertFlag {}", MultilevelPointer::GetLastError()));

			// flip the value;
			currentFlag == 1 ? currentFlag = 0 : currentFlag = 1;
			
			// write the flag
			if (!doubleRevertFlag->writeData(&currentFlag)) throw HCMRuntimeException(std::format("Failed to write DoubleRevert flag {}", MultilevelPointer::GetLastError()));
			messagesGUI->addMessage("Checkpoint double reverted.");

			// fire revert event. Not our problem if it fails.
			settings->forceRevertEvent->operator()();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}

	}




public:
	ForceDoubleRevert(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl), 
		mForceDoubleRevertCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->forceDoubleRevertEvent, [this]() { onForceDoubleRevert(); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()), 
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		doubleRevertFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>("doubleRevertFlag", mGame);
	}

	~ForceDoubleRevert()
	{
		PLOG_VERBOSE << "~" << getName();
	}

	std::string_view getName() override { return nameof(ForceDoubleRevert); }


};