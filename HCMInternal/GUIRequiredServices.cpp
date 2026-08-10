#include "pch.h"
#include "GUIRequiredServices.h"


// ALL GUI ELEMENTS, nested or not, are allowed to go in here - but if they have no required services, you don't have to
const std::map <GUIElementEnum, std::vector<OptionalCheatEnum>> GUIRequiredServices::requiredServicesPerGUIElement =
{
	{GUIElementEnum::GUIShowingPausesGame,
			{OptionalCheatEnum::TogglePause}
	},
	{GUIElementEnum::GUIShowingFreesCursor,
			{OptionalCheatEnum::ToggleFreeCursor}
	},
	{GUIElementEnum::GUIShowingBlocksInput,
			{OptionalCheatEnum::ToggleBlockInput}
	},
	// Above elements don't directly use the optionalCheat but rather the control service container directly. 
	// But the optionalCheats only successfully construct when the control services do.

	{GUIElementEnum::togglePauseGUI,
			{OptionalCheatEnum::TogglePause}
	},
	{GUIElementEnum::advanceTicksGUI,
			{OptionalCheatEnum::AdvanceTicks, OptionalCheatEnum::TogglePause}
	},
	{GUIElementEnum::pauseAlsoFreesCursorGUI,
			{OptionalCheatEnum::TogglePause, OptionalCheatEnum::ToggleFreeCursor}
	},
	{GUIElementEnum::pauseAlsoBlocksInputGUI,
			{OptionalCheatEnum::TogglePause, OptionalCheatEnum::ToggleBlockInput}
	},
		{GUIElementEnum::OBSBypassToggleGUI,
			{OptionalCheatEnum::OBSBypassCheck}
	},

			{GUIElementEnum::HideWatermarkGUI,
			{OptionalCheatEnum::HideWatermarkCheck}
	},

		{GUIElementEnum::presetSaveButton,
			{OptionalCheatEnum::PresetManager}
	},
		{GUIElementEnum::presetLoadButton,
			{OptionalCheatEnum::PresetManager}
	},

		{GUIElementEnum::disableBarriersToggle,
			{OptionalCheatEnum::DisableBarriers}
	},

	{GUIElementEnum::forceCheckpointGUI,
			{OptionalCheatEnum::ForceCheckpoint}
	},
	// Halo Campaign Evolved has no simple "force checkpoint" flag to write - forcing one needs three code hooks,
	// and those same hooks are what implements "disable natural checkpoints". So both HaloCER-only elements below
	// share the single HCECheckpointDetours cheat (getOrMakeCheat caches per game+cheat pair, so only one is made).
	{GUIElementEnum::hceForceCheckpointGUI,
			{OptionalCheatEnum::HCECheckpointDetours}
	},
	{GUIElementEnum::hceNaturalCheckpointDisableGUI,
			{OptionalCheatEnum::HCECheckpointDetours}
	},
	{GUIElementEnum::hceDisableFadeFromBlackGUI,
			{OptionalCheatEnum::HCEDisableFadeFromBlack}
	},
	{GUIElementEnum::hceSkyFixGUI,
			{OptionalCheatEnum::HCESkyFix}
	},
	{GUIElementEnum::hceFreecamNoclipGUI,
			{OptionalCheatEnum::HCEFreecamExtras}
	},
	{GUIElementEnum::hceFreecamGimbalBypassGUI,
			{OptionalCheatEnum::HCEFreecamExtras}
	},
	{GUIElementEnum::forceRevertGUI,
			{OptionalCheatEnum::ForceRevert}
	},
	{GUIElementEnum::forceDoubleRevertGUI,
			{OptionalCheatEnum::ForceDoubleRevert} 
	},
	{GUIElementEnum::forceCoreSaveGUI,
			{OptionalCheatEnum::ForceCoreSave} 
	},
	{GUIElementEnum::forceCoreLoadGUI,
			{OptionalCheatEnum::ForceCoreLoad} 
	},

	{GUIElementEnum::injectCheckpointGUI,
			{OptionalCheatEnum::InjectCheckpoint} 
	},
	{GUIElementEnum::injectCheckpointForcesRevert,
			{OptionalCheatEnum::InjectCheckpoint, OptionalCheatEnum::ForceRevert}
	},
	{GUIElementEnum::injectCheckpointLevelCheck,
			{OptionalCheatEnum::InjectCheckpoint, OptionalCheatEnum::GetCurrentLevelCode}
	},
	{GUIElementEnum::injectCheckpointVersionCheck,
			{OptionalCheatEnum::InjectCheckpoint}
	},
	{GUIElementEnum::injectCheckpointDifficultyCheck,
			{OptionalCheatEnum::InjectCheckpoint, OptionalCheatEnum::GetCurrentDifficulty}
	},
	{GUIElementEnum::injectCheckpointIgnoresChecksum,
			{OptionalCheatEnum::InjectCheckpoint, OptionalCheatEnum::ForceRevert, OptionalCheatEnum::IgnoreCheckpointChecksum}
	},
	{GUIElementEnum::dumpCheckpointGUI,
			{OptionalCheatEnum::DumpCheckpoint}
	},
	// Halo Campaign Evolved's dump is its own cheat: MCC's DumpCheckpoint needs an IGetMCCVersion version stamp
	// HaloCER has none of (and must never carry), plus a checkpointLocation pointer into an in-process buffer the
	// shipped game does not have. It DOES use ISharedMemory::getDumpInfo now that HCMExternal has a HaloCER tab -
	// dumps go to the save folder that tab has selected, falling back to <HCM dir>\HaloCER Checkpoints\.
	// See HCEDumpCheckpoint.h.
	// Both HaloCER dump and inject additionally need HCECheckpointDetours: the shipped game keeps its checkpoints
	// OUTSIDE this process, so the only way to reach a blob is to intercept the hand-off to the storage provider
	// (and force a checkpoint so that hand-off happens). Both resolve it as a dependency anyway; listing it here
	// keeps the requirement visible and construction order sane.
	{GUIElementEnum::hceDumpCheckpointGUI,
			{OptionalCheatEnum::HCEDumpCheckpoint, OptionalCheatEnum::HCECheckpointDetours}
	},
	{GUIElementEnum::hceDumpCheckpointAutonameGUI,
			{OptionalCheatEnum::HCEDumpCheckpoint}
	},
	// The shadow toggle is owned by HCECheckpointDetours (it is the thing that installs the hand-off hook and holds
	// the buffer), but it is meaningless without the dump that consumes it, so both are required.
	{GUIElementEnum::hceDumpCheckpointShadowGUI,
			{OptionalCheatEnum::HCEDumpCheckpoint, OptionalCheatEnum::HCECheckpointDetours}
	},
	// ... and its injection, for the same reasons. It reads ISharedMemory::getInjectInfo too - the checkpoint
	// selected on HCMExternal's HaloCER tab - and falls back to a file dialog when there is no usable selection.
	// See HCEInjectCheckpoint.h.
	{GUIElementEnum::hceInjectCheckpointGUI,
			{OptionalCheatEnum::HCEInjectCheckpoint, OptionalCheatEnum::HCECheckpointDetours}
	},
	{GUIElementEnum::hceInjectCheckpointLevelCheck,
			{OptionalCheatEnum::HCEInjectCheckpoint}
	},
	{GUIElementEnum::hceInjectCheckpointDifficultyCheck,
			{OptionalCheatEnum::HCEInjectCheckpoint}
	},
	{GUIElementEnum::hceInjectCheckpointVersionCheck,
			{OptionalCheatEnum::HCEInjectCheckpoint}
	},
	{GUIElementEnum::hceInjectCheckpointRewriteIdentity,
			{OptionalCheatEnum::HCEInjectCheckpoint}
	},
	// The revert is a separate cheat and this toggle fires its event, so it needs both - exactly like MCC's
	// injectCheckpointForcesRevert.
	{GUIElementEnum::hceInjectCheckpointForcesRevert,
			{OptionalCheatEnum::HCEInjectCheckpoint, OptionalCheatEnum::ForceRevert}
	},
	{GUIElementEnum::dumpCheckpointAutonameGUI,
			{OptionalCheatEnum::DumpCheckpoint}
	},
	{GUIElementEnum::dumpCheckpointForcesSave,
			{OptionalCheatEnum::DumpCheckpoint, OptionalCheatEnum::ForceCheckpoint}
	},
	{GUIElementEnum::injectCoreGUI,
			{OptionalCheatEnum::InjectCore} 
	},
	{GUIElementEnum::injectCoreForcesRevert,
			{OptionalCheatEnum::InjectCore, OptionalCheatEnum::ForceCoreLoad}
	},
	{GUIElementEnum::injectCoreLevelCheck,
			{OptionalCheatEnum::InjectCore, OptionalCheatEnum::GetCurrentLevelCode}
	},
	{GUIElementEnum::injectCoreVersionCheck,
			{OptionalCheatEnum::InjectCore}
	},
	{GUIElementEnum::injectCoreDifficultyCheck,
			{OptionalCheatEnum::InjectCore, OptionalCheatEnum::GetCurrentDifficulty}
	},
	{GUIElementEnum::dumpCoreGUI,
			{OptionalCheatEnum::DumpCore} 
	},
	{GUIElementEnum::dumpCoreAutonameGUI,
			{OptionalCheatEnum::DumpCore}
	},
		{GUIElementEnum::naturalCheckpointDisableGUI,
			{OptionalCheatEnum::NaturalCheckpointDisable}
	},
	{GUIElementEnum::dumpCoreForcesSave,
			{OptionalCheatEnum::DumpCore, OptionalCheatEnum::ForceCoreSave}
	},

	{ GUIElementEnum::forceFutureCheckpointGUI,
	{OptionalCheatEnum::ForceFutureCheckpoint, OptionalCheatEnum::ForceCheckpoint, OptionalCheatEnum::GameTickEventHook}
	},

	{ GUIElementEnum::forceMissionRestartGUI,
	{OptionalCheatEnum::ForceMissionRestart}
	},

	
	{GUIElementEnum::speedhackGUI,
			{OptionalCheatEnum::Speedhack} 
	},
	{GUIElementEnum::invulnGUI,
			{OptionalCheatEnum::Invulnerability} 
	},
	{ GUIElementEnum::invulnerabilitySettingsSubheading,
	{OptionalCheatEnum::Invulnerability}
	},
	{GUIElementEnum::aiFreezeGUI,
			{OptionalCheatEnum::AIFreeze}
	},
	// Halo Campaign Evolved. This map is keyed by GUIElementEnum ONLY (not by game), so aiFreezeGUI cannot map
	// to AIFreeze on MCC and HCEFreezeAI on HaloCER - hence the parallel hce* element, same as
	// hceForceCheckpointGUI. Every hce* cheat also pulls in HCEGetPlayerState via resolveDependentCheat, so it
	// does not need to be listed here.
	{GUIElementEnum::hceAiFreezeGUI,
			{OptionalCheatEnum::HCEFreezeAI}
	},
	{ GUIElementEnum::medusaGUI,
	{OptionalCheatEnum::Medusa}
	},
	{ GUIElementEnum::forceTeleportGUI,
			{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportSettingsSubheading,
			{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportApplyToPlayer,
			{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportCustomObject,
			{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportSettingsRadioGroup,
			{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportForward,
			{OptionalCheatEnum::ForceTeleport, OptionalCheatEnum::GetPlayerViewAngle}
	},
	{ GUIElementEnum::forceTeleportRelativeVec3,
	{OptionalCheatEnum::ForceTeleport ,OptionalCheatEnum::GetPlayerViewAngle}
	},
	{ GUIElementEnum::forceTeleportForwardIgnoreZ,
	{OptionalCheatEnum::ForceTeleport ,OptionalCheatEnum::GetPlayerViewAngle}
	},
	{ GUIElementEnum::forceTeleportManual,
	{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceTeleportAbsoluteVec3,
{OptionalCheatEnum::ForceTeleport}
	},
	{ GUIElementEnum::forceLaunchGUI,
			{OptionalCheatEnum::ForceLaunch}
	},
	{ GUIElementEnum::forceLaunchSettingsSubheading,
			{OptionalCheatEnum::ForceLaunch}
	},
	{ GUIElementEnum::forceLaunchApplyToPlayer,
			{OptionalCheatEnum::ForceLaunch}
	},
		{ GUIElementEnum::forceLaunchCustomObject,
			{OptionalCheatEnum::ForceLaunch}
	},
		{ GUIElementEnum::forceLaunchSettingsRadioGroup,
			{OptionalCheatEnum::ForceLaunch}
	},
	{ GUIElementEnum::forceLaunchForward,
			{OptionalCheatEnum::ForceLaunch ,OptionalCheatEnum::GetPlayerViewAngle }
	},
		{ GUIElementEnum::forceLaunchRelativeVec3,
			{OptionalCheatEnum::ForceLaunch ,OptionalCheatEnum::GetPlayerViewAngle }
	},
		{ GUIElementEnum::forceLaunchForwardIgnoreZ,
			{OptionalCheatEnum::ForceLaunch ,OptionalCheatEnum::GetPlayerViewAngle }
	},
		{ GUIElementEnum::forceLaunchManual,
			{OptionalCheatEnum::ForceLaunch}
	},
		{ GUIElementEnum::forceLaunchAbsoluteVec3,
			{OptionalCheatEnum::ForceLaunch}
	},

	{ GUIElementEnum::hceForceTeleportGUI,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportSettingsSubheading,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportSettingsRadioGroup,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportForward,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportRelativeVec3,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportForwardIgnoreZ,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportManual,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportAbsoluteVec3,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportAbsoluteFillCurrent,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportAbsoluteCopy,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceTeleportAbsolutePaste,
			{OptionalCheatEnum::HCEForceTeleport}
	},
	{ GUIElementEnum::hceForceLaunchGUI,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchSettingsSubheading,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchSettingsRadioGroup,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchForward,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchRelativeVec3,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchForwardIgnoreZ,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchManual,
			{OptionalCheatEnum::HCEForceLaunch}
	},
	{ GUIElementEnum::hceForceLaunchAbsoluteVec3,
			{OptionalCheatEnum::HCEForceLaunch}
	},

	{ GUIElementEnum::infiniteAmmoGUI,
		{OptionalCheatEnum::InfiniteAmmo}
	},

	{ GUIElementEnum::bottomlessClipGUI,
		{OptionalCheatEnum::BottomlessClip}
	},

	{ GUIElementEnum::season7PhysicsToggle,
		{OptionalCheatEnum::Season7Physics}
	},

	{ GUIElementEnum::dropShadowsOnObjectsToggle,
		{OptionalCheatEnum::DropShadowsOnObjects}
	},

	{ GUIElementEnum::sphereSpecularForceToggle,
		{OptionalCheatEnum::SphereSpecularForce}
	},

	{ GUIElementEnum::offscreenShadowCastersToggle,
		{OptionalCheatEnum::OffscreenShadowCasters}
	},

	{ GUIElementEnum::offscreenShadowCastersMultiplierGUI,
		{OptionalCheatEnum::OffscreenShadowCasters}
	},

	{ GUIElementEnum::uncapDropShadowsToggle,
		{OptionalCheatEnum::UncapDropShadows}
	},

	{ GUIElementEnum::uncapVisibilityLimitsToggle,
		{OptionalCheatEnum::UncapVisibilityLimits}
	},

	{ GUIElementEnum::uncapClusterLimitToggle,
		{OptionalCheatEnum::UncapClusterLimit}
	},

	{ GUIElementEnum::h2ShadowResolutionCombo,
		{OptionalCheatEnum::H2ShadowResolution}
	},

	{ GUIElementEnum::h2ArmorColourToggle,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorColourPrimaryPicker,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorColourSecondaryPicker,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorColourSavePresetButton,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorColourLoadPresetButton,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorEmblemToggle,
		{OptionalCheatEnum::H2ArmorColour}
	},
	{ GUIElementEnum::h2ArmorEmblemLoadButton,
		{OptionalCheatEnum::H2ArmorColour}
	},

	{ GUIElementEnum::farClipDistanceGUI,
		{OptionalCheatEnum::FarClipDistance}
	},

	{ GUIElementEnum::sunScaleFixToggle,
		{OptionalCheatEnum::SunScaleFix}
	},

	{ GUIElementEnum::fpScaleFixToggle,
		{OptionalCheatEnum::FPScaleFix}
	},
	{ GUIElementEnum::fpScaleFixLegSizeGUI,
		{OptionalCheatEnum::FPScaleFix}
	},
	{ GUIElementEnum::fpScaleFixLegTuckGUI,
		{OptionalCheatEnum::FPScaleFix}
	},

	{ GUIElementEnum::animationFixesToggle,
		{OptionalCheatEnum::AnimationFixes}
	},

	{ GUIElementEnum::havokDebuggerGUI,
		{OptionalCheatEnum::HavokDebugger}
	},

	{ GUIElementEnum::masterTickrateToggleGUI,
		{OptionalCheatEnum::MasterTickrate}
	},

	{ GUIElementEnum::masterTickrateCustomGUI,
		{OptionalCheatEnum::MasterTickrate, OptionalCheatEnum::GameTickEventHook}
	},

	{ GUIElementEnum::replayRecord30GUI,
		{OptionalCheatEnum::ReplayRecorder}
	},
	{ GUIElementEnum::replayRecord60GUI,
		{OptionalCheatEnum::ReplayRecorder}
	},
	{ GUIElementEnum::replayStopSaveGUI,
		{OptionalCheatEnum::ReplayRecorder}
	},
	{ GUIElementEnum::replayLoadFileGUI,
		{OptionalCheatEnum::ReplayPlayer}
	},
	{ GUIElementEnum::replayPlayGUI,
		{OptionalCheatEnum::ReplayPlayer}
	},
	{ GUIElementEnum::replayStopPlaybackGUI,
		{OptionalCheatEnum::ReplayPlayer}
	},

		{ GUIElementEnum::playerPositionToClipboardGUI,
		{OptionalCheatEnum::PlayerPositionToClipboard}
	},


		{ GUIElementEnum::triggerOverlayToggle,
		{OptionalCheatEnum::TriggerOverlay}
		},

				{ GUIElementEnum::triggerOverlaySettings,
		{OptionalCheatEnum::TriggerOverlay}
		},

		{ GUIElementEnum::triggerOverlayCheckHitToggle,
		{OptionalCheatEnum::UpdateTriggerLastChecked }
		},

				{ GUIElementEnum::triggerOverlayCheckMissToggle,
		{OptionalCheatEnum::UpdateTriggerLastChecked }
		},


		{ GUIElementEnum::triggerOverlayMessageOnCheckHit,
		{OptionalCheatEnum::UpdateTriggerLastChecked }
		},

	{ GUIElementEnum::triggerOverlayMessageOnCheckMiss,
	{OptionalCheatEnum::UpdateTriggerLastChecked }
	},

		{ GUIElementEnum::triggerOverlayMessageOnEnter,
	{OptionalCheatEnum::TrackTriggerEnterExit }
	},
			{ GUIElementEnum::triggerOverlayMessageOnExit,
	{OptionalCheatEnum::TrackTriggerEnterExit }
	},


		{ GUIElementEnum::triggerOverlayFilterToggle,
	{OptionalCheatEnum::TriggerFilter, OptionalCheatEnum::TriggerFilterModalDialogManager }
	},

			{ GUIElementEnum::triggerOverlayPositionToggle,
	{ OptionalCheatEnum::GetPlayerTriggerPosition }
	},

	{ GUIElementEnum::softCeilingOverlayToggle,
{OptionalCheatEnum::SoftCeilingOverlay}
	},

			{ GUIElementEnum::softCeilingOverlaySettings,
		{OptionalCheatEnum::SoftCeilingOverlay}
	},

	{ GUIElementEnum::placementPointsOverlayToggle,
{OptionalCheatEnum::PlacementPointsOverlay}
	},
	{ GUIElementEnum::placementPointsOverlaySettings,
{OptionalCheatEnum::PlacementPointsOverlay}
	},
	{ GUIElementEnum::placementPointsOverlayExtended,
{OptionalCheatEnum::PlacementPointsOverlay}
	},
	{ GUIElementEnum::placementPointsOverlayShowValidity,
{OptionalCheatEnum::PlacementPointsOverlay}
	},
	{ GUIElementEnum::placementPointsOverlayRadius,
{OptionalCheatEnum::PlacementPointsOverlay}
	},

			{ GUIElementEnum::shieldInputPrinterToggle,
		{OptionalCheatEnum::ShieldInputPrinter,}
		},

					{ GUIElementEnum::sensDriftOverlayToggle,
		{OptionalCheatEnum::SensDriftOverlay,}
		},


	{ GUIElementEnum::sensDriftOverlaySettings,
{OptionalCheatEnum::SensDriftOverlay,}
	},


	{ GUIElementEnum::sensResetCountOnRevertToggle,
{OptionalCheatEnum::RevertEventHook,}
	},

			{ GUIElementEnum::abilityMeterOverlayToggle,
{OptionalCheatEnum::AbilityMeterOverlay}
	},

	{ GUIElementEnum::abilityMeterOverlaySettings,
{OptionalCheatEnum::AbilityMeterOverlay}
},

		{ GUIElementEnum::display2DInfoToggleGUI,
		{OptionalCheatEnum::DisplayPlayerInfo, OptionalCheatEnum::GetPlayerDatum, OptionalCheatEnum::GetObjectAddress}
	},

	{ GUIElementEnum::display2DInfoSettingsInfoSubheading,
{OptionalCheatEnum::DisplayPlayerInfo, OptionalCheatEnum::GetPlayerDatum, OptionalCheatEnum::GetObjectAddress}
	},

		{ GUIElementEnum::display2DInfoSettingsVisualSubheading,
{OptionalCheatEnum::DisplayPlayerInfo, OptionalCheatEnum::GetPlayerDatum, OptionalCheatEnum::GetObjectAddress}
	},

	// Halo Campaign Evolved's own info overlay. Only HCEDisplayInfo - the MCC overlay's GetPlayerDatum /
	// GetObjectAddress dependencies have no HaloCER pointer data and are not used here. The leaf rows are pure
	// settings and need no entry.
	{ GUIElementEnum::hceDisplayInfoToggleGUI,
{OptionalCheatEnum::HCEDisplayInfo}
	},
	{ GUIElementEnum::hceDisplayInfoSettingsInfoSubheading,
{OptionalCheatEnum::HCEDisplayInfo}
	},
	{ GUIElementEnum::hceDisplayInfoSettingsVisualSubheading,
{OptionalCheatEnum::HCEDisplayInfo}
	},



			{ GUIElementEnum::display2DInfoShowAggro,
{ OptionalCheatEnum::GetAggroData}
	},

				{ GUIElementEnum::display2DInfoShowRNG,
{ OptionalCheatEnum::GetCurrentRNG}
	},


		{ GUIElementEnum::display2DInfoShowBSP,
{ OptionalCheatEnum::GetCurrentBSP}
		},

				{ GUIElementEnum::display2DInfoShowNextObjectDatum,
{ OptionalCheatEnum::GetNextObjectDatum}
	},

		{ GUIElementEnum::display2DInfoShowPlayerViewAngle,
{ OptionalCheatEnum::GetPlayerViewAngle}
	},


				{ GUIElementEnum::display2DInfoShowPlayerPosition,
{OptionalCheatEnum::GetObjectPhysics}
	},

					{ GUIElementEnum::display2DInfoShowPlayerVelocity, // don't need to list abs/xy/xyz since they're child elements of this
{ OptionalCheatEnum::GetObjectPhysics}
	},
						{ GUIElementEnum::display2DInfoShowPlayerHealth,
{OptionalCheatEnum::GetObjectHealth}
	},

	{ GUIElementEnum::display2DInfoShowPlayerVehicleHealth,
{OptionalCheatEnum::GetObjectHealth}
	},


	{ GUIElementEnum::display2DInfoShowEntityTagName,
{OptionalCheatEnum::GetObjectTagName}
	},

	{ GUIElementEnum::display2DInfoShowEntityPosition,
{OptionalCheatEnum::GetObjectPhysics}
	},


					{ GUIElementEnum::display2DInfoShowEntityVelocity,
{OptionalCheatEnum::GetObjectPhysics}
	},


						{ GUIElementEnum::display2DInfoShowEntityHealth,
{OptionalCheatEnum::GetObjectHealth}
	},

	{ GUIElementEnum::display2DInfoShowEntityVehicleHealth,
{OptionalCheatEnum::GetObjectHealth}
	},

		{ GUIElementEnum::freeCameraToggleGUI,
{OptionalCheatEnum::FreeCamera}
	},

	// Halo Campaign Evolved freecam. Mirrors the {FreeCamera, ForceTeleport} pairing below - HCEFreecam calls
	// HCEForceTeleport::teleportPlayerTo so the "teleport to camera" row must not appear without it.
	{ GUIElementEnum::hceFreecamToggleGUI,
{OptionalCheatEnum::HCEFreecam}
	},
	{ GUIElementEnum::hceFreecamTeleportToCamera,
{OptionalCheatEnum::HCEFreecam, OptionalCheatEnum::HCEForceTeleport}
	},

	// Halo Campaign Evolved trigger overlay. Parallel hce* elements rather than widening triggerOverlayToggle,
	// because requiredServicesPerGUIElement is keyed by ELEMENT only - reusing the MCC element would map
	// HaloCER onto the MCC TriggerOverlay cheat, which has no HaloCER pointer data at all. The SETTINGS are
	// still shared (HaloCER and MCC can never be the same process), so the leaf rows hang off the same
	// triggerOverlay* settings the MCC overlay uses and need no service entry of their own.
	{ GUIElementEnum::hceTriggerOverlayToggleGUI,
{OptionalCheatEnum::HCETriggerOverlay}
	},
	{ GUIElementEnum::hceTriggerOverlaySettingsSubheading,
{OptionalCheatEnum::HCETriggerOverlay}
	},

	// Structure-BSP overlay. Its own cheat rather than a mode of HCETriggerOverlay: it reads a completely
	// different tag chain (the BSP's raw resource, not the scenario's trigger volume block) and carries its own
	// pointer data, so folding it in would make the trigger overlay fail whenever the BSP path did.
	{ GUIElementEnum::hceBspOverlayToggleGUI,
{OptionalCheatEnum::HCEBspOverlay}
	},
	{ GUIElementEnum::hceBspOverlaySettingsSubheading,
{OptionalCheatEnum::HCEBspOverlay}
	},

	// Soft ceiling overlay. Same reasoning as the two above: the MCC softCeilingOverlayToggle element is keyed
	// to the MCC SoftCeilingOverlay cheat, whose whole data path (GetSoftCeilingData -> TagBlockReader ->
	// GetActiveStructureDesignTags) has no HaloCER implementation, so HaloCER needs its own element pointing at
	// its own cheat. The softCeilingOverlay* SETTINGS and the hotkey are shared, because the tag data is
	// identical in shape - see HCESoftCeilingOverlay.h.
	{ GUIElementEnum::hceSoftCeilingOverlayToggleGUI,
{OptionalCheatEnum::HCESoftCeilingOverlay}
	},
	{ GUIElementEnum::hceSoftCeilingOverlaySettingsSubheading,
{OptionalCheatEnum::HCESoftCeilingOverlay}
	},

	// Same reasoning as the trigger overlay above: the MCC disableBarriersToggle element is keyed to the MCC
	// DisableBarriers cheat (a code patch with no HaloCER pointer data), so HaloCER needs its own element. The
	// disableBarriersToggle SETTING and hotkey are still shared.
	{ GUIElementEnum::hceDisableBarriersGUI,
{OptionalCheatEnum::HCEDisableBarriers}
	},

			{ GUIElementEnum::freeCameraTeleportToCamera,
{OptionalCheatEnum::FreeCamera, OptionalCheatEnum::ForceTeleport}
	},


			{ GUIElementEnum::freeCameraSettingsSimpleSubheading,
{OptionalCheatEnum::FreeCamera}
	},

		{ GUIElementEnum::freeCameraSettingsAdvancedSubheading,
{OptionalCheatEnum::FreeCamera}
		},

			{ GUIElementEnum::freeCameraThirdPersonRendering,
		{OptionalCheatEnum::ThirdPersonRendering}
	},

				{ GUIElementEnum::freeCameraDisableScreenEffects,
		{OptionalCheatEnum::DisableScreenEffects}
	},

	{ GUIElementEnum::freeCameraGameInputDisable,
{OptionalCheatEnum::BlockPlayerCharacterInput}
	},

		{ GUIElementEnum::editPlayerViewAngleSubheading,
{OptionalCheatEnum::EditPlayerViewAngle}
	},

		{ GUIElementEnum::editPlayerViewAngleIDSubheading,
{OptionalCheatEnum::EditPlayerViewAngleID}
		},

			{ GUIElementEnum::switchBSPGUI,
{OptionalCheatEnum::SwitchBSP}
	},

			{ GUIElementEnum::switchBSPSetLoadSet,
{OptionalCheatEnum::SwitchBSPSet}
			},

						{ GUIElementEnum::switchBSPSetFillCurrent,
{OptionalCheatEnum::SwitchBSPSet}
			},

						{ GUIElementEnum::switchBSPSetLoadIndex,
{OptionalCheatEnum::SwitchBSPSet}
			},

		{ GUIElementEnum::switchBSPSetUnloadIndex,
{OptionalCheatEnum::SwitchBSPSet}
		},

				{ GUIElementEnum::hideHUDToggle,
{OptionalCheatEnum::HideHUD}
	},

					{ GUIElementEnum::setPlayerHealthGUI,
{OptionalCheatEnum::SetPlayerHealth}
	},

			{ GUIElementEnum::waypoint3DGUIToggle,
{OptionalCheatEnum::Waypoint3D}
			},

			{ GUIElementEnum::waypoint3DGUIList,
{OptionalCheatEnum::Waypoint3D}
			},

				{ GUIElementEnum::waypoint3DGUISettings,
{OptionalCheatEnum::Waypoint3D}
				},

	{ GUIElementEnum::viewAngleLine3DGUIToggle,
{OptionalCheatEnum::ViewAngle3D}
	},

	{ GUIElementEnum::viewAngleLine3DGUIList,
{OptionalCheatEnum::ViewAngle3D}
	},

	{ GUIElementEnum::viewAngleLine3DGUISettings,
{OptionalCheatEnum::ViewAngle3D}
	},


		{ GUIElementEnum::skullToggleGUI,
{OptionalCheatEnum::SkullToggler}
		},

		{ GUIElementEnum::hceSkullToggleGUI,
{OptionalCheatEnum::HCESkullToggler}
		},

	{ GUIElementEnum::consoleCommandGUI,
{OptionalCheatEnum::CommandConsoleManager}
	},

	{ GUIElementEnum::consoleCommandSettings,
{OptionalCheatEnum::CommandConsoleManager}
	},

					{ GUIElementEnum::consoleCommandOutputEvent,
{OptionalCheatEnum::HaloScriptOutputHookEvent}
					},

{ GUIElementEnum::consoleCommandPauseGame,
{OptionalCheatEnum::TogglePause}
},

{ GUIElementEnum::consoleCommandBlockInput,
{OptionalCheatEnum::ToggleBlockInput}
},

	{ GUIElementEnum::consoleCommandFreeCursor,
{OptionalCheatEnum::ToggleFreeCursor}
	},

		{ GUIElementEnum::getPlayerDatumGUI,
			{OptionalCheatEnum::GetPlayerDatumPresenter}
		},

		{ GUIElementEnum::getPlayerAddressGUI,
			{OptionalCheatEnum::GetPlayerDatumPresenter}
		},


			{ GUIElementEnum::getObjectAddressGUI,
{OptionalCheatEnum::GetObjectAddressCLI}
			},

			{ GUIElementEnum::getTagAddressGUI,
{OptionalCheatEnum::GetTagAddressPresenter}
			},


			{ GUIElementEnum::soundClassGainAdjusterToggle,
{OptionalCheatEnum::SoundClassGain}
			},

				{ GUIElementEnum::soundClassGainAdjusterSettings,
{OptionalCheatEnum::SoundClassGain}
				},


	{ GUIElementEnum::changeOOBBackgroundToggle,
{OptionalCheatEnum::ChangeOOBBackground}
	},

		{ GUIElementEnum::disableFogToggle,
{OptionalCheatEnum::DisableFog}
		},



#ifdef HCM_DEBUG



#endif

};




#define MAKE_MAPPAIR(r, element, i, game) {GUIElementEnum::element, GameState::Value::game},
#define MAKE_PAIRWISE_SET(r, d, seq) 	BOOST_PP_SEQ_FOR_EACH_I(MAKE_MAPPAIR, BOOST_PP_TUPLE_ELEM(0, seq), BOOST_PP_TUPLE_TO_SEQ(BOOST_PP_TUPLE_ELEM(1, seq)))
#define MAKE_ALL_PAIRWISE(seq) BOOST_PP_SEQ_FOR_EACH(MAKE_PAIRWISE_SET, _, seq)



const std::vector<std::pair< GUIElementEnum, GameState>> GUIRequiredServices::toplevelGUIElements =
{
	MAKE_ALL_PAIRWISE(TOPGUIELEMENTS_ANDSUPPORTEDGAMES)
};




#define MAKE_GAMESTATE(s, data, game) GameState::Value::game
#define MAKE_MAPSET(element, games) {GUIElementEnum::element, {BOOST_PP_SEQ_ENUM(BOOST_PP_SEQ_TRANSFORM(MAKE_GAMESTATE, _, games))} },
#define MAKE_MAPSET_SET(r, d, i, seq) 	MAKE_MAPSET(BOOST_PP_TUPLE_ELEM(0, seq), BOOST_PP_TUPLE_TO_SEQ(BOOST_PP_TUPLE_ELEM(1, seq))) 
#define MAKE_ALL_MAPSET(seq) BOOST_PP_SEQ_FOR_EACH_I(MAKE_MAPSET_SET, _, seq) 

const std::map<GUIElementEnum, std::set<GameState>> GUIRequiredServices::supportedGamesPerGUIElement =
{
	MAKE_ALL_MAPSET(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES1)
	MAKE_ALL_MAPSET(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES2)
	MAKE_ALL_MAPSET(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES3)
#ifdef HCM_DEBUG
	MAKE_ALL_MAPSET(DEBUGGUIELEMENTS_ANDSUPPORTEDGAMES)
#endif
};




std::set<std::pair<GameState, OptionalCheatEnum>> outRequiredServices;
bool initRequiredServices = false;
const std::set<std::pair<GameState, OptionalCheatEnum>>& GUIRequiredServices::getAllRequiredServices()
{
	if (initRequiredServices) return outRequiredServices;

	for (auto& [element, cheat] : requiredServicesPerGUIElement)
	{
		if (!supportedGamesPerGUIElement.contains(element)) throw HCMInitException("Somehow supportedGames was missing an element");
		for (auto& game : supportedGamesPerGUIElement.at(element))
		{
			if (!requiredServicesPerGUIElement.contains(element)) continue; // element has no required services
			auto reqServs = requiredServicesPerGUIElement.at(element);

			for (auto req : reqServs)
			{
				outRequiredServices.insert(std::make_pair( game, req ));
			}
		}
	}


	initRequiredServices = true;
	return outRequiredServices;

	//if (initRequiredServices) return outRequiredServices;

	//for (auto& [element, game] : toplevelGUIElements)
	//{
	//		if (!requiredServicesPerGUIElement.contains(element)) continue; // element has no required services

	//		auto reqServs = requiredServicesPerGUIElement.at(element);

	//		for (auto req : reqServs)
	//		{
	//			outRequiredServices.push_back({ game, req });
	//		}
	//}
	//initRequiredServices = true;
	//return outRequiredServices;
};