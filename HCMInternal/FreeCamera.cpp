#include "pch.h"
#include "FreeCamera.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "MidhookFlagInterpreter.h"
#include "IMakeOrGetCheat.h"
#include "GetGameCameraData.h"
#include "GetPlayerViewAngle.h"
#include "ThirdPersonRendering.h"
#include "DisableScreenEffects.h"
#include "BlockPlayerCharacterInput.h"
//#include "PlayerControlledFreeCamera.h"
//#include "ObjectAnchoredFreeCam.h"
#include "UpdateGameCameraData.h"
#include "FreeCameraData.h"
#include "GameCameraData.h"
#include "CameraTransformer.h"
#include "Speedhack.h"

#include "ISmoother.h"
#include "NullSmoother.h"
#include "LinearSmoother.h"

#include "UserCameraInputReader.h"
#include "GlobalKill.h"
#include "ForceTeleport.h"
#include "FreeCameraFOVOverride.h"
#include "H2FreeCameraCollision.h"

template <GameState::Value gameT>  // templated so that each game gets a seperate instance of the static members
class FreeCameraImpl : public FreeCameraImplUntemplated
{
private:
	//GameState mGame;
	static inline FreeCameraImpl<gameT>* instance = nullptr;
	GameState gameImpl;

	static inline std::atomic_bool hookIsRunning = false;

	// event callbacks
	ScopedCallback <ToggleEvent> mFreeCameraToggleCallback;
	ScopedCallback< eventpp::CallbackList<void(const MCCState&)>> MCCStateChangedCallback;
	ScopedCallback <ToggleEvent> mThirdPersonRenderingToggleCallback; 
	ScopedCallback <ToggleEvent> mDisableScreenEffectsToggleCallback;
	ScopedCallback <ToggleEvent> mBlockPlayerCharacterInputToggleCallback;

	ScopedCallback <ActionEvent> freeCameraTeleportToCameraCallback;

	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetPositionCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetPositionFillCurrentCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetPositionCopyCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetPositionPasteCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetRotationCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetRotationFillCurrentCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetRotationCopyCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraSetRotationPasteCallback;

	ScopedCallback <ActionEvent> freeCameraUserInputCameraIncreaseTranslationSpeedHotkeyCallback;
	ScopedCallback <ActionEvent> freeCameraUserInputCameraDecreaseTranslationSpeedHotkeyCallback;
	ScopedCallback <ActionEvent> freeCameraZoomHotkeyCallback;


	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<GetPlayerViewAngle> getPlayerViewAngleWeak;
	std::weak_ptr<Speedhack> speedhackWeak;
	std::weak_ptr<GetGameCameraData> getGameCameraDataWeak;
	std::weak_ptr<UpdateGameCameraData> updateGameCameraDataWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::optional<std::weak_ptr< DisableScreenEffects>> disableScreenEffectsOptionalWeak;
	std::shared_ptr<SharedRequestToken> disableScreenEffectsRequest;
	std::optional<std::weak_ptr< ThirdPersonRendering>> thirdPersonRenderingOptionalWeak;
	std::shared_ptr<SharedRequestToken> thirdPersonRenderingRequest;
	std::optional<std::weak_ptr< BlockPlayerCharacterInput>> blockPlayerCharacterInputOptionalWeak;
	std::shared_ptr<SharedRequestToken> blockPlayerCharacterInputRequest;

	std::optional<std::weak_ptr<ForceTeleport>> forceTeleportOptionalWeak;


	std::weak_ptr<UserCameraInputReader> userCameraInputReaderWeak;
	PositionTransformer userControlledPosition;
	RotationTransformer userControlledRotation;
	FOVTransformer userControlledFOV;

	std::weak_ptr<FreeCameraFOVOverride> freeCameraFOVOverrideProviderWeak;
	std::shared_ptr<SharedRequestToken> freeCameraFOVOverrideRequest;

	// hooks
	static inline std::shared_ptr<ModuleMidHook> setCameraDataHook;

	// data
	bool needToResetCamera = true;
	std::shared_ptr<MultilevelPointer> gameFrameDeltaPointer;

	// Halo 2 only: free camera collision (see H2FreeCameraCollision.h). The camera position of the previous frame is the start
	// of this frame's swept test. collisionHaveLast is cleared (any thread) whenever the camera is placed somewhere new
	// (reset, set-position) so that jump is never swept through walls.
	SimpleMath::Vector3 collisionLastPos = SimpleMath::Vector3::Zero;
	std::atomic_bool collisionHaveLast = false;

	// ZOOM (2026-11-20, user: "a 'zoom' button/option to Free Cam ... implement it the way Halo 3 does"). Halo 3's Theater
	// flying camera (halo3.dll sub_180215648): a button press toggles zoom (flag bit 0); while zoomed the FOV is globals'
	// "zoomed field of view" (20 deg), look speed x "zoomed look speed" (0.5), and the change is eased over "zoom transition
	// time" (0.5 s) - values from H3EK globals.globals' camera block. Here the zoomed FOV comes from Zoom Amount (x
	// magnification of the free camera's own FOV) and the base FOV setting is never changed by zooming.
	static constexpr float kZoomTransitionSeconds = 0.5f;
	static constexpr float kZoomLookScale = 0.5f;
	std::atomic_bool zoomWanted = false;
	float zoomBlend = 0.f;   // 0 = out, 1 = fully in (camera thread only)

	void onFreeCameraZoomHotkey()
	{
		lockOrThrow(settingsWeak, settings);
		if (!settings->freeCameraToggle->GetValue()) { zoomWanted = false; return; }
		zoomWanted = !zoomWanted.load();
		PLOG_INFO << "free camera zoom " << (zoomWanted ? "in" : "out");
	}

	// the zoomed FOV for this frame (degrees); advances the ease by frameDelta (0 = paused: holds)
	float zoomedFOV(float baseFOV, float frameDelta, float zoomAmount, FreeCameraData& data)
	{
		const float target = zoomWanted ? 1.f : 0.f;
		if (frameDelta > 0.f)
		{
			const float step = frameDelta / kZoomTransitionSeconds;
			zoomBlend = target > zoomBlend ? std::min(target, zoomBlend + step) : std::max(target, zoomBlend - step);
		}
		const float e = zoomBlend * zoomBlend * (3.f - 2.f * zoomBlend);   // smoothstep
		data.zoomLookScale = 1.f + (kZoomLookScale - 1.f) * e;
		if (e <= 0.f || zoomAmount <= 1.f || baseFOV <= 0.f) return baseFOV;
		const float mag = std::pow(zoomAmount, e);                          // eased in magnification, so it feels even
		constexpr float d2r = 0.017453292f;
		const float half = std::clamp(baseFOV, 0.01f, 179.f) * 0.5f * d2r;
		return 2.f * std::atan(std::tan(half) / mag) / d2r;
	}

	// event to fire on mcc state change (just set needToSetupCamera to true). also disable freecam.
	void onGameStateChange(const MCCState&)
	{
		try
		{
			PLOG_DEBUG << "free camera onGameStateChange";
			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValueDisplay())
			{
				settings->freeCameraToggle->GetValueDisplay() = false;
				settings->freeCameraToggle->UpdateValueWithInput();
			}

				

		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}

		needToResetCamera = true;
	}


	

	static void setCameraDataHookFunction(SafetyHookContext& ctx)
	{
		LOG_ONCE("setCameraDataHookFunction running for the first time")

		ScopedAtomicBool lock(hookIsRunning);

		if (instance == nullptr)
		{
			PLOG_ERROR << "null freeCameraImpl for gameState " << ((GameState)gameT).toString();
			return;
		}

		LOG_ONCE("calling instance->setCameraData()")
		instance->setCameraData();
	
	}

	

	void resetCamera(GameCameraData& gameCameraData, std::shared_ptr<SettingsStateAndEvents> settings, float targetFOVCopy)
	{
		LOG_ONCE(PLOG_VERBOSE << "resetting camera data");
		// position
		userControlledPosition.setPositionTransformation(*gameCameraData.position);
		collisionHaveLast = false;


		// rotation
		float initialYaw = std::atan2(gameCameraData.lookDirForward->y, gameCameraData.lookDirForward->x);
		float initialPitch = std::asin(gameCameraData.lookDirForward->z) * -1.f;
		float initialRoll = 0.f;

		PLOG_DEBUG << "Initial EulerYaw: " << initialYaw;
		PLOG_DEBUG << "Initial EulerPitch: " << initialPitch;
		PLOG_DEBUG << "Initial EulerRoll: " << initialRoll;

		userControlledRotation.setRotationTransformation(initialYaw, initialPitch, initialRoll);

		// fov
		userControlledFOV.setFOV(targetFOVCopy);

	}


	// Halo 2: keep the free camera out of the level (and the objects the engine's own camera collides with) unless the
	// shared "No-clip camera" setting is on. Game thread, inside the observer update. Any failure = no collision.
	void applyHalo2Collision(FreeCameraData& freeCameraData, bool noClip, float collisionRadius)
	{
		const bool finiteNow = std::isfinite(freeCameraData.currentPosition.x) && std::isfinite(freeCameraData.currentPosition.y) && std::isfinite(freeCameraData.currentPosition.z);
		if (!finiteNow) { collisionHaveLast = false; return; }

		if (!noClip && collisionHaveLast.load())
		{
			const float from[3] = { collisionLastPos.x, collisionLastPos.y, collisionLastPos.z };
			float to[3] = { freeCameraData.currentPosition.x, freeCameraData.currentPosition.y, freeCameraData.currentPosition.z };
			if (h2fcc::collide(from, to, collisionRadius))
			{
				const SimpleMath::Vector3 allowed(to[0], to[1], to[2]);
				if (allowed != freeCameraData.currentPosition)
				{
					// The camera's position is a SMOOTHED value chasing a target that the input moves; the target runs well
					// ahead of it (the lead). Putting both on `allowed` (what the first version did) threw the lead away on
					// every frame that touched a wall, so the camera crawled along walls - the "friction". Instead the lead
					// is moved through the same collision from the allowed position: the part of it that points into the
					// wall is lost, the part along the wall is kept - what Halo 3's flying camera gets from its mover
					// handing back the displacement that is left.
					const SimpleMath::Vector3 smoothed = userControlledPosition.getPositionTransformation();
					const SimpleMath::Vector3 base = freeCameraData.currentPosition - smoothed;   // whatever else placed the camera
					const SimpleMath::Vector3 lead = userControlledPosition.getTargetPositionTransformation() - smoothed;
					SimpleMath::Vector3 leadEnd = allowed;
					if (std::isfinite(lead.x) && std::isfinite(lead.y) && std::isfinite(lead.z) && lead.LengthSquared() > 1e-10f)
					{
						float lt[3] = { allowed.x + lead.x, allowed.y + lead.y, allowed.z + lead.z };
						if (h2fcc::collide(to, lt, collisionRadius)) leadEnd = SimpleMath::Vector3(lt[0], lt[1], lt[2]);
					}
					freeCameraData.currentPosition = allowed;
					userControlledPosition.setPositionTransformations(allowed - base, leadEnd - base);
				}
			}
		}

		collisionLastPos = freeCameraData.currentPosition;
		collisionHaveLast = true;
	}

	void setCameraData()
	{

		LOG_ONCE(PLOG_VERBOSE << "setCameraData running for the first time");

		try
		{
			if (GlobalKill::isKillSet())
			{
				onFreeCameraToggleChange(false);
				return;
			}


			LOG_ONCE(PLOG_VERBOSE << "getting game camera data");
			lockOrThrow(getGameCameraDataWeak, getGameCameraData);
			auto gameCameraData = getGameCameraData->getGameCameraData();

			// "world" absolute data. Gets transformed into final position/rotation by cameraTransformers.
			LOG_ONCE(PLOG_VERBOSE << "constructing world-null camera data");
			FreeCameraData freeCameraData;

			lockOrThrow(settingsWeak, settings);

			LOG_ONCE(PLOG_VERBOSE << "getting base FOV from setting value");
			float targetFOVCopy = settings->freeCameraUserInputCameraBaseFOV->GetValue();
			LOG_ONCE_CAPTURE(PLOG_VERBOSE << "FOV value: " << fov, fov = targetFOVCopy);

			if (needToResetCamera)
			{
				LOG_ONCE(PLOG_VERBOSE << "Resetting camera");
				resetCamera(gameCameraData, settings, targetFOVCopy);
				needToResetCamera = false;
			}

			LOG_ONCE(PLOG_VERBOSE << "getting frameDelta");
			float frameDelta;
			if (!gameFrameDeltaPointer->readData(&frameDelta)) throw HCMRuntimeException("Could not resolve MCCFrameDeltaPointer");
			LOG_ONCE_CAPTURE(PLOG_VERBOSE << "frameDelta value: " << fd, fd = frameDelta);

			// can be exactly zero if paused, but negative doesn't even make sense.
			if (frameDelta < 0.f) throw HCMRuntimeException(std::format("negative frame delta?! that's unpossible! {} ", frameDelta));

			// adjust frameDelta by speedhack setting
			{
				LOG_ONCE(PLOG_VERBOSE << "adjusting frameDelta by speedhack multiplier");
				lockOrThrow(speedhackWeak, speedhack);
				frameDelta = frameDelta * (1.00 / speedhack->getCurrentSpeedMultiplier()); // this is also why speedhack must never be zero
				LOG_ONCE_CAPTURE(PLOG_VERBOSE << "speedhack-adjusted frameDelta value: " << fd, fd = frameDelta);
			}

			LOG_ONCE(PLOG_DEBUG << "reading user inputs and applying userControlledCamera transformations");

			if (frameDelta != 0.f) // this would cause division by zeros. frameDelta can be zero when game is paused.
			{
				// we will scale rotation sensitivity by fov value so fov transform first
				userControlledFOV.transformFOV(freeCameraData, frameDelta, targetFOVCopy);
				LOG_ONCE(PLOG_DEBUG << "userControlledFOV.transformFOV DONE");
				// zoom: the camera (and look speed) use the zoomed FOV; the base FOV setting keeps targetFOVCopy
				freeCameraData.currentFOV = zoomedFOV(targetFOVCopy, frameDelta, settings->freeCameraZoomAmount->GetValue(), freeCameraData);

				// rotate before position so that the position transformer knows which direction is "forward/up/right" etc 
				userControlledRotation.transformRotation(freeCameraData, frameDelta);
				LOG_ONCE(PLOG_DEBUG << "userControlledRotation.transformRotation DONE");

				userControlledPosition.transformPosition(freeCameraData, frameDelta);
				LOG_ONCE(PLOG_DEBUG << "userControlledPosition.transformPosition DONE");

				if constexpr (gameT == GameState::Value::Halo2)
					applyHalo2Collision(freeCameraData, settings->chaseCameraNoClip->GetValue(), settings->freeCameraCollisionRadius->GetValue());


			}


			// must be after userControlled and before any other transformers
			LOG_ONCE_CAPTURE(PLOG_VERBOSE << "updating FOV setting with transformed value: " << fov, fov = targetFOVCopy);
			settings->freeCameraUserInputCameraBaseFOV->GetValueDisplay() = targetFOVCopy; 
			settings->freeCameraUserInputCameraBaseFOV->GetValue() = targetFOVCopy;
			LOG_ONCE(PLOG_VERBOSE << "fov setting updated");

			LOG_ONCE(PLOG_DEBUG << "updating game camera");
			lockOrThrow(updateGameCameraDataWeak, updateGameCameraData);
			// zoomed FOV (eased); paused frames hold the current zoom
			const float renderFOV = frameDelta != 0.f ? freeCameraData.currentFOV : zoomedFOV(targetFOVCopy, 0.f, settings->freeCameraZoomAmount->GetValue(), freeCameraData);
			updateGameCameraData->updateGameCameraData(gameCameraData, freeCameraData, renderFOV);
			LOG_ONCE(PLOG_DEBUG << "updating game camera DONE");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
			settingsWeak.lock()->freeCameraToggle->GetValueDisplay() = false;
			settingsWeak.lock()->freeCameraToggle->UpdateValueWithInput();
		}
	}



	// setting toggle event callback
	void onFreeCameraToggleChange(bool newValue)
	{
		// zoom starts out each time the free camera is switched on or off
		zoomWanted = false;
		zoomBlend = 0.f;
		try
		{

			needToResetCamera = true; 
			PLOG_DEBUG << "onToggleChange: newval: " << (newValue ? "true" : "false");



			if ( thirdPersonRenderingOptionalWeak.has_value())
			{
				lockOrThrow(settingsWeak, settings);
				lockOrThrow(thirdPersonRenderingOptionalWeak.value(), thirdPersonRendering);
				
				if (newValue && settings->freeCameraThirdPersonRendering->GetValue())
					thirdPersonRenderingRequest = thirdPersonRendering->makeScopedRequest();
				else
					thirdPersonRenderingRequest.reset();
			}

			if (disableScreenEffectsOptionalWeak.has_value())
			{
				lockOrThrow(settingsWeak, settings);
				lockOrThrow(disableScreenEffectsOptionalWeak.value(), disableScreenEffects);

				if (newValue && settings->freeCameraDisableScreenEffects->GetValue())
					disableScreenEffectsRequest = disableScreenEffects->makeScopedRequest();
				else
					disableScreenEffectsRequest.reset();
			}

			if (blockPlayerCharacterInputOptionalWeak.has_value())
			{
				lockOrThrow(settingsWeak, settings);
				lockOrThrow(blockPlayerCharacterInputOptionalWeak.value(), blockPlayerCharacterInput);

				if (newValue && settings->freeCameraGameInputDisable->GetValue())
					blockPlayerCharacterInputRequest = blockPlayerCharacterInput->makeScopedRequest();
				else
					blockPlayerCharacterInputRequest.reset();
			}

			// set hooks 
			{
				LOG_ONCE(PLOG_VERBOSE << "freezing threads as we update setCameraDataHook");


				LOG_ONCE(PLOG_VERBOSE << "setCameraDataHook->setWantsToBeAttached()")
				setCameraDataHook->setWantsToBeAttached(newValue);
				LOG_ONCE(PLOG_VERBOSE << "setCameraDataHook->setWantsToBeAttached() DONE");
				
				if (newValue)
				{
					PLOG_DEBUG << "setCameraDataHook redirecting to new func at: " << std::hex << &setCameraDataHookFunction;
				}

				if (newValue)
				{
					lockOrThrow(freeCameraFOVOverrideProviderWeak, freeCameraFOVOverrideProvider);
					freeCameraFOVOverrideRequest = freeCameraFOVOverrideProvider->makeScopedRequest();
				}
				else if (freeCameraFOVOverrideRequest)
					freeCameraFOVOverrideRequest.reset();

			}



			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameT))
			{
				lockOrThrow(messagesGUIWeak, messagesGUI)
				messagesGUI->addMessage(newValue ? "Free Camera enabled." : "Free Camera disabled.");
			}

		}
		catch (HCMRuntimeException ex)
		{
			thirdPersonRenderingRequest.reset();
			blockPlayerCharacterInputRequest.reset();
			disableScreenEffectsRequest.reset();
			freeCameraFOVOverrideRequest.reset();
			runtimeExceptions->handleMessage(ex);
			try
			{
				lockOrThrow(settingsWeak, settings);
				settings->freeCameraToggle->resetToDefaultValue();
			}
			catch (HCMRuntimeException ex)
			{
				runtimeExceptions->handleMessage(ex);
			}
		}

	}


	void onThirdPersonRenderingChange(bool& newValue)
	{
		if (!thirdPersonRenderingOptionalWeak.has_value()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValue()) // only need to bother flipping if freeCamera is enabled
			{
				lockOrThrow(thirdPersonRenderingOptionalWeak.value(), thirdPersonRendering);

				if (newValue)
					thirdPersonRenderingRequest = thirdPersonRendering->makeScopedRequest();
				else
					thirdPersonRenderingRequest.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onDisableScreenEffectsChange(bool& newValue)
	{
		if (!disableScreenEffectsOptionalWeak.has_value()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValue()) // only need to bother flipping if freeCamera is enabled
			{
				lockOrThrow(disableScreenEffectsOptionalWeak.value(), disableScreenEffects);

				if (newValue)
					disableScreenEffectsRequest = disableScreenEffects->makeScopedRequest();
				else
					disableScreenEffectsRequest.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onBlockPlayerCharacterInputChange(bool& newValue)
	{
		if (!blockPlayerCharacterInputOptionalWeak.has_value()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValue()) // only need to bother flipping if freeCamera is enabled
			{
				lockOrThrow(blockPlayerCharacterInputOptionalWeak.value(), blockPlayerCharacterInput);

				if (newValue)
					blockPlayerCharacterInputRequest = blockPlayerCharacterInput->makeScopedRequest();
				else
					blockPlayerCharacterInputRequest.reset();
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}



	void onFreeCameraUserInputCameraSetPosition()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);

			if (settings->freeCameraToggle->GetValue() == false)
			{
				throw HCMRuntimeException("Can't do that while freecam is disabled!");
			}

			userControlledPosition.setPositionTransformation(settings->freeCameraUserInputCameraSetPositionVec3->GetValue());
			collisionHaveLast = false; // an explicit placement is never swept through walls
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetPositionFillCurrent()
	{
		try 
		{
			PLOG_DEBUG << "onFreeCameraUserInputCameraSetPositionFillCurrent running game: " << gameImpl.toString();

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);

			if (settings->freeCameraToggle->GetValue() == false)
			{
				throw HCMRuntimeException("Can't do that while freecam is disabled!");
			}

			// why is this getting junk data?

			settings->freeCameraUserInputCameraSetPositionVec3->GetValueDisplay() = userControlledPosition.getPositionTransformation();
			settings->freeCameraUserInputCameraSetPositionVec3->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetPositionCopy()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			settings->freeCameraUserInputCameraSetPositionVec3->serialiseToClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetPositionPaste()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			settings->freeCameraUserInputCameraSetPositionVec3->deserialiseFromClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetRotation()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);

			if (settings->freeCameraToggle->GetValue() == false)
			{
				throw HCMRuntimeException("Can't do that while freecam is disabled!");
			}

			userControlledRotation.setRotationTransformation(settings->freeCameraUserInputCameraSetRotationVec3->GetValue());
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetRotationFillCurrent()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);

			if (settings->freeCameraToggle->GetValue() == false)
			{
				throw HCMRuntimeException("Can't do that while freecam is disabled!");
			}

			settings->freeCameraUserInputCameraSetRotationVec3->GetValueDisplay() = userControlledRotation.getRotationTransformation();
			settings->freeCameraUserInputCameraSetRotationVec3->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetRotationCopy()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			settings->freeCameraUserInputCameraSetRotationVec3->serialiseToClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	void onFreeCameraUserInputCameraSetRotationPaste()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			settings->freeCameraUserInputCameraSetRotationVec3->deserialiseFromClipboard();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onFreeCameraUserInputCameraIncreaseTranslationSpeedHotkey()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValue() == false) return;

			float factor = settings->freeCameraUserInputCameraTranslationSpeedChangeFactor->GetValue();
			settings->freeCameraUserInputCameraTranslationSpeed->GetValueDisplay() = settings->freeCameraUserInputCameraTranslationSpeed->GetValueDisplay() * factor;
			settings->freeCameraUserInputCameraTranslationSpeed->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onFreeCameraUserInputCameraDecreaseTranslationSpeedHotkey()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);
			if (settings->freeCameraToggle->GetValue() == false) return;

			float factor = settings->freeCameraUserInputCameraTranslationSpeedChangeFactor->GetValue();
			settings->freeCameraUserInputCameraTranslationSpeed->GetValueDisplay() = settings->freeCameraUserInputCameraTranslationSpeed->GetValueDisplay() / factor;
			settings->freeCameraUserInputCameraTranslationSpeed->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onFreeCameraTeleportToCameraCallback()
	{
		try
		{

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(gameImpl) == false) return;

			lockOrThrow(settingsWeak, settings);

			if (settings->freeCameraToggle->GetValue() == false)
			{
				PLOG_ERROR << "Can't do that while freecam is disabled!";
				return;
			}

			if (forceTeleportOptionalWeak.has_value() == false)
			{
				throw HCMRuntimeException("Could not resolve force teleport service!");
			}

			lockOrThrow(forceTeleportOptionalWeak.value(), forceTeleport);


			auto cameraPosition = userControlledPosition.getPositionTransformation();

			if (settings->freeCameraTeleportToCameraSlightlyBehind->GetValue())
			{
				auto camRot = userControlledRotation.getRotationTransformation();
				// convert euler to lookdir
				SimpleMath::Vector3 cameraDirection =
				{
					std::cos(camRot.x) * std::cos(camRot.y),
					std::sin(camRot.x) * std::cos(camRot.y),
					std::sin(camRot.y)
				};

				// cam direction is a unit vector; one unit behind should be enough to make it not visible.
				cameraPosition -= cameraDirection;
			}

			forceTeleport->teleportPlayerTo(cameraPosition);


		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}




public: 
	FreeCameraImpl(GameState game, IDIContainer& dicon)
		:
		gameImpl(game),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		getGameCameraDataWeak(resolveDependentCheat(GetGameCameraData)),
		getPlayerViewAngleWeak(resolveDependentCheat(GetPlayerViewAngle)),
		updateGameCameraDataWeak(resolveDependentCheat(UpdateGameCameraData)),
		userCameraInputReaderWeak(resolveDependentCheat(UserCameraInputReader)),
		speedhackWeak(resolveDependentCheat(Speedhack)),
		mFreeCameraToggleCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraToggle->valueChangedEvent, [this](bool& n) { onFreeCameraToggleChange(n); }),
		mThirdPersonRenderingToggleCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraThirdPersonRendering->valueChangedEvent, [this](bool& n) { onThirdPersonRenderingChange(n); }),
		mDisableScreenEffectsToggleCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraDisableScreenEffects->valueChangedEvent, [this](bool& n) { onDisableScreenEffectsChange(n); }),
		mBlockPlayerCharacterInputToggleCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraGameInputDisable->valueChangedEvent, [this](bool& n) { onBlockPlayerCharacterInputChange(n); }),
		MCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& state) { onGameStateChange(state); }),
		userControlledPosition(userCameraInputReaderWeak.lock(), settingsWeak.lock()->freeCameraUserInputCameraTranslationInterpolator, settingsWeak.lock()->freeCameraUserInputCameraTranslationInterpolatorLinearFactor, settingsWeak.lock()->freeCameraUserInputCameraTranslationInterpolatorDrift),
		userControlledRotation(userCameraInputReaderWeak.lock(), settingsWeak.lock()->freeCameraUserInputCameraRotationInterpolator, settingsWeak.lock()->freeCameraUserInputCameraRotationInterpolatorLinearFactor, settingsWeak.lock()->freeCameraUserInputCameraRotationInterpolatorDrift),
		userControlledFOV(userCameraInputReaderWeak.lock(), settingsWeak.lock()->freeCameraUserInputCameraFOVInterpolator, settingsWeak.lock()->freeCameraUserInputCameraFOVInterpolatorLinearFactor, settingsWeak.lock()->freeCameraUserInputCameraFOVInterpolatorDrift),

		freeCameraUserInputCameraSetPositionCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetPosition, [this]() { onFreeCameraUserInputCameraSetPosition(); }),
		freeCameraUserInputCameraSetPositionFillCurrentCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetPositionFillCurrent, [this]() { onFreeCameraUserInputCameraSetPositionFillCurrent(); }),
		freeCameraUserInputCameraSetPositionCopyCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetPositionCopy, [this]() { onFreeCameraUserInputCameraSetPositionCopy(); }),
		freeCameraUserInputCameraSetPositionPasteCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetPositionPaste, [this]() { onFreeCameraUserInputCameraSetPositionPaste(); }),
		freeCameraUserInputCameraSetRotationCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetRotation, [this]() { onFreeCameraUserInputCameraSetRotation(); }),
		freeCameraUserInputCameraSetRotationFillCurrentCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetRotationFillCurrent, [this]() { onFreeCameraUserInputCameraSetRotationFillCurrent(); }),
		freeCameraUserInputCameraSetRotationCopyCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetRotationCopy, [this]() { onFreeCameraUserInputCameraSetRotationCopy(); }),
		freeCameraUserInputCameraSetRotationPasteCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraSetRotationPaste, [this]() { onFreeCameraUserInputCameraSetRotationPaste(); }),
	
		freeCameraUserInputCameraIncreaseTranslationSpeedHotkeyCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraIncreaseTranslationSpeedHotkey, [this]() { onFreeCameraUserInputCameraIncreaseTranslationSpeedHotkey(); }),
		freeCameraUserInputCameraDecreaseTranslationSpeedHotkeyCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraUserInputCameraDecreaseTranslationSpeedHotkey, [this]() { onFreeCameraUserInputCameraDecreaseTranslationSpeedHotkey(); }),
		freeCameraZoomHotkeyCallback(dicon.Resolve< SettingsStateAndEvents>().lock()->freeCameraZoomHotkey, [this]() { try { onFreeCameraZoomHotkey(); } catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); } }),
		freeCameraTeleportToCameraCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->freeCameraTeleportToCameraEvent, [this]() {onFreeCameraTeleportToCameraCallback(); }),
		freeCameraFOVOverrideProviderWeak(resolveDependentCheat(FreeCameraFOVOverride))
	{
		instance = this;
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		gameFrameDeltaPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(gameFrameDeltaPointer));
		auto setCameraDataFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(setCameraDataFunction), game);
		setCameraDataHook = ModuleMidHook::make(game.toModuleName(), setCameraDataFunction, setCameraDataHookFunction);

		try
		{
			thirdPersonRenderingOptionalWeak = resolveDependentCheat(ThirdPersonRendering);
		}
		catch (HCMInitException ex)
		{
			PLOG_DEBUG << "failed to resolve optional dependent cheat thirdPersonRenderingOptionalWeak, error: " << ex.what();
		}

		try
		{
			disableScreenEffectsOptionalWeak = resolveDependentCheat(DisableScreenEffects);
		}
		catch (HCMInitException ex)
		{
			PLOG_DEBUG << "failed to resolve optional dependent cheat disableScreenEffectsOptionalWeak, error: " << ex.what();
		}

		try
		{
			blockPlayerCharacterInputOptionalWeak = resolveDependentCheat(BlockPlayerCharacterInput);
		}
		catch (HCMInitException ex)
		{
			PLOG_DEBUG << "failed to resolve optional dependent cheat blockPlayerCharacterInputOptionalWeak, error: " << ex.what();
		}

		try
		{
			forceTeleportOptionalWeak = resolveDependentCheat(ForceTeleport);
		}
		catch (HCMInitException ex)
		{
			PLOG_DEBUG << "failed to resolve optional dependent cheat forceTeleportOptionalWeak, error: " << ex.what();
		}

		   

	}

	~FreeCameraImpl()
	{
		PLOG_DEBUG << "~FreeCameraImpl";
		// safety check that hook isn't running
		if (hookIsRunning)
		{
			PLOG_INFO << "Waiting for freeCamera hook to finish execution";
			hookIsRunning.wait(true);
		}



		if (setCameraDataHook)
			setCameraDataHook.reset();


		instance = nullptr;
	}
};







FreeCamera::FreeCamera(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo1:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::Halo1>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo2:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo3:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::Halo3>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::Halo3ODST>>(gameImpl, dicon);
		break;

	case GameState::Value::HaloReach:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::HaloReach>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo4:
		pimpl = std::make_unique<FreeCameraImpl<GameState::Value::Halo4>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("not impl");
	}
}

FreeCamera::~FreeCamera()
{
	PLOG_DEBUG << "FreeCamera destructor was called! are we not cleaning up?";
	PLOG_DEBUG << "~" << getName();
}