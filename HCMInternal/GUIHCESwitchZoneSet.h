#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "HCESwitchZoneSet.h"

// A dropdown of the CURRENT scenario's zone sets, plus the button that switches to the selected one.
//
// Why a hand-written element rather than GUIComboEnum: the list is not an enum. It is read out of the loaded
// scenario tag and differs per level (a50 has 7, c20 has 10), so there is nothing to template over. The names
// and the selection both live in HCEZoneSetBridge because the button's event fires on a DETACHED THREAD with
// no access to this object.
//
// ⚠ ONLY SWITCHABLE ZONE SETS ARE LISTED. Every level ships one internal zone set with no name in the tag,
// and HaloScript addresses zone sets BY NAME, so it can never be switched to - listing it only offers a dead
// choice. Because the list is filtered, the combo's row order is NOT the engine's index order: every entry
// carries its real engine index, and that is what the selection stores.
class GUIHCESwitchZoneSet : public IGUIElement
{
private:
	std::string mLabel;
	std::weak_ptr<ActionEvent> mEventToFireWeak;
	std::vector<std::thread> mFireEventThreads;

	struct Entry { int engineIndex; std::string name; };
	std::vector<Entry> mEntries;          // switchable only
	size_t mRawCount = 0;                 // how many the scenario actually declares, for the hidden-count note
	int mLastCurrent = -2;
	double mLastRefresh = 0.0;

	// Synthesised placeholders are the ones the reader could not name out of the tag; they are bracketed so
	// they can never collide with a real zone set name, which the engine spells set_floor_1 / set_landing etc.
	static bool switchable(const std::string& name) { return !name.empty() && name.front() != '('; }

	void refreshEntries()
	{
		const std::vector<std::string> names = HCEZoneSetBridge::names();
		mRawCount = names.size();
		mEntries.clear();
		for (int i = 0; i < (int)names.size(); ++i)
			if (switchable(names[(size_t)i])) mEntries.push_back({ i, names[(size_t)i] });
	}

	int rowForEngineIndex(int engineIndex) const
	{
		for (int r = 0; r < (int)mEntries.size(); ++r)
			if (mEntries[(size_t)r].engineIndex == engineIndex) return r;
		return -1;
	}

public:
	GUIHCESwitchZoneSet(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey,
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

		// Re-read at most twice a second. The scenario only changes on a level load, and the bridge itself
		// short-circuits when the scenario pointer has not moved.
		const double now = ImGui::GetTime();
		if (now - mLastRefresh > 0.5 || mEntries.empty())
		{
			mLastRefresh = now;
			refreshEntries();
		}

		if (mEntries.empty())
		{
			// Say WHY rather than render an empty combo that looks broken. Distinguish "nothing readable"
			// from "readable, but none of them can be named" - different faults, different fixes.
			ImGui::TextDisabled(!HCEZoneSetBridge::isUsable() ? "Zone sets: unavailable on this build"
				: mRawCount == 0 ? "Zone sets: none readable (is a level loaded?)"
				: "Zone sets: none of this level's zone sets are named in the tag");
			renderTooltip();
			DEBUG_GUI_HEIGHT;
			return;
		}

		// The selection is stored as an ENGINE index; map it onto a row, and repair it if it no longer exists
		// (level change, or it pointed at something now filtered out).
		int row = rowForEngineIndex(HCEZoneSetBridge::selection());
		if (row < 0) { row = 0; HCEZoneSetBridge::setSelection(mEntries[0].engineIndex); }

		// Follow the engine when IT changes zone set, but only once per change, so the user's own pick is not
		// yanked away while they are looking at the list. Skipped when the engine is on an unlisted set.
		const int current = HCEZoneSetBridge::currentIndex();
		if (current >= 0 && current != mLastCurrent)
		{
			mLastCurrent = current;
			const int currentRow = rowForEngineIndex(current);
			if (currentRow >= 0) { row = currentRow; HCEZoneSetBridge::setSelection(current); }
		}

		ImGui::SetNextItemWidth(220.f);
		if (ImGui::BeginCombo(std::format("##zonesetcombo{}", mLabel).c_str(), mEntries[(size_t)row].name.c_str()))
		{
			for (int r = 0; r < (int)mEntries.size(); ++r)
			{
				const bool isSelected = (r == row);
				// Mark the one the game is actually on, so the list is readable at a glance.
				const std::string entry = (mEntries[(size_t)r].engineIndex == current)
					? std::format("{}  <- current", mEntries[(size_t)r].name)
					: mEntries[(size_t)r].name;
				if (ImGui::Selectable(std::format("{}##zs{}", entry, r).c_str(), isSelected))
					HCEZoneSetBridge::setSelection(mEntries[(size_t)r].engineIndex);
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

	std::string_view getName() override { return nameof(GUIHCESwitchZoneSet); }

	~GUIHCESwitchZoneSet()
	{
		for (auto& thread : mFireEventThreads)
			if (thread.joinable()) thread.join();
	}
};
