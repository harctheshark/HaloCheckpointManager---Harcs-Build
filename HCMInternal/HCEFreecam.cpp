#include "pch.h"
#include "HCEFreecam.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEForceTeleport.h"

// Ported from HCM_Evolved player_camera.py::toggle_freecam and teleport_player_to_freecam.
//
// One reference-tool bug is deliberately NOT copied: toggle_freecam sets its checkbox before the write and never
// reverts it on failure, and its read_pointer omits suppress_errors, so a bad read tears the whole connection
// down. Here a failed write surfaces as an on-screen HCMRuntimeException and leaves the setting alone; the
// re-assert pass then keeps trying while the toggle is on.

class HCEFreecam::HCEFreecamImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;
	std::weak_ptr<HCEForceTeleport> forceTeleportWeak;

	std::atomic<bool> mReady{ false };
	std::atomic<bool> mWantFreecam{ false };
	std::chrono::steady_clock::time_point mLastMaintain{};

	// The engine's own freecam toggle (sim rva 0x1F49E0) writes THREE adjacent bytes, not one:
	//
	//     cmp  byte ptr [rcx+0x9C8], r8b   ; r8b = 0
	//     sete r8b                         ; r8b = !current
	//     mov  byte ptr [rcx+0x9C8], r8b   ; master
	//     mov  byte ptr [rcx+0x9C9], 0     ; always cleared
	//     mov  byte ptr [rcx+0x9CA], r8b   ; follows the master
	//
	// +0x9C9 and +0x9CA are independently key-bindable sub-modes (their own toggle handlers live at sim
	// 0x275E4D and 0x275EE8), and a second "is freecam active" predicate at sim 0x2764C0 answers on
	// (+0x9C8 && +0x9C9) rather than +0x9C8 alone. Writing only the master - which is what this did - turns
	// freecam on fine but leaves +0x9CA latched on when it goes off, so the view never returns to the player.
	//
	// Nothing else happens on a real toggle: the handler tail-jumps to 0x1FE0E0, which is just the HaloScript
	// expression-result publisher. So reproducing these three bytes reproduces the transition exactly. We write
	// the bytes rather than calling the handler because it reads gs:[0x58] and would have to run on the
	// simulation thread, and because it is a toggle rather than a setter.
	void applyFreecam(bool enabled) // throws HCMRuntimeException
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t address = playerState->getFreecamToggleAddress(); // +0x9C8; the trio is contiguous
		const uint8_t state = enabled ? 1 : 0;
		const uint8_t values[3]{ state, 0, state };
		if (!HCEGetPlayerState::tryWriteRaw(address, values, sizeof(values)))
		{
			playerState->invalidateCache();
			throw HCMRuntimeException("Could not write the Halo Campaign Evolved camera state");
		}
	}

	void onToggleChange(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			mWantFreecam.store(newValue, std::memory_order_release);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			applyFreecam(newValue);
			messagesGUI->addMessage(newValue ? "Freecam enabled." : "Freecam disabled.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onTeleportToCamera()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(forceTeleportWeak, forceTeleport);

			// Camera position is 3 floats at cameraEntry + 0x20. The camera does not have to be ON for this to
			// read a position, but the value is only meaningful when it is.
			const uintptr_t cameraEntry = playerState->getActiveCameraEntry();
			float coords[3]{};
			if (!HCEGetPlayerState::tryReadRaw(cameraEntry + kCameraPositionOffset, coords, sizeof(coords)))
				throw HCMRuntimeException("Could not read the camera position");

			const SimpleMath::Vector3 target(coords[0], coords[1], coords[2]);
			forceTeleport->teleportPlayerTo(target); // identical 8-write sequence as Force Teleport

			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("Teleported to camera at {:.3f}, {:.3f}, {:.3f}", target.x, target.y, target.z));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Freecam teleport failed: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Throttled re-assert, silent by design - see the same pattern in HCEFreezeAI.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantFreecam.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		if (mLastMaintain.time_since_epoch().count() != 0 && (now - mLastMaintain) < std::chrono::milliseconds(250)) return;
		mLastMaintain = now;

		try
		{
			auto mccStateHook = mccStateHookWeak.lock();
			if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			auto playerState = playerStateWeak.lock();
			if (!playerState) return;

			const uintptr_t address = playerState->getFreecamToggleAddress();
			uint8_t current = 0;
			if (!HCEGetPlayerState::tryReadRaw(address, &current, sizeof(current))) return;
			if (current == 1) return;

			// Only reached once the master has been knocked off under us (a revert, a map load - see the
			// camera reset at sim 0x26D385, which restores these bytes only when its game-state check passes).
			// Re-establish the engine's full ON state, not just the master, for the reason in applyFreecam.
			// Gating on master == 0 is what keeps this from fighting the player's own sub-mode hotkeys: while
			// freecam is genuinely on we return above and never touch +0x9C9 / +0x9CA.
			const uint8_t values[3]{ 1, 0, 1 };
			HCEGetPlayerState::tryWriteRaw(address, values, sizeof(values));
		}
		catch (HCMRuntimeException)
		{
			// expected while the chain is down
		}
	}

	// Camera position offset within a camera entry. Hard-coded in the reference tool too
	// (player_camera.py:216) and absent from its addresses.json.
	static constexpr int64_t kCameraPositionOffset = 0x20;

	// Declared LAST, see HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ActionEvent> mTeleportToCameraCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEFreecamImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		forceTeleportWeak(resolveDependentCheat(HCEForceTeleport)),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->freeCameraToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mTeleportToCameraCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->freeCameraTeleportToCameraEvent, [this]() { onTeleportToCamera(); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFreecam only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEFreecamImpl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mTeleportToCameraCallback.removeCallback();
		mRenderEventCallback.removeCallback();
	}
};


HCEFreecam::HCEFreecam(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEFreecamImpl>(game, dicon))
{
}

HCEFreecam::~HCEFreecam()
{
	PLOG_VERBOSE << "~" << getName();
}
