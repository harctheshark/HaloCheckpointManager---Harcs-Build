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
// the list of every map from MCC's mod packs plus the stock maps (Halo 2's built in, the other games' from MCC's catalogue).
// Play selects the map (and the gametype chosen for its game) in MCC's open custom game lobby of that game and starts it -
// every game but ODST since 2026-11-19. The element owns the launcher (its hooks live as long as HCM does).
class GUIMapLauncher : public IGUIElement
{
private:
	std::shared_ptr<MapLauncher::Launcher> mLauncher;
	std::shared_ptr<BinarySetting<bool>> mPauseToggle;   // HCM's Pause (may be null)
	bool mToggleKeyDown = false;                         // the window toggle hotkey went down this frame
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

	// Also hide blacklisted maps / packs in MCC's OWN Choose Map list (2026-11-18; saved as "hidemcc=1" in hcm_maplauncher.cfg).
	// The launcher does the work (MapLauncher.cpp, "Hiding maps in MCC's own map list"); this panel hands it the blacklist.
	bool mHideInMcc = false;

	// The gametype Play selects (2026-11-19, user: "save whatever option you last chose, so if say, I selected 'Bounce
	// Passion' it would always have that selected by default"), PER GAME (same day: "bounce passion saved for h2, CTF for
	// halo 3 etc"). Saved as "gametype.<game>=<category id><TAB><name>" in hcm_maplauncher.cfg (<game> = the Game enum
	// number; an older "gametype=" line is Halo 2's); an empty name = the lobby's own gametype. The dropdown shows the
	// selected map's game. mVariants = MCC's list for mVariantsGame, re-read while the dropdown is open (and now and then,
	// so a saved choice MCC does not have can be pointed out).
	static constexpr int kGames = (int)MapLauncher::Game::Count;
	int32_t mGtCategory[kGames] = { -1, -1, -1, -1, -1, -1, -1 };
	std::string mGtName[kGames];
	std::vector<MapLauncher::GameVariant> mVariants;
	MapLauncher::Game mVariantsGame = MapLauncher::Game::Count;
	double mLastVariantPull = -10.0;
	void pushGametype(MapLauncher::Game g) { mLauncher->setGametype(g, mGtCategory[(int)g], mGtName[(int)g]); }
	void pushGametypes() { for (int g = 0; g < kGames; ++g) pushGametype((MapLauncher::Game)g); }

	// the mark keys are the launcher's (it needs them too, to know which maps the blacklist covers)
	static std::string mapMarkKey(const MapLauncher::MapEntry& e) { return MapLauncher::mapMarkKey(e); }
	// "" = this entry has no pack (stock maps)
	static std::string packMarkKey(const MapLauncher::MapEntry& e) { return MapLauncher::packMarkKey(e); }
	void pushBlacklist()
	{
		std::vector<std::string> black;
		for (const auto& [k, v] : mMarks) if (v == MarkBlack) black.push_back(k);
		mLauncher->setBlacklist(std::move(black), mHideInMcc);
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
		pushBlacklist();
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
		// further lines are "name=value" (an older file simply has none)
		mHideInMcc = false;
		for (int g = 0; g < kGames; ++g) { mGtCategory[g] = -1; mGtName[g].clear(); }
		while (f && std::getline(f, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line == "hidemcc=1") { mHideInMcc = true; continue; }
			// "gametype.<game>=..." (per game) or the first version's "gametype=..." (Halo 2's)
			int g = -1; size_t eq = std::string::npos;
			if (line.rfind("gametype=", 0) == 0) { g = (int)MapLauncher::Game::Halo2; eq = 8; }
			else if (line.rfind("gametype.", 0) == 0 && (eq = line.find('=')) != std::string::npos)
			{
				try { g = std::stoi(line.substr(9, eq - 9)); } catch (...) { g = -1; }
			}
			if (g < 0 || g >= kGames) continue;
			const std::string v = line.substr(eq + 1);
			const size_t tab = v.find('\t');
			if (tab == std::string::npos) continue;
			try { mGtCategory[g] = std::stoi(v.substr(0, tab)); } catch (...) { mGtCategory[g] = -1; }
			mGtName[g] = v.substr(tab + 1);
		}
	}
	void saveCfg()
	{
		std::ofstream f(kCfg, std::ios::trunc);
		for (bool b : mShow) f << (b ? '1' : '0');
		f << "\n";
		f << "hidemcc=" << (mHideInMcc ? '1' : '0') << "\n";
		for (int g = 0; g < kGames; ++g)
			if (!mGtName[g].empty()) f << "gametype." << g << '=' << mGtCategory[g] << '\t' << mGtName[g] << "\n";
	}

	// "Gametype (<game>): [ ... v]" - the lobby's own, or one of MCC's for that game (grouped by category, as MCC's Choose
	// Gametype lists them). g = the selected map's game.
	void renderGametype(MapLauncher::Game g)
	{
		const int gi = (int)g;
		const double now = ImGui::GetTime();
		if (g != mVariantsGame) { mVariants.clear(); mVariantsGame = g; mLastVariantPull = -10.0; }
		std::string& name = mGtName[gi];
		int32_t& cat = mGtCategory[gi];
		const bool known = [&] { for (const auto& v : mVariants) if (v.name == name) return true; return false; }();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Gametype (%s):", MapLauncher::gameName(g));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.f);
		const std::string preview = name.empty() ? std::string("Lobby's gametype") : MapLauncher::displayText(name, g);
		const bool open = ImGui::BeginCombo("##mlgametype", preview.c_str(), ImGuiComboFlags_HeightLarge);
		if (open || now - mLastVariantPull > (mVariants.empty() ? 2.0 : 10.0))
		{
			if (now - mLastVariantPull > 1.0) { mLastVariantPull = now; mVariants = mLauncher->gameVariants(g); }
		}
		if (open)
		{
			if (ImGui::Selectable("Lobby's gametype##mlgtlobby", name.empty()))
			{
				name.clear(); cat = -1; saveCfg(); pushGametype(g);
			}
			if (mVariants.empty()) ImGui::TextDisabled("MCC hasn't loaded %s's gametypes yet - open a %s custom game lobby.", MapLauncher::gameName(g), MapLauncher::gameName(g));
			std::string lastCat;
			for (size_t i = 0; i < mVariants.size(); ++i)
			{
				const auto& v = mVariants[i];
				if (i == 0 || v.categoryName != lastCat)
				{
					lastCat = v.categoryName;
					ImGui::Separator();
					ImGui::TextDisabled("%s", v.categoryDisplay.empty() ? "(no category)" : v.categoryDisplay.c_str());
				}
				const bool isSel = v.name == name && v.category == cat;
				if (ImGui::Selectable(std::format("   {}##mlgt{}", v.display, i).c_str(), isSel))
				{
					name = v.name; cat = v.category; saveCfg(); pushGametype(g);
				}
				if (isSel) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Play also selects this gametype in MCC's lobby (if the map supports it).\nRemembered between sessions, separately for each game. \"Lobby's gametype\" keeps whatever the lobby has.");
		if (!name.empty() && !mVariants.empty() && !known)
		{
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.25f, 1.f), "not in MCC's list right now");
		}
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
		// the key that closes the HCM window is not text (it fires from a text box; see HotkeyManager::pollInput)
		if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) return self->mToggleKeyDown ? 1 : 0;
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
		pushBlacklist();   // not from render(): the hiding must work without this panel ever being opened
		pushGametypes();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	std::string_view getName() override { return "Map Launcher"; }

	void render(HotkeyRenderer& hotkeyRenderer) override
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
		{
			const bool can = mLauncher->hideSupported();
			ImGui::BeginDisabled(!can);
			if (ImGui::Checkbox("Hide blacklisted in MCC's own map list##mlhidemcc", &mHideInMcc)) { saveCfg(); pushBlacklist(); }
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(can ? "Blacklisted maps and packs are also left out of the Choose Map list in MCC's custom game lobby.\nThis machine only - other players still see them. Reopen MCC's list to see a change."
					: "Not available: this MCC build's map list is not the one this was made for.");
			if (can && mHideInMcc) { ImGui::SameLine(); ImGui::TextDisabled("(%d hidden)", mLauncher->hiddenCount()); }
		}

		// search (Tab completes, Enter picks the best match)
		mToggleKeyDown = hotkeyRenderer.isPressedNow(RebindableHotkeyEnum::toggleGUI);
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::InputTextWithHint("##mlsearch", "Search maps or packs (Tab completes, Enter picks)", mSearch, sizeof(mSearch),
			ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue, &GUIMapLauncher::completeCallback, this))
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

		// gametype, then play / status
		const MapLauncher::MapEntry* sel = selected();
		renderGametype(sel ? sel->game : MapLauncher::Game::Halo2);
		const bool canPlay = sel && mLauncher->canLaunch(sel->game);
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
		if (sel && !MapLauncher::launchSupported(sel->game)) ImGui::TextDisabled("Launching %s maps is not supported (in MCC it only has Firefight, a different lobby).", MapLauncher::gameName(sel->game));
		else if (sel && !mLauncher->canLaunch(sel->game)) ImGui::TextDisabled("Can't launch %s maps with this MCC build (its custom game lobby wasn't recognised).", MapLauncher::gameName(sel->game));
		else if (sel) ImGui::TextDisabled("Open a %s custom game lobby in MCC, then press Play.", MapLauncher::gameName(sel->game));
		const std::string msg = mLauncher->message();
		if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
		if (mPauseCleared) ImGui::TextWrapped("HCM's Pause was on - switched off so the game can start.");
		else if (mPauseToggle && mPauseToggle->GetValue())
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.25f, 1.f), "HCM's Pause is ON - a game cannot start while it is (Play switches it off).");

		this->currentHeight = ImGui::GetCursorPosY() - y0;
	}
};
