#include "pch.h"
#include "MCCSwitchZoneSet.h"
#include "MCCZoneSets.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "IMakeOrGetCheat.h"

// See MCCSwitchZoneSet.h for why this is two stores and not an engine call.

class MCCSwitchZoneSet::Impl
{
private:
	GameState mGame;
	std::shared_ptr<MCCZoneSets> mZoneSets;
	std::shared_ptr<MultilevelPointer> zoneSetSwitchIndex;
	std::shared_ptr<MultilevelPointer> zoneSetSwitchFlag;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	void requestSwitch(int index)
	{
		const auto names = mZoneSets->getZoneSetNames();   // throws when no scenario is loaded
		if (index < 0 || (size_t)index >= names.size())
			throw HCMRuntimeException(std::format("Zone set {} is out of range (this level has {}: 0..{})", index, names.size(), names.size() - 1));

		uint8_t pending = 0;
		if (!zoneSetSwitchFlag->readData(&pending))
			throw HCMRuntimeException(std::format("Could not read the zone set switch flag: {}", MultilevelPointer::GetLastError()));
		if (pending)
			throw HCMRuntimeException("A zone set switch is already pending - wait for it to finish");

		if (mZoneSets->getCurrentZoneSet() == index && mZoneSets->isZoneSetFullyLoaded(index))
			throw HCMRuntimeException(std::format("Already on zone set {}", names[(size_t)index]));

		int32_t idx = index;
		if (!zoneSetSwitchIndex->writeData(&idx))
			throw HCMRuntimeException(std::format("Could not write the zone set switch index: {}", MultilevelPointer::GetLastError()));
		uint8_t one = 1;
		if (!zoneSetSwitchFlag->writeData(&one))   // ⚠ LAST - see the header
			throw HCMRuntimeException(std::format("Could not write the zone set switch flag: {}", MultilevelPointer::GetLastError()));

		PLOG_INFO << "Requested zone set switch to " << index << " (" << names[(size_t)index] << ") in " << mGame.toString();
		lockOrThrow(messagesGUIWeak, messagesGUI);
		messagesGUI->addMessage(std::format("Switching to zone set: {}", names[(size_t)index]));
	}

	void onSwitchEvent()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			requestSwitch(MCCZoneSetBridge::selection(mGame));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST so it is destroyed FIRST: no event can reach a half-destroyed Impl.
	ScopedCallback<ActionEvent> mSwitchCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mSwitchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->switchZoneSetEvent, [this]() { onSwitchEvent(); })
	{
		mZoneSets = resolveDependentCheat(MCCZoneSets);
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		zoneSetSwitchIndex = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(zoneSetSwitchIndex), game);
		zoneSetSwitchFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(zoneSetSwitchFlag), game);
		MCCZoneSetBridge::registerSwitch(game, mZoneSets);
	}

	~Impl()
	{
		MCCZoneSetBridge::unregisterSwitch(mGame);
	}
};


MCCSwitchZoneSet::MCCSwitchZoneSet(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}
MCCSwitchZoneSet::~MCCSwitchZoneSet() = default;
