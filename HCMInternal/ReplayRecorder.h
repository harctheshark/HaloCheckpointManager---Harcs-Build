#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include <filesystem>

// Records per-tick player input. Flow (matches user spec + H2V c_replay_system):
//   1. arm()          - subscribe to revert + player-action-update events
//   2. <user reverts> - capture begins; startTickAbs = current tick
//   3. capture each tick until...
//   4. stopAndSave()  - write .h2r (+ optionally dump checkpoint), disarm
class ReplayRecorder : public IOptionalCheat
{
private:
    class ReplayRecorderImpl;
    std::unique_ptr<ReplayRecorderImpl> pimpl;

public:
    ReplayRecorder(GameState game, IDIContainer& dicon);
    ~ReplayRecorder();

    // Arm/disarm. While armed, the next revert (re)starts capture.
    void arm();
    void disarm();
    bool isArmed() const;
    bool isRecording() const;
    size_t frameCount() const;

    // Stop capturing and write the replay to disk. Returns true on success.
    bool stopAndSave(const std::filesystem::path& outFile);

    std::string_view getName() override { return nameof(ReplayRecorder); }
};
