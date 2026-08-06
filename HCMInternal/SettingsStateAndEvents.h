#pragma once
#include <atomic>
#include "UnarySetting.h"
#include "BinarySetting.h"
#include "ISettingsSerialiser.h"
#include "GUIServiceInfo.h"
#include "WaypointList.h"
#include "SkullEnum.h"
#include "BitBoolPointer.h"
#include "SettingsEnums.h"
#include "SubpixelID.h"
#include "ViewAngleLineList.h"

class SettingsStateAndEvents
{
private:
	std::shared_ptr<ISettingsSerialiser> mSerialiser;

	// COMPLETE list of every BinarySetting (unlike allSerialisableOptions, which is a curated persist-subset that
	// omits the cheat toggles). Presets snapshot this. It's built automatically: mPresetCollectorArmer points the
	// static collector at this vector BEFORE any setting member constructs, each BinarySetting self-registers (see
	// BinarySetting's ctor), and mPresetCollectorDisarmer (the very last member) clears the collector afterwards.
	// These two helpers MUST bracket every setting member in declaration order.
	std::vector<SerialisableSetting*> allPresetOptions;
	struct PresetCollectorArmer { PresetCollectorArmer(std::vector<SerialisableSetting*>* v) { SerialisableSetting::s_presetCollector = v; } };
	PresetCollectorArmer mPresetCollectorArmer{ &allPresetOptions };

public:
	SettingsStateAndEvents(std::shared_ptr<ISettingsSerialiser> serialiser)
		: mSerialiser(serialiser)
	{ 
		// deserialise (load) serialisable options
		mSerialiser->deserialise(allSerialisableOptions); 

	}
	~SettingsStateAndEvents() {
		PLOG_DEBUG << "~SettingsStateAndEvents()";
		// serialise (save) serialisable options
		mSerialiser->serialise(allSerialisableOptions);
	};

	// --- Presets: save/load the full settings snapshot to/from a named file (see PresetManager) ---
	// Fired by the Save/Load Preset buttons. PresetManager handles the file dialog then calls these.
	std::shared_ptr<ActionEvent> presetSaveEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> presetLoadEvent = std::make_shared<ActionEvent>();

	// Presets snapshot EVERY setting EXCEPT a deliberately-excluded few (see presetOptionsExcludingGated): the master
	// tickrate (its arming gate + value) and the Season 7 physics fix. Those are per-session, potentially game-
	// destabilising toggles the user wants to arm by hand each boot, never auto-restored by a loaded profile.
	std::vector<SerialisableSetting*> presetOptionsExcludingGated()
	{
		std::vector<SerialisableSetting*> out;
		out.reserve(allPresetOptions.size());
		for (auto* s : allPresetOptions)
			if (s != masterTickrateEnabled.get() && s != customTickrate.get() && s != season7PhysicsToggle.get())
				out.push_back(s);
		return out;
	}

	// Write every setting (including cheat toggles) to the given file - EXCEPT the gated ones above. That's what
	// makes a preset able to turn features on/off, while leaving the tickrate / Season 7 physics out of profiles.
	void savePresetToFile(const std::string& fullFilePath)
	{
		mSerialiser->serialiseToPath(fullFilePath, presetOptionsExcludingGated());
	}

	// Apply a full settings snapshot from the given file. Each setting fires its valueChangedEvent, so features
	// react live. Settings absent from the file keep their current value. MUST be called on the render thread.
	void loadPresetFromFile(const std::string& fullFilePath)
	{
		mSerialiser->deserialiseFromPath(fullFilePath, presetOptionsExcludingGated());
	}

	//	hotkeys - see HotkeyEventsLambdas for how they connect to respective toggle (they just flip the value)
	std::shared_ptr<ActionEvent> toggleGUIHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> togglePauseHotkeyEvent = std::make_shared<ActionEvent>();

	// replay (input record/playback) action events - fired by both the GUI buttons and the rebindable hotkeys.
	// ReplayRecorder / ReplayPlayer subscribe to these directly.
	std::shared_ptr<ActionEvent> replayRecord30Event = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> replayRecord60Event = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> replayStopSaveEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> replayLoadFileEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> replayPlayEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> replayStopPlaybackEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> speedhackHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> invulnerabilityHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> aiFreezeHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> medusaHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> naturalCheckpointDisableHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> infiniteAmmoHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> bottomlessClipHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> display2DInfoHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraGameInputDisableHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraCameraInputDisableHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraIncreaseTranslationSpeedHotkey = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraDecreaseTranslationSpeedHotkey = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraMaintainVelocityHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraAnchorPositionToObjectPositionHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraAnchorPositionToObjectRotationHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraAnchorRotationToObjectPositionHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraAnchorRotationToObjectFacingHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraAnchorFOVToObjectDistanceHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> hideHUDToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> setPlayerHealthEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> toggleWaypoint3DHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> toggleViewAngle3DHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> triggerOverlayToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> softCeilingOverlayToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> disableBarriersHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> abilityMeterToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> sensDriftOverlayToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> shieldInputPrinterHotkeyEvent = std::make_shared<ActionEvent>();
	
	

	
	// events
	std::shared_ptr<ActionEvent> showGUIFailures = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> advanceTicksEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceCheckpointEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceRevertEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceDoubleRevertEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceCoreSaveEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceCoreLoadEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> masterTickrateFlipEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> injectCheckpointEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> dumpCheckpointEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> injectCoreEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> dumpCoreEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceMissionRestartEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> commandConsoleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> commandConsoleExecuteBufferEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> getObjectAddressEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> getTagAddressEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> playerPositionToClipboardEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceTeleportEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceTeleportAbsoluteFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceTeleportAbsoluteCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceTeleportAbsolutePaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceLaunchEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleSet = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAnglePaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleAdjustHorizontal = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleAdjustVertical = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDSet = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDPaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDAdjustNegative = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> editPlayerViewAngleIDAdjustPositive = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraTeleportToCameraEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraBindingsPopup = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetPosition = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetPositionFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetPositionCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetPositionPaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetRotation = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetRotationFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetRotationCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetRotationPaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetVelocity = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetVelocityFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetVelocityCopy = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> freeCameraUserInputCameraSetVelocityPaste = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> switchBSPEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> switchBSPSetLoadSetEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> switchBSPLoadIndexEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> switchBSPUnloadIndexEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> switchBSPSetFillCurrent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> forceFutureCheckpointFillEvent = std::make_shared<ActionEvent>(); 
	std::shared_ptr<ActionEvent> triggerOverlayFilterStringDialogEvent = std::make_shared<ActionEvent>(); 
	std::shared_ptr<ActionEvent> triggerOverlayFilterStringCopyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> triggerOverlayFilterStringPasteEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> getPlayerDatumEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> getPlayerAddressEvent = std::make_shared<ActionEvent>(); // resolve the local player's object (heap) address
	std::shared_ptr<ActionEvent> sensResetCountsEvent = std::make_shared<ActionEvent>();

	

	// waypoint events (delete, edit)
	std::shared_ptr<WaypointAndListEvent> deleteWaypointEvent = std::make_shared<WaypointAndListEvent>();
	std::shared_ptr<WaypointAndListEvent> editWaypointEvent = std::make_shared<WaypointAndListEvent>();
	std::shared_ptr<WaypointListEvent> addWaypointEvent = std::make_shared<WaypointListEvent>();

	// view angle line events (delete, edit)
	std::shared_ptr<ViewAngleLineAndListEvent> deleteViewAngleLineEvent = std::make_shared<ViewAngleLineAndListEvent>();
	std::shared_ptr<ViewAngleLineAndListEvent> editViewAngleLineEvent = std::make_shared<ViewAngleLineAndListEvent>();
	std::shared_ptr<ViewAngleLineListEvent> addViewAngleLineEvent = std::make_shared<ViewAngleLineListEvent>();


	// skulls
	std::shared_ptr<eventpp::CallbackList<void(GameState)>> updateSkullBitBoolCollectionEvent = std::make_shared<eventpp::CallbackList<void(GameState)>>(); // called by GUI to tell SkullToggler to update pointer data
	std::map<SkullEnum, BitBoolPointer> skullBitBoolCollection; // pointer data is cached and updated on MCC gamestate change
	std::atomic_bool skullBitBoolCollectionInUse = false; // locked while cache is being updated or in use by gui

	// Halo Campaign Evolved skulls. Deliberately separate from the three members above: HCE has 56 skulls
	// addressed by BIT INDEX into one contiguous bitfield rather than MCC's per-skull MultilevelPointers, and 25
	// of them have no SkullEnum entry at all (see HCESkullEnum.h). Kept HaloCER-only so nothing MCC-side moves.
	// GUIHCESkullToggle reads/writes only these; HCESkullToggler owns every memory access.
	std::shared_ptr<eventpp::CallbackList<void(GameState)>> hceSkullUpdateEvent = std::make_shared<eventpp::CallbackList<void(GameState)>>(); // gui -> cheat: refresh hceSkullEngineState from game memory
	std::shared_ptr<eventpp::CallbackList<void(int, bool)>> hceSkullSetEvent = std::make_shared<eventpp::CallbackList<void(int, bool)>>();     // gui -> cheat: user toggled bit N
	std::array<bool, 56> hceSkullEngineState{}; // last read of the engine's bitfield. Not a shadow copy - refreshed every render.
	bool hceSkullStateValid = false;            // false => chain is down, gui shows "waiting for game"


	// hotkeys for each skull
	std::shared_ptr<ActionEvent> skullAngerToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullAssassinsToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullBlackEyeToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullBlindToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullCatchToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullEyePatchToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullFamineToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullFogToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullForeignToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullIronToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullJackedToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullMasterblasterToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullMythicToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullRecessionToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullSoAngryToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullStreakingToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullSwarmToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullThatsJustWrongToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullTheyComeBackToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullThunderstormToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullTiltToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullToughLuckToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullBandannaToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullBondedPairToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullBoomToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullCowbellToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullEnvyToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullFeatherToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullGhostToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullGruntBirthdayPartyToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullGruntFuneralToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullIWHBYDToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullMalfunctionToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullPinataToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullProphetBirthdayPartyToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullScarabToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullSputnikToggleHotkeyEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> skullAcrophobiaToggleHotkeyEvent = std::make_shared<ActionEvent>();


	// settings

	std::shared_ptr<BinarySetting<float>> messagesFontSize = std::make_shared<BinarySetting<float>>
		(
			10.f,
			[](float in) { return in > 6.f; }, // must be positive
			nameof(messagesFontSize)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> messagesFontColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 1.f, 0.2f, 0.f, 1.f },
			[](SimpleMath::Vector4 in) { return true; },
			nameof(messagesFontColor)
		);

	std::shared_ptr<BinarySetting<bool>> GUIWindowOpen = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(GUIWindowOpen)
		);

	std::shared_ptr<BinarySetting<bool>> GUIShowingFreesCursor = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(GUIShowingFreesCursor)
		);


	std::shared_ptr<BinarySetting<bool>> GUIShowingBlocksInput = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(GUIShowingBlocksInput)
		);

	std::shared_ptr<BinarySetting<bool>> GUIShowingPausesGame = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(GUIShowingPausesGame)
		);

	std::shared_ptr<BinarySetting<bool>> togglePause = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(togglePause)
		);

	std::shared_ptr<BinarySetting<bool>> pauseAlsoBlocksInput = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(pauseAlsoBlocksInput)
		);

	std::shared_ptr<BinarySetting<bool>> pauseAlsoFreesCursor = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(pauseAlsoFreesCursor)
		);

	std::shared_ptr<BinarySetting<int>> advanceTicksCount = std::make_shared<BinarySetting<int>>
		(
			1,
			[](int in) { return in > 0; }, // must be positive
			nameof(advanceTicksCount)
		);


	std::shared_ptr<BinarySetting<bool>> hideWatermark = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hideWatermark)
		);

	std::shared_ptr<BinarySetting<bool>> hideWatermarkHideMessages = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hideWatermarkHideMessages)
		);

	std::shared_ptr<BinarySetting<bool>> injectionIgnoresChecksum = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectionIgnoresChecksum)
		);

	std::shared_ptr<BinarySetting<bool>> injectCheckpointForcesRevert = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCheckpointForcesRevert)
		);

	std::shared_ptr<BinarySetting<bool>> injectCheckpointLevelCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCheckpointLevelCheck)
		);

	std::shared_ptr<BinarySetting<bool>> injectCheckpointVersionCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCheckpointVersionCheck)
		);

	std::shared_ptr<BinarySetting<bool>> injectCheckpointDifficultyCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCheckpointDifficultyCheck)
		);

	std::shared_ptr<BinarySetting<bool>> autonameCheckpoints = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(autonameCheckpoints)
		);

	std::shared_ptr<BinarySetting<bool>> dumpCheckpointForcesSave = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(dumpCheckpointForcesSave)
		);

	// Halo Campaign Evolved ONLY. The shipped game keeps its checkpoints outside the process, so the only instant a
	// checkpoint blob exists here is the hand-off to the storage provider. This makes HCM shadow-copy that blob on
	// every checkpoint, natural or forced, so Dump Checkpoint can write the checkpoint the player ALREADY HAS
	// instead of forcing a new one. Default on, because a dump that finds nothing is useless; the toggle exists so
	// the cost can be declined - turning it off installs no hook and copies no bytes at all.
	// Cost measured on the dev machine: 0.44 ms median / 0.98 ms worst for the 0xC10000-byte state, once per
	// checkpoint. See HCECheckpointDetours.cpp.
	std::shared_ptr<BinarySetting<bool>> hceShadowCheckpoints = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceShadowCheckpoints)
		);

	std::shared_ptr<BinarySetting<bool>> injectCoreForcesRevert = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCoreForcesRevert)
		);

	std::shared_ptr<BinarySetting<bool>> injectCoreLevelCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCoreLevelCheck)
		);

	std::shared_ptr<BinarySetting<bool>> injectCoreVersionCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCoreVersionCheck)
		);

	std::shared_ptr<BinarySetting<bool>> injectCoreDifficultyCheck = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(injectCoreDifficultyCheck)
		);

	std::shared_ptr<BinarySetting<bool>> dumpCoreForcesSave = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(dumpCoreForcesSave)
		);

	std::shared_ptr<BinarySetting<bool>> autonameCoresaves = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(autonameCoresaves)
		);

	std::shared_ptr<BinarySetting<bool>> forceFutureCheckpointToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(forceFutureCheckpointToggle)
		);

	std::shared_ptr<BinarySetting<int>> forceFutureCheckpointTick = std::make_shared<BinarySetting<int>>
		(
			1000,
			[](int in) { return in >= 0; },
			nameof(forceFutureCheckpointTick)
		);

	std::shared_ptr<BinarySetting<bool>> speedhackToggle = std::make_shared<BinarySetting<bool>>
	(
		false,
		[](bool in) { return true; },
		nameof(speedhackToggle)
	);

	std::shared_ptr<BinarySetting<double>> speedhackSetting = std::make_shared<BinarySetting<double>>
	(
		10.f,
		[](double in) { return in > 0; }, // must be positive
		nameof(speedhackSetting)
	);


	std::shared_ptr<BinarySetting<bool>> invulnerabilityToggle = std::make_shared<BinarySetting<bool>>
	(
		false,
		[](bool in) { return true; },
		nameof(invulnerabilityToggle)
	);

	std::shared_ptr<BinarySetting<bool>> invulnerabilityNPCToggle = std::make_shared<BinarySetting<bool>>
	(
		false,
		[](bool in) { return true; },
		nameof(invulnerabilityNPCToggle)
	);

	std::shared_ptr<BinarySetting<bool>> aiFreezeToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(aiFreezeToggle)
		);

	std::shared_ptr<BinarySetting<bool>> havokDebuggerToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(havokDebuggerToggle)
		);

	std::shared_ptr<BinarySetting<bool>> medusaToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(medusaToggle)
		);



	std::shared_ptr<BinarySetting<bool>> consoleCommandPauseGame = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(consoleCommandPauseGame)
		);

	std::shared_ptr<BinarySetting<bool>> consoleCommandBlockInput = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(consoleCommandBlockInput)
		);

	std::shared_ptr<BinarySetting<bool>> consoleCommandFreeCursor = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(consoleCommandFreeCursor)
		);

	std::shared_ptr<BinarySetting<float>> consoleCommandFontSize = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](int in) { return in > 6.f; },
			nameof(consoleCommandFontSize)
		);


	

	std::shared_ptr<BinarySetting<uint32_t>> getObjectAddressDWORD = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(getObjectAddressDWORD)
		);

	std::shared_ptr<BinarySetting<uint32_t>> getTagAddressDWORD = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(getTagAddressDWORD)
		);

	// Result boxes for the Debug "Get Player Datum" / "Get Player Address" buttons: the presenter writes the resolved
	// value here and the read-only GUI box displays it so the user can select & copy it (vs a transient on-screen print).
	std::shared_ptr<BinarySetting<std::string>> getPlayerDatumResult = std::make_shared<BinarySetting<std::string>>
		(
			"",
			[](std::string in) { return true; },
			nameof(getPlayerDatumResult)
		);

	std::shared_ptr<BinarySetting<std::string>> getPlayerAddressResult = std::make_shared<BinarySetting<std::string>>
		(
			"",
			[](std::string in) { return true; },
			nameof(getPlayerAddressResult)
		);

	std::shared_ptr<BinarySetting<bool>> forceTeleportApplyToPlayer = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(forceTeleportApplyToPlayer)
		);

	std::shared_ptr<BinarySetting<uint32_t>> forceTeleportCustomObject = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(forceTeleportCustomObject)
		);


	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> forceTeleportAbsoluteVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(forceTeleportAbsoluteVec3)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> forceTeleportRelativeVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 5.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(forceTeleportRelativeVec3)
		);

	std::shared_ptr<BinarySetting<bool>> forceTeleportForward = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(forceTeleportForward)
		);



	std::shared_ptr<BinarySetting<bool>> forceTeleportForwardIgnoreZ = std::make_shared<BinarySetting<bool>> // ignore vertical component of players look angle
		(
			false,
			[](bool in) { return true; },
			nameof(forceTeleportForwardIgnoreZ)
		);



	std::shared_ptr<BinarySetting<bool>> forceTeleportManual = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(forceTeleportManual)
		);

	std::shared_ptr<BinarySetting<bool>> forceLaunchApplyToPlayer = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(forceLaunchApplyToPlayer)
		);

	std::shared_ptr<BinarySetting<uint32_t>> forceLaunchCustomObject = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADB33F,
			[](uint32_t in) { return true; },
			nameof(forceLaunchCustomObject)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> forceLaunchAbsoluteVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.f, 0.f, 5.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(forceLaunchAbsoluteVec3)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> forceLaunchRelativeVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.5f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(forceLaunchRelativeVec3)
		);


	std::shared_ptr<BinarySetting<bool>> forceLaunchForward = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(forceLaunchForward)
		);

	std::shared_ptr<BinarySetting<bool>> forceLaunchForwardIgnoreZ = std::make_shared<BinarySetting<bool>> // ignore vertical component of players look angle
		(
			false,
			[](bool in) { return true; },
			nameof(forceLaunchForwardIgnoreZ)
		);

	std::shared_ptr<BinarySetting<bool>> forceLaunchManual = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(forceLaunchManual)
		);



	std::shared_ptr<BinarySetting<bool>> naturalCheckpointDisable = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(naturalCheckpointDisable)
		);

	std::shared_ptr<BinarySetting<bool>> infiniteAmmoToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(infiniteAmmoToggle)
		);

	std::shared_ptr<BinarySetting<bool>> bottomlessClipToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(bottomlessClipToggle)
		);

	std::shared_ptr<BinarySetting<bool>> season7PhysicsToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(season7PhysicsToggle)
		);

	std::shared_ptr<BinarySetting<bool>> dropShadowsOnObjectsToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(dropShadowsOnObjectsToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sphereSpecularForceToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sphereSpecularForceToggle)
		);

	std::shared_ptr<BinarySetting<bool>> uncapDropShadowsToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(uncapDropShadowsToggle)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::H2ShadowResolution>> h2ShadowResolutionSetting = std::make_shared<BinarySetting<SettingsEnums::H2ShadowResolution>>
		(
			SettingsEnums::H2ShadowResolution::Retail,
			[](SettingsEnums::H2ShadowResolution in) { return true; },
			nameof(h2ShadowResolutionSetting)
		);

	std::shared_ptr<BinarySetting<bool>> h2ArmorColourToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(h2ArmorColourToggle)
		);
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> h2ArmorColourPrimary = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 0.15f, 0.35f, 0.15f, 1.0f }, // MC-ish green
			[](SimpleMath::Vector4 in) { return true; },
			nameof(h2ArmorColourPrimary)
		);
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> h2ArmorColourSecondary = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 0.20f, 0.20f, 0.20f, 1.0f }, // dark grey trim
			[](SimpleMath::Vector4 in) { return true; },
			nameof(h2ArmorColourSecondary)
		);
	std::shared_ptr<ActionEvent> h2ArmorColourSavePresetEvent = std::make_shared<ActionEvent>();
	std::shared_ptr<ActionEvent> h2ArmorColourLoadPresetEvent = std::make_shared<ActionEvent>();

	// Custom emblem (pixel-injection): show any PNG on the MasterChief biped emblem.
	std::shared_ptr<BinarySetting<bool>> h2ArmorEmblemToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(h2ArmorEmblemToggle)
		);
	std::shared_ptr<ActionEvent> h2ArmorEmblemLoadEvent = std::make_shared<ActionEvent>();

	std::shared_ptr<BinarySetting<bool>> uncapVisibilityLimitsToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(uncapVisibilityLimitsToggle)
		);

	std::shared_ptr<BinarySetting<bool>> uncapClusterLimitToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(uncapClusterLimitToggle)
		);

	std::shared_ptr<BinarySetting<bool>> offscreenShadowCastersToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(offscreenShadowCastersToggle)
		);

	// Halo 2: multiplier applied to the instanced-geo/object bounding-sphere radius in the visibility test
	// (OffscreenShadowCasters). Higher = casters stay shadowing from further off-camera. Live-editable.
	std::shared_ptr<BinarySetting<float>> offscreenShadowCastersMultiplier = std::make_shared<BinarySetting<float>>
		(
			25.f,
			[](float in) { return in >= 1.f; },
			nameof(offscreenShadowCastersMultiplier)
		);

	std::shared_ptr<BinarySetting<float>> farClipDistance = std::make_shared<BinarySetting<float>>
		(
			1024.f,
			[](float in) { return in >= 0.f; },
			nameof(farClipDistance)
		);

	// Master-tickrate arming gate. OFF by default and deliberately EXCLUDED from presets/normal persistence (see
	// presetOptionsExcludingGated), so every boot starts un-armed: the tickrate value input + 60/30 flip only appear
	// (GUIToggleWithChildren) and only apply once the user turns this on, so the tickrate can't be nudged by accident.
	// Turning it OFF restores the game's stock tickrate (MasterTickrate subscribes to this).
	std::shared_ptr<BinarySetting<bool>> masterTickrateEnabled = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(masterTickrateEnabled)
		);

	// Halo 2: arbitrary master simulation tickrate (Hz). Applied by the MasterTickrate cheat, which also writes
	// the matching seconds-per-tick (dt = 1/rate) so game speed stays 1x. Restricted 3..32766: the engine reads the
	// tickrate as a SIGNED int16 (movsx word[+2] -> cvtdq2ps in the tick accumulator @halo2.dll+0x706CAB), so 32767
	// is the true ceiling (32766 as a safe limit) and rates of 1-2 crash the game.
	std::shared_ptr<BinarySetting<int>> customTickrate = std::make_shared<BinarySetting<int>>
		(
			60,
			// reject 1-2 (crash the game) and anything past the signed-int16 tickrate field's safe max (32766).
			// An invalid input is rejected (value reverts), so a partial keystroke never applies a bad rate.
			[](int in) { return in >= 3 && in <= 32766; },
			nameof(customTickrate)
		);

	// Runtime coordination flag (NOT a serialisable setting): set true by MasterTickrate once the user applies a
	// custom tickrate, so its collision "tickrate scalar" repoint owns halo2.dll+0x70DBFA. Season7Physics checks
	// this and stands down while it's set (both patch the same instruction). Cleared on MasterTickrate teardown.
	std::atomic<bool> customTickrateScalarActive{ false };

	// Halo 2 "FP Scale Fix": renders the first-person legs/body at a fixed reference FOV (no FOV stretch), via a
	// runtime code cave. LegSize = the cave's TAN_REF reference (lower = bigger legs); LegTuck = body push FOV (rad).
	std::shared_ptr<BinarySetting<bool>> fpScaleFixToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool) { return true; },
			nameof(fpScaleFixToggle)
		);
	std::shared_ptr<BinarySetting<float>> fpScaleFixLegSize = std::make_shared<BinarySetting<float>>
		(
			0.65f,
			[](float in) { return in > 0.f && in < 4.f; },
			nameof(fpScaleFixLegSize)
		);
	std::shared_ptr<BinarySetting<float>> fpScaleFixLegTuck = std::make_shared<BinarySetting<float>>
		(
			1.5f,
			[](float in) { return in >= 0.f && in < 3.2f; },
			nameof(fpScaleFixLegTuck)
		);

	// Halo 2: anchors the sun glow + occlusion query at the fixed corona distance instead of the
	// far-clip plane, so raising Far Clip Distance no longer shrinks the sun. Sits under the slider.
	std::shared_ptr<BinarySetting<bool>> sunScaleFixToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sunScaleFixToggle)
		);

	// Halo 2: bundle of animation/interpolation fixes (rocket firing interp + cyclotron elevator
	// + rocket launcher animation). Replaces the old standalone rocketLauncherAnimationFixToggle.
	std::shared_ptr<BinarySetting<bool>> animationFixesToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(animationFixesToggle)
		);


	std::shared_ptr<BinarySetting<bool>> display2DInfoToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(display2DInfoToggle)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowGameTick = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowGameTick)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowNextObjectDatum = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowNextObjectDatum)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowAggro = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowAggro)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowRNG = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowRNG)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowBSP = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowBSP)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowBSPSet = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowBSPSet)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoTrackPlayer = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoTrackPlayer)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerViewAngle= std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerViewAngle)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerViewAngleID = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerViewAngleID)
		);





	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerPosition = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerPosition)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerVelocity = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerVelocity)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerVelocityAbs = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerVelocityAbs)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerVelocityXY = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerVelocityXY)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerVelocityXYZ = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerVelocityXYZ)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerHealth = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerHealth)
		);


	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerRechargeCooldown = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerRechargeCooldown)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowPlayerVehicleHealth = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowPlayerVehicleHealth)
		);



	std::shared_ptr<BinarySetting<bool>> display2DInfoTrackCustomObject = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(display2DInfoTrackCustomObject)
		);

	std::shared_ptr<BinarySetting<uint32_t>> display2DInfoCustomObjectDatum = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADB33F,
			[](uint32_t in) { return true; },
			nameof(display2DInfoCustomObjectDatum)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityObjectType = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityObjectType)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityTagName = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityTagName)
		);


	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityPosition = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityPosition)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityVelocity = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityVelocity)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityVelocityAbs = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityVelocityAbs)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityVelocityXY = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityVelocityXY)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityVelocityXYZ = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityVelocityXYZ)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityHealth = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityHealth)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityRechargeCooldown = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityRechargeCooldown)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoShowEntityVehicleHealth = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoShowEntityVehicleHealth)
		);


	std::shared_ptr<BinarySetting<SettingsEnums::ScreenAnchorEnum>> display2DInfoAnchorCorner = std::make_shared<BinarySetting<SettingsEnums::ScreenAnchorEnum>>
		(
			SettingsEnums::ScreenAnchorEnum::BottomRight,
			[](SettingsEnums::ScreenAnchorEnum in) { return true; },
			nameof(display2DInfoAnchorCorner)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> display2DInfoScreenOffset = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{400, 400},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(display2DInfoScreenOffset)
		);

	std::shared_ptr<BinarySetting<float>> display2DInfoFontSize = std::make_shared<BinarySetting<float>>
		(
			16.f, 
			[](int in) { return in > 6.f ; },
			nameof(display2DInfoFontSize)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> display2DInfoFontColour = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.00f, 0.60f, 0.25f, 1.00f}, // a nice orange colour that matches the rest of the gui
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(display2DInfoFontColour)
		);

	std::shared_ptr<BinarySetting<int>> display2DInfoFloatPrecision = std::make_shared<BinarySetting<int>>
		(
			7,
			[](int in) { return in >= 0; },
			nameof(display2DInfoFloatPrecision)
		);

	std::shared_ptr<BinarySetting<bool>> display2DInfoOutline = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(display2DInfoOutline)
		);

	// Halo Campaign Evolved 2D info overlay content toggles. HCEDisplayInfo reuses display2DInfoToggle and every
	// display2DInfo* VISUAL setting (anchor/offset/size/colour/precision/outline) - only the row list differs,
	// because HCE can supply nothing but these seven things. See HCEDisplayInfo.cpp.
	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowCoordinates = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowCoordinates)
		);

	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowVelocity = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowVelocity)
		);

	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowLevel = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowLevel)
		);

	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowBSP = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowBSP)
		);

	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowTick = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowTick)
		);

	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowPlayerDatum = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowPlayerDatum)
		);

	// Diagnostic row. If this shows an address, the game-thread TLS walk is alive and so is every other HCE
	// feature; if it does not, none of them can work. Worth keeping even though it means nothing to a player.
	std::shared_ptr<BinarySetting<bool>> hceDisplayInfoShowTEB = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceDisplayInfoShowTEB)
		);

	// Halo Campaign Evolved trigger overlay. It reuses triggerOverlayToggle (and therefore the existing
	// Trigger Overlay hotkey), triggerOverlayNormalColor, triggerOverlaySectorColor and
	// triggerOverlayLabelScale verbatim; only these two are HCE-specific.
	//
	// There is deliberately no FOV setting: the overlay reads the UE5 render camera's horizontal FOV live
	// (APlayerCameraManager::DoUpdateCamera midhook -> POV.FOV). See HCETriggerOverlay.cpp.

	// WORLD units (1 world unit = 10 feet), NOT the MCC-scaled renderDistance3D. 50 matches the reference
	// tool's default.
	std::shared_ptr<BinarySetting<float>> hceTriggerOverlayRenderDistance = std::make_shared<BinarySetting<float>>
		(
			50.f,
			[](float in) { return in >= 1.f && in <= 2000.f; },
			nameof(hceTriggerOverlayRenderDistance)
		);

	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowLabels = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowLabels)
		);

	// The player's trigger test point, drawn as a sphere - HCE's equivalent of the MCC overlay's
	// triggerOverlayPositionToggle. Reuses triggerOverlayPositionColor and triggerOverlayPositionScale.
	// Keeps World Partition streaming areas from tearing down when the player leaves them, which is what makes
	// the sky and lighting vanish out of bounds. Default OFF - see HCESkyFix.h for the accumulation caveat.
	// Removes the fade-in from black after a revert. See HCEDisableFadeFromBlack.h - it patches the fade
	// DURATION on the post-restore path only, so cinematic fades are untouched.
	std::shared_ptr<BinarySetting<bool>> hceDisableFadeFromBlackToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceDisableFadeFromBlackToggle)
		);

	std::shared_ptr<BinarySetting<bool>> hceSkyFixToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceSkyFixToggle)
		);

	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowVertex = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowVertex)
		);

	// Colour volumes the mission scripts are ACTIVELY testing differently from dormant ones. "Active" means a
	// HaloScript thread evaluated the volume within the last few seconds - see HCETriggerActivity. Volumes that
	// are never tested stay the normal colour, so this reads as "what could actually fire right now".
	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayHighlightActive = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayHighlightActive)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceTriggerOverlayActiveColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(0.f, 1.f, 0.2f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceTriggerOverlayActiveColor)
		);

	// Labels used to inherit the volume's own colour, which made them unreadably dark against the world
	// (a wireframe colour that reads fine as a thin line is far too dark as text). Own setting, bright default.
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceTriggerOverlayLabelColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(1.f, 1.f, 1.f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceTriggerOverlayLabelColor)
		);

	// BSP / zone-set switching volumes, coloured separately because hitting one unloads and loads whole areas -
	// it is the single most consequential kind of volume to cross by accident. See HCESpeedrunTriggerNames.h:
	// the classification is a NAME HEURISTIC, because Halo's scenario format has no flag for it.
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceTriggerOverlayBspColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(1.f, 0.35f, 0.9f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceTriggerOverlayBspColor)
		);

	// Kill volumes ("stay OUT") and safe zones ("stay IN") are the same feature family and share one toggle, but
	// they mean OPPOSITE things - the engine kills you when you are inside no safe zone - so drawing them the
	// same colour would be actively misleading. Red for lethal-to-enter, blue for lethal-to-leave.
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceTriggerOverlayKillColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(1.f, 0.15f, 0.15f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceTriggerOverlayKillColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceTriggerOverlaySafeZoneColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(0.25f, 0.6f, 1.f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceTriggerOverlaySafeZoneColor)
		);

	// ---- Halo Campaign Evolved structure-BSP overlay. See HCEBspOverlay.h.
	std::shared_ptr<BinarySetting<bool>> hceBspOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceBspOverlayToggle)
		);

	// TRUE = only surfaces carrying the collision INVISIBLE flag (bit1 of surface_flags).
	//
	// ⚠ DEFAULTS OFF, and that is a MEASURED decision, not a preference. On the first real level tested, 575 of
	// 575 structure surfaces had bit1 clear, so this filter drew nothing at all - while the level's structural
	// shell was exactly what the user wanted to see and showed correctly with the filter off. The engine on
	// this title evidently does not mark its non-rendered structure surfaces with the invisible bit. The
	// toggle is kept because the flag is real and other levels may use it, but defaulting it ON ships a
	// feature that looks broken. HCEBspOverlay logs a full surface-flag histogram per rebuild so which bits a
	// level actually uses is a matter of record rather than guesswork.
	std::shared_ptr<BinarySetting<bool>> hceBspOverlayInvisibleOnly = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceBspOverlayInvisibleOnly)
		);

	// Solid AND wireframe. This started as wireframe-only because the edge-ring winding convention the faces
	// are rebuilt from was unverified on this build - it is now VERIFIED: a 575-surface structure BSP
	// reconstructed with ZERO ring-walk failures, so every face closed and the fill is trustworthy. Wireframe
	// stays on top of the fill because the edges are what make the shell's shape readable.
	std::shared_ptr<BinarySetting<SettingsEnums::TriggerRenderStyle>> hceBspOverlayRenderStyle = std::make_shared<BinarySetting<SettingsEnums::TriggerRenderStyle>>
		(
			SettingsEnums::TriggerRenderStyle::SolidAndWireframe,
			[](SettingsEnums::TriggerRenderStyle in) { return true; },
			nameof(hceBspOverlayRenderStyle)
		);

	// WORLD units, same scale as hceTriggerOverlayRenderDistance (1 world unit = 10 feet). Defaults far higher
	// than the trigger overlay's 50 because a structural shell is one connected object - a short distance clips
	// it into a confusing fragment rather than thinning it out.
	std::shared_ptr<BinarySetting<float>> hceBspOverlayRenderDistance = std::make_shared<BinarySetting<float>>
		(
			200.f,
			[](float in) { return in >= 1.f && in <= 2000.f; },
			nameof(hceBspOverlayRenderDistance)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceBspOverlayColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(1.f, 0.f, 0.f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceBspOverlayColor)
		);

	std::shared_ptr<BinarySetting<float>> hceBspOverlayAlpha = std::make_shared<BinarySetting<float>>
		(
			0.3f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(hceBspOverlayAlpha)
		);

	// What to draw for a batch of surfaces the camera is INSIDE, exactly as the MCC trigger overlay's
	// "Render Solid Interior as" does. Normal draws it the same as from outside (this is what makes the shell
	// read as two-sided); DontRender skips it, which is the behaviour trigger volumes want - see the walls
	// from outside and nothing from within.
	//
	// ⚠ THIS IS ALSO THE PATTERN SWITCH. Patterned turns on the world-space checker, which is generated in the
	// PIXEL SHADER rather than sampled from a texture - the D3D12 path implements no textured draws, which is
	// why this option previously did nothing whatsoever on this game. Normal means NO pattern; the contrast
	// and size sliders only shape it once Patterned is selected.
	//
	// Defaults to Patterned because distinguishing a real surface from a hole is the main thing this overlay
	// gets used for, and a flat translucent colour cannot do it. DontRender would draw nothing at all, since
	// for a structure BSP the camera is essentially always inside the shell.
	std::shared_ptr<BinarySetting<SettingsEnums::TriggerInteriorStyle>> hceBspOverlayInteriorStyle = std::make_shared<BinarySetting<SettingsEnums::TriggerInteriorStyle>>
		(
			SettingsEnums::TriggerInteriorStyle::Patterned,
			[](SettingsEnums::TriggerInteriorStyle in) { return true; },
			nameof(hceBspOverlayInteriorStyle)
		);

	std::shared_ptr<BinarySetting<float>> hceBspOverlayWireframeAlpha = std::make_shared<BinarySetting<float>>
		(
			0.5f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(hceBspOverlayWireframeAlpha)
		);

	// Wireframe colours, per side. ⚠ .w MUST stay 1.0 on both: GUIColourPicker edits RGB only, so a zero alpha
	// here would be unrecoverable from the GUI. Opacity is hceBspOverlayWireframeAlpha, applied on top.
	// Cool-white and warm-amber rather than a red/green pair - distinguishable under common colour vision
	// deficiencies, and both brighter than either fill colour so the outlines actually read.
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceBspOverlayWireframeColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(0.118f, 1.f, 0.f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceBspOverlayWireframeColor)
		);

	// Shade each face by how much it points at a fixed world light.
	//
	// ⚠ NOT a lighting effect - it is the ONLY thing that makes an interior legible. The pixel shader is a
	// flat colour, so without this every wall of a room is the identical colour with no boundary between
	// them, and standing inside a closed shell fills the whole screen with one block of colour. From outside
	// the silhouette against the sky gives you the shape for free; inside there is no silhouette, so the
	// shape has to come from tone.
	std::shared_ptr<BinarySetting<bool>> hceBspOverlayFaceShading = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceBspOverlayFaceShading)
		);

	// How far shading may darken a face below its chosen colour. 0 = flat, 0.8 = strong.
	//
	// ⚠ CAPPED WELL BELOW 1.0 ON PURPOSE. Shading multiplies the colour, and the colour is then blended at the
	// opacity slider - so a heavily darkened face at a low opacity becomes effectively invisible and reads as a
	// HOLE punched in the overlay rather than as a shaded wall. Shading is a shape cue; it must never be able
	// to make geometry disappear. The brightest face is always exactly the colour the user picked.
	std::shared_ptr<BinarySetting<float>> hceBspOverlayShadingStrength = std::make_shared<BinarySetting<float>>
		(
			0.5f,
			[](float in) { return in >= 0.f && in <= 0.8f; },
			nameof(hceBspOverlayShadingStrength)
		);

	// Slight per-SURFACE tint variation, so a large area built from several collision surfaces reads as
	// separate wall pieces rather than one undifferentiated slab. Per surface, NOT per triangle - a triangle
	// is an arbitrary product of triangulation and varying by it would just look like noise.
	//
	// The offset is a stable hash of the surface's ordinal, so a wall keeps its tint as you move; it is
	// centred on zero so it brightens as often as it darkens and turning it on does not change how bright the
	// overlay looks overall.
	std::shared_ptr<BinarySetting<float>> hceBspOverlaySurfaceVariation = std::make_shared<BinarySetting<float>>
		(
			0.f,
			[](float in) { return in >= 0.f && in <= 0.5f; },
			nameof(hceBspOverlaySurfaceVariation)
		);

	// Whether a nearer surface hides the ones behind it (a depth pre-pass) or every surface blends through.
	//
	// These are the same property seen from two sides, so it cannot be decided for the user: occluding is what
	// stops an interior reading as a uniform wash, but it also means you cannot see a far wall through a near
	// one. Defaults OFF - seeing all the geometry at once is the more common reason to have a collision
	// overlay open at all, and the shading and per-surface variation now carry most of the readability that
	// the pre-pass was originally added to provide.
	std::shared_ptr<BinarySetting<bool>> hceBspOverlayOccludeFarSurfaces = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceBspOverlayOccludeFarSurfaces)
		);

	// World-space checker on filled surfaces.
	//
	// ⚠ THIS IS A DIAGNOSTIC AS MUCH AS A STYLE. A flat translucent fill genuinely cannot distinguish "there
	// is a surface here" from "I am looking through a hole at something behind it" - both are a wash of
	// colour. A pattern locked to WORLD space settles it: patterned means a surface is present, unpatterned
	// means it is not. It also gives an interior real depth cues, because the cells foreshorten with distance
	// and angle, which a flat colour never does.
	std::shared_ptr<BinarySetting<float>> hceBspOverlayPatternContrast = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(hceBspOverlayPatternContrast)
		);

	// Checker cell size in WORLD units (1 unit = 10 feet on this title), so ~0.5 is a chest-high square.
	std::shared_ptr<BinarySetting<float>> hceBspOverlayPatternScale = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.01f && in <= 20.f; },
			nameof(hceBspOverlayPatternScale)
		);

	// How many stacked layers a single facing surface should be made to LOOK like.
	//
	// ⚠ THIS IS COMPOSITING MATH, NOT A FUDGE. Looking into the shell from outside, a view ray crosses several
	// surfaces and they composite to 1-(1-a)^N. Standing inside a room it crosses ONE, giving a flat `a`, so
	// the same geometry reads far weaker from within - and the opacity slider cannot correct it, because it
	// lifts the stacked case by exactly as much. Raising this applies the same identity to the single layer.
	// 1.0 = off. 3 is about what a simple room costs you (near wall + far wall + floor).
	std::shared_ptr<BinarySetting<float>> hceBspOverlayLayerCompensation = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 1.f && in <= 8.f; },
			nameof(hceBspOverlayLayerCompensation)
		);

	// The face that points AWAY from the camera.
	//
	// ⚠ THE KEY IS STILL NAMED "Inside" ON PURPOSE - renaming it would silently reset every persisted config.
	// The LABEL is "faces away", because that is what the code computes: dot(planeNormal, camera) + d asks
	// "does this surface face me", not "am I inside this room". There is no per-surface notion of a room, which
	// is why the old inside/outside labelling read as inverted depending on where the user was standing.
	//
	// ⚠ THIS IS NOT A COSMETIC DUPLICATE OF hceBspOverlayColor. Looking at a shell from OUTSIDE, a view ray
	// crosses the near wall, the far wall, the floor and the ceiling, so four translucent layers accumulate and
	// the geometry reads as solid. From INSIDE a room it crosses ONE surface, and the identical colour and
	// opacity render as a faint wash that barely reads as a wall. The two sides need genuinely different
	// settings; a single pair cannot serve both. Defaults deliberately brighter and more opaque than the
	// outside pair for exactly that reason.
	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> hceBspOverlayInsideColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4(1.f, 0.f, 0.f, 1.f),
			[](SimpleMath::Vector4 in) { return true; },
			nameof(hceBspOverlayInsideColor)
		);

	// ⚠ SAME DEFAULT AS hceBspOverlayAlpha, deliberately. It used to default to double the facing value, to
	// compensate for away-faces looking weaker - but that weakness was a DRAW-ORDER bug (away-faces were being
	// painted over the faces you were looking at), not a property of the geometry. With the order fixed, equal
	// alpha genuinely reads equal, and defaulting them differently would just reintroduce the imbalance from
	// the other direction. They remain separate settings so the two sides CAN be told apart when wanted.
	std::shared_ptr<BinarySetting<float>> hceBspOverlayInsideAlpha = std::make_shared<BinarySetting<float>>
		(
			0.2f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(hceBspOverlayInsideAlpha)
		);

	// Show ONLY the volumes a speedrun has to hit (the community completion-requirement lists).
	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlaySpeedrunOnly = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hceTriggerOverlaySpeedrunOnly)
		);

	// "Types Shown" - per-category visibility. A level can carry hundreds of volumes and most of them are
	// irrelevant to whatever you are looking at, so these are a filter, not a style.
	// A volume is categorised ONCE (see HCETriggerOverlay), and the categories are checked in the same priority
	// the colours use, so a zone-set switch that is also a sector counts as a zone-set switch for both.
	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowRegular = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowRegular)
		);

	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowSector = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowSector)
		);

	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowKill = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowKill)
		);

	std::shared_ptr<BinarySetting<bool>> hceTriggerOverlayShowZoneSet = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(hceTriggerOverlayShowZoneSet)
		);

	std::shared_ptr<BinarySetting<bool>> hideHUDToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(hideHUDToggle)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> editPlayerViewAngleVec2 = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2(4.20, 0.69), // gottem
			[](SimpleMath::Vector2 in) { return (in.x >= 0.f) && (in.x < (std::numbers::pi_v<float> * 2.f)) && (in.y < (std::numbers::pi_v<float> / 2.f)) && (in.y > ((std::numbers::pi_v<float> / 2.f) * -1.f)); }, // x (yaw) must be from 0 to 6.14, y (pitch) must be from -1.57 to 1.57
			nameof(editPlayerViewAngleVec2)
		);

	std::shared_ptr<BinarySetting<float>> editPlayerViewAngleAdjustFactor = std::make_shared<BinarySetting<float>>
		(
			0.1f,
			[](float in) { return true; },
			nameof(editPlayerViewAngleAdjustFactor)
		);


	std::shared_ptr<BinarySetting<SubpixelID>> editPlayerViewAngleIDInt = std::make_shared<BinarySetting<SubpixelID>>
		(
			SubpixelID::fromFloat(std::numbers::pi_v<float>),
			[](int in) { return (in >= SubpixelID::fromFloat(0.f)) && (in < SubpixelID::fromFloat(std::numbers::pi_v<float> *2.f));  },
			nameof(editPlayerViewAngleIDVec2)
		);

	std::shared_ptr<BinarySetting<int>> editPlayerViewAngleIDAdjustFactor = std::make_shared<BinarySetting<int>>
		(
			1,
			[](int in) { return in > 0; },
			nameof(editPlayerViewAngleIDAdjustFactor)
		);






	std::shared_ptr<BinarySetting<bool>> freeCameraToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraToggle)
		);


	std::shared_ptr<BinarySetting<bool>> freeCameraThirdPersonRendering = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraThirdPersonRendering)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraDisableScreenEffects = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraDisableScreenEffects)
		);




	std::shared_ptr<BinarySetting<bool>> freeCameraGameInputDisable = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraGameInputDisable)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraCameraInputDisable = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraCameraInputDisable)
		);


	std::shared_ptr<BinarySetting<bool>> freeCameraTeleportToCameraSlightlyBehind = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraTeleportToCameraSlightlyBehind)
		);

	

	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraTranslationSpeedChangeFactor = std::make_shared<BinarySetting<float>>
		(
			1.5f,
			[](float in) { return in > 1.f; }, // 
			nameof(freeCameraUserInputCameraTranslationSpeedChangeFactor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> freeCameraUserInputCameraSetPositionVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 5.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(freeCameraUserInputCameraSetPositionVec3)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> freeCameraUserInputCameraSetRotationVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(freeCameraUserInputCameraSetRotationVec3)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraUserInputCameraMaintainVelocity = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraUserInputCameraMaintainVelocity)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> freeCameraUserInputCameraSetVelocityVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 5.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(freeCameraUserInputCameraSetVelocityVec3)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraBaseFOV = std::make_shared<BinarySetting<float>>
		(
			90.f,
			[](float in) { return in > 0.f && in < 180.f; }, 
			nameof(freeCameraUserInputCameraBaseFOV)
		);


	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraTranslationSpeed = std::make_shared<BinarySetting<float>>
		(
			3.f,
			[](float in) { return true; }, // if the user wants to have a negative speed that's their perogative
			nameof(freeCameraUserInputCameraTranslationSpeed)
		); 
	
	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraUserInputCameraTranslationInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraUserInputCameraTranslationInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraTranslationInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraUserInputCameraTranslationInterpolatorLinearFactor)
		);


	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraRotationSpeed = std::make_shared<BinarySetting<float>>
		(
			3.f,
			[](float in) { return true; }, // if the user wants to have a negative speed that's their perogative
			nameof(freeCameraUserInputCameraRotationSpeed)
		);

	// actually an enum but stored as int
	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraUserInputCameraRotationInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraUserInputCameraRotationInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraRotationInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraUserInputCameraRotationInterpolatorLinearFactor)
		);



	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraFOVSpeed = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return true; }, // if the user wants to have a negative speed that's their perogative
			nameof(freeCameraUserInputCameraFOVSpeed)
		);


	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraUserInputCameraFOVInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraUserInputCameraFOVInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraUserInputCameraFOVInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraUserInputCameraFOVInterpolatorLinearFactor)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraAnchorPositionToObjectPosition = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraAnchorPositionToObjectPosition)
		);


		std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>> freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup = std::make_shared<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>>
			(
				SettingsEnums::FreeCameraObjectTrackEnum::Player,
				[](SettingsEnums::FreeCameraObjectTrackEnum in) { return true; },
				nameof(freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup)
			);

	std::shared_ptr<BinarySetting<uint32_t>> freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraAnchorPositionToObjectPositionTranslationInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraAnchorPositionToObjectPositionTranslationInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor)
		);


	std::shared_ptr<BinarySetting<bool>> freeCameraAnchorPositionToObjectRotation = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraAnchorPositionToObjectRotation)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraAnchorRotationToObjectPosition = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraAnchorRotationToObjectPosition)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>> freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup = std::make_shared<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>>
		(
			SettingsEnums::FreeCameraObjectTrackEnum::Player,
			[](SettingsEnums::FreeCameraObjectTrackEnum in) { return true; },
			nameof(freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup)
		);

	std::shared_ptr<BinarySetting<uint32_t>> freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3)
		);
	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraAnchorRotationToObjectPositionRotationInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraAnchorRotationToObjectPositionRotationInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraAnchorRotationToObjectFacing = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraAnchorRotationToObjectFacing)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>> freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup = std::make_shared<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnum>>
		(
			SettingsEnums::FreeCameraObjectTrackEnum::Player,
			[](SettingsEnums::FreeCameraObjectTrackEnum in) { return true; },
			nameof(freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup)
		);

	std::shared_ptr<BinarySetting<uint32_t>> freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraAnchorRotationToObjectFacingRotationInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraAnchorRotationToObjectFacingRotationInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraAnchorFOVToObjectDistance = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(freeCameraAnchorFOVToObjectDistance)
		);


	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnumPlusAbsolute>> freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup = std::make_shared<BinarySetting<SettingsEnums::FreeCameraObjectTrackEnumPlusAbsolute>>
		(
			SettingsEnums::FreeCameraObjectTrackEnumPlusAbsolute::Player,
			[](SettingsEnums::FreeCameraObjectTrackEnumPlusAbsolute in) { return true; },
			nameof(freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup)
		);

	std::shared_ptr<BinarySetting<uint32_t>> freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum = std::make_shared<BinarySetting<uint32_t>>
		(
			0xDEADBEEF,
			[](uint32_t in) { return true; },
			nameof(freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector3>> freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3 = std::make_shared<BinarySetting<SimpleMath::Vector3>>
		(
			SimpleMath::Vector3{ 0.f, 0.f, 0.f },
			[](SimpleMath::Vector3 in) { return true; },
			nameof(freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>> freeCameraAnchorFOVToObjectDistanceFOVInterpolator = std::make_shared<BinarySetting<SettingsEnums::FreeCameraInterpolationTypesEnum>>
		(
			SettingsEnums::FreeCameraInterpolationTypesEnum::Linear,
			[](SettingsEnums::FreeCameraInterpolationTypesEnum in) { return true; },
			nameof(freeCameraAnchorFOVToObjectDistanceFOVInterpolator)
		);

	std::shared_ptr<BinarySetting<float>> freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor = std::make_shared<BinarySetting<float>>
		(
			0.06f,
			[](float in) { return in > 0.f && in <= 1.f; },
			nameof(freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraUserInputCameraRotationScalesToFOV = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraRotationSensivitiyScalesToFOV)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraUserInputCameraNonLinearFOVAtMinimum = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraUserInputCameraNonLinearFOVAtMinimum)
		);

	std::shared_ptr<BinarySetting<bool>> freeCameraUserInputCameraNonLinearFOVAtMaximum = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(freeCameraUserInputCameraNonLinearFOVAtMaximum)
		);

	std::shared_ptr<BinarySetting<int>> switchBSPIndex = std::make_shared<BinarySetting<int>>
		(
			0,
			[](int in) { return true; },
			nameof(switchBSPIndex)
		);

	std::shared_ptr<BinarySetting<int>> switchBSPSetLoadSet = std::make_shared<BinarySetting<int>>
		(
			0,
			[](int in) { return true; },
			nameof(switchBSPSetLoadSet)
		);

	std::shared_ptr<BinarySetting<int>> switchBSPSetLoadIndex = std::make_shared<BinarySetting<int>>
		(
			0,
			[](int in) { return true; },
			nameof(switchBSPSetLoadIndex)
		);

	std::shared_ptr<BinarySetting<int>> switchBSPSetUnloadIndex = std::make_shared<BinarySetting<int>>
		(
			0,
			[](int in) { return true; },
			nameof(switchBSPSetUnloadIndex)
		);


	std::shared_ptr<BinarySetting<bool>> OBSBypassToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(OBSBypassToggle)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> setPlayerHealthVec2 = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2(1.f, 1.f), // full health, full shields
			[](SimpleMath::Vector2 in) { return (in.x >= 0.f) && (in.x <= 1.f) && (in.y >= 0.f) && (in.y <= 1.f); }, // 0 to 1 inclusive
			nameof(setPlayerHealthVec2)
		);

	std::shared_ptr<BinarySetting<bool>> carrierBumpAnalyserToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(carrierBumpAnalyserToggle)
		);

	std::shared_ptr<BinarySetting<bool>> waypoint3DToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(waypoint3DToggle)
		);

	std::shared_ptr<BinarySetting<bool>> waypoint3DClampToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(waypoint3DClampToggle)
		);


	std::shared_ptr<BinarySetting<float>> waypoint3DGlobalSpriteScale = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in > 0.f; },
			nameof(waypoint3DGlobalSpriteScale)
		);

	std::shared_ptr<BinarySetting<float>> waypoint3DGlobalLabelScale = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](float in) { return in > 6.f; },
			nameof(waypoint3DGlobalLabelScale)
		);

	std::shared_ptr<BinarySetting<float>> waypoint3DGlobalDistanceScale = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](float in) { return in > 6.f; },
			nameof(waypoint3DGlobalDistanceScale)
		);

	std::shared_ptr<BinarySetting<int>> waypoint3DGlobalDistancePrecision = std::make_shared<BinarySetting<int>>
		(
			3,
			[](int in) { return in > 0; },
			nameof(waypoint3DGlobalDistancePrecision)
		);


	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> waypoint3DGlobalSpriteColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 0.f, 1.f, 0.f, 1.f }, // green
			[](SimpleMath::Vector4 in) { return true; },
			nameof(waypoint3DGlobalSpriteColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> waypoint3DGlobalLabelColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 0.f, 1.f, 0.f, 1.f }, // green
			[](SimpleMath::Vector4 in) { return true; },
			nameof(waypoint3DGlobalLabelColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> waypoint3DGlobalDistanceColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 0.f, 1.f, 0.f, 1.f }, // green
			[](SimpleMath::Vector4 in) { return true; },
			nameof(waypoint3DGlobalDistanceColor)
		);

	std::shared_ptr<UnarySetting<WaypointList>> waypoint3DList = std::make_shared<UnarySetting<WaypointList>>
		(
			WaypointList(), // default constructed
			nameof(waypoint3DList)
		);

	std::shared_ptr<BinarySetting<bool>> viewAngleLine3DToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(viewAngleLine3DToggle)
		);


	std::shared_ptr<BinarySetting<float>> viewAngleLine3DGlobalLabelScale = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](float in) { return in > 6.f; },
			nameof(viewAngleLine3DGlobalLabelScale)
		);

	std::shared_ptr<BinarySetting<float>> viewAngleLine3DGlobalDistanceScale = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](float in) { return in > 6.f; },
			nameof(viewAngleLine3DGlobalDistanceScale)
		);

	std::shared_ptr<BinarySetting<int>> viewAngleLine3DGlobalDistancePrecision = std::make_shared<BinarySetting<int>>
		(
			7,
			[](int in) { return in > 0; },
			nameof(viewAngleLine3DGlobalDistancePrecision)
		);


	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> viewAngleLine3DGlobalSpriteColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 1.f, 0.f, 0.f, 1.f }, // red
			[](SimpleMath::Vector4 in) { return true; },
			nameof(viewAngleLine3DGlobalSpriteColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> viewAngleLine3DGlobalLabelColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 1.f, 0.f, 0.f, 1.f }, // red
			[](SimpleMath::Vector4 in) { return true; },
			nameof(viewAngleLine3DGlobalLabelColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> viewAngleLine3DGlobalDistanceColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{ 1.f, 0.f, 0.f, 1.f }, // red
			[](SimpleMath::Vector4 in) { return true; },
			nameof(viewAngleLine3DGlobalDistanceColor)
		);

	std::shared_ptr<UnarySetting<ViewAngleLineList>> viewAngleLine3DList = std::make_shared<UnarySetting<ViewAngleLineList>>
		(
			ViewAngleLineList(std::vector<ViewAngleLine>{ViewAngleLine(SubpixelID::fromFloat(0.f), "Zero Boundary")}),
			nameof(viewAngleLine3DList)
		);


	std::shared_ptr<BinarySetting<bool>> triggerOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(triggerOverlayToggle)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayFilterToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(triggerOverlayFilterToggle)
		);

	std::shared_ptr<UnarySetting<int>> triggerOverlayFilterCountLabel = std::make_shared<UnarySetting<int>>
		(
			0,
			nameof(triggerOverlayFilterCountLabel)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayFilterExactMatch = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayFilterExactMatch)
		);

	std::shared_ptr<BinarySetting<std::string>> triggerOverlayFilterString = std::make_shared<BinarySetting<std::string>>
		(
			"",
			[](std::string in) { return true; },
			nameof(triggerOverlayFilterString)
		);




	std::shared_ptr<BinarySetting<SettingsEnums::TriggerRenderStyle>> triggerOverlayRenderStyle = std::make_shared<BinarySetting<SettingsEnums::TriggerRenderStyle>>
		(
			SettingsEnums::TriggerRenderStyle::SolidAndWireframe,
			[](SettingsEnums::TriggerRenderStyle in) { return true; },
			nameof(triggerOverlayRenderStyle)
		);



	std::shared_ptr<BinarySetting<SettingsEnums::TriggerInteriorStyle>> triggerOverlayInteriorStyle = std::make_shared<BinarySetting<SettingsEnums::TriggerInteriorStyle>>
		(
			SettingsEnums::TriggerInteriorStyle::Normal,
			[](SettingsEnums::TriggerInteriorStyle in) { return true; },
			nameof(triggerOverlayInteriorStyle)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::TriggerLabelStyle>> triggerOverlayLabelStyle = std::make_shared<BinarySetting<SettingsEnums::TriggerLabelStyle>>
		(
			SettingsEnums::TriggerLabelStyle::Center,
			[](SettingsEnums::TriggerLabelStyle in) { return true; },
			nameof(triggerOverlayLabelStyle)
		);



	std::shared_ptr<BinarySetting<float>> triggerOverlayLabelScale = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](float in) { return in > 6.f; },
			nameof(triggerOverlayLabelScale)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlayNormalColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.0f, 0.0f, 0.0f, 0.25f}, // red
			[](SimpleMath::Vector4 in) { return  in.w >= 0.05f; },
			nameof(triggerOverlayNormalColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlayBSPColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 0.0f, 1.0f, 0.25f}, // blue
			[](SimpleMath::Vector4 in) { return  in.w >= 0.05f; },
			nameof(triggerOverlayBSPColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlaySectorColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.0f, 0.0f, 1.0f, 0.25f}, // purple
			[](SimpleMath::Vector4 in) { return  in.w >= 0.05f; },
			nameof(triggerOverlaySectorColor)
		);

	std::shared_ptr<BinarySetting<float>> triggerOverlayAlpha = std::make_shared<BinarySetting<float>>
		(
			0.4f,
			[](float in) { return in >= 0.05f && in <= 1.f; },
			nameof(triggerOverlayAlpha)
		);

	std::shared_ptr<BinarySetting<float>> triggerOverlayWireframeAlpha = std::make_shared<BinarySetting<float>>
		(
			1.0f,
			[](float in) { return in >= 0.05f && in <= 1.f; },
			nameof(triggerOverlayWireframeAlpha)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayCheckHitToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayCheckHitToggle)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayCheckMissToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayCheckMissToggle)
		);

	std::shared_ptr<BinarySetting<int>> triggerOverlayCheckHitFalloff = std::make_shared<BinarySetting<int>>
		(
			60,
			[](int in) { return in >= 0; },
			nameof(triggerOverlayCheckHitFalloff)
		);

	std::shared_ptr<BinarySetting<int>> triggerOverlayCheckMissFalloff = std::make_shared<BinarySetting<int>>
		(
			15,
			[](int in) { return in >= 0; },
			nameof(triggerOverlayCheckMissFalloff)
		);



	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlayCheckHitColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 1.0f, 0.0f, 1.f}, // green
			[](SimpleMath::Vector4 in) { return  in.w >= 0.05f; },
			nameof(triggerOverlayCheckHitColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlayCheckMissColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.0f, 1.0f, 0.0f, 1.0f}, // yellow
			[](SimpleMath::Vector4 in) { return  in.w >= 0.05f; }, 
			nameof(triggerOverlayCheckMissColor)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayMessageOnEnter = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayMessageOnEnter)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayMessageOnExit = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayMessageOnExit)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayMessageOnCheckHit = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayMessageOnCheckHit)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayMessageOnCheckMiss = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(triggerOverlayMessageOnCheckMiss)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayPositionToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(triggerOverlayPositionToggle)
		);



	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> triggerOverlayPositionColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 1.0f, 0.0f, 1.f}, // green
			[](SimpleMath::Vector4 in) { return in.w == 1.f; },
			nameof(triggerOverlayPositionColor)
		);

	std::shared_ptr<BinarySetting<float>> triggerOverlayPositionScale = std::make_shared<BinarySetting<float>>
		(
			0.1f,
			[](float in) { return in > 0.f; },
			nameof(triggerOverlayPositionScale)
		);

	std::shared_ptr<BinarySetting<bool>> triggerOverlayPositionWireframe = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(triggerOverlayPositionWireframe)
		);

	std::shared_ptr<BinarySetting<bool>> shieldInputPrinterToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(shieldInputPrinterToggle)
		);


	std::shared_ptr<BinarySetting<bool>> softCeilingOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(softCeilingOverlayToggle)
		);

	// Placement Points Overlay (Halo 3 respawn / vehicle-exit candidate visualisation)
	std::shared_ptr<BinarySetting<bool>> placementPointsOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(placementPointsOverlayToggle)
		);
	std::shared_ptr<BinarySetting<bool>> placementPointsOverlayExtended = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(placementPointsOverlayExtended)
		);
	std::shared_ptr<BinarySetting<bool>> placementPointsOverlayShowValidity = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(placementPointsOverlayShowValidity)
		);
	std::shared_ptr<BinarySetting<float>> placementPointsOverlayRadius = std::make_shared<BinarySetting<float>>
		(
			1.0f, // verified live: the respawn search uses radii 1.0/2.0/4.0 (base 1.0 = 2x biped radius)
			[](float in) { return in >= 0.f; },
			nameof(placementPointsOverlayRadius)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::SoftCeilingRenderTypes>> softCeilingOverlayRenderTypes = std::make_shared<BinarySetting<SettingsEnums::SoftCeilingRenderTypes>>
		(
			SettingsEnums::SoftCeilingRenderTypes::BipedsOrVehicles,
			[](SettingsEnums::SoftCeilingRenderTypes in) { return true; },
			nameof(softCeilingOverlayRenderTypes)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::SoftCeilingRenderDirection>> softCeilingOverlayRenderDirection = std::make_shared<BinarySetting<SettingsEnums::SoftCeilingRenderDirection>>
		(
			SettingsEnums::SoftCeilingRenderDirection::Both,
			[](SettingsEnums::SoftCeilingRenderDirection in) { return true; },
			nameof(softCeilingOverlayRenderDirection)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> softCeilingOverlayColorAccel = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 1.0f, 0.0f, 1.f}, // green
			[](SimpleMath::Vector4 in) { return in.w == 1.f; },
			nameof(softCeilingOverlayColorAccel)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> softCeilingOverlayColorSlippy = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.0f, 1.0f, 0.0f, 1.f}, // yellow
			[](SimpleMath::Vector4 in) { return in.w == 1.f; },
			nameof(softCeilingOverlayColorSlippy)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> softCeilingOverlayColorKill = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.0f, 0.0f, 0.0f, 1.f}, // red
			[](SimpleMath::Vector4 in) { return in.w == 1.f; },
			nameof(softCeilingOverlayColorKill)
		);

	std::shared_ptr<BinarySetting<float>> softCeilingOverlaySolidTransparency = std::make_shared<BinarySetting<float>>
		(
			0.3f,
			[](float in) { return true; },
			nameof(softCeilingOverlaySolidTransparency)
		);

	std::shared_ptr<BinarySetting<float>> softCeilingOverlayWireframeTransparency = std::make_shared<BinarySetting<float>>
		(
			0.6f,
			[](float in) { return true; },
			nameof(softCeilingOverlayWireframeTransparency)
		);

	std::shared_ptr<BinarySetting<bool>> disableBarriersToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(disableBarriersToggle)
		);

	std::shared_ptr<BinarySetting<float>> renderDistance3D = std::make_shared<BinarySetting<float>>
		(
			1000.f,
			[](float in) { return in >= 0.1f; }, 
			nameof(renderDistance3D)
		); 

	std::shared_ptr<BinarySetting<bool>> abilityMeterOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(abilityMeterOverlayToggle)
		);

	
		std::shared_ptr<BinarySetting<bool>> abilityMeterHeroAssistToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(abilityMeterHeroAssistToggle)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::ScreenAnchorEnum>> abilityMeterAbilityAnchorCorner = std::make_shared<BinarySetting<SettingsEnums::ScreenAnchorEnum>>
		(
			SettingsEnums::ScreenAnchorEnum::BottomRight,
			[](SettingsEnums::ScreenAnchorEnum in) { return true; },
			nameof(abilityMeterAbilityAnchorCorner)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> abilityMeterAbilityScreenOffset = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{800, 800},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(abilityMeterAbilityScreenOffset)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> abilityMeterAbilitySize = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{400, 40},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(abilityMeterAbilitySize)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterAbilityBackgroundColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 0.0f, 0.0f, 0.f}, // transparent
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterAbilityBackgroundColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterAbilityForegroundColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.3f, 0.3f, 1.0f, 0.7f}, // blue 
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterAbilityForegroundColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterAbilityHighlightColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.4f, 0.7f, 1.0f, 1.f}, // blue but more opaque
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterAbilityHighlightColor)
		);

	std::shared_ptr<BinarySetting<bool>> abilityMeterCooldownToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(abilityMeterCooldownToggle)
		);

	std::shared_ptr<BinarySetting<SettingsEnums::ScreenAnchorEnum>> abilityMeterCooldownAnchorCorner = std::make_shared<BinarySetting<SettingsEnums::ScreenAnchorEnum>>
		(
			SettingsEnums::ScreenAnchorEnum::BottomRight,
			[](SettingsEnums::ScreenAnchorEnum in) { return true; },
			nameof(abilityMeterCooldownAnchorCorner)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> abilityMeterCooldownScreenOffset = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{800, 750},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(abilityMeterCooldownScreenOffset)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> abilityMeterCooldownSize = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{400, 20},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(abilityMeterCooldownSize)
		);


	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterCooldownBackgroundColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.0f, 0.0f, 0.0f, 0.f}, // transparent
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterCooldownBackgroundColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterCooldownForegroundColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.3f, 0.3f, 1.0f, 0.7f}, // blue 
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterCooldownForegroundColor)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> abilityMeterCooldownHighlightColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.4f, 0.7f, 1.0f, 1.f}, // blue but lighter and more opaque
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(abilityMeterCooldownHighlightColor)
		);



	std::shared_ptr<BinarySetting<float>> soundClassGainDialog = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainDialog)
		);

	std::shared_ptr<BinarySetting<float>> soundClassGainAmbience = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainAmbience)
		);

	std::shared_ptr<BinarySetting<float>> soundClassGainWeapons = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainWeapons)
		);

	std::shared_ptr<BinarySetting<float>> soundClassGainVehicles = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainVehicles)
		);

	std::shared_ptr<BinarySetting<float>> soundClassGainMusic = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainMusic)
		);

	std::shared_ptr<BinarySetting<float>> soundClassGainOther = std::make_shared<BinarySetting<float>>
		(
			1.f,
			[](float in) { return in >= 0.f && in <= 1.f; },
			nameof(soundClassGainOther)
		);

	std::shared_ptr<BinarySetting<bool>> soundClassGainAdjusterToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(soundClassGainAdjusterToggle)
		);

	std::shared_ptr<BinarySetting<bool>> changeOOBBackgroundToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(changeOOBBackgroundToggle)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> changeOOBBackgroundColor = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{0.235f, 0.106f, 0.553f, 1.f}, // default value used by halo 3
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(changeOOBBackgroundColor)
		);


	std::shared_ptr<BinarySetting<bool>> disableFogToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(disableFogToggle)
		);

	

		std::shared_ptr<BinarySetting<bool>> sensDriftOverlayToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensDriftOverlayToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensOverDotFrameToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(sensOverDotFrameToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensMessageOnOverDotToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensMessageOnOverDotToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensSoundOnOverDotToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensSoundOnOverDotToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensSubpixelDriftToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(sensSubpixelDriftFrameToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensMessageOnSubpixelDriftToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensMessageOnSubpixelDriftToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensSoundOnSubpixelDriftToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensSoundOnSubpixelDriftToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensSubpixelDriftSciNotationToggle = std::make_shared<BinarySetting<bool>>
		(
			false,
			[](bool in) { return true; },
			nameof(sensSubpixelDriftSciNotationToggle)
		);

	std::shared_ptr<BinarySetting<bool>> sensCountTurnsToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(sensCountTurnsToggle)
		);

	

	std::shared_ptr<BinarySetting<bool>> sensResetCountOnRevertToggle = std::make_shared<BinarySetting<bool>>
		(
			true,
			[](bool in) { return true; },
			nameof(sensResetCountOnRevertToggle)
		);


	std::shared_ptr<BinarySetting<SettingsEnums::ScreenAnchorEnum>> sensAnchorCorner = std::make_shared<BinarySetting<SettingsEnums::ScreenAnchorEnum>>
		(
			SettingsEnums::ScreenAnchorEnum::BottomRight,
			[](SettingsEnums::ScreenAnchorEnum in) { return true; },
			nameof(sensAnchorCorner)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector2>> sensScreenOffset = std::make_shared<BinarySetting<SimpleMath::Vector2>>
		(
			SimpleMath::Vector2{400, 400},
			[](SimpleMath::Vector2 in) { return in.x >= 0 && in.y >= 0; }, // no negative offsets
			nameof(sensScreenOffset)
		);

	std::shared_ptr<BinarySetting<float>> sensFontSize = std::make_shared<BinarySetting<float>>
		(
			16.f,
			[](int in) { return in > 6.f; },
			nameof(sensFontSize)
		);

	std::shared_ptr<BinarySetting<SimpleMath::Vector4>> sensFontColour = std::make_shared<BinarySetting<SimpleMath::Vector4>>
		(
			SimpleMath::Vector4{1.00f, 0.60f, 0.25f, 1.00f}, // a nice orange colour that matches the rest of the gui
			[](SimpleMath::Vector4 in) { return in.x >= 0 && in.y >= 0 && in.z >= 0 && in.w >= 0 && in.x <= 1 && in.y <= 1 && in.z <= 1 && in.w <= 1; }, // range 0.f ... 1.f 
			nameof(sensFontColour)
		);



	// settings that ought to be serialised/deserialised between HCM runs
	std::vector<std::shared_ptr<SerialisableSetting>> allSerialisableOptions
	{
		changeOOBBackgroundColor,
		soundClassGainDialog,
			soundClassGainAmbience,
			soundClassGainWeapons,
			soundClassGainVehicles,
			soundClassGainMusic,
			soundClassGainOther,



			sensOverDotFrameToggle,
			sensMessageOnOverDotToggle,
			sensSoundOnOverDotToggle,
			sensSubpixelDriftToggle,
			sensMessageOnSubpixelDriftToggle,
			sensSoundOnSubpixelDriftToggle,
			sensSubpixelDriftSciNotationToggle,
			sensCountTurnsToggle,
			sensResetCountOnRevertToggle,
			sensAnchorCorner,
			sensScreenOffset,
			sensFontSize,
			sensFontColour,


			abilityMeterAbilityAnchorCorner,
			abilityMeterAbilityScreenOffset,
			abilityMeterAbilitySize,
			abilityMeterAbilityBackgroundColor,
			abilityMeterAbilityForegroundColor,
			abilityMeterAbilityHighlightColor,
			abilityMeterCooldownToggle,
			abilityMeterCooldownAnchorCorner,
			abilityMeterCooldownScreenOffset,
			abilityMeterCooldownSize,
			abilityMeterCooldownBackgroundColor,
			abilityMeterCooldownForegroundColor,
			abilityMeterCooldownHighlightColor,
		renderDistance3D,
		messagesFontSize,
			messagesFontColor,
		triggerOverlayFilterToggle,
		triggerOverlayFilterString,
		triggerOverlayRenderStyle,
		triggerOverlayInteriorStyle,
			triggerOverlayLabelStyle,
			triggerOverlayLabelScale,
		triggerOverlayNormalColor,
		triggerOverlayBSPColor,
			triggerOverlaySectorColor,
		triggerOverlayAlpha,
			triggerOverlayWireframeAlpha,
		triggerOverlayCheckHitToggle,
		triggerOverlayCheckMissToggle,
		triggerOverlayCheckHitFalloff,
		triggerOverlayCheckMissFalloff,
		triggerOverlayCheckHitColor,
		triggerOverlayCheckMissColor,
		triggerOverlayMessageOnEnter,
		triggerOverlayMessageOnExit,
		triggerOverlayMessageOnCheckHit,
		triggerOverlayMessageOnCheckMiss,
			triggerOverlayPositionToggle,
			triggerOverlayPositionColor,
			triggerOverlayPositionScale,
			triggerOverlayPositionWireframe,
			softCeilingOverlayRenderTypes,
			softCeilingOverlayColorAccel,
			softCeilingOverlayColorSlippy,
			softCeilingOverlayColorKill,
			softCeilingOverlaySolidTransparency,
			softCeilingOverlayWireframeTransparency,
		hideWatermarkHideMessages,
		advanceTicksCount,
		injectionIgnoresChecksum,
		injectCheckpointForcesRevert,
		injectCheckpointLevelCheck,
		injectCheckpointVersionCheck,
			injectCheckpointDifficultyCheck,
		autonameCheckpoints,
		dumpCheckpointForcesSave,
		hceShadowCheckpoints,
			injectCoreForcesRevert,
		injectCoreLevelCheck,
		injectCoreVersionCheck,
			injectCoreDifficultyCheck,
		autonameCoresaves,
		dumpCoreForcesSave,
		forceFutureCheckpointTick,
		invulnerabilityNPCToggle, 
		speedhackSetting, 
		GUIShowingFreesCursor, 
		GUIShowingBlocksInput, 
		GUIShowingPausesGame, 
		pauseAlsoBlocksInput,
		pauseAlsoFreesCursor,
		getObjectAddressDWORD,
				getTagAddressDWORD,
		forceTeleportApplyToPlayer,
		forceTeleportCustomObject,
		forceTeleportAbsoluteVec3,
		forceTeleportRelativeVec3,
		forceTeleportForward,
		forceTeleportForwardIgnoreZ,
		forceTeleportManual,
		forceLaunchApplyToPlayer,
		forceLaunchCustomObject,
		forceLaunchAbsoluteVec3,
		forceLaunchRelativeVec3,
		forceLaunchForward,
		forceLaunchForwardIgnoreZ,
		forceLaunchManual,
		display2DInfoShowGameTick,
		display2DInfoShowAggro,
		display2DInfoShowRNG,
		display2DInfoShowBSP,
			display2DInfoShowBSPSet,
		display2DInfoShowNextObjectDatum,
		display2DInfoTrackPlayer,
		display2DInfoShowPlayerViewAngle,
		display2DInfoShowPlayerViewAngleID,
		display2DInfoShowPlayerPosition,
		display2DInfoShowPlayerVelocity,
		display2DInfoShowPlayerVelocityAbs,
		display2DInfoShowPlayerVelocityXY,
		display2DInfoShowPlayerVelocityXYZ,
		display2DInfoShowPlayerHealth,
		display2DInfoShowPlayerRechargeCooldown,
		display2DInfoShowPlayerVehicleHealth,
		display2DInfoTrackCustomObject,
		display2DInfoCustomObjectDatum,
		display2DInfoShowEntityObjectType,
		display2DInfoShowEntityTagName,
		display2DInfoShowEntityPosition,
		display2DInfoShowEntityVelocity,
		display2DInfoShowEntityVelocityAbs,
		display2DInfoShowEntityVelocityXY,
		display2DInfoShowEntityVelocityXYZ,
		display2DInfoShowEntityHealth,
		display2DInfoShowEntityRechargeCooldown,
		display2DInfoShowEntityVehicleHealth,
		display2DInfoAnchorCorner,
		display2DInfoScreenOffset,
		display2DInfoFontSize,
		display2DInfoFontColour,
		display2DInfoFloatPrecision,
		display2DInfoOutline,
		hceDisplayInfoShowCoordinates,
		hceDisplayInfoShowVelocity,
		hceDisplayInfoShowLevel,
		hceDisplayInfoShowBSP,
		hceDisplayInfoShowTick,
		hceDisplayInfoShowPlayerDatum,
		hceDisplayInfoShowTEB,
		hceTriggerOverlayRenderDistance,
		hceTriggerOverlayShowLabels,
		hceSkyFixToggle,
		hceDisableFadeFromBlackToggle,
		hceTriggerOverlayShowVertex,
		hceTriggerOverlayHighlightActive,
		hceTriggerOverlayActiveColor,
		hceTriggerOverlayLabelColor,
		hceTriggerOverlayBspColor,
		hceTriggerOverlayKillColor,
		hceTriggerOverlaySafeZoneColor,
		hceTriggerOverlaySpeedrunOnly,
		hceTriggerOverlayShowRegular,
		hceTriggerOverlayShowSector,
		hceTriggerOverlayShowKill,
		hceTriggerOverlayShowZoneSet,
		// ⚠ hceBspOverlayToggle is DELIBERATELY ABSENT from this list. Everything here is written to
		// HCMInternalConfig.xml and read back at startup, so listing the toggle would make the overlay
		// re-arm itself every launch just because it was on when HCM last closed. A visualiser that turns
		// itself on unasked is a surprise, and this one is expensive (it walks every structure BSP). The
		// COLOURS and styles below are still persisted - it is only the on/off that resets.
		hceBspOverlayInvisibleOnly,
		hceBspOverlayRenderStyle,
		hceBspOverlayRenderDistance,
		hceBspOverlayColor,
		hceBspOverlayAlpha,
		hceBspOverlayWireframeAlpha,
		hceBspOverlayInteriorStyle,
		hceBspOverlayInsideColor,
		hceBspOverlayInsideAlpha,
		hceBspOverlayWireframeColor,
		hceBspOverlayFaceShading,
		hceBspOverlayOccludeFarSurfaces,
		hceBspOverlayPatternContrast,
		hceBspOverlayPatternScale,
		hceBspOverlayLayerCompensation,
		hceBspOverlayShadingStrength,
		hceBspOverlaySurfaceVariation,
		editPlayerViewAngleVec2,
		editPlayerViewAngleAdjustFactor,
		editPlayerViewAngleIDInt,
		editPlayerViewAngleIDAdjustFactor,
		freeCameraThirdPersonRendering,
		freeCameraGameInputDisable,
		//freeCameraCameraInputDisable,
		freeCameraUserInputCameraTranslationSpeedChangeFactor,
		freeCameraUserInputCameraSetPositionVec3,
		freeCameraUserInputCameraSetRotationVec3,
		freeCameraUserInputCameraMaintainVelocity,
		freeCameraUserInputCameraSetVelocityVec3,
		freeCameraUserInputCameraBaseFOV,
		freeCameraUserInputCameraTranslationSpeed,
		freeCameraUserInputCameraTranslationInterpolator,
		freeCameraUserInputCameraTranslationInterpolatorLinearFactor,
		freeCameraUserInputCameraRotationSpeed,
		freeCameraUserInputCameraRotationInterpolator,
		freeCameraUserInputCameraRotationInterpolatorLinearFactor,
		freeCameraUserInputCameraFOVSpeed,
		freeCameraUserInputCameraFOVInterpolator,
		freeCameraUserInputCameraFOVInterpolatorLinearFactor,
		freeCameraTeleportToCameraSlightlyBehind,
		//freeCameraAnchorPositionToObjectPosition,
		freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup,
		freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum,
		freeCameraAnchorPositionToObjectPositionTranslationInterpolator,
		freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor,
		//freeCameraAnchorPositionToObjectRotation,
		//freeCameraAnchorRotationToObjectPosition,
		freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup,
		freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum,
		freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3,
		freeCameraAnchorRotationToObjectPositionRotationInterpolator,
		freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor,
		//freeCameraAnchorRotationToObjectFacing,
		freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup,
		freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum,
		freeCameraAnchorRotationToObjectFacingRotationInterpolator,
		freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor,
		//freeCameraAnchorFOVToObjectDistance,
		freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup,
		freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum,
		freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3,
		freeCameraAnchorFOVToObjectDistanceFOVInterpolator,
		freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor,
		freeCameraUserInputCameraRotationScalesToFOV,
		freeCameraUserInputCameraNonLinearFOVAtMinimum,
		freeCameraUserInputCameraNonLinearFOVAtMaximum,
		switchBSPIndex,
		switchBSPSetLoadSet,
		switchBSPSetLoadIndex,
		switchBSPSetUnloadIndex,
		setPlayerHealthVec2,
		waypoint3DClampToggle,
		waypoint3DGlobalSpriteScale,
		waypoint3DGlobalLabelScale,
		waypoint3DGlobalDistanceScale,
		waypoint3DGlobalDistancePrecision,
		waypoint3DGlobalSpriteColor,
		waypoint3DGlobalLabelColor,
		waypoint3DGlobalDistanceColor,
		waypoint3DList,
		viewAngleLine3DGlobalLabelScale,
		viewAngleLine3DGlobalDistanceScale,
		viewAngleLine3DGlobalDistancePrecision,
		viewAngleLine3DGlobalSpriteColor,
		viewAngleLine3DGlobalLabelColor,
		viewAngleLine3DGlobalDistanceColor,
		viewAngleLine3DList,
		consoleCommandFontSize,
		consoleCommandFreeCursor,
		consoleCommandBlockInput,
		consoleCommandPauseGame,

	};

	// MUST be the last member: disarms the preset collector once every setting above has self-registered into
	// allPresetOptions. Any BinarySetting declared AFTER this line would be missing from presets.
	struct PresetCollectorDisarmer { PresetCollectorDisarmer() { SerialisableSetting::s_presetCollector = nullptr; } };
	PresetCollectorDisarmer mPresetCollectorDisarmer;

};

