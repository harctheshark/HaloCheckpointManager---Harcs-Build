#include "pch.h"
#include "ReplayPlayer.h"
#include "ReplayData.h"
#include "PlayerActionUpdateHook.h"
#include "GameTickEventHook.h"
#include "BlockPlayerCharacterInput.h"
#include "SharedRequestToken.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "DirPathContainer.h"
#include "GetPlayerViewAngle.h"
#include "GetGameCameraData.h"
#include "GameCameraData.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "ScopedAtomicBool.h"
#include "IMakeOrGetCheat.h"
#include <fstream>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

class ReplayPlayer::ReplayPlayerImpl
{
private:
    GameState mGame;

    std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
    std::weak_ptr<IMessagesGUI> messagesWeak;
    std::weak_ptr<IMCCStateHook> mccStateHookWeak;
    std::weak_ptr<SettingsStateAndEvents> settingsWeak; // to check if HCM freecam is active (then we yield the camera to it)
    std::weak_ptr<GameTickEventHook> gameTickHookWeak;
    std::weak_ptr<PlayerActionUpdateHook> playerActionHookWeak;
    std::optional<std::weak_ptr<BlockPlayerCharacterInput>> blockInputOptionalWeak;
    // Optional: used to re-assert the recorded view angle onto the unit each tick. The player_action
    // block carries the biped facing, but the FIRST-PERSON CAMERA is a separate hierarchy driven by
    // live look input (frozen while we block input) - writing playerViewAngle (unit-control +0x38, the
    // same field the block facing is copied from) makes the FP view follow the recorded aim.
    std::optional<std::weak_ptr<GetPlayerViewAngle>> getPlayerViewAngleOptionalWeak;
    // FP camera is a separate hierarchy; we replay it by overriding the engine's per-frame camera set.
    std::optional<std::weak_ptr<GetGameCameraData>> getGameCameraDataOptionalWeak;
    std::unique_ptr<ModuleMidHook> mCameraHook;              // on setCameraDataFunction; attached only while playing
    // The input hook is now PER SIM TICK, but the camera hook is PER FRAME. We hold this tick's camera (A)
    // and the next tick's (B) and interpolate A->B per frame by real sub-tick time, so the camera stays
    // smooth at any fps from a tick-rate recording (the reference's between-tick interpolation).
    CameraSample mCamA{}, mCamB{};
    bool mCamValid = false;

    // static plumbing for the camera midhook (ReplayPlayer is single-instance, Halo2 only)
    static inline ReplayPlayerImpl* cameraHookInstance = nullptr;
    static inline std::atomic_bool cameraHookIsRunning = false;

    static void cameraHookFunction(SafetyHookContext&)
    {
        ScopedAtomicBool lock(cameraHookIsRunning);
        if (cameraHookInstance) cameraHookInstance->applyPendingCamera();
    }

    static void normalize3(float v[3])
    {
        float m = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (m > 1e-6f) { v[0]/=m; v[1]/=m; v[2]/=m; }
    }

    // runs every frame (engine's camera-set) while attached: overwrite the live camera ORIENTATION with the
    // recorded one, interpolated between this tick's sample (A) and the next (B) by real sub-tick time.
    void applyPendingCamera()
    {
        if (!mPlaying || !mCamValid) return;
        // If HCM's freecam is active, leave the camera entirely to it (montage / external dollycam control)
        // and just keep replaying the player's inputs. Without this the replay's per-frame camera override
        // fights freecam whenever playback was started BEFORE freecam was enabled: the two share the
        // setCameraDataFunction hook and the earlier-attached one runs last and wins.
        if (auto settings = settingsWeak.lock(); settings && settings->freeCameraToggle->GetValue()) return;
        if (!getGameCameraDataOptionalWeak.has_value()) return;
        auto getCam = getGameCameraDataOptionalWeak.value().lock();
        if (!getCam) return;
        try
        {
            // sub-tick fraction = the engine's own interpolation alpha (so the camera rides the exact same
            // 0..1 the renderer uses for the body -> smooth at any fps). Fall back to 0 (snap) if unavailable.
            float ft = 0.f;
            if (mInterpAlpha)
            {
                float a = 0.f;
                if (mInterpAlpha->readData(&a)) ft = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
            }

            float fwd[3], up[3];
            for (int i = 0; i < 3; ++i)
            {
                fwd[i] = mCamA.fwd[i] + (mCamB.fwd[i] - mCamA.fwd[i]) * ft;
                up[i]  = mCamA.up[i]  + (mCamB.up[i]  - mCamA.up[i])  * ft;
            }
            float fov = mCamA.fov + (mCamB.fov - mCamA.fov) * ft;
            normalize3(fwd);
            normalize3(up);

            GameCameraData cam = getCam->getGameCameraData();
            // ORIENTATION + FOV only. Camera POSITION is left to the engine (pinned to the re-simulated biped
            // eye) - forcing recorded absolute position detaches the world-space FP legs from the view.
            if (cam.lookDirForward) *cam.lookDirForward = SimpleMath::Vector3(fwd[0], fwd[1], fwd[2]);
            if (cam.lookDirUp)      *cam.lookDirUp = SimpleMath::Vector3(up[0], up[1], up[2]);
            if (cam.FOV)            *cam.FOV = fov;
        }
        catch (HCMRuntimeException) { /* swallow on the render thread; keep replaying */ }
    }

    // Revert is detected by watching for the game tick to DECREASE on the per-tick hook (matches the
    // original xlive c_replay_system) - HCM's RevertEventHook has no Halo2 pointer data.
    std::unique_ptr<ScopedCallback<PlayerActionUpdateEvent>> mUpdateCallback;
    std::shared_ptr<SharedRequestToken> mBlockInputToken; // held while playing

    // persistent GUI/hotkey event subscriptions (live for the lifetime of the service)
    ScopedCallback<ActionEvent> mLoadCallback;
    ScopedCallback<ActionEvent> mPlayCallback;
    ScopedCallback<ActionEvent> mStopCallback;
    std::string mReplayDir; // <HCM dir>; replays live under <dir>\replays\

    ReplayData mData;
    bool mLoaded = false;
    bool mArmed = false;
    bool mPlaying = false;
    uint32_t mPlaybackStartTick = 0;
    uint32_t mLastTick = 0;       // for revert (tick-decrease) detection while armed
    bool mHaveLastTick = false;
    uint32_t mDbgPrevPlayCurRel = 0xFFFFFFFFu; // #2 diagnostic: previous tick's curRel (0xFFFFFFFF = none yet)
    size_t mFrameIndex = 0;
    // engine sub-tick interpolation alpha (the SAME 0..1 the renderer uses to interpolate the body between
    // ticks). Driving the camera lerp with this makes it track the body smoothly at ANY framerate, including
    // when capture fps != playback fps. Null -> fall back to snapping to the tick sample.
    std::shared_ptr<MultilevelPointer> mInterpAlpha;

    uint32_t currentTick()
    {
        lockOrThrow(gameTickHookWeak, gameTickHook);
        return gameTickHook->getCurrentGameTick();
    }

    void finishPlayback(const char* reason)
    {
        if (auto m = messagesWeak.lock()) m->addMessage(std::format("Replay playback {}.", reason));
        PLOG_INFO << "Replay playback ended: " << reason;
        stopPlaybackInternal();
    }

    void stopPlaybackInternal()
    {
        mPlaying = false;
        mArmed = false;
        mFrameIndex = 0;
        mHaveLastTick = false;
        mCamValid = false;
        if (mCameraHook) mCameraHook->setWantsToBeAttached(false); // stop overriding the camera
        mBlockInputToken.reset();   // re-enable real input
        mUpdateCallback.reset();
    }

    // the revert (tick-decrease) that loads the matching checkpoint has happened -> anchor + start feeding
    void beginPlaybackNow(uint32_t cur)
    {
        mPlaybackStartTick = cur;
        mFrameIndex = 0;
        mPlaying = true;

        // block real player input so it cannot fight the injected values
        if (blockInputOptionalWeak.has_value())
        {
            if (auto block = blockInputOptionalWeak.value().lock())
                mBlockInputToken = block->makeScopedRequest();
        }

        // start overriding the FP camera if this replay has a recorded camera stream
        mCamValid = false;
        if (mData.hasCamera() && mCameraHook)
            mCameraHook->setWantsToBeAttached(true);

        mDbgPrevPlayCurRel = 0xFFFFFFFFu;
        PLOG_INFO << "[replay-play] anchor startTick=" << cur << " frames=" << mData.frames.size()
                  << " first.relTick=" << (mData.frames.empty() ? 0u : mData.frames.front().relTick)
                  << " last.relTick=" << (mData.frames.empty() ? 0u : mData.frames.back().relTick);

        if (auto m = messagesWeak.lock())
            m->addMessage(std::format("Replay playback started ({} frames{}).", mData.frames.size(),
                mData.hasCamera() ? " +camera" : ""));
    }

    // Re-assert player 0's recorded facing (yaw @block+0x08, pitch @block+0x0C) onto the unit's view
    // angle so the first-person camera follows the recording. No-op if the angle service is unavailable
    // or player 0 isn't active this frame.
    void assertRecordedViewAngle(const PlayerActionFrame& f)
    {
        if (!(f.mask & 1u)) return;
        if (!getPlayerViewAngleOptionalWeak.has_value()) return;
        auto getPlayerViewAngle = getPlayerViewAngleOptionalWeak.value().lock();
        if (!getPlayerViewAngle) return;

        float yaw, pitch;
        std::memcpy(&yaw, f.actions + 0x08, sizeof(float));
        std::memcpy(&pitch, f.actions + 0x0C, sizeof(float));
        try { getPlayerViewAngle->setPlayerViewAngle(SimpleMath::Vector2(yaw, pitch)); }
        catch (HCMRuntimeException) { /* non-fatal: keep replaying inputs even if the view write fails */ }
    }

    // Capture this tick's camera pair (A=this tick, B=next) for the per-frame camera hook to interpolate
    // between, using the engine's interp alpha as the fraction.
    void setCameraPairForTick(size_t idx)
    {
        if (!mData.hasCamera() || idx >= mData.cameras.size()) { mCamValid = false; return; }
        mCamA = mData.cameras[idx];
        mCamB = (idx + 1 < mData.cameras.size()) ? mData.cameras[idx + 1] : mCamA;
        mCamValid = true;
    }

    void onPlayerActionUpdate(PlayerActionUpdateContext& pac)
    {
        if (!mArmed) return;
        try
        {
            lockOrThrow(mccStateHookWeak, mccStateHook);
            if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

            uint32_t cur = currentTick();

            if (!mPlaying)
            {
                // wait for the revert (tick went backwards) that loads the matching checkpoint
                if (mHaveLastTick && cur < mLastTick)
                    beginPlaybackNow(cur);
                mLastTick = cur;
                mHaveLastTick = true;
                if (!mPlaying) return; // still waiting
            }

            uint32_t curRel = cur - mPlaybackStartTick;

            // This hook now fires ONCE PER SIM TICK (sub_1806A3910), so we inject exactly one frame per tick.
            // #2 diagnostic: flag a sim-tick step != 1 (would skip recorded ticks)
            if (mDbgPrevPlayCurRel != 0xFFFFFFFFu && curRel != mDbgPrevPlayCurRel + 1)
                PLOG_INFO << "[replay-play] TICK STEP!=1: prevCurRel=" << mDbgPrevPlayCurRel << " curRel=" << curRel
                          << " absTick=" << cur;
            mDbgPrevPlayCurRel = curRel;

            // advance to the frame for this tick (normally exactly +1 per call)
            while (mFrameIndex < mData.frames.size() && mData.frames[mFrameIndex].relTick < curRel)
                ++mFrameIndex;

            if (mFrameIndex >= mData.frames.size()) { finishPlayback("finished"); return; }

            const PlayerActionFrame& f = mData.frames[mFrameIndex];
            if (f.relTick == curRel)
            {
                if (mFrameIndex < 8)
                    PLOG_INFO << "[replay-play] inject idx=" << mFrameIndex << " absTick=" << cur
                              << " curRel=" << curRel << " relTick=" << f.relTick;
                *pac.maskRef = f.mask;                                            // overwrite simUpdate mask
                std::memcpy(pac.actions, f.actions, ReplayFormat::kActionArraySize); // overwrite player_actions[16]
                assertRecordedViewAngle(f);                                       // keep biped/weapon aim consistent
                setCameraPairForTick(mFrameIndex);                                // camera pair for per-frame interp
                ++mFrameIndex;                                                    // one frame consumed per tick
                if (mFrameIndex >= mData.frames.size()) finishPlayback("finished");
            }
            else if (curRel < f.relTick)
            {
                PLOG_INFO << "[replay-play] WAITING: curRel=" << curRel << " < frame relTick=" << f.relTick;
            }
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); stopPlaybackInternal(); }
    }

    // Win32 open-file dialog. Runs a modal loop on the calling (render) thread, so the game briefly
    // stops presenting while the dialog is open - acceptable for a user-driven file pick and avoids
    // any cross-thread access to mData.
    static std::optional<std::filesystem::path> browseForReplayFile(const std::wstring& initialDir)
    {
        wchar_t fileBuf[MAX_PATH];
        fileBuf[0] = L'\0';
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetForegroundWindow();
        ofn.lpstrFilter = L"HCM Replay (*.h2r)\0*.h2r\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle = L"Load HCM Replay (.h2r)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
            return std::filesystem::path(fileBuf);
        return std::nullopt; // user cancelled
    }

    // GUI/hotkey: Load .h2r File - browse + load into memory (does not start playback).
    void onLoadFileEvent()
    {
        if (mArmed || mPlaying)
        {
            if (auto m = messagesWeak.lock()) m->addMessage("Replay: stop playback before loading another file.");
            return;
        }
        std::wstring initialDir = (std::filesystem::path(mReplayDir) / "replays").wstring();
        auto chosen = browseForReplayFile(initialDir);
        if (!chosen.has_value()) return;
        loadReplay(chosen.value());
    }

public:
    ReplayPlayerImpl(GameState game, IDIContainer& dicon)
        : mGame(game),
        runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>().lock()),
        messagesWeak(dicon.Resolve<IMessagesGUI>()),
        mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
        settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
        mLoadCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayLoadFileEvent, [this]() { onLoadFileEvent(); }),
        mPlayCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayPlayEvent, [this]() { startPlayback(); }),
        mStopCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayStopPlaybackEvent, [this]() { stopPlayback(); })
    {
        gameTickHookWeak = resolveDependentCheat(GameTickEventHook);
        playerActionHookWeak = resolveDependentCheat(PlayerActionUpdateHook);
        try { blockInputOptionalWeak = resolveDependentCheat(BlockPlayerCharacterInput); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: BlockPlayerCharacterInput unavailable: " << ex.what(); }
        try { getPlayerViewAngleOptionalWeak = resolveDependentCheat(GetPlayerViewAngle); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: GetPlayerViewAngle unavailable: " << ex.what(); }
        mReplayDir = dicon.Resolve<DirPathContainer>().lock()->dirPath;

        // camera replay layer: override the engine's per-frame camera set while playing back. Shares the
        // setCameraDataFunction with FreeCamera but only ever attaches while a replay (with camera data) is
        // playing, when FreeCamera's hook is detached - so the two never patch the same bytes simultaneously.
        cameraHookInstance = this;
        try
        {
            getGameCameraDataOptionalWeak = resolveDependentCheat(GetGameCameraData);
            auto ptr = dicon.Resolve<PointerDataStore>().lock();
            auto setCameraDataFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(setCameraDataFunction), game);
            mCameraHook = ModuleMidHook::make(game.toModuleName(), setCameraDataFunction, &cameraHookFunction); // detached until playback
            try { mInterpAlpha = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(interpAlphaPointer), game); }
            catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: interpAlphaPointer unavailable (camera will snap per tick): " << ex.what(); }
        }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: camera replay unavailable (FP view won't follow): " << ex.what(); }
    }

    bool loadReplay(const std::filesystem::path& inFile)
    {
        try
        {
            std::ifstream in(inFile, std::ios::binary);
            if (!in) throw HCMRuntimeException(std::format("Replay load: could not open {}", inFile.string()));

            ReplayData fresh{};
            in.read(reinterpret_cast<char*>(&fresh.header), sizeof(ReplayFileHeader));
            if (in.gcount() != sizeof(ReplayFileHeader)) throw HCMRuntimeException("Replay load: file too small / truncated header");
            if (!fresh.magicValid()) throw HCMRuntimeException("Replay load: invalid file (bad magic)");
            if (fresh.header.version != ReplayFormat::kVersion)
                throw HCMRuntimeException(std::format("Replay load: unsupported version {} (expected {})", fresh.header.version, ReplayFormat::kVersion));

            fresh.frames.resize(fresh.header.frameCount);
            if (fresh.header.frameCount)
                in.read(reinterpret_cast<char*>(fresh.frames.data()),
                        (std::streamsize)(fresh.header.frameCount * sizeof(PlayerActionFrame)));

            // optional per-tick camera stream (1:1 with frames)
            if ((fresh.header.reserved[0] & ReplayFormat::kFlagHasCamera) && fresh.header.frameCount)
            {
                fresh.cameras.resize(fresh.header.frameCount);
                in.read(reinterpret_cast<char*>(fresh.cameras.data()),
                        (std::streamsize)(fresh.header.frameCount * sizeof(CameraSample)));
                if (in.gcount() != (std::streamsize)(fresh.header.frameCount * sizeof(CameraSample)))
                    throw HCMRuntimeException("Replay load: truncated camera stream");
            }

            mData = std::move(fresh);
            mLoaded = true;
            if (auto m = messagesWeak.lock())
                m->addMessage(std::format("Replay loaded: {} frames{}.", mData.header.frameCount,
                    mData.hasCamera() ? " (+camera)" : ""));
            return true;
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); mLoaded = false; return false; }
    }

    void startPlayback()
    {
        if (!mLoaded) { if (auto m = messagesWeak.lock()) m->addMessage("Replay: no replay loaded."); return; }
        try
        {
            lockOrThrow(playerActionHookWeak, playerActionHook);

            mHaveLastTick = false; // re-baseline tick-decrease detection
            mUpdateCallback = playerActionHook->getPlayerActionUpdateEvent()->subscribe([this](PlayerActionUpdateContext& p) { onPlayerActionUpdate(p); });

            mArmed = true;
            if (auto m = messagesWeak.lock())
                m->addMessage("Replay armed - revert the matching checkpoint to begin playback.");
        }
        catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); stopPlaybackInternal(); }
    }

    void stopPlayback()
    {
        bool wasActive = mArmed || mPlaying;
        stopPlaybackInternal();
        if (wasActive) { if (auto m = messagesWeak.lock()) m->addMessage("Replay playback stopped."); }
    }

    bool isLoaded() const { return mLoaded; }
    bool isArmed() const { return mArmed; }
    bool isPlaying() const { return mPlaying; }

    ~ReplayPlayerImpl()
    {
        // tear down the camera hook safely: detach, wait for any in-flight hook call to finish, clear instance
        if (mCameraHook) mCameraHook->setWantsToBeAttached(false);
        if (cameraHookIsRunning) cameraHookIsRunning.wait(true);
        mCameraHook.reset();
        cameraHookInstance = nullptr;
    }
};

ReplayPlayer::ReplayPlayer(GameState game, IDIContainer& dicon)
    : pimpl(std::make_unique<ReplayPlayerImpl>(game, dicon)) {}
ReplayPlayer::~ReplayPlayer() { PLOG_DEBUG << "~" << getName(); }

bool ReplayPlayer::loadReplay(const std::filesystem::path& inFile) { return pimpl->loadReplay(inFile); }
void ReplayPlayer::startPlayback() { pimpl->startPlayback(); }
void ReplayPlayer::stopPlayback() { pimpl->stopPlayback(); }
bool ReplayPlayer::isLoaded() const { return pimpl->isLoaded(); }
bool ReplayPlayer::isArmed() const { return pimpl->isArmed(); }
bool ReplayPlayer::isPlaying() const { return pimpl->isPlaying(); }
