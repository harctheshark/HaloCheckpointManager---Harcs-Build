#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"
#include "ObservedEvent.h"

// Fires once per simulation tick, at the player-action update funnel
// (halo2 1.3528: sub_180866A10 @ +0x866A10), giving subscribers read/WRITE access
// to the player_action_mask (RCX) and the player_actions array (RDX, 4 x 0x60).
//
// ReplayRecorder reads these; ReplayPlayer overwrites them. Owning the single
// hook here (mirroring GameTickEventHook) avoids double-hooking the address.

struct PlayerActionUpdateContext
{
    uint32_t* maskRef;   // -> player_action_mask (writable; low dword of RCX slot)
    uint8_t*  actions;   // -> player_actions[kMaxPlayers][0x60]  (writable, 0x180 bytes)
};

using PlayerActionUpdateEvent = eventpp::CallbackList<void(PlayerActionUpdateContext&)>;

class PlayerActionUpdateHook : public IOptionalCheat
{
public:
    class PlayerActionUpdateHookImpl
    {
    public:
        virtual ~PlayerActionUpdateHookImpl() = default;
        virtual std::shared_ptr<ObservedEvent<PlayerActionUpdateEvent>> getPlayerActionUpdateEvent() = 0;
    };
private:
    std::unique_ptr<PlayerActionUpdateHookImpl> pimpl;
    static inline std::mutex constructionMutex;

public:
    PlayerActionUpdateHook(GameState game, IDIContainer& dicon);
    ~PlayerActionUpdateHook();

    std::shared_ptr<ObservedEvent<PlayerActionUpdateEvent>> getPlayerActionUpdateEvent();

    std::string_view getName() override { return nameof(PlayerActionUpdateHook); }
};
