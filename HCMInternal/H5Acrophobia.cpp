#include "pch.h"
#include "H5Acrophobia.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "imgui.h"
#include "SharedMemoryInternal.h"

// See H5Acrophobia.h for the flight model and why input is read through ImGui.

namespace
{
	// The CER constants. Do not "tidy" these into something that looks nicer - the polynomial is the
	// speed limiter and the damping is what makes the model steer instead of just accelerating.
	constexpr float kAcroScale   = 0.0625f;   // t -> s;  s saturates at t = 16 wu/s
	constexpr float kAcroAccel   = 9.0f;
	constexpr float kAcroAccel2  = 18.0f;
	constexpr float kAcroDamping = 0.2f;
	constexpr float kAcroIdle    = 0.2f;   // crouch/hover: scale the whole velocity toward rest each tick
}

class H5Acrophobia::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	// Only non-null while the toggle is on, so the per-frame cost is exactly zero when it is off.
	std::unique_ptr<ScopedCallback<RenderEvent>> mRenderCallback;

	std::chrono::steady_clock::time_point mLastTick{};
	std::chrono::steady_clock::time_point mLastFailureLog{};
	bool mWasFlying = false;

	void onToggleChanged(bool& newValue)
	{
		try
		{
			if (newValue && !mRenderCallback)
			{
				mLastTick = {};
				mWasFlying = false;
				mRenderCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onFrame(); });
			}
			else if (!newValue && mRenderCallback)
			{
				mRenderCallback.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Acrophobia: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onFrame()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				// Loading, or at a menu. Stay attached and wait, but forget the flight clock - otherwise
				// the first frame back would bank the whole load as one dt.
				mWasFlying = false;
				mLastTick = {};
				return;
			}

			// ⚠⚠ READ INPUT THROUGH IMGUI, NOT THE FORWARDED KEY ARRAY DIRECTLY.
			// The first version read forwardedKeyStates()[VK_SPACE], which is keyboard-only - so Acrophobia
			// simply did not respond to a controller. ImGui already has BOTH: HCM feeds it the forwarded
			// keyboard state AND polls XInput into the ImGuiKey_Gamepad* keys every frame. This callback
			// runs inside the ImGui frame (between NewFrame and Render), so IsKeyDown is valid here.
			//
			// Jump = A / Space. Crouch = LEFT THUMBSTICK CLICK / Left Ctrl.
			// ⚠ L3, confirmed by the user - not B and not R3, which were my guesses at Halo 5's layout.
			// Binding a wrong button here is worse than binding none: B is melee, so hovering would fire
			// every time the player meleed.
			const bool jump = ImGui::IsKeyDown(ImGuiKey_Space)
				|| ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
			const bool crouch = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
				|| ImGui::IsKeyDown(ImGuiKey_GamepadL3);
			const bool flying = jump || crouch;

			// Clock the delta off real time, and only while flying, so a pause or a long frame does not
			// bank a huge dt and fire the player across the map on the next tick.
			const auto now = std::chrono::steady_clock::now();
			float dt = 1.0f / 60.0f;
			if (mWasFlying && mLastTick.time_since_epoch().count() != 0)
				dt = std::clamp(std::chrono::duration<float>(now - mLastTick).count(), 1.0f / 1000.0f, 1.0f / 15.0f);
			mLastTick = now;
			mWasFlying = flying;

			if (!flying) return;

			lockOrThrow(playerStateWeak, playerState);

			auto aim = playerState->getPlayerAim();
			if (aim.LengthSquared() < 0.0001f) return;
			aim.Normalize();

			playerState->modifyPlayerVelocity([&](SimpleMath::Vector3 v) -> SimpleMath::Vector3
				{
					if (jump)
					{
						const float t = std::max(0.0f, aim.Dot(v));
						const float s = std::clamp(t * kAcroScale, 0.0f, 1.0f);
						const float a = kAcroAccel + (s * kAcroAccel - s * s * kAcroAccel2);   // 9(1-s)(1+2s)

						// steer onto the aim axis, then thrust along it
						v -= (v - aim * t) * kAcroDamping;
						v += aim * (a * dt);
						return v;
					}

					// ⚠ THE CROUCH / HOVER BRANCH - this was missing, which is why "crouch to float" did
					// nothing. It is not a variation on the thrust: the model simply scales the whole
					// velocity toward rest each tick, which kills the fall as well as the drift and leaves
					// you hanging in place. Jump takes priority when both are held, exactly as the
					// reference does.
					return v * kAcroIdle;
				});
		}
		catch (HCMRuntimeException& ex)
		{
			// ⚠⚠ SELF-HEAL. DO NOT TURN THE TOGGLE OFF HERE.
			// The previous version did, and it made Acrophobia switch itself off constantly in normal play:
			// H5GetPlayerState throws as its NORMAL result at a menu, during a load, while the player is
			// DEAD, and across a BSP / zone set switch (see the header of H5GetPlayerState.h). Those are
			// transient by definition, so treating the first one as fatal meant dying - or crossing a BSP
			// boundary - permanently disabled the feature until the user re-toggled it by hand.
			//
			// Behave like Display 2D Game Info instead: skip the frame, keep the callback attached, and pick
			// the player back up by ourselves as soon as the state resolves again.
			onTransientFailure(ex.what());
		}
		catch (...)
		{
			// A per-frame callback must never let anything escape into the render loop.
			onTransientFailure("unknown error");
		}
	}

	// Recover in place from a bad frame. Kept out of onFrame so both catch blocks share it exactly.
	void onTransientFailure(std::string_view what) noexcept
	{
		try
		{
			// ⚠ The cached character-controller address does NOT survive a BSP or zone set switch - the
			// physics world is rebuilt and the old array is freed. Drop it so the next good frame resolves
			// cleanly rather than revalidating against a stale pointer.
			if (auto playerState = playerStateWeak.lock())
				playerState->invalidateProxyCache();

			// Forget the flight clock too. Without this the first frame after a long outage would see the
			// whole gap as one dt and fling the player across the map.
			mWasFlying = false;
			mLastTick = {};

			// ⚠ Rate-limited: at 60fps a persistent fault would otherwise write thousands of identical
			// lines. Log only - deliberately NOT the on-screen message queue, because dying is a routine
			// way to land here and a popup every death is exactly the behaviour being fixed.
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Acrophobia skipped a frame (will retry): " << what;
			}
		}
		catch (...) {}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5AcrophobiaToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5Acrophobia only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mRenderCallback.reset();
		mToggleCallback.removeCallback();
	}
};

H5Acrophobia::H5Acrophobia(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5Acrophobia::~H5Acrophobia() { PLOG_VERBOSE << "~" << getName(); }
