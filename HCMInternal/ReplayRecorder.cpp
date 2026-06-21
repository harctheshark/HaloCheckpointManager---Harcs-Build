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
        return std::format("{}_{}tick.h2r", buf, tickRate);
    }

    uint32_t currentTick()
    {
        lockOrThrow(gameTickHookWeak, gameTickHook);
        return gameTickHook->getCurrentGameTick();
    }

    bool cameraAvailable() const { return getGameCameraDataOptionalWeak.has_value() && !getGameCameraDataOptionalWeak.value().expired(); }

    void beginCapture()
    {
        mData.frames.clear();
        mData = ReplayData{};
        std::memcpy(mData.header.magic, ReplayFormat::kMagic, 4);
        mData.header.version = ReplayFormat::kVersion;
        mData.header.gameTickRate = mTickRate; // chosen via the Record (30/60) action
        mData.header.startTickAbs = currentTick();
        mData.header.playerCount = 1; // SP; refined from observed mask below
        mData.header.reserved[0] = cameraAvailable() ? ReplayFormat::kFlagHasCamera : 0u;
        mRecording = true;
        mDbgPrevRecRel = 0xFFFFFFFFu;
        mHaveRecordedTick = false;

        lockOrThrow(messagesWeak, messages);
        messages->addMessage("Replay recording started.");
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

            uint32_t cur = currentTick();

            // revert / checkpoint-load = the tick went backwards -> (re)start a clean capture here
            if (mHaveLastTick && cur < mLastTick)
                beginCapture();
            mLastTick = cur;
            mHaveLastTick = true;

            if (!mRecording) return; // armed but waiting for the first revert

            // THE HOOK FIRES PER RENDERED FRAME, not per sim tick. Record exactly ONE frame per sim tick
            // (when the game tick advances) so playback is framerate-independent and stays deterministic.
            if (mHaveRecordedTick && cur == mLastRecordedTick) return;
            mLastRecordedTick = cur;
            mHaveRecordedTick = true;

            PlayerActionFrame f{};
            f.relTick = cur - mData.header.startTickAbs;
            f.mask = *pac.maskRef;
            std::memcpy(f.actions, pac.actions, ReplayFormat::kActionArraySize);
            mData.frames.push_back(f);

            // #2 tick-alignment diagnostic: log the first few frames + any non-+1 relTick step (a skipped/
            // doubled sim tick during recording would desync playback). mDbgPrevRecRel==0xFFFFFFFF = first.
            {
                bool firstFew = mData.frames.size() <= 8;
                bool gap = (mDbgPrevRecRel != 0xFFFFFFFFu) && (f.relTick != mDbgPrevRecRel + 1);
                if (firstFew || gap)
                    PLOG_INFO << "[replay-rec] frame=" << (mData.frames.size() - 1) << " absTick=" << cur
                              << " relTick=" << f.relTick << (gap ? "  <-- GAP (step!=1)" : "");
                mDbgPrevRecRel = f.relTick;
            }

            // capture the FP camera for this tick (kept 1:1 with frames). On any failure push a zeroed
            // sample so the parallel arrays stay aligned.
            if (mData.header.reserved[0] & ReplayFormat::kFlagHasCamera)
                mData.cameras.push_back(sampleCameraOrZero());

            // track how many player slots are actually used
            uint32_t m = f.mask;
            uint32_t count = 0; while (m) { count += (m & 1u); m >>= 1; }
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
    void onRecordEvent(uint32_t tickRate)
    {
        mTickRate = tickRate;
        arm();
    }

    // GUI/hotkey: Stop & Save - flush the recording to an auto-named .h2r under <HCM dir>\replays\.
    void onStopSaveEvent()
    {
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
        mRecord30Callback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayRecord30Event, [this]() { onRecordEvent(30); }),
        mRecord60Callback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayRecord60Event, [this]() { onRecordEvent(60); }),
        mStopSaveCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayStopSaveEvent, [this]() { onStopSaveEvent(); })
    {
        gameTickHookWeak = resolveDependentCheat(GameTickEventHook);
        playerActionHookWeak = resolveDependentCheat(PlayerActionUpdateHook);
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
        mData.header.reserved[0] = writeCamera ? ReplayFormat::kFlagHasCamera : 0u;

        try
        {
            std::filesystem::create_directories(outFile.parent_path());
            std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
            if (!out) throw HCMRuntimeException(std::format("Replay save: could not open {}", outFile.string()));

            out.write(reinterpret_cast<const char*>(&mData.header), sizeof(ReplayFileHeader));
            out.write(reinterpret_cast<const char*>(mData.frames.data()),
                      (std::streamsize)(mData.frames.size() * sizeof(PlayerActionFrame)));
            if (writeCamera)
                out.write(reinterpret_cast<const char*>(mData.cameras.data()),
                          (std::streamsize)(mData.cameras.size() * sizeof(CameraSample)));
            out.close();
            if (!out) throw HCMRuntimeException("Replay save: write failed (disk full / flush error)");

            if (auto m = messagesWeak.lock())
                m->addMessage(std::format("Replay saved: {} frames{} -> {}", mData.header.frameCount,
                    writeCamera ? " (+camera)" : "", outFile.filename().string()));
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
