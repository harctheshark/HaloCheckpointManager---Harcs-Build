#pragma once
#include "SettingsStateAndEvents.h"


// actual hotkey events are defined in SettingsStateAndEvents. this class simply binds the hotkey event of eg toggle settings to actually doing something (ie flip the toggle setting).
// simple ActionEvent hotkeys do not need this.

class HotkeyEventsLambdas
{
	ScopedCallback<ActionEvent> mSpeedhackHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mInvulnerabilityHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mAIFreezeHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mMedusaHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mToggleGUIHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mTogglePauseHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mNaturalCheckpointDisableHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mInfiniteAmmoHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mBottomlessClipHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mDisplay2DInfoHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mFreeCameraHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mFreeCameraGameInputDisableHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mFreeCameraCameraInputDisableHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mFreeCameraUserInputCameraMaintainVelocityHotkeyCallbackHandle;
	ScopedCallback<ActionEvent> mHideHUDToggleHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> mtoggleWaypoint3DHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> mtoggleViewAngle3DHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> mtriggerOverlayToggleHotkeyEventCallbackHangle;
	ScopedCallback<ActionEvent> mdisableBarriersHotkeyEventCallbackHangle;
	ScopedCallback<ActionEvent> msoftCeilingOverlayToggleHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> mabilityMeterToggleHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> msensDriftOverlayToggleHotkeyEventCallbackHandle;
	ScopedCallback<ActionEvent> mshieldInputPrinterToggleHotkeyEventCallbackHandle;

	// Halo Campaign Evolved feature hotkeys. One entry per subscription, built in the constructor body from the
	// tables there rather than written out as 44 named members: a named member per hotkey is 44 more chances to
	// name one thing and bind another, and the tables sit right next to the index comments that define them.
	// unique_ptr because ScopedCallback deliberately bans copy AND move, so it cannot be stored in a container
	// by value or built in a loop.
	std::vector<std::unique_ptr<ScopedCallback<ActionEvent>>> mHCEHotkeyCallbacks;


public:
	HotkeyEventsLambdas(std::shared_ptr< SettingsStateAndEvents> settings)
		: mSpeedhackHotkeyCallbackHandle(settings->speedhackHotkeyEvent, [boolsetting = settings->speedhackToggle]() { boolsetting->flipBoolSetting(); }),
		mInvulnerabilityHotkeyCallbackHandle(settings->invulnerabilityHotkeyEvent, [boolsetting = settings->invulnerabilityToggle]() { boolsetting->flipBoolSetting(); }),
		mAIFreezeHotkeyCallbackHandle(settings->aiFreezeHotkeyEvent, [boolsetting = settings->aiFreezeToggle]() { boolsetting->flipBoolSetting(); }),
		mMedusaHotkeyCallbackHandle(settings->medusaHotkeyEvent, [boolsetting = settings->medusaToggle]() { boolsetting->flipBoolSetting(); }),
		mToggleGUIHotkeyCallbackHandle(settings->toggleGUIHotkeyEvent, [boolsetting = settings->GUIWindowOpen]() { boolsetting->flipBoolSetting(); }),
		mTogglePauseHotkeyCallbackHandle(settings->togglePauseHotkeyEvent, [boolsetting = settings->togglePause]() { boolsetting->flipBoolSetting(); }),
		mNaturalCheckpointDisableHotkeyCallbackHandle(settings->naturalCheckpointDisableHotkeyEvent, [boolsetting = settings->naturalCheckpointDisable]() { boolsetting->flipBoolSetting(); }),
		mInfiniteAmmoHotkeyCallbackHandle(settings->infiniteAmmoHotkeyEvent, [boolsetting = settings->infiniteAmmoToggle]() { boolsetting->flipBoolSetting(); }),
		mBottomlessClipHotkeyCallbackHandle(settings->bottomlessClipHotkeyEvent, [boolsetting = settings->bottomlessClipToggle]() { boolsetting->flipBoolSetting(); }),
		mDisplay2DInfoHotkeyCallbackHandle(settings->display2DInfoHotkeyEvent, [boolsetting = settings->display2DInfoToggle]() { boolsetting->flipBoolSetting(); }),
		mFreeCameraHotkeyCallbackHandle(settings->freeCameraHotkeyEvent, [boolsetting = settings->freeCameraToggle]() { boolsetting->flipBoolSetting(); }),
		mFreeCameraGameInputDisableHotkeyCallbackHandle(settings->freeCameraGameInputDisableHotkeyEvent, [boolsetting = settings->freeCameraGameInputDisable]() { boolsetting->flipBoolSetting(); }),
		mFreeCameraCameraInputDisableHotkeyCallbackHandle(settings->freeCameraCameraInputDisableHotkeyEvent, [boolsetting = settings->freeCameraCameraInputDisable]() { boolsetting->flipBoolSetting(); }),
		mFreeCameraUserInputCameraMaintainVelocityHotkeyCallbackHandle(settings->freeCameraUserInputCameraMaintainVelocityHotkeyEvent, [boolsetting = settings->freeCameraUserInputCameraMaintainVelocity]() { boolsetting->flipBoolSetting(); }),
		mHideHUDToggleHotkeyEventCallbackHandle(settings->hideHUDToggleHotkeyEvent, [boolsetting = settings->hideHUDToggle]() { boolsetting->flipBoolSetting(); }),
		mtoggleWaypoint3DHotkeyEventCallbackHandle(settings->toggleWaypoint3DHotkeyEvent, [boolsetting = settings->waypoint3DToggle]() { boolsetting->flipBoolSetting(); }),
		mtoggleViewAngle3DHotkeyEventCallbackHandle(settings->toggleViewAngle3DHotkeyEvent, [boolsetting = settings->viewAngleLine3DToggle]() { boolsetting->flipBoolSetting(); }),
		mtriggerOverlayToggleHotkeyEventCallbackHangle(settings->triggerOverlayToggleHotkeyEvent, [boolsetting = settings->triggerOverlayToggle]() {boolsetting->flipBoolSetting(); }),
		mdisableBarriersHotkeyEventCallbackHangle(settings->disableBarriersHotkeyEvent, [boolsettings = settings->disableBarriersToggle]() {boolsettings->flipBoolSetting(); }),
		msoftCeilingOverlayToggleHotkeyEventCallbackHandle(settings->softCeilingOverlayToggleHotkeyEvent, [boolsetting = settings->softCeilingOverlayToggle]() {boolsetting->flipBoolSetting(); }),
		mabilityMeterToggleHotkeyEventCallbackHandle(settings->abilityMeterToggleHotkeyEvent, [boolsetting = settings->abilityMeterOverlayToggle]() {boolsetting->flipBoolSetting(); }),
		msensDriftOverlayToggleHotkeyEventCallbackHandle(settings->sensDriftOverlayToggleHotkeyEvent, [boolsetting = settings->sensDriftOverlayToggle]() {boolsetting->flipBoolSetting(); }),
		mshieldInputPrinterToggleHotkeyEventCallbackHandle(settings->shieldInputPrinterHotkeyEvent, [boolsetting = settings->shieldInputPrinterToggle]() {boolsetting->flipBoolSetting(); })
		

	{
		PLOG_DEBUG << "HotkeyEvents con";

		// ---- Halo Campaign Evolved feature hotkeys -------------------------------------------------------
		// ⚠ EVERY INDEX BELOW IS A POSITION IN HCE_HOTKEYS (HotkeysEnum.h). The tables are written in that
		// macro's order and the position in the table IS the hotkey - so add to both, at the same place, or a
		// hotkey silently drives the setting next to the one it is named after. Nothing catches that: the
		// counts still add up and it compiles.

		constexpr size_t hceToggleCount = 50; // [0..49]  flip a bool
		constexpr size_t hceRadioCount = 4;  // [50..53] set a radio option
		constexpr size_t hceAliasCount = 7;  // [54..60] fire an event that already has its own subscriber
		static_assert(hceToggleCount + hceRadioCount + hceAliasCount == SettingsStateAndEvents::kHCEHotkeyCount,
			"The tables below no longer cover every HCE_HOTKEYS entry - a hotkey has been added without being wired up");

		// [0..49] plain toggles: flip the bool. Same order as the first block of HCE_HOTKEYS.
		const std::array<std::shared_ptr<BinarySetting<bool>>, hceToggleCount> hceToggleTargets
		{
			settings->hceShadowCheckpoints,
			settings->hceInjectCheckpointRewriteIdentity,
			settings->autonameCheckpoints,
			settings->injectCheckpointForcesRevert,
			settings->injectCheckpointLevelCheck,
			settings->injectCheckpointDifficultyCheck,
			settings->injectCheckpointVersionCheck,
			settings->forceTeleportForwardIgnoreZ,
			settings->forceLaunchForwardIgnoreZ,
			settings->hceDisplayInfoShowCoordinates,
			settings->hceDisplayInfoShowVelocity,
			settings->hceDisplayInfoShowVelocityXY,
			settings->hceDisplayInfoShowVelocityXYZ,
			settings->hceDisplayInfoShowLevel,
			settings->hceDisplayInfoShowBSP,
			settings->hceDisplayInfoShowZoneSet,
			settings->hceDisplayInfoShowCameraDiag,
			settings->hceDisplayInfoShowTick,
			settings->hceDisplayInfoShowPlayerDatum,
			settings->hceDisplayInfoShowTEB,
			settings->display2DInfoOutline,
			settings->hceSoftCeilingOverlayShowDisabled,
			settings->hceBspOverlayToggle,
			settings->hceBspOverlayInvisibleOnly,
			settings->hceBspOverlayOccludeFarSurfaces,
			settings->hceBspOverlayFaceShading,
			settings->hceDisableFadeFromBlackToggle,
			settings->hceFreecamGimbalBypassToggle,
			settings->hceFreecamNoclipToggle,
			settings->hceSkyFixToggle,
			settings->hceTriggerOverlayHighlightActive,
			settings->triggerOverlayCheckHitToggle,
			settings->triggerOverlayMessageOnCheckHit,
			settings->hceTriggerOverlayZoneSetReport,
			settings->hceTriggerOverlayShowRegular,
			settings->hceTriggerOverlayShowKill,
			settings->hceTriggerOverlayShowZoneSet,
			settings->hceTriggerOverlayShowBeginZoneSet,
			settings->hceTriggerOverlayShowCheckpointGrant,
			settings->hceTriggerOverlaySpeedrunOnly,
			settings->hceTriggerOverlayShowVertex,
			settings->hceTriggerOverlayShowLabels,
			settings->triggerOverlayFilterToggle,
			settings->triggerOverlayFilterExactMatch,
			settings->hceFieldOfViewToggle,
			settings->hceGameSpeedToggle,
			settings->hceFreecamKeepPositionToggle,
			settings->hceConsoleOpenToggle,
			settings->hceFreecamDriftToggle,
			settings->hceAISquadOverlayToggle,
		};

		for (size_t i = 0; i < hceToggleTargets.size(); i++)
			mHCEHotkeyCallbacks.emplace_back(std::make_unique<ScopedCallback<ActionEvent>>(
				settings->hceHotkeyEvents[i],
				[boolsetting = hceToggleTargets[i]]() { boolsetting->flipBoolSetting(); }));

		// [49..52] radio options: {the one to switch ON, the sibling to switch OFF}.
		// A radio must SET, never flip - flipping one of an exclusive pair leaves both on or both off, which is
		// precisely the broken state GUIRadioGroup exists to repair, and the repair picks the FIRST option, not
		// the one you pressed. Both settings are written the same way the group's own click handler writes them.
		const std::array<std::pair<std::shared_ptr<BinarySetting<bool>>, std::shared_ptr<BinarySetting<bool>>>, hceRadioCount> hceRadioTargets
		{ {
			{ settings->forceTeleportForward, settings->forceTeleportManual },
			{ settings->forceTeleportManual,  settings->forceTeleportForward },
			{ settings->forceLaunchForward,   settings->forceLaunchManual },
			{ settings->forceLaunchManual,    settings->forceLaunchForward },
		} };

		for (size_t i = 0; i < hceRadioTargets.size(); i++)
			mHCEHotkeyCallbacks.emplace_back(std::make_unique<ScopedCallback<ActionEvent>>(
				settings->hceHotkeyEvents[hceToggleCount + i],
				[optionOn = hceRadioTargets[i].first, optionOff = hceRadioTargets[i].second]()
				{
					optionOn->GetValueDisplay() = true;
					optionOn->UpdateValueWithInput();
					optionOff->GetValueDisplay() = false;
					optionOff->UpdateValueWithInput();
				}));

		// [54..60] are aliases of events that already exist and already have a subscriber (see the end of the
		// SettingsStateAndEvents constructor). They need nothing here - binding one more callback would run the
		// action twice.
	}

	~HotkeyEventsLambdas() { PLOG_DEBUG << "~HotkeyEvents"; }
};