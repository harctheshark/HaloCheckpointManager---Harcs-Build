#include "pch.h"
#include "BSPSetChangeHookEvent.h"
#include "GlobalKill.h"
#include "ModuleHook.h"
#include "PointerDataStore.h"
#include "GetCurrentBSPSet.h"
#include "IMakeOrGetCheat.h"


template<GameState::Value gameT>
class BSPSetChangeHookEventTemplated : public IBSPSetChangeHookEvent
{
private:
	static inline BSPSetChangeHookEventTemplated<gameT>* instance = nullptr;

	std::shared_ptr< GetCurrentBSPSet> getCurrentBSPSet;
	std::shared_ptr<ModuleMidHook> BSPSetChangeHook;
	std::shared_ptr<ObservedEvent<eventpp::CallbackList<void(BSPSet)>>> BSPSetChangeEvent;




	void onCallbackListChanged()
	{
		// Guard against shutdown: a subscriber token can expire after our destructor resets
		// BSPSetChangeHook, still firing this notification -> null deref. (See exit-crash signature A.)
		if (!BSPSetChangeHook) return;
		BSPSetChangeHook->setWantsToBeAttached(BSPSetChangeEvent->isEventSubscribed());
	}


	static void BSPSetChangeHookFunction(SafetyHookContext& ctx)
	{
		if (GlobalKill::isKillSet()) return; // stop firing events into services being torn down
		if (!instance)
			return;

		try
		{
			instance->BSPSetChangeEvent->fireEvent(instance->getCurrentBSPSet->getCurrentBSPSet());
		}
		catch (HCMRuntimeException ex)
		{
			PLOG_ERROR << ex.what();
		}


	}
public:
	BSPSetChangeHookEventTemplated(GameState game, IDIContainer& dicon)
		: getCurrentBSPSet(resolveDependentCheat(GetCurrentBSPSet)),
		BSPSetChangeEvent(std::make_shared< ObservedEvent<eventpp::CallbackList<void(BSPSet)>>>([this]() {onCallbackListChanged(); }))
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		auto BSPSetChangeFunction = ptr->getData < std::shared_ptr<MultilevelPointer>>(nameof(BSPSetChangeFunction), game);
		BSPSetChangeHook = ModuleMidHook::make(game.toModuleName(), BSPSetChangeFunction, BSPSetChangeHookFunction);
		instance = this;
		// (There used to be a subscription here to the old SwitchBSPSet, which wrote the BSP mask directly and so had to
		// fire this event itself. Its replacement, MCCSwitchZoneSet, asks the ENGINE to switch, so the switch goes
		// through the hooked commit and fires this event like any other.)
	}

	std::shared_ptr<ObservedEvent<eventpp::CallbackList<void(BSPSet)>>> getBSPSetChangeEvent() {
		return BSPSetChangeEvent;
	}

	~BSPSetChangeHookEventTemplated()
	{
		instance = nullptr;
	}
};

BSPSetChangeHookEvent::BSPSetChangeHookEvent(GameState game, IDIContainer& dicon)
{
	switch (game)
	{
	case GameState::Value::Halo1:
		throw HCMInitException("BSPSet not applicable to h1");
		break;

	case GameState::Value::Halo2:
		throw HCMInitException("BSPSet not applicable to h2");
		break;

	case GameState::Value::Halo3:
		pimpl = std::make_unique<BSPSetChangeHookEventTemplated<GameState::Value::Halo3>>(game, dicon);
		break;

	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<BSPSetChangeHookEventTemplated<GameState::Value::Halo3ODST>>(game, dicon);
		break;

	case GameState::Value::HaloReach:
		pimpl = std::make_unique<BSPSetChangeHookEventTemplated<GameState::Value::HaloReach>>(game, dicon);
		break;

	case GameState::Value::Halo4:
		pimpl = std::make_unique<BSPSetChangeHookEventTemplated<GameState::Value::Halo4>>(game, dicon);
		break;

	default:
		throw HCMInitException("Not impl yet");
	}
}
BSPSetChangeHookEvent::~BSPSetChangeHookEvent() = default;

