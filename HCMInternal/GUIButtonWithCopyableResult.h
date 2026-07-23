#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"

// A button that fires an ActionEvent, followed by a read-only (still selectable + Ctrl+C copyable) text box that
// displays a result string the event's handler writes back. Used by the Debug "Get Player Datum" / "Get Player
// Address" rows: click -> resolve -> the value lands in the box for the user to select & copy - a persistent,
// practical alternative to a transient on-screen message.
//
// The event is fired SYNCHRONOUSLY on the render thread (unlike GUISimpleButton's detached thread) so the handler's
// write to the result string and this box's read of it cannot race - the resolution is just a few fast memory reads.
class GUIButtonWithCopyableResult : public IGUIElement
{
private:
	std::string mButtonText;
	std::weak_ptr<ActionEvent> mEventToFireWeak;
	std::weak_ptr<BinarySetting<std::string>> mResultWeak;

public:

	GUIButtonWithCopyableResult(GameState implGame, ToolTipCollection tooltip, std::string buttonText,
		std::shared_ptr<ActionEvent> eventToFire, std::shared_ptr<BinarySetting<std::string>> result)
		: IGUIElement(implGame, std::nullopt, tooltip), mButtonText(buttonText), mEventToFireWeak(eventToFire), mResultWeak(result)
	{
		if (mButtonText.empty()) throw HCMInitException("Cannot have empty button text (needs label for imgui ID system, use ## for invisible labels)");
		PLOG_VERBOSE << "Constructing GUIButtonWithCopyableResult, name: " << getName();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto ev = mEventToFireWeak.lock();
		auto result = mResultWeak.lock();
		if (!ev || !result)
		{
			PLOG_ERROR << "bad weakptr when rendering " << getName();
			return;
		}

		if (ImGui::Button(mButtonText.c_str()))
			ev->operator()(); // synchronous: the handler resolves the value and writes it into `result` on this thread
		renderTooltip();

		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		// read-only so a stray edit can't clobber it, but the user can still highlight the text and Ctrl+C it
		ImGui::InputText(std::format("##{}_result", mButtonText).c_str(), &result->GetValueDisplay(), ImGuiInputTextFlags_ReadOnly);
		DEBUG_GUI_HEIGHT;
	}

	~GUIButtonWithCopyableResult() {}

	std::string_view getName() override { return mButtonText; }

};
