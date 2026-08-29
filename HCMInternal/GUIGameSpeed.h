#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"

// Checkbox plus a multiplier box on one line, mirroring GUISpeedhack so the two read the same way - but this
// one drives HCEGameSpeed, which writes the engine's own s_game_time_globals.speed instead of hooking the
// process clock. See HCEGameSpeed.h.
//
// ⚠ DELIBERATELY NO WORKER THREAD. GUISpeedhack (and most of the GUI widgets) push every edit onto a detached
// std::thread that is emplaced into a vector nothing ever erases, because their setting handlers can do real
// work. HCEGameSpeed's handler stores one float into an atomic - the game thread picks it up on its next tick -
// so calling it inline is both correct and cheaper than creating a thread to do it.
class GUIGameSpeed : public IGUIElement {

private:
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;

public:

	GUIGameSpeed(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey, std::shared_ptr<SettingsStateAndEvents> settings)
		: IGUIElement(implGame, hotkey, tooltip), mSettingsWeak(settings)
	{
		PLOG_VERBOSE << "Constructing GUIGameSpeed, name: " << getName();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto mSettings = mSettingsWeak.lock();
		if (!mSettings)
		{
			PLOG_ERROR << "bad mSettings weakptr when rendering " << getName();
			return;
		}

		hotkeyRenderer.renderHotkey(mHotkey);

		if (ImGui::Checkbox("Game Speed", &mSettings->hceGameSpeedToggle->GetValueDisplay()))
		{
			PLOG_VERBOSE << "GUIGameSpeed being toggled to " << mSettings->hceGameSpeedToggle->GetValueDisplay();
			mSettings->hceGameSpeedToggle->UpdateValueWithInput();
		}
		renderTooltip();

		ImGui::SameLine();
		ImGui::SetNextItemWidth(100);
		if (ImGui::InputDouble("##settingForHCEGameSpeed", &mSettings->hceGameSpeedSetting->GetValueDisplay()))
		{
			PLOG_VERBOSE << "GUIGameSpeed multiplier being updated to " << mSettings->hceGameSpeedSetting->GetValueDisplay();
			mSettings->hceGameSpeedSetting->UpdateValueWithInput();
		}
		renderTooltip();
		DEBUG_GUI_HEIGHT;
	}

	~GUIGameSpeed() = default;

	std::string_view getName() override { return nameof(GUIGameSpeed); }
};
