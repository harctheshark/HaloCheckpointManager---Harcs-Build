#include "pch.h"
#include "MasterTickrate.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"

// The master tickrate lives in the "game time globals" struct ( *(halo2.dll+0x15FE008) ): an int16 tickrate
// at +0x02 and a seconds-per-tick float at +0x04 (set together). 343's build runs at 60; flipping these two
// fields to 30 / (1/30) switches the simulation to 30 Hz (and back). 77 callers read the tickrate live via
// the getter, so the change takes effect without a reload (until the game re-runs its timing setter on the
// next map load).
template <GameState::Value gameT>
class MasterTickrateImpl : public IMasterTickrateImpl
{
private:
	GameState mGame;

	ScopedCallback<ActionEvent> mFlipCallback;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> masterTickratePointer;   // int16 @ timingStruct + 0x02
	std::shared_ptr<MultilevelPointer> masterTickrateDtPointer; // float @ timingStruct + 0x04

	void onFlip()
	{
		PLOG_DEBUG << "MasterTickrate onFlip";
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				messagesGUI->addMessage("Load a Halo 2 game first to change the tickrate.");
				return;
			}

			int16_t currentRate = 0;
			if (!masterTickratePointer->readData(&currentRate))
				throw HCMRuntimeException(std::format("Failed to read master tickrate: {}", MultilevelPointer::GetLastError()));

			// flip: anything at (about) 60 -> 30, otherwise -> 60
			int16_t newRate = (currentRate >= 45) ? (int16_t)30 : (int16_t)60;
			float newDt = 1.0f / (float)newRate;

			if (!masterTickratePointer->writeData(&newRate))
				throw HCMRuntimeException(std::format("Failed to write master tickrate: {}", MultilevelPointer::GetLastError()));
			if (!masterTickrateDtPointer->writeData(&newDt))
				throw HCMRuntimeException(std::format("Failed to write tickrate dt: {}", MultilevelPointer::GetLastError()));

			messagesGUI->addMessage(std::format("Master tickrate set to {} Hz.", newRate));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	MasterTickrateImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mFlipCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->masterTickrateFlipEvent, [this]() { onFlip(); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		masterTickratePointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrate), mGame);
		masterTickrateDtPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrateDt), mGame);
	}
};


MasterTickrate::MasterTickrate(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("MasterTickrate not impl for this game");
	}
}

MasterTickrate::~MasterTickrate()
{
	PLOG_DEBUG << "~" << getName();
}
