#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "HCESkullEnum.h"

// ================================================================================================================
// Halo Campaign Evolved skull widget. Structurally GUISkullToggle, with two differences:
//   - checkbox state comes from settings->hceSkullEngineState, which HCESkullToggler refreshes straight out of
//     game memory when hceSkullUpdateEvent fires. Like GUISkullToggle, HCM keeps no shadow copy of skull state -
//     what you see is what the engine currently has, which is what a checkpoint revert leaves behind.
//   - clicking fires hceSkullSetEvent(bit, value). All memory access lives in the cheat, none in the widget.
//
// The hotkey column works exactly as GUISkullToggle's does; the enum comes from kHCESkullHotkeys[bitIndex] rather
// than a per-row member. Every one of them is unbound out of the box - 56 default bindings would collide with half
// the keyboard - so the column renders as a bare "..." until the user assigns something.
//
// Displayed alphabetically but indexed by bit ordinal, exactly like create_skull_actions' sorted(enumerate(...)).
// ================================================================================================================
template<bool startsOpen>
class GUIHCESkullToggle : public IGUIElement
{
private:
	std::string mHeadingText;
	bool headingOpen = false;
	float mLeftMargin;
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;

	// bit indices, sorted by display name. Built once.
	std::vector<int> mDisplayOrder;

public:
	GUIHCESkullToggle(GameState implGame, ToolTipCollection tooltip, std::string headingText, std::shared_ptr<SettingsStateAndEvents> settings, float leftMargin = 20.f)
		: IGUIElement(implGame, std::nullopt, tooltip), mLeftMargin(leftMargin), mHeadingText(headingText), mSettingsWeak(settings)
	{
		PLOG_VERBOSE << "Constructing GUIHCESkullToggle, name: " << getName();

		// Alphabetical display, ordinal indexing - exactly what create_skull_actions' sorted(enumerate(...),
		// key=casefold) does. The bit index travels with the entry, it is never the row number.
		// Only the skulls the shipped game actually offers. The other 14 bits stay in kHCESkulls so the array index
		// remains the bit index, but showing a checkbox for something the game never exposes is just a lie.
		mDisplayOrder.reserve(kHCESkullCount);
		for (int i = 0; i < (int)kHCESkullCount; i++) if (kHCESkulls[i].inMenu) mDisplayOrder.push_back(i);
		std::sort(mDisplayOrder.begin(), mDisplayOrder.end(), [](int a, int b)
			{
				const char* l = kHCESkulls[a].displayName;
				const char* r = kHCESkulls[b].displayName;
				for (; *l && *r; ++l, ++r)
				{
					const char lc = (char)std::tolower((unsigned char)*l);
					const char rc = (char)std::tolower((unsigned char)*r);
					if (lc != rc) return lc < rc;
				}
				return *r != '\0';
			});

		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto mSettings = mSettingsWeak.lock();
		if (!mSettings) { PLOG_ERROR << "bad mSettings weak ptr"; return; }

		ImGui::BeginChild(mHeadingText.c_str(), { GUIWindowWidth - mLeftMargin, currentHeight - GUISpacing });

		constexpr ImGuiTreeNodeFlags treeFlags = startsOpen ? ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_FramePadding;
		headingOpen = ImGui::TreeNodeEx(mHeadingText.c_str(), treeFlags);
		renderTooltip();
		DEBUG_GUI_HEIGHT;

		if (headingOpen)
		{
			currentHeight = GUIFrameHeightWithSpacing * 2;

			// Ask the cheat to refresh hceSkullEngineState from game memory before we read it.
			mSettings->hceSkullUpdateEvent->operator()(mImplGame);
			ImGui::Text("Some skulls do not work. State is reset by checkpoints; HCM re-applies.");

			if (!mSettings->hceSkullStateValid)
			{
				currentHeight += GUIFrameHeightWithSpacing;
				ImGui::Text("Waiting for game...");
			}
			else
			{
				currentHeight += GUIFrameHeightWithSpacing; // table padding
				currentHeight += GUIFrameHeightWithSpacing * std::ceil(mDisplayOrder.size() / 2.f);

				ImGui::BeginTable("hceSkullTable", 2, ImGuiTableFlags_RowBg);

				for (int bitIndex : mDisplayOrder)
				{
					ImGui::TableNextColumn();

					const auto& info = kHCESkulls[bitIndex];

					hotkeyRenderer.renderHotkey(kHCESkullHotkeys[bitIndex], 30);
					ImGui::SameLine();

					bool tempValue = mSettings->hceSkullEngineState[bitIndex];
					if (ImGui::Checkbox(info.displayName, &tempValue))
					{
						mSettings->hceSkullEngineState[bitIndex] = tempValue; // optimistic; the next refresh corrects it
						mSettings->hceSkullSetEvent->operator()(bitIndex, tempValue);
					}

					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						ImGui::SetTooltip(info.toolTip);
					}
				}
				ImGui::EndTable();
			}

			ImGui::TreePop();
		}
		else
		{
			currentHeight = GUIFrameHeightWithSpacing;
		}
		ImGui::EndChild();
	}

	std::string_view getName() override { return mHeadingText; }
};
