#pragma once

// MUST HAVE SAME NAME AS ASSOCIATED CLASS (the macro will be used in a std::variant and etc to convert from enum to type)
//
// HARD LIMIT - 64 ENTRIES PER LIST. Each list is expanded through MAKECASE in OptionalCheatManager.cpp, i.e.
// BOOST_PP_VARIADIC_TO_SEQ, which is capped by BOOST_PP_LIMIT_VARIADIC. That is 64 here, MEASURED, not assumed:
// pch.h tries to raise it to 128, but boost/stacktrace.hpp (and friends) are included above that #define and
// they already pull in boost/preprocessor/config/limits.hpp, whose include guard makes the later #define a
// no-op. Compiling pch.h's exact include order and printing BOOST_PP_LIMIT_VARIADIC gives 64.
// ALLOPTIONALCHEATS1 is at EXACTLY 64 - it is FULL, do not add to it. ALLOPTIONALCHEATS2 is at 60.
// When that one fills, add an ALLOPTIONALCHEATS3 and give it its own MAKECASE line plus an entry in the enum
// below, rather than growing either list past 64. Overrunning does not give a clean error - it produces a wall
// of unrelated preprocessor noise.
#define ALLOPTIONALCHEATS1 	\
TogglePause,\
GameTickEventHook,\
AdvanceTicks,\
ToggleBlockInput,\
ToggleFreeCursor,\
ForceCheckpoint,\
ForceRevert,\
ForceDoubleRevert,\
ForceCoreSave,\
ForceCoreLoad,\
IgnoreCheckpointChecksum,\
InjectCheckpoint,\
DumpCheckpoint,\
InjectCore,\
DumpCore,\
ForceFutureCheckpoint,\
Speedhack,\
Invulnerability,\
GetCurrentDifficulty,\
GetPlayerDatum,\
GetObjectAddress,\
GetObjectAddressCLI,\
GetCurrentLevelCode,\
GetPlayerViewAngle,\
AIFreeze,\
Medusa,\
ForceTeleport,\
ForceLaunch,\
GetObjectPhysics,\
GetHavokComponent,\
GetHavokAnchorPoint,\
UnfreezeObjectPhysics,\
NaturalCheckpointDisable,\
InfiniteAmmo,\
BottomlessClip,\
DisplayPlayerInfo,\
GetAggroData,\
GetObjectHealth,\
GetTagName,\
GetObjectTagName,\
GetBipedsVehicleDatum,\
GetNextObjectDatum,\
FreeCamera,\
GetGameCameraData,\
UpdateGameCameraData,\
UserCameraInputReader,\
ThirdPersonRendering,\
BlockPlayerCharacterInput,\
GetCurrentRNG,\
EditPlayerViewAngle,\
EditPlayerViewAngleID,\
SwitchBSP,\
GetCurrentBSP,\
DisableScreenEffects,\
HideHUD,\
OBSBypassCheck,\
SetPlayerHealth,\
Waypoint3D,\
Render3DEventProvider,\
MeasurePlayerDistanceToObject,\
SkullToggler,\
TriggerOverlay,\
GetTriggerData,\
UpdateTriggerLastChecked



// Turns out there's a limit on how many entries in a tuple that boost::preprocesser can handle, so split it in two!

#define ALLOPTIONALCHEATS2	\
ObjectTableRange,\
HideWatermarkCheck,\
PlayerPositionToClipboard,\
TriggerFilter,\
GetPlayerTriggerPosition,\
TrackTriggerEnterExit,\
ShieldInputPrinter,\
TriggerFilterModalDialogManager,\
GetScenarioAddress,\
GetDebugString,\
TagBlockReader,\
TagTableRange,\
GetSoftCeilingData,\
SoftCeilingOverlay,\
PlacementPointsOverlay,\
GetCurrentBSPSet,\
SwitchBSPSet,\
GetCurrentZoneSet,\
BSPChangeHookEvent,\
ZoneSetChangeHookEvent,\
BSPSetChangeHookEvent,\
GetActiveStructureDesignTags,\
TagReferenceReader,\
CommandConsoleManager,\
HaloScriptOutputHookEvent,\
DisableBarriers,\
GetPlayerDatumPresenter,\
FreeCameraFOVOverride,\
GetAbilityData,\
AbilityMeterOverlay,\
GetTagAddressPresenter,\
SoundClassGain,\
ChangeOOBBackground,\
DisableFog,\
SensDriftOverlay,\
RevertEventHook,\
ForceMissionRestart,\
GameEngineDetail,\
GameEngineFunctions,\
ViewAngle3D,\
Season7Physics,\
FPScaleFix,\
DropShadowsOnObjects,\
SphereSpecularForce,\
OffscreenShadowCasters,\
UncapDropShadows,\
UncapVisibilityLimits,\
UncapClusterLimit,\
H2ShadowResolution,\
H2ArmorColour,\
FarClipDistance,\
SunScaleFix,\
AnimationFixes,\
HavokDebugger,\
PlayerActionUpdateHook,\
ReplayRecorder,\
ReplayPlayer,\
MasterTickrate,\
HCECheckpointDetours,\
PresetManager


// Halo Campaign Evolved. ALLOPTIONALCHEATS2 was at 60 of the hard 64 when these were added, i.e. 4 free slots
// for 7 cheats, so they get their own list rather than overrunning (which produces a wall of unrelated
// preprocessor noise, not a clean error). Remember to add a matching MAKECASE line in OptionalCheatManager.cpp.
// HCEGetPlayerState is the foundation - it owns the game-thread TLS walk and every one of the others
// resolveDependentCheat()s it.
#define ALLOPTIONALCHEATS3	\
HCEGetPlayerState,\
HCEGetCameraData,\
HCEFreezeAI,\
HCEDisplayInfo,\
HCESkullToggler,\
HCEForceTeleport,\
HCEForceLaunch,\
HCEFreecam,\
HCETriggerOverlay,\
HCETriggerActivity,\
HCEDisableBarriers


enum class OptionalCheatEnum {
	ALLOPTIONALCHEATS1,
	ALLOPTIONALCHEATS2,
	ALLOPTIONALCHEATS3,
};

