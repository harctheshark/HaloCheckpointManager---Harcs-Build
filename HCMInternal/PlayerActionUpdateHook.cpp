#include "pch.h"
#include "PlayerActionUpdateHook.h"
#include "ModuleHook.h"
#include "MidhookContextInterpreter.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "RuntimeExceptionHandler.h"
#include "ReplayData.h"

// Modeled on GameTickEventHook: owns one midhook, fires an ObservedEvent whose
// subscription state drives hook attach/detach.

template <GameState::Value gameT>
class PlayerActionUpdateHookTemplated : public PlayerActionUpdateHook::PlayerActionUpdateHookImpl
{
private:
    static inline std::atomic_bool hookRunningMutex = false;
    static inline PlayerActionUpdateHookTemplated<gameT>* instance = nullptr;

    std::shared_ptr<ObservedEvent<PlayerActionUpdateEvent>> playerActionUpdateEvent;
    std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

    std::unique_ptr<ModuleMidHook> updateHook;
    std::shared_ptr<MidhookContextInterpreter> contextInterpreter;

    enum class param { simUpdate = 0 };

    void onEventCallbackListChanged()
    {
        updateHook->setWantsToBeAttached(playerActionUpdateEvent->isEventSubscribed());
    }

    static void updateHookFunction(SafetyHookContext& ctx)
    {
        if (!instance) { PLOG_ERROR << "null PlayerActionUpdateHookTemplated instance"; return; }
        ScopedAtomicBool lock(hookRunningMutex);
        try
        {
            auto* interp = instance->contextInterpreter.get();
            uintptr_t* rcxSlot = interp->getParameterRef(ctx, (int)param::simUpdate); // &RCX
            uintptr_t simUpdate = *rcxSlot;                                           // simulation_update*
            if (!simUpdate) return;

            // mask @ simUpdate+0x8, player_actions[16] @ simUpdate+0x100 (RE: sub_1806A3910)
            uint32_t* maskRef = reinterpret_cast<uint32_t*>(simUpdate + ReplayFormat::kSimUpdateMaskOffset);
            uint8_t*  actions = reinterpret_cast<uint8_t*>(simUpdate + ReplayFormat::kSimUpdateActionsOffset);
            if (IsBadWritePtr(actions, ReplayFormat::kActionArraySize)) return;

            PlayerActionUpdateContext pac{ maskRef, actions };
            instance->playerActionUpdateEvent->fireEvent(pac);
        }
        catch (HCMRuntimeException ex)
        {
            instance->runtimeExceptions->handleMessage(ex);
        }
    }

public:
    PlayerActionUpdateHookTemplated(GameState game, IDIContainer& dicon)
        :
        runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>().lock()),
        playerActionUpdateEvent(std::make_shared<ObservedEvent<PlayerActionUpdateEvent>>([this]() { onEventCallbackListChanged(); }))
    {
        if (instance) throw HCMInitException("Cannot have more than one PlayerActionUpdateHookTemplated per game");

        auto ptr = dicon.Resolve<PointerDataStore>().lock();
        auto updateFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(playerActionUpdateFunction), game);
        contextInterpreter = ptr->getData<std::shared_ptr<MidhookContextInterpreter>>(nameof(playerActionUpdateContextInterpreter), game);
        updateHook = ModuleMidHook::make(game.toModuleName(), updateFunction, updateHookFunction);

        instance = this;
    }

    std::shared_ptr<ObservedEvent<PlayerActionUpdateEvent>> getPlayerActionUpdateEvent() override
    {
        return playerActionUpdateEvent;
    }

    ~PlayerActionUpdateHookTemplated()
    {
        PLOG_DEBUG << "~PlayerActionUpdateHookTemplated";
        if (hookRunningMutex)
        {
            PLOG_INFO << "Waiting for playerActionUpdateHook to finish execution";
            hookRunningMutex.wait(true);
        }
        updateHook.reset();
        instance = nullptr;
    }
};

PlayerActionUpdateHook::PlayerActionUpdateHook(GameState game, IDIContainer& dicon)
{
    std::lock_guard<std::mutex> lock(constructionMutex);
    if (pimpl) return;

    switch (game)
    {
    case GameState::Value::Halo2:
        pimpl = std::make_unique<PlayerActionUpdateHookTemplated<GameState::Value::Halo2>>(game, dicon);
        break;
    default:
        throw HCMInitException("PlayerActionUpdateHook not implemented for this game yet");
    }
}

PlayerActionUpdateHook::~PlayerActionUpdateHook()
{
    PLOG_DEBUG << "~" << getName();
}

std::shared_ptr<ObservedEvent<PlayerActionUpdateEvent>> PlayerActionUpdateHook::getPlayerActionUpdateEvent()
{
    return pimpl->getPlayerActionUpdateEvent();
}
