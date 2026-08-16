#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "GUIRadioButton.h"

class GUIRadioGroup : public IGUIElement
{
private:
	std::vector<std::pair<int, std::shared_ptr<GUIRadioButton >>> mRadioButtons;
	std::string mDebugName;

	bool hasRenderedYet = false;
	int selectedRadioOption = 0;

	// True if ANY option in this group is bindable to a hotkey. Everything hotkey-related below is gated on it,
	// so a group without hotkeys - which is every MCC one - renders and behaves exactly as it always has. Note
	// the two are not interchangeable: renderHotkey() draws blank padding rather than nothing when handed no
	// hotkey, so calling it unconditionally would indent every radio button in the program.
	bool mAnyOptionHasHotkey = false;

public:

	GUIRadioGroup(GameState implGame, ToolTipCollection tooltip, std::string debugName, std::vector<std::optional<std::shared_ptr<IGUIElement>>> radioButtons)
		: IGUIElement(implGame, std::nullopt, tooltip), mDebugName(debugName)
	{
		int radioButtonCount = 0;
		for (auto maybeRadioButton : radioButtons)
		{
			if (maybeRadioButton.has_value())
			{
				mRadioButtons.push_back(std::make_pair(radioButtonCount, std::dynamic_pointer_cast<GUIRadioButton>(maybeRadioButton.value())));
				if (maybeRadioButton.value()->getHotkey().has_value()) mAnyOptionHasHotkey = true;
				radioButtonCount++;
			}

		}

		currentHeight = radioButtonCount * 20;
	}

	std::string_view getName() override { return mDebugName; }

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		

		if (mRadioButtons.empty())
		{
			currentHeight = 0;
			return;
		}

		// first run safety checks to be sure that one and only one option is selected
		if (!hasRenderedYet)
		{
			PLOG_DEBUG << "RadioButtonGroup setting up, total button count: " << mRadioButtons.size();

			hasRenderedYet = true;
			int countEnabledOptions = 0;
			for (auto& [radioIndex, radioBut] : mRadioButtons)
			{
				if (radioBut->mBoundOption.lock()->GetValue()) countEnabledOptions++;
			}

			if (countEnabledOptions == 0) // bad!
			{
				PLOG_ERROR << getName() << ": countEnabledOptions was zero!";
				// need to force at least one to be enabled
				mRadioButtons.at(0).second->mBoundOption.lock()->GetValueDisplay() = true;
				mRadioButtons.at(0).second->mBoundOption.lock()->UpdateValueWithInput();
			}

			if (countEnabledOptions > 1) // bad!
			{
				PLOG_ERROR << getName() << ": countEnabledOptions was over 1!";
				// need to disable all except one
				for (auto& [radioIndex, radioBut] : mRadioButtons)
				{
					radioBut->mBoundOption.lock()->GetValueDisplay() = false;
					radioBut->mBoundOption.lock()->UpdateValueWithInput();
				}
				// enable the first one
				mRadioButtons.at(0).second->mBoundOption.lock()->GetValueDisplay() = true;
				mRadioButtons.at(0).second->mBoundOption.lock()->UpdateValueWithInput();
			}

			if (countEnabledOptions == 1) // good!
			{
				PLOG_VERBOSE << getName() << ": phew, countEnabledOptions was 1";
				// need to set selectedRadioOption to the correct value
				for (int i = 0; i < mRadioButtons.size(); i++)
				{
					if (mRadioButtons.at(i).second->mBoundOption.lock()->GetValueDisplay())
					{
						selectedRadioOption = i;
						break;
					}
				}
			}
		}

		// A hotkey changes the bound settings from outside this group, so when one exists the selection has to be
		// re-derived from them each frame - selectedRadioOption is otherwise only ever moved by a click here, and
		// the group would keep displaying (and keep rendering the children of) the option the user just left.
		if (mAnyOptionHasHotkey)
		{
			for (int i = 0; i < mRadioButtons.size(); i++)
			{
				auto boundOption = mRadioButtons.at(i).second->mBoundOption.lock();
				if (boundOption && boundOption->GetValueDisplay())
				{
					selectedRadioOption = i;
					break;
				}
			}
		}

		bool debugAlreadyRenderedSelected = false;
		ImGui::BeginChild(std::format("##{}", mDebugName).c_str(), {GUIWindowWidth - 30, std::max(0.f, currentHeight - GUISpacing)});
		currentHeight = GUISpacing;
		// render each element
		// somethings is going wrong with the currentHeight here
		// actually more importantly, some big blank space is getting inserted after the first radio button for some reason
		for (auto& [radioIndex, radioBut] : mRadioButtons)
		{
			currentHeight += GUIFrameHeightWithSpacing;
			if (mAnyOptionHasHotkey)
			{
				hotkeyRenderer.renderHotkey(radioBut->getHotkey());
				ImGui::SameLine();
			}
			if (ImGui::RadioButton(radioBut->getName().data(), &selectedRadioOption, radioIndex))
			{
				for (auto& [otherRadioIndex, otherRadioBut] : mRadioButtons)
				{
					// turn off all, except this one we turn on
					if (radioIndex == otherRadioIndex)
					{
						otherRadioBut->mBoundOption.lock()->GetValueDisplay() = true;
						otherRadioBut->mBoundOption.lock()->UpdateValueWithInput();
					}
					else
					{
						otherRadioBut->mBoundOption.lock()->GetValueDisplay() = false;
						otherRadioBut->mBoundOption.lock()->UpdateValueWithInput();
					}
				}
			}
			radioBut->renderTooltip();
			DEBUG_GUI_HEIGHT;

			// if this button is the currently active one, render its children
			if (radioIndex == selectedRadioOption)
			{
				if (debugAlreadyRenderedSelected)
				{
					LOG_ONCE(PLOG_ERROR << "already rendered a selected option this frame!");
				}
				else
				{
					debugAlreadyRenderedSelected = true;
					radioBut->render(hotkeyRenderer);
					currentHeight += radioBut->getCurrentHeight();
				}

			}
		}
		ImGui::EndChild();
	}
};