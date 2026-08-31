#pragma once
#include "pch.h"
#include <boost\preprocessor.hpp>



// game set tuples
#define ALL_SUPPORTED_GAMES Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4
#define ALL_GAMES_AND_MAINMENU Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4, NoGame
// Pause works on HaloCER too (see pauseGameFunction/pauseGameCode in InternalPointerData.xml), but the plain
// ALL_GAMES_AND_MAINMENU list predates HaloCER, so the pause elements were never offered for it and the toggle
// simply did not appear. Widened for those elements only, rather than changing the shared macro.
#define ALL_GAMES_AND_MAINMENU_AND_HALOCER Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4, NoGame, HaloCER
// Halo Campaign Evolved is deliberately NOT in ALL_SUPPORTED_GAMES: it is a separate title with its own
// (almost entirely missing) pointer data, so opting every MCC gui element into it would just produce a few
// hundred failed services. Opt elements in one at a time using these instead.
#define ALL_SUPPORTED_GAMES_AND_HALOCER Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4, HaloCER
#define HALOCER_ONLY HaloCER
// Deliberately narrower than "widen ALL_GAMES_AND_MAINMENU": this is only for the handful of elements that are
// pure HCM plumbing (no game pointer data at all) and that HaloCER genuinely needs. Right now that means the
// Control heading and the "Show optional cheat service failures" button underneath it - without those two,
// a HaloCER-only cheat that fails to construct makes its gui rows silently vanish with the reason visible
// nowhere but the log file. The heading's other children stay MCC-only and each independently return nullopt;
// GUIHeading::render early-outs when every child is nullopt, so this cannot produce an empty heading.
// (ALL_GAMES_AND_MAINMENU_AND_HALOCER is defined once, above - it used to be defined twice, identically.)
#define FREE_CAMERA_SUPPORT Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4
#define THIRD_GEN Halo3, Halo3ODST, HaloReach, Halo4
#define ABILITY_GAMES HaloReach, Halo4

// interpolator macro for freecamera
#define defFreeCameraInterpolator(name)\
((freeCamera##name##Interpolator, (FREE_CAMERA_SUPPORT)))\
((freeCamera##name##InterpolatorLinearFactor, (FREE_CAMERA_SUPPORT)))


// A sequence of pairs, where the first element of a pair is the GUIElementEnum name, and the second element is a tuple of supported games for that guielement
// Indentation is cosmetic but indicates hiearchy of elements
//
// DO NOT put comments inside either sequence below - they are macro bodies, and a comment ends (or silently
// mangles) the continuation. Notes go here instead.
//
// Halo Campaign Evolved: cheatsHeadingGUI / overlaysHeadingGUI / cameraHeadingGUI are ALL_SUPPORTED_GAMES_AND_HALOCER
// so their hce* children can be constructed. That is not an MCC regression: every MCC-only child independently
// returns std::nullopt for HaloCER (see the "does not support Game" early-out in GUIElementConstructor.cpp), and
// GUIHeading sets hasElements = false and render() early-outs when every child is nullopt, so an all-nullopt
// heading draws nothing. Same reasoning as ALL_GAMES_AND_MAINMENU_AND_HALOCER above. Keep the three of them in
// sync with TOPGUIELEMENTS_RELEASE in GUIRequiredServices.h.
//
// requiredServicesPerGUIElement is keyed by GUIElementEnum ONLY, not by game, so an element cannot map to
// AIFreeze on Halo2 and HCEFreezeAI on HaloCER. That is why HaloCER gets parallel hce* elements rather than the
// MCC elements being widened - the same pattern hceForceCheckpointGUI already ships with.
//
// The hce* teleport/launch rows are absolute-coordinates only, so there is no radio group, applyToPlayer or
// customObject row. (The player view angle IS now resolved - see hcePlayerControl* in InternalPointerData.xml
// and HCEGetPlayerState::getPlayerViewAngle, which the trigger overlay uses - so a "relative to look direction"
// mode is possible; it just hasn't been built yet.) They reuse the MCC settings/events/hotkeys because HaloCER
// and the MCC games can never be in one process.
//
// TWO EXCEPTIONS to the "parallel hce* element" rule above, both deliberate:
//   havokDebuggerGUI just gains HaloCER in its own tuple. requiredServicesPerGUIElement maps it to
//   OptionalCheatEnum::HavokDebugger, which IS the correct cheat for HaloCER - HavokDebugger branches to
//   HceHavokDebuggerBridge internally. Same for speedhackGUI -> OptionalCheatEnum::Speedhack, whose mechanism
//   (hooking the timing exports) is engine-agnostic. Widening a tuple also costs no macro-nesting depth.
//   The trigger overlay does NOT qualify: OptionalCheatEnum::TriggerOverlay is MCC-only code, so HaloCER gets
//   parallel hceTriggerOverlay* elements in sequence 3 driving the SAME triggerOverlay* settings.
#define RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES1 \
((presetsHeadingGUI, (ALL_GAMES_AND_MAINMENU)))\
	((presetSaveButton, (ALL_GAMES_AND_MAINMENU)))\
	((presetLoadButton, (ALL_GAMES_AND_MAINMENU)))\
((controlHeadingGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	/* HaloCER: NOT cosmetic. hideWatermarkHideMessages defaults true, so hiding the watermark also hides the
	   message log - without this rebind row a user can erase every visible trace of HCM and have nothing on
	   screen telling them which key brings it back. (The key itself works on CER already.) */\
	((toggleGUIHotkeyGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	((messagesFontSize, (ALL_GAMES_AND_MAINMENU)))\
	((messagesFontColor, (ALL_GAMES_AND_MAINMENU)))\
	((GUISettingsSubheading, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
		((GUIShowingFreesCursor, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
		((GUIShowingBlocksInput, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
		((GUIShowingPausesGame, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	((togglePauseGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	((togglePauseSettingsSubheading, (ALL_GAMES_AND_MAINMENU)))\
		((advanceTicksGUI, (ALL_SUPPORTED_GAMES)))\
		((pauseAlsoFreesCursorGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
		((pauseAlsoBlocksInputGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	((showGUIFailuresGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	/* HaloCER: the bypass works there now - OBS picks d3d11/d3d10/d3d12_capture behind ONE shared Present hook,
	   so D3D12Hook::setOBSBypass does the same job against Present and Present1. The backend was wired before
	   this row was widened, so the feature existed with no way to switch it on. */\
	((OBSBypassToggleGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	/* The suppression itself (HCMInternalGUI.cpp) is backend-agnostic and already works on the D3D12 path;
	   only these rows were gated off. */\
	((HideWatermarkGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
	((HideWatermarkIncludeMessagesGUI, (ALL_GAMES_AND_MAINMENU_AND_HALOCER)))\
((saveManagementHeadingGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((forceCheckpointGUI, (ALL_SUPPORTED_GAMES)))\
	((hceForceCheckpointGUI, (HALOCER_ONLY)))\
	((forceRevertGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((forceDoubleRevertGUI, (Halo2, Halo3, Halo3ODST, HaloReach, Halo4, HaloCER)))\
	((forceCoreSaveGUI, (Halo1)))\
	((forceCoreLoadGUI, (Halo1)))\
	((injectCheckpointGUI, (ALL_SUPPORTED_GAMES)))\
	((injectCheckpointSettingsSubheading, (ALL_SUPPORTED_GAMES)))\
		((injectCheckpointForcesRevert, (ALL_SUPPORTED_GAMES)))\
		((injectCheckpointLevelCheck, (ALL_SUPPORTED_GAMES)))\
		((injectCheckpointVersionCheck, (ALL_SUPPORTED_GAMES)))\
		((injectCheckpointDifficultyCheck, (ALL_SUPPORTED_GAMES)))\
		((injectCheckpointIgnoresChecksum, (Halo1, Halo2, Halo3, Halo3ODST, HaloReach, Halo4)))\
	((dumpCheckpointGUI, (ALL_SUPPORTED_GAMES)))\
	((dumpCheckpointSettingsSubheading, (ALL_SUPPORTED_GAMES)))\
		((dumpCheckpointAutonameGUI, (ALL_SUPPORTED_GAMES)))\
		((dumpCheckpointForcesSave, (ALL_SUPPORTED_GAMES)))\
	((injectCoreGUI, (Halo1)))\
	((injectCoreSettingsSubheading, (Halo1)))\
		((injectCoreForcesRevert, (Halo1)))\
		((injectCoreLevelCheck, (Halo1)))\
		((injectCoreVersionCheck, (Halo1)))\
		((injectCoreDifficultyCheck, (Halo1)))\
	((dumpCoreSettingsSubheading, (Halo1)))\
		((dumpCoreAutonameGUI, (Halo1)))\
		((dumpCoreForcesSave, (Halo1)))\
	((dumpCoreGUI, (Halo1)))\
	((naturalCheckpointDisableGUI, (ALL_SUPPORTED_GAMES)))\
	((hceNaturalCheckpointDisableGUI, (HALOCER_ONLY)))\
	((forceFutureCheckpointGUI, (ALL_SUPPORTED_GAMES)))\
		((forceFutureCheckpointToggle, (ALL_SUPPORTED_GAMES)))\
		((forceFutureCheckpointTick, (ALL_SUPPORTED_GAMES)))\
		((forceFutureCheckpointFill, (ALL_SUPPORTED_GAMES)))\
	((forceMissionRestartGUI, (ALL_SUPPORTED_GAMES)))\
	((replayInputSubheading, (Halo2)))\
		((replayRecord30GUI, (Halo2)))\
		((replayRecord60GUI, (Halo2)))\
		((replayStopSaveGUI, (Halo2)))\
		((replayLoadFileGUI, (Halo2)))\
		((replayPlayGUI, (Halo2)))\
		((replayStopPlaybackGUI, (Halo2)))\
((cheatsHeadingGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((speedhackGUI, (ALL_SUPPORTED_GAMES)))\
	((invulnGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((invulnerabilitySettingsSubheading, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((invulnNPCGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((infiniteAmmoGUI, (Halo1, Halo2)))\
	((bottomlessClipGUI, (Halo1, Halo2)))\
	((season7PhysicsToggle, (Halo2)))\
	((dropShadowsOnObjectsToggle, (Halo2)))\
	((sphereSpecularForceToggle, (Halo2)))\
	((offscreenShadowCastersToggle, (Halo2)))\
	((offscreenShadowCastersMultiplierGUI, (Halo2)))\
	((uncapDropShadowsToggle, (Halo2)))\
	((uncapVisibilityLimitsToggle, (Halo2)))\
	((uncapClusterLimitToggle, (Halo2)))\
	((h2ShadowResolutionCombo, (Halo2)))\
	((h2ArmorColourToggle, (Halo2)))\
	((h2ArmorColourPrimaryPicker, (Halo2)))\
	((h2ArmorColourSecondaryPicker, (Halo2)))\
	((h2ArmorColourSavePresetButton, (Halo2)))\
	((h2ArmorColourLoadPresetButton, (Halo2)))\
	((h2ArmorEmblemToggle, (Halo2)))\
	((h2ArmorEmblemLoadButton, (Halo2)))\
	((farClipDistanceGUI, (Halo2)))\
	((sunScaleFixToggle, (Halo2)))\
	((fpScaleFixToggle, (Halo2)))\
	((fpScaleFixLegSizeGUI, (Halo2)))\
	((fpScaleFixLegTuckGUI, (Halo2)))\
	((animationFixesToggle, (Halo2)))\
	((havokDebuggerGUI, (Halo2, Halo3, Halo3ODST, HaloReach, HaloCER)))\
	((masterTickrateEnableGUI, (Halo2)))\
	((masterTickrateToggleGUI, (Halo2)))\
	((masterTickrateCustomGUI, (Halo2)))\
	((aiFreezeGUI, (ALL_SUPPORTED_GAMES)))\
	((medusaGUI, (Halo1, Halo2)))\
	((forceTeleportGUI, (ALL_SUPPORTED_GAMES)))\
	((forceTeleportSettingsSubheading, (ALL_SUPPORTED_GAMES)))\
		((forceTeleportApplyToPlayer, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportCustomObject, (ALL_SUPPORTED_GAMES)))\
		((forceTeleportSettingsRadioGroup, (ALL_SUPPORTED_GAMES)))\
		((forceTeleportForward, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportRelativeVec3, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportForwardIgnoreZ, (ALL_SUPPORTED_GAMES)))\
		((forceTeleportManual, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportAbsoluteVec3, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportAbsoluteFillCurrent, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportAbsoluteCopy, (ALL_SUPPORTED_GAMES)))\
			((forceTeleportAbsolutePaste, (ALL_SUPPORTED_GAMES)))\
	((forceLaunchGUI, (ALL_SUPPORTED_GAMES)))\
	((forceLaunchSettingsSubheading, (ALL_SUPPORTED_GAMES)))\
		((forceLaunchApplyToPlayer, (ALL_SUPPORTED_GAMES)))\
			((forceLaunchCustomObject, (ALL_SUPPORTED_GAMES)))\
		((forceLaunchSettingsRadioGroup, (ALL_SUPPORTED_GAMES)))\
		((forceLaunchForward, (ALL_SUPPORTED_GAMES)))\
			((forceLaunchRelativeVec3, (ALL_SUPPORTED_GAMES)))\
			((forceLaunchForwardIgnoreZ, (ALL_SUPPORTED_GAMES)))\
		((forceLaunchManual, (ALL_SUPPORTED_GAMES)))\
			((forceLaunchAbsoluteVec3, (ALL_SUPPORTED_GAMES)))\
	((switchBSPGUI, (Halo1, Halo2)))\
	((switchBSPSetGUI, (THIRD_GEN)))\
		((switchBSPSetLoadSet, (THIRD_GEN)))\
		((switchBSPSetFillCurrent, (THIRD_GEN)))\
		((switchBSPSetLoadIndex, (THIRD_GEN)))\
		((switchBSPSetUnloadIndex, (THIRD_GEN)))\
	((setPlayerHealthSubheadingGUI, (ALL_SUPPORTED_GAMES)))\
		((setPlayerHealthGUI, (ALL_SUPPORTED_GAMES)))\
		((setPlayerHealthValueGUI, (ALL_SUPPORTED_GAMES)))\
	((skullToggleGUI, (ALL_SUPPORTED_GAMES)))\
	((playerPositionToClipboardGUI, (ALL_SUPPORTED_GAMES)))\
	((consoleCommandGUI, (ALL_SUPPORTED_GAMES)))\
	((consoleCommandSettings, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandOutputEvent, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandPauseGame, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandBlockInput, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandFreeCursor, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandFontSize, (ALL_SUPPORTED_GAMES)))\
		((consoleCommandExecuteBuffer, (ALL_SUPPORTED_GAMES)))\
	((disableBarriersToggle, (THIRD_GEN)))\
	((soundClassGainAdjusterToggle, (Halo1)))\
	((soundClassGainAdjusterSettings, (Halo1)))\
		((soundClassGainDialog, (Halo1)))\
		((soundClassGainAmbience, (Halo1)))\
		((soundClassGainWeapons, (Halo1)))\
		((soundClassGainVehicles, (Halo1)))\
		((soundClassGainMusic, (Halo1)))\
		((soundClassGainOther, (Halo1)))




#define RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES2 \
((overlaysHeadingGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((renderDistance3DGUI, (ALL_SUPPORTED_GAMES)))\
	((display2DInfoToggleGUI, (ALL_SUPPORTED_GAMES)))\
	((display2DInfoSettingsInfoSubheading, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoShowGameTick, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoShowAggro, (Halo1)))\
			((display2DInfoShowRNG, (Halo1)))\
			((display2DInfoShowBSP, (Halo1, Halo2)))\
			((display2DInfoShowBSPSet, (THIRD_GEN)))\
			((display2DInfoShowNextObjectDatum, (Halo2)))\
			((display2DInfoTrackPlayer, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowPlayerViewAngle, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowPlayerViewAngleID, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowPlayerPosition, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowPlayerVelocity, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowPlayerVelocityAbs, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowPlayerVelocityXY, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowPlayerVelocityXYZ, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowPlayerHealth, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowPlayerRechargeCooldown, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowPlayerVehicleHealth, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoTrackCustomObject, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoCustomObjectDatum, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowEntityObjectType, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowEntityTagName, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowEntityPosition, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowEntityVelocity, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowEntityVelocityAbs, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowEntityVelocityXY, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowEntityVelocityXYZ, (ALL_SUPPORTED_GAMES)))\
				((display2DInfoShowEntityHealth, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowEntityRechargeCooldown, (ALL_SUPPORTED_GAMES)))\
					((display2DInfoShowEntityVehicleHealth, (ALL_SUPPORTED_GAMES)))\
	((display2DInfoSettingsVisualSubheading, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoAnchorCorner, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoScreenOffset, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoFontSize, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoFontColour, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoFloatPrecision, (ALL_SUPPORTED_GAMES)))\
			((display2DInfoOutline, (ALL_SUPPORTED_GAMES)))\
	((waypoint3DGUIToggle, (ALL_SUPPORTED_GAMES)))\
	((waypoint3DGUIList, (ALL_SUPPORTED_GAMES)))\
	((waypoint3DGUISettings, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIClampToggle, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalSpriteColor, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalSpriteScale, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalLabelColor, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalLabelScale, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalDistanceColor, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalDistanceScale, (ALL_SUPPORTED_GAMES)))\
		((waypoint3DGUIGlobalDistancePrecision, (ALL_SUPPORTED_GAMES)))\
	((viewAngleLine3DGUIToggle, (ALL_SUPPORTED_GAMES)))\
	((viewAngleLine3DGUIList, (ALL_SUPPORTED_GAMES)))\
	((viewAngleLine3DGUISettings, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalSpriteColor, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalLabelColor, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalLabelScale, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalDistanceColor, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalDistanceScale, (ALL_SUPPORTED_GAMES)))\
		((viewAngleLine3DGUIGlobalDistancePrecision, (ALL_SUPPORTED_GAMES)))\
	((triggerOverlayToggle, (ALL_SUPPORTED_GAMES)))\
	((triggerOverlaySettings, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayFilterToggle, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayFilterExactMatch, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayFilterStringDialog, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayFilterCountLabel, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayFilterStringCopy, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayFilterStringPaste, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayRenderStyle, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayInteriorStyle, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayLabelStyle, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayLabelScale, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayNormalColor, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayBSPColor, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlaySectorColor, (Halo3ODST, HaloReach, Halo4)))\
		((triggerOverlayAlpha, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayWireframeAlpha, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayCheckHitToggle, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayCheckHitFalloff, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayCheckHitColor, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayCheckMissToggle, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayCheckMissFalloff, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayCheckMissColor, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayMessageOnEnter, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayMessageOnExit, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayMessageOnCheckHit, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayMessageOnCheckMiss, (ALL_SUPPORTED_GAMES)))\
		((triggerOverlayPositionToggle, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayPositionColor, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayPositionScale, (ALL_SUPPORTED_GAMES)))\
			((triggerOverlayPositionWireframe, (ALL_SUPPORTED_GAMES)))\
	((shieldInputPrinterToggle, (Halo1)))\
	((sensDriftOverlayToggle, (Halo1)))\
	((sensDriftOverlaySettings, (Halo1)))\
		((sensOverDotFrameToggle, (Halo1)))\
			((sensMessageOnOverDotToggle, (Halo1)))\
			((sensSoundOnOverDotToggle, (Halo1)))\
		((sensSubpixelDriftToggle, (Halo1)))\
			((sensMessageOnSubpixelDriftToggle, (Halo1)))\
			((sensSoundOnSubpixelDriftToggle, (Halo1)))\
			((sensSubpixelDriftSciNotationToggle, (Halo1)))\
		((sensCountTurnsToggle, (Halo1)))\
		((sensResetCountAction, (Halo1)))\
		((sensResetCountOnRevertToggle, (Halo1)))\
		((sensAnchorCorner, (Halo1)))\
		((sensScreenOffset, (Halo1)))\
		((sensFontSize, (Halo1)))\
		((sensFontColour, (Halo1)))\
	((softCeilingOverlayToggle, (THIRD_GEN)))\
	((softCeilingOverlaySettings, (THIRD_GEN)))\
	((placementPointsOverlayToggle, (Halo3, Halo3ODST, HaloReach)))\
	((placementPointsOverlaySettings, (Halo3, Halo3ODST, HaloReach)))\
	((placementPointsOverlayExtended, (Halo3, Halo3ODST, HaloReach)))\
	((placementPointsOverlayShowValidity, (Halo3, Halo3ODST, HaloReach)))\
	((placementPointsOverlayRadius, (Halo3, Halo3ODST, HaloReach)))\
		((softCeilingOverlayRenderTypes, (THIRD_GEN)))\
		((softCeilingOverlayRenderDirection, (THIRD_GEN)))\
		((softCeilingOverlayColorAccel, (THIRD_GEN)))\
		((softCeilingOverlayColorSlippy, (THIRD_GEN)))\
		((softCeilingOverlayColorKill, (THIRD_GEN)))\
		((softCeilingOverlaySolidTransparency, (THIRD_GEN)))\
		((softCeilingOverlayWireframeTransparency, (THIRD_GEN)))\
	((abilityMeterOverlayToggle, (ABILITY_GAMES)))\
	((abilityMeterOverlaySettings, (ABILITY_GAMES)))\
			((abilityMeterHeroAssistToggle, (Halo4)))\
			((abilityMeterAbilityAnchorCorner, (ABILITY_GAMES)))\
			((abilityMeterAbilityScreenOffset, (ABILITY_GAMES)))\
			((abilityMeterAbilitySize, (ABILITY_GAMES)))\
			((abilityMeterAbilityBackgroundColor, (ABILITY_GAMES)))\
			((abilityMeterAbilityForegroundColor, (ABILITY_GAMES)))\
			((abilityMeterAbilityHighlightColor, (ABILITY_GAMES)))\
			((abilityMeterCooldownToggle, (ABILITY_GAMES)))\
				((abilityMeterCooldownAnchorCorner, (ABILITY_GAMES)))\
				((abilityMeterCooldownScreenOffset, (ABILITY_GAMES)))\
				((abilityMeterCooldownSize, (ABILITY_GAMES)))\
				((abilityMeterCooldownBackgroundColor, (ABILITY_GAMES)))\
				((abilityMeterCooldownForegroundColor, (ABILITY_GAMES)))\
				((abilityMeterCooldownHighlightColor, (ABILITY_GAMES)))\
((cameraHeadingGUI, (ALL_SUPPORTED_GAMES_AND_HALOCER)))\
	((hideHUDToggle, (FREE_CAMERA_SUPPORT)))\
	((editPlayerViewAngleSubheading, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleSet, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleVec2, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleFillCurrent, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleCopy, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAnglePaste, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleAdjustHorizontal, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleAdjustVertical, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleAdjustFactor, (ALL_SUPPORTED_GAMES)))\
	((editPlayerViewAngleIDSubheading, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDSet, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDInt, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDFillCurrent, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDCopy, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDPaste, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDAdjustPositive, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDAdjustNegative, (ALL_SUPPORTED_GAMES)))\
		((editPlayerViewAngleIDAdjustFactor, (ALL_SUPPORTED_GAMES)))\
	((freeCameraToggleGUI, (FREE_CAMERA_SUPPORT)))\
	((freeCameraSettingsSimpleSubheading, (FREE_CAMERA_SUPPORT)))\
	((freeCameraSettingsAdvancedSubheading, (FREE_CAMERA_SUPPORT)))\
		((freeCameraTeleportToCamera, (FREE_CAMERA_SUPPORT)))\
			((freeCameraTeleportToCameraSlightlyBehind, (FREE_CAMERA_SUPPORT)))\
		((freeCameraThirdPersonRendering, (FREE_CAMERA_SUPPORT)))\
		((freeCameraDisableScreenEffects, (Halo1, Halo2, HaloReach)))\
		((freeCameraGameInputDisable, (FREE_CAMERA_SUPPORT)))\
		((freeCameraCameraInputDisable, (FREE_CAMERA_SUPPORT)))\
		((freeCameraUserInputCameraSettings, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraBindingsSubheading, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputTranslateUpBinding, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputTranslateDownBinding, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputRollLeftBinding, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputRollRightBinding, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputFOVIncreaseBinding, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputFOVDecreaseBinding, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraBaseFOV, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraTranslationSpeed, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraIncreaseTranslationSpeedHotkey, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraDecreaseTranslationSpeedHotkey, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraTranslationSpeedChangeFactor, (FREE_CAMERA_SUPPORT)))\
			defFreeCameraInterpolator(UserInputCameraTranslation)\
			((freeCameraUserInputCameraRotationSpeed, (FREE_CAMERA_SUPPORT)))\
			defFreeCameraInterpolator(UserInputCameraRotation)\
			((freeCameraUserInputCameraRotationScalesToFOV, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraFOVSpeed, (FREE_CAMERA_SUPPORT)))\
			defFreeCameraInterpolator(UserInputCameraFOV)\
			((freeCameraUserInputCameraNonLinearFOVAtMinimum, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraNonLinearFOVAtMaximum, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraSetPosition, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraSetPositionChildren, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetPositionVec3, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetPositionFillCurrent, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetPositionCopy, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetPositionPaste, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraSetRotation, (FREE_CAMERA_SUPPORT)))\
			((freeCameraUserInputCameraSetRotationChildren, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetRotationVec3, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetRotationFillCurrent, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetRotationCopy, (FREE_CAMERA_SUPPORT)))\
				((freeCameraUserInputCameraSetRotationPaste, (FREE_CAMERA_SUPPORT)))\
	((changeOOBBackgroundToggle, (Halo3)))\
		((changeOOBBackgroundColor, (Halo3)))\
	((disableFogToggle, (ALL_SUPPORTED_GAMES)))\
((theaterHeadingGUI, (Halo3,Halo3ODST,HaloReach,Halo4)))\
((debugHeadingGUI, (ALL_SUPPORTED_GAMES)))\
	((getPlayerDatumGUI, (ALL_SUPPORTED_GAMES)))\
	((getPlayerAddressGUI, (ALL_SUPPORTED_GAMES)))\
	((getObjectAddressGUI, (ALL_SUPPORTED_GAMES)))\
	((getTagAddressGUI, (ALL_SUPPORTED_GAMES)))\


// Halo Campaign Evolved gui elements.
//
// These live in their own sequence for a MEASURED reason, not tidiness: appending them to sequence 2
// (which was already at 212 elements) made every translation unit that includes this header fail with
// C1009 "compiler limit: macros nested too deeply" at the GUIElementEnum definition below. That is MSVC's
// own preprocessor recursion limit, hit well before boost's BOOST_PP_LIMIT_SEQ of 256. Sequences 1 and 2
// are therefore back at exactly their pre-HaloCER contents - if you add more elements to either and see
// C1009, split again rather than trying to raise a limit.
//
// Sequence order does not imply gui order or hierarchy: both come from the createNestedElement child
// lists in GUIElementConstructor.cpp. Grouped by owning heading here purely for readability.
#define RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES3 \
	((hceAiFreezeGUI, (HALOCER_ONLY)))\
	((hceForceTeleportGUI, (HALOCER_ONLY)))\
	((hceForceTeleportSettingsSubheading, (HALOCER_ONLY)))\
		((hceForceTeleportSettingsRadioGroup, (HALOCER_ONLY)))\
		((hceForceTeleportForward, (HALOCER_ONLY)))\
			((hceForceTeleportRelativeVec3, (HALOCER_ONLY)))\
			((hceForceTeleportForwardIgnoreZ, (HALOCER_ONLY)))\
		((hceForceTeleportManual, (HALOCER_ONLY)))\
			((hceForceTeleportAbsoluteVec3, (HALOCER_ONLY)))\
			((hceForceTeleportAbsoluteFillCurrent, (HALOCER_ONLY)))\
			((hceForceTeleportAbsoluteCopy, (HALOCER_ONLY)))\
			((hceForceTeleportAbsolutePaste, (HALOCER_ONLY)))\
	((hceForceLaunchGUI, (HALOCER_ONLY)))\
	((hceForceLaunchSettingsSubheading, (HALOCER_ONLY)))\
		((hceForceLaunchSettingsRadioGroup, (HALOCER_ONLY)))\
		((hceForceLaunchForward, (HALOCER_ONLY)))\
			((hceForceLaunchRelativeVec3, (HALOCER_ONLY)))\
			((hceForceLaunchForwardIgnoreZ, (HALOCER_ONLY)))\
		((hceForceLaunchManual, (HALOCER_ONLY)))\
			((hceForceLaunchAbsoluteVec3, (HALOCER_ONLY)))\
	((hceSkullToggleGUI, (HALOCER_ONLY)))\
	((hceDisplayInfoToggleGUI, (HALOCER_ONLY)))\
	((hceDisplayInfoSettingsInfoSubheading, (HALOCER_ONLY)))\
			((hceDisplayInfoShowCoordinates, (HALOCER_ONLY)))\
			((hceDisplayInfoShowVelocity, (HALOCER_ONLY)))\
			((hceDisplayInfoShowVelocityXY, (HALOCER_ONLY)))\
			((hceDisplayInfoShowVelocityXYZ, (HALOCER_ONLY)))\
			((hceDisplayInfoShowLevel, (HALOCER_ONLY)))\
			((hceDisplayInfoShowBSP, (HALOCER_ONLY)))\
			((hceDisplayInfoShowZoneSet, (HALOCER_ONLY)))\
			((hceDisplayInfoShowCameraDiag, (HALOCER_ONLY)))\
			((hceDisplayInfoShowTick, (HALOCER_ONLY)))\
			((hceDisplayInfoShowPlayerDatum, (HALOCER_ONLY)))\
			((hceDisplayInfoShowTEB, (HALOCER_ONLY)))\
	((hceDisplayInfoSettingsVisualSubheading, (HALOCER_ONLY)))\
			((hceDisplayInfoAnchorCorner, (HALOCER_ONLY)))\
			((hceDisplayInfoScreenOffset, (HALOCER_ONLY)))\
			((hceDisplayInfoFontSize, (HALOCER_ONLY)))\
			((hceDisplayInfoFontColour, (HALOCER_ONLY)))\
			((hceDisplayInfoFloatPrecision, (HALOCER_ONLY)))\
			((hceDisplayInfoOutline, (HALOCER_ONLY)))\
	((hceFreecamToggleGUI, (HALOCER_ONLY)))\
	((hceFreecamTeleportToCamera, (HALOCER_ONLY)))\
	((hceSkyFixGUI, (HALOCER_ONLY)))\
	((hceFreecamNoclipGUI, (HALOCER_ONLY)))\
	((hceFreecamGimbalBypassGUI, (HALOCER_ONLY)))\
	((hceCameraRollGUI, (HALOCER_ONLY)))\
	((hceCameraRollLeftBindingGUI, (HALOCER_ONLY)))\
	((hceCameraRollRightBindingGUI, (HALOCER_ONLY)))\
	((hceCameraRollSpeedGUI, (HALOCER_ONLY)))\
	((hceCameraRollResetGUI, (HALOCER_ONLY)))\
	((hceCameraMoveSpeedGUI, (HALOCER_ONLY)))\
	((hceCameraMoveSpeedResetGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewToggleGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewValueGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewIncreaseBindingGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewDecreaseBindingGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewSpeedGUI, (HALOCER_ONLY)))\
	((hceFieldOfViewResetGUI, (HALOCER_ONLY)))\
	((hceDisableFadeFromBlackGUI, (HALOCER_ONLY)))\
	((hceGameSpeedGUI, (HALOCER_ONLY)))\
	((hceFreecamKeepPositionGUI, (HALOCER_ONLY)))\
	((hceFreecamDriftGUI, (HALOCER_ONLY)))\
	((hceFreecamDriftAmountGUI, (HALOCER_ONLY)))\
	((hceConsoleGUI, (HALOCER_ONLY)))\
	((freeCameraUserInputCameraTranslationInterpolatorDrift, (FREE_CAMERA_SUPPORT)))\
	((freeCameraUserInputCameraRotationInterpolatorDrift, (FREE_CAMERA_SUPPORT)))\
	((freeCameraUserInputCameraFOVInterpolatorDrift, (FREE_CAMERA_SUPPORT)))\
	((hceTriggerOverlayToggleGUI, (HALOCER_ONLY)))\
	((hceTriggerOverlaySettingsSubheading, (HALOCER_ONLY)))\
			((hceTriggerOverlayRenderStyle, (HALOCER_ONLY)))\
			((hceTriggerOverlayRenderDistance, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowVertex, (HALOCER_ONLY)))\
			((hceTriggerOverlayVertexColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayVertexScale, (HALOCER_ONLY)))\
			((hceTriggerOverlayHighlightActive, (HALOCER_ONLY)))\
			((hceTriggerOverlayActiveColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayFlashOnHit, (HALOCER_ONLY)))\
			((hceTriggerOverlayHitColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayHitFalloff, (HALOCER_ONLY)))\
			((hceTriggerOverlayMessageOnHit, (HALOCER_ONLY)))\
			((hceTriggerOverlaySpeedrunOnly, (HALOCER_ONLY)))\
			((hceTriggerOverlayNameFilterButton, (HALOCER_ONLY)))\
			((hceTriggerOverlayNameFilterToggle, (HALOCER_ONLY)))\
			((hceTriggerOverlayNameFilterExactMatch, (HALOCER_ONLY)))\
			((hceTriggerOverlayTypesShownSubheading, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowRegular, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowKill, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowZoneSet, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowBeginZoneSet, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowCheckpointGrant, (HALOCER_ONLY)))\
			((hceTriggerOverlayLabelColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayBspColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayBeginZoneSetColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayCheckpointGrantColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayZoneSetReport, (HALOCER_ONLY)))\
			((hceTriggerOverlayKillColour, (HALOCER_ONLY)))\
			((hceTriggerOverlaySafeZoneColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayAlpha, (HALOCER_ONLY)))\
			((hceTriggerOverlayShowLabels, (HALOCER_ONLY)))\
			((hceTriggerOverlayLabelScale, (HALOCER_ONLY)))\
			((hceTriggerOverlayBoxColour, (HALOCER_ONLY)))\
			((hceTriggerOverlayWireframeAlpha, (HALOCER_ONLY)))\
	((hceBspOverlayToggleGUI, (HALOCER_ONLY)))\
	((hceBspOverlaySettingsSubheading, (HALOCER_ONLY)))\
			((hceBspOverlayInvisibleOnly, (HALOCER_ONLY)))\
			((hceBspOverlayRenderStyle, (HALOCER_ONLY)))\
			((hceBspOverlayInteriorStyle, (HALOCER_ONLY)))\
			((hceBspOverlayRenderDistance, (HALOCER_ONLY)))\
			((hceBspOverlayColour, (HALOCER_ONLY)))\
			((hceBspOverlayAlpha, (HALOCER_ONLY)))\
			((hceBspOverlayInsideColour, (HALOCER_ONLY)))\
			((hceBspOverlayInsideAlpha, (HALOCER_ONLY)))\
			((hceBspOverlayWireframeAlpha, (HALOCER_ONLY)))\
			((hceBspOverlayWireframeColour, (HALOCER_ONLY)))\
			((hceBspOverlayOccludeFarSurfaces, (HALOCER_ONLY)))\
			((hceBspOverlayPatternContrast, (HALOCER_ONLY)))\
			((hceBspOverlayPatternScale, (HALOCER_ONLY)))\
			((hceBspOverlayLayerCompensation, (HALOCER_ONLY)))\
			((hceBspOverlayFaceShading, (HALOCER_ONLY)))\
			((hceBspOverlayShadingStrength, (HALOCER_ONLY)))\
			((hceBspOverlaySurfaceVariation, (HALOCER_ONLY)))\
	((hceSoftCeilingOverlayToggleGUI, (HALOCER_ONLY)))\
	((hceSoftCeilingOverlaySettingsSubheading, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayRenderTypes, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayRenderDirection, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayRenderDistance, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayColorAccel, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayColorKill, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayColorSlippy, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayShowDisabled, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayDisabledColour, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlaySolidTransparency, (HALOCER_ONLY)))\
			((hceSoftCeilingOverlayWireframeTransparency, (HALOCER_ONLY)))\
	((hceDisableBarriersGUI, (HALOCER_ONLY)))\
	((hceDumpCheckpointGUI, (HALOCER_ONLY)))\
	((hceDumpCheckpointSettingsSubheading, (HALOCER_ONLY)))\
			((hceDumpCheckpointAutonameGUI, (HALOCER_ONLY)))\
			((hceDumpCheckpointShadowGUI, (HALOCER_ONLY)))\
	((hceInjectCheckpointGUI, (HALOCER_ONLY)))\
	((hceInjectCheckpointSettingsSubheading, (HALOCER_ONLY)))\
			((hceInjectCheckpointForcesRevert, (HALOCER_ONLY)))\
			((hceInjectCheckpointLevelCheck, (HALOCER_ONLY)))\
			((hceInjectCheckpointDifficultyCheck, (HALOCER_ONLY)))\
			((hceInjectCheckpointVersionCheck, (HALOCER_ONLY)))\
	((competitionModeToggle, (Halo2)))\
	((competitionModeSettings, (Halo2)))\
	((competitionModeScale, (Halo2)))\
	((competitionModeBackgroundColour, (Halo2)))\
	((competitionModeTextColour, (Halo2)))\
	((competitionModeShowKD, (Halo2)))\
	((competitionModeShowKDRatio, (Halo2)))\
	((competitionModeShowColumnHeaders, (Halo2)))\
	((competitionModeOutlineText, (Halo2)))\
	((competitionModeForceBothPanels, (Halo2)))\
	((competitionModeLeftSettings, (Halo2)))\
	((competitionModeLeftOffset, (Halo2)))\
	((competitionModeLeftPanelWidth, (Halo2)))\
	((competitionModeLeftFontSize, (Halo2)))\
	((competitionModeLeftColour, (Halo2)))\
	((competitionModeRightSettings, (Halo2)))\
	((competitionModeRightOffset, (Halo2)))\
	((competitionModeRightPanelWidth, (Halo2)))\
	((competitionModeRightFontSize, (Halo2)))\
	((competitionModeRightColour, (Halo2)))\
	((hceScriptHeadingGUI, (HALOCER_ONLY)))\
			((hceInjectCheckpointRewriteIdentity, (HALOCER_ONLY)))



#define DEBUGGUIELEMENTS_ANDSUPPORTEDGAMES \
((HCMDebugHeadingGUI, (ALL_SUPPORTED_GAMES)))\





//#ifdef HCM_DEBUG
//
//#define ALLGUIELEMENTS_ANDSUPPORTEDGAMES BOOST_PP_CAT(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES, DEBUGGUIELEMENTS_ANDSUPPORTEDGAMES)
//
//#else 
//
//#define ALLGUIELEMENTS_ANDSUPPORTEDGAMES RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES
//
//#endif




#define MAKE_FIRSTOFPAIR_SET(r, d, seq) BOOST_PP_TUPLE_ELEM(0, seq),
#define MAKE_ALL_FIRSTOFPAIR(seq) BOOST_PP_SEQ_FOR_EACH(MAKE_FIRSTOFPAIR_SET, _, seq)
#define ALLGUIELEMENTS1 MAKE_ALL_FIRSTOFPAIR(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES1)

#define ALLGUIELEMENTS2 MAKE_ALL_FIRSTOFPAIR(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES2)
#define ALLGUIELEMENTS3 MAKE_ALL_FIRSTOFPAIR(RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES3)
#define ALLGUIELEMENTSDEBUG MAKE_ALL_FIRSTOFPAIR(DEBUGGUIELEMENTS_ANDSUPPORTEDGAMES)

enum class GUIElementEnum {
	ALLGUIELEMENTS1
	ALLGUIELEMENTS2
	ALLGUIELEMENTS3
#ifdef HCM_DEBUG
	ALLGUIELEMENTSDEBUG
#endif
};


// ⚠ THIS ENUM IS TOO BIG FOR THE GLOBAL magic_enum RANGE, ON PURPOSE. It is the only one that is, so it
// carries its own range rather than making every other reflected enum pay for its size.
//
// magic_enum reflects by instantiating a __FUNCSIG__ probe for every candidate VALUE in [min, max], per
// enum. The global bound in pch.h is therefore a tax on all ~30 reflected enums, and raising it to fit this
// one is the expensive way round. MAGIC_ENUM_RANGE_MAX is 256 there - enough for everything else, and
// enough for the byte-backed enums that depend on it (see the warning in pch.h about LevelID).
//
// ⚠⚠ WHY 1024 AND NOT THE EXACT COUNT. The range is INCLUSIVE - magic_enum computes
// `range_size = max - min + 1` - so setting max to the highest enumerator leaves ZERO headroom and the very
// next GUI row you add silently loses its name. And "silently" is literal: an out-of-range enumerator does
// not fail to compile, it reflects as an empty string, so the symptom is blank entries in PLOG lines, in
// the "you forgot a creation case label" message, and in the GUIServiceInfo failure listings - the exact
// output you would be reading while trying to work out what broke. A comment here previously undercounted
// this enum by 132 and nearly caused that. 1024 is room for roughly another 530 rows.
//
// Measured 2026-08-17: list1 148 + list2 206 + list3 134 = 488 literal entries, plus 4
// defFreeCameraInterpolator lines that each expand to two = 492 enumerators, values 0..491.
// Re-measure by counting `((` lines per list rather than trusting this number.
//
// Must be visible in every TU that reflects this enum, which is why it lives in this header immediately
// under the definition rather than in a .cpp: a specialisation seen in some TUs and not others is an ODR
// violation, and magic_enum would quietly use different ranges in different objects.
namespace magic_enum::customize
{
	template <>
	struct enum_range<GUIElementEnum>
	{
		static constexpr int min = 0;
		static constexpr int max = 1024;
	};
}
