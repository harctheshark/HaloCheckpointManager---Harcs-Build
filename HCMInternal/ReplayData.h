#pragma once
#include "pch.h"
#include "GameState.h"

// ============================================================================
//  HCM Replay System - on-disk + in-memory data format ("inputs only")
//
//  Records, once per SIMULATION TICK, the player_action part of the tick's simulation_update at the entry of the
//  function that consumes it (RCX = simulation_update*), and overwrites it on playback after a checkpoint revert (the
//  revert gives the determinism; only inputs are stored). Every game has the same shape - a valid-player mask and
//  player_actions[16] - with its own offsets and action size (ActionLayout). The action blocks are OPAQUE bytes.
//
//    game     consumer (RVA, 1.3528)   mask        actions   stride   yaw/pitch in a block
//    Halo 2   0x6A3910                 u32 @+0x8   +0x100    0x60     +0x08 / +0x0C
//    Halo 3   0xE444C                  u32 @+0x8   +0x10     0x88     +0x10 / +0x14
//    ODST     0xFEA10                  u32 @+0x8   +0x10     0x240    +0x10 / +0x14
//    Reach    0x47050                  u32 @+0x8   +0x10     0xA8     +0x10 / +0x14
//    Halo 4   0x82330                  u64 @+0x8   +0x10     0x100    +0x10 / +0x14
//  RE: Documents\Halo Mod And Tools\Replay Port\REPORT_*.md (2026-10-25); Halo 2: REPLAY_SYSTEM_DESIGN.md.
//
//  File versions:
//    v4 (Halo 2 only): header + frameCount x { u32 relTick, u32 mask, 16 x 0x60 action bytes } [+ camera stream]
//    v5 (every game):  header (reserved[1] = game, reserved[2] = action stride) + frameCount x
//                      { u32 relTick, u64 mask, popcount(mask) x stride bytes (the masked slots, ascending) } [+ camera stream]
//                      [+ checkpoint: u64 size, size bytes = the checkpoint the recording started from, as a dump has it]
//  Files are <name>.HaloReplay (older builds wrote .h2r - same format, still loads).
// ============================================================================

namespace ReplayFormat
{
    // Halo 2's layout (kept: the v4 reader and the old names below are Halo 2's)
    inline constexpr size_t kPlayerActionStride = 0x60;
    inline constexpr size_t kMaxPlayers         = 16;
    inline constexpr size_t kActionArraySize    = kPlayerActionStride * kMaxPlayers; // 0x600
    inline constexpr size_t kSimUpdateMaskOffset    = 0x8;
    inline constexpr size_t kSimUpdateActionsOffset = 0x100;

    inline constexpr char     kMagic[4]   = { 'H', 'C', 'M', 'R' };
    inline constexpr uint32_t kVersionV4  = 4u;   // Halo 2, fixed 16 x 0x60 frames
    inline constexpr uint32_t kVersion    = 5u;   // every game: per-game stride, masked slots only

    // header.reserved[0] flags: a per-tick camera stream follows the input frames; then the start checkpoint.
    inline constexpr uint32_t kFlagHasCamera = 1u;
    inline constexpr uint32_t kFlagHasCheckpoint = 2u;

    inline constexpr const char*    kExtension  = ".HaloReplay";
    inline constexpr const wchar_t* kExtensionW = L".HaloReplay";
}

// Where a game keeps the player actions in its simulation_update, and how big one is.
struct ActionLayout
{
    uint32_t maskOffset = 0x8;
    uint32_t maskBytes = 4;          // 4 (u32) or 8 (u64, Halo 4)
    uint32_t actionsOffset = 0x10;
    uint32_t stride = 0;             // bytes per player_action
    uint32_t slots = 16;
    uint32_t yawOffset = 0x10, pitchOffset = 0x14;   // facing inside one block (absolute radians)
    // the simulation_update's own tick (the game time it simulates; drops on a revert), 0 = use HCM's tick counter.
    // ODST needs it: HCM's ODST tickCounter (*(halo3odst+0x1FE9630)+0x10C) stops while BlockPlayerCharacterInput is on
    // (live 2026-10-27: frozen through all of playback) - so playback waited forever on frame 1.
    uint32_t tickOffset = 0;
    bool valid() const { return stride != 0; }
};

inline ActionLayout replayLayoutFor(GameState::Value g)
{
    ActionLayout l;
    switch (g)
    {
    case GameState::Value::Halo2:     l.actionsOffset = 0x100; l.stride = 0x60; l.yawOffset = 0x08; l.pitchOffset = 0x0C; break;
    case GameState::Value::Halo3:     l.stride = 0x88; break;
    case GameState::Value::Halo3ODST: l.stride = 0x240; l.tickOffset = 0x2410; break;   // REPORT_H3_ODST.md: tick @ +0x2410
    case GameState::Value::HaloReach: l.stride = 0xA8; break;
    case GameState::Value::Halo4:     l.stride = 0x100; l.maskBytes = 8; break;
    default: break;   // invalid (stride 0)
    }
    return l;
}

inline uint32_t replayPopcount(uint64_t m) { uint32_t c = 0; while (m) { c += (uint32_t)(m & 1u); m >>= 1; } return c; }

#pragma pack(push, 1)

// v4 on-disk frame (Halo 2). Only read now (converted to ReplayFrame on load).
struct PlayerActionFrameV4
{
    uint32_t relTick;
    uint32_t mask;
    uint8_t  actions[ReplayFormat::kActionArraySize];
};
static_assert(sizeof(PlayerActionFrameV4) == 8 + ReplayFormat::kActionArraySize, "PlayerActionFrameV4 must be tightly packed");

// One per-tick first-person camera sample (raw engine values: FOV in radians, vectors world-space).
struct CameraSample
{
    float pos[3];   // world position
    float fwd[3];   // lookDirForward
    float up[3];    // lookDirUp
    float fov;      // radians
};
static_assert(sizeof(CameraSample) == 40, "CameraSample must be tightly packed (10 floats)");

// File header. Followed by the frames (see the versions above), then the optional camera stream.
struct ReplayFileHeader
{
    char     magic[4];        // "HCMR"
    uint32_t version;         // 4 (Halo 2 legacy) or 5
    uint32_t gameTickRate;    // 30 or 60 (informational)
    uint32_t startTickAbs;    // absolute tickCounter at record start
    uint32_t playerCount;     // players actually present (SP = 1)
    uint32_t frameCount;      // number of frames following (filled on save)
    char     levelCode[8];    // GetCurrentLevelCode, sanity check on load
    uint32_t reserved[4];     // [0] flags (kFlagHasCamera), v5: [1] GameState::Value, [2] action stride
};
static_assert(sizeof(ReplayFileHeader) == 4 + 4 + 4 + 4 + 4 + 4 + 8 + 16,
              "ReplayFileHeader layout changed - bump version");

#pragma pack(pop)

// One simulation tick of recorded input, in memory: the mask and the blocks of the players it marks (ascending slot order).
struct ReplayFrame
{
    uint32_t relTick = 0;
    uint64_t mask = 0;
    std::vector<uint8_t> slots;     // replayPopcount(mask) * stride bytes
};

// In-memory representation of a loaded/recording replay.
struct ReplayData
{
    ReplayFileHeader         header{};
    std::vector<ReplayFrame> frames;
    std::vector<CameraSample> cameras;   // parallel to frames when header.reserved[0] & kFlagHasCamera
    std::vector<byte>         checkpoint; // the checkpoint the recording started from (header.reserved[0] & kFlagHasCheckpoint)

    GameState::Value game() const { return header.version >= 5 ? (GameState::Value)header.reserved[1] : GameState::Value::Halo2; }
    uint32_t stride() const { return header.version >= 5 ? header.reserved[2] : (uint32_t)ReplayFormat::kPlayerActionStride; }

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

// ---- file I/O shared by the recorder and the player ----
// write a v5 file; throws HCMRuntimeException-free std::runtime_error text through the bool + error string
inline bool replayWriteV5(std::ostream& out, const ReplayData& d, std::string& err)
{
    out.write(reinterpret_cast<const char*>(&d.header), sizeof(ReplayFileHeader));
    for (const ReplayFrame& f : d.frames)
    {
        out.write(reinterpret_cast<const char*>(&f.relTick), 4);
        out.write(reinterpret_cast<const char*>(&f.mask), 8);
        if (!f.slots.empty()) out.write(reinterpret_cast<const char*>(f.slots.data()), (std::streamsize)f.slots.size());
    }
    if (d.hasCamera())
        out.write(reinterpret_cast<const char*>(d.cameras.data()), (std::streamsize)(d.cameras.size() * sizeof(CameraSample)));
    if (d.header.reserved[0] & ReplayFormat::kFlagHasCheckpoint)
    {
        const uint64_t size = d.checkpoint.size();
        out.write(reinterpret_cast<const char*>(&size), 8);
        out.write(reinterpret_cast<const char*>(d.checkpoint.data()), (std::streamsize)size);
    }
    if (!out) { err = "write failed (disk full / flush error)"; return false; }
    return true;
}

// read a v4 (Halo 2) or v5 file into d. false + err on anything malformed.
inline bool replayRead(std::istream& in, ReplayData& d, std::string& err)
{
    d = ReplayData{};
    in.read(reinterpret_cast<char*>(&d.header), sizeof(ReplayFileHeader));
    if (in.gcount() != sizeof(ReplayFileHeader)) { err = "file too small / truncated header"; return false; }
    if (!d.magicValid()) { err = "invalid file (bad magic)"; return false; }
    const uint32_t n = d.header.frameCount;
    if (n > 50'000'000) { err = "frame count implausible"; return false; }
    if (d.header.version == ReplayFormat::kVersionV4)
    {
        d.frames.resize(n);
        PlayerActionFrameV4 v4;
        for (uint32_t i = 0; i < n; ++i)
        {
            in.read(reinterpret_cast<char*>(&v4), sizeof(v4));
            if (in.gcount() != sizeof(v4)) { err = "truncated frames"; return false; }
            ReplayFrame& f = d.frames[i];
            f.relTick = v4.relTick; f.mask = v4.mask;
            for (uint32_t s = 0; s < ReplayFormat::kMaxPlayers; ++s)
                if (f.mask & (1ull << s))
                    f.slots.insert(f.slots.end(), v4.actions + s * ReplayFormat::kPlayerActionStride, v4.actions + (s + 1) * ReplayFormat::kPlayerActionStride);
        }
    }
    else if (d.header.version == ReplayFormat::kVersion)
    {
        const uint32_t stride = d.header.reserved[2];
        if (stride == 0 || stride > 0x1000) { err = "action stride implausible"; return false; }
        d.frames.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            ReplayFrame& f = d.frames[i];
            in.read(reinterpret_cast<char*>(&f.relTick), 4);
            in.read(reinterpret_cast<char*>(&f.mask), 8);
            if (!in) { err = "truncated frames"; return false; }
            const uint32_t c = replayPopcount(f.mask);
            if (c > 16) { err = "frame mask implausible"; return false; }
            f.slots.resize((size_t)c * stride);
            if (!f.slots.empty()) in.read(reinterpret_cast<char*>(f.slots.data()), (std::streamsize)f.slots.size());
            if (!in) { err = "truncated frames"; return false; }
        }
    }
    else { err = std::format("unsupported version {} (expected 4 or 5)", d.header.version); return false; }

    if ((d.header.reserved[0] & ReplayFormat::kFlagHasCamera) && n)
    {
        d.cameras.resize(n);
        in.read(reinterpret_cast<char*>(d.cameras.data()), (std::streamsize)(n * sizeof(CameraSample)));
        if (in.gcount() != (std::streamsize)(n * sizeof(CameraSample))) { err = "truncated camera stream"; return false; }
    }
    if (d.header.reserved[0] & ReplayFormat::kFlagHasCheckpoint)
    {
        uint64_t size = 0;
        in.read(reinterpret_cast<char*>(&size), 8);
        if (!in || size == 0 || size > (256ull << 20)) { err = "checkpoint section missing or implausible"; return false; }
        d.checkpoint.resize((size_t)size);
        in.read(reinterpret_cast<char*>(d.checkpoint.data()), (std::streamsize)size);
        if (in.gcount() != (std::streamsize)size) { err = "truncated checkpoint"; return false; }
    }
    return true;
}
