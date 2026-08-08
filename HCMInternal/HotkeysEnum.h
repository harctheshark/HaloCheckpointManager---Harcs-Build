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
#define ALL_REBINDABLE_HOTKEYS ALL_EVENTONPRESS_HOTKEYS, NOEVENT_HOTKEYS, SKULL_HOTKEYS, REPLAY_HOTKEYS, HCE_SKULL_HOTKEYS


//enum class RebindableHotkeyEnum : int {
//	ALL_EVENTONPRESS_HOTKEYS
//};


enum class RebindableHotkeyEnum : int {
	ALL_REBINDABLE_HOTKEYS
};
