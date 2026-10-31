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
#include "GetCurrentLevelCode.h"
#include "IGetMCCVersion.h"
#include "ReplayCheckpoint.h"
#include "Halo3TheaterInterp.h"
#include "ReplayGameTls.h"
#include <fstream>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

namespace
{
    // *(float*)(*(tlsBlock + slot) + 4), or false.
    bool readTlsAlpha(uintptr_t tlsBlock, uint32_t slot, float& out)
    {
        __try
        {
            const uintptr_t interp = *reinterpret_cast<const uintptr_t*>(tlsBlock + slot);
            if (!interp) return false;
            out = *reinterpret_cast<const float*>(interp + 4);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // The first-person view fix for playback (what the per-frame control update - blocked by BlockPlayerCharacterInput -
    // normally keeps current), per game. All offsets are into the SIM thread's TLS block (TLS[TlsIndex]):
    //  - alphaSlot: the render interpolation fraction *(f32*)(*(tls + alphaSlot) + 4), stored by the main loop after the ticks
    //  - observerSlot: the observer array *(tls + observerSlot) (stride 976; fwd +0x144, up +0x150 - the FP legs, the leg
    //    anchor and the cull read these, not HCM's camera copy); the camera hook's rsi must equal it
    //  - control*: player-control globals *(tls + controlSlot): per user u the per-frame facing (yaw, pitch) at
    //    controlStride*u + facing (integrated from the mouse every frame) and its render copy at renderStride*u + renderFacing
    //    (copied every frame; read by the eye/aim getter and in-tick by the sim)
    // Halo 3 (halo3.dll 1.3528): main loop stores at [r15+0x88]+4 @0x1800B2AE2; hook 0x17E70C after sub_18017E608's publish
    //   loop (sub_18017DF44 rewrites +0x144/+0x150), observers *(tls+1400); control *(tls+192), +272u+212, copy +128u+1300
    //   (sub_1800F9634; sub_1800F786C copies the action there, film-only; sub_1800F7694 reads it, in-tick at 0x1800EDB4D).
    // ODST (halo3odst.dll 1.3528): [r15+0x78]+4 @0x1800C02AC; hook 0x1AD2B0 after the same loop (sub_1801ACAF0),
    //   observers *(tls+0x5A8); control *(tls+0xD8), +0x2C0u+212, copy +560u+3028 (sub_180115750; sub_180113540 action copy).
    // Reach (haloreach.dll 1.3528): main loop stores sub_18005C430() (time globals *(tls+160)+0x14, clamped) at [r12+0x70]+4
    //   @0x1800344B3; no observer write (observerSlot 0): HCM's Reach camera data IS the observer record (*(tls+0x688)+340);
    //   control *(tls+0xD8), +0x180u+148 (block +136, copied by sub_1800663BC), copy +176u+1684 (table +1672; the action copy
    //   sub_180062FCC writes yaw/pitch at block+12). ⚠ Reach's camera hook (0x344E5) runs AFTER the frame's camera/director
    //   updates (sub_1800D9EE8 / sub_1800C4458 / sub_1800C7F98), so a facing written there reached the legs a frame late -
    //   they jittered on turns. Reach writes the facing at its own hook instead, 0x344B8: right after the main loop stores
    //   the frame's alpha (0x1800344B3), before those updates - where the blocked per-frame control update would have.
    //   And the engine builds the camera from that facing (engineCamera): HCM's recorded-camera override is skipped, so
    //   the view and the legs come from one source.
    struct ViewFix
    {
        uint32_t alphaSlot, observerSlot, controlSlot, controlStride, facing, renderStride, renderFacing;
        uint32_t facingHookRva = 0;                 // 0: the facing is written at the camera hook
        uint8_t facingHookBytes[8] = {}; uint8_t facingHookLen = 0;   // the instruction(s) expected at that site
        bool engineCamera = false;                  // skip the recorded-camera override: the engine builds it from the facing
    };
    std::optional<ViewFix> viewFixFor(GameState::Value g)
    {
        switch (g)
        {
        case GameState::Value::Halo3:     return ViewFix{ 0x88, 1400, 192, 272, 212, 128, 1300 };
        case GameState::Value::Halo3ODST: return ViewFix{ 0x78, 0x5A8, 0xD8, 0x2C0, 212, 560, 3028 };
        case GameState::Value::HaloReach: return ViewFix{ 0x70, 0, 0xD8, 0x180, 148, 176, 1684, 0x344B8, { 0xB8, 0xA0, 0x00, 0x00, 0x00 }, 5, true };   // mov eax, 0A0h
        default: return std::nullopt;
        }
    }

    bool writeControlFacing(uintptr_t tls, const ViewFix& v, uint32_t user, float yaw, float pitch, bool alsoRenderCopy)
    {
        __try
        {
            const uintptr_t control = *reinterpret_cast<const uintptr_t*>(tls + v.controlSlot);
            if (!control) return false;
            float* f = reinterpret_cast<float*>(control + (uintptr_t)v.controlStride * user + v.facing);
            f[0] = yaw; f[1] = pitch;
            if (alsoRenderCopy)
            {
                float* r = reinterpret_cast<float*>(control + (uintptr_t)v.renderStride * user + v.renderFacing);
                r[0] = yaw; r[1] = pitch;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Write observer 0's forward (+0x144) and up (+0x150) - only when the hook's register really is the observer array the
    // game uses this frame (*observerSlot), so a different build / call site can never get random memory written.

    bool writeObserverOrientation(uintptr_t observerSlot, uintptr_t observers, const float fwd[3], const float up[3])
    {
        __try
        {
            if (!observers || *reinterpret_cast<const uintptr_t*>(observerSlot) != observers) return false;
            float* o = reinterpret_cast<float*>(observers + 0x144);
            o[0] = fwd[0]; o[1] = fwd[1]; o[2] = fwd[2];
            o[3] = up[0];  o[4] = up[1];  o[5] = up[2];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

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
    float mYawA = 0, mYawB = 0, mPitchA = 0, mPitchB = 0;   // player 0's recorded facing, this tick and the next
    bool mFacingValid = false;
    bool mCamValid = false;

    // static plumbing for the camera midhook. One player exists per game (each with its own hook on its own
    // module), so the hook target is per game - a single shared pointer was owned by the last-built game and the
    // others' camera override never ran (Halo 3: the view did not turn on playback).
    static inline std::array<std::atomic<ReplayPlayerImpl*>, 16> cameraHookInstances{};
    static inline std::atomic_bool cameraHookIsRunning = false;

    // the early facing hook (ViewFix::facingHookRva): writes the interpolated recorded facing before the frame uses it
    std::unique_ptr<ModuleMidHook> mFacingHook;
    static void facingHookFunction(SafetyHookContext&)
    {
        ScopedAtomicBool lock(cameraHookIsRunning);
        for (auto& slot : cameraHookInstances)
            if (ReplayPlayerImpl* p = slot.load()) p->applyEarlyFacing();
    }
    void applyEarlyFacing()
    {
        if (!mPlaying || !mViewFix || !mViewFix->facingHookRva) return;
        float a = 0.f; bool ok = false;
        writeInterpolatedFacing(currentAlpha(a, ok));
    }
    bool facingHookSiteMatches() const
    {
        if (!mViewFix || !mViewFix->facingHookRva || !mViewFix->facingHookLen) return false;
        const auto base = (const uint8_t*)GetModuleHandleW(mGame.toModuleName().c_str());
        if (!base) return false;
        const uint8_t* at = base + mViewFix->facingHookRva;
        return !IsBadReadPtr(at, mViewFix->facingHookLen) && std::memcmp(at, mViewFix->facingHookBytes, mViewFix->facingHookLen) == 0;
    }

    static void cameraHookFunction(SafetyHookContext& ctx)
    {
        ScopedAtomicBool lock(cameraHookIsRunning);
        for (auto& slot : cameraHookInstances)
            if (ReplayPlayerImpl* p = slot.load())
            {
                const auto t0 = std::chrono::steady_clock::now();
                p->applyPendingCamera(ctx);   // no-op unless that game is playing
                p->mDiag.cameraMicros += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            }
    }

    size_t cameraSlot() const { return (size_t)mGame.operator GameState::Value() % cameraHookInstances.size(); }

    // One player exists per game and they all hear the same GUI events: only the game being played answers.
    bool isMyGame()
    {
        auto mccStateHook = mccStateHookWeak.lock();
        return mccStateHook && mccStateHook->isGameCurrentlyPlaying(mGame);
    }

    static void normalize3(float v[3])
    {
        float m = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (m > 1e-6f) { v[0]/=m; v[1]/=m; v[2]/=m; }
    }

    // runs every frame (engine's camera-set) while attached: overwrite the live camera ORIENTATION with the
    // recorded one, interpolated between this tick's sample (A) and the next (B) by real sub-tick time.
    // the engine's interpolation fraction now (0 when unavailable: the camera snaps per tick)
    float currentAlpha(float& raw, bool& ok)
    {
        ok = false; raw = 0.f;
        if (mInterpAlpha) ok = mInterpAlpha->readData(&raw);
        else if (const uintptr_t tls = mSimTls.load(); tls && mTlsAlphaSlot) ok = readTlsAlpha(tls, mTlsAlphaSlot, raw);
        return ok ? (raw < 0.f ? 0.f : (raw > 1.f ? 1.f : raw)) : 0.f;
    }

    // the recorded facing between this tick and the next at `ft`, into the per-frame control facing and its render copy
    void writeInterpolatedFacing(float ft)
    {
        const uintptr_t tls = mSimTls.load();
        if (!tls || !mFacingValid || !mViewFix) return;
        // shortest way round for yaw ([0, 2pi) wraps), plain lerp for pitch - the engine's per-frame facing
        constexpr float kTwoPi = 6.28318530718f;
        float dy = mYawB - mYawA;
        if (dy > kTwoPi * 0.5f) dy -= kTwoPi; else if (dy < -kTwoPi * 0.5f) dy += kTwoPi;
        float yaw = mYawA + dy * ft;
        if (yaw < 0.f) yaw += kTwoPi; else if (yaw >= kTwoPi) yaw -= kTwoPi;
        const float pitch = mPitchA + (mPitchB - mPitchA) * ft;
        if (writeControlFacing(tls, *mViewFix, 0, yaw, pitch, true)) ++mDiag.facingWrites;
    }

    void applyPendingCamera(SafetyHookContext& ctx)
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
            float a = 0.f;
            bool alphaOk = false;
            const float ft = currentAlpha(a, alphaOk);

            float fwd[3], up[3];
            for (int i = 0; i < 3; ++i)
            {
                fwd[i] = mCamA.fwd[i] + (mCamB.fwd[i] - mCamA.fwd[i]) * ft;
                up[i]  = mCamA.up[i]  + (mCamB.up[i]  - mCamA.up[i])  * ft;
            }
            float fov = mCamA.fov + (mCamB.fov - mCamA.fov) * ft;
            normalize3(fwd);
            normalize3(up);

            // ORIENTATION + FOV only. Camera POSITION is left to the engine (pinned to the re-simulated biped
            // eye) - forcing recorded absolute position detaches the world-space FP legs from the view. Not at all where
            // the engine builds the camera from the facing we write early (Reach).
            if (!(mViewFix && mViewFix->engineCamera))
            {
                GameCameraData cam = getCam->getGameCameraData();
                if (cam.lookDirForward) *cam.lookDirForward = SimpleMath::Vector3(fwd[0], fwd[1], fwd[2]);
                if (cam.lookDirUp)      *cam.lookDirUp = SimpleMath::Vector3(up[0], up[1], up[2]);
                if (cam.FOV)            *cam.FOV = fov;
            }

            // The FP legs, the cull and the leg anchor read the OBSERVER's own forward/up, not HCM's camera copy above - so
            // the view turned but the legs stayed with the engine's frozen aim (ViewFix above: the hook sits right after the
            // publish loop that rewrote them; rsi = the observer array, SP player = observer 0).
            if (mViewFix)
            {
                const uintptr_t tls = mSimTls.load();
                if (tls && mViewFix->observerSlot && writeObserverOrientation(tls + mViewFix->observerSlot, ctx.rsi, fwd, up)) ++mDiag.observerWrites;
                if (!mViewFix->facingHookRva || !mFacingHook) writeInterpolatedFacing(ft);   // else the early hook wrote it
            }

            // once a second: is the interpolation alpha alive (sweeping 0..1) and did the observer write pass its guard
            ++mDiag.frames;
            if (alphaOk) { ++mDiag.alphaReads; mDiag.alphaMin = (std::min)(mDiag.alphaMin, a); mDiag.alphaMax = (std::max)(mDiag.alphaMax, a); }
            const auto now = std::chrono::steady_clock::now();
            if (now - mDiag.since >= std::chrono::seconds(1))
            {
                PLOG_INFO << "[replay-cam] frames=" << mDiag.frames << " ticks=" << mDiag.ticks << " alphaReads=" << mDiag.alphaReads
                          << " alpha[min,max]=[" << mDiag.alphaMin << "," << mDiag.alphaMax << "] observerWrites=" << mDiag.observerWrites
                          << " facingWrites=" << mDiag.facingWrites << " tickWrites=" << mDiag.tickWrites
                          << " cameraHook=" << mDiag.cameraMicros << "us tickHook=" << mDiag.tickMicros << "us (max " << mDiag.tickMaxMicros
                          << "us; state " << mDiag.secMicros[0] << " inject " << mDiag.secMicros[1] << " viewAngle " << mDiag.secMicros[2]
                          << " facing " << mDiag.secMicros[3] << " camPair " << mDiag.secMicros[4] << ") simTls=" << (void*)mSimTls.load();
                mDiag = CamDiag{};
                mDiag.since = now;
            }
        }
        catch (HCMRuntimeException) { /* swallow on the render thread; keep replaying */ }
    }

    // Revert is detected by watching for the game tick to DECREASE on the per-tick hook (matches the
    // original xlive c_replay_system) - HCM's RevertEventHook has no Halo2 pointer data.
    std::unique_ptr<ScopedCallback<PlayerActionUpdateEvent>> mUpdateCallback;
    std::shared_ptr<SharedRequestToken> mBlockInputToken; // held while playing
    // a replay that carries its start checkpoint: Play injects it and reverts (one step)
    std::unique_ptr<ReplayCheckpoint> mCheckpoint;
    std::optional<std::weak_ptr<GetCurrentLevelCode>> levelCodeOptionalWeak;
    std::weak_ptr<IGetMCCVersion> getMCCVerWeak;
    // Halo 3: the theater fix's crouch smoothing (ungated, so it works in gameplay) held on while a replay is armed/playing
    std::optional<std::weak_ptr<Halo3TheaterInterp>> theaterInterpOptionalWeak;
    std::shared_ptr<void> mCrouchHold;

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
    // Games whose alpha is thread-local: the render alpha is *(f32*)(*(simTls + slot) + 4) (Halo 3: halo3.dll main loop
    // stores sub_1800F33D0()'s fraction at [r15+0x88]+4 @0x1800B2AE2 after the ticks, before the camera update).
    // The sim thread's TLS block is captured in the per-tick hook (it runs on that thread).
    uint32_t mTlsAlphaSlot = 0;
    std::optional<ViewFix> mViewFix;                         // this game's first-person view fix, if it has one
    std::atomic<uintptr_t> mSimTls{ 0 };
    struct CamDiag
    {
        uint32_t frames = 0, alphaReads = 0, observerWrites = 0, facingWrites = 0, tickWrites = 0, ticks = 0;
        uint32_t cameraMicros = 0, tickMicros = 0, tickMaxMicros = 0;
        uint32_t secMicros[5] = {};   // per-tick sections: 0 state+tick read, 1 inject, 2 view angle, 3 facing, 4 camera pair
        float alphaMin = 1e9f, alphaMax = -1e9f;
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    } mDiag;

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
        mFacingValid = false;
        if (mCameraHook) mCameraHook->setWantsToBeAttached(false); // stop overriding the camera
        if (mFacingHook) mFacingHook->setWantsToBeAttached(false);
        mBlockInputToken.reset();   // re-enable real input
        mCrouchHold.reset();        // the theater fix back to the user's own setting
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
        if (mFacingHook)
        {
            if (facingHookSiteMatches()) mFacingHook->setWantsToBeAttached(true);
            else
            {
                PLOG_ERROR << "ReplayPlayer: the early facing hook site's bytes differ (another build?) - the facing is written at the camera hook instead";
                mFacingHook.reset();
            }
        }

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
    void assertRecordedViewAngle(const ReplayFrame& f, const ActionLayout& L)
    {
        if (!(f.mask & 1u) || f.slots.size() < L.stride) return;     // player 0 = the first stored block
        if (!getPlayerViewAngleOptionalWeak.has_value()) return;
        auto getPlayerViewAngle = getPlayerViewAngleOptionalWeak.value().lock();
        if (!getPlayerViewAngle) return;

        float yaw, pitch;
        std::memcpy(&yaw, f.slots.data() + L.yawOffset, sizeof(float));
        std::memcpy(&pitch, f.slots.data() + L.pitchOffset, sizeof(float));
        // the fast write: the checked one VirtualQueries the whole region every call (~6.5 ms on Reach - it cut playback to
        // ~25 fps). Non-fatal if it fails: keep replaying inputs.
        getPlayerViewAngle->setPlayerViewAngleFast(SimpleMath::Vector2(yaw, pitch));
    }

    // Capture this tick's camera pair (A=this tick, B=next) for the per-frame camera hook to interpolate
    // between, using the engine's interp alpha as the fraction.
    // player 0's recorded (yaw, pitch) of frame idx, or false (player 0 not in that frame)
    bool recordedFacing(size_t idx, const ActionLayout& L, float& yaw, float& pitch) const
    {
        if (idx >= mData.frames.size()) return false;
        const ReplayFrame& f = mData.frames[idx];
        if (!(f.mask & 1u) || f.slots.size() < L.stride) return false;
        std::memcpy(&yaw, f.slots.data() + L.yawOffset, sizeof(float));
        std::memcpy(&pitch, f.slots.data() + L.pitchOffset, sizeof(float));
        return true;
    }

    void setFacingPairForTick(size_t idx, const ActionLayout& L)
    {
        mFacingValid = recordedFacing(idx, L, mYawA, mPitchA);
        if (!mFacingValid) return;
        if (!recordedFacing(idx + 1, L, mYawB, mPitchB)) { mYawB = mYawA; mPitchB = mPitchA; }
    }

    void setCameraPairForTick(size_t idx)
    {
        if (!mData.hasCamera() || idx >= mData.cameras.size()) { mCamValid = false; return; }
        mCamA = mData.cameras[idx];
        mCamB = (idx + 1 < mData.cameras.size()) ? mData.cameras[idx + 1] : mCamA;
        mCamValid = true;
    }

    void onPlayerActionUpdate(PlayerActionUpdateContext& pac)
    {
        const auto tickStart = std::chrono::steady_clock::now();
        struct TickTimer { ReplayPlayerImpl* p; std::chrono::steady_clock::time_point t0;
            ~TickTimer() { if (p->mPlaying) { ++p->mDiag.ticks; const uint32_t us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                p->mDiag.tickMicros += us; p->mDiag.tickMaxMicros = (std::max)(p->mDiag.tickMaxMicros, us); } } }
            tickTimer{ this, tickStart };
        auto lap = std::chrono::steady_clock::now();
        auto section = [&](int i) { const auto t = std::chrono::steady_clock::now();
            mDiag.secMicros[i] += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(t - lap).count(); lap = t; };
        if (!mArmed) return;
        try
        {
            lockOrThrow(mccStateHookWeak, mccStateHook);
            if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
            if (mTlsAlphaSlot) mSimTls = ReplayGameTls::moduleTlsBlockOfThisThread(mGame.toModuleName().c_str());

            const uint32_t cur = pac.hasTick() ? pac.tick() : currentTick();
            section(0);

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

            const ReplayFrame& f = mData.frames[mFrameIndex];
            if (f.relTick == curRel)
            {
                if (mFrameIndex < 8)
                    PLOG_INFO << "[replay-play] inject idx=" << mFrameIndex << " absTick=" << cur
                              << " curRel=" << curRel << " relTick=" << f.relTick;
                // overwrite the mask and the recorded players' blocks (every tick, a 0 mask too - the builder's own
                // state is not restored by the revert, so nothing of the live input may survive)
                pac.setMask(f.mask);
                const uint32_t stride = pac.layout->stride;
                size_t k = 0;
                for (uint32_t s = 0; s < pac.layout->slots; ++s)
                    if (f.mask & (1ull << s))
                    {
                        if ((k + 1) * stride <= f.slots.size()) std::memcpy(pac.actions + (size_t)s * stride, f.slots.data() + k * stride, stride);
                        ++k;
                    }
                section(1);
                assertRecordedViewAngle(f, *pac.layout);                          // keep biped/weapon aim consistent
                section(2);
                if (mViewFix)
                {
                    setFacingPairForTick(mFrameIndex, *pac.layout);
                    if (const uintptr_t tls = mSimTls.load(); tls && mFacingValid)
                        if (writeControlFacing(tls, *mViewFix, 0, mYawA, mPitchA, true)) ++mDiag.tickWrites;   // the render copy too: the tick reads it
                                                                                       // (H3 0x1800EDB4D) and in gameplay nothing
                                                                                       // else sets it (the action copy is film-only)
                }
                section(3);
                setCameraPairForTick(mFrameIndex);                                // camera pair for per-frame interp
                section(4);
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
        ofn.lpstrFilter = L"Halo Replay (*.HaloReplay)\0*.HaloReplay;*.h2r\0All Files (*.*)\0*.*\0";   // .h2r = older builds
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.lpstrTitle = L"Load Halo Replay (.HaloReplay)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
            return std::filesystem::path(fileBuf);
        return std::nullopt; // user cancelled
    }

    // GUI/hotkey: Load .HaloReplay File - browse + load into memory (does not start playback).
    void onLoadFileEvent()
    {
        if (!isMyGame()) return;   // one file dialog, from the game being played
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
        mPlayCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayPlayEvent, [this]() { if (isMyGame()) startPlayback(); }),
        mStopCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->replayStopPlaybackEvent, [this]() { if (mArmed || mPlaying || isMyGame()) stopPlayback(); })
    {
        gameTickHookWeak = resolveDependentCheat(GameTickEventHook);
        playerActionHookWeak = resolveDependentCheat(PlayerActionUpdateHook);
        try { blockInputOptionalWeak = resolveDependentCheat(BlockPlayerCharacterInput); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: BlockPlayerCharacterInput unavailable: " << ex.what(); }
        try { levelCodeOptionalWeak = resolveDependentCheat(GetCurrentLevelCode); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: GetCurrentLevelCode unavailable: " << ex.what(); }
        try { mCheckpoint = std::make_unique<ReplayCheckpoint>(game, dicon); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: no checkpoint data for this game (replays with one need it loaded by hand): " << ex.what(); }
        getMCCVerWeak = dicon.Resolve<IGetMCCVersion>();
        if (game.operator GameState::Value() == GameState::Value::Halo3)
        {
            try { theaterInterpOptionalWeak = resolveDependentCheat(Halo3TheaterInterp); }
            catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: Halo3TheaterInterp unavailable (crouch not smoothed in replays): " << ex.what(); }
        }
        try { getPlayerViewAngleOptionalWeak = resolveDependentCheat(GetPlayerViewAngle); }
        catch (HCMInitException ex) { PLOG_ERROR << "ReplayPlayer: GetPlayerViewAngle unavailable: " << ex.what(); }
        mReplayDir = dicon.Resolve<DirPathContainer>().lock()->dirPath;

        // camera replay layer: override the engine's per-frame camera set while playing back. Shares the
        // setCameraDataFunction with FreeCamera but only ever attaches while a replay (with camera data) is
        // playing, when FreeCamera's hook is detached - so the two never patch the same bytes simultaneously.
        cameraHookInstances[cameraSlot()] = this;
        try
        {
            getGameCameraDataOptionalWeak = resolveDependentCheat(GetGameCameraData);
            auto ptr = dicon.Resolve<PointerDataStore>().lock();
            auto setCameraDataFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(setCameraDataFunction), game);
            mCameraHook = ModuleMidHook::make(game.toModuleName(), setCameraDataFunction, &cameraHookFunction); // detached until playback
            try { mInterpAlpha = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(interpAlphaPointer), game); }
            catch (HCMInitException ex)
            {
                mViewFix = viewFixFor(game.operator GameState::Value());
                if (mViewFix) mTlsAlphaSlot = mViewFix->alphaSlot;                  // thread-local alpha
                else PLOG_ERROR << "ReplayPlayer: interpAlphaPointer unavailable (camera will snap per tick): " << ex.what();
            }
            if (mViewFix && mViewFix->facingHookRva)
            {
                auto site = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(game.toModuleName(), std::vector<int64_t>{ (int64_t)mViewFix->facingHookRva });
                mFacingHook = ModuleMidHook::make(game.toModuleName(), site, &facingHookFunction);   // detached until playback
            }
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
            std::string err;
            if (!replayRead(in, fresh, err)) throw HCMRuntimeException("Replay load: " + err);
            // a replay plays only in the game it was recorded in (the action blocks are that game's)
            const GameState::Value here = mGame.operator GameState::Value();
            if (fresh.game() != here)
                throw HCMRuntimeException(std::format("Replay load: this replay was recorded in {} - load it there", GameState(fresh.game()).toString()));
            if (fresh.stride() != replayLayoutFor(here).stride)
                throw HCMRuntimeException(std::format("Replay load: action size {} does not match this game's {}", fresh.stride(), replayLayoutFor(here).stride));

            mData = std::move(fresh);
            mLoaded = true;
            if (auto m = messagesWeak.lock())
                m->addMessage(std::format("Replay loaded: {} frames{}{}.", mData.header.frameCount,
                    mData.hasCamera() ? " (+camera)" : "", mData.checkpoint.empty() ? "" : " (+checkpoint)"));
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

            // a replay with its start checkpoint: check it fits BEFORE arming (wrong level / build = refuse, not crash)
            const bool carriesCheckpoint = !mData.checkpoint.empty();
            if (carriesCheckpoint)
            {
                if (!mCheckpoint) throw HCMRuntimeException("Replay: this game has no checkpoint support - load the replay's checkpoint by hand");
                if (!mCheckpoint->lengthMatches(mData.checkpoint))
                    throw HCMRuntimeException("Replay: its checkpoint is from another game build (wrong size)");
                if (auto getMCCVer = getMCCVerWeak.lock())
                {
                    const std::string recorded = ReplayCheckpoint::versionOf(mData.checkpoint);
                    const std::string here(getMCCVer->getGameDataVersionAsString(mGame));
                    if (!recorded.empty() && recorded != here)
                        throw HCMRuntimeException(std::format("Replay: recorded on game build {}, this is {} - it would not play the same", recorded, here));
                }
                const std::string recordedLevel(mData.header.levelCode, strnlen(mData.header.levelCode, sizeof(mData.header.levelCode)));
                if (!recordedLevel.empty() && levelCodeOptionalWeak.has_value())
                    if (auto levelCode = levelCodeOptionalWeak.value().lock())
                    {
                        const std::string current = levelCode->getCurrentLevelCode();
                        if (current.substr(0, 3) != recordedLevel.substr(0, 3))
                            throw HCMRuntimeException(std::format("Replay: recorded on {} - load that level first (this is {})", recordedLevel, current));
                    }
            }

            // baseline the tick-decrease detection NOW, so a revert straight after Play is still seen as one (the first tick
            // the hook sees may already be the reverted one: no earlier tick to compare against = never starts). A game whose
            // update carries its own tick (ODST) baselines from the first update instead - the revert is the user's, later.
            if (replayLayoutFor(mGame.operator GameState::Value()).tickOffset == 0)
            {
                mLastTick = currentTick();
                mHaveLastTick = true;
            }
            else
                mHaveLastTick = false;
            mUpdateCallback = playerActionHook->getPlayerActionUpdateEvent()->subscribe([this](PlayerActionUpdateContext& p) { onPlayerActionUpdate(p); });
            mArmed = true;

            if (theaterInterpOptionalWeak.has_value())
                if (auto theaterInterp = theaterInterpOptionalWeak.value().lock())
                    try { mCrouchHold = theaterInterp->holdCrouchInterpForReplay(); }
                    catch (HCMRuntimeException ex) { PLOG_ERROR << "ReplayPlayer: crouch smoothing not armed: " << ex.what(); }

            if (carriesCheckpoint)
            {
                // the replay's own start goes into the game's checkpoint (as an inject); the user's revert starts playback
                mCheckpoint->inject(mData.checkpoint);
                if (auto m = messagesWeak.lock()) m->addMessage("Replay armed with its checkpoint - revert to begin playback.");
            }
            else if (auto m = messagesWeak.lock())
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
        cameraHookInstances[cameraSlot()] = nullptr;      // first: a hook call after this can't reach us
        if (mCameraHook) mCameraHook->setWantsToBeAttached(false);
        if (mFacingHook) mFacingHook->setWantsToBeAttached(false);
        if (cameraHookIsRunning) cameraHookIsRunning.wait(true);
        mCameraHook.reset();
        mFacingHook.reset();
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
