#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "HCEConsole.h"

// A one-line HaloScript entry box with live autocomplete, for Halo Campaign Evolved. See HCEConsole.h for the
// entry point and why commands are queued rather than executed inline.
//
// Deliberately modest: a text field, a suggestion list under it, and a short history. The MCC console's
// larger window is welded to GameEngineFunctions, which HaloCER has no equivalent of, and refactoring shared
// code used by six other titles to share a widget is not worth the regression risk.
class GUIHCEConsole : public IGUIElement
{
private:
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;

	char mInput[512]{};
	std::vector<std::string> mHistory;      // most recent first
	std::string mStatus;                    // result of the last submit
	bool mStatusIsError = false;
	int mHighlighted = -1;                  // index into the current suggestion list

	static constexpr size_t kMaxSuggestions = 8;
	static constexpr size_t kMaxHistory = 12;

	void submit()
	{
		std::string command(mInput);
		while (!command.empty() && (command.back() == ' ' || command.back() == '\t')) command.pop_back();
		if (command.empty()) return;

		std::string why;
		if (HCEConsoleBridge::queue(command, why))
		{
			mStatus = "queued: " + command;
			mStatusIsError = false;
			mHistory.insert(mHistory.begin(), command);
			if (mHistory.size() > kMaxHistory) mHistory.pop_back();
			mInput[0] = '\0';
		}
		else
		{
			mStatus = why;
			mStatusIsError = true;
		}
		mHighlighted = -1;
	}

public:
	GUIHCEConsole(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey,
		std::shared_ptr<SettingsStateAndEvents> settings)
		: IGUIElement(implGame, hotkey, tooltip), mSettingsWeak(settings)
	{
		PLOG_VERBOSE << "Constructing GUIHCEConsole";
		this->currentHeight = GUIFrameHeightWithSpacing * 3.f;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		hotkeyRenderer.renderHotkey(mHotkey);

		float used = GUIFrameHeightWithSpacing;

		if (!HCEConsoleBridge::isUsable())
		{
			ImGui::TextDisabled("Console unavailable on this build of Halo Campaign Evolved");
			renderTooltip();
			currentHeight = used;
			return;
		}

		ImGui::SetNextItemWidth(320.f);
		const bool entered = ImGui::InputText("HaloScript##hceConsole", mInput, sizeof(mInput),
			ImGuiInputTextFlags_EnterReturnsTrue);
		renderTooltip();
		if (entered) submit();

		ImGui::SameLine();
		if (ImGui::Button("Run##hceConsole")) submit();

		// ---- suggestions ----------------------------------------------------------------------------
		// Only the leading token is completed: the rest of the line is arguments, and the game's own
		// preprocessor decides whether the line becomes (set x ...) or (x ...) based on that first token.
		std::string_view line(mInput);
		size_t tokenStart = 0;
		while (tokenStart < line.size() && (line[tokenStart] == '(' || line[tokenStart] == ' ')) ++tokenStart;
		size_t tokenEnd = line.find_first_of(" \t", tokenStart);
		const bool completingFirstToken = (tokenEnd == std::string_view::npos);
		std::string_view prefix = completingFirstToken ? line.substr(tokenStart) : std::string_view{};

		if (!prefix.empty())
		{
			auto hits = HCEConsoleBridge::complete(prefix, kMaxSuggestions);
			for (size_t i = 0; i < hits.size(); ++i)
			{
				const auto* e = hits[i];
				// A stubbed command is still listed - the game lists it too - but it is dimmed, so the one
				// that will actually do something is the one that stands out.
				if (e->isLive) ImGui::TextUnformatted(e->signature.c_str());
				else ImGui::TextDisabled("%s", e->signature.c_str());

				if (ImGui::IsItemClicked())
				{
					const std::string chosen = e->name + (e->argc > 0 || e->isGlobal ? " " : "");
					std::snprintf(mInput, sizeof(mInput), "%s", chosen.c_str());
				}
				used += GUIFrameHeightWithSpacing * 0.8f;
			}
		}

		if (!mStatus.empty())
		{
			if (mStatusIsError) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%s", mStatus.c_str());
			else ImGui::TextDisabled("%s", mStatus.c_str());
			used += GUIFrameHeightWithSpacing * 0.8f;
		}

		used += GUIFrameHeightWithSpacing;
		currentHeight = used;
		DEBUG_GUI_HEIGHT;
	}

	~GUIHCEConsole() = default;
	std::string_view getName() override { return nameof(GUIHCEConsole); }
};
