#include "pch.h"
#include "GUIElementConstructor.h"
#include "GameProcessFilter.h"
#include <typeinfo>   // typeid, for naming an unexpected exception type in the catch-alls below
#include "SettingsStateAndEvents.h"
#include "HotkeysEnum.h"
#include "IGUIElement.h"
#include "GUIElementEnum.h"

#include "GUISimpleButton.h"
#include "GUIButtonWithCopyableResult.h"
#include "GUISimpleToggle.h"
#include "GUISpeedhack.h"
#include "GUIGameSpeed.h"
#include "GUIHCEConsole.h"
#include "GUIH5LuaConsole.h"
#include "GUIHCESwitchZoneSet.h"
#include "GUIH5SwitchZoneSet.h"
#include "GUIMCCSwitchZoneSet.h"
#include "GUIMapLauncher.h"
#include "GUIInvulnerability.h"
#include "GUIHeading.h"
#include "GUISubHeading.h"
#include "GUIHotkeyOnly.h"
#include "GUIAdvanceTicks.h"
#include "GUIRadioButton.h"
#include "GUIRadioGroup.h"
#include "GUIFloat.h"
#include "GUIFloatStepper.h"
#include "GUIToggleWithChildren.h"
#include "GUIInputString.h"
#include "GUIInputMultilineString.h"
#include "GUIInputDWORD.h"
#include "GUIColourPicker.h"
#include "GUIColourPickerAlpha.h"
#include "GUIInputInt.h"
#include "GUIComboEnum.h"
#include "GUIH2Physics.h"
#include "GUIComboEnumWithChildren.h"
#include "GUIDummyContainer.h"
#include "GUIVec3.h"
#include "GUIVec2.h"
#include "GUIButtonAndInt.h"
#include "SwitchBSP.h"   // SwitchBSPNames: the BSP names for the switch control
#include "GUIWaypointList.h"
#include "GUISkullToggle.h"
#include "GUIHCESkullToggle.h"
#include "GUILabel.h"
#include "GUIButtonAndBinaryInt.h"
#include "GUIInvisible.h"
#include "GUIViewAngleLineList.h"
#include "GUIInputSubpixel.h"








class GUIElementConstructor::GUIElementConstructorImpl {
private:
	std::set<std::pair<GUIElementEnum, GameState::Value>> cacheFailedServices;

	std::optional<std::shared_ptr<IGUIElement>> createGUIElementAndStoreResult(GUIElementEnum guielementenum, GameState game, std::shared_ptr<IGUIRequiredServices> guireq, std::shared_ptr<OptionalCheatInfo> fail, std::shared_ptr<GUIServiceInfo> info, std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<RuntimeExceptionHandler> exp)
	{
		auto res = createGUIElement(guielementenum, game, guireq, fail, info, settings, exp);
		if (res.has_value()) mStore->mapOfSuccessfullyConstructedGUIElements.at(game).insert(guielementenum);
		return res;
	}

	std::optional<std::shared_ptr<IGUIElement>> createGUIElement(GUIElementEnum guielementenum, GameState game, std::shared_ptr<IGUIRequiredServices> guireq, std::shared_ptr<OptionalCheatInfo> fail, std::shared_ptr<GUIServiceInfo> info, std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<RuntimeExceptionHandler> exp)
	{
		if (!guireq->getSupportedGamesPerGUIElement().contains(guielementenum)) throw HCMInitException("getSupportedGamesPerGUIElement missing element, how did that even happen?");
		if (!guireq->getSupportedGamesPerGUIElement().at(guielementenum).contains(game))
		{
			PLOG_DEBUG << "GUIElementEnum::" << magic_enum::enum_name(guielementenum) << " does not support Game::" << game.toString() << ", skipping construction";
			return std::nullopt; // don't construct for unsupported game
		}

		// Halo Campaign Evolved is a SEPARATE TITLE from MCC - the two can never be the same process. HCMInternal
		// still knows about every game, so without this the CER process builds GUI elements for all six MCC games
		// (and MCC builds the HaloCER ones), and each of those resolves optional cheats that OptionalCheatManager
		// deliberately never created for this process. That surfaced as ~471 bogus entries in the "Failed optional
		// cheat services" dialog on a perfectly healthy launch.
		//
		// Note this must mirror the filter in OptionalCheatManager::createCheats exactly - the two are the same
		// decision made in two places, and if they ever disagree you get either phantom failures (element built,
		// cheat wasn't) or silently missing UI (cheat built, element wasn't).
		{
			// See GameProcessFilter.h. This was a boolean (isHaloCER != isCampaignEvolvedProcess) until
			// Halo 5: Forge made a third title exist, at which point "not HaloCER" stopped identifying
			// "an MCC game" and each standalone title would have been built inside the other's process.
			const auto elementGame = game.operator GameState::Value();
			if (!gameBelongsToProcess(elementGame, mProcType))
			{
				PLOG_DEBUG << "GUIElementEnum::" << magic_enum::enum_name(guielementenum) << " for Game::"
					<< game.toString() << " belongs to the other title, skipping construction";
				return std::nullopt;
			}
		}

		if (cacheFailedServices.contains(std::make_pair(guielementenum, game))) // ignore elements that have already failed construction
		{
			PLOG_DEBUG << "GUIElementEnum::" << magic_enum::enum_name(guielementenum) << " for Game::" << game.toString() << " already failed construction, skipping construction";
			return std::nullopt;
		}


		// stupid override to make core save/dump stuff only construct for steam since I can't be bothered reworking the macro system to do this properly
		if ((magic_enum::enum_name<GUIElementEnum>(guielementenum).contains("core") || magic_enum::enum_name<GUIElementEnum>(guielementenum).contains("Core")) // is the gui elemenet a core save thing? this is the most icky part of this, doing a stringcheck of the enum name.. what if I want to add another cheat that uses the name "core" to mean something else? eh prolly won't come up
			&& game.operator GameState::Value() == GameState::Value::Halo1 // and is halo 1
			&& mProcType == MCCProcessType::WinStore // and current process is winstore
			)
		{
			PLOG_DEBUG << "GUIElementEnum::" << magic_enum::enum_name(guielementenum) << " for game " << game.toString() << " does not support WinStore version of MCC (steam only), skipping construction";
			return std::nullopt; // don't construct 
		}


		PLOG_DEBUG << "attempting to construct guielement: " << magic_enum::enum_name(guielementenum) << " for game: " << game.toString();
		try 
		{
			// check for failed required services
			if (guireq->getRequiredServicesPerGUIElement().contains(guielementenum))
			{
				std::vector<HCMInitException> failures;
				for (auto reqSer : guireq->getRequiredServicesPerGUIElement().at(guielementenum))
				{
					auto info = fail->getOptionalCheatInfo({ game, reqSer });
					if (info.init == false && info.ex.has_value()) failures.push_back(info.ex.value());
				}

				if (!failures.empty())
				{
					info->addFailure(guielementenum, game, failures);

					// TODO: get rid of this
					PLOG_ERROR << "ERROR: ";
					for (auto& fail : failures)
					{
						PLOG_ERROR << fail.what();
					}

					return std::nullopt;
				}
			}


			typedef  std::vector<std::optional<std::shared_ptr<IGUIElement>>> headerChildElements;
			typedef  std::vector<std::vector<std::optional<std::shared_ptr<IGUIElement>>>> vectorOfHeaderChildElements;
#define createNestedElement(elementEnum) createGUIElementAndStoreResult(elementEnum, game, guireq, fail, info, settings, exp) // optional nested element, recursively calls this function




			// ALL GUI ELEMENTS MUST HAVE A CASE HERE, TOP LEVEL OR NOT
			std::optional<std::shared_ptr<IGUIElement>> x;
			switch (guielementenum)
			{
			case GUIElementEnum::controlHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("General HCM options"), "Control", headerChildElements
						{
						createNestedElement(GUIElementEnum::toggleGUIHotkeyGUI),
						createNestedElement(GUIElementEnum::messagesFontSize),
						createNestedElement(GUIElementEnum::messagesFontColor),
						createNestedElement(GUIElementEnum::GUISettingsSubheading),
						createNestedElement(GUIElementEnum::togglePauseGUI),
						createNestedElement(GUIElementEnum::togglePauseSettingsSubheading),
						createNestedElement(GUIElementEnum::showGUIFailuresGUI),
						createNestedElement(GUIElementEnum::OBSBypassToggleGUI),
						createNestedElement(GUIElementEnum::HideWatermarkGUI),
						}));

				case GUIElementEnum::toggleGUIHotkeyGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<63>>
						(game, ToolTipCollection("Brings up or collapses this main GUI window"), RebindableHotkeyEnum::toggleGUI, "Toggle this GUI"));

				case GUIElementEnum::messagesFontSize:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(6.f, 120.f)>>
						(game, ToolTipCollection("Font size of messages that show up under this window"), "Message Font Size", settings->messagesFontSize));


				case GUIElementEnum::messagesFontColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Color of messages that show up under this window"), "Messages Font Color", settings->messagesFontColor));


				case GUIElementEnum::GUISettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Options related to this main GUI"), "GUI settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::GUIShowingFreesCursor),
							createNestedElement(GUIElementEnum::GUIShowingBlocksInput),
							createNestedElement(GUIElementEnum::GUIShowingPausesGame),
							}));

					case GUIElementEnum::GUIShowingFreesCursor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically free the curson when this GUI is open"), std::nullopt, "Free cursor when GUI open", settings->GUIShowingFreesCursor));

					case GUIElementEnum::GUIShowingBlocksInput:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically block game input when this GUI is open"), std::nullopt, "Block input when GUI open", settings->GUIShowingBlocksInput));

					case GUIElementEnum::GUIShowingPausesGame:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically pause the game when this GUI is open"), std::nullopt, "Pause game when GUI open", settings->GUIShowingPausesGame));

				case GUIElementEnum::togglePauseGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Pauses the game simulation"), RebindableHotkeyEnum::togglePause, "Pause game", settings->togglePause));

				case GUIElementEnum::togglePauseSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for pausing the game"), "Pause settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::advanceTicksGUI),
							createNestedElement(GUIElementEnum::pauseAlsoBlocksInputGUI),
							createNestedElement(GUIElementEnum::pauseAlsoFreesCursorGUI),
							createNestedElement(GUIElementEnum::pauseAlsoFreezesEffectsGUI),
							}));


				case GUIElementEnum::advanceTicksGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIAdvanceTicks<true>>
						(game, ToolTipCollection("Advances the game simulation by x ticks while the game is paused"), RebindableHotkeyEnum::advanceTicks, settings->advanceTicksEvent, settings->advanceTicksCount));

					case GUIElementEnum::pauseAlsoBlocksInputGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Makes this pause feature also block game input"), std::nullopt, "Pause also blocks game input", settings->pauseAlsoBlocksInput));

					case GUIElementEnum::pauseAlsoFreezesEffectsGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("On: pausing also freezes particles, smoke and grass/decorator wind, like MCC's own pause. Off: they keep animating while the game is paused."), std::nullopt, "Pause also freezes effects", settings->pauseAlsoFreezesEffects));

					case GUIElementEnum::pauseAlsoFreesCursorGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Makes this pause feature also free MCC's cursor"), std::nullopt, "Pause also frees cursor", settings->pauseAlsoFreesCursor));

				case GUIElementEnum::showGUIFailuresGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Shows a modal list of optional cheat services that failed to initialise, sorted by game"), std::nullopt, "Show optional cheat service failures", settings->showGUIFailures));

					


				case GUIElementEnum::OBSBypassToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Causes HCMs GUI to not be captured by recording software like OBS"), std::nullopt, "Bypass OBS Capture", settings->OBSBypassToggle));


				case GUIElementEnum::HideWatermarkGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
						(game, ToolTipCollection("Hides the HCM Watermark when the GUI is not open"), std::nullopt, "Hide HCM Watermark", settings->hideWatermark, headerChildElements
							{
							createNestedElement(GUIElementEnum::HideWatermarkIncludeMessagesGUI),
							}));

				case GUIElementEnum::HideWatermarkIncludeMessagesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Hides the HCM message log too."), std::nullopt, "Also hide HCM messages", settings->hideWatermarkHideMessages));

				case GUIElementEnum::mapLauncherGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIMapLauncher>
						(game, ToolTipCollection("Launch a map from MCC's mod map packs (or Halo 2 stock maps) with the lobby's gametype. Pick a map, press Play, then Start in a Halo 2 custom game lobby. Halo 2 only for now.")));

				case GUIElementEnum::presetsHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
						(game, ToolTipCollection("Save or load a full HCM settings snapshot (every toggle, value and hotkey) to a named .hcm preset file. Loading a preset applies it live."), "Presets", headerChildElements
							{
							createNestedElement(GUIElementEnum::presetSaveButton),
							createNestedElement(GUIElementEnum::presetLoadButton),
							}));

					case GUIElementEnum::presetSaveButton:
						// "##hcmGlobalPreset" keeps the visible label "Save Preset..." but gives ImGui a unique ID
						// (the armour-colour section reuses the plain "Save Preset..." label - identical IDs collide).
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Save every current HCM setting to a preset file (opens a Save dialog). Presets live in <HCM folder>/presets."), std::nullopt, "Save Preset...##hcmGlobalPreset", settings->presetSaveEvent));

					case GUIElementEnum::presetLoadButton:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Load a preset file and apply it live (opens an Open dialog). Every setting in the file is applied; anything not in the file keeps its current value."), std::nullopt, "Load Preset...##hcmGlobalPreset", settings->presetLoadEvent));


			case GUIElementEnum::saveManagementHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Features for managing your checkpoints (savestates)"), "Save Management", headerChildElements
						{
							createNestedElement(GUIElementEnum::forceCheckpointGUI),
							createNestedElement(GUIElementEnum::hceForceCheckpointGUI),
							createNestedElement(GUIElementEnum::h5ForceCheckpointGUI),
							createNestedElement(GUIElementEnum::h5ForceTeleportGUI),
							createNestedElement(GUIElementEnum::h5ForceTeleportSettingsSubheading),
							createNestedElement(GUIElementEnum::h5ForceLaunchGUI),
							createNestedElement(GUIElementEnum::h5ForceLaunchSettingsSubheading),
							createNestedElement(GUIElementEnum::h5AcrophobiaGUI),
							createNestedElement(GUIElementEnum::h5InvincibilityGUI),
							createNestedElement(GUIElementEnum::h5InfiniteAmmoGUI),
							createNestedElement(GUIElementEnum::h5BottomlessClipGUI),
							createNestedElement(GUIElementEnum::h5OneShotKillGUI),
							createNestedElement(GUIElementEnum::h5OneShotKillMultiplierGUI),
							createNestedElement(GUIElementEnum::h5PauseMenuFixGUI),
							createNestedElement(GUIElementEnum::h5OutOfBoundsBypassGUI),
							createNestedElement(GUIElementEnum::h5GameSpeedGUI),
							createNestedElement(GUIElementEnum::h5GameSpeedAmountGUI),
							createNestedElement(GUIElementEnum::h5HavokOverlayToggleGUI),
							createNestedElement(GUIElementEnum::h5HavokOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::h5SwitchZoneSetGUI),
							createNestedElement(GUIElementEnum::h5LuaConsoleGUI),
							createNestedElement(GUIElementEnum::h5TriggerOverlayToggleGUI),
							createNestedElement(GUIElementEnum::h5TriggerOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::h5ForceRevertGUI),
							createNestedElement(GUIElementEnum::forceRevertGUI),
							createNestedElement(GUIElementEnum::forceDoubleRevertGUI),
							createNestedElement(GUIElementEnum::mpRevertRepairGUI),
							createNestedElement(GUIElementEnum::forceCoreSaveGUI),
							createNestedElement(GUIElementEnum::forceCoreLoadGUI),
							createNestedElement(GUIElementEnum::injectCheckpointGUI),
							createNestedElement(GUIElementEnum::injectCheckpointSettingsSubheading),
							createNestedElement(GUIElementEnum::dumpCheckpointGUI),
							createNestedElement(GUIElementEnum::dumpCheckpointSettingsSubheading),
							createNestedElement(GUIElementEnum::hceInjectCheckpointGUI),
							createNestedElement(GUIElementEnum::hceInjectCheckpointSettingsSubheading),
							createNestedElement(GUIElementEnum::hceDumpCheckpointGUI),
							createNestedElement(GUIElementEnum::hceDumpCheckpointSettingsSubheading),
							createNestedElement(GUIElementEnum::injectCoreGUI),
							createNestedElement(GUIElementEnum::injectCoreSettingsSubheading),
							createNestedElement(GUIElementEnum::dumpCoreGUI),
							createNestedElement(GUIElementEnum::dumpCoreSettingsSubheading),
							createNestedElement(GUIElementEnum::naturalCheckpointDisableGUI),
							createNestedElement(GUIElementEnum::hceNaturalCheckpointDisableGUI),
							createNestedElement(GUIElementEnum::forceFutureCheckpointGUI),
							createNestedElement(GUIElementEnum::forceMissionRestartGUI),
							createNestedElement(GUIElementEnum::replayInputSubheading),

						}));

				case GUIElementEnum::forceCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces a checkpoint, regardless of if the player is safe or not"), RebindableHotkeyEnum::forceCheckpoint, "Force Checkpoint", settings->forceCheckpointEvent));

				// Halo Campaign Evolved's own force checkpoint. Deliberately reuses forceCheckpointEvent (and so the
				// existing forceCheckpoint hotkey) - HaloCER and the MCC games can never coexist in one process, so
				// exactly one listener exists at a time. "##hce" keeps the visible label but gives imgui a unique ID.
				case GUIElementEnum::hceForceCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces a checkpoint, regardless of if the player is safe or not"), RebindableHotkeyEnum::forceCheckpoint, "Force Checkpoint##hce", settings->forceCheckpointEvent));

				// Halo 5: Forge. Same deliberate event/hotkey reuse as the HaloCER pair above - the three
				// titles can never coexist in one process, so exactly one listener exists at a time.
				// "##h5" keeps the visible label but gives imgui a unique ID.
				case GUIElementEnum::h5ForceCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces a checkpoint, regardless of if the player is safe or not"), RebindableHotkeyEnum::forceCheckpoint, "Force Checkpoint##h5", settings->forceCheckpointEvent));

				case GUIElementEnum::h5ForceRevertGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces a revert to the last checkpoint.\nHalo 5: campaign only - refused in a session where you are not the simulation authority."), RebindableHotkeyEnum::forceRevert, "Force Revert##h5", settings->forceRevertEvent));

				// Force Teleport. Mirrors the HaloCER tree below, and deliberately REUSES the hce* hotkey
				// slots for the sub-options rather than adding Halo-5-specific ones: inserting entries into
				// HotkeysEnum.h shifts the hard-coded alias indices in HotkeyEventsLambdas.h and trips the
				// static_asserts, and the two titles can never be loaded at the same time anyway.
				case GUIElementEnum::h5ForceTeleportGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Teleports the player.\nUses the engine's own teleport call, so physics follows - writing the position directly desyncs and then kills you."), RebindableHotkeyEnum::forceTeleport, "Force Teleport##h5", settings->forceTeleportEvent));

				case GUIElementEnum::h5ForceTeleportSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Teleport Settings##h5", headerChildElements
							{
							createNestedElement(GUIElementEnum::h5ForceTeleportSettingsRadioGroup),
							}));

					case GUIElementEnum::h5ForceTeleportSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Teleport Radio Group##h5", headerChildElements
								{
								createNestedElement(GUIElementEnum::h5ForceTeleportForward),
								createNestedElement(GUIElementEnum::h5ForceTeleportManual),
								}));

						case GUIElementEnum::h5ForceTeleportForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport relative to the players position and look-direction"), RebindableHotkeyEnum::hceForceTeleportForwardHotkey, "Teleport Relative to Player##h5", settings->forceTeleportForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::h5ForceTeleportRelativeVec3),
									createNestedElement(GUIElementEnum::h5ForceTeleportForwardIgnoreZ),
									}));

							case GUIElementEnum::h5ForceTeleportRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How far forward/right/up to teleport the player, relative to their look-direction"), "Teleport: ##h5ForceTeleportRelativeVec3", settings->forceTeleportRelativeVec3, "Forward", "Right", "Up"));

							case GUIElementEnum::h5ForceTeleportForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), RebindableHotkeyEnum::hceForceTeleportForwardIgnoreZHotkey, "Ignore vertical look angle##h5Teleport", settings->forceTeleportForwardIgnoreZ));

						case GUIElementEnum::h5ForceTeleportManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport to absolute world coordinates"), RebindableHotkeyEnum::hceForceTeleportManualHotkey, "Teleport to Manual Coordinates##h5", settings->forceTeleportManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::h5ForceTeleportAbsoluteVec3),
									createNestedElement(GUIElementEnum::h5ForceTeleportAbsoluteFillCurrent),
									}));

							case GUIElementEnum::h5ForceTeleportAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("The xyz world coordinates to teleport to"), "Teleport: ##h5ForceTeleportAbsoluteVec3", settings->forceTeleportAbsoluteVec3));

							case GUIElementEnum::h5ForceTeleportAbsoluteFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
									(game, ToolTipCollection("Fill with the current xyz position of the player"), RebindableHotkeyEnum::hceForceTeleportAbsoluteFillCurrentHotkey, "Fill with current position##h5", settings->forceTeleportAbsoluteFillCurrent));

				// Force Launch. Same structure as the teleport tree above, writing the character
				// controller's VELOCITY instead of its position. Reuses the hce* hotkey slots for the same
				// reason - adding entries to HotkeysEnum.h shifts hard-coded alias indices.
				case GUIElementEnum::h5ForceLaunchGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Launches the player at the given velocity.\nWrites the character controller's own velocity, so the engine integrates it and gravity applies normally."), RebindableHotkeyEnum::forceLaunch, "Force Launch##h5", settings->forceLaunchEvent));

				case GUIElementEnum::h5ForceLaunchSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Launch Settings##h5", headerChildElements
							{
							createNestedElement(GUIElementEnum::h5ForceLaunchSettingsRadioGroup),
							}));

					case GUIElementEnum::h5ForceLaunchSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Launch Radio Group##h5", headerChildElements
								{
								createNestedElement(GUIElementEnum::h5ForceLaunchForward),
								createNestedElement(GUIElementEnum::h5ForceLaunchManual),
								}));

						case GUIElementEnum::h5ForceLaunchForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Launch relative to the players look-direction"), RebindableHotkeyEnum::hceForceLaunchForwardHotkey, "Launch Relative to Player##h5", settings->forceLaunchForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::h5ForceLaunchRelativeVec3),
									createNestedElement(GUIElementEnum::h5ForceLaunchForwardIgnoreZ),
									}));

							case GUIElementEnum::h5ForceLaunchRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How fast to launch forward/right/up, relative to the players look-direction"), "Launch: ##h5ForceLaunchRelativeVec3", settings->forceLaunchRelativeVec3, "Forward", "Right", "Up"));

							case GUIElementEnum::h5ForceLaunchForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), RebindableHotkeyEnum::hceForceLaunchForwardIgnoreZHotkey, "Ignore vertical look angle##h5Launch", settings->forceLaunchForwardIgnoreZ));

						case GUIElementEnum::h5ForceLaunchManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Launch at an absolute world-space velocity"), RebindableHotkeyEnum::hceForceLaunchManualHotkey, "Launch at Manual Velocity##h5", settings->forceLaunchManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::h5ForceLaunchAbsoluteVec3),
									}));

							case GUIElementEnum::h5ForceLaunchAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("The xyz world-space velocity to launch at"), "Launch: ##h5ForceLaunchAbsoluteVec3", settings->forceLaunchAbsoluteVec3));

				case GUIElementEnum::h5AcrophobiaGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Acrophobia (boots off the ground): hold JUMP to fly along your look direction.\nThe Halo 1 / CER flight model - the acceleration curve is its own speed limiter, topping out at 16 wu/s."), RebindableHotkeyEnum::hceSkullAcrophobiaHotkey, "Acrophobia##h5", settings->h5AcrophobiaToggle));


				case GUIElementEnum::h5TriggerOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Draw the scenario's trigger volumes in 3D. Sector-shaped volumes are drawn as bounding boxes and hidden by default. If boxes don't line up with the world, set FOV below to your in-game FOV."), std::nullopt, "Trigger Overlay##h5", settings->h5TriggerOverlayToggle));

				case GUIElementEnum::h5TriggerOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Trigger overlay filters, colours and appearance"), "Trigger Overlay Settings##h5", headerChildElements
							{
								createNestedElement(GUIElementEnum::h5TriggerOverlayShowRegular),
								createNestedElement(GUIElementEnum::h5TriggerOverlayShowKill),
								createNestedElement(GUIElementEnum::h5TriggerOverlayShowBeginZoneSet),
								createNestedElement(GUIElementEnum::h5TriggerOverlayShowCommitZoneSet),
								createNestedElement(GUIElementEnum::h5TriggerOverlayShowLabels),
								createNestedElement(GUIElementEnum::h5TriggerOverlaySpeedrunOnly),
								createNestedElement(GUIElementEnum::h5TriggerOverlayEditNameFilter),
								createNestedElement(GUIElementEnum::h5TriggerOverlayColourByScript),
								createNestedElement(GUIElementEnum::h5TriggerOverlayUseLiveActivity),
								createNestedElement(GUIElementEnum::h5TriggerOverlayActivityWindowMs),
								createNestedElement(GUIElementEnum::h5TriggerOverlayScriptedColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayInertColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayLabelScale),
								createNestedElement(GUIElementEnum::h5TriggerOverlayLabelColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayHitMessages),
								createNestedElement(GUIElementEnum::h5TriggerOverlayNormalColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayKillColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayBeginZoneSetColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayCommitZoneSetColor),
								createNestedElement(GUIElementEnum::h5TriggerOverlayAlpha),
								createNestedElement(GUIElementEnum::h5TriggerOverlayWireframeAlpha),
								createNestedElement(GUIElementEnum::h5TriggerOverlayRenderDistance),
							}));

					case GUIElementEnum::h5TriggerOverlayShowRegular:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show volumes that belong to no category - the ordinary scripted triggers."), std::nullopt, "Show Regular##h5tv", settings->h5TriggerOverlayShowRegular));

					case GUIElementEnum::h5TriggerOverlayUseLiveActivity:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Colour regular volumes by whether a mission script actually tested them just now, not just whether the script mentions them. Kill and zone-set volumes keep their category colour."), std::nullopt, "Live Script Activity##h5tv", settings->h5TriggerOverlayUseLiveActivity));

					case GUIElementEnum::h5TriggerOverlayActivityWindowMs:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(50.f, 10000.f)>>
							(game, ToolTipCollection("How long after a script tests a volume it still counts as live, in milliseconds. Too short a window makes live volumes flicker."), "Activity Window (ms)##h5tv", settings->h5TriggerOverlayActivityWindowMs));

					case GUIElementEnum::h5TriggerOverlaySpeedrunOnly:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Draws only the volumes a speedrun must touch: the level's goal advance volumes plus its end trigger. Not every step is a volume (kills, devices, flags), so this shows a subset of the route."), std::nullopt, "Speedrun Triggers Only##h5tv", settings->h5TriggerOverlaySpeedrunOnly));

					case GUIElementEnum::h5TriggerOverlayEditNameFilter:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Pick which trigger volumes to draw by name from a checkbox list. Needs a level loaded with the Trigger Overlay on. An empty list means everything is drawn; selecting names turns the filter on."), std::nullopt, "Filter by Name...##h5tv", settings->h5TriggerOverlayEditNameFilterEvent));

					case GUIElementEnum::h5TriggerOverlayShowKill:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show kill volumes."), std::nullopt, "Show Kill##h5tv", settings->h5TriggerOverlayShowKill));

					case GUIElementEnum::h5TriggerOverlayShowBeginZoneSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show volumes that BEGIN a zone set switch - entering one starts preparing the next zone set's BSPs."), std::nullopt, "Show Begin Zone Set##h5tv", settings->h5TriggerOverlayShowBeginZoneSet));

					case GUIElementEnum::h5TriggerOverlayShowCommitZoneSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show volumes that COMMIT a zone set switch - entering one performs the switch."), std::nullopt, "Show Zone Set Commit##h5tv", settings->h5TriggerOverlayShowCommitZoneSet));

					case GUIElementEnum::h5TriggerOverlayShowLabels:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Mark each visible volume's centre with a small sphere."), std::nullopt, "Mark Centres##h5tv", settings->h5TriggerOverlayShowLabels));

					case GUIElementEnum::h5TriggerOverlayLabelScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.5f, 64.f)>>
							(game, ToolTipCollection("Size of the volume name text."), "Name Text Size##h5tv", settings->h5TriggerOverlayLabelScale));

					case GUIElementEnum::h5TriggerOverlayLabelColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of the volume name text"), "Name Text Colour##h5tv", settings->h5TriggerOverlayLabelColor));

					case GUIElementEnum::h5TriggerOverlayHitMessages:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Print a message when you enter or exit a trigger volume. This is based on where you stand, not on a script testing it. Sector-shaped volumes are not included. Works with the overlay off."), std::nullopt, "Report Volumes Entered/Exited##h5tv", settings->h5TriggerOverlayHitMessages));

					case GUIElementEnum::h5TriggerOverlayColourByScript:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Colour volumes by script use instead of category: blue = a mission script uses it, red = nothing references it. Read from your H5_Scripts folder, not live. Kill/zone-set volumes keep their colours."), std::nullopt, "Colour By Script Activity##h5tv", settings->h5TriggerOverlayColourByScript));

					case GUIElementEnum::h5TriggerOverlayScriptedColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour for volumes a script can hit or wake"), "Script-Reachable Colour##h5tv", settings->h5TriggerOverlayScriptedColor));

					case GUIElementEnum::h5TriggerOverlayInertColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour for volumes nothing references"), "Inert Colour##h5tv", settings->h5TriggerOverlayInertColor));

					case GUIElementEnum::h5TriggerOverlayNormalColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of regular trigger volumes"), "Regular Colour##h5tv", settings->h5TriggerOverlayNormalColor));

					case GUIElementEnum::h5TriggerOverlayKillColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of kill volumes"), "Kill Colour##h5tv", settings->h5TriggerOverlayKillColor));

					case GUIElementEnum::h5TriggerOverlayBeginZoneSetColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of begin-zone-set volumes"), "Begin Zone Set Colour##h5tv", settings->h5TriggerOverlayBeginZoneSetColor));

					case GUIElementEnum::h5TriggerOverlayCommitZoneSetColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of zone-set-commit volumes"), "Zone Set Commit Colour##h5tv", settings->h5TriggerOverlayCommitZoneSetColor));

					case GUIElementEnum::h5TriggerOverlayAlpha:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection("Opacity of the filled volume faces. Zero draws wireframe only."), "Fill Opacity##h5tv", settings->h5TriggerOverlayAlpha));

					case GUIElementEnum::h5TriggerOverlayWireframeAlpha:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection("Opacity of the volume edges."), "Edge Opacity##h5tv", settings->h5TriggerOverlayWireframeAlpha));

					case GUIElementEnum::h5TriggerOverlayRenderDistance:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
							(game, ToolTipCollection("Hide volumes further than this from the camera, measured to the volume's surface rather than its centre so a volume you are standing inside is never culled."), "Render Distance##h5tv", settings->h5TriggerOverlayRenderDistance));

				case GUIElementEnum::h5InvincibilityGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Player takes no damage and cannot be killed. Re-applied every tick. Falling damage is untraced and multiplayer is untested, so treat it as campaign/Forge only."), std::nullopt, "Invincibility##h5", settings->h5InvincibilityToggle));

				case GUIElementEnum::h5InfiniteAmmoGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Weapons and grenades never run out of reserve ammo. Re-applied every frame, so it survives respawns. You still reload; use Bottomless Clip to stop that."), std::nullopt, "Infinite Ammo##h5", settings->h5InfiniteAmmoToggle));

				case GUIElementEnum::h5BottomlessClipGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The magazine never empties, so you never reload, and grenades stay topped up. Stronger than Infinite Ammo; both can be on together. A respawn resets it, so it is re-applied every frame."), std::nullopt, "Bottomless Clip##h5", settings->h5BottomlessClipToggle));

				case GUIElementEnum::h5OneShotKillGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Multiplies the damage you deal (weapon, melee and grenade) so anything dies in one hit. Outgoing damage only; incoming damage is unchanged. Re-applied every frame; a respawn resets it."), std::nullopt, "One Shot Kill##h5", settings->h5OneShotKillToggle));

				case GUIElementEnum::h5OneShotKillMultiplierGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1000.f)>>
						(game, ToolTipCollection("Damage multiplier applied to weapon, melee and grenade damage you deal. 1.0 is normal.\n\nBelow 1.0 works too, if you want a harder run rather than an easier one."), "Damage Multiplier##h5", settings->h5OneShotKillMultiplier));

				case GUIElementEnum::h5GameSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Scales the simulation speed only; rendering and the frame pacer keep real time. Suppresses scripted slow-motion while on. Above 15x the simulation stops keeping up instead of going faster."), std::nullopt, "Game Speed##h5", settings->h5GameSpeedToggle));

				case GUIElementEnum::h5GameSpeedAmountGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.01f, 15.f)>>
						(game, ToolTipCollection("Simulation speed multiplier. 1.0 is normal, below 1 is slow motion."), "Speed Multiplier##h5", settings->h5GameSpeedAmount));

				case GUIElementEnum::h5OutOfBoundsBypassGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Fly out of bounds forever without being removed or killed. Outside the broadphase you collide with nothing until you come back inside."), std::nullopt, "Havok Broadphase Deletion Bypass##h5", settings->h5OutOfBoundsBypassToggle));

				case GUIElementEnum::h5PauseMenuFixGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Removes the roughly 3 second delay before you can reopen the pause menu."), std::nullopt, "Pause Menu Fix##h5", settings->h5PauseMenuFixToggle));

				case GUIElementEnum::h5HavokOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Draw the live Havok collision geometry (static level, instanced, then dynamic objects), wireframe by default. Only collision near you is decoded; raise Radius to see more."), std::nullopt, "Havok Debugger##h5", settings->h5HavokOverlayToggle));

				case GUIElementEnum::h5HavokOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Havok debugger filters and appearance"), "Havok Debugger Settings##h5", headerChildElements
							{
								createNestedElement(GUIElementEnum::h5HavokOverlayShowStatic),
								createNestedElement(GUIElementEnum::h5HavokOverlayShowInstances),
								createNestedElement(GUIElementEnum::h5HavokOverlayShowObjects),
								createNestedElement(GUIElementEnum::h5HavokOverlayLayerFilter),
								createNestedElement(GUIElementEnum::h5HavokOverlayRadius),
								createNestedElement(GUIElementEnum::h5HavokOverlayTriangleBudget),
								createNestedElement(GUIElementEnum::h5HavokOverlayRefreshMs),
								createNestedElement(GUIElementEnum::h5HavokOverlayWireAlpha),
								createNestedElement(GUIElementEnum::h5HavokOverlayFillAlpha),
								createNestedElement(GUIElementEnum::h5HavokOverlayStaticColor),
								createNestedElement(GUIElementEnum::h5HavokOverlayObjectColor),
							}));

					case GUIElementEnum::h5HavokOverlayShowStatic:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show the static level collision mesh - the walls and floors you actually collide with."), std::nullopt, "Show World Geometry##h5hk", settings->h5HavokOverlayShowStatic));

					case GUIElementEnum::h5HavokOverlayShowInstances:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show instanced geometry placed around the level."), std::nullopt, "Show Instances##h5hk", settings->h5HavokOverlayShowInstances));

					case GUIElementEnum::h5HavokOverlayShowObjects:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Show dynamic object collision - hulls, spheres and capsules, including the player's own capsule.\n\nOff by default: these move every frame and clutter the view."), std::nullopt, "Show Object Shapes##h5hk", settings->h5HavokOverlayShowObjects));

					case GUIElementEnum::h5HavokOverlayLayerFilter:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputString>
							(game, ToolTipCollection("Which collision layers to draw. Empty draws everything (default). The overlay reports the level's layers on screen; turn layers off to find yours. Decimal or hex, comma or space separated."), "Collision Layers##h5hk", settings->h5HavokOverlayLayerFilter));

					case GUIElementEnum::h5HavokOverlayRadius:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 32767.f)>>
							(game, ToolTipCollection("How far from the camera collision is decoded, in world units (1 WU = 10 feet). 32767 means no distance culling. Past the level size the triangle budget limits what you see. Ctrl+click to type a value."), "Radius (world units)##h5hk", settings->h5HavokOverlayRadius));

					case GUIElementEnum::h5HavokOverlayTriangleBudget:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1000.f, 400000.f)>>
							(game, ToolTipCollection("Hard cap on triangles decoded per refresh. Decoding STOPS at this number, so a large radius draws an incomplete picture instead of stalling."), "Triangle Budget##h5hk", settings->h5HavokOverlayTriangleBudget));

					case GUIElementEnum::h5HavokOverlayRefreshMs:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(16.f, 5000.f)>>
							(game, ToolTipCollection("How often the collision is re-decoded, in milliseconds. Collision does not move, so decoding every frame would burn CPU for an identical picture."), "Refresh (ms)##h5hk", settings->h5HavokOverlayRefreshMs));

					case GUIElementEnum::h5HavokOverlayWireAlpha:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection("Opacity of the collision wireframe."), "Wireframe Opacity##h5hk", settings->h5HavokOverlayWireAlpha));

					case GUIElementEnum::h5HavokOverlayFillAlpha:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection("Opacity of filled collision faces. Zero (the default) draws wireframe only - solid collision is an opaque wall."), "Fill Opacity##h5hk", settings->h5HavokOverlayFillAlpha));

					case GUIElementEnum::h5HavokOverlayStaticColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of the static level collision"), "World Geometry Colour##h5hk", settings->h5HavokOverlayStaticColor));

					case GUIElementEnum::h5HavokOverlayObjectColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Colour of object collision shapes"), "Object Shape Colour##h5hk", settings->h5HavokOverlayObjectColor));

				case GUIElementEnum::h5SwitchZoneSetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIH5SwitchZoneSet>
						(game, ToolTipCollection("Switch to any zone set the current map declares. Pick one and press the button. This can load geometry you have not reached or unload what you stand on. '<- current' marks the active one."), std::nullopt, "Switch Zone Set##h5", settings->h5SwitchZoneSetEvent));

				case GUIElementEnum::forceRevertGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces a revert (without having to die or go to the pause menu)"), RebindableHotkeyEnum::forceRevert, "Force Revert", settings->forceRevertEvent));

				case GUIElementEnum::mpRevertRepairGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Multiplayer: after a revert, removes network entities of objects that no longer exist and relinks restored objects - stops the spawn-then-delete revert crash."), std::nullopt, "MP revert fix", settings->mpRevertRepairToggle));

				case GUIElementEnum::forceDoubleRevertGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Reverts you back to the checkpoint BEFORE the last checkpoint you got"), RebindableHotkeyEnum::forceDoubleRevert, "Force Double Revert", settings->forceDoubleRevertEvent));

				case GUIElementEnum::forceCoreSaveGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Core saves are exactly like checkpoints, but seperate from them (they don't override eachother)"), RebindableHotkeyEnum::forceCoreSave, "Force Core Save", settings->forceCoreSaveEvent));

				case GUIElementEnum::forceCoreLoadGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Reverts to your core save"), RebindableHotkeyEnum::forceCoreLoad, "Force Core Load", settings->forceCoreLoadEvent));

				case GUIElementEnum::injectCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Injects the selected checkpoint into the games memory in place of your last actual checkpoint"), RebindableHotkeyEnum::injectCheckpoint, "Inject Checkpoint", settings->injectCheckpointEvent));

				case GUIElementEnum::injectCheckpointSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Inject Checkpoint Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::injectCheckpointForcesRevert),
							createNestedElement(GUIElementEnum::injectCheckpointLevelCheck),
							createNestedElement(GUIElementEnum::injectCheckpointVersionCheck),
							createNestedElement(GUIElementEnum::injectCheckpointDifficultyCheck),
							createNestedElement(GUIElementEnum::injectCheckpointIgnoresChecksum)
							}));

					case GUIElementEnum::injectCheckpointForcesRevert:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically revert to the checkpoint you injected when injecting"), std::nullopt, "Force revert after injecting", settings->injectCheckpointForcesRevert));

					case GUIElementEnum::injectCheckpointLevelCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a checkpoint from a level that ISN'T from the one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong level", settings->injectCheckpointLevelCheck));

					case GUIElementEnum::injectCheckpointVersionCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a checkpoint from an MCC version that ISN'T the same one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong game version", settings->injectCheckpointVersionCheck));

					case GUIElementEnum::injectCheckpointDifficultyCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a checkpoint from a difficulty that ISN'T the same one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong difficulty", settings->injectCheckpointDifficultyCheck));

					case GUIElementEnum::injectCheckpointIgnoresChecksum:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Turns off the games checkpoint integrity check system, allowing you to inject checkpoints made by other players or from modified maps"), std::nullopt, "Ignore checkpoint checksum on revert", settings->injectionIgnoresChecksum));

				case GUIElementEnum::dumpCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Dumps your current last checkpoint to the HCM checkpoint library for later use"), RebindableHotkeyEnum::dumpCheckpoint, "Dump Checkpoint", settings->dumpCheckpointEvent));

				case GUIElementEnum::dumpCheckpointSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Dump Checkpoint Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::dumpCheckpointAutonameGUI),
							createNestedElement(GUIElementEnum::dumpCheckpointForcesSave)
							}));

					case GUIElementEnum::dumpCheckpointAutonameGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically name dumped checkpoints with a timestamp instead of prompting you to input a name"), std::nullopt, "Autoname checkpoints", settings->autonameCheckpoints));

					case GUIElementEnum::dumpCheckpointForcesSave:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically force a new checkpoint before dumping it"), std::nullopt, "Force checkpoint before dumping", settings->dumpCheckpointForcesSave));

				// Halo Campaign Evolved's own checkpoint dump. Deliberately reuses dumpCheckpointEvent (and so the
				// existing dumpCheckpoint hotkey) and the autonameCheckpoints setting - HaloCER and the MCC games
				// can never coexist in one process, so exactly one listener exists at a time. "##hce" keeps the
				// visible label but gives imgui a unique ID. There is no "force checkpoint before dumping" here:
				// the dump writes the checkpoint the player ALREADY has, which HCM shadow-copies as the game hands
				// it to its storage provider - press Force Checkpoint first if you want a fresh one.
				case GUIElementEnum::hceDumpCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Dumps the checkpoint you're on to the save folder chosen in HCMExternal's Halo Campaign Evolved tab, or <HCM folder>/HaloCER Checkpoints/. It copies the last checkpoint the game made."), RebindableHotkeyEnum::dumpCheckpoint, "Dump Checkpoint##hce", settings->dumpCheckpointEvent));

				case GUIElementEnum::hceDumpCheckpointSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Dump Checkpoint Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceDumpCheckpointAutonameGUI),
							createNestedElement(GUIElementEnum::hceDumpCheckpointShadowGUI),
							}));

					case GUIElementEnum::hceDumpCheckpointAutonameGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Automatically name dumped checkpoints after the level and the current time, instead of prompting you to input a name"), RebindableHotkeyEnum::hceAutonameCheckpointsHotkey, "Autoname checkpoints##hce", settings->autonameCheckpoints));

					// The only thing that makes dumping possible at all on this game: Halo Campaign Evolved keeps
					// its checkpoints outside the process, so HCM has to copy each one as it goes past. Turning
					// this off means no hook is installed and no bytes are copied - and nothing to dump.
					case GUIElementEnum::hceDumpCheckpointShadowGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Keeps a copy of your latest checkpoint in memory so it can be dumped without making a new one. Costs about half a millisecond per checkpoint. Off means there is nothing to dump."), RebindableHotkeyEnum::hceShadowCheckpointsHotkey, "Keep last checkpoint ready to dump##hce", settings->hceShadowCheckpoints));

				// Halo Campaign Evolved's own checkpoint injection. Deliberately reuses injectCheckpointEvent (and so
				// the existing injectCheckpoint hotkey) and MCC's three warning settings - HaloCER and the MCC games
				// can never coexist in one process, so exactly one listener exists at a time. "##hce" keeps the
				// visible label but gives imgui a unique ID. The file is picked with a normal Windows file dialog
				// unless HCMExternal is SHOWING its Halo Campaign Evolved tab with a usable checkpoint selected - unlike the
				// dump folder, the checkpoint selection is still one visible-tab slot. See HCEInjectCheckpoint.h.
				//
				// There is no "ignore checkpoint checksum" element here, and there cannot be: the revert calls the
				// verifier with a2 = 0 (19DBB8: xor edx, edx), so the 0xBB x 20 bypass sentinel is unreachable. HCM
				// recomputes the digest on every injection instead.
				case GUIElementEnum::hceInjectCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Replaces the game's current checkpoint with one you dumped earlier, so your next revert loads it. Uses the checkpoint selected in HCMExternal, or opens a file browser. Checks level and difficulty."), RebindableHotkeyEnum::injectCheckpoint, "Inject Checkpoint##hce", settings->injectCheckpointEvent));

				case GUIElementEnum::hceInjectCheckpointSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Inject Checkpoint Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceInjectCheckpointForcesRevert),
							createNestedElement(GUIElementEnum::hceInjectCheckpointLevelCheck),
							createNestedElement(GUIElementEnum::hceInjectCheckpointDifficultyCheck),
							createNestedElement(GUIElementEnum::hceInjectCheckpointVersionCheck),
							createNestedElement(GUIElementEnum::hceInjectCheckpointRewriteIdentity),
							}));

					case GUIElementEnum::hceInjectCheckpointForcesRevert:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Automatically revert to the checkpoint right after injecting it. DANGER: a checkpoint the engine rejects restarts the level and your run is gone, with no gap to notice first."), RebindableHotkeyEnum::hceInjectCheckpointForcesRevertHotkey, "Force revert after injecting##hce", settings->injectCheckpointForcesRevert));

					case GUIElementEnum::hceInjectCheckpointLevelCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Warns with an 'are you sure?' popup if the checkpoint you inject is from a different level or part of one. Leave ON: a mismatch restarts the level on your next revert."), RebindableHotkeyEnum::hceInjectCheckpointLevelCheckHotkey, "Warn on injecting to wrong level##hce", settings->injectCheckpointLevelCheck));

					case GUIElementEnum::hceInjectCheckpointDifficultyCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Warns with an 'are you sure?' popup if the injected checkpoint was made on a different difficulty or skulls. Leave ON: a mismatch restarts the level on your next revert."), RebindableHotkeyEnum::hceInjectCheckpointDifficultyCheckHotkey, "Warn on injecting to wrong difficulty##hce", settings->injectCheckpointDifficultyCheck));

					case GUIElementEnum::hceInjectCheckpointRewriteIdentity:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Makes checkpoints shared between players work by rewriting the saver's identity to yours on injection. Without it they load, then kill you seconds later. Leave ON."), RebindableHotkeyEnum::hceInjectCheckpointRewriteIdentityHotkey, "Re-identify shared checkpoints as mine##hce", settings->hceInjectCheckpointRewriteIdentity));

					case GUIElementEnum::hceInjectCheckpointVersionCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Warns with an 'are you sure?' popup if the injected checkpoint came from a different Halo Campaign Evolved build. Leave ON: a mismatch is the likeliest cause of a level restart."), RebindableHotkeyEnum::hceInjectCheckpointVersionCheckHotkey, "Warn on injecting from wrong game build##hce", settings->injectCheckpointVersionCheck));

				case GUIElementEnum::injectCoreGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Injects the selected core save into the games memory in place of your last actual core save"), RebindableHotkeyEnum::injectCore, "Inject Core Save", settings->injectCoreEvent));

				case GUIElementEnum::injectCoreSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Inject Core Save Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::injectCoreForcesRevert),
							createNestedElement(GUIElementEnum::injectCoreLevelCheck),
							createNestedElement(GUIElementEnum::injectCoreVersionCheck),
							createNestedElement(GUIElementEnum::injectCoreDifficultyCheck),
							}));

					case GUIElementEnum::injectCoreForcesRevert:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically revert to the core save you injected when injecting"), std::nullopt, "Force core load after injecting", settings->injectCoreForcesRevert));

					case GUIElementEnum::injectCoreLevelCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a core save from a level that ISN'T from the one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong level", settings->injectCoreLevelCheck));

					case GUIElementEnum::injectCoreVersionCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a core save from an MCC version that ISN'T the same one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong game version", settings->injectCoreVersionCheck));

					case GUIElementEnum::injectCoreDifficultyCheck:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("If you try to inject a core save from a difficulty that ISN'T the same one you're playing on, will warn you with an \"are you sure?\" popup"), std::nullopt, "Warn on injecting to wrong difficulty", settings->injectCoreDifficultyCheck));


				case GUIElementEnum::dumpCoreGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Dumps your current last core save to the HCM checkpoint library for later use"), RebindableHotkeyEnum::dumpCore, "Dump Core Save", settings->dumpCoreEvent));

				case GUIElementEnum::dumpCoreSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Dump Core Save Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::dumpCoreAutonameGUI),
							createNestedElement(GUIElementEnum::dumpCoreForcesSave)
							}));

					case GUIElementEnum::dumpCoreAutonameGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically name dumped core saves with a timestamp instead of prompting you to input a name"), std::nullopt, "Autoname core saves", settings->autonameCoresaves));

					case GUIElementEnum::dumpCoreForcesSave:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically force a new core save before dumping it"), std::nullopt, "Force core save before dumping", settings->dumpCoreForcesSave));

				case GUIElementEnum::naturalCheckpointDisableGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disables naturally occuring checkpoints (will break cutscenes)"), RebindableHotkeyEnum::naturalCheckpointDisable, "Disable Natural Checkpoints", settings->naturalCheckpointDisable));

				// Halo Campaign Evolved version. Same setting (and so the same hotkey and preset entry) as above -
				// only one of the two elements can ever exist in a given process. Forced checkpoints still work
				// while this is on, matching the tool this was ported from.
				case GUIElementEnum::hceNaturalCheckpointDisableGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disables naturally occuring checkpoints (will break cutscenes). Force Checkpoint still works while this is on."), RebindableHotkeyEnum::naturalCheckpointDisable, "Disable Natural Checkpoints##hce", settings->naturalCheckpointDisable));

				case GUIElementEnum::forceFutureCheckpointGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Useful for forcing checkpoints in coop, as to avoid a desync both players need to force a checkpoint on the exact same game tick"), "Force Future Checkpoint (co-op)", headerChildElements
							{
							createNestedElement(GUIElementEnum::forceFutureCheckpointToggle),
							createNestedElement(GUIElementEnum::forceFutureCheckpointTick),
							createNestedElement(GUIElementEnum::forceFutureCheckpointFill)
							}));

				case GUIElementEnum::forceFutureCheckpointToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("When ticked, HCM will force a checkpoint on the below tick"), std::nullopt, "Force future checkpoint", settings->forceFutureCheckpointToggle));

				case GUIElementEnum::forceFutureCheckpointTick:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
						(game, ToolTipCollection("Which tick to force a checkpoint on (make sure you use the same value as your coop partner)"), "on tick:", settings->forceFutureCheckpointTick));

				case GUIElementEnum::forceFutureCheckpointFill:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Sets the tick for 5 seconds from now, giving you enough time to communicate with your coop partner which tick to use"), std::nullopt, "Set for 5s from now", settings->forceFutureCheckpointFillEvent));

				case GUIElementEnum::forceMissionRestartGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Forces the mission to restart, same as doing it from the pause menu"), RebindableHotkeyEnum::forceMissionRestart, "Force Mission Restart", settings->forceMissionRestartEvent));

				case GUIElementEnum::replayInputSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Deterministic per-tick input record/playback (.HaloReplay). Recording starts on the next revert and saves that checkpoint inside the replay file; Play injects it, then your revert starts playback."), "Input Replay (.HaloReplay)", headerChildElements
							{
							createNestedElement(GUIElementEnum::replayRecord30GUI),
							createNestedElement(GUIElementEnum::replayStopSaveGUI),
							createNestedElement(GUIElementEnum::replayLoadFileGUI),
							createNestedElement(GUIElementEnum::replayPlayGUI),
							createNestedElement(GUIElementEnum::replayStopPlaybackGUI),
							}));

					case GUIElementEnum::replayRecord30GUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Arm recording. Capture begins on your next revert/checkpoint-load, at the tick rate the game is running then (detected automatically)."), RebindableHotkeyEnum::replayRecord30Hotkey, "Record", settings->replayRecord30Event));

					case GUIElementEnum::replayRecord60GUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Arm recording at 60 tickrate. Capture begins on your next revert/checkpoint-load."), RebindableHotkeyEnum::replayRecord60Hotkey, "Record (60 tick)", settings->replayRecord60Event));

					case GUIElementEnum::replayStopSaveGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Stop recording and save it (auto-named with a timestamp) to <HCM dir>\\replays\\."), RebindableHotkeyEnum::replayStopSaveHotkey, "Stop & Save Recording", settings->replayStopSaveEvent));

					case GUIElementEnum::replayLoadFileGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Browse for and load a .HaloReplay file into memory (does not start playback). Older .h2r files load too."), RebindableHotkeyEnum::replayLoadFileHotkey, "Load .HaloReplay File", settings->replayLoadFileEvent));

					case GUIElementEnum::replayPlayGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Play the loaded replay. Real input is blocked and recorded inputs are fed from the revert. An older replay needs its matching checkpoint loaded by hand. Playback starts on your revert."), RebindableHotkeyEnum::replayPlayHotkey, "Play Replay", settings->replayPlayEvent));

					case GUIElementEnum::replayStopPlaybackGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Stop playback and restore real input."), RebindableHotkeyEnum::replayStopPlaybackHotkey, "Stop Playback", settings->replayStopPlaybackEvent));


			case GUIElementEnum::cheatsHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Stuff like invulnerabiltiy, speedhack, teleport, ai disable etc"), "Useful Cheats", headerChildElements
						{
							createNestedElement(GUIElementEnum::speedhackGUI),
							createNestedElement(GUIElementEnum::hceGameSpeedGUI),
							// Havok Debugger sits between speedhack and invulnerability by request - it is a
							// HaloCER-facing tool and belongs near the top rather than buried among the MCC
							// rendering fixes below.
							createNestedElement(GUIElementEnum::havokDebuggerGUI),
							createNestedElement(GUIElementEnum::havokWorldCacheGUI),
							createNestedElement(GUIElementEnum::havokWorldCacheClearGUI),
							createNestedElement(GUIElementEnum::invulnGUI),
							createNestedElement(GUIElementEnum::invulnerabilitySettingsSubheading),
							createNestedElement(GUIElementEnum::infiniteAmmoGUI),
							createNestedElement(GUIElementEnum::bottomlessClipGUI),
							createNestedElement(GUIElementEnum::halo3TheaterInterpToggleGUI),
							createNestedElement(GUIElementEnum::halo3TheaterInterpCrouchGUI),
							createNestedElement(GUIElementEnum::season7PhysicsToggle),
							createNestedElement(GUIElementEnum::farClipDistanceGUI),
							createNestedElement(GUIElementEnum::sunScaleFixToggle),
							createNestedElement(GUIElementEnum::fpScaleFixToggle),
							createNestedElement(GUIElementEnum::fpScaleFixLegSizeGUI),
							createNestedElement(GUIElementEnum::fpScaleFixLegTuckGUI),
							createNestedElement(GUIElementEnum::animationFixesToggle),
							createNestedElement(GUIElementEnum::masterTickrateToggleGUI),   // no arming checkbox: these always apply
							createNestedElement(GUIElementEnum::masterTickrateCustomGUI),
							createNestedElement(GUIElementEnum::aiFreezeGUI),
							createNestedElement(GUIElementEnum::hceAiFreezeGUI),
							createNestedElement(GUIElementEnum::medusaGUI),
							createNestedElement(GUIElementEnum::forceTeleportGUI),
							createNestedElement(GUIElementEnum::forceTeleportSettingsSubheading),
							createNestedElement(GUIElementEnum::forceLaunchGUI),
							createNestedElement(GUIElementEnum::forceLaunchSettingsSubheading),
							// Launch before Teleport, by request.
							createNestedElement(GUIElementEnum::hceForceLaunchGUI),
							createNestedElement(GUIElementEnum::hceForceLaunchSettingsSubheading),
							createNestedElement(GUIElementEnum::hceForceTeleportGUI),
							createNestedElement(GUIElementEnum::hceForceTeleportSettingsSubheading),
							createNestedElement(GUIElementEnum::switchBSPGUI),
							createNestedElement(GUIElementEnum::mccSwitchZoneSetGUI),
							createNestedElement(GUIElementEnum::setPlayerHealthSubheadingGUI),
							createNestedElement(GUIElementEnum::skullToggleGUI),
							createNestedElement(GUIElementEnum::hceSkullToggleGUI),
							createNestedElement(GUIElementEnum::playerPositionToClipboardGUI),
							createNestedElement(GUIElementEnum::consoleCommandGUI),
							createNestedElement(GUIElementEnum::consoleCommandSettings),
							createNestedElement(GUIElementEnum::disableBarriersToggle),
							createNestedElement(GUIElementEnum::hceDisableBarriersGUI),
							createNestedElement(GUIElementEnum::havokBroadphaseBypassGUI),
							createNestedElement(GUIElementEnum::hceSkyFixGUI),
							createNestedElement(GUIElementEnum::hceDisableFadeFromBlackGUI),
							createNestedElement(GUIElementEnum::soundClassGainAdjusterToggle),
							createNestedElement(GUIElementEnum::soundClassGainAdjusterSettings),
							createNestedElement(GUIElementEnum::dropShadowsOnObjectsToggle),
							createNestedElement(GUIElementEnum::sphereSpecularForceToggle),
							createNestedElement(GUIElementEnum::offscreenShadowCastersToggle),
							createNestedElement(GUIElementEnum::offscreenShadowCastersMultiplierGUI),
							createNestedElement(GUIElementEnum::uncapDropShadowsToggle),
							createNestedElement(GUIElementEnum::uncapVisibilityLimitsToggle),
							createNestedElement(GUIElementEnum::uncapClusterLimitToggle),
							createNestedElement(GUIElementEnum::uncapRenderSectionsToggle),
							createNestedElement(GUIElementEnum::h2ShadowResolutionCombo),
							createNestedElement(GUIElementEnum::h2ArmorColourToggle),
							createNestedElement(GUIElementEnum::h2ArmorColourPrimaryPicker),
							createNestedElement(GUIElementEnum::h2ArmorColourSecondaryPicker),
							createNestedElement(GUIElementEnum::h2ArmorColourSavePresetButton),
							createNestedElement(GUIElementEnum::h2ArmorColourLoadPresetButton),
							createNestedElement(GUIElementEnum::h2ArmorEmblemToggle),
							createNestedElement(GUIElementEnum::h2ArmorEmblemLoadButton),
						}));

				case GUIElementEnum::speedhackGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISpeedhack>
						(game, ToolTipCollection("Multiply the games clock speed by this value (1 = normal speed). \nFor non-stuttery framerates at low speedhack values, make sure the game is running with an uncapped framerate."), RebindableHotkeyEnum::speedhack, settings));

				case GUIElementEnum::invulnGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInvulnerability>
						(game, ToolTipCollection("Makes the player invulnerable to all damage and kill triggers"), RebindableHotkeyEnum::invuln, "Invulnerability", settings->invulnerabilityToggle
							)); 


				case GUIElementEnum::invulnerabilitySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "InvulnerabilitySettings", headerChildElements
							{
								createNestedElement(GUIElementEnum::invulnNPCGUI),
							}));

					case GUIElementEnum::invulnNPCGUI:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection("Makes all non-player characters invulnerable as well"), std::nullopt, "NPC's invulnerable too", settings->invulnerabilityNPCToggle
								));

				case GUIElementEnum::infiniteAmmoGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Gives the player infinite ammo (doesn't consume on reload) and infinite grenades"), RebindableHotkeyEnum::infiniteAmmo, "Infinite Ammo", settings->infiniteAmmoToggle
						));

				case GUIElementEnum::bottomlessClipGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("The player no longer has to reload"), RebindableHotkeyEnum::bottomlessClip, "Bottomless Clip", settings->bottomlessClipToggle
						));

				case GUIElementEnum::season7PhysicsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: swaps the physics/tickrate scalar to the Season 7 value while enabled, restoring the default when disabled."), std::nullopt, "Season 7 Physics", settings->season7PhysicsToggle
						));

				case GUIElementEnum::dropShadowsOnObjectsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: lets objects receive the drop-shadow blobs cast by other objects (e.g. the player's shadow falling onto a nearby crate). Enables a feature the engine ships disabled."), std::nullopt, "Drop Shadows On Objects", settings->dropShadowsOnObjectsToggle
						));

				case GUIElementEnum::sphereSpecularForceToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: forces sphere lights to always show specular highlights. Stock, they only glint during cutscenes. Reverts to stock when disabled. Build 1.3528 only."), std::nullopt, "Sphere Specular Force", settings->sphereSpecularForceToggle
						));

				case GUIElementEnum::offscreenShadowCastersToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: instanced geometry and objects keep casting projective-light shadows when off-camera. The multiplier below sets the bounding radius inflation. Build 1.3528 only."), std::nullopt, "Off-Screen Shadow Casters", settings->offscreenShadowCastersToggle
						));

				case GUIElementEnum::offscreenShadowCastersMultiplierGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloatStepper>
						(game, ToolTipCollection("Halo 2: bounding-sphere radius multiplier for off-screen shadow casters. Higher keeps shadows from further off-camera (25 is a good default). Too high in a dense scene can hit the visibility caps."), "  Off-Screen Caster Radius x", settings->offscreenShadowCastersMultiplier
						));

				case GUIElementEnum::uncapDropShadowsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: raises the 32-shadow cap to 120 and the rasterizer pool to 8MB, so many more drop-shadows render at once. Fully reverts on disable. Build 1.3528 only."), std::nullopt, "Uncap Drop Shadows", settings->uncapDropShadowsToggle
						));

				case GUIElementEnum::uncapVisibilityLimitsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: raises the visibility caps for objects, instanced geometry and building meshes from 256/128/384 to 4096, so big maps stop de-rendering chunks. Applies on next level load. Build 1.3528 only."), std::nullopt, "Uncap Visibility Limits", settings->uncapVisibilityLimitsToggle
						));

				case GUIElementEnum::uncapClusterLimitToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: raises the 128 region-clusters-per-region cap to 255, fixing chunks of large maps not rendering from high vantage points. Pairs with Uncap Visibility Limits. Offline only. Build 1.3528 only."), std::nullopt, "Uncap Cluster Limit", settings->uncapClusterLimitToggle
						));

				case GUIElementEnum::collisionViewerToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
						(game, ToolTipCollection(std::string(game == GameState(GameState::Value::Halo2) ? "Halo 2 (MCC 1.3528): collision wireframe sorted by type, Havok capsules, kill triggers, and a live overlay of Havok's per-polygon TIM values (the cached distance behind ghosting). Lines are depth-tested against the game's own scene, so your biped, AI and the gun hide them, and they never go through bloom." : "Halo 3 / Halo 3: ODST (MCC 1.3528): the same collision wireframe and colour codes as Halo 2 (world BSP, invisible surfaces, breakables, instanced geometry, kill volumes, scenery / crates / machines), Havok 4.5 capsules, and a ghost / TIM debug view: the narrow phase caches a separating distance (w) per polygon; when w over-credits the real gap the polygon is skipped and you can ghost through it. Works in unmodded play. Lines are depth-tested against the game's scene at the first-person pass.")), std::nullopt, "Collision Viewer", settings->collisionViewerToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerOcclusionGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowBspGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowInstancedGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowInvisibleGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowBreakableGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowSceneryGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowCratesGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowMachinesGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowKillTriggersGUI),
							createNestedElement(GUIElementEnum::collisionViewerShowSoftCeilingsGUI),
							createNestedElement(GUIElementEnum::collisionViewerFillsGUI),
							createNestedElement(GUIElementEnum::collisionViewerTimGUI),
							createNestedElement(GUIElementEnum::collisionViewerTimLabelsGUI),
							createNestedElement(GUIElementEnum::collisionViewerPillsGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostJumpGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostVehiclesGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostHaloGUI),
							createNestedElement(GUIElementEnum::collisionViewerCreasesGUI),
							createNestedElement(GUIElementEnum::collisionViewerCreaseFacesGUI),
							createNestedElement(GUIElementEnum::collisionViewerLineWidthGUI),
							createNestedElement(GUIElementEnum::collisionViewerDepthPullGUI),
							createNestedElement(GUIElementEnum::collisionViewerRadiusGUI),
							createNestedElement(GUIElementEnum::collisionViewerHiddenAlphaGUI),
							}));

				case GUIElementEnum::collisionViewerOcclusionGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::CollisionViewerOcclusionEnum, 260.f>>
						(game, ToolTipCollection("How lines are hidden behind the scene: InFrame_NoBloom, PostBloom_KeepsBloom, OwnCollisionDepth (the viewer's own depth), or XRay (no occlusion). InFrame turns the game's bloom off while shown."), "Occlusion", settings->collisionViewerOcclusion));

				case GUIElementEnum::collisionViewerShowBspGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Structure-BSP collision edges."), std::nullopt, "World BSP (cyan)", settings->collisionViewerShowBsp));

				case GUIElementEnum::collisionViewerShowInstancedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Instanced-geometry collision edges."), std::nullopt, "Instanced geometry (blue)", settings->collisionViewerShowInstanced));

				case GUIElementEnum::collisionViewerShowInvisibleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Collision-only surfaces (world red, instanced purple), with a faint fill."), std::nullopt, "Invisible \"*\" surfaces (red / purple)", settings->collisionViewerShowInvisible));

				case GUIElementEnum::collisionViewerShowBreakableGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Breakable surfaces; they drop out when broken."), std::nullopt, "Breakable (orange)", settings->collisionViewerShowBreakable));

				case GUIElementEnum::collisionViewerShowSceneryGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Scenery object collision (moves with the object)."), std::nullopt, "Scenery (magenta)", settings->collisionViewerShowScenery));

				case GUIElementEnum::collisionViewerShowCratesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Crate object collision."), std::nullopt, "Crates (green)", settings->collisionViewerShowCrates));

				case GUIElementEnum::collisionViewerShowMachinesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Device-machine collision (doors, lifts)."), std::nullopt, "Machines (pink)", settings->collisionViewerShowMachines));

				case GUIElementEnum::collisionViewerShowKillTriggersGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Scenario kill-trigger volumes (box + faint fill)."), std::nullopt, "Kill triggers (red-orange)", settings->collisionViewerShowKillTriggers));

				case GUIElementEnum::collisionViewerFillsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Faint fills on invisible barriers and kill-trigger volumes."), std::nullopt, "Translucent fills", settings->collisionViewerFills));

				case GUIElementEnum::collisionViewerTimGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Shows Havok's per-polygon TIM (cached minimum distance) around your capsule. White = touching, grey = measured, yellow = skipped, orange/red with '!' = over-credited (the ghosting state)."), std::nullopt, "TIM overlay", settings->collisionViewerTim));

				case GUIElementEnum::collisionViewerTimLabelsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Print the TIM value at the middle of each polygon edge."), std::nullopt, "TIM labels on edges", settings->collisionViewerTimLabels));

				case GUIElementEnum::collisionViewerPillsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The capsule Havok actually collides with, for every biped (yours yellow), drawn through walls."), std::nullopt, "Havok capsules", settings->collisionViewerPills));

				case GUIElementEnum::collisionViewerInfoPanelGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
						(game, ToolTipCollection("Small panel: mode, pair state S, the TIM table, ghost rows, kill volumes, weld zones. Tick it to choose which sections it shows (Halo 2)."), std::nullopt, "Info panel", settings->collisionViewerInfoPanel, headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerInfoGeneralGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoTimGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoGhostGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoGhostObjectsGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoGhostDetailsGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoPassGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoTravelGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoLegendGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoGridsGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoKillGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoSuperbounceGUI),
							createNestedElement(GUIElementEnum::collisionViewerInfoWeldGUI),
							}));

				case GUIElementEnum::collisionViewerInfoGeneralGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Occlusion mode, line settings and the per-category line counts at the top."), std::nullopt, "General (occlusion, widths, category counts)", settings->collisionViewerInfoGeneral));

				case GUIElementEnum::collisionViewerInfoTimGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The TIM overlay line and the per-polygon TIM table."), std::nullopt, "TIM table", settings->collisionViewerInfoTim));

				case GUIElementEnum::collisionViewerInfoGhostGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The GHOST debug line and one line per player (S, skipped, suspect, max over-credit, believed poses)."), std::nullopt, "Ghost summary", settings->collisionViewerInfoGhost));

				case GUIElementEnum::collisionViewerInfoGhostObjectsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Object pair counts, pair rows and nearby machines under each player."), std::nullopt, "Ghost objects", settings->collisionViewerInfoGhostObjects));

				case GUIElementEnum::collisionViewerInfoGhostDetailsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Havok-Halo offsets, last rewind, and diagnostic notes under each player."), std::nullopt, "Ghost details", settings->collisionViewerInfoGhostDetails));

				case GUIElementEnum::collisionViewerInfoPassGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The PASS-THROUGH line and the per-polygon pass test rows."), std::nullopt, "Pass-through rows", settings->collisionViewerInfoPass));

				case GUIElementEnum::collisionViewerInfoTravelGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The travel region rows (level and objects)."), std::nullopt, "Travel region rows", settings->collisionViewerInfoTravel));

				case GUIElementEnum::collisionViewerInfoLegendGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The two grey legend lines under the ghost section."), std::nullopt, "Color legend", settings->collisionViewerInfoLegend));

				case GUIElementEnum::collisionViewerInfoGridsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Tolerance, broadphase / MOPP grids and backstep parameters."), std::nullopt, "Level grids", settings->collisionViewerInfoGrids));

				case GUIElementEnum::collisionViewerInfoKillGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The out-of-bounds timer and the kill volume rows (needs Kill volumes on)."), std::nullopt, "Out-of-bounds timer + kill volume rows", settings->collisionViewerInfoKill));

				case GUIElementEnum::collisionViewerInfoWeldGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The weld zone summary (needs a weld zone mode on)."), std::nullopt, "Weld zones", settings->collisionViewerInfoWeld));

				case GUIElementEnum::collisionViewerGhostGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
						(game, ToolTipCollection("Halo 2 ghosting: your Havok capsule turns lime while a backstep rewind is active (S > 0) or a nearby polygon is stale or over-credited, red at S = -3. More options appear when ticked."), std::nullopt, "Ghost debug (desync view)", settings->collisionViewerGhost, headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostPlayerHeadingGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostLevelHeadingGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostObjectsHeadingGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostOffsetsHeadingGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostLoggingHeadingGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostColorsHeadingGUI),
							}));

				case GUIElementEnum::collisionViewerGhostPlayerHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
						(game, ToolTipCollection("Your own capsule: rewinds, poses, trail, pass-through prediction."), "Player", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostShowRealGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostRewindGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostPrevGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostTrailGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostPassGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostLinkLabelsGUI),
							}));

				case GUIElementEnum::collisionViewerGhostLevelHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
						(game, ToolTipCollection("Level polygons: believed poses, travel regions, weld zones, desync edges."), "Level", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostBelievedGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostTravelGUI),
							createNestedElement(GUIElementEnum::collisionViewerWeldZonesNearGUI),
							createNestedElement(GUIElementEnum::collisionViewerWeldZonesWeldedGUI),
							createNestedElement(GUIElementEnum::collisionViewerWeldZoneRangeGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostEdgesGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostEdgesSuspectGUI),
							}));

				case GUIElementEnum::collisionViewerGhostObjectsHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
						(game, ToolTipCollection("Objects paired with you: crates, doors, machines, vehicles, other bipeds, baked scenery."), "Objects", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostBelievedObjectsGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostTravelObjectsGUI),
							}));

				case GUIElementEnum::collisionViewerGhostOffsetsHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
						(game, ToolTipCollection("Save your offsets together with a checkpoint and bring them back after reverting."), "Offset save state", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerOffsetSaveGUI),
							createNestedElement(GUIElementEnum::collisionViewerOffsetLoadGUI),
							createNestedElement(GUIElementEnum::collisionViewerOffsetNaturalRevertsGUI),
							}));

				case GUIElementEnum::collisionViewerGhostLoggingHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
						(game, ToolTipCollection("Desync logger."), "Logging", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostLogGUI),
							}));

				case GUIElementEnum::collisionViewerGhostColorsHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Ghost capsule colors."), "Colors", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerGhostColNormalGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostColDesyncGUI),
							createNestedElement(GUIElementEnum::collisionViewerGhostColBelievedGUI),
							}));

				case GUIElementEnum::oobBarSettingsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Where the out-of-bounds timer bars sit on screen and how big they are. Each bar is placed from the chosen screen corner: the offset is the distance from that corner to the bar's nearest corner."), "Out-of-bounds Bar Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::oobBarAnchorCornerGUI),
							createNestedElement(GUIElementEnum::oobBarScreenOffsetGUI),
							createNestedElement(GUIElementEnum::oobBarSizeGUI),
							createNestedElement(GUIElementEnum::detectBarAnchorCornerGUI),
							createNestedElement(GUIElementEnum::detectBarScreenOffsetGUI),
							createNestedElement(GUIElementEnum::detectBarSizeGUI),
							}));

				case GUIElementEnum::oobBarAnchorCornerGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
						(game, ToolTipCollection("Death bar (out of bounds / kill volume)"), "Death bar: Corner to Anchor to##oobBar", settings->oobBarAnchorCorner));

				case GUIElementEnum::oobBarScreenOffsetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 3840.f, 2160.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Death bar: Pixel Offset from Corner##oobBar", settings->oobBarScreenOffset, "Horizontal", "Vertical"));

				case GUIElementEnum::oobBarSizeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 2000.f, 200.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Death bar: Bar Size##oobBarSize", settings->oobBarSize, "Horizontal", "Vertical"));

				case GUIElementEnum::detectBarAnchorCornerGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
						(game, ToolTipCollection("Detection bar (next out-of-bounds check)"), "Detection bar: Corner to Anchor to##detectBar", settings->detectBarAnchorCorner));

				case GUIElementEnum::detectBarScreenOffsetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 3840.f, 2160.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Detection bar: Pixel Offset from Corner##detectBar", settings->detectBarScreenOffset, "Horizontal", "Vertical"));

				case GUIElementEnum::detectBarSizeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 2000.f, 200.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Detection bar: Bar Size##detectBarSize", settings->detectBarSize, "Horizontal", "Vertical"));

				case GUIElementEnum::collisionViewerSuperbounceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: shows where you will land and whether it launches. Seams are coloured by odds (green likely, orange edge, yellow lottery), with path and launch arrow. Halo 3/ODST/Reach need Halo 2 physics on."), std::nullopt, "Superbounce viewer", settings->collisionViewerSuperbounce));

				case GUIElementEnum::collisionViewerSuperbounceSeamTextGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Shows the viewer's text in the world: seam verdicts, your landing's verdict and drift hint, and the squeeze-loaded note. Off: only the colours and launch arrows with speed and height."), std::nullopt, "Seam info text", settings->collisionViewerSuperbounceSeamText));

				case GUIElementEnum::collisionViewerSuperbounceSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Your current speed (u/s, the whole velocity) just above the CHARGED / not charged indicator at the bottom of the screen."), std::nullopt, "Speed above the charge indicator", settings->collisionViewerSuperbounceSpeed
						));

				case GUIElementEnum::collisionViewerSuperbounceObjectsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Include objects (scenery, crates, machines) in the superbounce viewer: their seams and surfaces in launch checks. Off: only the level's own collision is used, with no cost from objects."), std::nullopt, "Include objects", settings->collisionViewerSuperbounceObjects
						));

				case GUIElementEnum::collisionViewerSuperbounceDebounceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Flags landings and seams where a late contact would start so far below the floor (0.19+) that you fall through instead of launching. A LOTTERY needing it turns red; LIKELY / EDGE notes the risk."), std::nullopt, "Debounce predictor", settings->collisionViewerSuperbounceDebounce
						));

				case GUIElementEnum::collisionViewerSuperbounceSeamsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Draws the viewer's seams, path, landing circle, launch arrow and contact pairs. Off: only monster/dreadnought spots and the CHARGED indicator show. Charge options, recording and scans still work."), std::nullopt, "Seams, path and landing", settings->collisionViewerSuperbounceSeams
						));

				case GUIElementEnum::collisionViewerSuperbounceSpotsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Rings, arrows and labels at places a landing launched you 20+ u/s (35+: a dreadnought), with how often they launched and the arrival speeds that give the best odds. Hidden, they are still recorded."), std::nullopt, "Remembered monster / dreadnought spots", settings->collisionViewerSuperbounceSpots
						));

				case GUIElementEnum::collisionViewerSuperbouncePredictedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Shows predicted monster (20+ u/s) and dreadnought (35+ u/s) launch spots as dashed rings, with arrows for approach heading and launch, fall speed, drift and odds. Scanned per map in background."), std::nullopt, "Predicted monster / dreadnought spots", settings->collisionViewerSuperbouncePredicted
						));

				case GUIElementEnum::collisionViewerSuperbounceRecordGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Writes superbounce_log.csv in the game's folder: per-tick position, velocity and nearest seam, each launch, touched triangles, fins, viewer verdicts and live contact points."), std::nullopt, "Record bounces (superbounce_log.csv)", settings->collisionViewerSuperbounceRecord));

				case GUIElementEnum::collisionViewerSuperbounceHighlightGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Marks the whole map's candidates: monster gaps (magenta) and fins (orange; a dreadnought when the launch estimate is 35+ u/s), with launch direction and headroom. Work in progress."), std::nullopt, "Highlight possible monster bounces (WIP)", settings->collisionViewerSuperbounceHighlight));

				case GUIElementEnum::collisionViewerSuperbounceKeepChargeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: a crouch charge survives checkpoints and reverts; reverting restores it if you were charged then or when that checkpoint was made. Writes game memory. Halo 3/ODST/Reach need Halo 2 physics on."), std::nullopt, "Maintain charge state on checkpoints and reverts", settings->collisionViewerSuperbounceKeepCharge));

				case GUIElementEnum::collisionViewerInfoSuperbounceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The superbounce viewer's section: the charge, the sweep speed, your jump take-off, air control, the last launch and its verdict (needs Superbounce viewer on)."), std::nullopt, "Superbounce##collisionViewerInfoSuperbounce", settings->collisionViewerInfoSuperbounce));

				case GUIElementEnum::collisionViewerSuperbounceForceChargeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: keeps you crouch-charged at all times by setting your pair with the level to -3 whenever it exists and is not. Overrides Maintain charge state. Works without the viewer. Writes game memory."), std::nullopt, "Force Charged state", settings->collisionViewerSuperbounceForceCharge));

				case GUIElementEnum::collisionViewerSuperbounceSettingsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the superbounce viewer, and its charge options (these work without the viewer itself)"), "Superbounce Viewer Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceSeamTextGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceSpeedGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceDebounceGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceObjectsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceSeamsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceSpotsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedRangeGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedMaxApproachGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedMonstersGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedDreadnoughtsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedHeightsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbouncePredictedHideNarrowGUI),
							createNestedElement(GUIElementEnum::collisionViewerSpotScanRadiusGUI),
							createNestedElement(GUIElementEnum::collisionViewerSpotScanSlowGUI),
							createNestedElement(GUIElementEnum::collisionViewerSpotScanCurrentTickGUI),
							createNestedElement(GUIElementEnum::collisionViewerSpotScanButtonGUI),
							createNestedElement(GUIElementEnum::collisionViewerRecipeButtonGUI),
							createNestedElement(GUIElementEnum::collisionViewerRecipeRedoButtonGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceRecordGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceHighlightGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceKeepChargeGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceForceChargeGUI),
							}));

				case GUIElementEnum::collisionViewerKillVolumesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: on-screen bar for the out-of-bounds kill. Appears full when the game starts counting, drains to red over 16 ticks to the kill; vanishes if the count resets. Info panel shows exact numbers."), std::nullopt, "Out-of-bounds timer bar", settings->collisionViewerKillVolumes));

				case GUIElementEnum::collisionViewerOffsetNaturalRevertsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Off: only Load Offsets restores them; dying or reverting from the pause menu resets them like normal. On: any revert that puts you back where the offsets were saved restores them too."), std::nullopt, "Restore offsets after natural reverts too", settings->collisionViewerOffsetNaturalReverts));

				case GUIElementEnum::collisionViewerGhostBelievedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Shows where each over-credited polygon's cached TIM believes you are: your capsule pushed away from that polygon, with a line back to the real capsule (up to 6)."), std::nullopt, "Level believed poses", settings->collisionViewerGhostBelieved));

				case GUIElementEnum::collisionViewerGhostLinkLabelsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: labels each line between your capsule and a believed pose with its length in world units, which is how much farther the cached TIM thinks the surface is than it really is."), std::nullopt, "Believed pose distances", settings->collisionViewerGhostLinkLabels));

				case GUIElementEnum::collisionViewerOffsetSaveGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Halo 2 (Ghost debug on): snapshots every TIM you hold, forces a checkpoint, then writes the snapshot back so you keep your offsets. Uses HCM's own Force Checkpoint."), RebindableHotkeyEnum::offsetSave, "Save Offsets (+ checkpoint)", settings->offsetSaveEvent));

				case GUIElementEnum::collisionViewerOffsetLoadGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Halo 2, needs Ghost debug: forces a revert to the checkpoint made by Save Offsets, then writes the saved TIMs back into the matching pairs. Nothing is written if a newer checkpoint replaced it."), RebindableHotkeyEnum::offsetLoad, "Load Offsets (revert)", settings->offsetLoadEvent));

				case GUIElementEnum::collisionViewerGhostPassGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: flashes green every welded polygon or object piece your current offset lets you pass through. The info panel lists the numbers per offset polygon."), std::nullopt, "Pass-through prediction", settings->collisionViewerGhostPass));

				case GUIElementEnum::collisionViewerGhostTravelGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2, needs Ghost debug: for up to 6 over-credited polygons, shows what keeps the over-credit alive. Box = weld zone (keep the cross inside); red sheet = depth where the TIM reaches 0."), std::nullopt, "Level travel regions", settings->collisionViewerGhostTravel));

				case GUIElementEnum::collisionViewerGhostTravelObjectsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2, needs Ghost debug: travel region for over-credited object TIMs (up to 6). Red sheet = TIM reset plane; box = where your capsule centre must stay. Leaving it resets every TIM on that object."), std::nullopt, "Object travel regions", settings->collisionViewerGhostTravelObjects));

				case GUIElementEnum::collisionViewerWeldZonesNearGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2 (on foot): draws weld zones of nearby level polygons and rigid bodies; a welder child exists while the white cross (capsule centre) is inside. Within 'Weld zone range'."), std::nullopt, "Weld zones (nearby)", settings->collisionViewerWeldZonesNear));

				case GUIElementEnum::collisionViewerWeldZonesWeldedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2 (on foot): weld zones of only the polygons and objects currently welded to you. Cheaper than the nearby view; ignores the range."), std::nullopt, "Weld zones (welded only)", settings->collisionViewerWeldZonesWelded));

				case GUIElementEnum::collisionViewerSuperbouncePredictedMonstersGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Predicted spots: draw the MONSTERS (ceiling 20-35 u/s, cyan rings)."), std::nullopt, "Predicted monsters (20-35)", settings->collisionViewerSuperbouncePredictedMonsters
						));

				case GUIElementEnum::collisionViewerSuperbouncePredictedDreadnoughtsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Predicted spots: draw the DREADNOUGHTS (ceiling 35+ u/s, white rings)."), std::nullopt, "Predicted dreadnoughts (35+)", settings->collisionViewerSuperbouncePredictedDreadnoughts
						));

				case GUIElementEnum::collisionViewerSuperbouncePredictedHeightsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Predicted spots: also shows the start height band of the approach, meaning how high your feet are above the landing surface at the start of the landing tick for the launch to work."), std::nullopt, "Show start heights", settings->collisionViewerSuperbouncePredictedHeights
						));

				case GUIElementEnum::collisionViewerSuperbouncePredictedHideNarrowGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Predicted spots: hide monsters and dreadnoughts whose window is under 1% of tested approaches (fewer rings). Off: show all, narrow ones thin and dim. A narrow window is a precise approach, not luck."), std::nullopt, "Hide narrow windows (under 1%)", settings->collisionViewerSuperbouncePredictedHideNarrow
						));

				case GUIElementEnum::collisionViewerSpotScanRadiusGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 4096.f)>>
						(game, ToolTipCollection("Scan around me: the geometry within this distance of you (wu) is scanned (a sphere)."), "Spot scan radius (wu)", settings->collisionViewerSpotScanRadius));

				case GUIElementEnum::collisionViewerSpotScanSlowGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Scan around me: test EVERY fall speed from 4 to 50 u/s (slow approaches and very fast plunges) instead of 10/22/30/35/40. About 10x slower."), std::nullopt, "Slow mode (4-50 u/s)", settings->collisionViewerSpotScanSlow
						));

				case GUIElementEnum::collisionViewerSpotScanCurrentTickGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Scan and show predictions at the game's current tick rate (e.g. 60) instead of 30. Each tick rate has its own prediction file. The launch model was validated at 30 tick; 60-tick is less certain."), std::nullopt, "Use current tick rate", settings->collisionViewerSpotScanCurrentTick
						));

				case GUIElementEnum::collisionViewerSpotScanButtonGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Scans the geometry around you for monster / dreadnought spots on the GPU in the background, then merges results into this map's prediction file. Uses the radius, slow mode and tick rate options above."), std::nullopt, "Scan around me", settings->collisionViewerSpotScanEvent));

				case GUIElementEnum::collisionViewerRecipeRedoButtonGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Re-works the approaches for every predicted spot within the Predicted spots range, replacing saved ones. Use for spots worked out by an older HCM. Uses the GPU tool; never runs on its own."), std::nullopt, "Redo approaches here", settings->collisionViewerRecipeRedoEvent));

				case GUIElementEnum::collisionViewerRecipeButtonGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Works out approach and launch direction, heading, fall and drift ranges and start heights for each predicted spot within the Predicted spots range. Runs in background, saved per map. Never automatic."), std::nullopt, "Work out approaches", settings->collisionViewerRecipeEvent));

				case GUIElementEnum::collisionViewerSuperbouncePredictedRangeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(10.f, 1000.f)>>
						(game, ToolTipCollection("Predicted monster / dreadnought spots: only spots within this distance of you (wu) are drawn, nearest first (at most 800). Raise it on big maps to find them."), "Predicted spots range (wu)", settings->collisionViewerSuperbouncePredictedRange));

				case GUIElementEnum::collisionViewerSuperbouncePredictedMaxApproachGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(5.f, 100.f)>>
						(game, ToolTipCollection("Hides predicted monster/dreadnought spots whose approach needs a landing speed above this (u/s, falling plus moving). 100 = no limit. Press Work out approaches for exact numbers."), "Monster approach speed (u/s)", settings->collisionViewerSuperbouncePredictedMaxApproach));

				case GUIElementEnum::collisionViewerWeldZoneRangeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 20.f)>>
						(game, ToolTipCollection("Weld zones (nearby): only zones within this distance of the white cross are drawn. 0 = only zones you are inside."), "Weld zone range (wu)", settings->collisionViewerWeldZoneRange));

				case GUIElementEnum::collisionViewerGhostBelievedObjectsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: believed poses for TIMs against objects, drawn orange: baked scenery plus every rigid body pair (crates, vehicles, other bipeds). Suspects turn the capsule lime and go to the desync logger."), std::nullopt, "Object believed poses", settings->collisionViewerGhostBelievedObjects));

				case GUIElementEnum::collisionViewerGhostTrailGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Havok's pose on each of the last 8 game ticks, fading; lime on ticks where a backstep was active. Rewinds show up as kinks."), std::nullopt, "Tick trail", settings->collisionViewerGhostTrail));

				case GUIElementEnum::collisionViewerShowSoftCeilingsGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 3 / ODST: the structure design soft ceilings (acceleration surfaces, soft kills, slip surfaces) of the loaded bsps, as pale-yellow triangle outlines."), std::nullopt, "Soft ceilings (pale yellow)", settings->collisionViewerShowSoftCeilings));

				case GUIElementEnum::collisionViewerGhostJumpGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 3 / ODST: shows the last position jump, where the body was moved between ticks by something other than the simulation (e.g. Force Teleport). Yellow = next step start, red-orange = previous end."), std::nullopt, "Jump markers (raw position writes)", settings->collisionViewerGhostJump));

				case GUIElementEnum::collisionViewerGhostRewindGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The last backstep rewind: yellow = where the bisector rewound you to, red-orange = where you were before, with the rewind distance and t* in the info panel."), std::nullopt, "Backstep rewind poses", settings->collisionViewerGhostRewind));

				case GUIElementEnum::collisionViewerGhostPrevGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Havok's pose at the start of the current physics step (motion PREV). Differs from the current pose by about one tick of movement while running."), std::nullopt, "Step-start pose", settings->collisionViewerGhostPrev));

				case GUIElementEnum::collisionViewerGhostHaloGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Where Halo itself thinks the biped is (object position), white, when it differs from Havok's body."), std::nullopt, "Halo object position", settings->collisionViewerGhostHalo));

				case GUIElementEnum::collisionViewerGhostEdgesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Thick lime edges on every polygon Havok is currently skipping for you (TIM > 0), thick red on stale/over-credited ones - the surfaces you are desyncing from."), std::nullopt, "Desync edges", settings->collisionViewerGhostEdges));

				case GUIElementEnum::collisionViewerGhostEdgesSuspectGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Only highlight stale / over-credited polygons (hides the routine skipped ones, e.g. floor seams)."), std::nullopt, "Offset polygons only", settings->collisionViewerGhostEdgesSuspect));

				case GUIElementEnum::collisionViewerGhostShowRealGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Draw the capsule where Havok really is. Turn off to see only the desync poses (believed / rewind / trail) without the overlap."), std::nullopt, "Show real capsule", settings->collisionViewerGhostShowReal));

				case GUIElementEnum::collisionViewerGhostColNormalGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of your real Havok capsule (and trail) while nothing is desynced."), "Capsule color", settings->collisionViewerGhostColNormal));

				case GUIElementEnum::collisionViewerGhostColDesyncGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of your real Havok capsule (and trail ticks) while a backstep is active or a nearby polygon is stale/over-credited."), "Desync capsule color", settings->collisionViewerGhostColDesync));

				case GUIElementEnum::collisionViewerGhostColBelievedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of the believed-pose capsules (where the over-credited TIMs think you are)."), "Believed pose color", settings->collisionViewerGhostColBelieved));

				case GUIElementEnum::collisionViewerGhostVehiclesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("While you're in a vehicle (any seat), show the vehicle's Havok body and its TIM/backstep desync instead of your biped's; back to your biped when you get out."), std::nullopt, "Follow vehicle when seated", settings->collisionViewerGhostVehicles));

				case GUIElementEnum::collisionViewerCreasesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Highlights concave edges where two surfaces meet at under 90 degrees. Only a first guess at desync geometry, already disproved; use the Desync logger instead. Yellow = just under 90, red = very sharp."), std::nullopt, "Acute-crease prediction (unproven)", settings->collisionViewerCreases));

				case GUIElementEnum::collisionViewerCreaseFacesGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Also outline both surfaces of each desyncable crease."), std::nullopt, "  Outline crease faces", settings->collisionViewerCreaseFaces));

				case GUIElementEnum::collisionViewerGhostLogGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("While Ghost debug is on, writes every backstep, rewind and TIM over-credit episode to GhostDesyncLog_<date>.jsonl next to HCMInternal.dll, one JSON object per line. Turning it off closes the file."), std::nullopt, "Desync logger", settings->collisionViewerGhostLog));

				case GUIElementEnum::collisionViewerLineWidthGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 8.f)>>
						(game, ToolTipCollection("Screen-space line thickness."), "Line width (px)", settings->collisionViewerLineWidth));

				case GUIElementEnum::collisionViewerDepthPullGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 0.02f)>>
						(game, ToolTipCollection("Pulls lines toward the camera by this fraction of their distance so lines lying on a surface don't flicker. Raise if lines z-fight; lower if they poke through thin walls."), "Depth pull", settings->collisionViewerDepthPull));

				case GUIElementEnum::collisionViewerRadiusGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(2.f, 4096.f)>>
						(game, ToolTipCollection("Only edges within this distance of the camera are drawn."), "Draw radius (wu)", settings->collisionViewerRadius));

				case GUIElementEnum::collisionViewerHiddenAlphaGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("Draw occluded lines faintly (0 = hide them)."), "Hidden-line opacity", settings->collisionViewerHiddenAlpha));

				case GUIElementEnum::collisionViewerReversedZGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Leave on. Static RE says Halo 2 MCC stores depth reversed (1 - z/w). If in-frame lines are ALL hidden or NEVER hidden, flip this and report which looked right."), std::nullopt, "Game depth is reversed-Z (diagnostic)", settings->collisionViewerReversedZ));

				case GUIElementEnum::uncapRenderSectionsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: fixes the player model and far scenery flickering when moving fast on big maps (e.g. Metropolis) by raising the 850 render-section limit to 4096. Applies live, offline only. Build 1.3528 only."), std::nullopt, "Uncap Render Sections", settings->uncapRenderSectionsToggle
						));

				case GUIElementEnum::chaseCameraFovGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 150.f)>>
						(game, ToolTipCollection("Field of view of the chase camera in degrees (0 = the game's own). Applied to the camera command the chase camera writes each frame, so it only affects the chase camera while Chase Camera is on."), "  Chase FOV (deg, 0 = game)", settings->chaseCameraFov));

				case GUIElementEnum::chaseCameraHeightGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(-5.f, 10.f)>>
						(game, ToolTipCollection("Raises (or lowers) the point the chase camera orbits around, in world units - the camera and what it looks at move up together."), "  Chase vertical offset (wu)", settings->chaseCameraHeight));

				case GUIElementEnum::chaseCameraNoClipGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Chase Camera: don't pull the camera in when walls/objects are between it and you (sets the observer's own 'no camera collision' command flag). Only affects the chase camera while Chase Camera is on."), std::nullopt, "  No-clip camera", settings->chaseCameraNoClip));

			// ---- Poorboy's Mods (ported from Poorboys Checkpoint Manager by Poorboy, MIT): Halo2Physics.cpp, GUIH2Physics.h, BottomlessClip.cpp ----
			case GUIElementEnum::halo2PhysicsHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Poorboy's mods for Halo 3, ODST and Reach, ported from Poorboys Checkpoint Manager: Halo 2 physics, button combos and lunges, fly, save spot, bottomless clip."), "Poorboy's Mods", headerChildElements
						{
						createNestedElement(GUIElementEnum::h2pStatusGUI),
						createNestedElement(GUIElementEnum::h2pPhysicsToggleGUI),
						createNestedElement(GUIElementEnum::h2pArmGUI),
						createNestedElement(GUIElementEnum::h2pFlyToggleGUI),
						createNestedElement(GUIElementEnum::h2pFlySpeedGUI),
						createNestedElement(GUIElementEnum::h2pFlyHoldGUI),
						createNestedElement(GUIElementEnum::h2pBarriersGUI),
						createNestedElement(GUIElementEnum::h2pTick30ToggleGUI),
						createNestedElement(GUIElementEnum::h2pCombosToggleGUI),
						createNestedElement(GUIElementEnum::h2pNoFallDeathsToggleGUI),
						createNestedElement(GUIElementEnum::h2pDisableAIGUI),
						createNestedElement(GUIElementEnum::h2pForgeBounceGUI),
						createNestedElement(GUIElementEnum::spotSaveGUI),
						createNestedElement(GUIElementEnum::spotBackGUI),
						createNestedElement(GUIElementEnum::h2pBottomlessClipGUI),
						}));

				case GUIElementEnum::h2pStatusGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIH2PhysicsStatus>
						(game, ToolTipCollection("ARMED: a fall onto the right edge bounces. Speed is the Halo 2 body's; a step has to pass 5.45 to sweep")));

				case GUIElementEnum::h2pPhysicsToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2's player physics, movement and jump. Off: the mod unloads and the game is untouched"), RebindableHotkeyEnum::h2pPhysicsHotkey, "Halo 2 physics", settings->h2pPhysicsToggle));

				case GUIElementEnum::h2pArmGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Puts the Halo 2 body in the armed state now (what standing up inside geometry does) so you can go straight to a bounce spot. Lost if you stand still long enough to sleep. Bindable with the .. button."), RebindableHotkeyEnum::h2pArmHotkey, "Arm now", settings->h2pArmEvent));

				case GUIElementEnum::h2pFlyToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Fly where you look: stick to move, jump up, crouch down, through walls. Works online in custom games (others see you fly), with Halo 2 physics on or off. Always starts off."), RebindableHotkeyEnum::h2pFlyHotkey, "Fly", settings->h2pFlyToggle));

				case GUIElementEnum::h2pFlySpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 40.f)>>
						(game, ToolTipCollection("Fly speed in world units a second (running is about 2.25)"), "Fly speed##h2p", settings->h2pFlySpeed));

				case GUIElementEnum::h2pFlyHoldGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("The Fly hotkey flies only while you hold it: let go and you drop (instead of a press switching fly on and off). Bind the Fly hotkey with its .. button"), std::nullopt, "Hold the Fly key to fly##h2p", settings->h2pFlyHoldToggle));

				case GUIElementEnum::h2pBarriersGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIH2PhysicsBarriers>
						(game, ToolTipCollection("Invisible barriers while Halo 2 physics is on. The hotkey steps through the four"), RebindableHotkeyEnum::h2pBarriersCycleHotkey, settings->h2pBarriers));

				case GUIElementEnum::h2pTick30ToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection(game.operator GameState::Value() == GameState::Value::HaloReach
							? "Reach itself always runs at 60 ticks a second: this switches only the Halo 2 physics, any time (mid-game too). On: Halo 2 at 30 ticks (Xbox), one Halo 2 step every second Reach tick: superbounces, big landings on slopes stop on ledges. Off: Halo 2 at 60 ticks (MCC Halo 2's old 60 tick): you slide down slopes, and bounces (60 tick bouncing) need much bigger drops but go much higher"
							: "Switches the game and the Halo 2 physics between 30 and 60 ticks a second, any time (mid-game too). On: Halo 2 at 30 ticks (Xbox): superbounces, big landings on slopes stop on ledges. Off: Halo 2 at 60 ticks (MCC Halo 2's old 60 tick): you slide down slopes, and bounces (60 tick bouncing) need much bigger drops but go much higher"),
							RebindableHotkeyEnum::h2pTick30Hotkey, "30 tick", settings->h2pTick30Toggle));

				case GUIElementEnum::h2pCombosToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2's button combos: melee cancels (BXR, BXB, double shot), melee lock, YX instant weapon swap, plasma pistol double grenade. With Halo 2 physics on, also Halo 2's melee and sword lunges."), RebindableHotkeyEnum::h2pCombosHotkey, "Halo 2 button combos", settings->h2pCombosToggle));

				case GUIElementEnum::h2pNoFallDeathsToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Falls never kill or hurt you (campaign and Firefight). Other damage still counts"), RebindableHotkeyEnum::h2pNoFallDeathsHotkey, "No falling deaths", settings->h2pNoFallDeathsToggle));

				case GUIElementEnum::h2pDisableAIGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Switches the AI off (ai_enable false): enemies and allies stand still and don't shoot. Theatre films replay it exactly. Works with Halo 2 physics on or off; starts off every time."), RebindableHotkeyEnum::h2pDisableAIHotkey, "Disable AI", settings->h2pDisableAIToggle));

				case GUIElementEnum::h2pForgeBounceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("With Halo 2 physics on: Forge objects count as part of the map for the Halo 2 body, so seams between Forge pieces bounce like map seams. For building your own bounce maps. Off = Halo 2's own rule."), RebindableHotkeyEnum::h2pForgeBounceHotkey, "Forge bounces", settings->h2pForgeBounceToggle));

				case GUIElementEnum::spotSaveGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Saves your place, speed, crouch, look direction and Halo 2's armed state. Back to spot returns you there. Unlike a revert it moves only your player, so it suits multiplayer jumping (online too)."), RebindableHotkeyEnum::spotSaveHotkey, "Save spot", settings->spotSaveEvent));

				case GUIElementEnum::spotBackGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Puts you back at the saved spot with the same facing and speed, armed again if you were. Others online see you there. Objects stay put; use Force Checkpoint / Revert. Bindable with the .. button."), RebindableHotkeyEnum::spotBackHotkey, "Back to spot", settings->spotBackEvent));

				case GUIElementEnum::h2pBottomlessClipGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Your magazine never empties, plasma batteries don't drain and grenades never run out (need at least one of a kind). Reserve ammo still counts; plasma weapons still overheat. Reload first."), RebindableHotkeyEnum::bottomlessClip, "Bottomless Clip", settings->bottomlessClipToggle));

				case GUIElementEnum::chaseCameraToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Forces the engine's third-person chase camera (the vehicle one) on you while on foot, in Halo 2, 3, ODST, Reach and 4. Off returns to first person. Free Camera overrides it. MCC build 1.3528 only."), RebindableHotkeyEnum::chaseCamera, "Chase Camera", settings->chaseCameraToggle
						));

				case GUIElementEnum::h2ArmorColourToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: live-recolours your MasterChief biped using the game's own colour setter, holding across respawns. Set the two colours below. Campaign or MP. Build 1.3528 only."), std::nullopt, "MasterChief Colour", settings->h2ArmorColourToggle
						));

				case GUIElementEnum::h2ArmorColourPrimaryPicker:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Primary armour colour (change-color slot 0)."), "  Primary Colour", settings->h2ArmorColourPrimary));

				case GUIElementEnum::h2ArmorColourSecondaryPicker:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Secondary / trim armour colour (change-color slot 1)."), "  Secondary Colour", settings->h2ArmorColourSecondary));

				case GUIElementEnum::h2ArmorColourSavePresetButton:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Save the current primary+secondary colours to a preset file (opens a Save dialog)."), std::nullopt, "Save Preset...", settings->h2ArmorColourSavePresetEvent));

				case GUIElementEnum::h2ArmorColourLoadPresetButton:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Load a colour preset from a file (opens an Open dialog)."), std::nullopt, "Load Preset...", settings->h2ArmorColourLoadPresetEvent));

				case GUIElementEnum::h2ArmorEmblemToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: shows a custom image on your MasterChief's emblem. Click 'Load Emblem...' to pick a PNG, animated GIF or video (loops, no audio; use a small clip). Build 1.3528 only."), std::nullopt, "Custom Emblem", settings->h2ArmorEmblemToggle
						));

				case GUIElementEnum::h2ArmorEmblemLoadButton:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Pick a PNG, animated GIF, or video (mp4/mov/mkv/...) to use as your custom emblem (opens an Open dialog). Videos loop and play without audio."), std::nullopt, "Load Emblem...", settings->h2ArmorEmblemLoadEvent));

				case GUIElementEnum::h2ShadowResolutionCombo:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::H2ShadowResolution, 90.f>>
						(game, ToolTipCollection("Halo 2: shadow-buffer resolution and detail. Retail = no changes. RetailLOD = max LOD, no object distance-culling. x2048/x4096/x8192 add that shadow res (x8192 ~512MB VRAM). Build 1.3528 only."), "Shadow / Detail", settings->h2ShadowResolutionSetting));

				case GUIElementEnum::farClipDistanceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloatStepper>
						(game, ToolTipCollection("Halo 2: renderer far clip distance. +/- steps by 512; click (or ctrl-click) the field to type a value. Synced to the live value when a game loads."), "Far Clip Distance", settings->farClipDistance
						));

				case GUIElementEnum::sunScaleFixToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: keeps the sun a constant size when Far Clip Distance is raised, instead of shrinking to a hard square at high far clip."), std::nullopt, "Sun Scale Fix", settings->sunScaleFixToggle
						));

				case GUIElementEnum::fpScaleFixToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Halo 2: renders the first-person legs/body at a fixed reference FOV so they stop stretching when you change FOV. Needs a loaded Halo 2 game; tune with the two values below."), std::nullopt, "FP Scale Fix", settings->fpScaleFixToggle
						));

				case GUIElementEnum::fpScaleFixLegSizeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloatStepper>
						(game, ToolTipCollection("FP Scale Fix reference scale (TAN_REF). Lower = bigger legs, higher = smaller. Default 0.65. Applies live while the fix is on."), "Leg Size", settings->fpScaleFixLegSize, 0.01f, 0.05f, "%.4f"
						));

				case GUIElementEnum::fpScaleFixLegTuckGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloatStepper>
						(game, ToolTipCollection("FP Scale Fix body push/tuck FOV in radians. ~1.36 = 78deg (no push/centered), higher pushes the body into the lowered pose. Default 1.5. Applies live while the fix is on."), "Leg Tuck (rad)", settings->fpScaleFixLegTuck, 0.01f, 0.05f, "%.4f"
						));

				case GUIElementEnum::animationFixesToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Halo 2: bundle of animation fixes: rocket launcher barrel and animation pops, Cyclotron elevator loop glitch, first-person leg jitter, and crouch judder at low tick rates. Re-applies on map load."), std::nullopt, "Animation Fixes", settings->animationFixesToggle
						));

				case GUIElementEnum::havokDebuggerGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Starts an in-process Havok Visual Debugger server; connect the client to 127.0.0.1:25001. Halo 3: live world. Halo 2: static BSP collision. Halo CE: live and static collision. First walk hitches."), std::nullopt, "Havok Debugger", settings->havokDebuggerToggle
						));

				case GUIElementEnum::masterTickrateEnableGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::AlwaysShowChildren, false>>
						(game, ToolTipCollection("Arms the master simulation tickrate. Off by default and not saved to presets; the 30/60 toggle and value box only work while checked. Unchecking restores stock 60 Hz. Not available for Halo 1 (CE)."), std::nullopt, "Master Tickrate", settings->masterTickrateEnabled, headerChildElements
							{
							createNestedElement(GUIElementEnum::masterTickrateToggleGUI),
							createNestedElement(GUIElementEnum::masterTickrateCustomGUI),
							}));

				case GUIElementEnum::masterTickrateToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Flips the master simulation tickrate between 60 and 30 Hz. Needs a loaded game. Games run at 60 until you pick; your last pick is re-applied on every level load and mission restart this session."), std::nullopt, "Toggle Tickrate (60/30)", settings->masterTickrateFlipEvent
						));

				case GUIElementEnum::masterTickrateCustomGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<SliderParam<int>{}, true>>
						(game, ToolTipCollection("Master tickrate in Hz, 3 to 32766; press Enter to apply (not while typing). Needs a loaded game; HCM re-applies it on every map load. Rates of 1-2 crash the game and are rejected."), "Custom Tickrate (Hz)", settings->customTickrate
						));

				case GUIElementEnum::aiFreezeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disables NPC AI, causing them to stand around like puppets"), RebindableHotkeyEnum::aiFreeze, "Freeze AI", settings->aiFreezeToggle));

				// Halo Campaign Evolved's own Freeze AI. Same setting and same hotkey as the MCC one - HaloCER and
				// MCC can never be in one process, so only one of the two cheats ever listens. "##hce" keeps the
				// visible label identical while giving imgui a unique id.
				case GUIElementEnum::hceAiFreezeGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disables NPC AI, causing them to stand around like puppets. Re-applied automatically after checkpoint reverts."), RebindableHotkeyEnum::aiFreeze, "Freeze AI##hce", settings->aiFreezeToggle));

				case GUIElementEnum::medusaGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Automatically kill enemies when they see the player"), RebindableHotkeyEnum::medusa, "Medusa", settings->medusaToggle));


				case GUIElementEnum::forceTeleportGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Teleports an objects physical position"), RebindableHotkeyEnum::forceTeleport, "Force Teleport", settings->forceTeleportEvent));

				case GUIElementEnum::forceTeleportSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Teleport Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::forceTeleportApplyToPlayer),
							createNestedElement(GUIElementEnum::forceTeleportSettingsRadioGroup),
							}));

					case GUIElementEnum::forceTeleportApplyToPlayer:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenFalse, false>>
							(game, ToolTipCollection("The teleport should be applied to the player, as opposed to some other object"), std::nullopt, "Apply to player", settings->forceTeleportApplyToPlayer, headerChildElements
								{
								createNestedElement(GUIElementEnum::forceTeleportCustomObject),
								}));

						case GUIElementEnum::forceTeleportCustomObject:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
								(game, ToolTipCollection("The datum (unique ID) of the object the teleport should be applied to"), "Custom object datum", settings->forceTeleportCustomObject));



					case GUIElementEnum::forceTeleportSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Teleport Radio Group", headerChildElements
								{
								createNestedElement(GUIElementEnum::forceTeleportForward),
								createNestedElement(GUIElementEnum::forceTeleportManual)
								}));


						case GUIElementEnum::forceTeleportForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport relative to the players position and look-direction"), std::nullopt, "Teleport Relative to Player",   settings->forceTeleportForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::forceTeleportRelativeVec3),
									createNestedElement(GUIElementEnum::forceTeleportForwardIgnoreZ)
									}));

							case GUIElementEnum::forceTeleportRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How far forward/right/up to teleport the player, relative to their look-direction"), "Teleport: ##forceTeleportRelativeVec3", settings->forceTeleportRelativeVec3, "Forward", "Right", "Up"));


							case GUIElementEnum::forceTeleportForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), std::nullopt, "Ignore vertical look angle",  settings->forceTeleportForwardIgnoreZ));


						case GUIElementEnum::forceTeleportManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport to absolute world coordinates"), std::nullopt, "Teleport to Manual Coordinates",  settings->forceTeleportManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::forceTeleportAbsoluteVec3),
									createNestedElement(GUIElementEnum::forceTeleportAbsoluteFillCurrent),
									createNestedElement(GUIElementEnum::forceTeleportAbsoluteCopy),
									createNestedElement(GUIElementEnum::forceTeleportAbsolutePaste),
									}));

							case GUIElementEnum::forceTeleportAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("The xyz world coordinates to teleport to"), "Teleport: ##forceTeleportAbsoluteVec3", settings->forceTeleportAbsoluteVec3));


							case GUIElementEnum::forceTeleportAbsoluteFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Fill with the current xyz position of the player/object"), std::nullopt, "Fill with current position",  settings->forceTeleportAbsoluteFillCurrent));

							case GUIElementEnum::forceTeleportAbsoluteCopy:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Copy the position above to your clipboard"), std::nullopt, "Copy to Clipboard", settings->forceTeleportAbsoluteCopy));

							case GUIElementEnum::forceTeleportAbsolutePaste:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Paste a position from your clipboard into above"), std::nullopt, "Paste from Clipboard", settings->forceTeleportAbsolutePaste));



				case GUIElementEnum::forceLaunchGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Boosts an objects velocity"), RebindableHotkeyEnum::forceLaunch, "Force Launch", settings->forceLaunchEvent));

				case GUIElementEnum::forceLaunchSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Launch Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::forceLaunchApplyToPlayer),
							createNestedElement(GUIElementEnum::forceLaunchSettingsRadioGroup),
							}));

					case GUIElementEnum::forceLaunchApplyToPlayer:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenFalse, false>>
							(game, ToolTipCollection("The launch should be applied to the player, as opposed to some other object"), std::nullopt, "Apply to player", settings->forceLaunchApplyToPlayer, headerChildElements
								{
								createNestedElement(GUIElementEnum::forceLaunchCustomObject)
								}));

						case GUIElementEnum::forceLaunchCustomObject:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
								(game, ToolTipCollection("The datum (unique ID) of the object the launch should be applied to"), "Custom object datum", settings->forceLaunchCustomObject));


					case GUIElementEnum::forceLaunchSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Launch Radio Group", headerChildElements
								{
								createNestedElement(GUIElementEnum::forceLaunchForward),
								createNestedElement(GUIElementEnum::forceLaunchManual)
								}));


						case GUIElementEnum::forceLaunchForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Add velocity relative to the players look-direction"), std::nullopt, "Launch relative to player facing",  settings->forceLaunchForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::forceLaunchRelativeVec3),
									createNestedElement(GUIElementEnum::forceLaunchForwardIgnoreZ)
									}));

							case GUIElementEnum::forceLaunchRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How much velocity to apply in the forward/right/up directions, relative to the players look-direction"), "Launch: ##forceLaunchRelativeVec3", settings->forceLaunchRelativeVec3, "Forward", "Right", "Up"));


							case GUIElementEnum::forceLaunchForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), std::nullopt, "Ignore vertical look angle", settings->forceLaunchForwardIgnoreZ));


						case GUIElementEnum::forceLaunchManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Add velocity relative to the absolute world-axes"), std::nullopt, "Launch with absolute velocity", settings->forceLaunchManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::forceLaunchAbsoluteVec3),
									}));


							case GUIElementEnum::forceLaunchAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How much velocity to add, in absolute world-axes"), "Launch: ##forceLaunchAbsoluteVec3", settings->forceLaunchAbsoluteVec3));


				// ---- Halo Campaign Evolved teleport / launch --------------------------------------------------
				// Mirrors the MCC layout above, minus "apply to custom object": MCC reaches an arbitrary object
				// through GetObjectPhysics, which validates the datum, and HCE has no equivalent - the write
				// sequences were reversed against the player biped specifically. Everything else, including the
				// relative-to-look-direction mode, is here. Settings, events and hotkeys are the MCC ones.
				case GUIElementEnum::hceForceTeleportGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Teleports the player, either to absolute coordinates or relative to where they are looking"), RebindableHotkeyEnum::forceTeleport, "Force Teleport##hce", settings->forceTeleportEvent));

				case GUIElementEnum::hceForceTeleportSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Teleport Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceForceTeleportSettingsRadioGroup),
							}));

					case GUIElementEnum::hceForceTeleportSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Teleport Radio Group##hce", headerChildElements
								{
								createNestedElement(GUIElementEnum::hceForceTeleportForward),
								createNestedElement(GUIElementEnum::hceForceTeleportManual)
								}));

						case GUIElementEnum::hceForceTeleportForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport relative to the players position and look-direction"), RebindableHotkeyEnum::hceForceTeleportForwardHotkey, "Teleport Relative to Player##hce", settings->forceTeleportForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::hceForceTeleportRelativeVec3),
									createNestedElement(GUIElementEnum::hceForceTeleportForwardIgnoreZ)
									}));

							case GUIElementEnum::hceForceTeleportRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How far forward/right/up to teleport the player, relative to their look-direction"), "Teleport: ##hceForceTeleportRelativeVec3", settings->forceTeleportRelativeVec3, "Forward", "Right", "Up"));

							case GUIElementEnum::hceForceTeleportForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), RebindableHotkeyEnum::hceForceTeleportForwardIgnoreZHotkey, "Ignore vertical look angle##hceTeleport", settings->forceTeleportForwardIgnoreZ));

						case GUIElementEnum::hceForceTeleportManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Teleport to absolute world coordinates"), RebindableHotkeyEnum::hceForceTeleportManualHotkey, "Teleport to Manual Coordinates##hce", settings->forceTeleportManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::hceForceTeleportAbsoluteVec3),
									createNestedElement(GUIElementEnum::hceForceTeleportAbsoluteFillCurrent),
									createNestedElement(GUIElementEnum::hceForceTeleportAbsoluteCopy),
									createNestedElement(GUIElementEnum::hceForceTeleportAbsolutePaste),
									}));

							case GUIElementEnum::hceForceTeleportAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("The xyz world coordinates to teleport to"), "Teleport: ##hceForceTeleportAbsoluteVec3", settings->forceTeleportAbsoluteVec3));

							case GUIElementEnum::hceForceTeleportAbsoluteFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
									(game, ToolTipCollection("Fill with the current xyz position of the player"), RebindableHotkeyEnum::hceForceTeleportAbsoluteFillCurrentHotkey, "Fill with current position##hce", settings->forceTeleportAbsoluteFillCurrent));

							case GUIElementEnum::hceForceTeleportAbsoluteCopy:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
									(game, ToolTipCollection("Copy the position above to your clipboard"), RebindableHotkeyEnum::hceForceTeleportAbsoluteCopyHotkey, "Copy to Clipboard##hce", settings->forceTeleportAbsoluteCopy));

							case GUIElementEnum::hceForceTeleportAbsolutePaste:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
									(game, ToolTipCollection("Paste a position from your clipboard into above"), RebindableHotkeyEnum::hceForceTeleportAbsolutePasteHotkey, "Paste from Clipboard##hce", settings->forceTeleportAbsolutePaste));

				case GUIElementEnum::hceForceLaunchGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Boosts the players velocity"), RebindableHotkeyEnum::forceLaunch, "Force Launch##hce", settings->forceLaunchEvent));

				case GUIElementEnum::hceForceLaunchSettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection(""), "Force Launch Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceForceLaunchSettingsRadioGroup),
							}));

					case GUIElementEnum::hceForceLaunchSettingsRadioGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioGroup>
							(game, ToolTipCollection(""), "Force Launch Radio Group##hce", headerChildElements
								{
								createNestedElement(GUIElementEnum::hceForceLaunchForward),
								createNestedElement(GUIElementEnum::hceForceLaunchManual)
								}));

						case GUIElementEnum::hceForceLaunchForward:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Add velocity relative to the players look-direction"), RebindableHotkeyEnum::hceForceLaunchForwardHotkey, "Launch relative to player facing##hce", settings->forceLaunchForward, headerChildElements
									{
									createNestedElement(GUIElementEnum::hceForceLaunchRelativeVec3),
									createNestedElement(GUIElementEnum::hceForceLaunchForwardIgnoreZ)
									}));

							case GUIElementEnum::hceForceLaunchRelativeVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("How much velocity to ADD in the forward/right/up directions, relative to the players look-direction"), "Launch: ##hceForceLaunchRelativeVec3", settings->forceLaunchRelativeVec3, "Forward", "Right", "Up"));

							case GUIElementEnum::hceForceLaunchForwardIgnoreZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
									(game, ToolTipCollection("Will ignore the vertical component of your look direction (ie pretends you're looking at the horizon)"), RebindableHotkeyEnum::hceForceLaunchForwardIgnoreZHotkey, "Ignore vertical look angle##hceLaunch", settings->forceLaunchForwardIgnoreZ));

						case GUIElementEnum::hceForceLaunchManual:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIRadioButton>
								(game, ToolTipCollection("Set velocity along the absolute world-axes"), RebindableHotkeyEnum::hceForceLaunchManualHotkey, "Launch with absolute velocity##hce", settings->forceLaunchManual, headerChildElements
									{
									createNestedElement(GUIElementEnum::hceForceLaunchAbsoluteVec3),
									}));

							case GUIElementEnum::hceForceLaunchAbsoluteVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, true, 8>>
									(game, ToolTipCollection("The velocity to set, in absolute world-axes. This REPLACES your current velocity rather than adding to it."), "Launch: ##hceForceLaunchAbsoluteVec3", settings->forceLaunchAbsoluteVec3));

				case GUIElementEnum::switchBSPGUI:
				{
					auto el = std::make_shared<GUIButtonAndInt<true>>
						(game, ToolTipCollection("Loads a different part of the map by index"), RebindableHotkeyEnum::switchBSP,
							((game.operator GameState::Value() == GameState::Value::Halo1 || game.operator GameState::Value() == GameState::Value::Halo2) ? "Switch BSP to" : "Switch Zone Set to"),
							"Index",
							settings->switchBSPIndex, settings->switchBSPEvent);
					// Halo 2: the BSP's name after the index, all of them on hover (2026-11-02)
					if (game.operator GameState::Value() == GameState::Value::Halo2)
						el->setNames([](int i) { return SwitchBSPNames::describe(i); }, []() { return SwitchBSPNames::listAll(); });
					return std::optional<std::shared_ptr<IGUIElement>>(el);
				}

				// Halo 2 / H3 / ODST / Reach / H4 / H2A MP / HaloCER. See HavokBroadphaseBypass.h.
				case GUIElementEnum::havokBroadphaseBypassGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Stops the game deleting an object whose physics body leaves the Havok broadphase. The 'went outside of the world' timer and kill volumes still apply. Outside it, objects collide with nothing."),
							std::nullopt, "Havok Broadphase Bypass", settings->havokBroadphaseBypassToggle));

				// Halo 3 / ODST / Reach / Halo 4. Replaced "Switch BSP Set" (a raw binary BSP mask). See MCCSwitchZoneSet.h.
				case GUIElementEnum::mccSwitchZoneSetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIMCCSwitchZoneSet>
						(game, ToolTipCollection("Switch to any zone set the current level declares. Pick one and press the button. Loads/unloads geometry and AI zones the normal way, with a short hitch. A revert or level load cancels a pending one."),
							RebindableHotkeyEnum::switchZoneSet, "Switch Zone Set", settings->switchZoneSetEvent));


				case GUIElementEnum::setPlayerHealthSubheadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Directly set the players health and shields"), "Set Player Health", headerChildElements
							{
							createNestedElement(GUIElementEnum::setPlayerHealthGUI),
							createNestedElement(GUIElementEnum::setPlayerHealthValueGUI),
							},
							0.f));

				case GUIElementEnum::setPlayerHealthGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Directly set the players health and shields"), RebindableHotkeyEnum::setPlayerHealth,
							"Set Player Health",
							settings->setPlayerHealthEvent));

				case GUIElementEnum::setPlayerHealthValueGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec2<true, false, 3>>
						(game, ToolTipCollection("Directly set the players health and shields"),
							 "setPlayerHealthValue",
							settings->setPlayerHealthVec2,
							"Health ##setPlayerHealthValue",
							"Shields##setPlayerHealthValue"));

				case GUIElementEnum::skullToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISkullToggle<false>>
						(game, ToolTipCollection("Set skull state. Note that skull state is saved/loaded by checkpoints"),
							"Skull Toggles",
							settings));

				// Halo Campaign Evolved skulls: 56 of them, addressed by bit index, 25 with no SkullEnum
				// equivalent - so a separate widget rather than widening GUISkullToggle. See HCESkullEnum.h.
				case GUIElementEnum::hceSkullToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHCESkullToggle<false>>
						(game, ToolTipCollection("Set skull state. Some skulls do not work. Skull state is reset by checkpoint loads; HCM re-applies your choices."),
							"Skull Toggles##hce",
							settings));

				case GUIElementEnum::playerPositionToClipboardGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Directly set the players health and shields"), std::nullopt,
							"Copy Position to Clipboard",
							settings->playerPositionToClipboardEvent));

				case GUIElementEnum::consoleCommandGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Brings up the command console, allowing you to send engine commands"), RebindableHotkeyEnum::commandConsoleHotkey,
							"Command Console", settings->commandConsoleHotkeyEvent));

				case GUIElementEnum::consoleCommandSettings:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the command console"), "Command Console Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::consoleCommandOutputEvent),
							createNestedElement(GUIElementEnum::consoleCommandPauseGame),
							createNestedElement(GUIElementEnum::consoleCommandBlockInput),
							createNestedElement(GUIElementEnum::consoleCommandFreeCursor),
							createNestedElement(GUIElementEnum::consoleCommandFontSize),
							createNestedElement(GUIElementEnum::consoleCommandExecuteBuffer),
							}));

				case GUIElementEnum::consoleCommandOutputEvent:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared< GUIInvisible>(game, ToolTipCollection(""), "Console Command Output"));


					case GUIElementEnum::consoleCommandPauseGame:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically pause the game when the command console is open"), std::nullopt, "Pause game when Console open", settings->consoleCommandPauseGame));


					case GUIElementEnum::consoleCommandBlockInput:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically block game input when the command console is open"), std::nullopt, "Block input when Console open", settings->consoleCommandBlockInput));

					case GUIElementEnum::consoleCommandFreeCursor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Automatically free cursor when the command console is open"), std::nullopt, "Free cursor when Console open", settings->consoleCommandFreeCursor));


					case GUIElementEnum::consoleCommandFontSize:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 50.f)>>
							(game, ToolTipCollection(""), "Font Size##Console", settings->consoleCommandFontSize));


					case GUIElementEnum::consoleCommandExecuteBuffer:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Execute the command currently in the console (without clearing the console)"), RebindableHotkeyEnum::commandConsoleExecuteBuffer,
								"Execute Command",
								settings->commandConsoleExecuteBufferEvent));
	
				case GUIElementEnum::disableBarriersToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disable Barriers (aka Soft Ceilings)"), RebindableHotkeyEnum::disableBarriers, "Disable Barriers", settings->disableBarriersToggle));

				// Halo Campaign Evolved. Same setting and same hotkey as the MCC row above, but its own element
				// so it can map to the HCEDisableBarriers cheat - see GUIRequiredServices.cpp.
				case GUIElementEnum::hceDisableBarriersGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Disable Barriers (aka Soft Ceilings). Clears the engine's own soft-ceiling enable bits, so nothing pushes you back at the level boundaries. Re-applied automatically after checkpoint reverts."), RebindableHotkeyEnum::disableBarriers, "Disable Barriers##hce", settings->disableBarriersToggle));

				case GUIElementEnum::soundClassGainAdjusterToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Adjust in-game volume levels by sound type"), std::nullopt, "Sound Gain Adjuster", settings->soundClassGainAdjusterToggle));

				case GUIElementEnum::soundClassGainAdjusterSettings:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for in-game volume levels"), "Sound Gain Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::soundClassGainDialog),
							createNestedElement(GUIElementEnum::soundClassGainAmbience),
							createNestedElement(GUIElementEnum::soundClassGainWeapons),
							createNestedElement(GUIElementEnum::soundClassGainVehicles),
							createNestedElement(GUIElementEnum::soundClassGainMusic),
							createNestedElement(GUIElementEnum::soundClassGainOther),
							}));

					case GUIElementEnum::soundClassGainDialog:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection(""), "Dialog##SoundGainAdjuster", settings->soundClassGainDialog));

					case GUIElementEnum::soundClassGainAmbience:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection(""), "Ambience##SoundGainAdjuster", settings->soundClassGainAmbience));

					case GUIElementEnum::soundClassGainWeapons:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection(""), "Weapons##SoundGainAdjuster", settings->soundClassGainWeapons));

					case GUIElementEnum::soundClassGainVehicles:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection(""), "Vehicles##SoundGainAdjuster", settings->soundClassGainVehicles));

					case GUIElementEnum::soundClassGainMusic:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection(""), "Music##SoundGainAdjuster", settings->soundClassGainMusic));

					case GUIElementEnum::soundClassGainOther:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
							(game, ToolTipCollection("Footsteps, devices/machines, certain scripted sounds, and miscellaneous"), "Other##SoundGainAdjuster", settings->soundClassGainOther));


			case GUIElementEnum::overlaysHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Overlays that various information over the top of the game view"), "Overlays", headerChildElements
						{ 
							createNestedElement(GUIElementEnum::renderDistance3DGUI),
							createNestedElement(GUIElementEnum::display2DInfoToggleGUI),
							createNestedElement(GUIElementEnum::display2DInfoSettingsInfoSubheading),
							createNestedElement(GUIElementEnum::display2DInfoSettingsVisualSubheading),
							createNestedElement(GUIElementEnum::hceDisplayInfoToggleGUI),
							createNestedElement(GUIElementEnum::h5DisplayInfoToggleGUI),
							createNestedElement(GUIElementEnum::h5DisplayInfoShowZoneSetPrep),
							createNestedElement(GUIElementEnum::hceDisplayInfoSettingsInfoSubheading),
							createNestedElement(GUIElementEnum::hceDisplayInfoSettingsVisualSubheading),
							createNestedElement(GUIElementEnum::waypoint3DGUIToggle),
							createNestedElement(GUIElementEnum::waypoint3DGUIList),
							createNestedElement(GUIElementEnum::waypoint3DGUISettings),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIToggle),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIList),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUISettings),
							createNestedElement(GUIElementEnum::triggerOverlayToggle),
							createNestedElement(GUIElementEnum::triggerOverlaySettings),
							createNestedElement(GUIElementEnum::hceTriggerOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceTriggerOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::hceAISquadOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceAISquadOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::hceBspOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceBspOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceVisibleGeometryOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::hceSoftCeilingOverlayToggleGUI),
							createNestedElement(GUIElementEnum::hceSoftCeilingOverlaySettingsSubheading),
							createNestedElement(GUIElementEnum::softCeilingOverlayToggle),
							createNestedElement(GUIElementEnum::softCeilingOverlaySettings),
							createNestedElement(GUIElementEnum::collisionViewerInfoPanelGUI),
							createNestedElement(GUIElementEnum::collisionViewerKillVolumesGUI),
							createNestedElement(GUIElementEnum::oobBarSettingsGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceGUI),
							createNestedElement(GUIElementEnum::collisionViewerSuperbounceSettingsGUI),
							createNestedElement(GUIElementEnum::collisionViewerToggleGUI),
							createNestedElement(GUIElementEnum::launchTrajectoryToggleGUI),
							createNestedElement(GUIElementEnum::launchTrajectorySettingsGUI),
							createNestedElement(GUIElementEnum::placementPointsOverlayToggle),
							createNestedElement(GUIElementEnum::placementPointsOverlaySettings),
							createNestedElement(GUIElementEnum::shieldInputPrinterToggle),
							createNestedElement(GUIElementEnum::abilityMeterOverlayToggle),
							createNestedElement(GUIElementEnum::abilityMeterOverlaySettings),
							createNestedElement(GUIElementEnum::havokSleepCountdownToggleGUI),
							createNestedElement(GUIElementEnum::havokSleepCountdownSimpleGUI),
							createNestedElement(GUIElementEnum::havokSleepAutoJumpGUI),
							createNestedElement(GUIElementEnum::sensDriftOverlayToggle),
							createNestedElement(GUIElementEnum::sensDriftOverlaySettings),
							createNestedElement(GUIElementEnum::competitionModeToggle),
							createNestedElement(GUIElementEnum::competitionModeSettings),
						}));


				case GUIElementEnum::renderDistance3DGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1f, 1000.f, ImGuiSliderFlags_None)>>
						(game, ToolTipCollection("How far objects render when using 3D overlays eg Trigger Overlay"), "3D Render Distance", settings->renderDistance3D));


				case GUIElementEnum::display2DInfoToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Displays various player and game information as text on your screen"), RebindableHotkeyEnum::display2DInfo, "Display 2D Game Info", settings->display2DInfoToggle));

				case GUIElementEnum::display2DInfoSettingsInfoSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Control what information should be displayed"), "Info Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::display2DInfoShowGameTick),
							createNestedElement(GUIElementEnum::display2DInfoShowAggro),
							createNestedElement(GUIElementEnum::display2DInfoShowRNG),
							createNestedElement(GUIElementEnum::display2DInfoShowLevelLoadRNG),
							createNestedElement(GUIElementEnum::display2DInfoShowBSP),
							createNestedElement(GUIElementEnum::display2DInfoShowBSPSet),
							createNestedElement(GUIElementEnum::display2DInfoShowZoneSet),
							createNestedElement(GUIElementEnum::display2DInfoShowNextObjectDatum),
							createNestedElement(GUIElementEnum::display2DInfoTrackPlayer),
							createNestedElement(GUIElementEnum::display2DInfoTrackCustomObject),
							}));

					case GUIElementEnum::display2DInfoShowGameTick:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection(""), std::nullopt, "Show Game Tickcounter", settings->display2DInfoShowGameTick));




					case GUIElementEnum::display2DInfoShowAggro:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection(""), std::nullopt, "Show Aggro info", settings->display2DInfoShowAggro));

					case GUIElementEnum::display2DInfoShowRNG:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection(""), std::nullopt, "Show RNG seed", settings->display2DInfoShowRNG));

					case GUIElementEnum::display2DInfoShowLevelLoadRNG:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("The RNG seed as it was at the start of the level (game tick 0), "
								"rather than the live value. Re-captured every time the level restarts; a checkpoint "
								"revert deliberately does not change it."),
								std::nullopt, "Show level load RNG seed", settings->display2DInfoShowLevelLoadRNG));


					case GUIElementEnum::display2DInfoShowBSP:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection(""), std::nullopt, 
								("Show BSP Index"),
								settings->display2DInfoShowBSP));

					case GUIElementEnum::display2DInfoShowBSPSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection(""), std::nullopt,
								("Show BSP Set"),
								settings->display2DInfoShowBSPSet));

					// Declared in GUI GROUP 4 (GuiElementEnum.h) - groups 1-3 are at the MSVC C1009 ceiling.
					case GUIElementEnum::display2DInfoShowZoneSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Shows the current zone set: its index, plus its name in Halo 3, ODST, Reach and Halo 4. '(loading)' is appended while the switch is still in progress. -1 / none when no level is loaded."), std::nullopt,
								("Show Current Zone Set"),
								settings->display2DInfoShowZoneSet));


					case GUIElementEnum::display2DInfoShowNextObjectDatum:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Shows the datum (salt + index) of the next object to spawn. Useful for Halo 2's Arbitary-unit-possession trick."), std::nullopt, "Show Next Object Datum", settings->display2DInfoShowNextObjectDatum));

					case GUIElementEnum::display2DInfoTrackPlayer:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
							(game, ToolTipCollection(""), std::nullopt, "Track Player Entity", settings->display2DInfoTrackPlayer, headerChildElements
								{
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerViewAngle),
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerViewAngleID),
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerPosition),
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerVelocity),
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerHealth),
								}
								));

						case GUIElementEnum::display2DInfoShowPlayerViewAngle:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Player View Angle", settings->display2DInfoShowPlayerViewAngle));

						case GUIElementEnum::display2DInfoShowPlayerViewAngleID:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show View Angle Subpixel ID", settings->display2DInfoShowPlayerViewAngleID));



						case GUIElementEnum::display2DInfoShowPlayerPosition:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Position##player", settings->display2DInfoShowPlayerPosition));

						case GUIElementEnum::display2DInfoShowPlayerVelocity:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Velocity##player", settings->display2DInfoShowPlayerVelocity, headerChildElements
									{
										createNestedElement(GUIElementEnum::display2DInfoShowPlayerVelocityAbs),
										createNestedElement(GUIElementEnum::display2DInfoShowPlayerVelocityXY),
										createNestedElement(GUIElementEnum::display2DInfoShowPlayerVelocityXYZ),
									}));

							case GUIElementEnum::display2DInfoShowPlayerVelocityAbs:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As absolute velocity##player", settings->display2DInfoShowPlayerVelocityAbs));

							case GUIElementEnum::display2DInfoShowPlayerVelocityXY:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As XY magnitude##player", settings->display2DInfoShowPlayerVelocityXY));

							case GUIElementEnum::display2DInfoShowPlayerVelocityXYZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As XYZ magnitude##player", settings->display2DInfoShowPlayerVelocityXYZ));

						case GUIElementEnum::display2DInfoShowPlayerHealth:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Health/Shields##player", settings->display2DInfoShowPlayerHealth, headerChildElements
									{
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerRechargeCooldown),
									createNestedElement(GUIElementEnum::display2DInfoShowPlayerVehicleHealth),
									}
									));

						case GUIElementEnum::display2DInfoShowPlayerRechargeCooldown:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Recharge Cooldown##entity", settings->display2DInfoShowPlayerRechargeCooldown));

							case GUIElementEnum::display2DInfoShowPlayerVehicleHealth:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "Show Vehicle's Health/Shields##player", settings->display2DInfoShowPlayerVehicleHealth));




					case GUIElementEnum::display2DInfoTrackCustomObject:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
							(game, ToolTipCollection(""), std::nullopt, "Track Custom Entity", settings->display2DInfoTrackCustomObject, headerChildElements
							{
							createNestedElement(GUIElementEnum::display2DInfoCustomObjectDatum),
							createNestedElement(GUIElementEnum::display2DInfoShowEntityObjectType),
							createNestedElement(GUIElementEnum::display2DInfoShowEntityTagName),
							createNestedElement(GUIElementEnum::display2DInfoShowEntityPosition),
							createNestedElement(GUIElementEnum::display2DInfoShowEntityVelocity),
							createNestedElement(GUIElementEnum::display2DInfoShowEntityHealth),
							}
							));

						case GUIElementEnum::display2DInfoCustomObjectDatum:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
								(game, ToolTipCollection("Datum of custom object to track"), "Custom entity datum: ", settings->display2DInfoCustomObjectDatum));


						case GUIElementEnum::display2DInfoShowEntityObjectType:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Object Type##entity", settings->display2DInfoShowEntityObjectType));

						case GUIElementEnum::display2DInfoShowEntityTagName:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Tag Name##entity", settings->display2DInfoShowEntityTagName));

						case GUIElementEnum::display2DInfoShowEntityPosition:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Position##entity", settings->display2DInfoShowEntityPosition));

						case GUIElementEnum::display2DInfoShowEntityVelocity:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Velocity##entity", settings->display2DInfoShowEntityVelocity, headerChildElements
									{
										createNestedElement(GUIElementEnum::display2DInfoShowEntityVelocityAbs),
										createNestedElement(GUIElementEnum::display2DInfoShowEntityVelocityXY),
										createNestedElement(GUIElementEnum::display2DInfoShowEntityVelocityXYZ),
									}));

							case GUIElementEnum::display2DInfoShowEntityVelocityAbs:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As absolute velocity##entity", settings->display2DInfoShowEntityVelocityAbs));

							case GUIElementEnum::display2DInfoShowEntityVelocityXY:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As XY magnitude##entity", settings->display2DInfoShowEntityVelocityXY));

							case GUIElementEnum::display2DInfoShowEntityVelocityXYZ:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "As XYZ magnitude##entity", settings->display2DInfoShowEntityVelocityXYZ));

						case GUIElementEnum::display2DInfoShowEntityHealth:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
								(game, ToolTipCollection(""), std::nullopt, "Show Health/Shields##entity", settings->display2DInfoShowEntityHealth, headerChildElements
							{
								createNestedElement(GUIElementEnum::display2DInfoShowEntityRechargeCooldown),
								createNestedElement(GUIElementEnum::display2DInfoShowEntityVehicleHealth),
							}
							));

							case GUIElementEnum::display2DInfoShowEntityRechargeCooldown:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "Show Recharge Cooldown##entity", settings->display2DInfoShowEntityRechargeCooldown));

							case GUIElementEnum::display2DInfoShowEntityVehicleHealth:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
									(game, ToolTipCollection(""), std::nullopt, "Show Vehicle's Health/Shields##entity", settings->display2DInfoShowEntityVehicleHealth));




				case GUIElementEnum::display2DInfoSettingsVisualSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Cosmetic/visual settings for how the info should be displayed"), "Visual Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::display2DInfoAnchorCorner),
							createNestedElement(GUIElementEnum::display2DInfoScreenOffset),
							createNestedElement(GUIElementEnum::display2DInfoFontSize),
							createNestedElement(GUIElementEnum::display2DInfoFontColour),
							createNestedElement(GUIElementEnum::display2DInfoFloatPrecision),
							createNestedElement(GUIElementEnum::display2DInfoOutline),
							}));

					case GUIElementEnum::display2DInfoAnchorCorner:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
							(game, ToolTipCollection(""), "Corner to Anchor to", settings->display2DInfoAnchorCorner));

					case GUIElementEnum::display2DInfoScreenOffset:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({0.f, 0.f}, {1600.f, 1000.f}, ImGuiSliderFlags_None) >>
							(game, ToolTipCollection(""), "Pixel Offset from Corner", settings->display2DInfoScreenOffset, "Horizontal", "Vertical"));


					case GUIElementEnum::display2DInfoFontSize:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection(""),  "Info Font Size", settings->display2DInfoFontSize));

					case GUIElementEnum::display2DInfoFontColour:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
							(game, ToolTipCollection(""),  "Colour#2dinfo", settings->display2DInfoFontColour));

					case GUIElementEnum::display2DInfoFloatPrecision:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<SliderParam<int>(0, 10)>>
							(game, ToolTipCollection(""), "Info Decimal Precision", settings->display2DInfoFloatPrecision));

					case GUIElementEnum::display2DInfoOutline:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Adds a black outline to text (has a small negative performance impact)"), std::nullopt, "Info Font Outline", settings->display2DInfoOutline));

				// ---- Halo Campaign Evolved 2D info overlay ----------------------------------------------------
				// Same toggle, same hotkey and the SAME six visual settings as the MCC overlay - only the row
				// list is HaloCER-specific, because HCE can supply nothing but these seven values.
				case GUIElementEnum::hceDisplayInfoToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Displays player and game information as text on your screen"), RebindableHotkeyEnum::display2DInfo, "Display 2D Game Info##hce", settings->display2DInfoToggle));

				// Halo 5: Forge. Same toggle, same hotkey, same six visual settings - only the row list differs,
				// because Halo 5 can honestly supply position/aim/datum/object/sim-kind/object-write-gate and
				// nothing more today. No settings subheading: every row is unconditional, so there is nothing
				// to configure yet.
				// No hotkey (std::nullopt) deliberately: HCE_HOTKEYS is index-addressed, so adding one there
				// shifts every alias index after it. This drives the SAME setting object as HaloCER's
				// transition rows - the titles cannot coexist in one process.
				case GUIElementEnum::h5DisplayInfoShowZoneSetPrep:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Shows two extra rows: Preparing Zone Set (True while a switch is in flight) and Prepared Zone Set (the incoming name, or NULL). The Zone Set row only changes once the new one is resident."),
							std::nullopt, "Show Zone Set Transition##h5", settings->hceDisplayInfoShowZoneSetPrep));

				case GUIElementEnum::h5DisplayInfoToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Displays player and game information as text on your screen"), RebindableHotkeyEnum::display2DInfo, "Display 2D Game Info##h5", settings->display2DInfoToggle));

				case GUIElementEnum::hceDisplayInfoSettingsInfoSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Control what information should be displayed"), "Info Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceDisplayInfoShowCoordinates),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowVelocity),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowVelocityXY),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowVelocityXYZ),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowLevel),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowBSP),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowZoneSet),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowZoneSetPrep),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowCameraDiag),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowTick),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowPlayerDatum),
							createNestedElement(GUIElementEnum::hceDisplayInfoShowTEB),
							}));

					case GUIElementEnum::hceDisplayInfoShowCoordinates:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the player's xyz world position"), RebindableHotkeyEnum::hceDisplayInfoShowCoordinatesHotkey, "Show Coordinates", settings->hceDisplayInfoShowCoordinates));

					case GUIElementEnum::hceDisplayInfoShowVelocity:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the player's velocity split into X, Y and Z components"), RebindableHotkeyEnum::hceDisplayInfoShowVelocityHotkey, "Show Velocity", settings->hceDisplayInfoShowVelocity));

					case GUIElementEnum::hceDisplayInfoShowVelocityXY:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show horizontal speed as a single number: sqrt(x^2 + y^2). Ignores vertical motion, so falling and jumping do not inflate it."), RebindableHotkeyEnum::hceDisplayInfoShowVelocityXYHotkey, "Show Absolute XY Velocity", settings->hceDisplayInfoShowVelocityXY));

					case GUIElementEnum::hceDisplayInfoShowVelocityXYZ:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show total speed as a single number: sqrt(x^2 + y^2 + z^2). Includes vertical motion."), RebindableHotkeyEnum::hceDisplayInfoShowVelocityXYZHotkey, "Show Absolute XYZ Velocity", settings->hceDisplayInfoShowVelocityXYZ));

					case GUIElementEnum::hceDisplayInfoShowLevel:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the currently loaded level (scenario) name"), RebindableHotkeyEnum::hceDisplayInfoShowLevelHotkey, "Show Current Level", settings->hceDisplayInfoShowLevel));

					case GUIElementEnum::hceDisplayInfoShowBSP:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the current BSP index. The value is actually the zone set index (what current_zone_set returns). 'Show Current Zone Set' below shows the same value as a name."), RebindableHotkeyEnum::hceDisplayInfoShowBSPHotkey, "Show Current BSP", settings->hceDisplayInfoShowBSP));

				case GUIElementEnum::hceDisplayInfoShowZoneSetPrep:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Shows two extra rows: Preparing Zone Set (True while a switch is in flight) and Prepared Zone Set (the incoming name, or NULL). Current Zone Set only changes once the new one is resident."),
							RebindableHotkeyEnum::hceDisplayInfoShowZoneSetPrepHotkey, "Show Zone Set Transition", settings->hceDisplayInfoShowZoneSetPrep));

				case GUIElementEnum::hceDisplayInfoShowZoneSet:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Show the name of the zone set that is fully loaded and committed. Waits until a switch's BSPs are resident. To watch a switch in progress, enable 'Show Zone Set Transition' below."), RebindableHotkeyEnum::hceDisplayInfoShowZoneSetHotkey, "Show Current Zone Set", settings->hceDisplayInfoShowZoneSet));

				case GUIElementEnum::hceDisplayInfoShowCameraDiag:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("DIAGNOSTIC. Shows the render-camera election state: hook fire count, whether a camera was adopted, last update age and the rejecting check. Under Wine/Proton also the open file-descriptor count."), RebindableHotkeyEnum::hceDisplayInfoShowCameraDiagHotkey, "Show Diagnostics", settings->hceDisplayInfoShowCameraDiag));

					case GUIElementEnum::hceDisplayInfoShowTick:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the engine's tick counter"), RebindableHotkeyEnum::hceDisplayInfoShowTickHotkey, "Show Tick Counter", settings->hceDisplayInfoShowTick));

					case GUIElementEnum::hceDisplayInfoShowPlayerDatum:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Show the player's datum (unique ID). Reads (dead) when the player has no object."), RebindableHotkeyEnum::hceDisplayInfoShowPlayerDatumHotkey, "Show Player Datum", settings->hceDisplayInfoShowPlayerDatum));

					case GUIElementEnum::hceDisplayInfoShowTEB:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Diagnostic: the game thread's TEB address. If this shows an address, HCM has found the game thread and every other Halo Campaign Evolved feature can work."), RebindableHotkeyEnum::hceDisplayInfoShowTEBHotkey, "Show TEB Base (debug)", settings->hceDisplayInfoShowTEB));

				case GUIElementEnum::hceDisplayInfoSettingsVisualSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Cosmetic/visual settings for how the info should be displayed"), "Visual Settings##hce", headerChildElements
							{
							createNestedElement(GUIElementEnum::hceDisplayInfoAnchorCorner),
							createNestedElement(GUIElementEnum::hceDisplayInfoScreenOffset),
							createNestedElement(GUIElementEnum::hceDisplayInfoFontSize),
							createNestedElement(GUIElementEnum::hceDisplayInfoFontColour),
							createNestedElement(GUIElementEnum::hceDisplayInfoFloatPrecision),
							createNestedElement(GUIElementEnum::hceDisplayInfoOutline),
							}));

					case GUIElementEnum::hceDisplayInfoAnchorCorner:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
							(game, ToolTipCollection(""), "Corner to Anchor to##hce", settings->display2DInfoAnchorCorner));

					case GUIElementEnum::hceDisplayInfoScreenOffset:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
							(game, ToolTipCollection(""), "Pixel Offset from Corner##hce", settings->display2DInfoScreenOffset, "Horizontal", "Vertical"));

					case GUIElementEnum::hceDisplayInfoFontSize:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection(""), "Info Font Size##hce", settings->display2DInfoFontSize));

					case GUIElementEnum::hceDisplayInfoFontColour:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
							(game, ToolTipCollection(""), "Colour##hce2dinfo", settings->display2DInfoFontColour));

					case GUIElementEnum::hceDisplayInfoFloatPrecision:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<SliderParam<int>(0, 10)>>
							(game, ToolTipCollection(""), "Info Decimal Precision##hce", settings->display2DInfoFloatPrecision));

					case GUIElementEnum::hceDisplayInfoOutline:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Adds a black outline to text (has a small negative performance impact)"), RebindableHotkeyEnum::hceDisplayInfoOutlineHotkey, "Info Font Outline##hce", settings->display2DInfoOutline));

				case GUIElementEnum::waypoint3DGUIToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Toggles 3D waypoints"), RebindableHotkeyEnum::toggleWaypoint3D, "Custom Waypoints", settings->waypoint3DToggle));

				case GUIElementEnum::waypoint3DGUIList:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIWaypointList<false>>
						(game, ToolTipCollection("List of 3D Waypoints"), settings->waypoint3DList, settings->editWaypointEvent, settings->deleteWaypointEvent, settings->addWaypointEvent, exp));


				case GUIElementEnum::waypoint3DGUISettings:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Custom Waypoint Settings"), "Custom Waypoint Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::waypoint3DGUIClampToggle),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalSpriteColor),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalSpriteScale),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalLabelColor),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalLabelScale),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalDistanceColor),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalDistanceScale),
							createNestedElement(GUIElementEnum::waypoint3DGUIGlobalDistancePrecision),
							}));


					case GUIElementEnum::waypoint3DGUIClampToggle:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Clamps waypoints to the edge of the screen when you turn away from them"), std::nullopt, "Clamp to Screen", settings->waypoint3DClampToggle));


					case GUIElementEnum::waypoint3DGUIGlobalSpriteColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to waypoint sprite"), "Global Sprite Color", settings->waypoint3DGlobalSpriteColor));

					case GUIElementEnum::waypoint3DGUIGlobalSpriteScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 10.f)>>
							(game, ToolTipCollection("Scaling factor of waypoint sprite"), "Global Sprite Scale", settings->waypoint3DGlobalSpriteScale));

					case GUIElementEnum::waypoint3DGUIGlobalLabelColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to waypoint label"), "Global Label Color", settings->waypoint3DGlobalLabelColor));

					case GUIElementEnum::waypoint3DGUIGlobalLabelScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection("Scaling factor of waypoint label"), "Global Label Scale", settings->waypoint3DGlobalLabelScale));

					case GUIElementEnum::waypoint3DGUIGlobalDistanceColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to waypoint distance text"), "Global Distance Color", settings->waypoint3DGlobalDistanceColor));

					case GUIElementEnum::waypoint3DGUIGlobalDistanceScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection("Scaling factor of waypoint distance text"), "Global Distance Scale", settings->waypoint3DGlobalDistanceScale));

					case GUIElementEnum::waypoint3DGUIGlobalDistancePrecision:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<SliderParam<int>(0, 10)>>
							(game, ToolTipCollection("Decimal precision of waypoint distance text"), "Global Distance Precision", settings->waypoint3DGlobalDistancePrecision));

				case GUIElementEnum::viewAngleLine3DGUIToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Toggles 3D View Angle Line overlay"), RebindableHotkeyEnum::toggleViewAngleLine3D, "View Angle Overlay", settings->viewAngleLine3DToggle));

				case GUIElementEnum::viewAngleLine3DGUIList:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIViewAngleLineList<false>>
						(game, ToolTipCollection("List of 3D ViewAngleLines"), settings->viewAngleLine3DList, settings->editViewAngleLineEvent, settings->deleteViewAngleLineEvent, settings->addViewAngleLineEvent, exp));


				case GUIElementEnum::viewAngleLine3DGUISettings:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Custom ViewAngleLine Settings"), "View Angle Overlay Settings", headerChildElements
							{
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalSpriteColor),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalLabelColor),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalLabelScale),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalDistanceColor),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalDistanceScale),
							createNestedElement(GUIElementEnum::viewAngleLine3DGUIGlobalDistancePrecision),
							}));



					case GUIElementEnum::viewAngleLine3DGUIGlobalSpriteColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to viewAngleLine sprite"), "Global Sprite Color", settings->viewAngleLine3DGlobalSpriteColor));

					case GUIElementEnum::viewAngleLine3DGUIGlobalLabelColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to viewAngleLine label"), "Global Label Color", settings->viewAngleLine3DGlobalLabelColor));

					case GUIElementEnum::viewAngleLine3DGUIGlobalLabelScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection("Scaling factor of viewAngleLine label"), "Global Label Scale", settings->viewAngleLine3DGlobalLabelScale));

					case GUIElementEnum::viewAngleLine3DGUIGlobalDistanceColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<false>>
							(game, ToolTipCollection("Color to apply to viewAngleLine distance text"), "Global Distance Color", settings->viewAngleLine3DGlobalDistanceColor));

					case GUIElementEnum::viewAngleLine3DGUIGlobalDistanceScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
							(game, ToolTipCollection("Scaling factor of viewAngleLine distance text"), "Global Distance Scale", settings->viewAngleLine3DGlobalDistanceScale));

					case GUIElementEnum::viewAngleLine3DGUIGlobalDistancePrecision:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<SliderParam<int>(0, 10)>>
							(game, ToolTipCollection("Decimal precision of viewAngleLine distance text"), "Global Distance Precision", settings->viewAngleLine3DGlobalDistancePrecision));


						

				case GUIElementEnum::triggerOverlayToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Overlays trigger volumes onto the screen"), RebindableHotkeyEnum::triggerOverlayToggleHotkey, "Trigger Overlay", settings->triggerOverlayToggle));

				case GUIElementEnum::triggerOverlaySettings:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the trigger overlay"), "Trigger Overlay Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::triggerOverlayFilterToggle),
								createNestedElement(GUIElementEnum::triggerOverlayRenderStyle),
								createNestedElement(GUIElementEnum::triggerOverlayInteriorStyle),
								createNestedElement(GUIElementEnum::triggerOverlayLabelStyle),
								createNestedElement(GUIElementEnum::triggerOverlayLabelScale),
								createNestedElement(GUIElementEnum::triggerOverlayNormalColor),
								createNestedElement(GUIElementEnum::triggerOverlayBSPColor),
								createNestedElement(GUIElementEnum::triggerOverlaySectorColor),
								createNestedElement(GUIElementEnum::triggerOverlayAlpha),
								createNestedElement(GUIElementEnum::triggerOverlayWireframeAlpha),
								createNestedElement(GUIElementEnum::triggerOverlayCheckHitToggle),
								createNestedElement(GUIElementEnum::triggerOverlayCheckMissToggle),
								createNestedElement(GUIElementEnum::triggerOverlayMessageOnEnter),
								createNestedElement(GUIElementEnum::triggerOverlayMessageOnExit),
								createNestedElement(GUIElementEnum::triggerOverlayMessageOnCheckHit),
								createNestedElement(GUIElementEnum::triggerOverlayMessageOnCheckMiss),
								createNestedElement(GUIElementEnum::triggerOverlayPositionToggle),
							}));

				case GUIElementEnum::triggerOverlayFilterToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::AlwaysShowChildren, false>>
						(game, ToolTipCollection("Filter which triggers to show by name"), std::nullopt, "Filter Triggers by Name", settings->triggerOverlayFilterToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::triggerOverlayFilterExactMatch),
							createNestedElement(GUIElementEnum::triggerOverlayFilterStringDialog),
							createNestedElement(GUIElementEnum::triggerOverlayFilterCountLabel),
							createNestedElement(GUIElementEnum::triggerOverlayFilterStringCopy),
							createNestedElement(GUIElementEnum::triggerOverlayFilterStringPaste),
							}));

				case GUIElementEnum::triggerOverlayFilterExactMatch:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Trigger names must exactly match. If disabled, trigger names that contain an entry in the list will also be shown."), std::nullopt, "Exact Match##TriggerOverlayFilter", settings->triggerOverlayFilterExactMatch));

				case GUIElementEnum::triggerOverlayFilterStringDialog:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Which triggers should show up. Names should be semicolon seperated."), std::nullopt, "Edit Filter Name Whitelist", settings->triggerOverlayFilterStringDialogEvent));


				case GUIElementEnum::triggerOverlayFilterCountLabel:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUILabel<int>>
						(game, ToolTipCollection("How many trigger names are in the filter list."), "Filter Name Count: {}", settings->triggerOverlayFilterCountLabel));


				case GUIElementEnum::triggerOverlayFilterStringCopy:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Copy the filter list to the clipboard"), std::nullopt, "Copy Filter List", settings->triggerOverlayFilterStringCopyEvent));

				case GUIElementEnum::triggerOverlayFilterStringPaste:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Paste the filter list from the clipboard"), std::nullopt, "Paste Filter List", settings->triggerOverlayFilterStringPasteEvent));

				case GUIElementEnum::triggerOverlayRenderStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerRenderStyle, 150.f>>
						(game, ToolTipCollection("What style to render trigger volumes as"), "Render trigger volumes as: ", settings->triggerOverlayRenderStyle));

				case GUIElementEnum::triggerOverlayInteriorStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerInteriorStyle, 100.f>>
						(game, ToolTipCollection("What style to render trigger volumes as when the camera is inside them"), "Render Solid Interior as: ", settings->triggerOverlayInteriorStyle));

				case GUIElementEnum::triggerOverlayLabelStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerLabelStyle, 100.f>>
						(game, ToolTipCollection("Render Labels (Trigger names) at center of volume, corner, or not at all."), "Render Labels at: ", settings->triggerOverlayLabelStyle));

				case GUIElementEnum::triggerOverlayLabelScale:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
						(game, ToolTipCollection("How large the font size of the label should be, in pts"), "Label Font Size", settings->triggerOverlayLabelScale));


				case GUIElementEnum::triggerOverlayNormalColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Color to render triggers when not checked or BSP"), "Trigger Normal Colour", settings->triggerOverlayNormalColor));

				case GUIElementEnum::triggerOverlayBSPColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Color to render BSP (loadzone) triggers"), "Trigger BSP Colour", settings->triggerOverlayBSPColor));

				case GUIElementEnum::triggerOverlaySectorColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Color to render Sector-type triggers"), "Trigger Sector Colour", settings->triggerOverlaySectorColor));

				case GUIElementEnum::triggerOverlayAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1, 1.f)>>
						(game, ToolTipCollection("Multiplies how opaque triggers are, from 0 (fully transparent) to 1 (fully opaque)"), "Trigger Opacity Multiplier", settings->triggerOverlayAlpha));

				case GUIElementEnum::triggerOverlayWireframeAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1, 1.f)>>
						(game, ToolTipCollection("Multiplies how opaque trigger wireframes are, from 0 (fully transparent) to 1 (fully opaque)"), "Wireframe Opacity Multiplier", settings->triggerOverlayWireframeAlpha));


				case GUIElementEnum::triggerOverlayCheckHitToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::AlwaysShowChildren, false>>
						(game, ToolTipCollection("Whether to flash triggers to a certain colour when the script checks them for the players presence"), std::nullopt, "Flash Triggers on Successful Check", settings->triggerOverlayCheckHitToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::triggerOverlayCheckHitFalloff),
							createNestedElement(GUIElementEnum::triggerOverlayCheckHitColor),
							}));

					case GUIElementEnum::triggerOverlayCheckHitFalloff:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
							(game, ToolTipCollection("How many seconds does the check-flash last"), "Flash for x ticks", settings->triggerOverlayCheckHitFalloff));

					case GUIElementEnum::triggerOverlayCheckHitColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
							(game, ToolTipCollection("Color to flash trigger when it successfully checks for the player"), "Colour on Successful Check", settings->triggerOverlayCheckHitColor));

				case GUIElementEnum::triggerOverlayCheckMissToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::AlwaysShowChildren, false>>
						(game, ToolTipCollection("Whether to flash triggers to a certain colour when the script checks them for the players presence"), std::nullopt, "Flash Triggers on Failed Check", settings->triggerOverlayCheckMissToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::triggerOverlayCheckMissFalloff),
							createNestedElement(GUIElementEnum::triggerOverlayCheckMissColor),
							}));

					case GUIElementEnum::triggerOverlayCheckMissFalloff:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
							(game, ToolTipCollection("How many seconds does the check-flash last"), "Flash for x ticks", settings->triggerOverlayCheckMissFalloff));

					case GUIElementEnum::triggerOverlayCheckMissColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
							(game, ToolTipCollection("Color to flash trigger when it successfully checks for the player"), "Colour on Failed Check", settings->triggerOverlayCheckMissColor));



				case GUIElementEnum::triggerOverlayMessageOnEnter:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Prints a message to the screen when the player enters a trigger"), std::nullopt, "Message on Entering Trigger", settings->triggerOverlayMessageOnEnter));

				case GUIElementEnum::triggerOverlayMessageOnExit:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Prints a message to the screen when the player exits a trigger"), std::nullopt, "Message on Exiting Trigger", settings->triggerOverlayMessageOnExit));

				case GUIElementEnum::triggerOverlayMessageOnCheckHit:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Prints a message to the screen when a trigger successfully checks for the player"), std::nullopt, "Message on Successful Trigger Check", settings->triggerOverlayMessageOnCheckHit));


				case GUIElementEnum::triggerOverlayMessageOnCheckMiss:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Prints a message to the screen when a trigger failfully checks for the player"), std::nullopt, "Message on Failed Trigger Check", settings->triggerOverlayMessageOnCheckMiss));

				case GUIElementEnum::triggerOverlayPositionToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::AlwaysShowChildren, false>>
						(game, ToolTipCollection("Draws a sphere at the vertex of the player model that activates triggers"), std::nullopt, "Show Player Trigger Vertex", settings->triggerOverlayPositionToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::triggerOverlayPositionColor),
							createNestedElement(GUIElementEnum::triggerOverlayPositionScale),
							createNestedElement(GUIElementEnum::triggerOverlayPositionWireframe),
							}));

					case GUIElementEnum::triggerOverlayPositionColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
							(game, ToolTipCollection("Color of the sphere drawn at the players trigger vertex"), "Trigger Vertex Color", settings->triggerOverlayPositionColor));

					case GUIElementEnum::triggerOverlayPositionScale:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.01f, 4.f)>>
							(game, ToolTipCollection("Size of the sphere drawn at the players trigger vertex"), "Trigger Vertex Scale", settings->triggerOverlayPositionScale));

					case GUIElementEnum::triggerOverlayPositionWireframe:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Draw the player-trigger-vertex sphere as wireframe instead of solid"), std::nullopt, "Trigger Vertex Wireframe", settings->triggerOverlayPositionWireframe));


			// ---- Launch Trajectory (Halo 2) ----
			case GUIElementEnum::launchTrajectoryToggleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Draws your predicted path if launched from your current velocity, through wall and floor contacts to rest. For momentum-storage launches: stored, pre-launch and actual paths in different colours."), std::nullopt, "Launch Trajectory", settings->launchTrajectoryToggle));

			case GUIElementEnum::launchTrajectorySettingsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Settings for the launch trajectory overlay"), "Launch Trajectory Settings", headerChildElements
						{
						createNestedElement(GUIElementEnum::launchTrajectoryMinSpeedGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryDurationGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryOnlyWhenStoredGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryKeepAfterLaunchGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryShowActualPathGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryStopAtCollisionGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryIncludeBarriersGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryIncludeObjectsGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryUseBodyExtentsGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryIgnoreFirstTicksGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryGroundDecelGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryAirFrictionGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryWallBackstepGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryGroundSlideMaxSpeedGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReleaseModelGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReleaseFrictionGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReleaseKickGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryH2DecompGroundGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReleaseKickRangeGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReachGameCollisionGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchRunGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchRunsGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchFirstCrouchGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchCrouchTicksGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchCyclesGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchCycleLengthGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchAttackTicksGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchCrouchDelayGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchStepGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchReleaseDelayGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchCrouchKeyGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchMeleeKeyGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchReleaseKeyGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchSpeedStartGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryBenchSpeedStepGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryHudStoredGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryHudScaleGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryHudHeightGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReachCrouchReleaseGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryReachReleaseRatioGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryFallDeathGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryFallDeathSpeedGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryNextHitToggleGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryNextHitRotateWithViewGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryNextHitColorGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryRestitutionGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryMaxSlopeGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryMaxSpeedGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryDampingGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryShowTickMarksGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryShowLabelsGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryLogToFileGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryLogSpeedUpsGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryLabelScaleGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryLineColorGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryStoredColorGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryHeldColorGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryActualColorGUI),
						createNestedElement(GUIElementEnum::launchTrajectoryImpactColorGUI)
						}));

			case GUIElementEnum::launchTrajectoryMinSpeedGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Only draw the live prediction when your speed is at least this many world units per second. Running is about 2.3 wu/s. This is also the minimum speed for a stall to count as stored momentum."), "Minimum Speed (wu/s)##launchTrajectory", settings->launchTrajectoryMinSpeed));

			case GUIElementEnum::launchTrajectoryDurationGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.5f, 120.f)>>
					(game, ToolTipCollection("Upper limit on how far ahead to simulate (and how much actual path to keep). The line normally stops earlier, where the body comes to rest; if the end label says still moving, raise this."), "Max Prediction Length (s)##launchTrajectory", settings->launchTrajectoryDuration));

			case GUIElementEnum::launchTrajectoryOnlyWhenStoredGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Only draw the live line while momentum is being stored (velocity high, but the body is not actually moving). Hides it during normal play."), std::nullopt, "Only While Momentum Is Stored##launchTrajectory", settings->launchTrajectoryOnlyWhenStored));

			case GUIElementEnum::launchTrajectoryKeepAfterLaunchGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("When a stored launch is released, keep the prediction made on the last stored tick on screen for the prediction length + 3 seconds, so it can be compared with the path you actually flew."), std::nullopt, "Keep Pre-Launch Prediction##launchTrajectory", settings->launchTrajectoryKeepAfterLaunch));

			case GUIElementEnum::launchTrajectoryShowActualPathGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Draw the path your centre of mass actually took, one point per tick. Restarts at each launch and on reverts / teleports."), std::nullopt, "Show Actual Path##launchTrajectory", settings->launchTrajectoryShowActualPath));

			case GUIElementEnum::launchTrajectoryStopAtCollisionGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Collide with the level (world BSP and instanced geometry): slide along what you hit and rest on the ground. Off: the pure ballistic arc straight through everything for the full Max Prediction Length."), std::nullopt, "Collide With World##launchTrajectory", settings->launchTrajectoryStopAtCollision));

			case GUIElementEnum::launchTrajectoryIncludeBarriersGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Treat invisible collision surfaces (player barriers) as solid."), std::nullopt, "Include Invisible Barriers##launchTrajectory", settings->launchTrajectoryIncludeBarriers));

			case GUIElementEnum::launchTrajectoryIncludeObjectsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Also collide with scenery, crates and machines near the path. Re-read every frame because objects move."), std::nullopt, "Include Objects##launchTrajectory", settings->launchTrajectoryIncludeObjects));

			case GUIElementEnum::launchTrajectoryUseBodyExtentsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Collide with three rays (capsule top, bottom and centre) so floors and ceilings are hit when feet or head reach them. Off: centre only, so side impacts can be up to one capsule radius late."), std::nullopt, "Use Body Height##launchTrajectory", settings->launchTrajectoryUseBodyExtents));

			case GUIElementEnum::launchTrajectoryIgnoreFirstTicksGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Skip collision for this many ticks at the start of the path. Storing momentum often leaves you touching or embedded in geometry, which would otherwise end the line immediately."), "Ignore Collision For First N Ticks##launchTrajectory", settings->launchTrajectoryIgnoreFirstTicks));

			case GUIElementEnum::launchTrajectoryGroundDecelGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("How fast you slow down while sliding on walkable ground with no stick input, in wu/s per second. Measured from launch logs: 9.60, a constant slow-down."), "Ground Friction (wu/s^2)##launchTrajectory", settings->launchTrajectoryGroundDecel));

			case GUIElementEnum::launchTrajectoryRestitutionGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
					(game, ToolTipCollection("Bounciness on contact: 0 = the velocity going into a surface is removed and you slide along it (bipeds), 1 = a perfect bounce."), "Bounce (Restitution)##launchTrajectory", settings->launchTrajectoryRestitution));

			case GUIElementEnum::launchTrajectoryMaxSlopeGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 89.f)>>
					(game, ToolTipCollection("Surfaces flatter than this (degrees from horizontal) can be stood on; steeper ones are slid down. Halo 2: 50, the maximum slope angle in both masterchief.biped and dervish.biped."), "Walkable Slope (deg)##launchTrajectory", settings->launchTrajectoryMaxSlope));

			case GUIElementEnum::launchTrajectoryLogToFileGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Writes one CSV per launch to the LaunchLogs folder in the HCM folder: game vs predicted values each tick, plus fitted constants. Records from momentum release until rest, revert, death or teleport."), std::nullopt, "Log Launches To File##launchTrajectory", settings->launchTrajectoryLogToFile));

			case GUIElementEnum::launchTrajectoryLogSpeedUpsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Also log launches with no storage phase: any tick where speed jumps from below Minimum Speed to above it (explosions, vehicle hits, grenade jumps)."), std::nullopt, "Also Log Launches Without Storage##launchTrajectory", settings->launchTrajectoryLogSpeedUps));

			case GUIElementEnum::launchTrajectoryAirFrictionGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("How fast your horizontal speed drops while airborne with no stick input, in wu/s per second. Measured from launch logs: exactly 1.050, a constant slow-down. Vertical speed only changes by gravity."), "Air Friction (wu/s^2)##launchTrajectory", settings->launchTrajectoryAirFriction));

			case GUIElementEnum::launchTrajectoryWallBackstepGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Impacts faster than 4.085 wu/s into a surface cut your velocity to exactly 4.085 wu/s, same direction. Gentler contacts only remove the part going into the surface. Off: all contacts are gentle."), std::nullopt, "Hard Impacts Cut Speed To 4.085##launchTrajectory", settings->launchTrajectoryWallBackstep));

			case GUIElementEnum::launchTrajectoryGroundSlideMaxSpeedGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Above this speed a body sliding on the ground gets only Air Friction, not Ground Friction. Measured between 6.1 and 11.9 wu/s; default 9."), "Ground Friction Only Below (wu/s)##launchTrajectory", settings->launchTrajectoryGroundSlideMaxSpeed));

			case GUIElementEnum::launchTrajectoryReleaseModelGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Models stored momentum released from the ground: the release tick adds one tick of ground friction back and skips gravity, then 4 ticks of ground-style slowing (Release Friction)."), std::nullopt, "Model Release Ticks##launchTrajectory", settings->launchTrajectoryReleaseModel));

			case GUIElementEnum::launchTrajectoryReleaseFrictionGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Slow-down along your whole velocity for the 4 ticks after a release, in wu/s per second. Best fit across your logs: 9.3."), "Release Friction (wu/s^2)##launchTrajectory", settings->launchTrajectoryReleaseFriction));

			case GUIElementEnum::launchTrajectoryH2DecompGroundGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Experimental: three Halo 2 ground-movement details from the Xbox decompilation. Replayed against 766 Halo 2 logs they changed almost nothing (within 0.3 wu), so off by default; turn on to compare."), std::nullopt, "Halo 2: Decompiled Ground Details (experimental)##launchTrajectory", settings->launchTrajectoryH2DecompGround));

			case GUIElementEnum::launchTrajectoryReleaseKickGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Upward speed added on the release tick of a stored launch. Forward-tap releases got +1.4 to +6.0 wu/s (median ~3.5). Set 0 for releases that do not kick."), "Release Kick (wu/s)##launchTrajectory", settings->launchTrajectoryReleaseKick));

			case GUIElementEnum::launchTrajectoryFallDeathGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("End the line, with a red marker, where you would die mid-air to the fall timer. Skipped automatically while HCM Invulnerability is on - then the whole line is drawn."), std::nullopt, "Show Fall Death Point##launchTrajectory", settings->launchTrajectoryFallDeath));

			case GUIElementEnum::launchTrajectoryFallDeathSpeedGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Downward speed at which the mid-air fall death happens. For Halo 2, the globals tag's maximum falling distance of 14 gives 9.4785 wu/s. Matched all 20 logged deaths."), "Fall Death Speed (wu/s)##launchTrajectory", settings->launchTrajectoryFallDeathSpeed));

			case GUIElementEnum::launchTrajectoryNextHitToggleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Draws a second line: where you would go if you took one more hit like the last one (sword lunge, melee, rocket, grenade; anything adding over 4 wu/s). Appears once a hit has been seen this session."), std::nullopt, "Show One-More-Hit Line##launchTrajectory", settings->launchTrajectoryNextHitToggle));

			case GUIElementEnum::launchTrajectoryNextHitRotateWithViewGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Turns the hit's push with your view: a melee or sword hit pushes where you aim. Off: the next hit pushes in the same world direction as the last (better for an explosion from a fixed spot)."), std::nullopt, "One-More-Hit Follows Your Aim##launchTrajectory", settings->launchTrajectoryNextHitRotateWithView));

			case GUIElementEnum::launchTrajectoryNextHitColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Colour of the one-more-hit line"), "One-More-Hit Colour##launchTrajectory", settings->launchTrajectoryNextHitColor));

			case GUIElementEnum::launchTrajectoryReleaseKickRangeGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("While storing, also draws two dimmed lines for the release kick this much lower and higher than Release Kick, showing where you could end up. 0 = off."), "Release Kick Spread (+/- wu/s)##launchTrajectory", settings->launchTrajectoryReleaseKickRange));

			case GUIElementEnum::launchTrajectoryReachCrouchReleaseGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Reach crouch storage: the line shows the release, with stored speed turned horizontal down the slope you are on. On flat floor it falls back to your aim. Stored speeds over ~31 wu/s risk death."), std::nullopt, "Reach: Show Crouch Storage Release##launchTrajectory", settings->launchTrajectoryReachCrouchRelease));

			case GUIElementEnum::launchTrajectoryReachReleaseRatioGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Horizontal speed after a crouch-storage release (once on the ground), as a fraction of the stored speed. Your 17 logged releases ranged 0.72 - 0.86, mean ~0.82."), "Reach: Release Speed Ratio##launchTrajectory", settings->launchTrajectoryReachReleaseRatio));

			case GUIElementEnum::launchTrajectoryHudStoredGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("While momentum is stored, shows the stored speed and where the prediction ends (or the fall/Reach release death risk) in big text under the crosshair, readable in first person."), std::nullopt, "Show Stored Speed On HUD##launchTrajectory", settings->launchTrajectoryHudStored));

			case GUIElementEnum::launchTrajectoryHudScaleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Size of the HUD read-out (1 = about 3% of the screen height)."), "HUD Text Size##launchTrajectory", settings->launchTrajectoryHudScale));

			case GUIElementEnum::launchTrajectoryHudHeightGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Where the HUD read-out sits, as a fraction of the screen height (0 = top, 0.5 = crosshair, 1 = bottom)."), "HUD Position (screen height)##launchTrajectory", settings->launchTrajectoryHudHeight));

			case GUIElementEnum::launchTrajectoryReachGameCollisionGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Reach: tests the predicted path with the game's own collision test instead of the overlay's level copy, for exact terrain and objects (with Include Objects). Lines draw a tick behind."), std::nullopt, "Reach: Use The Game's Own Collision##launchTrajectory", settings->launchTrajectoryReachGameCollision));

			case GUIElementEnum::launchTrajectoryBenchRunGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Launch Test Bench: repeats your storage launch from your checkpoint. First Force Checkpoint and turn on Disable Natural Checkpoints. Sends real keys, so keep MCC focused. Needs Launch Trajectory on."), RebindableHotkeyEnum::launchTrajectoryBenchHotkey, "Run Launch Test Bench##launchTrajectory", settings->launchTrajectoryBenchRun));

			case GUIElementEnum::launchTrajectoryBenchRunsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("How many launches the bench does before it stops (and switches itself off)."), "Test Bench: Runs##launchTrajectory", settings->launchTrajectoryBenchRuns));

			case GUIElementEnum::launchTrajectoryBenchFirstCrouchGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Ticks after the revert before the first storage cycle starts, on the first run. Every run shifts the whole input sequence by the Step, so each run stores at a slightly different moment."), "Test Bench: Start (ticks after revert)##launchTrajectory", settings->launchTrajectoryBenchFirstCrouch));

			case GUIElementEnum::launchTrajectoryBenchCrouchTicksGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("How long the crouch key is spammed (pressed every other tick). 0 = no crouch."), "Test Bench: Crouch Spam Length (ticks)##launchTrajectory", settings->launchTrajectoryBenchCrouchTicks));

			case GUIElementEnum::launchTrajectoryBenchCyclesGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("How many times the storage inputs repeat before the release (Halo 2 sword storage: attack, crouch on the impact frame, crouch more - repeated). Reach: 1."), "Test Bench: Storage Cycles##launchTrajectory", settings->launchTrajectoryBenchCycles));

			case GUIElementEnum::launchTrajectoryBenchCycleLengthGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Ticks from the start of one cycle to the start of the next."), "Test Bench: Cycle Length (ticks)##launchTrajectory", settings->launchTrajectoryBenchCycleLength));

			case GUIElementEnum::launchTrajectoryBenchAttackTicksGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Each cycle starts by spamming the Attack Key (pressed every other tick) for this many ticks. 0 = no attack (Reach)."), "Test Bench: Attack Spam (ticks per cycle, 0 = none)##launchTrajectory", settings->launchTrajectoryBenchAttackTicks));

			case GUIElementEnum::launchTrajectoryBenchCrouchDelayGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Ticks from the start of the cycle (the first attack press) to the first crouch press - set it to the frame the sword lands. The crouch is then spammed for Crouch Spam Length ticks."), "Test Bench: Crouch Delay (ticks after cycle start)##launchTrajectory", settings->launchTrajectoryBenchCrouchDelay));

			case GUIElementEnum::launchTrajectoryBenchStepGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("How far the crouch / melee timing moves each run. 1 = one tick later every run; 0 = repeat the identical launch (determinism check)."), "Test Bench: Step (ticks per run)##launchTrajectory", settings->launchTrajectoryBenchStep));

			case GUIElementEnum::launchTrajectoryBenchReleaseDelayGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Once the storage is detected and the spam is over, wait this many ticks, then tap the release key (held 2 ticks)."), "Test Bench: Release After Storing (ticks)##launchTrajectory", settings->launchTrajectoryBenchReleaseDelay));

			case GUIElementEnum::launchTrajectoryBenchCrouchKeyGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Windows virtual-key code of YOUR crouch binding. Left Ctrl = 162, C = 67, Left Shift = 160, Space = 32, letters A-Z = 65-90."), "Test Bench: Crouch Key (VK code)##launchTrajectory", settings->launchTrajectoryBenchCrouchKey));

			case GUIElementEnum::launchTrajectoryBenchMeleeKeyGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Key spammed at the start of each cycle (Halo 2: sword attack). Windows virtual-key code: 1 = left mouse, 2 = right, 4 = middle, 5/6 = side buttons; Q = 81, V = 86, F = 70, E = 69 (A-Z = 65-90)."), "Test Bench: Attack Key (VK code, 1 = left mouse)##launchTrajectory", settings->launchTrajectoryBenchMeleeKey));

			case GUIElementEnum::launchTrajectoryBenchReleaseKeyGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
					(game, ToolTipCollection("Windows virtual-key code of the key tapped to release: W = 87 (forward), S = 83, A = 65, D = 68. 1 = left mouse (Halo 2: release by hitting the object again, not crouching), 2 = right mouse."), "Test Bench: Release Key (VK code)##launchTrajectory", settings->launchTrajectoryBenchReleaseKey));

			case GUIElementEnum::launchTrajectoryBenchSpeedStartGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("Sweeps the stored speed: sets it to this on the first run, then adds the Speed Step each run (e.g. 10 with step 0.5 covers 10 to 34.5 wu/s in 50 runs). 0 = leave the stored speed alone."), "Test Bench: Stored Speed Start (wu/s, 0 = off)##launchTrajectory", settings->launchTrajectoryBenchSpeedStart));

			case GUIElementEnum::launchTrajectoryBenchSpeedStepGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("How much the stored speed goes up each run (wu/s) when Stored Speed Start is set."), "Test Bench: Stored Speed Step (wu/s per run)##launchTrajectory", settings->launchTrajectoryBenchSpeedStep));

			case GUIElementEnum::launchTrajectoryMaxSpeedGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
					(game, ToolTipCollection("The engine caps every Havok body's linear velocity to 100 wu/s each tick before gravity. If your launches beat the line, set this to 0 to disable the cap in the prediction."), "Max Speed Cap (wu/s, 0 = off)##launchTrajectory", settings->launchTrajectoryMaxSpeed));

			case GUIElementEnum::launchTrajectoryDampingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 5.f)>>
					(game, ToolTipCollection("Extra linear damping per second (v *= 1 - damping * tick length). The engine model needs 0; this exists only to fit the line if the Actual path consistently falls short."), "Linear Damping (1/s)##launchTrajectory", settings->launchTrajectoryDamping));

			case GUIElementEnum::launchTrajectoryShowTickMarksGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Mark every full second of flight along the line."), std::nullopt, "Second Markers##launchTrajectory", settings->launchTrajectoryShowTickMarks));

			case GUIElementEnum::launchTrajectoryShowLabelsGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Show your speed and the live gravity at your position, and flight time / horizontal distance / apex / height change at the end of the line."), std::nullopt, "Labels##launchTrajectory", settings->launchTrajectoryShowLabels));

			case GUIElementEnum::launchTrajectoryLabelScaleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(8.f, 40.f)>>
					(game, ToolTipCollection("Font size of the labels."), "Label Size##launchTrajectory", settings->launchTrajectoryLabelScale));

			case GUIElementEnum::launchTrajectoryLineColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Live prediction colour"), "Line Colour##launchTrajectory", settings->launchTrajectoryLineColor));

			case GUIElementEnum::launchTrajectoryStoredColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Live prediction colour while momentum is being stored"), "Stored Colour##launchTrajectory", settings->launchTrajectoryStoredColor));

			case GUIElementEnum::launchTrajectoryHeldColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Colour of the prediction kept from just before the launch"), "Pre-Launch Colour##launchTrajectory", settings->launchTrajectoryHeldColor));

			case GUIElementEnum::launchTrajectoryActualColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Colour of the path you actually flew"), "Actual Path Colour##launchTrajectory", settings->launchTrajectoryActualColor));

			case GUIElementEnum::launchTrajectoryImpactColorGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<false>>
					(game, ToolTipCollection("Colour of the impact marker and its label"), "Impact Colour##launchTrajectory", settings->launchTrajectoryImpactColor));

			case GUIElementEnum::softCeilingOverlayToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Renders Soft Ceilings (barriers)"), RebindableHotkeyEnum::softCeilingOverlayToggleHotkey, "Barrier Overlay", settings->softCeilingOverlayToggle));

			case GUIElementEnum::softCeilingOverlaySettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Settings for the barrier overlay"), "Barrier Overlay Settings", headerChildElements
						{
						createNestedElement(GUIElementEnum::softCeilingOverlayRenderTypes),
						createNestedElement(GUIElementEnum::softCeilingOverlayRenderDirection),
						createNestedElement(GUIElementEnum::softCeilingOverlayColorAccel),
						createNestedElement(GUIElementEnum::softCeilingOverlayColorSlippy),
						createNestedElement(GUIElementEnum::softCeilingOverlayColorKill),
						createNestedElement(GUIElementEnum::softCeilingOverlaySolidTransparency),
						createNestedElement(GUIElementEnum::softCeilingOverlayWireframeTransparency)
						}));

			case GUIElementEnum::placementPointsOverlayToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Halo 3: visualises the respawn / vehicle-exit placement search (the candidate points the game tries when finding a safe spot around the player)."), std::nullopt, "Placement Points Overlay", settings->placementPointsOverlayToggle));

			case GUIElementEnum::placementPointsOverlaySettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Settings for the placement points overlay"), "Placement Points Settings", headerChildElements
						{
						createNestedElement(GUIElementEnum::placementPointsOverlayShowValidity),
						createNestedElement(GUIElementEnum::placementPointsOverlayExtended),
						createNestedElement(GUIElementEnum::placementPointsOverlayRadius)
						}));

			case GUIElementEnum::placementPointsOverlayShowValidity:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Colour each candidate point by whether the game would accept it: GREEN = a valid spot (on/in collision world), RED = invalid. Turn off to colour by ring/direction instead."), std::nullopt, "Show Validity (green/red)", settings->placementPointsOverlayShowValidity));

			case GUIElementEnum::placementPointsOverlayExtended:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Also draw the 9 downward candidate points - these are only used for vehicle exits (you can't respawn downward in Halo 3)."), std::nullopt, "Extended Placement Points (Vehicle Only)", settings->placementPointsOverlayExtended));

			case GUIElementEnum::placementPointsOverlayRadius:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloatStepper>
					(game, ToolTipCollection("Base search radius. Verified live = 1.0 (the respawn search uses radii 1.0/2.0/4.0). Rings draw at this radius x1, x2, x4 to match the engine's expanding retries."), "Base Radius", settings->placementPointsOverlayRadius));

			case GUIElementEnum::softCeilingOverlayRenderTypes:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::SoftCeilingRenderTypes, 150.f>>
					(game, ToolTipCollection("Whether to render barriers that apply to bipeds, vehicles, or either."), "Render Barriers Affecting:", settings->softCeilingOverlayRenderTypes));

			case GUIElementEnum::softCeilingOverlayRenderDirection:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::SoftCeilingRenderDirection, 100.f>>
					(game, ToolTipCollection("Whether to render the front of barriers, the back, or both."), "Render Barriers Direction:", settings->softCeilingOverlayRenderDirection));


			case GUIElementEnum::softCeilingOverlayColorAccel:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
					(game, ToolTipCollection("Color of barriers of the \"Acceleration\" type"), "Accelleration Color", settings->softCeilingOverlayColorAccel));

			case GUIElementEnum::softCeilingOverlayColorSlippy:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
					(game, ToolTipCollection("Color of barriers of the \"Slip Surface\" type"), "Slippery Color", settings->softCeilingOverlayColorSlippy));

			case GUIElementEnum::softCeilingOverlayColorKill:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
					(game, ToolTipCollection("Color of barriers of the \"Soft Kill\" type"), "Kill Color", settings->softCeilingOverlayColorKill));

			case GUIElementEnum::softCeilingOverlaySolidTransparency:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 1.f)>>
					(game, ToolTipCollection("Opacity of the filled barrier triangles"), "Solid Opacity", settings->softCeilingOverlaySolidTransparency));

			case GUIElementEnum::softCeilingOverlayWireframeTransparency:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 1.f)>>
					(game, ToolTipCollection("Opacity of the outline of the barrier triangles"), "Outline Opacity", settings->softCeilingOverlayWireframeTransparency));



			case GUIElementEnum::shieldInputPrinterToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Locks onto nearest deployable shield; when shield disabled, prints which tick you pressed movement inputs"), RebindableHotkeyEnum::shieldInputPrinterToggleHotkey, "Shield Input Printer", settings->shieldInputPrinterToggle));

			case GUIElementEnum::competitionModeToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Broadcast scoreboard for tournament observers. Two independently placed panels, left and right, with team totals and a per-player breakdown. Gametype aware. Halo 2 Classic only."), std::nullopt, "Competition Mode", settings->competitionModeToggle));

			case GUIElementEnum::competitionModeSettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Appearance and layout of the Competition Mode scoreboard"), "Competition Mode Settings", headerChildElements
						{
							createNestedElement(GUIElementEnum::competitionModeScale),
							createNestedElement(GUIElementEnum::competitionModeBackgroundColour),
							createNestedElement(GUIElementEnum::competitionModeTextColour),
							createNestedElement(GUIElementEnum::competitionModeShowKD),
							createNestedElement(GUIElementEnum::competitionModeShowKDRatio),
							createNestedElement(GUIElementEnum::competitionModeShowColumnHeaders),
							createNestedElement(GUIElementEnum::competitionModeOutlineText),
							createNestedElement(GUIElementEnum::competitionModeForceBothPanels),
							createNestedElement(GUIElementEnum::competitionModeLeftSettings),
							createNestedElement(GUIElementEnum::competitionModeRightSettings),
						}));

				case GUIElementEnum::competitionModeScale:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.25f, 4.f)>>
						(game, ToolTipCollection("Scales both panels at once. Set this for the capture resolution, then fine tune each side below."), "Overall Scale", settings->competitionModeScale));

				case GUIElementEnum::competitionModeBackgroundColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Panel background. Drop the alpha for a lighter look, raise it for readability over busy maps."), "Panel Background##competitionMode", settings->competitionModeBackgroundColour));

				case GUIElementEnum::competitionModeTextColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection(""), "Text Colour##competitionMode", settings->competitionModeTextColour));

				case GUIElementEnum::competitionModeShowKD:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Show kills and deaths beside each player's score"), std::nullopt, "Show K/D", settings->competitionModeShowKD));

				case GUIElementEnum::competitionModeShowKDRatio:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Show the kill/death ratio as its own column. Independent of Show K/D - either can be on without the other."), std::nullopt, "Show K/D Ratio", settings->competitionModeShowKDRatio));

				case GUIElementEnum::competitionModeShowColumnHeaders:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Adds a header row labelling the columns, so viewers can tell K/D apart from game points"), std::nullopt, "Show Column Headers", settings->competitionModeShowColumnHeaders));

				case GUIElementEnum::competitionModeForceBothPanels:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws the second panel even when the match has no second team. A layout aid for positioning both sides before a match fills up - both panels will list the same players."), std::nullopt, "Force Both Panels", settings->competitionModeForceBothPanels));

				case GUIElementEnum::competitionModeOutlineText:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Outlines the text so it stays readable over bright map geometry. Worth leaving on for broadcast."), std::nullopt, "Outline Text", settings->competitionModeOutlineText));

			case GUIElementEnum::competitionModeLeftSettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Placement, size and colour for the left panel"), "Left Panel Settings", headerChildElements
						{
							createNestedElement(GUIElementEnum::competitionModeLeftOffset),
							createNestedElement(GUIElementEnum::competitionModeLeftPanelWidth),
							createNestedElement(GUIElementEnum::competitionModeLeftFontSize),
							createNestedElement(GUIElementEnum::competitionModeLeftColour),
						}));

				case GUIElementEnum::competitionModeLeftOffset:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection("Distance in from this panel's screen edge"), "Edge Margin##competitionModeLeft", settings->competitionModeLeftOffset, "Horizontal", "Vertical"));

				case GUIElementEnum::competitionModeLeftPanelWidth:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(120.f, 1600.f)>>
						(game, ToolTipCollection("Panel width in pixels, before Overall Scale. Widen it if long gamertags collide with the score column."), "Panel Width##competitionModeLeft", settings->competitionModeLeftPanelWidth));

				case GUIElementEnum::competitionModeLeftFontSize:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 96.f)>>
						(game, ToolTipCollection("Player row font size in pts, before Overall Scale"), "Font Size##competitionModeLeft", settings->competitionModeLeftFontSize));

				case GUIElementEnum::competitionModeLeftColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Accent and team colour for this panel"), "Accent Colour##competitionModeLeft", settings->competitionModeLeftColour));

			case GUIElementEnum::competitionModeRightSettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Placement, size and colour for the right panel"), "Right Panel Settings", headerChildElements
						{
							createNestedElement(GUIElementEnum::competitionModeRightOffset),
							createNestedElement(GUIElementEnum::competitionModeRightPanelWidth),
							createNestedElement(GUIElementEnum::competitionModeRightFontSize),
							createNestedElement(GUIElementEnum::competitionModeRightColour),
						}));

				case GUIElementEnum::competitionModeRightOffset:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection("Distance in from this panel's screen edge"), "Edge Margin##competitionModeRight", settings->competitionModeRightOffset, "Horizontal", "Vertical"));

				case GUIElementEnum::competitionModeRightPanelWidth:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(120.f, 1600.f)>>
						(game, ToolTipCollection("Panel width in pixels, before Overall Scale. Widen it if long gamertags collide with the score column."), "Panel Width##competitionModeRight", settings->competitionModeRightPanelWidth));

				case GUIElementEnum::competitionModeRightFontSize:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 96.f)>>
						(game, ToolTipCollection("Player row font size in pts, before Overall Scale"), "Font Size##competitionModeRight", settings->competitionModeRightFontSize));

				case GUIElementEnum::competitionModeRightColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Accent and team colour for this panel"), "Accent Colour##competitionModeRight", settings->competitionModeRightColour));

			case GUIElementEnum::abilityMeterOverlayToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Renders an overlay with info about the player ability ability"), RebindableHotkeyEnum::abilityMeterToggleHotkey, "Ability Meter Toggle", settings->abilityMeterOverlayToggle));

			case GUIElementEnum::havokSleepCountdownToggleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Shows your biped's Havok island sleep countdown: ticks until it sleeps, the JUMP line, quiet-round count (x/5), and per-body what keeps it awake. Judges each jump PERFECT / EARLY / LATE. MCC 1.3528."), std::nullopt, "Havok Sleep Countdown", settings->havokSleepCountdownToggle));

			case GUIElementEnum::havokSleepCountdownSimpleGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Havok Sleep Countdown shows only the bar: how many ticks until your island sleeps, whether the next check counts, and the JUMP line. Drag it where you want it while HCM's cursor is free."), std::nullopt, "Simple bar only##havokSleep", settings->havokSleepCountdownSimple));

			case GUIElementEnum::havokSleepAutoJumpGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
					(game, ToolTipCollection("Presses jump for you when the countdown reaches the JUMP line (2 ticks left at 30 tick/s, 1 at 60), once per cycle, when your island is still. Each jump is judged PERFECT / EARLY / LATE."), std::nullopt, "Auto-jump on the line##havokSleep", settings->havokSleepAutoJump));

			case GUIElementEnum::abilityMeterOverlaySettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Settings for the Ability Meter Overlay"), "Ability Meter Settings", headerChildElements
						{
							createNestedElement(GUIElementEnum::abilityMeterHeroAssistToggle),
							createNestedElement(GUIElementEnum::abilityMeterAbilityAnchorCorner),
							createNestedElement(GUIElementEnum::abilityMeterAbilityScreenOffset),
							createNestedElement(GUIElementEnum::abilityMeterAbilitySize),
							createNestedElement(GUIElementEnum::abilityMeterAbilityBackgroundColor),
							createNestedElement(GUIElementEnum::abilityMeterAbilityForegroundColor),
							createNestedElement(GUIElementEnum::abilityMeterAbilityHighlightColor),
							createNestedElement(GUIElementEnum::abilityMeterCooldownToggle),
						}));


				case GUIElementEnum::abilityMeterHeroAssistToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Instead of showing your current ability data, shows your sprint data instead"), std::nullopt, "Show Sprint instead", settings->abilityMeterHeroAssistToggle));


				case GUIElementEnum::abilityMeterAbilityAnchorCorner:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
						(game, ToolTipCollection(""), "Corner to Anchor to##abilityAbility", settings->abilityMeterAbilityAnchorCorner));

				case GUIElementEnum::abilityMeterAbilityScreenOffset:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Pixel Offset from Corner##abilityAbility", settings->abilityMeterAbilityScreenOffset, "Horizontal", "Vertical"));

				case GUIElementEnum::abilityMeterAbilitySize:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 500.f, 500.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Bar Size##abilityAbility", settings->abilityMeterAbilitySize, "Horizontal", "Vertical"));


				case GUIElementEnum::abilityMeterAbilityBackgroundColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection(""), "Background Colour##abilityAbility", settings->abilityMeterAbilityBackgroundColor));

				case GUIElementEnum::abilityMeterAbilityForegroundColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection(""), "Foreground Colour##abilityAbility", settings->abilityMeterAbilityForegroundColor));

				case GUIElementEnum::abilityMeterAbilityHighlightColor:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection(""), "Highlight Colour##abilityAbility", settings->abilityMeterAbilityHighlightColor));


					case GUIElementEnum::abilityMeterCooldownToggle:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
							(game, ToolTipCollection("Show the cooldown bar for your ability ability"), std::nullopt, "Ability Cooldown Bar", settings->abilityMeterCooldownToggle, headerChildElements
								{
								createNestedElement(GUIElementEnum::abilityMeterCooldownAnchorCorner),
								createNestedElement(GUIElementEnum::abilityMeterCooldownScreenOffset),
								createNestedElement(GUIElementEnum::abilityMeterCooldownSize),
								createNestedElement(GUIElementEnum::abilityMeterCooldownBackgroundColor),
								createNestedElement(GUIElementEnum::abilityMeterCooldownForegroundColor),
								createNestedElement(GUIElementEnum::abilityMeterCooldownHighlightColor),
								}));

						case GUIElementEnum::abilityMeterCooldownAnchorCorner:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
								(game, ToolTipCollection(""), "Corner to Anchor to##abilityCooldown", settings->abilityMeterCooldownAnchorCorner));

						case GUIElementEnum::abilityMeterCooldownScreenOffset:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
								(game, ToolTipCollection(""), "Pixel Offset from Corner##abilityCooldown", settings->abilityMeterCooldownScreenOffset, "Horizontal", "Vertical"));

						case GUIElementEnum::abilityMeterCooldownSize:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 500.f, 500.f }, ImGuiSliderFlags_None) >>
								(game, ToolTipCollection(""), "Bar Size##abilityCooldown", settings->abilityMeterCooldownSize, "Horizontal", "Vertical"));

						case GUIElementEnum::abilityMeterCooldownBackgroundColor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
								(game, ToolTipCollection(""), "Background Colour##abilityCooldown", settings->abilityMeterCooldownBackgroundColor));

						case GUIElementEnum::abilityMeterCooldownForegroundColor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
								(game, ToolTipCollection(""), "Foreground Colour##abilityCooldown", settings->abilityMeterCooldownForegroundColor));

						case GUIElementEnum::abilityMeterCooldownHighlightColor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
								(game, ToolTipCollection(""), "Highlight Colour##abilityCooldown", settings->abilityMeterCooldownHighlightColor));

			case GUIElementEnum::sensDriftOverlayToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Renders an overlay with information about sensitivity drift (misalignment of sensitivity manipulations)"), RebindableHotkeyEnum::sensDriftOverlayToggleHotkey, "Sensitivity Drift Overlay", settings->sensDriftOverlayToggle));

			case GUIElementEnum::sensDriftOverlaySettings:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
					(game, ToolTipCollection("Settings for the Sensitivity Drift Overlay"), "Sensitivity Drift Settings", headerChildElements
						{
							createNestedElement(GUIElementEnum::sensOverDotFrameToggle),
							createNestedElement(GUIElementEnum::sensMessageOnOverDotToggle),
							createNestedElement(GUIElementEnum::sensSoundOnOverDotToggle),
							createNestedElement(GUIElementEnum::sensSubpixelDriftToggle),
							createNestedElement(GUIElementEnum::sensMessageOnSubpixelDriftToggle),
							createNestedElement(GUIElementEnum::sensSoundOnSubpixelDriftToggle),
							createNestedElement(GUIElementEnum::sensSubpixelDriftSciNotationToggle),
							createNestedElement(GUIElementEnum::sensCountTurnsToggle),
							createNestedElement(GUIElementEnum::sensResetCountAction),
							createNestedElement(GUIElementEnum::sensResetCountOnRevertToggle),
							createNestedElement(GUIElementEnum::sensAnchorCorner),
							createNestedElement(GUIElementEnum::sensScreenOffset),
							createNestedElement(GUIElementEnum::sensFontSize),
							createNestedElement(GUIElementEnum::sensFontColour),

							
						}));



				case GUIElementEnum::sensOverDotFrameToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Over-Dots are how many more Dots than 1 that you did in a single frame.\nE.g. two dots in one frame is one overdot. Overdots have a chance of causing subpixel drift."), std::nullopt, "Over-Dot Count Overlay", settings->sensOverDotFrameToggle));

				case GUIElementEnum::sensMessageOnOverDotToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Print a message to the top left when an Over-Dot occurs."), std::nullopt, "Print Message on Over-Dot", settings->sensMessageOnOverDotToggle));

				case GUIElementEnum::sensSoundOnOverDotToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Play a beeping sound when an Over-Dot occurs."), std::nullopt, "Play annoying sound on Over-Dot", settings->sensSoundOnOverDotToggle));




				case GUIElementEnum::sensSubpixelDriftToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("."), std::nullopt, "Subpixel Drift Count Overlay", settings->sensSubpixelDriftToggle));

				case GUIElementEnum::sensMessageOnSubpixelDriftToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Print a message to the top left when subpixel drift occurs."), std::nullopt, "Print Message on Subpixel Drift", settings->sensMessageOnSubpixelDriftToggle));

				case GUIElementEnum::sensSoundOnSubpixelDriftToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Play a beeping sound when subpixel drift occurs."), std::nullopt, "Play annoying sound on Subpixel Drift", settings->sensSoundOnSubpixelDriftToggle));

				case GUIElementEnum::sensSubpixelDriftSciNotationToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Whether to use scientific notation like \"1.59e-6\" vs \"0.00000159\"."), std::nullopt, "Display subpixel drift angle in scientific notation", settings->sensSubpixelDriftSciNotationToggle));


				case GUIElementEnum::sensCountTurnsToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Counts how many clockwise turns you do; specifically crossing the 0 -> 6.28 radians boundary."), std::nullopt, "Count clockwise turns", settings->sensCountTurnsToggle));


				case GUIElementEnum::sensResetCountAction:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Resets the overlays counters of over-dot frames and sub-pixel drift"), RebindableHotkeyEnum::sensResetCountHotkey, "Reset counts", settings->sensResetCountsEvent));


				case GUIElementEnum::sensResetCountOnRevertToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Automatically overlay counters when reverting to a checkpoint/core-save"), std::nullopt, "Auto reset counts on revert", settings->sensResetCountOnRevertToggle));


				case GUIElementEnum::sensAnchorCorner:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::ScreenAnchorEnum, 100.f>>
						(game, ToolTipCollection(""), "Corner to Anchor to", settings->sensAnchorCorner));

				case GUIElementEnum::sensScreenOffset:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < false, false, 0, SliderParam<SimpleMath::Vector2>({ 0.f, 0.f }, { 1600.f, 1000.f }, ImGuiSliderFlags_None) >>
						(game, ToolTipCollection(""), "Pixel Offset from Corner", settings->sensScreenOffset, "Horizontal", "Vertical"));


				case GUIElementEnum::sensFontSize:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
						(game, ToolTipCollection(""), "Info Font Size", settings->sensFontSize));

				case GUIElementEnum::sensFontColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection(""), "Colour#2dinfo", settings->sensFontColour));



			case GUIElementEnum::hceScriptHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Run HaloScript directly. This is the game's own script language - the same one the campaign missions are written in."), "HaloScript", headerChildElements
						{
							createNestedElement(GUIElementEnum::hceSwitchZoneSetGUI),
							createNestedElement(GUIElementEnum::hceConsoleGUI),
						}
					));

			case GUIElementEnum::cameraHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Tools to manipulate the camera"), "Camera", headerChildElements
						{ 
							createNestedElement(GUIElementEnum::hideHUDToggle),
							createNestedElement(GUIElementEnum::editPlayerViewAngleSubheading),
							createNestedElement(GUIElementEnum::editPlayerViewAngleIDSubheading),
							createNestedElement(GUIElementEnum::hceFreecamToggleGUI),
							createNestedElement(GUIElementEnum::hceFreecamTeleportToCamera),
							createNestedElement(GUIElementEnum::hceFreecamNoclipGUI),
							createNestedElement(GUIElementEnum::hceFreecamGimbalBypassGUI),
							createNestedElement(GUIElementEnum::hceFreecamKeepPositionGUI),
							createNestedElement(GUIElementEnum::hceFreecamDriftGUI),
							createNestedElement(GUIElementEnum::hceCameraRollGUI),
							createNestedElement(GUIElementEnum::hceCameraRollLeftBindingGUI),
							createNestedElement(GUIElementEnum::hceCameraRollRightBindingGUI),
							createNestedElement(GUIElementEnum::hceCameraRollSpeedGUI),
							createNestedElement(GUIElementEnum::hceCameraRollResetGUI),
							createNestedElement(GUIElementEnum::hceCameraMoveSpeedGUI),
							createNestedElement(GUIElementEnum::hceCameraMoveSpeedResetGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewToggleGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewValueGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewIncreaseBindingGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewDecreaseBindingGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewSpeedGUI),
							createNestedElement(GUIElementEnum::hceFieldOfViewResetGUI),
							createNestedElement(GUIElementEnum::freeCameraToggleGUI),
							createNestedElement(GUIElementEnum::chaseCameraToggleGUI),
							createNestedElement(GUIElementEnum::chaseCameraNoClipGUI),
							createNestedElement(GUIElementEnum::chaseCameraFovGUI),
							createNestedElement(GUIElementEnum::chaseCameraHeightGUI),
							createNestedElement(GUIElementEnum::freeCameraSettingsSimpleSubheading),
							createNestedElement(GUIElementEnum::freeCameraSettingsAdvancedSubheading),
							createNestedElement(GUIElementEnum::changeOOBBackgroundToggle),
							createNestedElement(GUIElementEnum::disableFogToggle),
						}));

			case GUIElementEnum::hideHUDToggle:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
					(game, ToolTipCollection("Disables the HUD, similiar to the Blind skull but you keep your first person model"), RebindableHotkeyEnum::hideHUDToggle, "Hide HUD", settings->hideHUDToggle));


				case GUIElementEnum::editPlayerViewAngleSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Edit the view angle of the player"), "Edit View Angle", headerChildElements
							{
								createNestedElement(GUIElementEnum::editPlayerViewAngleSet),
								createNestedElement(GUIElementEnum::editPlayerViewAngleVec2),
								createNestedElement(GUIElementEnum::editPlayerViewAngleFillCurrent),
								createNestedElement(GUIElementEnum::editPlayerViewAngleCopy),
								createNestedElement(GUIElementEnum::editPlayerViewAnglePaste),
								createNestedElement(GUIElementEnum::editPlayerViewAngleAdjustHorizontal),
								createNestedElement(GUIElementEnum::editPlayerViewAngleAdjustVertical),
								createNestedElement(GUIElementEnum::editPlayerViewAngleAdjustFactor),
							}));

					case GUIElementEnum::editPlayerViewAngleSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Set player view direction to the specified angles"), RebindableHotkeyEnum::editPlayerViewAngleSet, "Set view angle to: ##editPlayerViewAngleSet", settings->editPlayerViewAngleSet));

					case GUIElementEnum::editPlayerViewAngleVec2:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec2 < true, false, 8, SliderParam<SimpleMath::Vector2>({0, -std::numbers::pi / 2.f }, {std::numbers::pi * 2.f, std::numbers::pi / 2.f})>>
							(game, ToolTipCollection("Rotation of the player"), "Rotation: ##editPlayerViewAngleVec2", settings->editPlayerViewAngleVec2, "yaw", "pitch"));


					case GUIElementEnum::editPlayerViewAngleFillCurrent:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Fill data above with players current look angles"), std::nullopt, "Fill with Current##editPlayerViewAngleFillCurrent", settings->editPlayerViewAngleFillCurrent));


					case GUIElementEnum::editPlayerViewAngleCopy:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Copy view angles to the clipboard"), std::nullopt, "Copy to Clipboard##editPlayerViewAngleCopy", settings->editPlayerViewAngleCopy));


					case GUIElementEnum::editPlayerViewAnglePaste:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Paste in view angles from the clipboard"), std::nullopt, "Paste from Clipboard##editPlayerViewAnglePaste", settings->editPlayerViewAnglePaste));


					case GUIElementEnum::editPlayerViewAngleAdjustHorizontal:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Nudges the horizontal view angle by the factor below"), RebindableHotkeyEnum::editPlayerViewAngleAdjustHorizontal, "Nudge Horizontal Angle##editPlayerViewAngleAdjustHorizontal", settings->editPlayerViewAngleAdjustHorizontal));


					case GUIElementEnum::editPlayerViewAngleAdjustVertical:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Nudges the vertical view angle by the factor below"), RebindableHotkeyEnum::editPlayerViewAngleAdjustVertical, "Nudge Vertical Angle##editPlayerViewAngleAdjustVertical", settings->editPlayerViewAngleAdjustVertical));


					case GUIElementEnum::editPlayerViewAngleAdjustFactor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
							(game, ToolTipCollection("Amount to nudge by when using above functions"), "Nudge Amount##editPlayerViewAngleAdjustFactor", settings->editPlayerViewAngleAdjustFactor));




				case GUIElementEnum::editPlayerViewAngleIDSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Edit the view angle of the player"), "Edit View Angle Subpixel ID", headerChildElements
							{
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDSet),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDInt),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDFillCurrent),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDCopy),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDPaste),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDAdjustNegative),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDAdjustPositive),
								createNestedElement(GUIElementEnum::editPlayerViewAngleIDAdjustFactor),
							}));

					case GUIElementEnum::editPlayerViewAngleIDSet:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Set player horizontal view direction to the specified subpixel ID"), RebindableHotkeyEnum::editPlayerViewAngleIDSet, "Set subpixel to: ##editPlayerViewAngleIDSet", settings->editPlayerViewAngleIDSet));

					case GUIElementEnum::editPlayerViewAngleIDInt:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputSubpixel>
							(game, ToolTipCollection("Rotation of the player"), "Subpixel ID: ##editPlayerViewAngleIDInt", settings->editPlayerViewAngleIDInt));


					case GUIElementEnum::editPlayerViewAngleIDFillCurrent:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Fill data above with players current horizontal sub pixel"), std::nullopt, "Fill with Current##editPlayerViewAngleIDFillCurrent", settings->editPlayerViewAngleIDFillCurrent));


					case GUIElementEnum::editPlayerViewAngleIDCopy:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Copy subpixel to the clipboard"), std::nullopt, "Copy to Clipboard##editPlayerViewAngleIDCopy", settings->editPlayerViewAngleIDCopy));


					case GUIElementEnum::editPlayerViewAngleIDPaste:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
							(game, ToolTipCollection("Paste in subpixel from the clipboard"), std::nullopt, "Paste from Clipboard##editPlayerViewAngleIDPaste", settings->editPlayerViewAngleIDPaste));


					case GUIElementEnum::editPlayerViewAngleIDAdjustNegative:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Decrease the current subpixel ID by the number below"), RebindableHotkeyEnum::editPlayerViewAngleIDAdjustNegative, "Decrease Subpixel##editPlayerViewAngleIDAdjustNegative", settings->editPlayerViewAngleIDAdjustNegative));


					case GUIElementEnum::editPlayerViewAngleIDAdjustPositive:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Increase the current subpixel ID by the number below"), RebindableHotkeyEnum::editPlayerViewAngleIDAdjustPositive, "Increase Subpixel##editPlayerViewAngleIDAdjustPositive", settings->editPlayerViewAngleIDAdjustPositive));


					case GUIElementEnum::editPlayerViewAngleIDAdjustFactor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
							(game, ToolTipCollection("Amount to decrease/increase by when using above functions"), "Nudge Amount##editPlayerViewAngleIDAdjustFactor", settings->editPlayerViewAngleIDAdjustFactor));



				case GUIElementEnum::freeCameraToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection(""), RebindableHotkeyEnum::freeCamera, "Free Camera", settings->freeCameraToggle));

				// ---- Halo Campaign Evolved free camera -------------------------------------------------------
				// Two rows, which is the whole feature: HCE's freecam is a single engine byte and the ENGINE flies
				// the camera. None of the MCC free camera's ~50 settings rows (input bindings, interpolators, FOV
				// curves, object anchoring) apply, because HCM never drives this camera. See HCEFreecam.h.
				case GUIElementEnum::hceFreecamToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Detaches the camera from the player. The game itself controls the camera; HCM only turns it on. Re-applied automatically after checkpoint reverts."), RebindableHotkeyEnum::freeCamera, "Free Camera##hce", settings->freeCameraToggle));

				case GUIElementEnum::hceFreecamTeleportToCamera:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Teleports the player to the cameras position"), RebindableHotkeyEnum::freeCameraTeleportToCameraHotkey, "Teleport to Camera##hce", settings->freeCameraTeleportToCameraEvent));

				// Halo Campaign Evolved trigger overlay. Every row here drives the SAME settings object as the MCC
				// trigger overlay (HaloCER and MCC can never share a process), so presets and the existing Trigger
				// Overlay hotkey carry over. Only the three hce* settings are new, and FOV is new because it is the
				// one piece of HCE's camera that is not resolved - see HCETriggerOverlay.cpp.
				case GUIElementEnum::hceTriggerOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws every trigger volume in the level as a wireframe box (sector volumes as their extruded polygon) with its name. Wireframe only, so volumes are not hidden by walls."), RebindableHotkeyEnum::triggerOverlayToggleHotkey, "Trigger Overlay##hce", settings->triggerOverlayToggle));

				case GUIElementEnum::hceTriggerOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the trigger overlay"), "Trigger Overlay Settings##hce", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceTriggerOverlaySpeedrunOnly),
								createNestedElement(GUIElementEnum::hceTriggerOverlayNameFilterButton),
								createNestedElement(GUIElementEnum::hceTriggerOverlayNameFilterToggle),
								createNestedElement(GUIElementEnum::hceTriggerOverlayNameFilterExactMatch),
								createNestedElement(GUIElementEnum::hceTriggerOverlayTypesShownSubheading),
								createNestedElement(GUIElementEnum::hceTriggerOverlayRenderStyle),
								createNestedElement(GUIElementEnum::hceTriggerOverlayRenderDistance),
								createNestedElement(GUIElementEnum::hceTriggerOverlayHighlightActive),
								createNestedElement(GUIElementEnum::hceTriggerOverlayActiveColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayFlashOnHit),
								createNestedElement(GUIElementEnum::hceTriggerOverlayHitColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayHitFalloff),
								createNestedElement(GUIElementEnum::hceTriggerOverlayMessageOnHit),
								createNestedElement(GUIElementEnum::hceTriggerOverlayBspColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayBeginZoneSetColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayCheckpointGrantColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayZoneSetReport),
								createNestedElement(GUIElementEnum::hceTriggerOverlayKillColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlaySafeZoneColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayLabelColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowVertex),
								createNestedElement(GUIElementEnum::hceTriggerOverlayVertexColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayVertexScale),
								createNestedElement(GUIElementEnum::hceTriggerOverlayAlpha),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowLabels),
								createNestedElement(GUIElementEnum::hceTriggerOverlayLabelScale),
								createNestedElement(GUIElementEnum::hceTriggerOverlayBoxColour),
								createNestedElement(GUIElementEnum::hceTriggerOverlayWireframeAlpha),
							}));

				// Halo Campaign Evolved structure-BSP overlay. Separate from the trigger overlay in every way -
				// its own cheat, its own settings, its own colour - because it answers a different question and
				// you generally want one or the other on, not both at once.
				// Halo Campaign Evolved AI squad overlay. Labels each live AI with the scenario squad and squad
				// group it belongs to. Every name is read out of the scenario tag at runtime, so it works on
				// every level without a shipped per-level table.
				case GUIElementEnum::havokWorldCacheGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Saves each level's collision geometry to disk the first time the Havok Debugger walks it, then loads it instantly. Costs about 1 GB per BSP set (~15 GB total), so off by default."), std::nullopt, "World Collision Cache", settings->havokWorldCacheToggle));

				case GUIElementEnum::havokWorldCacheClearGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
						(game, ToolTipCollection("Delete every cached world from HCM_HavokWorldCache and report how much space was reclaimed. Safe at any time - anything deleted is simply rebuilt on the next visit."), std::nullopt, "Clear World Cache", settings->havokWorldCacheClearEvent));

				case GUIElementEnum::halo3TheaterInterpToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("EXPERIMENTAL. Smooths Theater playback by interpolating camera orientation, which Halo 3 otherwise steps per tick. If the game looks wrong or unstable, turn off. Halo 3 builds 1.3528, 1.3495, 1.3385."), std::nullopt, "Theater Interpolation Fix (Experimental)", settings->halo3TheaterInterpToggle));

				case GUIElementEnum::halo3TheaterInterpCrouchGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Also interpolates crouch height change so crouching in Theater is smooth. Side effect: first-person legs separate slightly from the upper body during the change. Off keeps legs attached."), std::nullopt, "Interpolate Crouch", settings->halo3TheaterInterpCrouch));

				case GUIElementEnum::hceAISquadOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Prints the scenario squad (and squad group) above every live AI's head. AI that migrated out of their spawn squad are drawn in a second colour, since squad-based script gates can misbehave there."), RebindableHotkeyEnum::hceAISquadOverlayToggleHotkey, "AI Squad Overlay", settings->hceAISquadOverlayToggle));

				case GUIElementEnum::hceAISquadOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the AI squad overlay"), "AI Squad Overlay Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceAISquadOverlayTeamsShownSubheading),
								createNestedElement(GUIElementEnum::hceAISquadOverlayTraitsShownSubheading),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowTeamGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayRenderDistanceGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayLabelScaleGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayLabelHeightGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayCreatureScaleGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayNeutralColourGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayRequiredColourGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayTimedColourGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayMigratedColourGUI),
							}));

				case GUIElementEnum::hceAISquadOverlayTeamsShownSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Which teams get labelled. Defaults to all of them. The team is read from the live object, not the squad's tag data, so an AI that has switched sides is bucketed by what it actually is now."), "Teams Shown", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowPlayerGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowHumanGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowCovenantGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowFloodGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowSentinelGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayShowOtherGUI),
							}));

				case GUIElementEnum::hceAISquadOverlayShowPlayerGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI on the PLAYER team - marines and other allies that share your team index."), std::nullopt, "Player", settings->hceAISquadOverlayShowPlayer));

				case GUIElementEnum::hceAISquadOverlayShowHumanGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI on the HUMAN team."), std::nullopt, "Human", settings->hceAISquadOverlayShowHuman));

				case GUIElementEnum::hceAISquadOverlayShowCovenantGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI on the COVENANT team."), std::nullopt, "Covenant", settings->hceAISquadOverlayShowCovenant));

				case GUIElementEnum::hceAISquadOverlayShowFloodGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI on the FLOOD team."), std::nullopt, "Flood", settings->hceAISquadOverlayShowFlood));

				case GUIElementEnum::hceAISquadOverlayShowSentinelGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI on the SENTINEL team."), std::nullopt, "Sentinel", settings->hceAISquadOverlayShowSentinel));

				case GUIElementEnum::hceAISquadOverlayShowOtherGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Label AI whose team index is none of the named ones above. Turn this off to hide the odd scripted or neutral actor without hiding anything you care about."), std::nullopt, "Other", settings->hceAISquadOverlayShowOther));

				case GUIElementEnum::hceAISquadOverlayTraitsShownSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Which classifications get their own colour. They don't hide anyone: an AI with its trait off gets a neutral label. The label takes the highest-priority enabled trait (Required, Timed, Migrated)."), "Traits Shown", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceAISquadOverlayTraitRequiredGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayTraitTimedGUI),
								createNestedElement(GUIElementEnum::hceAISquadOverlayTraitMigratedGUI),
							}));

				case GUIElementEnum::hceAISquadOverlayTraitRequiredGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Colour AI whose death is REQUIRED to progress right now - a script gate with no timeout."), std::nullopt, "Required", settings->hceAISquadOverlayTraitRequired));

				case GUIElementEnum::hceAISquadOverlayTraitTimedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Colour AI behind a gate that gives up on its own after a timeout, and show the remaining seconds. Turning this off also hides the countdown."), std::nullopt, "Timed Gate", settings->hceAISquadOverlayTraitTimed));

				case GUIElementEnum::hceAISquadOverlayTraitMigratedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Colour AI that have MIGRATED out of the squad they spawned in. This is the trait most likely to sit on top of another one, so it is the first to turn off when you only care about what has to die."), std::nullopt, "Migrated", settings->hceAISquadOverlayTraitMigrated));

				case GUIElementEnum::hceAISquadOverlayShowTeamGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("Add the team name to each label, on its own line. Useful when you have several teams shown at once and the squad names alone do not tell them apart."), std::nullopt, "Show Team On Label", settings->hceAISquadOverlayShowTeam));

				case GUIElementEnum::hceAISquadOverlayRenderDistanceGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
						(game, ToolTipCollection("How far away AI stop being labelled, in world units (1 unit = 10 feet). Labels are screen-space text, so a large distance in a big fight turns into unreadable overlap long before it costs you frames."), "Label Render Distance", settings->hceAISquadOverlayRenderDistance));

				case GUIElementEnum::hceAISquadOverlayLabelScaleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(4.f, 64.f)>>
						(game, ToolTipCollection("Base font size of the labels, in pixels. Distant labels are scaled down from this (to a floor of about a third) so a crowded fight stays readable instead of becoming a wall of overlapping text."), "Label Size", settings->hceAISquadOverlayLabelScale));

				case GUIElementEnum::hceAISquadOverlayLabelHeightGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 10.f)>>
						(game, ToolTipCollection("How far above the AI's origin the label sits, in world units. The origin is at the feet, so around 0.7 puts the text just over a standard biped's head."), "Label Height", settings->hceAISquadOverlayLabelHeight));

				case GUIElementEnum::hceAISquadOverlayCreatureScaleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1f, 1.f)>>
						(game, ToolTipCollection("Label size for creatures (Flood infection forms) as a fraction of the normal label size. They are tiny and spawn in clumps, so 0.25 is about right; 1.0 matches everything else."), "Creature Label Size", settings->hceAISquadOverlayCreatureScale));

				case GUIElementEnum::hceAISquadOverlayNeutralColourGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of a normal label - an AI still in the squad it spawned in."), "Label Color", settings->hceAISquadOverlayNeutralColor));

				case GUIElementEnum::hceAISquadOverlayRequiredColourGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of AI whose death is required to progress now: they sit behind a script gate with no timeout. Only the currently active step of the level's gate chain is coloured."), "Required Color", settings->hceAISquadOverlayRequiredColor));

				case GUIElementEnum::hceAISquadOverlayTimedColourGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of AI behind a gate that gives up on its own after a timeout: killing them is faster, waiting also works. Shows remaining seconds if the overlay saw the gate start."), "Timed Gate Color", settings->hceAISquadOverlayTimedColor));

				case GUIElementEnum::hceAISquadOverlayMigratedColourGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of an AI whose CURRENT squad is not the squad it spawned in. The engine migrates actors between squads as fights collapse, and a script that counts a squad's survivors sees the current one."), "Migrated Color", settings->hceAISquadOverlayMigratedColor));

				case GUIElementEnum::hceBspOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws the level's invisible collision surfaces, the walls that block you but are never rendered. Only true structure BSP is drawn, not instanced geometry, so mostly-instanced levels may show little."), RebindableHotkeyEnum::hceBspOverlayToggleHotkey, "BSP Overlay", settings->hceBspOverlayToggle));

				case GUIElementEnum::hceBspOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the BSP overlay"), "BSP Overlay Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceBspOverlayInvisibleOnly),
								createNestedElement(GUIElementEnum::hceBspOverlayRenderStyle),
								createNestedElement(GUIElementEnum::hceBspOverlayInteriorStyle),
								createNestedElement(GUIElementEnum::hceBspOverlayRenderDistance),
								createNestedElement(GUIElementEnum::hceBspOverlayColour),
								createNestedElement(GUIElementEnum::hceBspOverlayAlpha),
								createNestedElement(GUIElementEnum::hceBspOverlayInsideColour),
								createNestedElement(GUIElementEnum::hceBspOverlayInsideAlpha),
								createNestedElement(GUIElementEnum::hceBspOverlayOccludeFarSurfaces),
								createNestedElement(GUIElementEnum::hceBspOverlayPatternContrast),
								createNestedElement(GUIElementEnum::hceBspOverlayPatternScale),
								createNestedElement(GUIElementEnum::hceBspOverlayLayerCompensation),
								createNestedElement(GUIElementEnum::hceBspOverlayFaceShading),
								createNestedElement(GUIElementEnum::hceBspOverlayShadingStrength),
								createNestedElement(GUIElementEnum::hceBspOverlaySurfaceVariation),
								createNestedElement(GUIElementEnum::hceBspOverlayWireframeColour),
								createNestedElement(GUIElementEnum::hceBspOverlayWireframeAlpha),
							}));

				// Halo Campaign Evolved soft ceiling overlay. Every row except the last three drives the SAME
				// settings object as the MCC barrier overlay - the tag data is byte-identical in shape (same
				// three types, same ignore-flag bits), HaloCER and MCC can never share a process, and sharing
				// means presets and the Barrier Overlay hotkey carry over. The three hce* rows are the ones
				// only this title can offer.
				case GUIElementEnum::hceSoftCeilingOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws the level's soft ceilings, the invisible barriers that push you back, slow you down or kill you, coloured by type. Read from the tag. Not hidden by walls."), RebindableHotkeyEnum::softCeilingOverlayToggleHotkey, "Barrier Overlay##hce", settings->softCeilingOverlayToggle));

				case GUIElementEnum::hceSoftCeilingOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the barrier overlay"), "Barrier Overlay Settings##hce", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayRenderTypes),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayRenderDirection),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayRenderDistance),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayColorAccel),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayColorKill),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayColorSlippy),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayShowDisabled),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayDisabledColour),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlaySolidTransparency),
								createNestedElement(GUIElementEnum::hceSoftCeilingOverlayWireframeTransparency),
							}));

				case GUIElementEnum::hceSoftCeilingOverlayRenderTypes:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::SoftCeilingRenderTypes, 150.f>>
						(game, ToolTipCollection("Whether to render barriers that apply to bipeds, vehicles, or either, using each barrier's own ignore flags. A barrier that doesn't affect you on foot can be hidden."), "Render Barriers Affecting:", settings->softCeilingOverlayRenderTypes));

				case GUIElementEnum::hceSoftCeilingOverlayRenderDirection:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::SoftCeilingRenderDirection, 100.f>>
						(game, ToolTipCollection("Whether to render the front of barriers, the back, or both. Barriers are single-sided triangles, so BOTH is usually what you want - FRONT or BACK is for working out which way one faces."), "Render Barriers Direction:", settings->softCeilingOverlayRenderDirection));

				case GUIElementEnum::hceSoftCeilingOverlayRenderDistance:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
						(game, ToolTipCollection("How far away barriers stop being drawn, in world units (1 unit = 10 feet). Barriers are level-scale, so a short distance tends to clip one into a confusing fragment rather than thinning the view out."), "Barrier Render Distance", settings->hceSoftCeilingOverlayRenderDistance));

				case GUIElementEnum::hceSoftCeilingOverlayColorAccel:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of ACCELERATION barriers - the ones that push you back towards the play space."), "Acceleration Color##hce", settings->softCeilingOverlayColorAccel));

				case GUIElementEnum::hceSoftCeilingOverlayColorKill:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of SOFT KILL barriers - crossing one starts the countdown that kills you."), "Soft Kill Color##hce", settings->softCeilingOverlayColorKill));

				case GUIElementEnum::hceSoftCeilingOverlayColorSlippy:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of SLIP SURFACE barriers - the ones you slide off instead of standing on."), "Slip Surface Color##hce", settings->softCeilingOverlayColorSlippy));

				case GUIElementEnum::hceSoftCeilingOverlayShowDisabled:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Also draw barriers a mission script has switched off, in the disabled colour, instead of hiding them. Disable Barriers clears every enable bit, so with it on everything shows as disabled."), RebindableHotkeyEnum::hceSoftCeilingOverlayShowDisabledHotkey, "Show Disabled Barriers", settings->hceSoftCeilingOverlayShowDisabled));

				case GUIElementEnum::hceSoftCeilingOverlayDisabledColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of barriers that are currently switched OFF. Deliberately drab by default so an inactive barrier does not read as a real one."), "Disabled Barrier Color", settings->hceSoftCeilingOverlayDisabledColor));

				case GUIElementEnum::hceSoftCeilingOverlaySolidTransparency:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque the filled faces are, from 0 (off entirely) to 1 (solid). Nothing here is hidden by walls, so keep this fairly low."), "Barrier Solid Opacity", settings->softCeilingOverlaySolidTransparency));

				case GUIElementEnum::hceSoftCeilingOverlayWireframeTransparency:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque the wireframe is, from 0 (off entirely) to 1 (solid). Barriers are drawn triangle by triangle, so a dense barrier's wireframe is dense - turn this down if it reads as a solid block."), "Barrier Wireframe Opacity", settings->softCeilingOverlayWireframeTransparency));

				case GUIElementEnum::hceInvisibleGeometryOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws invisible instanced collision geometry: blockers that stop you but are never rendered, like prop collision shells or the tube in a50's gravity lift. Structure BSP is the BSP Overlay's job."), std::nullopt, "Invisible Geometry Overlay", settings->hceInvisibleGeometryOverlayToggle));

				case GUIElementEnum::hceInvisibleGeometryOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the Invisible Geometry overlay"), "Invisible Geometry Overlay Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayRenderStyle),
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayRenderDistance),
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayWireframeColour),
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayWireframeAlpha),
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayColour),
								createNestedElement(GUIElementEnum::hceInvisibleGeometryOverlayAlpha),
							}));

				case GUIElementEnum::hceInvisibleGeometryOverlayRenderStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerRenderStyle, 150.f>>
						(game, ToolTipCollection("How to draw invisible geometry. Wireframe (default) outlines each piece; Solid adds a translucent fill. Nothing is hidden by walls."), "Render invisible geometry as: ", settings->hceInvisibleGeometryOverlayRenderStyle));

				case GUIElementEnum::hceInvisibleGeometryOverlayRenderDistance:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
						(game, ToolTipCollection("How far away invisible geometry is still drawn, in WORLD UNITS (1 world unit = 10 feet). Nearest pieces are drawn first."), "Invisible Geometry Render Distance (world units)", settings->hceInvisibleGeometryOverlayRenderDistance));

				case GUIElementEnum::hceInvisibleGeometryOverlayColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Fill colour, used when the render style includes Solid."), "Invisible Geometry Fill Color", settings->hceInvisibleGeometryOverlayColor));

				case GUIElementEnum::hceInvisibleGeometryOverlayAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque the fill is."), "Invisible Geometry Fill Opacity", settings->hceInvisibleGeometryOverlayAlpha));

				case GUIElementEnum::hceInvisibleGeometryOverlayWireframeColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of the outlines."), "Invisible Geometry Wireframe Color", settings->hceInvisibleGeometryOverlayWireframeColor));

				case GUIElementEnum::hceInvisibleGeometryOverlayWireframeAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque the outlines are."), "Invisible Geometry Wireframe Opacity", settings->hceInvisibleGeometryOverlayWireframeAlpha));

				case GUIElementEnum::hceVisibleGeometryOverlayToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws collision of visible instanced geometry (rocks, trees, crates, pipes, props). Complement of the Invisible Geometry Overlay. Nearest pieces in the radius drawn up to the triangle budget."), std::nullopt, "Visible Geometry Overlay", settings->hceVisibleGeometryOverlayToggle));

				case GUIElementEnum::hceVisibleGeometryOverlaySettingsSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Settings for the Visible Geometry overlay"), "Visible Geometry Overlay Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayRadius),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayTriangleBudget),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayRefreshMs),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayHideHiddenLines),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayColour),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayWireAlpha),
								createNestedElement(GUIElementEnum::hceVisibleGeometryOverlayFillAlpha),
							}));

				case GUIElementEnum::hceVisibleGeometryOverlayRadius:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 32767.f)>>
						(game, ToolTipCollection("Only collision within this many world units of the camera is drawn (1 WU = 10 feet). 32767 means no distance limit; the triangle budget below then limits what you see. Ctrl+click to type a value."), "Radius (world units)##hcevis", settings->hceVisibleGeometryOverlayRadius));

				case GUIElementEnum::hceVisibleGeometryOverlayTriangleBudget:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1000.f, 400000.f)>>
						(game, ToolTipCollection("Hard cap on collision triangles drawn, nearest pieces first. If the far end of the radius is empty, raise this rather than the radius. The log says when the budget was reached."), "Triangle Budget##hcevis", settings->hceVisibleGeometryOverlayTriangleBudget));

				case GUIElementEnum::hceVisibleGeometryOverlayRefreshMs:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(16.f, 5000.f)>>
						(game, ToolTipCollection("How often the pieces around the camera are re-selected, in milliseconds, and only when the camera has moved. Runs on a background thread, never the render thread."), "Refresh (ms)##hcevis", settings->hceVisibleGeometryOverlayRefreshMs));

				case GUIElementEnum::hceVisibleGeometryOverlayHideHiddenLines:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection("ON (default): only the nearest collision surface's lines are drawn; edges behind other collision are hidden. OFF: every edge is drawn. Lines behind the level's own walls and terrain still show."), std::nullopt, "Hide Lines Behind Collision##hcevis", settings->hceVisibleGeometryOverlayHideHiddenLines));

				case GUIElementEnum::hceVisibleGeometryOverlayColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of the visible geometry's collision."), "Visible Geometry Color##hcevis", settings->hceVisibleGeometryOverlayColor));

				case GUIElementEnum::hceVisibleGeometryOverlayWireAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("Opacity of the collision wireframe."), "Wireframe Opacity##hcevis", settings->hceVisibleGeometryOverlayWireAlpha));

				case GUIElementEnum::hceVisibleGeometryOverlayFillAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("Opacity of filled collision faces. Zero (the default) draws wireframe only - solid collision is an opaque wall."), "Fill Opacity##hcevis", settings->hceVisibleGeometryOverlayFillAlpha));

				case GUIElementEnum::hceBspOverlayInvisibleOnly:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("ON: draw only surfaces with the collision INVISIBLE flag. OFF (default): draw the whole structural shell. On the first level tested no surface had the flag, so ON drew nothing. Leave it off normally."), RebindableHotkeyEnum::hceBspOverlayInvisibleOnlyHotkey, "Invisible Surfaces Only", settings->hceBspOverlayInvisibleOnly));

				case GUIElementEnum::hceBspOverlayRenderStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerRenderStyle, 150.f>>
						(game, ToolTipCollection("How to draw BSP surfaces: filled faces are rebuilt from collision edge rings, with edges as fallback. Nothing is hidden by walls, so keep the fill fairly transparent."), "Render BSP surfaces as: ", settings->hceBspOverlayRenderStyle));

				case GUIElementEnum::hceBspOverlayInteriorStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerInteriorStyle, 100.f>>
						(game, ToolTipCollection("What to draw for structure surfaces the camera is inside. NORMAL draws them as from outside (reads as two-sided); DON'T RENDER skips them. You are usually inside the shell, so leave this on Normal."), "Render Solid Interior as: ", settings->hceBspOverlayInteriorStyle));

				case GUIElementEnum::hceBspOverlayRenderDistance:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
						(game, ToolTipCollection("How far away BSP surfaces are still drawn, in world units (1 world unit = 10 feet). Higher than the trigger default because the shell is one connected object."), "BSP Render Distance (world units)", settings->hceBspOverlayRenderDistance));

				case GUIElementEnum::hceBspOverlayColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of surfaces whose front side points at the camera: the walls around you in a corridor, or a building's exterior from outside. Based on which way a surface faces, not which room you are in."), "BSP Color (faces you)", settings->hceBspOverlayColor));

				case GUIElementEnum::hceBspOverlayInsideColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of surfaces that point away from the camera, typically the far side of the shell you are in. A separate colour gives interiors depth."), "BSP Color (faces away)", settings->hceBspOverlayInsideColor));

				case GUIElementEnum::hceBspOverlayInsideAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque faces-away surfaces are. These sit behind the ones facing you, so a lower value here keeps the near geometry readable."), "BSP Opacity Multiplier (faces away)", settings->hceBspOverlayInsideAlpha));

				case GUIElementEnum::hceBspOverlayAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque surfaces facing you are. A depth pre-pass blends only the nearest surface per pixel, so this value is what you actually see."), "BSP Opacity Multiplier (faces you)", settings->hceBspOverlayAlpha));

				case GUIElementEnum::hceBspOverlayWireframeAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("How opaque the BSP wireframe lines are. Keep this high - the outlines are what make a surface read as a wall rather than as a wash of colour."), "BSP Wireframe Opacity", settings->hceBspOverlayWireframeAlpha));

				case GUIElementEnum::hceBspOverlayWireframeColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of the BSP wireframe. Keep it clearly different from the fill colours, or the outlines become invisible."), "Wireframe Color", settings->hceBspOverlayWireframeColor));

				case GUIElementEnum::hceBspOverlayLayerCompensation:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 8.f)>>
						(game, ToolTipCollection("Makes a surface seen from inside as strong as the same shell seen from outside, where several surfaces stack up. 1 = off; 3 is about what a simple room costs. The opacity slider can't fix this."), "Inside Layer Compensation", settings->hceBspOverlayLayerCompensation));

				case GUIElementEnum::hceBspOverlayPatternContrast:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("Draws a world-space checker pattern on filled surfaces. It tells a surface from a hole, since a checker only appears where geometry is, and gives interiors depth. 0 turns it off."), "Surface Pattern Contrast", settings->hceBspOverlayPatternContrast));

				case GUIElementEnum::hceBspOverlayPatternScale:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.01f, 20.f)>>
						(game, ToolTipCollection("Checker cell size in WORLD units (1 unit = 10 feet on this title, so 0.5 is roughly a chest-high square). Smaller reads better up close and shimmers at distance; larger is calmer across a whole level."), "Surface Pattern Size", settings->hceBspOverlayPatternScale));

				case GUIElementEnum::hceBspOverlayOccludeFarSurfaces:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("OFF (default): every surface blends through every other, showing the whole shell at once. ON: nearer surfaces hide those behind, so interiors read as solid walls. OFF to survey, ON to read a room."), RebindableHotkeyEnum::hceBspOverlayOccludeFarSurfacesHotkey, "Near Surfaces Hide Far Ones", settings->hceBspOverlayOccludeFarSurfaces));

				case GUIElementEnum::hceBspOverlaySurfaceVariation:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 0.5f)>>
						(game, ToolTipCollection("Gives each collision surface a slightly different tint so adjacent wall pieces read as separate. Varies per surface, not per triangle, and keeps overall brightness. 0 turns it off."), "Per-Surface Variation", settings->hceBspOverlaySurfaceVariation));

				case GUIElementEnum::hceBspOverlayShadingStrength:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 0.8f)>>
						(game, ToolTipCollection("How much darker surfaces facing away from the light get. 0 is flat; higher separates floors, walls and ceilings more. If faces look like holes, lower this or raise opacity."), "Shading Strength", settings->hceBspOverlayShadingStrength));

				case GUIElementEnum::hceBspOverlayFaceShading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Shade each surface by which way it faces, so floors, walls and ceilings are visibly different tones. Leave ON: without it, standing inside a room fills the screen with one flat colour."), RebindableHotkeyEnum::hceBspOverlayFaceShadingHotkey, "Shade Faces By Angle", settings->hceBspOverlayFaceShading));

				case GUIElementEnum::hceDisableFadeFromBlackGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Removes the fade-in from black so you get your view back instantly: checkpoint reverts, level loads, mission restarts and script or cinematic fades. Fades to black still play."), RebindableHotkeyEnum::hceDisableFadeFromBlackHotkey, "Disable Fade From Black", settings->hceDisableFadeFromBlackToggle));

				case GUIElementEnum::hceSwitchZoneSetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHCESwitchZoneSet>
						(game, ToolTipCollection("Switch to any zone set the current level declares. Pick one and press the button. Can load unreached geometry or unload what you stand on. '<- current' updates on request, before BSPs finish loading."), std::nullopt, "Switch Zone Set", settings->hceSwitchZoneSetEvent));

				case GUIElementEnum::h5LuaConsoleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIH5LuaConsole>
						(game, ToolTipCollection("Run Lua in Halo 5's own campaign script engine (object_create, ai_place and the rest). Needs a level loaded. A bare name runs as a call; arguments need Lua syntax. Names are case-sensitive."), std::nullopt, settings));

				case GUIElementEnum::hceConsoleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHCEConsole>
						(game, ToolTipCollection("Run HaloScript commands with autocomplete. Type a bare command such as game_revert or cheat_medusa 1, no brackets. Greyed suggestions do nothing. Runs on the next frame; a level must be loaded."), RebindableHotkeyEnum::hceConsoleHotkey, settings));

				case GUIElementEnum::hceFreecamDriftGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
						(game, ToolTipCollection("Gives the free camera weight: it eases into motion and coasts to a stop instead of stopping dead. Good for cinematic shots. Only what you see trails the input, by the coast time below."), RebindableHotkeyEnum::hceFreecamDriftHotkey, "Camera Drift", settings->hceFreecamDriftToggle, headerChildElements
							{
							createNestedElement(GUIElementEnum::hceFreecamDriftAmountGUI),
							}));

				case GUIElementEnum::hceFreecamDriftAmountGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 10.f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat)>>
						(game, ToolTipCollection("Seconds of glide after you stop moving. Default 2 coasts about two seconds after release. 0 turns drift off. It never overshoots, but high values trail your input heavily. Scales with Game Speed."), "Coast Time (s)##hceFreecam", settings->hceFreecamDriftAmount));

				case GUIElementEnum::hceFreecamKeepPositionGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Reverting normally snaps the free camera back to where it was at the checkpoint. With this on, HCM restores your current camera position and orientation after the revert."), RebindableHotkeyEnum::hceFreecamKeepPositionHotkey, "Keep Freecam Position On Revert", settings->hceFreecamKeepPositionToggle));

				case GUIElementEnum::hceGameSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIGameSpeed>
						(game, ToolTipCollection("Multiplies the simulation speed (1 = normal). Unlike the Speedhack it does not touch the process clock; rendering and audio stay real time. Stops keeping up past about 5x. Suppresses scripted slow-mo."), RebindableHotkeyEnum::hceGameSpeedHotkey, settings));

				case GUIElementEnum::hceFreecamGimbalBypassGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Removes the camera's pitch clamp so you can look past vertical without the view flipping. Byte-checked first, so a game update disables this rather than corrupting code."), RebindableHotkeyEnum::hceFreecamGimbalBypassHotkey, "Gimbal Lock Bypass", settings->hceFreecamGimbalBypassToggle));

				case GUIElementEnum::hceCameraRollGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(-180.f, 180.f)>>
						(game, ToolTipCollection("Banks the free camera about its facing direction, in degrees. Not Gimbal Lock Bypass. Only applies while the free camera is on. The camera's roll keys (default G and T) adjust it while held."), "Camera Roll (degrees)", settings->hceCameraRollDegrees));

				// The two roll keys were always live on HaloCER - HCECameraRoll polls these same bindings - but
				// they had no rebind row here, so the only way to discover or change them was the tooltip above.
				// Same RebindableHotkeyEnum values the MCC free camera uses: HaloCER and the MCC games can never
				// share a process, so there is nothing to collide with.
				case GUIElementEnum::hceCameraRollLeftBindingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Tilt the camera anticlockwise. Click to nudge, or bind a key and hold to wind it continuously; Tilt Speed governs both. Unbound by default."), RebindableHotkeyEnum::hceCameraRollLeftBinding, "Tilt Left", settings->hceCameraTiltLeftEvent));

				case GUIElementEnum::hceCameraRollRightBindingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Tilt the camera clockwise. Click to nudge, or bind a key and hold to wind it continuously - one click is worth a tenth of a second of holding, so Tilt Speed governs both. Unbound by default."), RebindableHotkeyEnum::hceCameraRollRightBinding, "Tilt Right", settings->hceCameraTiltRightEvent));

				case GUIElementEnum::hceCameraRollSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 720.f)>>
						(game, ToolTipCollection("How fast the Tilt Left / Tilt Right keys wind the roll, in degrees per second. This is an HCM input rate - the game itself has no tilt speed."), "Tilt Speed (deg/sec)", settings->hceCameraRollSpeed));

				case GUIElementEnum::hceCameraMoveSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1f, 50.f)>>
						(game, ToolTipCollection("Multiplies how fast the camera moves, scaling forward, side and up translation evenly. 1.0 is the game's default."), "Camera Move Speed", settings->hceCameraMoveSpeed));

				case GUIElementEnum::hceCameraMoveSpeedResetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Puts camera movement speed back to the game's default of 1.0."), RebindableHotkeyEnum::hceCameraMoveSpeedResetHotkey, "Restore Default Move Speed", settings->hceCameraMoveSpeedResetEvent));

				// Like the tilt keys, these bindings were already live and polled (HCEFieldOfView::pollFovHotkeys)
				// but had no rebind row, so there was no way to see or change them.
				case GUIElementEnum::hceFieldOfViewIncreaseBindingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Widen the field of view (zoom out). Click to nudge, or bind a key and hold to wind it continuously; FOV Speed governs both. Only works while the Field of View toggle is on. Unbound by default."), RebindableHotkeyEnum::hceFieldOfViewIncreaseBinding, "FOV Up", settings->hceFieldOfViewIncreaseEvent));

				case GUIElementEnum::hceFieldOfViewDecreaseBindingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Narrow the field of view (zoom in). Click to nudge or hold a bound key to wind continuously; FOV Speed governs both. Only works while the Field of View toggle is on. Unbound by default."), RebindableHotkeyEnum::hceFieldOfViewDecreaseBinding, "FOV Down", settings->hceFieldOfViewDecreaseEvent));

				case GUIElementEnum::hceFieldOfViewSpeedGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 360.f)>>
						(game, ToolTipCollection("How fast the field-of-view keys wind the slider, in degrees per second. An HCM input rate, like Tilt Speed."), "FOV Speed (deg/sec)", settings->hceFieldOfViewSpeed));

				case GUIElementEnum::hceCameraRollResetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Levels the horizon again - sets Camera Roll back to zero."), RebindableHotkeyEnum::hceCameraRollResetHotkey, "Restore Default Tilt", settings->hceCameraRollResetEvent));

				case GUIElementEnum::hceFieldOfViewResetGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Puts the field of view back to the game's own default, read live from the engine, so it is right for this level."), RebindableHotkeyEnum::hceFieldOfViewResetHotkey, "Restore Default FOV", settings->hceFieldOfViewResetEvent));

				case GUIElementEnum::hceFieldOfViewToggleGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Locks the game's field of view to the value below. Turning this off hands the field of view straight back to the game, and so does closing HCM - it is never left locked behind you."), RebindableHotkeyEnum::hceFieldOfViewHotkey, "Field of View", settings->hceFieldOfViewToggle));

				case GUIElementEnum::hceFieldOfViewValueGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(50.f, 140.f)>>
						(game, ToolTipCollection("HORIZONTAL field of view in degrees, which is what this engine stores - roughly 78 is the game's own default. Only does anything while the Field of View toggle above is on."), "Field of View (degrees)", settings->hceFieldOfViewDegrees));

				case GUIElementEnum::hceFreecamNoclipGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Lets the theater / free camera pass through geometry instead of colliding with it. Writes the camera's collision distance directly - no game code is patched."), RebindableHotkeyEnum::hceFreecamNoclipHotkey, "Camera Noclip", settings->hceFreecamNoclipToggle));

				case GUIElementEnum::hceSkyFixGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Stops the sky and world lighting vanishing out of bounds by keeping World Partition areas loaded. Turn it on while still in bounds. Memory grows and streaming may hitch on big levels; turn off after."), RebindableHotkeyEnum::hceSkyFixHotkey, "Sky Fix", settings->hceSkyFixToggle));

				case GUIElementEnum::hceTriggerOverlayRenderStyle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnum<SettingsEnums::TriggerRenderStyle, 150.f>>
						(game, ToolTipCollection("What style to render trigger volumes as. Halo Campaign Evolved renders through D3D12 with no depth buffer, so SOLID faces sort back-to-front against each other but are not hidden by walls."), "Render trigger volumes as: ", settings->triggerOverlayRenderStyle));

				case GUIElementEnum::hceTriggerOverlayAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.1, 1.f)>>
						(game, ToolTipCollection("Multiplies how opaque the SOLID faces are, from 0 (fully transparent) to 1 (fully opaque). Wireframe opacity is separate."), "Trigger Opacity Multiplier", settings->triggerOverlayAlpha));

				case GUIElementEnum::hceTriggerOverlayHighlightActive:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Colour trigger volumes that mission scripts are actively testing right now differently from dormant ones, showing what could actually fire."), RebindableHotkeyEnum::hceTriggerOverlayHighlightActiveHotkey, "Highlight Active Triggers", settings->hceTriggerOverlayHighlightActive));

				// Hit flash / hit list. These drive the SAME settings the MCC trigger overlay uses
				// (triggerOverlayCheckHit*), because they model exactly the same thing - the script tested
				// this volume and the answer was YES. HaloCER needs its own ELEMENTS only because
				// requiredServicesPerGUIElement is keyed by element and the MCC ones map to the MCC cheat.
				case GUIElementEnum::hceTriggerOverlayFlashOnHit:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Flash a trigger volume the instant a mission script tests it and you are inside it, which is when you actually crossed the trigger, not just when a script is watching it."), RebindableHotkeyEnum::hceTriggerOverlayFlashOnHitHotkey, "Flash Triggers on Hit", settings->triggerOverlayCheckHitToggle));

				case GUIElementEnum::hceTriggerOverlayHitColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Colour a trigger flashes when you hit it. This outranks every other colour, including the active highlight - a hit is momentary and is the thing you are watching for."), "Colour on Hit", settings->triggerOverlayCheckHitColor));

				case GUIElementEnum::hceTriggerOverlayHitFalloff:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputInt<>>
						(game, ToolTipCollection("How long the hit flash lasts, in ticks (60 = one second). Short is usually better - the point is to see the exact moment you crossed."), "Flash for x ticks", settings->triggerOverlayCheckHitFalloff));

				case GUIElementEnum::hceTriggerOverlayMessageOnHit:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Print each trigger volume's name to the message feed as you hit it, building a list of what you triggered this run. Only volumes a script tested successfully are listed."), RebindableHotkeyEnum::hceTriggerOverlayMessageOnHitHotkey, "Print Triggers on Hit", settings->triggerOverlayMessageOnCheckHit));

				case GUIElementEnum::hceTriggerOverlayActiveColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of trigger volumes a mission script has tested in the last few seconds."), "Active Trigger Color", settings->hceTriggerOverlayActiveColor));

				case GUIElementEnum::hceTriggerOverlayTypesShownSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Which kinds of trigger volume to draw"), "Types Shown##hce", headerChildElements
							{
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowRegular),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowKill),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowZoneSet),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowBeginZoneSet),
								createNestedElement(GUIElementEnum::hceTriggerOverlayShowCheckpointGrant),
							}));

				case GUIElementEnum::hceTriggerOverlayShowRegular:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Ordinary trigger volumes, i.e. not kill volumes or zone-set switches. Includes checkpoint-grant volumes (own colour) and sector volumes. Turn on Checkpoint Grant Triggers alone to see only those."), RebindableHotkeyEnum::hceTriggerOverlayShowRegularHotkey, "Regular Triggers", settings->hceTriggerOverlayShowRegular));

				case GUIElementEnum::hceTriggerOverlayShowKill:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Kill volumes - the ones that kill you for leaving the intended play space."), RebindableHotkeyEnum::hceTriggerOverlayShowKillHotkey, "Kill Triggers", settings->hceTriggerOverlayShowKill));

				case GUIElementEnum::hceTriggerOverlayShowBeginZoneSet:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("BEGIN zone-set volumes. Crossing one runs prepare_to_switch_to_zone_set, which only unloads BSPs and retires forbidden designer zones; it loads nothing. If no BSP is shared, a full switch occurs."), RebindableHotkeyEnum::hceTriggerOverlayShowBeginZoneSetHotkey, "Begin Zoneset Triggers", settings->hceTriggerOverlayShowBeginZoneSet));

				case GUIElementEnum::hceTriggerOverlayShowCheckpointGrant:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Trigger volumes that grant a checkpoint when entered (373 across all 13 levels). A curated list from the scripts: 285 grant unconditionally; for the other 88 the condition prints when you enter."), RebindableHotkeyEnum::hceTriggerOverlayShowCheckpointGrantHotkey, "Checkpoint Grant Triggers", settings->hceTriggerOverlayShowCheckpointGrant));

				case GUIElementEnum::hceTriggerOverlayCheckpointGrantColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of trigger volumes that grant a checkpoint on entry."), "Checkpoint Grant Color", settings->hceTriggerOverlayCheckpointGrantColor));

				case GUIElementEnum::hceTriggerOverlayShowZoneSet:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("COMMIT zone-set volumes. Crossing one runs switch_zone_set, which brings in new BSP geometry and drops what the new zone set does not want. Read from the scenario's zone-set switch block."), RebindableHotkeyEnum::hceTriggerOverlayShowZoneSetHotkey, "Zoneset Triggers", settings->hceTriggerOverlayShowZoneSet));

				// Halo Campaign Evolved name filter. Drives the SAME triggerOverlayFilterString / FilterToggle /
				// FilterExactMatch settings as MCC's version, so presets carry across; only the picker differs.
				case GUIElementEnum::hceTriggerOverlayNameFilterButton:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
						(game, ToolTipCollection("Choose which trigger volumes to draw by name from a checkbox list, with a search box and All / None per category. Needs a level loaded with the Trigger Overlay on. Selecting names turns the filter on."), RebindableHotkeyEnum::hceTriggerOverlayEditNameFilterHotkey, "Filter by Name...##hce", settings->hceTriggerOverlayEditNameFilterEvent));

				case GUIElementEnum::hceTriggerOverlayNameFilterToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Apply the name filter chosen above. Off: every volume the Types Shown toggles allow is drawn. An empty name list means draw everything, so turning this on before picking anything changes nothing."), RebindableHotkeyEnum::hceTriggerOverlayNameFilterToggleHotkey, "Filter by Name##hce", settings->triggerOverlayFilterToggle));

				case GUIElementEnum::hceTriggerOverlayNameFilterExactMatch:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("ON: a volume is drawn only if its name matches a chosen name exactly (what the picker produces). OFF: entries are case-insensitive substrings, so typing 'kill' draws every volume containing it."), RebindableHotkeyEnum::hceTriggerOverlayNameFilterExactMatchHotkey, "Exact Name Match##hce", settings->triggerOverlayFilterExactMatch));

				case GUIElementEnum::hceTriggerOverlaySpeedrunOnly:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws only the trigger volumes a speedrun must hit, from community completion-requirement lists. It is a fixed name list, so a game update renaming a volume silently drops it."), RebindableHotkeyEnum::hceTriggerOverlaySpeedrunOnlyHotkey, "Speedrun Triggers Only", settings->hceTriggerOverlaySpeedrunOnly));

				case GUIElementEnum::hceTriggerOverlayBspColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of COMMIT zone-set volumes - the ones that actually load new BSP geometry in. Read from the scenario's zone-set switch block ('commit zone set', element +0x06), not guessed from names."), "Zone-set Color", settings->hceTriggerOverlayBspColor));

				case GUIElementEnum::hceTriggerOverlayBeginZoneSetColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of BEGIN zone-set volumes, which only unload geometry and free the object budget ahead of a switch."), "Begin Zone-set Color", settings->hceTriggerOverlayBeginZoneSetColor));

				case GUIElementEnum::hceTriggerOverlayZoneSetReport:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("On entering a zone-set volume, prints the target zone set's name and which BSPs it loads and unloads. HCM's own containment test, not an engine event, so it can fire slightly before or after it."), RebindableHotkeyEnum::hceTriggerOverlayZoneSetReportHotkey, "Print Zoneset Loads on Entry", settings->hceTriggerOverlayZoneSetReport));

				case GUIElementEnum::hceTriggerOverlayKillColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of KILL volumes - the ones that kill you for going IN. Read from the scenario's kill trigger block, not guessed from names."), "Kill Volume Color", settings->hceTriggerOverlayKillColor));

				case GUIElementEnum::hceTriggerOverlaySafeZoneColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of safe zones, the opposite of a kill volume: the game kills you when you are inside none of them. Shown under the Kill Triggers toggle, in a different colour."), "Safe Zone Color", settings->hceTriggerOverlaySafeZoneColor));

				case GUIElementEnum::hceTriggerOverlayLabelColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Colour of the trigger volume name labels. Separate from the volume colours, which are too dark to read as text."), "Label Text Color", settings->hceTriggerOverlayLabelColor));

				case GUIElementEnum::hceTriggerOverlayShowVertex:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draws a sphere at the point the game uses to test the player against trigger volumes."), RebindableHotkeyEnum::hceTriggerOverlayShowVertexHotkey, "Show Trigger Vertex", settings->hceTriggerOverlayShowVertex));

				case GUIElementEnum::hceTriggerOverlayVertexColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
						(game, ToolTipCollection("Color of the sphere drawn at the players trigger vertex"), "Trigger Vertex Color", settings->triggerOverlayPositionColor));

				case GUIElementEnum::hceTriggerOverlayVertexScale:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.01f, 4.f)>>
						(game, ToolTipCollection("Size of the sphere drawn at the players trigger vertex"), "Trigger Vertex Scale", settings->triggerOverlayPositionScale));

				case GUIElementEnum::hceTriggerOverlayRenderDistance:
					// Slider goes to the setting's full validated range. It used to stop at 500, which silently
					// capped big levels: on The Maw the speedrun volumes are far enough away that they matched
					// the filter but were culled before drawing, which looks exactly like "this level has none".
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(1.f, 2000.f)>>
						(game, ToolTipCollection("How far away a trigger volume is still drawn, in world units (1 world unit = 10 feet; 2000 is the whole map). Turn it up if a filter looks empty, as distance can cull a filtered-in volume."), "Render Distance (world units)", settings->hceTriggerOverlayRenderDistance));

				case GUIElementEnum::hceTriggerOverlayShowLabels:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
						(game, ToolTipCollection("Draw each volume's scripting name at its centre. Volumes whose name cannot be resolved are labelled trigger_<index>."), RebindableHotkeyEnum::hceTriggerOverlayShowLabelsHotkey, "Show Labels##hce", settings->hceTriggerOverlayShowLabels));

				case GUIElementEnum::hceTriggerOverlayLabelScale:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(6.f, 120.f)>>
						(game, ToolTipCollection("How large the font size of the label should be, in pts"), "Label Font Size##hce", settings->triggerOverlayLabelScale));

				case GUIElementEnum::hceTriggerOverlayBoxColour:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPickerAlpha<true>>
						(game, ToolTipCollection("Colour for ordinary trigger volumes, boxes and sector prisms alike"), "Trigger Colour##hce", settings->triggerOverlayNormalColor));

				case GUIElementEnum::hceTriggerOverlayWireframeAlpha:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam<float>(0.f, 1.f)>>
						(game, ToolTipCollection("Opacity of the wireframe lines"), "Wireframe Opacity##hce", settings->triggerOverlayWireframeAlpha));

				case GUIElementEnum::freeCameraSettingsSimpleSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Simple settings for Free Camera"), "Free Camera Simple Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::freeCameraTeleportToCamera),
								createNestedElement(GUIElementEnum::freeCameraGameInputDisable),
								createNestedElement(GUIElementEnum::freeCameraCameraInputDisable),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraBindingsSubheading),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationSpeed),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraIncreaseTranslationSpeedHotkey),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraDecreaseTranslationSpeedHotkey),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationSpeed),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPosition),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionChildren),

							}));

				case GUIElementEnum::freeCameraSettingsAdvancedSubheading:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<false>>
						(game, ToolTipCollection("Advanced settings for Free Camera"), "Free Camera Advanced Settings", headerChildElements
							{
								createNestedElement(GUIElementEnum::freeCameraTeleportToCamera),
								createNestedElement(GUIElementEnum::freeCameraTeleportToCameraSlightlyBehind),
								createNestedElement(GUIElementEnum::freeCameraThirdPersonRendering),
								createNestedElement(GUIElementEnum::freeCameraDisableScreenEffects),
								createNestedElement(GUIElementEnum::freeCameraGameInputDisable),
								createNestedElement(GUIElementEnum::freeCameraCameraInputDisable),
								createNestedElement(GUIElementEnum::freeCameraUserInputCameraSettings),
								//createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectPosition),
								//createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPosition),
								//createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectFacing),
								//createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistance),
							}));

					case GUIElementEnum::freeCameraTeleportToCamera:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
							(game, ToolTipCollection("Teleports the player to the cameras position"), RebindableHotkeyEnum::freeCameraTeleportToCameraHotkey, "Teleport to Camera", settings->freeCameraTeleportToCameraEvent));


						case GUIElementEnum::freeCameraTeleportToCameraSlightlyBehind:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection("When teleporting to camera, instead put the player a little behind the camera"), std::nullopt, "(Teleport Behind Camera)", settings->freeCameraTeleportToCameraSlightlyBehind));


					case GUIElementEnum::freeCameraThirdPersonRendering:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Makes the player model render, and hides game UI (ala Blind skull)"), std::nullopt, "Third Person Rendering", settings->freeCameraThirdPersonRendering));

					case GUIElementEnum::freeCameraDisableScreenEffects:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
							(game, ToolTipCollection("Disables screen effects like rumble or the screen flashing when player is hit"), std::nullopt, "Disable Screen Effects", settings->freeCameraDisableScreenEffects));


					case GUIElementEnum::freeCameraGameInputDisable:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Disables game inputs while freecam active"), RebindableHotkeyEnum::freeCameraGameInputDisable, "Disable Game Inputs", settings->freeCameraGameInputDisable));

					case GUIElementEnum::freeCameraCameraInputDisable:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
							(game, ToolTipCollection("Disables direct user camera inputs (translation, rotation)"), RebindableHotkeyEnum::freeCameraCameraInputDisable, "Disable Camera Inputs", settings->freeCameraCameraInputDisable));

					case GUIElementEnum::freeCameraUserInputCameraSettings:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
							(game, ToolTipCollection("Settings for user-controlled camera"), "User Input Camera Settings", headerChildElements
								{
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraBindingsSubheading),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraBaseFOV),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationSpeed),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraIncreaseTranslationSpeedHotkey),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraDecreaseTranslationSpeedHotkey),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationSpeedChangeFactor),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationInterpolator),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationSpeed),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationInterpolator),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationScalesToFOV),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraFOVSpeed),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraFOVInterpolator),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraNonLinearFOVAtMinimum),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraNonLinearFOVAtMaximum),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPosition),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionChildren),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotation),
									createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotationChildren),
									//createNestedElement(GUIElementEnum::freeCameraUserInputCameraMaintainVelocity),
								}));

						case GUIElementEnum::freeCameraUserInputCameraBindingsSubheading:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISubHeading<true>>
								(game, ToolTipCollection("Input bindings for user-controlled camera"), "Input Bindings", headerChildElements
									{
										createNestedElement(GUIElementEnum::freeCameraUserInputTranslateUpBinding),
										createNestedElement(GUIElementEnum::freeCameraUserInputTranslateDownBinding),
										createNestedElement(GUIElementEnum::freeCameraUserInputRollLeftBinding),
										createNestedElement(GUIElementEnum::freeCameraUserInputRollRightBinding),
										createNestedElement(GUIElementEnum::freeCameraUserInputFOVIncreaseBinding),
										createNestedElement(GUIElementEnum::freeCameraUserInputFOVDecreaseBinding),
									}));

							case GUIElementEnum::freeCameraUserInputTranslateUpBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Move camera up"), RebindableHotkeyEnum::cameraTranslateUpBinding, "Translate Up" ));

							case GUIElementEnum::freeCameraUserInputTranslateDownBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Move camera down"), RebindableHotkeyEnum::cameraTranslateDownBinding, "Translate Down"));

							case GUIElementEnum::freeCameraUserInputRollLeftBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Roll the camera to the left"), RebindableHotkeyEnum::cameraRollLeftBinding, "Roll Left"));

							case GUIElementEnum::freeCameraUserInputRollRightBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Roll the camera to the right"), RebindableHotkeyEnum::cameraRollRightBinding, "Roll Right"));

							case GUIElementEnum::freeCameraUserInputFOVIncreaseBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Increase the cameras Field of View (zoom out)"), RebindableHotkeyEnum::cameraFOVIncreaseBinding, "Increase FOV"));

							case GUIElementEnum::freeCameraUserInputFOVDecreaseBinding:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHotkeyOnly<103>>
									(game, ToolTipCollection("Decrease the cameras Field of View (zoom in)"), RebindableHotkeyEnum::cameraFOVDecreaseBinding, "Decrease FOV"));



						case GUIElementEnum::freeCameraUserInputCameraBaseFOV:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 179.9999f)>>
								(game, ToolTipCollection("Field of view of the camera, in vertical degrees (assumes 16:9. same as in-game-menu) (before any adjustment by anchorFOVtoObjectDistance automation)"), "Base Field of View", settings->freeCameraUserInputCameraBaseFOV));

						case GUIElementEnum::freeCameraUserInputCameraTranslationSpeed:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
								(game, ToolTipCollection("How fast the camera translates (moves positionally)"), "Translation Speed", settings->freeCameraUserInputCameraTranslationSpeed));


						case GUIElementEnum::freeCameraUserInputCameraIncreaseTranslationSpeedHotkey:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
								(game, ToolTipCollection("Increases camera translation speed by a scale factor"), RebindableHotkeyEnum::freeCameraUserInputCameraIncreaseTranslationSpeedHotkey, "Increase Translation Speed", settings->freeCameraUserInputCameraIncreaseTranslationSpeedHotkey));

						case GUIElementEnum::freeCameraUserInputCameraDecreaseTranslationSpeedHotkey:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
								(game, ToolTipCollection("Decreases camera translation speed by a scale factor"), RebindableHotkeyEnum::freeCameraUserInputCameraDecreaseTranslationSpeedHotkey, "Decrease Translation Speed", settings->freeCameraUserInputCameraDecreaseTranslationSpeedHotkey));

						case GUIElementEnum::freeCameraUserInputCameraTranslationSpeedChangeFactor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
								(game, ToolTipCollection("How much the camera translation speed changes when you press the above hotkeys"), "Translation Speed Change Factor", settings->freeCameraUserInputCameraTranslationSpeedChangeFactor));



						case GUIElementEnum::freeCameraUserInputCameraTranslationInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsEnums::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"),  "Translation Interpolation##UserInputCamera", settings->freeCameraUserInputCameraTranslationInterpolator,
									vectorOfHeaderChildElements
							{
								headerChildElements{},
								headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationInterpolatorLinearFactor) },
								headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationInterpolatorLinearFactor),
													 createNestedElement(GUIElementEnum::freeCameraUserInputCameraTranslationInterpolatorDrift) }
							}
							));

						case GUIElementEnum::freeCameraUserInputCameraTranslationInterpolatorLinearFactor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 1.f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat | ImGuiSliderFlags_Logarithmic)>>
								(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."),  "Snap Factor##UserInputCameraTranslation", settings->freeCameraUserInputCameraTranslationInterpolatorLinearFactor));



						case GUIElementEnum::freeCameraUserInputCameraTranslationInterpolatorDrift:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 0.95f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat)>>
								(game, ToolTipCollection("How long the camera keeps gliding after you stop giving input. 0 stops it dead (same as Linear); high values give a long, heavy coast. Snap Factor above still controls how hard it accelerates."), "Drift##UserInputCameraTranslation", settings->freeCameraUserInputCameraTranslationInterpolatorDrift));

						case GUIElementEnum::freeCameraUserInputCameraRotationInterpolatorDrift:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 0.95f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat)>>
								(game, ToolTipCollection("How long the camera keeps gliding after you stop giving input. 0 stops it dead (same as Linear); high values give a long, heavy coast. Snap Factor above still controls how hard it accelerates."), "Drift##UserInputCameraRotation", settings->freeCameraUserInputCameraRotationInterpolatorDrift));

						case GUIElementEnum::freeCameraUserInputCameraFOVInterpolatorDrift:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 0.95f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat)>>
								(game, ToolTipCollection("How long the camera keeps gliding after you stop giving input. 0 stops it dead (same as Linear); high values give a long, heavy coast. Snap Factor above still controls how hard it accelerates."), "Drift##UserInputCameraFOV", settings->freeCameraUserInputCameraFOVInterpolatorDrift));

						case GUIElementEnum::freeCameraUserInputCameraRotationSpeed:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
								(game, ToolTipCollection("How fast the camera rotates (turns about)"), "Rotation Speed", settings->freeCameraUserInputCameraRotationSpeed));

						case GUIElementEnum::freeCameraUserInputCameraRotationInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsEnums::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "Rotation Interpolation##UserInputCamera", settings->freeCameraUserInputCameraRotationInterpolator,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationInterpolatorLinearFactor) },
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationInterpolatorLinearFactor),
															 createNestedElement(GUIElementEnum::freeCameraUserInputCameraRotationInterpolatorDrift) }
									}
							));


						case GUIElementEnum::freeCameraUserInputCameraRotationScalesToFOV:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection("When enabled, rotation sensitivity will be lowered when your FOV is lowered."), std::nullopt, "Rotation speed scales w/ FOV", settings->freeCameraUserInputCameraRotationScalesToFOV));


						case GUIElementEnum::freeCameraUserInputCameraRotationInterpolatorLinearFactor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 1.f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat | ImGuiSliderFlags_Logarithmic)>>
								(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##UserInputCameraRotation", settings->freeCameraUserInputCameraRotationInterpolatorLinearFactor));



						case GUIElementEnum::freeCameraUserInputCameraFOVSpeed:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
								(game, ToolTipCollection("How fast the camera FOV changes when you hold down the increase/decrease FOV binding"), "FOV change Speed", settings->freeCameraUserInputCameraFOVSpeed));

						case GUIElementEnum::freeCameraUserInputCameraFOVInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsEnums::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "FOV Interpolation##UserInputCamera", settings->freeCameraUserInputCameraFOVInterpolator,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraFOVInterpolatorLinearFactor) },
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraUserInputCameraFOVInterpolatorLinearFactor),
															 createNestedElement(GUIElementEnum::freeCameraUserInputCameraFOVInterpolatorDrift) }
									}
							));

						case GUIElementEnum::freeCameraUserInputCameraNonLinearFOVAtMinimum:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection("Makes changing FOV with the hotkey non-linear, so it transitions slower at very low FOV values."), std::nullopt, "Non-Linear FOV at 0 degrees", settings->freeCameraUserInputCameraNonLinearFOVAtMinimum));

						case GUIElementEnum::freeCameraUserInputCameraNonLinearFOVAtMaximum:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
								(game, ToolTipCollection("Makes changing FOV with the hotkey non-linear, so it transitions slower at very high FOV values."), std::nullopt, "Non-Linear FOV at 180 degrees", settings->freeCameraUserInputCameraNonLinearFOVAtMaximum));

						case GUIElementEnum::freeCameraUserInputCameraFOVInterpolatorLinearFactor:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<SliderParam(0.f, 1.f, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat | ImGuiSliderFlags_Logarithmic)>>
								(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##UserInputCameraFOV", settings->freeCameraUserInputCameraFOVInterpolatorLinearFactor));




						case GUIElementEnum::freeCameraUserInputCameraSetPosition:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
								(game, ToolTipCollection("Set camera Position"), RebindableHotkeyEnum::freeCameraUserInputCameraSetPosition, "Set Position To##freeCameraUserInputCameraSetPosition", settings->freeCameraUserInputCameraSetPosition));


						case GUIElementEnum::freeCameraUserInputCameraSetPositionChildren:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIDummyContainer>
								(game, "freeCameraUserInputCameraSetPositionChildren", headerChildElements
									{
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionVec3),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionFillCurrent),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionCopy),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetPositionPaste),
									}
									));


							case GUIElementEnum::freeCameraUserInputCameraSetPositionVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, false, 8>>
									(game, ToolTipCollection("Position of the camera "), "Position: ##freeCameraUserInputCameraSetPositionVec3", settings->freeCameraUserInputCameraSetPositionVec3));

							case GUIElementEnum::freeCameraUserInputCameraSetPositionFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Fill data with current camera position"), std::nullopt, "Fill with Current##freeCameraUserInputCameraSetPositionCopyCurrent", settings->freeCameraUserInputCameraSetPositionFillCurrent));

							case GUIElementEnum::freeCameraUserInputCameraSetPositionCopy:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Copy Position to the clipboard"), std::nullopt, "Copy to Clipboard##freeCameraUserInputCameraSetPositionPaste", settings->freeCameraUserInputCameraSetPositionCopy));


							case GUIElementEnum::freeCameraUserInputCameraSetPositionPaste:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Paste in a Position from the clipboard"), std::nullopt, "Paste from Clipboard##freeCameraUserInputCameraSetPositionPaste", settings->freeCameraUserInputCameraSetPositionPaste));

						case GUIElementEnum::freeCameraUserInputCameraSetRotation:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
								(game, ToolTipCollection("Set camera Rotation"), RebindableHotkeyEnum::freeCameraUserInputCameraSetRotation, "Set Rotation To##freeCameraUserInputCameraSetRotation", settings->freeCameraUserInputCameraSetRotation));

						case GUIElementEnum::freeCameraUserInputCameraSetRotationChildren:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIDummyContainer>
								(game, "freeCameraUserInputCameraSetRotationChildren", headerChildElements
									{
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotationVec3),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotationFillCurrent),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotationCopy),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetRotationPaste),
									}
							));

							case GUIElementEnum::freeCameraUserInputCameraSetRotationVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared < GUIVec3 < true, false, 8, SliderParam<SimpleMath::Vector3>({ -std::numbers::pi, -std::numbers::pi / 2.f, -std::numbers::pi / 2.f } , { std::numbers::pi, std::numbers::pi / 2.f, std::numbers::pi / 2.f }) >>
									(game, ToolTipCollection("Rotation of the camera (in radians, yaw pitch roll)"), "Rotation: ##freeCameraUserInputCameraSetRotationVec3", settings->freeCameraUserInputCameraSetRotationVec3, "Yaw", "Pitch", "Roll"));

							case GUIElementEnum::freeCameraUserInputCameraSetRotationFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Fill data with current camera rotation"), std::nullopt, "Fill with Current##freeCameraUserInputCameraSetRotationCopyCurrent", settings->freeCameraUserInputCameraSetRotationFillCurrent));

							case GUIElementEnum::freeCameraUserInputCameraSetRotationCopy:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Copy Rotation to the clipboard"), std::nullopt, "Copy to Clipboard##freeCameraUserInputCameraSetRotationPaste", settings->freeCameraUserInputCameraSetRotationCopy));


							case GUIElementEnum::freeCameraUserInputCameraSetRotationPaste:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Paste in a Rotation from the clipboard"), std::nullopt, "Paste from Clipboard##freeCameraUserInputCameraSetRotationPaste", settings->freeCameraUserInputCameraSetRotationPaste));

								/* // Unfinished free camera features

						case GUIElementEnum::freeCameraUserInputCameraMaintainVelocity:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
								(game, ToolTipCollection("Maintains the current camera velocity while enabled"), RebindableHotkeyEnum::freeCameraUserInputCameraMaintainVelocity, "Maintain Velocity", settings->freeCameraUserInputCameraMaintainVelocity, headerChildElements
									{
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocity),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocityChildren),

									}
							));

						case GUIElementEnum::freeCameraUserInputCameraSetVelocity:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<true>>
								(game, ToolTipCollection("Set camera Velocity"), RebindableHotkeyEnum::freeCameraUserInputCameraSetVelocity, "Set Velocity To##freeCameraUserInputCameraSetVelocity", settings->freeCameraUserInputCameraSetVelocity));


						case GUIElementEnum::freeCameraUserInputCameraSetVelocityChildren:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIDummyContainer>
								(game, "freeCameraUserInputCameraSetVelocityChildren", headerChildElements
									{
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocityVec3),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocityFillCurrent),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocityCopy),
										createNestedElement(GUIElementEnum::freeCameraUserInputCameraSetVelocityPaste),
									}
							));


							case GUIElementEnum::freeCameraUserInputCameraSetVelocityVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, false, 8>>
									(game, ToolTipCollection("Velocity of the camera (relative to facing)"), "Velocity ##freeCameraUserInputCameraSetVelocityVec3", settings->freeCameraUserInputCameraSetVelocityVec3, "Forward", "Right", "Up"));

							case GUIElementEnum::freeCameraUserInputCameraSetVelocityFillCurrent:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Fill with the cameras current relative Velocity"), std::nullopt, "Fill with Current##freeCameraUserInputCameraSetVelocityCopyCurrent", settings->freeCameraUserInputCameraSetVelocityFillCurrent));

							case GUIElementEnum::freeCameraUserInputCameraSetVelocityCopy:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Copy a relative Velocity to the clipboard"), std::nullopt, "Copy to Clipboard##freeCameraUserInputCameraSetVelocityPaste", settings->freeCameraUserInputCameraSetVelocityCopy));


							case GUIElementEnum::freeCameraUserInputCameraSetVelocityPaste:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleButton<false>>
									(game, ToolTipCollection("Paste in a relative Velocity from the clipboard"), std::nullopt, "Paste from Clipboard##freeCameraUserInputCameraSetVelocityPaste", settings->freeCameraUserInputCameraSetVelocityPaste));

					case GUIElementEnum::freeCameraAnchorPositionToObjectPosition:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
							(game, ToolTipCollection("Anchors the camera position to an objects position, following it around"), RebindableHotkeyEnum::freeCameraAnchorPositionToObjectPosition, "Anchor Camera Position to Object Position", settings->freeCameraAnchorPositionToObjectPosition, headerChildElements
								{
								createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup),
								createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectPositionTranslationInterpolator),
								createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectRotation),
								}
							));

						case GUIElementEnum::freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraObjectTrackEnum, 100.f>>
								(game, ToolTipCollection(""), "Object to Anchor to##freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup", settings->freeCameraAnchorPositionToObjectPositionObjectToTrackComboGroup,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum)},
									}));


							case GUIElementEnum::freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
									(game, ToolTipCollection("The datum (unique ID) of the object the camera should anchor to"), "Custom object datum##freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum", settings->freeCameraAnchorPositionToObjectPositionObjectToTrackCustomObjectDatum));


						case GUIElementEnum::freeCameraAnchorPositionToObjectPositionTranslationInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "Position Interpolation##freeCameraAnchorPositionToObjectPositionTranslationInterpolator", settings->freeCameraAnchorPositionToObjectPositionTranslationInterpolator,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor) }
									}
							));

							case GUIElementEnum::freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
									(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor", settings->freeCameraAnchorPositionToObjectPositionTranslationInterpolatorLinearFactor));


						case GUIElementEnum::freeCameraAnchorPositionToObjectRotation:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<true>>
								(game, ToolTipCollection("Anchors the camera position to an objects rotation, staying on the same side of it as it turns around"), RebindableHotkeyEnum::freeCameraAnchorPositionToObjectRotation, "Anchor Camera Position to Object Rotation", settings->freeCameraAnchorPositionToObjectRotation
							));

						

					case GUIElementEnum::freeCameraAnchorRotationToObjectPosition:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
							(game, ToolTipCollection("Anchors the camera rotation to an objects position, panning to track it as it moves"), RebindableHotkeyEnum::freeCameraAnchorRotationToObjectPosition, "Anchor Camera Rotation to Object Position", settings->freeCameraAnchorRotationToObjectPosition, headerChildElements
								{
								createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup),
								createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPositionRotationInterpolator),
								}
						));

						case GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraObjectTrackEnumPlusAbsolute, 100.f>>
								(game, ToolTipCollection(""), "Object to Anchor to##freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup", settings->freeCameraAnchorRotationToObjectPositionObjectToTrackComboGroup,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum)},
										headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3)},
									}));


							case GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
									(game, ToolTipCollection("The datum (unique ID) of the object the camera should anchor to"), "Custom object datum##freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum", settings->freeCameraAnchorRotationToObjectPositionObjectToTrackCustomObjectDatum));


							case GUIElementEnum::freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, false, 8>>
									(game, ToolTipCollection("Absolute position to anchor to"), "Absolute Position ##freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3", settings->freeCameraAnchorRotationToObjectPositionObjectToTrackManualPositionVec3, "x", "y", "z"));



						case GUIElementEnum::freeCameraAnchorRotationToObjectPositionRotationInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "Rotation Interpolation##freeCameraAnchorRotationToObjectPositionRotationInterpolator", settings->freeCameraAnchorRotationToObjectPositionRotationInterpolator,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor) }
									}
							));

							case GUIElementEnum::freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
									(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor", settings->freeCameraAnchorRotationToObjectPositionRotationInterpolatorLinearFactor));


					case GUIElementEnum::freeCameraAnchorRotationToObjectFacing:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
							(game, ToolTipCollection("Anchors the camera rotation to an objects rotation, panning to track it as it moves"), RebindableHotkeyEnum::freeCameraAnchorRotationToObjectFacing, "Anchor Camera Rotation to Object Facing", settings->freeCameraAnchorRotationToObjectFacing, headerChildElements
								{
								createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup),
								createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectFacingRotationInterpolator),
								}
						));

						case GUIElementEnum::freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraObjectTrackEnum, 100.f>>
								(game, ToolTipCollection(""), "Object to Anchor to##freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup", settings->freeCameraAnchorRotationToObjectFacingObjectToTrackComboGroup,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum)},
									}));


							case GUIElementEnum::freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
									(game, ToolTipCollection("The datum (unique ID) of the object the camera should anchor to"), "Custom object datum##freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum", settings->freeCameraAnchorRotationToObjectFacingObjectToTrackCustomObjectDatum));


							case GUIElementEnum::freeCameraAnchorRotationToObjectFacingRotationInterpolator:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraInterpolationTypesEnum, 100.f>>
									(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "Rotation Interpolation##freeCameraAnchorRotationToObjectFacingRotationInterpolator", settings->freeCameraAnchorRotationToObjectFacingRotationInterpolator,
										vectorOfHeaderChildElements
										{
											headerChildElements{},
											headerChildElements{ createNestedElement(GUIElementEnum::freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor) }
										}
								));

								case GUIElementEnum::freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor:
									return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
										(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor", settings->freeCameraAnchorRotationToObjectFacingRotationInterpolatorLinearFactor));

					case GUIElementEnum::freeCameraAnchorFOVToObjectDistance:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, true>>
							(game, ToolTipCollection("Anchors the camera FOV to its distance to an object, zooming in as the object moves further away"), RebindableHotkeyEnum::freeCameraAnchorFOVToObjectDistance, "Anchor Camera FOV to Object Distance", settings->freeCameraAnchorFOVToObjectDistance, headerChildElements
								{
								createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup),
								createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistanceFOVInterpolator),
								}
						));

					case GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraObjectTrackEnumPlusAbsolute, 100.f>>
							(game, ToolTipCollection(""), "Object to Anchor to##freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup", settings->freeCameraAnchorFOVToObjectDistanceObjectToTrackComboGroup,
								vectorOfHeaderChildElements
								{
									headerChildElements{},
									headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum)},
									headerChildElements{createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3)},
								}));


					case GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
							(game, ToolTipCollection("The datum (unique ID) of the object the camera should anchor to"), "Custom object datum##freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum", settings->freeCameraAnchorFOVToObjectDistanceObjectToTrackCustomObjectDatum));


					case GUIElementEnum::freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIVec3<true, false, 8>>
							(game, ToolTipCollection("Absolute position to anchor to"), "Absolute Position: ##freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3", settings->freeCameraAnchorFOVToObjectDistanceObjectToTrackManualPositionVec3, "x", "y", "z"));



						case GUIElementEnum::freeCameraAnchorFOVToObjectDistanceFOVInterpolator:
							return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIComboEnumWithChildren<SettingsStateAndEvents::FreeCameraInterpolationTypesEnum, 100.f>>
								(game, ToolTipCollection("Function that controls how smoothly the camera adjusts to input"), "FOV Interpolation##freeCameraAnchorFOVToObjectDistanceFOVInterpolator", settings->freeCameraAnchorFOVToObjectDistanceFOVInterpolator,
									vectorOfHeaderChildElements
									{
										headerChildElements{},
										headerChildElements{ createNestedElement(GUIElementEnum::freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor) }
									}
							));

							case GUIElementEnum::freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor:
								return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIFloat<>>
									(game, ToolTipCollection("0 to 1 value controlling smoothness of the input. Low values make the camera sluggish, high values make it fast and snappy."), "Snap Factor##freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor", settings->freeCameraAnchorFOVToObjectDistanceFOVInterpolatorLinearFactor));
*/
				
				case GUIElementEnum::changeOOBBackgroundToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIToggleWithChildren<GUIToggleWithChildrenParameters::ShowWhenTrue, false>>
						(game, ToolTipCollection("Allows you to change the background colour of the void when out-of-bounds"), std::nullopt, "Override OOB Background Color", settings->changeOOBBackgroundToggle, 
							headerChildElements{
								createNestedElement(GUIElementEnum::changeOOBBackgroundColor),
							}));

					case GUIElementEnum::changeOOBBackgroundColor:
						return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIColourPicker<true>>
							(game, ToolTipCollection(""), "Background Colour##OOBBackgroundColor", settings->changeOOBBackgroundColor));


				case GUIElementEnum::disableFogToggle:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUISimpleToggle<false>>
						(game, ToolTipCollection(""), std::nullopt, "Disable Fog", settings->disableFogToggle));


			case GUIElementEnum::theaterHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("this is where theater stuff would go IF I HAD ANY"), "Camera", headerChildElements{ std::nullopt }));


			

			case GUIElementEnum::debugHeadingGUI:
				return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
					(game, ToolTipCollection("Debug tools"), "Debug", headerChildElements
						{ 
						createNestedElement(GUIElementEnum::getPlayerDatumGUI),
						createNestedElement(GUIElementEnum::getPlayerAddressGUI),
						createNestedElement(GUIElementEnum::getObjectAddressGUI),
						createNestedElement(GUIElementEnum::getTagAddressGUI),
						}));

				case GUIElementEnum::getPlayerDatumGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIButtonWithCopyableResult>
						(game, ToolTipCollection("Resolves the local player's object datum into the box, ready to copy (also copied to clipboard)."), "Get Player Datum", settings->getPlayerDatumEvent, settings->getPlayerDatumResult));

				case GUIElementEnum::getPlayerAddressGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIButtonWithCopyableResult>
						(game, ToolTipCollection("Resolves the local player's object (heap) address into the box, ready to copy (also copied to clipboard)."), "Get Player Address", settings->getPlayerAddressEvent, settings->getPlayerAddressResult));



				case GUIElementEnum::getObjectAddressGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
						(game, ToolTipCollection("Evaluates a main object datums address, copying it to the clipboard"), "Get Object Address: ", settings->getObjectAddressDWORD, settings->getObjectAddressEvent));

				case GUIElementEnum::getTagAddressGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIInputDWORD<true>>
						(game, ToolTipCollection("Finds the address of a tag by tag datum, copying it to the clipboard"), "Get Tag Address: ", settings->getTagAddressDWORD, settings->getTagAddressEvent));


#ifdef HCM_DEBUG

				case GUIElementEnum::HCMDebugHeadingGUI:
					return std::optional<std::shared_ptr<IGUIElement>>(std::make_shared<GUIHeading>
						(game, ToolTipCollection("Debug tools for HCM development"), "HCM Debug", headerChildElements
							{
						}));


#endif




			default:
				throw HCMInitException(std::format("You forgot to add a creation case label for GUIElement: {} in {}::{}", magic_enum::enum_name(guielementenum), nameof(GUIElementConstructor), nameof(createGUIElement)));
			}


		}
		catch (HCMInitException ex)
		{
			info->addFailure(guielementenum, game, ex);
			cacheFailedServices.insert(std::make_pair(guielementenum, game));
			return std::nullopt;
		}
		// ⚠ ANYTHING ELSE USED TO KILL THE WHOLE SESSION. Only HCMInitException was caught here, so a
		// std::exception (or anything else) thrown while building ONE gui row escaped all the way out of
		// App and took HCMInternal down - an unhandled 0xE06D7363, a minidump, and an injected module that
		// silently never finishes initialising. One optional row failing is not worth that: it is exactly
		// the case the HCMInitException path above already degrades gracefully for.
		//
		// The type name is logged because the old behaviour told you nothing at all about WHICH row or
		// WHAT threw - the crash landed in KERNELBASE with no HCM frame to read.
		catch (const std::exception& ex)
		{
			const auto msg = std::format("GUI element threw a non-HCM exception ({}): {}",
				typeid(ex).name(), ex.what());
			PLOG_ERROR << "GUIElementEnum::" << magic_enum::enum_name(guielementenum)
				<< " for Game::" << game.toString() << " - " << msg;
			info->addFailure(guielementenum, game, HCMInitException(msg));
			cacheFailedServices.insert(std::make_pair(guielementenum, game));
			return std::nullopt;
		}
		catch (...)
		{
			const auto msg = std::string("GUI element threw an exception of unknown type");
			PLOG_ERROR << "GUIElementEnum::" << magic_enum::enum_name(guielementenum)
				<< " for Game::" << game.toString() << " - " << msg;
			info->addFailure(guielementenum, game, HCMInitException(msg));
			cacheFailedServices.insert(std::make_pair(guielementenum, game));
			return std::nullopt;
		}
	}


	std::shared_ptr<GUIElementStore> mStore;

	MCCProcessType mProcType;

public:
	GUIElementConstructorImpl(std::shared_ptr<IGUIRequiredServices> guireq, std::shared_ptr<OptionalCheatInfo> fail, std::shared_ptr<GUIElementStore> store, std::shared_ptr<GUIServiceInfo> info, std::shared_ptr<SettingsStateAndEvents> settings, MCCProcessType procType, std::shared_ptr<RuntimeExceptionHandler> exp)
		: mStore(store), mProcType(procType)
	{
		// Create all top level GUI elements. Each one will recursively create it's nested elements, if it has any.
		for (auto& [element, game] : guireq->getToplevelGUIElements())
		{


			auto x = createGUIElementAndStoreResult(element, game, guireq, fail, info, settings, exp);
			if (x.has_value())
			{
				store->getTopLevelGUIElementsMutable().at(game).push_back(x.value());
			}
		}
	}
};






GUIElementConstructor::GUIElementConstructor(std::shared_ptr<IGUIRequiredServices> guireq, std::shared_ptr<OptionalCheatInfo> fail, std::shared_ptr<GUIElementStore> store, std::shared_ptr<GUIServiceInfo> info, std::shared_ptr<SettingsStateAndEvents> settings, MCCProcessType procType, std::shared_ptr<RuntimeExceptionHandler> exp) : pimpl(std::make_unique<GUIElementConstructorImpl>(guireq, fail, store, info, settings, procType, exp)) {}
GUIElementConstructor::~GUIElementConstructor() = default;
