#include "pch.h"
#include "PlayerActionUpdateHook.h"
#include "GlobalKill.h"
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
    ActionLayout mLayout = replayLayoutFor(gameT);
    bool mEntryChecked = false, mEntryOk = true;

    // The consumer's first bytes as this hook was reverse-engineered on (MCC 1.3528; Replay Port\REPORT_*.md). A different
    // build (or another tool's hook) refuses to attach instead of corrupting the tick. Halo 2: none (the hook predates this).
    static std::vector<uint8_t> expectedEntry()
    {
        switch (gameT)
        {
        case GameState::Value::Halo3:     return { 0x48,0x8B,0xC4,0x48,0x89,0x48,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x68,0x88,0x48,0x81,0xEC,0x38,0x01,0x00,0x00 };
        case GameState::Value::Halo3ODST: return { 0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC,0x24,0xA8,0xFD,0xFF,0xFF,0x48,0x81,0xEC,0x58,0x03,0x00,0x00 };
        case GameState::Value::HaloReach: return { 0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x38,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0xA0,0x01,0x00,0x00 };
        case GameState::Value::Halo4:     return { 0x48,0x8B,0xC4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0xA8,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0x30,0x02,0x00,0x00 };
        default: return {};
        }
    }
    std::shared_ptr<MultilevelPointer> mUpdateFunction;
    bool entryBytesOk()
    {
        const auto want = expectedEntry();
        if (want.empty()) return true;
        uintptr_t at = 0;
        if (!mUpdateFunction || !mUpdateFunction->resolve(&at) || !at) return false;   // module not loaded yet: check again later
        if (mEntryChecked) return mEntryOk;
        mEntryChecked = true;
        mEntryOk = !IsBadReadPtr((void*)at, want.size()) && std::memcmp((const void*)at, want.data(), want.size()) == 0;
        if (!mEntryOk) PLOG_ERROR << "PlayerActionUpdateHook: the simulation_update consumer's entry bytes differ (another build, or another tool's hook) - replay disabled for this game";
        return mEntryOk;
    }

    void onEventCallbackListChanged()
    {
        // Guard against shutdown: a subscriber token can expire after our destructor resets
        // updateHook, still firing this notification -> null deref. (See exit-crash signature A.)
        if (!updateHook) return;
        const bool want = playerActionUpdateEvent->isEventSubscribed();
        updateHook->setWantsToBeAttached(want && entryBytesOk());
    }

    static void updateHookFunction(SafetyHookContext& ctx)
    {
        if (GlobalKill::isKillSet()) return; // stop firing events into services being torn down
        if (!instance) { PLOG_ERROR << "null PlayerActionUpdateHookTemplated instance"; return; }
        ScopedAtomicBool lock(hookRunningMutex);
        try
        {
            auto* interp = instance->contextInterpreter.get();
            uintptr_t* rcxSlot = interp->getParameterRef(ctx, (int)param::simUpdate); // &RCX
            uintptr_t simUpdate = *rcxSlot;                                           // simulation_update*
            if (!simUpdate) return;

            // the game's layout (ReplayData.h): mask + player_actions[slots] inside the simulation_update
            const ActionLayout& L = instance->mLayout;
            void*    maskPtr = reinterpret_cast<void*>(simUpdate + L.maskOffset);
            uint8_t* actions = reinterpret_cast<uint8_t*>(simUpdate + L.actionsOffset);
            if (IsBadWritePtr(actions, (size_t)L.stride * L.slots)) return;
            PlayerActionUpdateContext pac{ maskPtr, actions, &L };
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
        mUpdateFunction = updateFunction;
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
    case GameState::Value::Halo3:
        pimpl = std::make_unique<PlayerActionUpdateHookTemplated<GameState::Value::Halo3>>(game, dicon);
        break;
    case GameState::Value::Halo3ODST:
        pimpl = std::make_unique<PlayerActionUpdateHookTemplated<GameState::Value::Halo3ODST>>(game, dicon);
        break;
    case GameState::Value::HaloReach:
        pimpl = std::make_unique<PlayerActionUpdateHookTemplated<GameState::Value::HaloReach>>(game, dicon);
        break;
    case GameState::Value::Halo4:
        pimpl = std::make_unique<PlayerActionUpdateHookTemplated<GameState::Value::Halo4>>(game, dicon);
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
