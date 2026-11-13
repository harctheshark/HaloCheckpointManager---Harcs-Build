#pragma once
#include "IGUIElement.h"
#include "MapLauncher.h"
#include "BinarySetting.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

// HCM addition (2026-11-02): the map launcher's panel (main menu). A game filter (opt each game in / out, remembered in
// hcm_maplauncher.cfg in the game folder), a search box (type to filter; Tab completes to the best match, Enter picks it) and
// the list of every map from MCC's mod packs plus Halo 2's stock maps. Play arms the selected map: the next Halo 2 custom game
// started from MCC's lobby loads it with the lobby's gametype. The element owns the launcher (its halo2.dll hook lives as long
// as HCM does).
class GUIMapLauncher : public IGUIElement
{
private:
	std::shared_ptr<MapLauncher::Launcher> mLauncher;
	std::shared_ptr<BinarySetting<bool>> mPauseToggle;   // HCM's Pause (may be null)
	bool mPauseCleared = false;                          // the last Play switched Pause off
	bool mShow[(int)MapLauncher::Game::Count];
	char mSearch[128] = {};
	std::vector<MapLauncher::MapEntry> mMaps;
	std::vector<int> mShown;          // indices into mMaps that pass the filter + search, best matches first
	std::string mSelKey;              // the selected map (game | pack | title), kept across list refreshes
	double mLastPull = -10.0;
	bool mFilterDirty = true;

	static constexpr const char* kCfg = "hcm_maplauncher.cfg";

	// Favourites / blacklist (2026-11-11). Stored in hcm_maplauncher_marks.txt next to hcm_maplauncher.cfg: one entry per line,
	// "F<TAB>key" or "B<TAB>key". Keys: "m:<MapGuid hex>" (mod map), "s:<stock index>" (stock map), "p:<ModGuid hex>" (a whole pack),
	// with fallbacks "mn:<game>|<pack>|<title>" / "pn:<pack>" when a GUID is missing. A missing / unreadable file = no marks.
	static constexpr const char* kMarks = "hcm_maplauncher_marks.txt";
	enum Mark : int { MarkNone = 0, MarkFav = 1, MarkBlack = 2 };
	std::map<std::string, int> mMarks;
	bool mFavOnly = false;
	bool mShowBlack = false;

	static bool guidBlank(const uint8_t* g) { for (int i = 0; i < 16; ++i) if (g[i]) return false; return true; }
	static std::string hex(const uint8_t* g, int n) { static const char* d = "0123456789abcdef"; std::string s; for (int i = 0; i < n; ++i) { s += d[g[i] >> 4]; s += d[g[i] & 15]; } return s; }
	static std::string mapMarkKey(const MapLauncher::MapEntry& e)
	{
		if (e.stock) return e.stockIndex >= 0 ? "s:" + std::to_string(e.stockIndex) : "mn:" + key(e);
		if (!guidBlank(e.guid)) return "m:" + hex(e.guid, 16);
		return "mn:" + key(e);
	}
	// "" = this entry has no pack (stock maps)
	static std::string packMarkKey(const MapLauncher::MapEntry& e)
	{
		if (e.stock) return "";
		if (!guidBlank(e.modGuid)) return "p:" + hex(e.modGuid, 16);
		return e.pack.empty() ? "" : "pn:" + e.pack;
	}
	int markOf(const std::string& k) const { if (k.empty()) return MarkNone; auto it = mMarks.find(k); return it == mMarks.end() ? (int)MarkNone : it->second; }
	bool isBlack(const MapLauncher::MapEntry& e) const { return markOf(mapMarkKey(e)) == MarkBlack || markOf(packMarkKey(e)) == MarkBlack; }
	bool isFav(const MapLauncher::MapEntry& e) const { return !isBlack(e) && (markOf(mapMarkKey(e)) == MarkFav || markOf(packMarkKey(e)) == MarkFav); }

	void loadMarks()
	{
		mMarks.clear();
		try
		{
			std::ifstream f(kMarks);
			std::string line;
			while (f && std::getline(f, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.size() < 3 || line[1] != '\t') continue;
				if (line[0] == 'F') mMarks[line.substr(2)] = MarkFav;
				else if (line[0] == 'B') mMarks[line.substr(2)] = MarkBlack;
			}
		}
		catch (...) { mMarks.clear(); }
	}
	void saveMarks()
	{
		try
		{
			std::ofstream f(kMarks, std::ios::trunc);
			for (const auto& [k, v] : mMarks) if (v == MarkFav || v == MarkBlack) f << (v == MarkFav ? 'F' : 'B') << '\t' << k << "\n";
		}
		catch (...) {}
	}
	// toggle: choosing the mark an entry already has clears it; otherwise it replaces any other mark (mutually exclusive)
	void toggleMark(const std::string& k, int mark)
	{
		if (k.empty()) return;
		auto it = mMarks.find(k);
		if (it != mMarks.end() && it->second == mark) mMarks.erase(it); else mMarks[k] = mark;
		saveMarks();
		mFilterDirty = true;
	}

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
			const bool black = isBlack(m), fav = isFav(m);
			if (black && !mShowBlack) continue;
			if (mFavOnly && !fav) continue;
			const std::string t = lower(m.title), all = t + " " + lower(m.pack) + " " + lower(m.scenario);
			bool ok = true;
			for (const auto& w : words) if (all.find(w) == std::string::npos) { ok = false; break; }
			if (!ok) continue;
			int rank = 2;
			if (!q.empty()) { if (t.rfind(q, 0) == 0) rank = 0; else if (t.find(q) != std::string::npos) rank = 1; }
			// favourites first (rank 0 for non-favourites is shifted by 3 so the search rank still orders within each group)
			ranked.push_back({ rank + (fav ? 0 : 3), i });
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
	// pauseToggle: HCM's Pause setting. A game that starts paused never gets past MCC's "Starting game..." screen, so Play
	// switches Pause off first (and says so).
	GUIMapLauncher(GameState implGame, ToolTipCollection tooltip, std::shared_ptr<BinarySetting<bool>> pauseToggle = nullptr)
		: IGUIElement(implGame, std::nullopt, tooltip), mLauncher(std::make_shared<MapLauncher::Launcher>()), mPauseToggle(std::move(pauseToggle))
	{
		loadCfg();
		loadMarks();
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

		// favourites / blacklist view options
		if (ImGui::Checkbox("Favourites only##mlfavonly", &mFavOnly)) mFilterDirty = true;
		ImGui::SameLine();
		if (ImGui::Checkbox("Show blacklisted##mlshowblack", &mShowBlack)) mFilterDirty = true;
		ImGui::SameLine();
		ImGui::TextDisabled("(* favourite, x blacklist; right-click a map for pack options)");

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
			const std::string mk = mapMarkKey(m), pk = packMarkKey(m);
			const int mMark = markOf(mk), pMark = markOf(pk);
			const bool black = isBlack(m), fav = isFav(m);
			const ImVec4 colFav(1.f, 0.82f, 0.2f, 1.f), colBlack(0.9f, 0.3f, 0.3f, 1.f);
			bool toggleFav = false, toggleBlack = false, togglePackFav = false, togglePackBlack = false;
			ImGui::PushID(i);
			// per-row star / x buttons (lit when the map itself carries that mark)
			ImGui::PushStyleColor(ImGuiCol_Text, mMark == MarkFav ? colFav : (fav ? ImVec4(colFav.x, colFav.y, colFav.z, 0.5f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)));
			toggleFav = ImGui::SmallButton("*##fav");
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(mMark == MarkFav ? "Remove from favourites" : "Favourite this map");
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, mMark == MarkBlack ? colBlack : (black ? ImVec4(colBlack.x, colBlack.y, colBlack.z, 0.5f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)));
			toggleBlack = ImGui::SmallButton("x##bl");
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(mMark == MarkBlack ? "Remove from blacklist" : "Blacklist this map");
			ImGui::SameLine();
			if (!can || black) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::Selectable(std::format("{}##ml", m.title).c_str(), k == mSelKey)) mSelKey = k;
			if (!can || black) ImGui::PopStyleColor();
			if (ImGui::BeginPopupContextItem("##mlctx"))
			{
				if (ImGui::MenuItem("Favourite map", nullptr, mMark == MarkFav)) toggleFav = true;
				if (ImGui::MenuItem("Blacklist map", nullptr, mMark == MarkBlack)) toggleBlack = true;
				if (!pk.empty())
				{
					ImGui::Separator();
					ImGui::TextDisabled("Pack: %s", m.pack.c_str());
					if (ImGui::MenuItem("Favourite pack", nullptr, pMark == MarkFav)) togglePackFav = true;
					if (ImGui::MenuItem("Blacklist pack", nullptr, pMark == MarkBlack)) togglePackBlack = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
			ImGui::SameLine();
			ImGui::TextDisabled("%s - %s%s%s", MapLauncher::gameName(m.game), m.pack.c_str(), pMark == MarkFav ? " [fav pack]" : "", black && pMark == MarkBlack ? " [blacklisted pack]" : "");
			if (toggleFav) toggleMark(mk, MarkFav);
			if (toggleBlack) toggleMark(mk, MarkBlack);
			if (togglePackFav) toggleMark(pk, MarkFav);
			if (togglePackBlack) toggleMark(pk, MarkBlack);
		}
		if (mShown.empty()) ImGui::TextDisabled(mLauncher->refreshing() ? "Reading the map packs..." : "No maps match.");
		ImGui::EndChild();

		// play / status
		const MapLauncher::MapEntry* sel = selected();
		const bool canPlay = sel && MapLauncher::launchSupported(sel->game) && mLauncher->hookReady();
		ImGui::BeginDisabled(!canPlay);
		if (ImGui::Button("Play"))
		{
			mPauseCleared = mPauseToggle && mPauseToggle->GetValue();
			if (mPauseCleared)
			{
				mPauseToggle->GetValueDisplay() = false;
				mPauseToggle->UpdateValueWithInput();
				PLOG_INFO << "[MapLauncher] HCM's Pause was on: switched off for the launch (a paused game never leaves MCC's Starting game screen)";
			}
			mLauncher->arm(*sel);
		}
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
		if (mPauseCleared) ImGui::TextWrapped("HCM's Pause was on - switched off so the game can start.");
		else if (mPauseToggle && mPauseToggle->GetValue())
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.25f, 1.f), "HCM's Pause is ON - a game cannot start while it is (Play switches it off).");

		this->currentHeight = ImGui::GetCursorPosY() - y0;
	}
};
