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

#include "ReplayData.h"

// Fires once per simulation tick at the entry of the game's simulation_update consumer (ReplayData.h table), with
// read/WRITE access to the valid-player mask and player_actions[slots] (layout = the game's ActionLayout).
struct PlayerActionUpdateContext
{
    void*    maskPtr;               // -> the valid-player mask (layout->maskBytes wide; writable)
    uint8_t* actions;               // -> player_actions[layout->slots], layout->stride bytes each (writable)
    const ActionLayout* layout;
    uint64_t getMask() const { return layout->maskBytes == 8 ? *(uint64_t*)maskPtr : (uint64_t)*(uint32_t*)maskPtr; }
    void setMask(uint64_t m) { if (layout->maskBytes == 8) *(uint64_t*)maskPtr = m; else *(uint32_t*)maskPtr = (uint32_t)m; }
    // the tick this update simulates, when the layout knows where the update keeps it (layout->tickOffset)
    bool hasTick() const { return layout->tickOffset != 0; }
    uint32_t tick() const { return *(const uint32_t*)(actions - layout->actionsOffset + layout->tickOffset); }
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
