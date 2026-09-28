#include "pch.h"
#include "GameTickEventHook.h"
#include "ModuleHook.h"
#include "PointerDataStore.h"
#include "RuntimeExceptionHandler.h"
#include "GlobalKill.h"

template <GameState::Value gameT>
class GameTickEventHookTemplated : public GameTickEventHook::GameTickEventHookImpl
{
private:
	GameState mGame;

	static inline std::atomic_bool gameTickHookRunningMutex = false;
	static inline GameTickEventHookTemplated<gameT>* instance = nullptr;

	std::shared_ptr<ObservedEvent<GameTickEvent>> gameTickEvent;
	std::unique_ptr<ScopedCallback<ActionEvent>> gameTickEventCallbackListChanged;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;


	std::unique_ptr<ModuleMidHook> tickIncrementHook;
	std::shared_ptr<MultilevelPointer> tickCounter;


	void onGameTickEventCallbackListChanged()
	{
		PLOG_DEBUG << "eeeeeettt";
		// During shutdown our destructor resets tickIncrementHook to null, yet a subscriber's
		// SharedRequestToken can expire afterwards and still fire this "callback list changed"
		// notification (via SharedRequestProvider's deleter -> updateService). Without this guard
		// we dereference a null unique_ptr -> AV reading @0x28. (Proven exit-crash signature A.)
		if (!tickIncrementHook) return;
		tickIncrementHook->setWantsToBeAttached(gameTickEvent->isEventSubscribed());
	}

	static void tickIncrementHookFunction(SafetyHookContext& ctx)
	{
		static int errorCount = 0;
		// Once shutdown has begun, don't fire the tick event: firing it reads game memory
		// (tickCounter / subscriber cheats) that MCC may already be tearing down, causing an
		// AV during exit. (Proven exit-crash signature B: MultilevelPointer read @0x7D8.)
		if (GlobalKill::isKillSet()) return;
		if (!instance) { PLOG_ERROR << "null GameTickEventHookTemplated instance"; return; }
		ScopedAtomicBool lock(gameTickHookRunningMutex);
		try
		{
			instance->gameTickEvent->fireEvent(instance->getCurrentGameTick());
			errorCount = 0;
		}
		catch (HCMRuntimeException ex)
		{
			errorCount++;
			PLOG_ERROR << "Error in game tick hook. This is only a problem if it happens repeatedly. Error details: " << ex.what();
			if (errorCount == 10)
			{
				ex.append("10x\n");
				instance->runtimeExceptions->handleMessage(ex);
			}
		}
		// This runs inside a midhook on the GAME's simulation thread: anything that escapes kills MCC. A subscriber
		// throwing a plain std:: exception (e.g. std::out_of_range from a substr/at) used to do exactly that.
		catch (const std::exception& ex)
		{
			errorCount++;
			PLOG_ERROR << "Non-HCM exception in game tick hook (" << typeid(ex).name() << "): " << ex.what();
			if (errorCount == 10)
			{
				HCMRuntimeException converted(std::format("Game tick hook subscriber threw {} 10x: {}", typeid(ex).name(), ex.what()));
				instance->runtimeExceptions->handleMessage(converted);
			}
		}

	}

public:
	GameTickEventHookTemplated(GameState game, IDIContainer& dicon) 
		: 
		mGame(game),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>().lock()),
		gameTickEvent(std::make_shared<ObservedEvent<GameTickEvent>>([this]() {onGameTickEventCallbackListChanged(); }))
	{
		if (instance) throw HCMInitException("Cannot have more than one GameTickEventHookTemplated per game");
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		auto tickIncrementFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(tickIncrementFunction), game);
		tickCounter = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(tickCounter), game);
		tickIncrementHook = ModuleMidHook::make(game.toModuleName(), tickIncrementFunction, tickIncrementHookFunction); 


		instance = this;
	}

	std::shared_ptr<ObservedEvent<GameTickEvent>> getGameTickEvent()
	{
		return gameTickEvent;
	}

	virtual uint32_t getCurrentGameTick() override
	{
			uint32_t out;
			if (!tickCounter->readData(&out)) throw HCMRuntimeException(std::format("Could not resolve tickcounter: {}", MultilevelPointer::GetLastError()));
			return out;
	}

	~GameTickEventHookTemplated()
	{
		PLOG_DEBUG << "~GameTickEventHookTemplated";
		if (gameTickHookRunningMutex)
		{
			PLOG_INFO << "Waiting for gameTickHook to finish execution";
			gameTickHookRunningMutex.wait(true);
		}

		tickIncrementHook.reset();

		instance = nullptr;
	}
};


GameTickEventHook::GameTickEventHook(GameState game, IDIContainer& dicon)
{
	std::lock_guard<std::mutex> lock(constructionMutex);
	if (pimpl) return;

	switch (game)
	{
	case GameState::Value::Halo1: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo1>>(game, dicon); break;
	case GameState::Value::Halo2: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo2>>(game, dicon); break;
	case GameState::Value::Halo3: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo3>>(game, dicon); break;
	case GameState::Value::Halo3ODST: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo3ODST>>(game, dicon); break;
	case GameState::Value::HaloReach: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::HaloReach>>(game, dicon); break;
	case GameState::Value::Halo4: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo4>>(game, dicon); break;
	// Halo 2 Anniversary MP (groundhog.dll, Halo 4 engine lineage). Same shape as Halo 4: the game mirrors
	// game_time_globals+0xC into a module global every tick, so no TLS walk. The copy moved into a callee
	// in groundhog, so the hook site is 0x3BCA2C rather than Halo 4's 0x9A6F6 - see InternalPointerData.xml.
	case GameState::Value::Halo2MP: pimpl = std::make_unique<GameTickEventHookTemplated<GameState::Value::Halo2MP>>(game, dicon); break;
	// HaloCER: deliberately NO implementation, and deliberately NOT the default throw. MasterTickrate<HaloCER>
	// (MasterTickrate.cpp member-init list) and masterTickrateCustomGUI (GUIRequiredServices.cpp) both resolve this
	// service for CER but never call it there; throwing would take Master Tickrate down on CER. The forwarders below
	// refuse cleanly if anything ever does call it.
	case GameState::Value::HaloCER: break;
	// !! REQUIRED. Without it an unlisted game left pimpl NULL and construction "succeeded"; the first
	// getGameTickEvent() (DisplayPlayerInfo's constructor, on a createCheats worker thread) was then a null
	// dereference - an access violation no C++ catch can stop, which took MCC down at injection.
	default: throw HCMInitException(std::format("GameTickEventHook not impl for this game: {}", game.toString()));
	}

}

GameTickEventHook::~GameTickEventHook()
{
	PLOG_DEBUG << "~" << getName();
}

// Null-safe: a game with no implementation (HaloCER, above) fails cleanly instead of dereferencing null.
std::shared_ptr<ObservedEvent<GameTickEvent>> GameTickEventHook::getGameTickEvent()
{
	if (!pimpl) throw HCMRuntimeException("GameTickEventHook has no implementation for this game");
	return pimpl->getGameTickEvent();
}

uint32_t GameTickEventHook::getCurrentGameTick()
{
	if (!pimpl) throw HCMRuntimeException("GameTickEventHook has no implementation for this game");
	return pimpl->getCurrentGameTick();
}