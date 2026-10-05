#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "MCCZoneSets.h"

// Halo 3 / ODST / Reach / Halo 4: a dropdown of the CURRENT scenario's zone sets plus the button that switches to the
// selected one (MCCSwitchZoneSet). Same shape as GUIH5SwitchZoneSet - the list is read from the loaded scenario tag, so
// there is nothing to template over, and the selection lives in MCCZoneSetBridge because the button's event fires on a
// DETACHED THREAD. ⚠ Unlike Halo 5 the bridge is KEYED BY GAME: MCC keeps all four game DLLs loaded and HCM builds one
// element and one switch service per game.
// Switches BY INDEX, so every zone set is reachable - unnamed ones are listed as "zone set N".
class GUIMCCSwitchZoneSet : public IGUIElement
{
private:
	std::string mLabel;
	std::weak_ptr<ActionEvent> mEventToFireWeak;
	std::vector<std::thread> mFireEventThreads;

	std::vector<std::string> mEntries;   // index-aligned with the engine's zone set indices
	int mLastCurrent = -2;
	double mLastRefresh = 0.0;

public:
	GUIMCCSwitchZoneSet(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey,
		std::string label, std::shared_ptr<ActionEvent> eventToFire)
		: IGUIElement(implGame, hotkey, tooltip), mLabel(label), mEventToFireWeak(eventToFire)
	{
		if (mLabel.empty()) throw HCMInitException("Cannot have empty label (imgui ID system)");
		this->currentHeight = GUIFrameHeightWithSpacing * 2.f;   // combo + button
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto mEventToFire = mEventToFireWeak.lock();
		if (!mEventToFire)
		{
			PLOG_ERROR << "bad mEventToFire weakptr when rendering " << getName();
			return;
		}

		// Re-read at most twice a second. MCCZoneSets caches per scenario, so this is cheap anyway.
		const double now = ImGui::GetTime();
		if (now - mLastRefresh > 0.5 || mEntries.empty())
		{
			mLastRefresh = now;
			mEntries = MCCZoneSetBridge::names(mImplGame);
		}

		if (mEntries.empty())
		{
			ImGui::TextDisabled(!MCCZoneSetBridge::isUsable(mImplGame)
				? "Zone sets: unavailable"
				: "Zone sets: none readable (is a level loaded?)");
			renderTooltip();
			DEBUG_GUI_HEIGHT;
			return;
		}

		// Repair a selection left over from a level with more zone sets than this one.
		int row = MCCZoneSetBridge::selection(mImplGame);
		if (row < 0 || (size_t)row >= mEntries.size()) { row = 0; MCCZoneSetBridge::setSelection(mImplGame, 0); }

		// Follow the engine when IT changes zone set (once per change, so the user's own pick is not yanked away).
		const int current = MCCZoneSetBridge::currentIndex(mImplGame);
		if (current >= 0 && current != mLastCurrent)
		{
			mLastCurrent = current;
			if ((size_t)current < mEntries.size()) { row = current; MCCZoneSetBridge::setSelection(mImplGame, current); }
		}

		ImGui::SetNextItemWidth(220.f);
		if (ImGui::BeginCombo(std::format("##mcczonesetcombo{}", mLabel).c_str(), mEntries[(size_t)row].c_str()))
		{
			for (int r = 0; r < (int)mEntries.size(); ++r)
			{
				const bool isSelected = (r == row);
				const std::string entry = (r == current)
					? std::format("{}: {}  <- current", r, mEntries[(size_t)r])
					: std::format("{}: {}", r, mEntries[(size_t)r]);
				if (ImGui::Selectable(std::format("{}##mcczs{}", entry, r).c_str(), isSelected))
					MCCZoneSetBridge::setSelection(mImplGame, r);
				if (isSelected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (mHotkey.has_value())
		{
			hotkeyRenderer.renderHotkey(mHotkey);
			ImGui::SameLine();
		}

		if (ImGui::Button(mLabel.c_str()))
		{
			auto& newThread = mFireEventThreads.emplace_back(std::thread([mEvent = mEventToFire]() { mEvent->operator()(); }));
			newThread.detach();
		}
		renderTooltip();
		DEBUG_GUI_HEIGHT;
	}

	std::string_view getName() override { return nameof(GUIMCCSwitchZoneSet); }

	~GUIMCCSwitchZoneSet()
	{
		for (auto& thread : mFireEventThreads)
			if (thread.joinable()) thread.join();
	}
};
