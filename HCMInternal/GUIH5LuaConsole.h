#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include "H5LuaConsole.h"
#include <cctype>
#include <cstdio>

// A Lua entry box for Halo 5: Forge. See H5LuaConsole.h for the entry point and why chunks are queued rather
// than executed inline.
//
// Shaped like GUIHCEConsole rather than like the MCC command console, for the reason that one already gives:
// the MCC console's larger window is welded to GameEngineFunctions, and refactoring shared code used by six
// other titles to share a widget is not worth the regression risk. Halo 5 is in the same position HaloCER is.
//
// Autocomplete comes from the GAME'S OWN GLOBALS TABLE, not from a scan of the executable. An earlier version
// of this comment said there was no honest corpus to complete against, because three separate static registrar
// scans each missed real bindings. That was the wrong conclusion: `pairs(_G)` enumerates 3093 globals, so the
// console asks Lua what it can call and gets the exact answer for whatever build is running.
//
// Keys: Enter runs, Up/Down walk history, Tab or Right-arrow-at-end accepts the top suggestion.
class GUIH5LuaConsole : public IGUIElement
{
private:
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;

	char mInput[1024]{};
	std::vector<std::string> mHistory;      // most recent first
	int mHistoryPos = -1;                   // -1 = editing a fresh line
	std::string mStashed;                   // the line being edited, kept while walking history
	std::string mStatus;
	bool mStatusIsError = false;
	uint64_t mSeenSeq = 0;
	// ⚠ DEFAULTS OFF ON PURPOSE. The plain lua_pcall path is the well-evidenced one; the script-thread path
	// goes through an engine function whose argument meaning was originally misread, which crashed the game.
	// It is believed correct now, but "believed" is why it is not the default any more.
	bool mAsThread = false;

	std::vector<std::string> mSuggestions;  // rebuilt each frame from the current prefix
	bool mFocusNext = false;                // set when the hotkey fires, to jump the caret into the box

	static constexpr size_t kMaxHistory = 24;
	static constexpr size_t kMaxSuggestions = 8;

	void submit()
	{
		std::string source(mInput);
		while (!source.empty() && (source.back() == ' ' || source.back() == '\t')) source.pop_back();
		if (source.empty()) return;

		std::string why;
		if (H5LuaConsoleBridge::queue(source, mAsThread, why))
		{
			mStatus = "running: " + source;
			mStatusIsError = false;
			// Drop an identical previous entry so repeating a command does not fill the history with it.
			for (size_t i = 0; i < mHistory.size(); ++i)
				if (mHistory[i] == source) { mHistory.erase(mHistory.begin() + i); break; }
			mHistory.insert(mHistory.begin(), source);
			if (mHistory.size() > kMaxHistory) mHistory.pop_back();
			mInput[0] = '\0';
		}
		else
		{
			mStatus = why;
			mStatusIsError = true;
		}
		mHistoryPos = -1;
		mStashed.clear();
	}

	void setInput(const std::string& s)
	{
		std::snprintf(mInput, sizeof(mInput), "%s", s.c_str());
	}

	// Accept the top suggestion, adding "(" so the next thing typed is the argument list.
	void acceptSuggestion(ImGuiInputTextCallbackData* data)
	{
		if (mSuggestions.empty()) return;
		const std::string chosen = mSuggestions.front() + "(";
		data->DeleteChars(0, data->BufTextLen);
		data->InsertChars(0, chosen.c_str());
	}

	void walkHistory(ImGuiInputTextCallbackData* data, int dir)
	{
		if (mHistory.empty()) return;
		const int prev = mHistoryPos;
		if (dir < 0)                                   // Up = further back
		{
			if (mHistoryPos == -1) { mStashed = data->Buf; mHistoryPos = 0; }
			else if (mHistoryPos + 1 < (int)mHistory.size()) ++mHistoryPos;
		}
		else                                           // Down = back toward the fresh line
		{
			if (mHistoryPos > 0) --mHistoryPos;
			else if (mHistoryPos == 0) mHistoryPos = -1;
		}
		if (prev == mHistoryPos) return;
		const std::string line = (mHistoryPos == -1) ? mStashed : mHistory[(size_t)mHistoryPos];
		data->DeleteChars(0, data->BufTextLen);
		data->InsertChars(0, line.c_str());
	}

	static int inputCallback(ImGuiInputTextCallbackData* data)
	{
		auto* self = static_cast<GUIH5LuaConsole*>(data->UserData);
		if (!self) return 0;
		switch (data->EventFlag)
		{
		case ImGuiInputTextFlags_CallbackCompletion:
			self->acceptSuggestion(data);
			break;
		case ImGuiInputTextFlags_CallbackHistory:
			self->walkHistory(data, data->EventKey == ImGuiKey_UpArrow ? -1 : +1);
			break;
		case ImGuiInputTextFlags_CallbackAlways:
			// Right arrow at the very end of the line accepts the suggestion too - asked for because it is
			// what shell completion does, and it costs nothing next to Tab.
			if (data->CursorPos == data->BufTextLen && !self->mSuggestions.empty()
				&& ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
				self->acceptSuggestion(data);
			break;
		default: break;
		}
		return 0;
	}

public:
	GUIH5LuaConsole(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey,
		std::shared_ptr<SettingsStateAndEvents> settings)
		: IGUIElement(implGame, hotkey, tooltip), mSettingsWeak(settings)
	{
		PLOG_VERBOSE << "Constructing GUIH5LuaConsole";
		this->currentHeight = GUIFrameHeightWithSpacing * 3.f;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		hotkeyRenderer.renderHotkey(mHotkey);

		float used = GUIFrameHeightWithSpacing;

		if (!H5LuaConsoleBridge::isUsable())
		{
			ImGui::TextDisabled("Lua console unavailable on this build of Halo 5: Forge");
			renderTooltip();
			currentHeight = used;
			return;
		}

		// The result arrives asynchronously - the chunk runs on the next sim tick - so pick it up by
		// sequence number rather than assuming the status still refers to what we typed.
		const auto result = H5LuaConsoleBridge::lastResult();
		if (result.valid && result.seq != mSeenSeq)
		{
			mSeenSeq = result.seq;
			mStatus = result.text;
			mStatusIsError = !result.ok;
		}

		// ---- suggestions, computed BEFORE the box so the callbacks can use them ----------------------
		const size_t corpus = H5LuaConsoleBridge::completionCount();
		if (corpus == 0) H5LuaConsoleBridge::requestCompletions();

		// Complete only the leading identifier - past that you are typing arguments, not a name.
		{
			std::string_view line(mInput);
			size_t s = 0;
			while (s < line.size() && (line[s] == ' ' || line[s] == '(')) ++s;
			size_t e = s;
			while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '_')) ++e;
			const bool completingName = (e == line.size());
			std::string_view prefix = completingName ? line.substr(s, e - s) : std::string_view{};
			mSuggestions = (prefix.size() >= 2 && corpus)
				? H5LuaConsoleBridge::complete(prefix, kMaxSuggestions)
				: std::vector<std::string>{};
		}

		if (mFocusNext) { ImGui::SetKeyboardFocusHere(); mFocusNext = false; }
		ImGui::SetNextItemWidth(320.f);
		const bool entered = ImGui::InputText("Lua##h5Console", mInput, sizeof(mInput),
			ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion
			| ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways,
			&GUIH5LuaConsole::inputCallback, this);
		renderTooltip();
		if (entered) { submit(); ImGui::SetKeyboardFocusHere(-1); }

		ImGui::SameLine();
		if (ImGui::Button("Run##h5Console")) submit();

		for (size_t i = 0; i < mSuggestions.size(); ++i)
		{
			// The first one is what Tab / Right arrow will take, so mark it.
			if (i == 0) ImGui::TextUnformatted((mSuggestions[i] + "   [Tab]").c_str());
			else        ImGui::TextDisabled("%s", mSuggestions[i].c_str());
			if (ImGui::IsItemClicked()) setInput(mSuggestions[i] + "(");
			used += GUIFrameHeightWithSpacing * 0.8f;
		}

		ImGui::Checkbox("Allow Sleep (experimental)##h5Console", &mAsThread);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Run the chunk as one of the engine's own script threads, so Sleep, SleepUntil "
				"and coroutine.yield work.\n\nLeave this OFF unless you need those. Off runs the chunk "
				"immediately, which is the better-tested path; a chunk that sleeps will then fail with "
				"\"attempt to yield across metamethod/C-call boundary\" rather than doing anything worse."
				"\n\n⚠ This path called an engine function whose arguments were originally misread, which "
				"crashed the game. The mistake is fixed and guarded, but it has not been proven in-game yet.");
		used += GUIFrameHeightWithSpacing;

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

	// ⚠ NO HOTKEY, and not by oversight. "Pull the console up whenever" needs the HCM overlay to OPEN and
	// the caret to land in this box; a RebindableHotkeyEnum entry alone does neither. HCE_HOTKEYS is also
	// index-locked to a fixed-size event array (static_assert in HotkeyDefinitions.h), and there is no Halo 5
	// hotkey group yet. Worth noting before copying the HaloCER console: hceConsoleHotkey has NO listener
	// anywhere in the codebase - it renders a binding widget and nothing else - so mirroring it would have
	// shipped a key that does nothing. Doing this properly means the MCC-style popup window with its own
	// input capture, which is a real piece of work rather than a flag.
	// mFocusNext is left in place because it is what such a hotkey would set.

	~GUIH5LuaConsole() = default;
	std::string_view getName() override { return nameof(GUIH5LuaConsole); }
};
