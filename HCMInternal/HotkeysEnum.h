#pragma once
#include "pch.h"

// hotkeys that fire an event when going from unpressed to pressed
#define ALL_EVENTONPRESS_HOTKEYS 	\
toggleGUI,\
togglePause,\
advanceTicks,\
forceCheckpoint,\
forceRevert,\
forceDoubleRevert,\
forceCoreSave,\
forceCoreLoad,\
injectCheckpoint,\
dumpCheckpoint,\
injectCore,\
dumpCore,\
forceMissionRestart,\
speedhack,\
invuln,\
aiFreeze,\
medusa,\
forceTeleport,\
forceLaunch,\
naturalCheckpointDisable,\
infiniteAmmo,\
bottomlessClip,\
display2DInfo,\
freeCamera,\
freeCameraTeleportToCameraHotkey,\
freeCameraGameInputDisable,\
freeCameraCameraInputDisable,\
freeCameraUserInputCameraIncreaseTranslationSpeedHotkey,\
freeCameraUserInputCameraDecreaseTranslationSpeedHotkey,\
freeCameraUserInputCameraSetPosition,\
freeCameraUserInputCameraSetRotation,\
freeCameraUserInputCameraMaintainVelocity, \
freeCameraUserInputCameraSetVelocity,\
freeCameraAnchorPositionToObjectPosition,\
freeCameraAnchorPositionToObjectRotation,\
freeCameraAnchorRotationToObjectPosition,\
freeCameraAnchorRotationToObjectFacing,\
freeCameraAnchorFOVToObjectDistance,\
editPlayerViewAngleSet,\
editPlayerViewAngleAdjustHorizontal,\
editPlayerViewAngleAdjustVertical,\
editPlayerViewAngleIDSet,\
editPlayerViewAngleIDAdjustNegative,\
editPlayerViewAngleIDAdjustPositive,\
switchBSP,\
switchBSPSet,\
hideHUDToggle,\
setPlayerHealth,\
toggleWaypoint3D,\
triggerOverlayToggleHotkey,\
commandConsoleHotkey,\
commandConsoleExecuteBuffer,\
disableBarriers,\
softCeilingOverlayToggleHotkey,\
abilityMeterToggleHotkey,\
sensResetCountHotkey,\
sensDriftOverlayToggleHotkey,\
toggleViewAngleLine3D,\
shieldInputPrinterToggleHotkey



// skull hotkeys. They don't behave any different to above, but BOOST_PP_TUPLE_SIZE can only handle up to 64 elements 
#define SKULL_HOTKEYS \
skullAngerHotkey, \
skullAssassinsHotkey, \
skullBlackEyeHotkey, \
skullBlindHotkey, \
skullCatchHotkey, \
skullEyePatchHotkey, \
skullFamineHotkey, \
skullFogHotkey, \
skullForeignHotkey, \
skullIronHotkey, \
skullJackedHotkey, \
skullMasterblasterHotkey, \
skullMythicHotkey, \
skullRecessionHotkey, \
skullSoAngryHotkey, \
skullStreakingHotkey, \
skullSwarmHotkey, \
skullThatsJustWrongHotkey, \
skullTheyComeBackHotkey, \
skullThunderstormHotkey, \
skullTiltHotkey, \
skullToughLuckHotkey, \
skullBandannaHotkey, \
skullBondedPairHotkey, \
skullBoomHotkey, \
skullCowbellHotkey, \
skullEnvyHotkey, \
skullFeatherHotkey, \
skullGhostHotkey, \
skullGruntBirthdayPartyHotkey, \
skullGruntFuneralHotkey, \
skullIWHBYDHotkey, \
skullMalfunctionHotkey, \
skullPinataHotkey, \
skullProphetBirthdayPartyHotkey, \
skullScarabHotkey, \
skullSputnikHotkey, \
skullAcrophobiaHotkey


// Halo Campaign Evolved skulls - one hotkey per skull, all unbound by default.
//
// HCE's 56 skulls are a different set to MCC's and are addressed by BIT INDEX into a single bitfield, so these
// cannot reuse SKULL_HOTKEYS. They get their own macro for the same reason SKULL_HOTKEYS and REPLAY_HOTKEYS do:
// BOOST_PP_TUPLE_SIZE counts at most 64 per tuple, and HotkeyDefinitions.h sums the macros rather than counting
// one combined list. 56 fits in one tuple with room to spare.
//
// ⚠ ORDER IS THE BIT INDEX. Entry N here must be skull bit N, matching kHCESkulls[] in HCESkullEnum.h, which maps
// them back with kHCESkullHotkeys[]. A static_assert there catches a count drift; ORDER drift it cannot catch, so
// do not sort, insert or remove entries. The names are also the on-disk serialisation keys (RebindableHotkey
// serialises via magic_enum::enum_name), so renaming one silently drops that user's existing binding.
#define HCE_SKULL_HOTKEYS \
hceSkullIronHotkey, \
hceSkullBlackEyeHotkey, \
hceSkullToughLuckHotkey, \
hceSkullCatchHotkey, \
hceSkullFogHotkey, \
hceSkullFamineHotkey, \
hceSkullThunderstormHotkey, \
hceSkullTiltHotkey, \
hceSkullMythicHotkey, \
hceSkullAssassinHotkey, \
hceSkullBlindHotkey, \
hceSkullSupermanHotkey, \
hceSkullGruntBirthdayPartyHotkey, \
hceSkullIWHBYDHotkey, \
hceSkullRedHotkey, \
hceSkullYellowHotkey, \
hceSkullBlueHotkey, \
hceSkullAngryHotkey, \
hceSkullBandannaHotkey, \
hceSkullBondedPairHotkey, \
hceSkullBoomHotkey, \
hceSkullEnvyHotkey, \
hceSkullEyePatchHotkey, \
hceSkullForeignHotkey, \
hceSkullGhostHotkey, \
hceSkullGruntFuneralHotkey, \
hceSkullJackedHotkey, \
hceSkullMalfunctionHotkey, \
hceSkullMasterblasterHotkey, \
hceSkullPinataHotkey, \
hceSkullRecessionHotkey, \
hceSkullScarabHotkey, \
hceSkullSoAngryHotkey, \
hceSkullSwarmHotkey, \
hceSkullThatsJustWrongHotkey, \
hceSkullTheyComeBackHotkey, \
hceSkullAcrophobiaHotkey, \
hceSkullAdaptationHotkey, \
hceSkullReloadHotkey, \
hceSkullSporeVisibilityHotkey, \
hceSkullNightVisionHotkey, \
hceSkullLightsOutHotkey, \
hceSkullRiskrunHotkey, \
hceSkullPopHotkey, \
hceSkullArmisticeHotkey, \
hceSkullFragileHotkey, \
hceSkullGiveAndTakeHotkey, \
hceSkullStowAndGrowHotkey, \
hceSkullHipFireHotkey, \
hceSkullTemperamentalHotkey, \
hceSkullFloorIsLavaHotkey, \
hceSkullMagnifiedHotkey, \
hceSkullJohnnyAmmoTreeHotkey, \
hceSkullLeadheadHotkey, \
hceSkullEfficientHotkey, \
hceSkullThirdPersonHotkey


// Halo Campaign Evolved feature hotkeys - the settings on the HaloCER tabs that had no way to be bound.
// All 48 are event-on-press and ALL ARE UNBOUND BY DEFAULT: 48 invented defaults would collide with each
// other and with the existing ones, so the user picks every one of them.
//
// They get their own macro, like SKULL_HOTKEYS / REPLAY_HOTKEYS / HCE_SKULL_HOTKEYS, because
// ALL_EVENTONPRESS_HOTKEYS is at 59 of the 64 arguments BOOST_PP_TUPLE_SIZE can count and these would blow
// straight past it. ⚠ THAT FAILURE IS NOT DIAGNOSABLE FROM THE ERROR: at 65 arguments boost does not say
// "too many" or mention tuples at all - you get an undeclared-identifier error pointing at preprocessor
// guts. If you ever see one after adding a hotkey, count the macro you added it to before anything else.
//
// ⚠ ORDER IS AN INDEX. SettingsStateAndEvents::hceHotkeyEvents is indexed by position in THIS macro
// (HotkeyDefinitions.h pairs them with BOOST_PP_SEQ_FOR_EACH_I) and HotkeyEventsLambdas.h wires each index
// to what it does. The three blocks below are what those indices mean, so keep entries in their block:
//     [0 .. 40] flip one bool setting
//     [41..44] radio options - each SETS its option and clears its sibling
//     [45..48] fire an event that already exists (the same object the equivalent button fires)
// The names are also the on-disk serialisation keys (RebindableHotkey serialises via magic_enum::enum_name),
// so renaming one silently drops that user's existing binding.
#define HCE_HOTKEYS \
hceShadowCheckpointsHotkey, \
hceInjectCheckpointRewriteIdentityHotkey, \
hceAutonameCheckpointsHotkey, \
hceInjectCheckpointForcesRevertHotkey, \
hceInjectCheckpointLevelCheckHotkey, \
hceInjectCheckpointDifficultyCheckHotkey, \
hceInjectCheckpointVersionCheckHotkey, \
hceForceTeleportForwardIgnoreZHotkey, \
hceForceLaunchForwardIgnoreZHotkey, \
hceDisplayInfoShowCoordinatesHotkey, \
hceDisplayInfoShowVelocityHotkey, \
hceDisplayInfoShowVelocityXYHotkey, \
hceDisplayInfoShowVelocityXYZHotkey, \
hceDisplayInfoShowLevelHotkey, \
hceDisplayInfoShowBSPHotkey, \
hceDisplayInfoShowTickHotkey, \
hceDisplayInfoShowPlayerDatumHotkey, \
hceDisplayInfoShowTEBHotkey, \
hceDisplayInfoOutlineHotkey, \
hceSoftCeilingOverlayShowDisabledHotkey, \
hceBspOverlayToggleHotkey, \
hceBspOverlayInvisibleOnlyHotkey, \
hceBspOverlayOccludeFarSurfacesHotkey, \
hceBspOverlayFaceShadingHotkey, \
hceDisableFadeFromBlackHotkey, \
hceFreecamGimbalBypassHotkey, \
hceFreecamNoclipHotkey, \
hceSkyFixHotkey, \
hceTriggerOverlayHighlightActiveHotkey, \
hceTriggerOverlayFlashOnHitHotkey, \
hceTriggerOverlayMessageOnHitHotkey, \
hceTriggerOverlayShowRegularHotkey, \
hceTriggerOverlayShowSectorHotkey, \
hceTriggerOverlayShowKillHotkey, \
hceTriggerOverlayShowZoneSetHotkey, \
hceTriggerOverlaySpeedrunOnlyHotkey, \
hceTriggerOverlayShowVertexHotkey, \
hceTriggerOverlayShowLabelsHotkey, \
hceTriggerOverlayNameFilterToggleHotkey, \
hceTriggerOverlayNameFilterExactMatchHotkey, \
hceFieldOfViewHotkey, \
hceForceTeleportForwardHotkey, \
hceForceTeleportManualHotkey, \
hceForceLaunchForwardHotkey, \
hceForceLaunchManualHotkey, \
hceForceTeleportAbsoluteFillCurrentHotkey, \
hceForceTeleportAbsoluteCopyHotkey, \
hceForceTeleportAbsolutePasteHotkey, \
hceTriggerOverlayEditNameFilterHotkey


// replay hotkeys. Like the skulls, kept in their own macro because ALL_EVENTONPRESS_HOTKEYS is near the
// BOOST_PP_TUPLE_SIZE limit of 64. These are event-on-press hotkeys (they fire ActionEvents).
#define REPLAY_HOTKEYS \
replayRecord30Hotkey, \
replayRecord60Hotkey, \
replayStopSaveHotkey, \
replayLoadFileHotkey, \
replayPlayHotkey, \
replayStopPlaybackHotkey

// hotkeys that do not fire an event. Usually used for continous inputs like freecamera
#define NOEVENT_HOTKEYS \
cameraTranslateUpBinding,\
cameraTranslateDownBinding,\
cameraRollLeftBinding,\
cameraRollRightBinding,\
cameraFOVIncreaseBinding,\
cameraFOVDecreaseBinding

// both event and non-event hotkeys are rebindable
#define ALL_REBINDABLE_HOTKEYS ALL_EVENTONPRESS_HOTKEYS, NOEVENT_HOTKEYS, SKULL_HOTKEYS, REPLAY_HOTKEYS, HCE_SKULL_HOTKEYS, HCE_HOTKEYS


//enum class RebindableHotkeyEnum : int {
//	ALL_EVENTONPRESS_HOTKEYS
//};


enum class RebindableHotkeyEnum : int {
	ALL_REBINDABLE_HOTKEYS
};
