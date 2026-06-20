#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"

// A float input with +/- step buttons (ImGui::InputFloat step). The +/- buttons add/subtract `mStep`,
// and clicking (or ctrl-clicking) the field lets you type a value manually - same idiom used elsewhere.
class GUIFloatStepper : public IGUIElement {

private:
	std::string mLabelText;
	float mStep;
	float mStepFast;
	std::string mFormat;
	std::weak_ptr<BinarySetting<float>> mOptionFloatWeak;
	std::vector<std::thread> mUpdateSettingThreads;

public:

	GUIFloatStepper(GameState implGame, ToolTipCollection tooltip, std::string labelText, std::shared_ptr<BinarySetting<float>> optionFloat, float step = 512.f, float stepFast = 2048.f, std::string format = "%.0f")
		: IGUIElement(implGame, std::nullopt, tooltip), mLabelText(labelText), mStep(step), mStepFast(stepFast), mFormat(format), mOptionFloatWeak(optionFloat)
	{
		if (mLabelText.empty()) throw HCMInitException("Cannot have empty label (needs label for imgui ID system, use ## for invisible labels)");
		PLOG_VERBOSE << "Constructing GUIFloatStepper, name: " << getName();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto mOptionFloat = mOptionFloatWeak.lock();
		if (!mOptionFloat)
		{
			PLOG_ERROR << "bad mOptionFloat weakptr when rendering " << getName();
			return;
		}

		ImGui::SetNextItemWidth(160);
		bool updateRequired = ImGui::InputFloat(mLabelText.c_str(), &mOptionFloat->GetValueDisplay(), mStep, mStepFast, mFormat.c_str());

		if (updateRequired)
		{
			PLOG_VERBOSE << "GUIFloatStepper (" << getName() << ") firing value event, new value: " << mOptionFloat->GetValueDisplay();
			auto& newThread = mUpdateSettingThreads.emplace_back(std::thread([optionToggle = mOptionFloat]() { optionToggle->UpdateValueWithInput(); }));
			newThread.detach();
		}

		renderTooltip();
		DEBUG_GUI_HEIGHT;
	}

	~GUIFloatStepper()
	{
		for (auto& thread : mUpdateSettingThreads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}
	}

	std::string_view getName() override { return mLabelText; }
};
