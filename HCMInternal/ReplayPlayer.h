#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include <filesystem>

// Plays back a recorded .h2r. Flow:
//   1. loadReplay(path)   - read + validate file into memory
//   2. startPlayback()    - arm; subscribe to revert + player-action-update
//   3. <user reverts the matching checkpoint> - feeding begins, real input blocked
//   4. inputs overwritten each tick until end-of-stream -> auto stop
class ReplayPlayer : public IOptionalCheat
{
private:
    class ReplayPlayerImpl;
    std::unique_ptr<ReplayPlayerImpl> pimpl;

public:
    ReplayPlayer(GameState game, IDIContainer& dicon);
    ~ReplayPlayer();

    bool loadReplay(const std::filesystem::path& inFile);
    void startPlayback();   // arm: begins on next revert
    void stopPlayback();
    bool isLoaded() const;
    bool isArmed() const;
    bool isPlaying() const;

    std::string_view getName() override { return nameof(ReplayPlayer); }
};
