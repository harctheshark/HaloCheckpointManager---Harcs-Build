#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "H5SwitchZoneSet.h"

// A dropdown of the CURRENT scenario's zone sets, plus the button that switches to the selected one.
//
// Why a hand-written element rather than GUIComboEnum: the list is not an enum. It is read out of the loaded
// scenario tag and differs per map, so there is nothing to template over. The names and the selection both
// live in H5ZoneSetBridge because the button's event fires on a DETACHED THREAD with no access to this object.
//
// ⚠ DELIBERATELY NOT SHARED WITH GUIHCESwitchZoneSet, despite the obvious resemblance. The two differ where it
// matters: HaloCER switches BY NAME through HaloScript, so it must filter out zone sets the tag does not name
// and its row order is therefore not the engine's index order. Halo 5 switches BY INDEX, so every zone set is
// reachable, nothing is filtered, and row == engine index. Folding them together would mean reintroducing the
// filtering machinery here to then disable it.
class GUIH5SwitchZoneSet : public IGUIElement
{
private:
	std::string mLabel;
	std::weak_ptr<ActionEvent> mEventToFireWeak;
	std::vector<std::thread> mFireEventThreads;

	std::vector<std::string> mEntries;   // index-aligned with the engine's own zone set indices
	int mLastCurrent = -2;
	double mLastRefresh = 0.0;

public:
	GUIH5SwitchZoneSet(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey,
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

		// Re-read at most twice a second. The table only changes on a level load.
		const double now = ImGui::GetTime();
		if (now - mLastRefresh > 0.5 || mEntries.empty())
		{
			mLastRefresh = now;
			mEntries = H5ZoneSetBridge::names();
		}

		if (mEntries.empty())
		{
			// Say WHY rather than render an empty combo that looks broken.
			ImGui::TextDisabled(!H5ZoneSetBridge::isUsable()
				? "Zone sets: unavailable"
				: "Zone sets: none readable (is a level loaded?)");
			renderTooltip();
			DEBUG_GUI_HEIGHT;
			return;
		}

		// Repair a selection left over from a map with more zone sets than this one.
		int row = H5ZoneSetBridge::selection();
		if (row < 0 || (size_t)row >= mEntries.size()) { row = 0; H5ZoneSetBridge::setSelection(0); }

		// Follow the engine when IT changes zone set, but only once per change, so the user's own pick is not
		// yanked away while they are looking at the list.
		const int current = H5ZoneSetBridge::currentIndex();
		if (current >= 0 && current != mLastCurrent)
		{
			mLastCurrent = current;
			if ((size_t)current < mEntries.size()) { row = current; H5ZoneSetBridge::setSelection(current); }
		}

		ImGui::SetNextItemWidth(220.f);
		if (ImGui::BeginCombo(std::format("##h5zonesetcombo{}", mLabel).c_str(), mEntries[(size_t)row].c_str()))
		{
			for (int r = 0; r < (int)mEntries.size(); ++r)
			{
				const bool isSelected = (r == row);
				// Mark the one the game is actually on, so the list is readable at a glance.
				const std::string entry = (r == current)
					? std::format("{}  <- current", mEntries[(size_t)r])
					: mEntries[(size_t)r];
				if (ImGui::Selectable(std::format("{}##h5zs{}", entry, r).c_str(), isSelected))
					H5ZoneSetBridge::setSelection(r);
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

	std::string_view getName() override { return nameof(GUIH5SwitchZoneSet); }

	~GUIH5SwitchZoneSet()
	{
		for (auto& thread : mFireEventThreads)
			if (thread.joinable()) thread.join();
	}
};
