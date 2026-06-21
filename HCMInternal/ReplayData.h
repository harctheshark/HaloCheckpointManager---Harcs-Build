#pragma once
#include "pch.h"

// ============================================================================
//  HCM Replay System - on-disk + in-memory data format ("inputs only" / .h2r)
//
//  Records the per-tick player_action update that halo2.dll's player-update
//  funnel (sub_180866A10 @ +0x866A10 in 1.3528) consumes:
//      RCX = player_action_mask (u32)
//      RDX = pointer to player_actions[4][0x60]   (0x180 bytes total)
//
//  Each per-player 0x60 block (treated as OPAQUE bytes by record/playback):
//      +0x08 facing yaw (f32)      +0x0C facing pitch (f32)
//      +0x10 throttle X (f32)      +0x14 throttle Y (f32)
//      +0x18 trigger (f32)         +0x1C secondary_trigger (f32)
//      +0x20 action_flags (u32  -- digital buttons)
//      +0x24 weapon_set (u16)      +0x26 zoom (i16)   +0x2A grenade (u16)
//
//  Determinism (RNG etc.) is handled by HCM's checkpoint system, so we only
//  persist inputs. See REPLAY_SYSTEM_DESIGN.md for the full RE writeup.
// ============================================================================

namespace ReplayFormat
{
    // Player-action layout constants (halo2 / MCC). These now describe the PER-TICK simulation_update:
    // mask @ simUpdate+0x8, player_actions[16] @ simUpdate+0x100, stride 0x60 (RE'd from sub_1806A3910).
    inline constexpr size_t kPlayerActionStride = 0x60;   // bytes per player block
    inline constexpr size_t kMaxPlayers         = 16;     // simulation_update player slots
    inline constexpr size_t kActionArraySize    = kPlayerActionStride * kMaxPlayers; // 0x600
    inline constexpr size_t kSimUpdateMaskOffset    = 0x8;    // player_action_mask within simulation_update
    inline constexpr size_t kSimUpdateActionsOffset = 0x100;  // player_actions[] within simulation_update

    inline constexpr char     kMagic[4]   = { 'H', 'C', 'M', 'R' };
    inline constexpr uint32_t kVersion    = 4u;   // v4: hooks the PER-SIM-TICK simulation_update (16 players)

    // header.reserved[0] flag: a per-tick camera stream follows the input frames.
    // NOTE: this is PURE deterministic input replay - we deliberately do NOT record or re-apply any biped
    // physics (velocity/position). The sim loop re-derives all of that from the injected inputs alone.
    inline constexpr uint32_t kFlagHasCamera = 1u;
}

#pragma pack(push, 1)

// One simulation tick of recorded input. Fixed size -> trivial seeking.
struct PlayerActionFrame
{
    uint32_t relTick;                                   // absTick - header.startTickAbs
    uint32_t mask;                                      // player_action_mask (RCX)
    uint8_t  actions[ReplayFormat::kActionArraySize];   // raw RDX bytes (4 x 0x60)
};
static_assert(sizeof(PlayerActionFrame) == 8 + ReplayFormat::kActionArraySize,
              "PlayerActionFrame must be tightly packed");

// One per-tick first-person camera sample (raw engine values: FOV in radians, vectors world-space).
// The player_action block only carries the biped facing; the FP camera is a separate hierarchy, so we
// record + replay it explicitly (mirrors the xlive c_replay_system camera layer).
struct CameraSample
{
    float pos[3];   // world position
    float fwd[3];   // lookDirForward
    float up[3];    // lookDirUp
    float fov;      // radians
};
static_assert(sizeof(CameraSample) == 40, "CameraSample must be tightly packed (10 floats)");

// File header. Followed immediately by frameCount PlayerActionFrame records.
struct ReplayFileHeader
{
    char     magic[4];        // "HCMR"
    uint32_t version;         // ReplayFormat::kVersion
    uint32_t gameTickRate;    // 30 or 60 (informational)
    uint32_t startTickAbs;    // absolute tickCounter at record start
    uint32_t playerCount;     // players actually present (SP = 1)
    uint32_t frameCount;      // number of frames following (filled on save)
    char     levelCode[8];    // GetCurrentLevelCode, sanity check on load
    uint32_t reserved[4];
};
static_assert(sizeof(ReplayFileHeader) == 4 + 4 + 4 + 4 + 4 + 4 + 8 + 16,
              "ReplayFileHeader layout changed - bump version");

#pragma pack(pop)

// In-memory representation of a loaded/recording replay.
struct ReplayData
{
    ReplayFileHeader               header{};
    std::vector<PlayerActionFrame> frames;
    std::vector<CameraSample>      cameras;   // parallel to frames when header.reserved[0] & kFlagHasCamera

    bool hasCamera() const
    {
        return (header.reserved[0] & ReplayFormat::kFlagHasCamera) && cameras.size() == frames.size() && !cameras.empty();
    }

    bool magicValid() const
    {
        return header.magic[0] == 'H' && header.magic[1] == 'C'
            && header.magic[2] == 'M' && header.magic[3] == 'R';
    }
};
