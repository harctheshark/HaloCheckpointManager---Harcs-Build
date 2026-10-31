#include "pch.h"
#include "ReplayRecorder.h"
#include "ReplayData.h"
#include "PlayerActionUpdateHook.h"
#include "GameTickEventHook.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "DirPathContainer.h"
#include "GetGameCameraData.h"
#include "IMakeOrGetCheat.h"
#include "GetCurrentLevelCode.h"
#include "ReplayCheckpoint.h"
#include "ReplayGameTls.h"
#include <fstream>
#include <ctime>

class ReplayRecorder::ReplayRecorderImpl
{
private:
    GameState mGame;

    // injected services
    std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
    std::weak_ptr<IMessagesGUI> messagesWeak;
    std::weak_ptr<IMCCStateHook> mccStateHookWeak;
    std::weak_ptr<GameTickEventHook> gameTickHookWeak;
    std::weak_ptr<PlayerActionUpdateHook> playerActionHookWeak;
    // Optional: per-tick FP camera capture (the FP camera is a separate hierarchy from player_actions).
    std::optional<std::weak_ptr<GetGameCameraData>> getGameCameraDataOptionalWeak;
    std::optional<std::weak_ptr<GetCurrentLevelCode>> levelCodeOptionalWeak;
    // the checkpoint the recording starts from goes into the file, so playing it back is one step
    std::unique_ptr<ReplayCheckpoint> mCheckpoint;

    // event subscription (null unless armed). Revert detection is done by watching for the game tick
    // to DECREASE on the per-tick hook (matches the original xlive c_replay_system) - HCM's
    // RevertEventHook has no Halo2 pointer data so we can't depend on it here.
    std::unique_ptr<ScopedCallback<PlayerActionUpdateEvent>> mUpdateCallback;

    // persistent GUI/hotkey event subscriptions (live for the lifetime of the service)
    ScopedCallback<ActionEvent> mRecord30Callback;
    ScopedCallback<ActionEvent> mRecord60Callback;
    ScopedCallback<ActionEvent> mStopSaveCallback;

    // where saved replays go (<HCM dir>\replays\)
    std::string mReplayDir;

    // state
    bool mArmed = false;
    bool mRecording = false;
    uint32_t mTickRate = 60; // set by the Record (30/60) actions before arming
    uint32_t mLastTick = 0;  // for revert (tick-decrease) detection
    bool mHaveLastTick = false;
    uint32_t mDbgPrevRecRel = 0xFFFFFFFFu; // #2 diagnostic: previous frame's relTick (0xFFFFFFFF = none yet)
    uint32_t mLastRecordedTick = 0;        // dedup: last game tick we committed a frame for
    bool mHaveRecordedTick = false;
    ReplayData mData;

    static std::string makeTimestampedName(uint32_t tickRate)
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[64];
        std::strftime(buf, sizeof(buf), "replay_%Y%m%d_%H%M%S", &tm);
        return std::format("{}_{}tick{}", buf, tickRate, ReplayFormat::kExtension);
    }

    uint32_t currentTick()
    {
        lockOrThrow(gameTickHookWeak, gameTickHook);
        return gameTickHook->getCurrentGameTick();
    }

    bool cameraAvailable() const { return getGameCameraDataOptionalWeak.has_value() && !getGameCameraDataOptionalWeak.value().expired(); }

    void beginCapture(uint32_t cur)
    {
        mData.frames.clear();
        mData = ReplayData{};
        std::memcpy(mData.header.magic, ReplayFormat::kMagic, 4);
        mData.header.version = ReplayFormat::kVersion;
        mData.header.reserved[1] = (uint32_t)mGame.operator GameState::Value();         // v5: the game ...
        mData.header.reserved[2] = replayLayoutFor(mGame.operator GameState::Value()).stride;   // ... and its action size
        // the rate being recorded: read from the game here (game thread, at the revert that starts the capture)
        if (const int rate = ReplayGameTls::simTickRate(mGame); rate > 0) mTickRate = (uint32_t)rate;
        else PLOG_ERROR << "[replay-rec] tick rate unreadable - recording as " << mTickRate;
        mData.header.gameTickRate = mTickRate;
        mData.header.startTickAbs = cur;
        mData.header.playerCount = 1; // SP; refined from observed mask below
        mData.header.reserved[0] = cameraAvailable() ? ReplayFormat::kFlagHasCamera : 0u;
        if (levelCodeOptionalWeak.has_value())
            if (auto levelCode = levelCodeOptionalWeak.value().lock())
                try { const std::string code = levelCode->getCurrentLevelCode(); std::memcpy(mData.header.levelCode, code.data(), (std::min)(code.size(), sizeof(mData.header.levelCode))); }
                catch (HCMRuntimeException) {}
        // the tick just went backwards = a revert loaded the current checkpoint: that is where this recording starts
        if (mCheckpoint)
            try { mData.checkpoint = mCheckpoint->read(); mData.header.reserved[0] |= ReplayFormat::kFlagHasCheckpoint; }
            catch (HCMRuntimeException ex) { PLOG_ERROR << "[replay-rec] start checkpoint not captured (the replay will need it loaded by hand): " << ex.what(); }
        mRecording = true;
        mDbgPrevRecRel = 0xFFFFFFFFu;
        mHaveRecordedTick = false;

        lockOrThrow(messagesWeak, messages);
        messages->addMessage(std::format("Replay recording started ({} tick).", mTickRate));
        PLOG_INFO << "[replay-rec] anchor startTickAbs=" << mData.header.startTickAbs;
    }

    // fires every sim tick while armed. Detects reverts via tick-decrease (start/restart capture from
    // the checkpoint), then records the per-tick input while recording.
    void onPlayerActionUpdate(PlayerActionUpdateContext& pac)
    {
        if (!mArmed) return;
        try
        {
            lockOrThrow(mccStateHookWeak, mccStateHook);
            if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

            const uint32_t cur = pac.hasTick() ? pac.tick() : currentTick();

            // revert / checkpoint-load = the tick went backwards -> (re)start a clean capture here
            if (mHaveLastTick && cur < mLastTick)
                beginCapture(cur);
            mLastTick = cur;
            mHaveLastTick = true;

            if (!mRecording) return; // armed but waiting for the first revert

            // THE HOOK FIRES PER RENDERED FRAME, not per sim tick. Record exactly ONE frame per sim tick
            // (when the game tick advances) so playback is framerate-independent and stays deterministic.
            if (mHaveRecordedTick && cur == mLastRecordedTick) return;
            mLastRecordedTick = cur;
            mHaveRecordedTick = true;

            // the mask and the blocks of the players it marks (ascending slot order) - in SP one block per tick
            ReplayFrame f{};
            f.relTick = cur - mData.header.startTickAbs;
            f.mask = pac.getMask();
            const uint32_t stride = pac.layout->stride;
            for (uint32_t s = 0; s < pac.layout->slots; ++s)
                if (f.mask & (1ull << s))
                    f.slots.insert(f.slots.end(), pac.actions + (size_t)s * stride, pac.actions + (size_t)(s + 1) * stride);
            mData.frames.push_back(std::move(f));
            const ReplayFrame& fr = mData.frames.back();

            // #2 tick-alignment diagnostic: log the first few frames + any non-+1 relTick step (a skipped/
            // doubled sim tick during recording would desync playback). mDbgPrevRecRel==0xFFFFFFFF = first.
            {
                bool firstFew = mData.frames.size() <= 8;
                bool gap = (mDbgPrevRecRel != 0xFFFFFFFFu) && (fr.relTick != mDbgPrevRecRel + 1);
                if (firstFew || gap)
                    PLOG_INFO << "[replay-rec] frame=" << (mData.frames.size() - 1) << " absTick=" << cur
                              << " relTick=" << fr.relTick << " mask=" << fr.mask << (gap ? "  <-- GAP (step!=1)" : "");
                mDbgPrevRecRel = fr.relTick;
            }

            // capture the FP camera for this tick (kept 1:1 with frames). On any failure push a zeroed
            // sample so the parallel arrays stay aligned.
            if (mData.header.reserved[0] & ReplayFormat::kFlagHasCamera)
                mData.cameras.push_back(sampleCameraOrZero());

            // track how many player slots are actually used
            uint32_t count = replayPopcount(fr.mask);
            if (count > mData.header.playerCount) mData.header.playerCount = count;
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
    }

    CameraSample sampleCameraOrZero()
    {
        CameraSample s{};
        try
        {
            if (!getGameCameraDataOptionalWeak.has_value()) return s;
            auto getCam = getGameCameraDataOptionalWeak.value().lock();
            if (!getCam) return s;
            GameCameraData cam = getCam->getGameCameraData();
            if (cam.position)       { s.pos[0] = cam.position->x; s.pos[1] = cam.position->y; s.pos[2] = cam.position->z; }
            if (cam.lookDirForward) { s.fwd[0] = cam.lookDirForward->x; s.fwd[1] = cam.lookDirForward->y; s.fwd[2] = cam.lookDirForward->z; }
            if (cam.lookDirUp)      { s.up[0] = cam.lookDirUp->x; s.up[1] = cam.lookDirUp->y; s.up[2] = cam.lookDirUp->z; }
            if (cam.FOV)            s.fov = *cam.FOV;
        }
        catch (HCMRuntimeException) { /* keep the zeroed sample; alignment preserved */ }
        return s;
    }

    // GUI/hotkey: Record (30/60 tick) - choose the tickrate and arm; capture begins on the next revert.
    // One recorder exists per game and they all hear the same GUI events: only the game being played answers.
    bool isMyGame()
    {
        auto mccStateHook = mccStateHookWeak.lock();
        return mccStateHook && mccStateHook->isGameCurrentlyPlaying(mGame);
    }

    // GUI/hotkey: Record - arm; the tick rate is read from the game when the capture starts (both old Record (30) / (60)
    // hotkeys land here, so existing bindings keep working)
    void onRecordEvent()
    {
        if (!isMyGame()) return;
        arm();
    }

    // GUI/hotkey: Stop & Save - flush the recording to an auto-named .HaloReplay under <HCM dir>\replays\.
    void onStopSaveEvent()
    {
        if (!mArmed && !mRecording && !isMyGame()) return;   // another game's recorder
        try
        {
            std::filesystem::path out = std::filesystem::path(mReplayDir) / "replays" / makeTimestampedName(mTickRate);
            stopAndSave(out);
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
    }

public:
    ReplayRecorderImpl(GameState game, IDIContainer& dicon)
        : mGame(game),
        runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>().lock()),
        messagesWeak(dicon.Resolve<IMessagesGUI>()),
        mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
        mRecord30Callback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayRecord30Event, [this]() { onRecordEvent(); }),
        mRecord60Callback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayRecord60Event, [this]() { onRecordEvent(); }),
        mStopSaveCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayStopSaveEvent, [this]() { onStopSaveEvent(); })
    {
        gameTickHookWeak = resolveDependentCheat(GameTickEventHook);
        playerActionHookWeak = resolveDependentCheat(PlayerActionUpdateHook);
        try { levelCodeOptionalWeak = resolveDependentCheat(GetCurrentLevelCode); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayRecorder: GetCurrentLevelCode unavailable: " << ex.what(); }
        try { mCheckpoint = std::make_unique<ReplayCheckpoint>(game, dicon); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayRecorder: no checkpoint data for this game (replays will not carry one): " << ex.what(); }
        try { getGameCameraDataOptionalWeak = resolveDependentCheat(GetGameCameraData); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayRecorder: GetGameCameraData unavailable (no camera will be recorded): " << ex.what(); }
        mReplayDir = dicon.Resolve<DirPathContainer>().lock()->dirPath;
    }

    void arm()
    {
        try
        {
            lockOrThrow(playerActionHookWeak, playerActionHook);

            mHaveLastTick = false; // re-baseline tick-decrease detection
            mUpdateCallback = playerActionHook->getPlayerActionUpdateEvent()->subscribe([this](PlayerActionUpdateContext& p) { onPlayerActionUpdate(p); });

            mArmed = true;
            lockOrThrow(messagesWeak, messages);
            messages->addMessage("Replay armed - revert a checkpoint to begin recording.");
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); disarm(); }
    }

    void disarm()
    {
        mArmed = false;
        mRecording = false;
        mHaveLastTick = false;
        mUpdateCallback.reset();
    }

    bool isArmed() const { return mArmed; }
    bool isRecording() const { return mRecording; }
    size_t frameCount() const { return mData.frames.size(); }

    bool stopAndSave(const std::filesystem::path& outFile)
    {
        bool wasRecording = mRecording;
        mRecording = false;

        if (!wasRecording || mData.frames.empty())
        {
            if (auto m = messagesWeak.lock()) m->addMessage("Replay save: nothing recorded.");
            disarm();
            return false;
        }

        mData.header.frameCount = (uint32_t)mData.frames.size();

        // keep the camera flag honest: only claim camera data if it's actually present 1:1 with frames
        const bool writeCamera = mData.cameras.size() == mData.frames.size() && !mData.cameras.empty();
        // keep the other flags (the start checkpoint's) - only the camera bit is decided here
        mData.header.reserved[0] = (mData.header.reserved[0] & ~ReplayFormat::kFlagHasCamera) | (writeCamera ? ReplayFormat::kFlagHasCamera : 0u);
        const bool writeCheckpoint = (mData.header.reserved[0] & ReplayFormat::kFlagHasCheckpoint) && !mData.checkpoint.empty();
        if (!writeCheckpoint) mData.header.reserved[0] &= ~ReplayFormat::kFlagHasCheckpoint;

        try
        {
            std::filesystem::create_directories(outFile.parent_path());
            std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
            if (!out) throw HCMRuntimeException(std::format("Replay save: could not open {}", outFile.string()));

            std::string err;
            if (!replayWriteV5(out, mData, err)) throw HCMRuntimeException("Replay save: " + err);
            out.close();
            if (!out) throw HCMRuntimeException("Replay save: write failed (disk full / flush error)");

            if (auto m = messagesWeak.lock())
                m->addMessage(std::format("Replay saved: {} frames{}{} -> {}", mData.header.frameCount,
                    writeCamera ? " (+camera)" : "", writeCheckpoint ? " (+checkpoint)" : " (NO checkpoint - load it by hand to play)",
                    outFile.filename().string()));
            PLOG_INFO << "[replay-rec] saved " << mData.header.frameCount << " frames, relTick span ["
                      << mData.frames.front().relTick << ".." << mData.frames.back().relTick
                      << "] startTickAbs=" << mData.header.startTickAbs << " -> " << outFile.string();
            disarm();
            return true;
        }
        catch (HCMRuntimeException ex)
        {
            runtimeExceptions->handleMessage(ex);
            disarm();
            return false;
        }
    }
};

ReplayRecorder::ReplayRecorder(GameState game, IDIContainer& dicon)
    : pimpl(std::make_unique<ReplayRecorderImpl>(game, dicon)) {}
ReplayRecorder::~ReplayRecorder() { PLOG_DEBUG << "~" << getName(); }

void ReplayRecorder::arm() { pimpl->arm(); }
void ReplayRecorder::disarm() { pimpl->disarm(); }
bool ReplayRecorder::isArmed() const { return pimpl->isArmed(); }
bool ReplayRecorder::isRecording() const { return pimpl->isRecording(); }
size_t ReplayRecorder::frameCount() const { return pimpl->frameCount(); }
bool ReplayRecorder::stopAndSave(const std::filesystem::path& outFile) { return pimpl->stopAndSave(outFile); }
