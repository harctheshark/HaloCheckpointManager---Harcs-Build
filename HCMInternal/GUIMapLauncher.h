#pragma once
#include "IGUIElement.h"
#include "MapLauncher.h"
#include <algorithm>
#include <cctype>
#include <fstream>

// HCM addition (2026-11-02): the map launcher's panel (main menu). A game filter (opt each game in / out, remembered in
// hcm_maplauncher.cfg in the game folder), a search box (type to filter; Tab completes to the best match, Enter picks it) and
// the list of every map from MCC's mod packs plus Halo 2's stock maps. Play arms the selected map: the next Halo 2 custom game
// started from MCC's lobby loads it with the lobby's gametype. The element owns the launcher (its halo2.dll hook lives as long
// as HCM does).
class GUIMapLauncher : public IGUIElement
{
private:
	std::shared_ptr<MapLauncher::Launcher> mLauncher;
	bool mShow[(int)MapLauncher::Game::Count];
	char mSearch[128] = {};
	std::vector<MapLauncher::MapEntry> mMaps;
	std::vector<int> mShown;          // indices into mMaps that pass the filter + search, best matches first
	std::string mSelKey;              // the selected map (game | pack | title), kept across list refreshes
	double mLastPull = -10.0;
	bool mFilterDirty = true;

	static constexpr const char* kCfg = "hcm_maplauncher.cfg";

	static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
	static std::string key(const MapLauncher::MapEntry& e) { return std::to_string((int)e.game) + "|" + e.pack + "|" + e.title; }

	void loadCfg()
	{
		for (auto& b : mShow) b = true;
		std::ifstream f(kCfg);
		std::string line;
		if (f && std::getline(f, line))
			for (int i = 0; i < (int)MapLauncher::Game::Count && i < (int)line.size(); ++i) mShow[i] = line[(size_t)i] != '0';
	}
	void saveCfg()
	{
		std::ofstream f(kCfg, std::ios::trunc);
		for (bool b : mShow) f << (b ? '1' : '0');
		f << "\n";
	}

	// every word of the search must appear in the title, pack or scenario; rank: title starts with the search, title contains
	// it, then the rest (pack / scenario only)
	void rebuildShown()
	{
		mShown.clear();
		const std::string q = lower(mSearch);
		std::vector<std::string> words;
		{ size_t p = 0; while (p < q.size()) { while (p < q.size() && q[p] == ' ') ++p; size_t e = q.find(' ', p); if (e == std::string::npos) e = q.size(); if (e > p) words.push_back(q.substr(p, e - p)); p = e; } }
		std::vector<std::pair<int, int>> ranked;
		for (int i = 0; i < (int)mMaps.size(); ++i)
		{
			const auto& m = mMaps[(size_t)i];
			if (!mShow[(int)m.game]) continue;
			const std::string t = lower(m.title), all = t + " " + lower(m.pack) + " " + lower(m.scenario);
			bool ok = true;
			for (const auto& w : words) if (all.find(w) == std::string::npos) { ok = false; break; }
			if (!ok) continue;
			int rank = 2;
			if (!q.empty()) { if (t.rfind(q, 0) == 0) rank = 0; else if (t.find(q) != std::string::npos) rank = 1; }
			ranked.push_back({ rank, i });
		}
		std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first < b.first; });
		for (auto& r : ranked) mShown.push_back(r.second);
		mFilterDirty = false;
	}

	static int completeCallback(ImGuiInputTextCallbackData* data)
	{
		auto* self = static_cast<GUIMapLauncher*>(data->UserData);
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion && !self->mShown.empty())
		{
			const std::string& t = self->mMaps[(size_t)self->mShown.front()].title;
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, t.c_str());
		}
		return 0;
	}

	const MapLauncher::MapEntry* selected() const
	{
		for (const auto& m : mMaps) if (key(m) == mSelKey) return &m;
		return nullptr;
	}

public:
	GUIMapLauncher(GameState implGame, ToolTipCollection tooltip)
		: IGUIElement(implGame, std::nullopt, tooltip), mLauncher(std::make_shared<MapLauncher::Launcher>())
	{
		loadCfg();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	std::string_view getName() override { return "Map Launcher"; }

	void render(HotkeyRenderer&) override
	{
		const float y0 = ImGui::GetCursorPosY();
		if (!ImGui::CollapsingHeader("Map Launcher"))
		{
			renderTooltip();
			this->currentHeight = ImGui::GetCursorPosY() - y0;
			return;
		}
		renderTooltip();

		const double now = ImGui::GetTime();
		if (now - mLastPull > 1.0) { mLastPull = now; auto m = mLauncher->maps(); if (m.size() != mMaps.size()) { mMaps = std::move(m); mFilterDirty = true; } }

		// game filter
		ImGui::TextUnformatted("Games:");
		for (int g = 0; g < (int)MapLauncher::Game::Count; ++g)
		{
			ImGui::SameLine();
			if (ImGui::Checkbox(std::format("{}##mlgame{}", MapLauncher::gameName((MapLauncher::Game)g), g).c_str(), &mShow[g])) { saveCfg(); mFilterDirty = true; }
		}

		// search (Tab completes, Enter picks the best match)
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::InputTextWithHint("##mlsearch", "Search maps or packs (Tab completes, Enter picks)", mSearch, sizeof(mSearch),
			ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_EnterReturnsTrue, &GUIMapLauncher::completeCallback, this))
		{
			rebuildShown();
			if (!mShown.empty()) mSelKey = key(mMaps[(size_t)mShown.front()]);
		}
		if (ImGui::IsItemEdited()) mFilterDirty = true;
		if (mFilterDirty) rebuildShown();

		// the list
		ImGui::BeginChild("##mllist", ImVec2(-1.f, ImGui::GetTextLineHeightWithSpacing() * 10.5f), true);
		for (int i : mShown)
		{
			const auto& m = mMaps[(size_t)i];
			const std::string k = key(m);
			const bool can = MapLauncher::launchSupported(m.game);
			if (!can) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(std::format("{}##ml{}", m.title, i).c_str(), k == mSelKey)) mSelKey = k;
			if (!can) ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled("%s - %s", MapLauncher::gameName(m.game), m.pack.c_str());
		}
		if (mShown.empty()) ImGui::TextDisabled(mLauncher->refreshing() ? "Reading the map packs..." : "No maps match.");
		ImGui::EndChild();

		// play / status
		const MapLauncher::MapEntry* sel = selected();
		const bool canPlay = sel && MapLauncher::launchSupported(sel->game) && mLauncher->hookReady();
		ImGui::BeginDisabled(!canPlay);
		if (ImGui::Button("Play")) mLauncher->arm(*sel);
		ImGui::EndDisabled();
		ImGui::SameLine();
		MapLauncher::MapEntry armed;
		const bool isArmed = mLauncher->armed(armed);
		ImGui::BeginDisabled(!isArmed);
		if (ImGui::Button("Cancel")) mLauncher->disarm();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(mLauncher->refreshing());
		if (ImGui::Button("Refresh list")) mLauncher->refresh();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%zu of %zu maps", mShown.size(), mMaps.size());
		if (sel && !MapLauncher::launchSupported(sel->game)) ImGui::TextDisabled("Launching %s maps is not supported yet (Halo 2 only for now).", MapLauncher::gameName(sel->game));
		const std::string msg = mLauncher->message();
		if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());

		this->currentHeight = ImGui::GetCursorPosY() - y0;
	}
};
