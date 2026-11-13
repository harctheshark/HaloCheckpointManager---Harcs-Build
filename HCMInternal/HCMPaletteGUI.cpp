#include "pch.h"
#include "HCMPaletteGUI.h"
#include "imgui.h"
#include "imgui_internal.h"   // ClearActiveID: drop the search box's keyboard focus when the game takes the cursor
#include "imgui_stdlib.h"
#include <unordered_set>
#include "ScopedCallback.h"
#include "GUIHeading.h"
#include "GUISubHeading.h"
#include "GUIInvisible.h"
#include "GUIDummyContainer.h"
#include "GUIRadioGroup.h"

// See HCMPaletteGUI.h for what this is. Layout of this file:
//   helpers (text, fuzzy match, small drawing)   -> index of the current game's options
//   -> favourites / recents persistence          -> the five views -> render()

namespace
{
	const ImVec4 kAmber{ 1.00f, 0.80f, 0.40f, 1.00f };   // the window title colour
	const ImVec4 kDim{ 0.52f, 0.61f, 0.61f, 1.00f };     // secondary text (HCM's TextDisabled is too dark to read)
	const ImVec4 kDim2{ 0.37f, 0.44f, 0.44f, 1.00f };
	const ImVec4 kSelBg{ 0.55f, 0.23f, 0.29f, 0.50f };
	const ImVec4 kSelBgHover{ 0.55f, 0.23f, 0.29f, 0.30f };
	const ImVec4 kTileOn{ 0.35f, 0.09f, 0.12f, 1.00f };
	const ImVec4 kTileOff{ 0.18f, 0.10f, 0.18f, 0.55f };
	const ImVec4 kTileButton{ 0.30f, 0.09f, 0.12f, 1.00f };
	const ImVec4 kTileHover{ 0.55f, 0.23f, 0.29f, 1.00f };

	constexpr const char* kTheaterReplayName = "Theater Replay";

	std::string toLower(std::string_view s)
	{
		std::string out(s);
		for (auto& c : out) c = (char)std::tolower((unsigned char)c);
		return out;
	}

	std::string trim(std::string_view s)
	{
		size_t a = 0, b = s.size();
		while (a < b && std::isspace((unsigned char)s[a])) ++a;
		while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
		return std::string(s.substr(a, b - a));
	}

	// Widget names double as ImGui IDs ("Font Size##Console") and some are indented with spaces; show neither.
	std::string cleanLabel(std::string_view raw, std::string_view fallback)
	{
		const auto hash = raw.find("##");
		std::string out = trim(hash == std::string_view::npos ? raw : raw.substr(0, hash));
		if (out == "GUISpeedhack") out = "Speedhack"; // that widget names itself after its class
		if (out.empty()) out = std::string(fallback);
		return out;
	}

	std::vector<std::string> splitList(const std::string& s)
	{
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= s.size())
		{
			const size_t end = s.find(';', start);
			std::string item = trim(std::string_view(s).substr(start, (end == std::string::npos ? s.size() : end) - start));
			if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.push_back(std::move(item));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return out;
	}

	std::string joinList(const std::vector<std::string>& v)
	{
		std::string out;
		for (const auto& s : v) { if (!out.empty()) out += ';'; out += s; }
		return out;
	}

	// ---- fuzzy match: contiguous substring first (best occurrence), else an in-order letter match ("fdr") ----
	struct Match { int score = 0; std::vector<int> idx; };

	bool isWordStart(const std::string& t, int i) { return i == 0 || !std::isalnum((unsigned char)t[i - 1]); }

	std::optional<Match> matchText(const std::string& q, const std::string& t)
	{
		if (q.empty() || t.empty()) return std::nullopt;

		std::optional<Match> best;
		for (size_t from = 0;;)
		{
			const size_t i = t.find(q, from);
			if (i == std::string::npos) break;
			const int sc = 1000 + (i == 0 ? 200 : 0) + (isWordStart(t, (int)i) ? 100 : 0) - (int)i - ((int)t.size() - (int)q.size()) * 2;
			if (!best || sc > best->score)
			{
				best = Match{ sc, {} };
				for (size_t k = 0; k < q.size(); ++k) best->idx.push_back((int)(i + k));
			}
			from = i + 1;
		}
		if (best) return best;

		auto trySequence = [&](bool preferWordStarts) -> std::optional<Match>
			{
				int pos = 0, prev = -2, sc = 400;
				Match m;
				for (const char ch : q)
				{
					if (ch == ' ') continue;
					int j = -1;
					if (prev >= 0 && prev + 1 < (int)t.size() && t[prev + 1] == ch) j = prev + 1;
					else
					{
						if (preferWordStarts)
							for (int k = pos; k < (int)t.size(); ++k)
								if (t[k] == ch && isWordStart(t, k)) { j = k; break; }
						if (j < 0)
						{
							const size_t f = t.find(ch, (size_t)pos);
							if (f == std::string::npos) return std::nullopt;
							j = (int)f;
						}
					}
					if (prev >= 0 && j == prev + 1) sc += 20;
					else if (prev >= 0) sc -= 3 * (j - prev - 1);
					else sc -= 2 * j;
					if (isWordStart(t, j)) sc += 30;
					m.idx.push_back(j);
					prev = j;
					pos = j + 1;
				}
				m.score = sc;
				return m;
			};

		auto a = trySequence(true), b = trySequence(false);
		std::optional<Match> r = (a && b) ? (a->score >= b->score ? a : b) : (a ? a : b);
		if (r && r->score >= 425) return r;
		return std::nullopt;
	}

	// ---- search aliases: words people type that are not in the option's name. Keyed by GUIElementEnum NAME so
	// a renamed / removed element just stops matching instead of failing to compile. Purely additive. ----
	const std::unordered_map<std::string_view, std::string_view>& aliasTable()
	{
		static const std::unordered_map<std::string_view, std::string_view> table
		{
			{ "invulnGUI", "god godmode invincible invincibility deathless immortal" },
			{ "speedhackGUI", "game speed slowmo slow motion fast forward timescale" },
			{ "hceGameSpeedGUI", "speedhack slowmo slow motion fast forward timescale" },
			{ "aiFreezeGUI", "ai disable stop enemies" },
			{ "hceAiFreezeGUI", "ai disable stop enemies" },
			{ "medusaGUI", "kill all enemies" },
			{ "forceTeleportGUI", "tp warp" },
			{ "hceForceTeleportGUI", "tp warp" },
			{ "forceLaunchGUI", "boost velocity fling" },
			{ "masterTickrateEnableGUI", "fps tick rate 30 60 framerate" },
			{ "masterTickrateToggleGUI", "fps tick rate 30 60 framerate" },
			{ "masterTickrateCustomGUI", "fps tick rate framerate" },
			{ "freeCameraToggleGUI", "noclip freecam fly cam spectate" },
			{ "hceFreecamToggleGUI", "noclip free camera fly cam spectate" },
			{ "hceFreecamNoclipGUI", "fly through walls" },
			{ "chaseCameraToggleGUI", "third person 3rd" },
			{ "chaseCameraNoClipGUI", "noclip" },
			{ "hideHUDToggle", "hud interface clean" },
			{ "infiniteAmmoGUI", "unlimited ammo" },
			{ "bottomlessClipGUI", "no reload unlimited ammo" },
			{ "oneShotKillGUI", "instakill ohk one hit damage" },
			{ "forceCheckpointGUI", "save cp" },
			{ "hceForceCheckpointGUI", "save cp" },
			{ "forceRevertGUI", "load reload cp" },
			{ "forceDoubleRevertGUI", "load previous cp" },
			{ "injectCheckpointGUI", "load import cp" },
			{ "dumpCheckpointGUI", "save export cp" },
			{ "naturalCheckpointDisableGUI", "no checkpoints" },
			{ "display2DInfoToggleGUI", "coords coordinates position velocity speedometer" },
			{ "triggerOverlayToggle", "trigger volumes zones" },
			{ "softCeilingOverlayToggle", "oob out of bounds invisible wall barrier" },
			{ "disableBarriersToggle", "oob out of bounds soft ceiling invisible wall" },
			{ "collisionViewerToggleGUI", "wireframe geometry bsp hitbox" },
			{ "collisionViewerKillVolumesGUI", "oob death timer kill volume" },
			{ "collisionViewerShowKillTriggersGUI", "oob death barrier kill volume" },
			{ "collisionViewerSuperbounceGUI", "sb bounce" },
			{ "launchTrajectoryToggleGUI", "arc prediction butterfly slide" },
			{ "consoleCommandGUI", "dev console haloscript hs script" },
			{ "switchBSPGUI", "zone set load area" },
			{ "skullToggleGUI", "skulls" },
			{ "togglePauseGUI", "freeze time stop" },
			{ "advanceTicksGUI", "frame advance step" },
			{ "waypoint3DGUIToggle", "markers nav points" },
			{ "classicLayoutToggleGUI", "old ui legacy window new layout" },
			{ "focusSearchHotkeyGUI", "find command palette" },
			{ "replayRecord30GUI", "tas input recording" },
			{ "replayPlayGUI", "tas input playback" },
		};
		return table;
	}

	bool isContainerHeading(IGUIElement* el)
	{
		return dynamic_cast<GUIHeading*>(el) || dynamic_cast<GUISubHeading<true>*>(el) || dynamic_cast<GUISubHeading<false>*>(el);
	}

	// containers that only exist to group / lay out their children and carry no user-facing name of their own
	bool isAnonymousContainer(IGUIElement* el)
	{
		return dynamic_cast<GUIDummyContainer*>(el) || dynamic_cast<GUIRadioGroup*>(el);
	}

	// ---- tabbed cards: an option whose settings are too many for one list gets them split into tabs when it is
	// opened from a search result or shown on Favourites. Keyed by the option's GUIElementEnum NAME; every item
	// is another element's enum name, drawn with that element's existing widget. A leading '*' means "draw that
	// heading's children rather than the (collapsed) heading itself". Items the current game does not have are
	// skipped, a tab with nothing in it is not shown, and any child of the option that no tab claims lands in an
	// automatic "Other" tab - so this table can go stale without hiding anything. Options not listed here are
	// drawn as their plain widget + child list.
	struct CardTab { const char* name; std::vector<std::string_view> items; };
	const std::unordered_map<std::string_view, std::vector<CardTab>>& cardTable()
	{
		static const std::unordered_map<std::string_view, std::vector<CardTab>> table
		{
			{ "collisionViewerToggleGUI",
				{
					{ "Geometry", { "collisionViewerOcclusionGUI", "collisionViewerShowBspGUI", "collisionViewerShowInstancedGUI", "collisionViewerShowInvisibleGUI",
						"collisionViewerShowBreakableGUI", "collisionViewerShowSceneryGUI", "collisionViewerShowCratesGUI", "collisionViewerShowMachinesGUI",
						"collisionViewerShowKillTriggersGUI", "collisionViewerShowSoftCeilingsGUI", "collisionViewerFillsGUI", "collisionViewerTimGUI",
						"collisionViewerTimLabelsGUI", "collisionViewerPillsGUI", "collisionViewerCreasesGUI", "collisionViewerCreaseFacesGUI" } },
					{ "Ghost", { "collisionViewerGhostGUI", "collisionViewerGhostJumpGUI", "collisionViewerGhostVehiclesGUI", "collisionViewerGhostHaloGUI" } },
					{ "Superbounce", { "collisionViewerSuperbounceGUI", "*collisionViewerSuperbounceSettingsGUI" } },
					{ "Info panel", { "collisionViewerInfoPanelGUI" } },
					{ "OOB bars", { "collisionViewerKillVolumesGUI", "*oobBarSettingsGUI" } },
					{ "Render", { "collisionViewerLineWidthGUI", "collisionViewerDepthPullGUI", "collisionViewerRadiusGUI", "collisionViewerHiddenAlphaGUI", "collisionViewerReversedZGUI" } },
				} },
		};
		return table;
	}

	void drawStar(ImDrawList* dl, ImVec2 c, float radius, ImU32 col, bool filled)
	{
		ImVec2 pts[10];
		for (int i = 0; i < 10; ++i)
		{
			const float ang = -1.5707963f + i * 0.6283185f;
			const float r = (i % 2 == 0) ? radius : radius * 0.42f;
			pts[i] = ImVec2(c.x + std::cos(ang) * r, c.y + std::sin(ang) * r);
		}
		if (filled) dl->AddConcavePolyFilled(pts, 10, col);
		else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.0f);
	}
}


class HCMPaletteGUI::Impl
{
public:
	// ------------------------------------------------------------------------------------------- services
	std::shared_ptr<GUIElementStore> mStore;
	std::shared_ptr<HotkeyRenderer> mHotkeyRenderer;
	std::shared_ptr<SettingsStateAndEvents> mSettings;
	std::shared_ptr<ControlServiceContainer> mControl;

	// hotkey events arrive on a detached thread (HotkeyManager), so the press is just latched here and
	// consumed by render().
	std::atomic<bool> mFocusRequested{ false };
	ScopedCallback<ActionEvent> mFocusHotkeyCallback;

	// ------------------------------------------------------------------------------------------- index
	struct Entry
	{
		const GUIElementNode* node = nullptr;
		std::string idName;        // GUIElementEnum name: the stable key favourites / recents are stored under
		std::string label, labelLower;
		std::string path, pathLower; // "Useful Cheats > Force Teleport Settings"
		std::string tipLower, aliasLower;
		int category = -1;
		int depth = 1;             // 1 = directly under a category
		bool heading = false;      // a (sub)heading: has no action of its own, only children
		bool indented = false;     // HCM marks a row that depends on the one above it by indenting its label
		int subCount = 0;          // how many options live under it (children + companion rows)
		std::vector<const GUIElementNode*> companions; // the "... Settings" block(s) that follow it in its parent
	};
	struct Category
	{
		std::string name;
		GUIElementEnum id{};
		std::vector<const GUIElementNode*> roots; // what Browse all draws for this category, in order
		int optionCount = 0;
	};
	struct Hit { int entry = -1; int score = 0; std::vector<int> idx; };

	std::optional<GameState> mIndexedGame;
	std::vector<Entry> mEntries;
	std::vector<Category> mCategories;
	std::unordered_map<std::string, int> mEntryByName;
	std::unordered_map<const GUIElementNode*, int> mEntryByNode;

	// ------------------------------------------------------------------------------------------- persisted lists
	std::vector<std::string> mFavourites, mRecents;      // GUIElementEnum names
	std::vector<std::string> mPendingFavouriteToggles;   // applied at the end of the frame (never mid-iteration)

	// ------------------------------------------------------------------------------------------- ui state
	enum Tab { TabFavourites = 0, TabBrowse = 1 };
	int mTab = TabBrowse;
	bool mTabPickedByUser = false;   // until the user clicks a tab, the tab follows "has favourites in this game?"
	// Every widget object drawn so far this frame. A widget keeps its own measured height, so drawing the same one in
	// two places makes the two fight over it (overlapping rows, empty boxes, the scroll position jumping).
	std::unordered_set<const IGUIElement*> mDrawn;
	int mPendingTab = -1;
	int mBrowseCategory = 0;      // -1 = All
	bool mShowActive = false;

	std::string mQuery, mLastQuery;
	std::vector<Hit> mHits;
	int mSelected = 0;
	int mExpandedEntry = -1;
	bool mScrollToSelected = false;
	std::string mCompletion;      // full label the ghost text would complete to ("" if the top hit is not a prefix match)

	bool mSearchActive = false;
	bool mRefocusSearch = false;
	bool mPrevCursorAtEnd = false;
	int mKeyNav = 0;
	bool mKeyTab = false;
	int mClearQueryFrames = 0;    // >0: empty the search box (a tab was clicked while results were showing)

	float mBodyContentHeight = 150.f;
	float mFixedHeight = 120.f;

	std::shared_ptr<SharedRequestToken> mHotkeyDisableToken, mBlockInputToken;

	Impl(std::shared_ptr<GUIElementStore> store, std::shared_ptr<HotkeyRenderer> hotkeyRenderer,
		std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<ControlServiceContainer> control)
		: mStore(store), mHotkeyRenderer(hotkeyRenderer), mSettings(settings), mControl(control),
		mFocusHotkeyCallback(settings->focusSearchHotkeyEvent, [this]() { mFocusRequested.store(true, std::memory_order_release); })
	{
		mFavourites = splitList(mSettings->uiFavourites->GetValue());
		mRecents = splitList(mSettings->uiRecents->GetValue());
	}

	// =========================================================================================== index
	void addNode(const GUIElementNode& n, const std::vector<GUIElementNode>& siblings, size_t siblingIndex, int category,
		const std::string& path, int depth, std::vector<const GUIElementNode*>& pulledOut)
	{
		IGUIElement* el = n.element.get();
		if (!el) return;
		if (dynamic_cast<GUIInvisible*>(el)) return;

		// The Replay System gets its own category in this layout (see rebuildIndex).
		if (n.id == GUIElementEnum::replayInputSubheading) { pulledOut.push_back(&n); return; }

		if (isAnonymousContainer(el))
		{
			for (size_t i = 0; i < n.children.size(); ++i) addNode(n.children[i], n.children, i, category, path, depth, pulledOut);
			return;
		}

		const bool heading = isContainerHeading(el);
		if (heading && n.children.empty()) return; // draws nothing in the classic layout either

		Entry e;
		e.node = &n;
		e.idName = std::string(magic_enum::enum_name(n.id));
		const std::string_view rawName = el->getName();
		e.indented = !rawName.empty() && rawName[0] == ' ';
		e.label = cleanLabel(rawName, e.idName);
		e.labelLower = toLower(e.label);
		e.path = path;
		e.pathLower = toLower(path);
		e.tipLower = toLower(el->getTooltipText());
		if (const auto alias = aliasTable().find(e.idName); alias != aliasTable().end()) e.aliasLower = std::string(alias->second);
		e.category = category;
		e.depth = depth;
		e.heading = heading;

		// companion rows: HCM's convention is "the toggle, then its settings block" as SIBLINGS. So that a
		// favourite or an expanded search result shows the option WITH its settings, collect what follows it:
		// indented rows, "... Settings" subheadings, and list widgets that sit between the two.
		if (!heading)
		{
			for (size_t j = siblingIndex + 1; j < siblings.size(); ++j)
			{
				IGUIElement* s = siblings[j].element.get();
				if (!s || siblings[j].id == GUIElementEnum::replayInputSubheading) break;
				const std::string_view sRaw = s->getName();
				const std::string sLower = toLower(sRaw);
				const bool sHeading = isContainerHeading(s);
				if (!e.indented && !sRaw.empty() && sRaw[0] == ' ') e.companions.push_back(&siblings[j]);
				else if (sHeading && sLower.find("setting") != std::string::npos) e.companions.push_back(&siblings[j]);
				else if (!sHeading && s->quickKind() == IGUIElement::QuickKind::None && sLower.find("list of") != std::string::npos) e.companions.push_back(&siblings[j]);
				else break;
			}
		}

		const int me = (int)mEntries.size();
		mEntryByName.emplace(e.idName, me);
		mEntryByNode.emplace(&n, me);
		mEntries.push_back(std::move(e));
		if (category >= 0 && category < (int)mCategories.size()) mCategories[category].optionCount++;

		const std::string childPath = path + " > " + mEntries[me].label;
		const int before = (int)mEntries.size();
		for (size_t i = 0; i < n.children.size(); ++i) addNode(n.children[i], n.children, i, category, childPath, depth + 1, pulledOut);
		int sub = (int)mEntries.size() - before;
		for (const auto* c : mEntries[me].companions) sub += 1 + (int)c->children.size();
		mEntries[me].subCount = sub;
	}

	void rebuildIndex(GameState game)
	{
		mEntries.clear(); mCategories.clear(); mEntryByName.clear(); mEntryByNode.clear();
		mHits.clear(); mLastQuery.clear(); mExpandedEntry = -1; mSelected = 0;

		const auto& tree = mStore->getElementTree(game);
		std::vector<const GUIElementNode*> pulledOut;

		// categories = the existing top level elements, in their existing order
		for (size_t t = 0; t < tree.size(); ++t)
		{
			const GUIElementNode& top = tree[t];
			if (!top.element) continue;
			const bool heading = isContainerHeading(top.element.get());
			if (heading && top.children.empty()) continue;

			Category c;
			c.id = top.id;
			c.name = cleanLabel(top.element->getName(), magic_enum::enum_name(top.id));
			const int ci = (int)mCategories.size();
			mCategories.push_back(std::move(c));

			if (heading)
			{
				for (size_t i = 0; i < top.children.size(); ++i)
				{
					const size_t pulledBefore = pulledOut.size();
					addNode(top.children[i], top.children, i, ci, mCategories[ci].name, 1, pulledOut);
					if (pulledOut.size() == pulledBefore) mCategories[ci].roots.push_back(&top.children[i]);
				}
			}
			else
			{
				// a top level element that is a widget in its own right (e.g. the map launcher): it IS the category
				mCategories[ci].roots.push_back(&top);
				addNode(top, tree, t, ci, mCategories[ci].name, 1, pulledOut);
			}
		}

		// "Theater Replay": the one deliberate exception to "categories are the top level headings". The Replay
		// System subtree is lifted out of wherever it sits (Save Management today) and shown as its own category,
		// in this layout only - the classic tree is not touched. Present only in games that have it.
		// (It is a direct child of its heading, which is what lets Browse all skip it there; if it ever moves
		// deeper it would simply appear in both places.)
		for (const auto* pulled : pulledOut)
		{
			if (pulled->children.empty()) continue;
			Category c;
			c.id = pulled->id;
			c.name = kTheaterReplayName;
			const int ci = (int)mCategories.size();
			mCategories.push_back(std::move(c));
			std::vector<const GUIElementNode*> ignored;
			for (size_t i = 0; i < pulled->children.size(); ++i)
			{
				mCategories[ci].roots.push_back(&pulled->children[i]);
				addNode(pulled->children[i], pulled->children, i, ci, kTheaterReplayName, 1, ignored);
			}
		}

		if (mBrowseCategory >= (int)mCategories.size()) mBrowseCategory = 0;
		PLOG_DEBUG << "HCMPaletteGUI indexed " << mEntries.size() << " options in " << mCategories.size() << " categories for " << game.toString();
	}

	// =========================================================================================== favourites / recents
	bool isFavourite(const std::string& idName) const { return std::find(mFavourites.begin(), mFavourites.end(), idName) != mFavourites.end(); }
	bool isRecent(const std::string& idName) const { return std::find(mRecents.begin(), mRecents.end(), idName) != mRecents.end(); }

	void persist(const std::shared_ptr<BinarySetting<std::string>>& setting, const std::vector<std::string>& list)
	{
		setting->GetValueDisplay() = joinList(list);
		setting->UpdateValueWithInput(); // marks the config dirty; the autosave thread writes it out
	}

	void applyPendingFavouriteToggles()
	{
		if (mPendingFavouriteToggles.empty()) return;
		for (const auto& name : mPendingFavouriteToggles)
		{
			auto it = std::find(mFavourites.begin(), mFavourites.end(), name);
			if (it != mFavourites.end()) mFavourites.erase(it); else mFavourites.push_back(name);
		}
		mPendingFavouriteToggles.clear();
		persist(mSettings->uiFavourites, mFavourites);
	}

	void touchRecent(const std::string& idName)
	{
		auto it = std::find(mRecents.begin(), mRecents.end(), idName);
		if (it != mRecents.end()) mRecents.erase(it);
		mRecents.insert(mRecents.begin(), idName);
		if (mRecents.size() > 6) mRecents.resize(6);
		persist(mSettings->uiRecents, mRecents);
	}

	std::vector<int> resolve(const std::vector<std::string>& names) const
	{
		std::vector<int> out;
		for (const auto& n : names)
			if (const auto it = mEntryByName.find(n); it != mEntryByName.end()) out.push_back(it->second);
		return out;
	}

	// toggles: flip. buttons: fire. anything else has no one-key action. Returns whether something happened.
	bool activate(int entryIndex)
	{
		if (entryIndex < 0 || entryIndex >= (int)mEntries.size()) return false;
		auto& e = mEntries[entryIndex];
		IGUIElement* el = e.node->element.get();
		if (el->quickKind() == IGUIElement::QuickKind::None) return false;
		el->quickActivate();
		touchRecent(e.idName);
		return true;
	}

	// "Currently on": switched-on toggles that are features in their own right - directly under a category, not
	// an indented dependent row, and not one of the Control heading's window preferences.
	bool countsAsActive(const Entry& e) const
	{
		if (e.depth != 1 || e.indented || e.heading) return false;
		if (e.category < 0 || mCategories[e.category].id == GUIElementEnum::controlHeadingGUI) return false;
		IGUIElement* el = e.node->element.get();
		return el->quickKind() == IGUIElement::QuickKind::Toggle && el->quickIsOn();
	}
	std::vector<int> activeEntries() const
	{
		std::vector<int> out;
		for (int i = 0; i < (int)mEntries.size(); ++i) if (countsAsActive(mEntries[i])) out.push_back(i);
		return out;
	}

	std::string hotkeyText(const Entry& e) const
	{
		const auto hk = e.node->element->getHotkey();
		return hk.has_value() ? mHotkeyRenderer->getBindingTextShort(hk.value()) : std::string();
	}

	// =========================================================================================== search
	void computeHits()
	{
		mHits.clear();
		mCompletion.clear();
		const std::string q = toLower(trim(mQuery));
		if (q.empty()) return;

		std::vector<char> labelHit(mEntries.size(), 0);
		for (int i = 0; i < (int)mEntries.size(); ++i)
		{
			const Entry& e = mEntries[i];
			const int boost = (isFavourite(e.idName) ? 300 : 0) + (isRecent(e.idName) ? 150 : 0);
			if (auto m = matchText(q, e.labelLower))
			{
				labelHit[i] = 1;
				mHits.push_back(Hit{ i, m->score + boost + (e.depth == 1 ? 30 : 0) + (e.subCount > 0 ? 20 : 0), std::move(m->idx) });
				continue;
			}
			if (q.size() >= 2 && !e.aliasLower.empty())
			{
				bool found = false;
				for (size_t pos = e.aliasLower.find(q); pos != std::string::npos; pos = e.aliasLower.find(q, pos + 1))
					if (isWordStart(e.aliasLower, (int)pos)) { found = true; break; }
				if (found) { labelHit[i] = 1; mHits.push_back(Hit{ i, 900 + boost, {} }); continue; }
			}
		}
		// location and tooltip matches rank below every name match ("superbounce" also lists what lives under it)
		for (int i = 0; i < (int)mEntries.size(); ++i)
		{
			if (labelHit[i]) continue;
			const Entry& e = mEntries[i];
			if (q.size() >= 3)
			{
				const size_t pos = e.pathLower.find(q);
				if (pos != std::string::npos) { mHits.push_back(Hit{ i, 300 - (int)std::min<size_t>(pos, 200), {} }); continue; }
			}
			if (q.size() >= 4 && e.tipLower.find(q) != std::string::npos) mHits.push_back(Hit{ i, 60, {} });
		}

		std::stable_sort(mHits.begin(), mHits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
		if (mHits.size() > 80) mHits.resize(80);

		if (!mHits.empty())
		{
			const Entry& top = mEntries[mHits[0].entry];
			const std::string typed = toLower(mQuery);
			if (top.labelLower.size() > typed.size() && top.labelLower.compare(0, typed.size(), typed) == 0) mCompletion = top.label;
		}
	}

	static int inputCallback(ImGuiInputTextCallbackData* data)
	{
		auto* self = static_cast<Impl*>(data->UserData);
		if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
		{
			if (data->EventKey == ImGuiKey_UpArrow) self->mKeyNav = -1;
			else if (data->EventKey == ImGuiKey_DownArrow) self->mKeyNav = 1;
		}
		else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
		{
			self->mKeyTab = true;
		}
		else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
		{
			// Right at the very end of the text accepts the ghost completion. "Was already at the end last frame"
			// keeps a Right that merely ARRIVES at the end from completing.
			if (self->mClearQueryFrames > 0 && data->BufTextLen > 0) data->DeleteChars(0, data->BufTextLen);
			const bool atEnd = data->CursorPos == data->BufTextLen && !data->HasSelection();
			if (atEnd && self->mPrevCursorAtEnd && !self->mCompletion.empty() && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
			{
				data->DeleteChars(0, data->BufTextLen);
				data->InsertChars(0, self->mCompletion.c_str());
				data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
			}
			self->mPrevCursorAtEnd = atEnd;
		}
		return 0;
	}

	void releaseKeyboard()
	{
		mHotkeyDisableToken.reset();
		mBlockInputToken.reset();
	}

	void renderSearchBar(bool cursorShowing)
	{
		const bool hotkeyFocus = mFocusRequested.exchange(false, std::memory_order_acq_rel) && cursorShowing;
		const bool wantFocus = hotkeyFocus || (mRefocusSearch && cursorShowing);
		const bool keepKeyboardThisFrame = wantFocus;
		mRefocusSearch = false;
		if (wantFocus) ImGui::SetKeyboardFocusHere();

		mKeyNav = 0;
		mKeyTab = false;

		const std::string focusKey = mHotkeyRenderer->getBindingTextShort(RebindableHotkeyEnum::focusSearchHotkey);
		const std::string hint = std::format("Search {} options...   click here{} and type", mEntries.size(),
			focusKey.empty() ? std::string() : std::format(" or press {}", focusKey));

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 5.f));
		ImGui::SetNextItemWidth(-FLT_MIN);
		const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion
			| ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_EscapeClearsAll;
		const bool enterPressed = ImGui::InputTextWithHint("##hcmSearch", hint.c_str(), &mQuery, flags, &Impl::inputCallback, this);
		ImGui::PopStyleVar();
		const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
		mSearchActive = ImGui::IsItemActive();

		// The game has taken the mouse back (unpaused / menu closed): a text box that still owns the keyboard
		// would leave the player with blocked input and dead hotkeys and no visible reason. Let go.
		if (mSearchActive && !cursorShowing)
		{
			ImGui::ClearActiveID();
			mSearchActive = false;
		}

		// A tab was clicked while results were showing: empty the box. Done here, after the InputText call, for a
		// couple of frames, because a box that has just lost focus writes its text back on its next call.
		if (mClearQueryFrames > 0)
		{
			if (!mSearchActive) mQuery.clear();
			--mClearQueryFrames;
		}

		if (mQuery != mLastQuery)
		{
			mLastQuery = mQuery;
			computeHits();
			mSelected = 0;
			mExpandedEntry = -1;
			mShowActive = false;
		}

		// keys (the callbacks only fire while the box has the keyboard)
		if (!mHits.empty())
		{
			if (mKeyNav != 0)
			{
				mSelected = std::clamp(mSelected + mKeyNav, 0, (int)mHits.size() - 1);
				mScrollToSelected = true;
			}
			mSelected = std::clamp(mSelected, 0, (int)mHits.size() - 1);
			const int selectedEntry = mHits[mSelected].entry;
			if (mKeyTab) mExpandedEntry = (mExpandedEntry == selectedEntry) ? -1 : selectedEntry;
			if (enterPressed)
			{
				if (!activate(selectedEntry)) mExpandedEntry = (mExpandedEntry == selectedEntry) ? -1 : selectedEntry; // nothing to toggle: show it
			}
			if (mSearchActive && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
				mPendingFavouriteToggles.push_back(mEntries[selectedEntry].idName);
		}
		if (enterPressed && cursorShowing) mRefocusSearch = true; // Enter deactivates an InputText; stay in the box

		// While the box owns the keyboard: HCM hotkeys must not fire and the game must not see the keys. Same two
		// requests the HaloScript console takes while it is open (CommandConsoleManager).
		if (mSearchActive || mRefocusSearch || keepKeyboardThisFrame)
		{
			if (!mHotkeyDisableToken && mControl && mControl->hotkeyDisabler)
				mHotkeyDisableToken = mControl->hotkeyDisabler->makeScopedRequest();
			if (!mBlockInputToken && mControl && mControl->blockGameInputService.has_value())
			{
				try { mBlockInputToken = mControl->blockGameInputService.value()->makeScopedRequest(); }
				catch (...) { LOG_ONCE(PLOG_ERROR << "HCMPaletteGUI: could not request block-game-input while typing"); }
			}
		}
		else releaseKeyboard();

		// ghost completion + result count, drawn over the input frame
		if (!mQuery.empty())
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(rmin, rmax, true);
			const std::string count = std::format("{} result{}", mHits.size(), mHits.size() == 1 ? "" : "s");
			const float countWidth = ImGui::CalcTextSize(count.c_str()).x;
			dl->AddText(ImVec2(rmax.x - countWidth - 6.f, rmin.y + 5.f), ImGui::GetColorU32(kDim), count.c_str());
			if (!mHits.empty())
			{
				const float typedWidth = ImGui::CalcTextSize(mQuery.c_str()).x;
				const Entry& top = mEntries[mHits[0].entry];
				const std::string ghost = !mCompletion.empty() && mCompletion.size() >= mQuery.size()
					? mCompletion.substr(mQuery.size()) : ("   -> " + top.label);
				dl->PushClipRect(rmin, ImVec2(rmax.x - countWidth - 14.f, rmax.y), true);
				dl->AddText(ImVec2(rmin.x + 6.f + typedWidth, rmin.y + 5.f), ImGui::GetColorU32(ImVec4(0.66f, 0.54f, 0.56f, 1.f)), ghost.c_str());
				dl->PopClipRect();
			}
			dl->PopClipRect();
		}
	}

	// =========================================================================================== drawing bits
	void tooltipFor(const Entry& e)
	{
		if (!ImGui::IsItemHovered()) return;
		const std::string_view tip = e.node->element->getTooltipText();
		if (tip.empty()) return;
		if (ImGui::BeginTooltip())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.f);
			ImGui::TextUnformatted(tip.data(), tip.data() + tip.size());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	// checkbox / run-triangle / heading arrow / "value" marker, drawn only (the caller owns the click)
	void drawGlyph(ImDrawList* dl, ImVec2 p, float size, const Entry& e, bool on)
	{
		const auto kind = e.node->element->quickKind();
		const ImVec2 q(p.x + size, p.y + size);
		const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
		if (e.heading)
		{
			dl->AddTriangleFilled(ImVec2(p.x + size * 0.30f, p.y + size * 0.20f), ImVec2(p.x + size * 0.30f, p.y + size * 0.80f),
				ImVec2(p.x + size * 0.75f, p.y + size * 0.50f), ImGui::GetColorU32(kDim));
		}
		else if (kind == IGUIElement::QuickKind::Toggle)
		{
			dl->AddRectFilled(p, q, ImGui::GetColorU32(ImGuiCol_FrameBg));
			dl->AddRect(p, q, border);
			if (on)
			{
				const ImVec2 pts[3] = { ImVec2(p.x + size * 0.22f, p.y + size * 0.52f), ImVec2(p.x + size * 0.42f, p.y + size * 0.74f), ImVec2(p.x + size * 0.80f, p.y + size * 0.26f) };
				dl->AddPolyline(pts, 3, ImGui::GetColorU32(ImGuiCol_CheckMark), ImDrawFlags_None, 2.0f);
			}
		}
		else if (kind == IGUIElement::QuickKind::Button)
		{
			dl->AddRectFilled(p, q, ImGui::GetColorU32(ImGuiCol_Button));
			dl->AddRect(p, q, border);
			dl->AddTriangleFilled(ImVec2(p.x + size * 0.34f, p.y + size * 0.24f), ImVec2(p.x + size * 0.34f, p.y + size * 0.76f),
				ImVec2(p.x + size * 0.76f, p.y + size * 0.50f), ImGui::GetColorU32(ImGuiCol_Text));
		}
		else
		{
			dl->AddRect(p, q, border);
			const ImU32 c = ImGui::GetColorU32(kDim);
			dl->AddLine(ImVec2(p.x + size * 0.28f, p.y + size * 0.40f), ImVec2(p.x + size * 0.72f, p.y + size * 0.40f), c);
			dl->AddLine(ImVec2(p.x + size * 0.28f, p.y + size * 0.62f), ImVec2(p.x + size * 0.72f, p.y + size * 0.62f), c);
		}
	}

	// the favourite star: amber when it is one. Click queues the change for the end of the frame.
	void starButton(int entryIndex, float height)
	{
		const Entry& e = mEntries[entryIndex];
		const bool fav = isFavourite(e.idName);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 size(15.f, height);
		if (ImGui::InvisibleButton("##star", size)) mPendingFavouriteToggles.push_back(e.idName);
		const bool hovered = ImGui::IsItemHovered();
		if (hovered) ImGui::SetTooltip(fav ? "Remove from favourites" : "Add to favourites");
		drawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f), 6.5f,
			ImGui::GetColorU32(fav ? kAmber : (hovered ? kDim : kDim2)), fav);
	}

	bool drawnAlready(const GUIElementNode& n) const
	{
		if (n.element && mDrawn.contains(n.element.get())) return true;
		for (const auto& c : n.children) if (drawnAlready(c)) return true;
		return false;
	}
	void markDrawn(const GUIElementNode& n)
	{
		if (n.element) mDrawn.insert(n.element.get());
		for (const auto& c : n.children) markDrawn(c);
	}

	// One option drawn by its EXISTING widget, the way GUIHeading draws a child (small left margin, same line).
	// Skipped when that widget (or something inside it) is already on screen this frame.
	void renderElement(const GUIElementNode& node, float leftMargin)
	{
		if (drawnAlready(node)) return;
		markDrawn(node);
		ImGui::Dummy(ImVec2(leftMargin, node.element->getCurrentHeight() == 0.f ? 0.f : 1.f));
		ImGui::SameLine();
		const float y0 = ImGui::GetCursorPosY();
		node.element->render(*mHotkeyRenderer);
		if (ImGui::GetCursorPosY() <= y0 + 0.5f) ImGui::NewLine(); // it drew nothing: finish the line the margin started
	}

	// The tabbed card for an option listed in cardTable(). Returns false (and draws nothing) for every other option.
	bool renderCard(int entryIndex)
	{
		const Entry& host = mEntries[entryIndex];
		const auto found = cardTable().find(host.idName);
		if (found == cardTable().end()) return false;

		// resolve every tab first: which of its items exist in this game, and which nodes the tabs claim
		struct Item { const GUIElementNode* node; bool childrenOnly; };
		std::vector<std::vector<Item>> resolved(found->second.size());
		std::set<const GUIElementNode*> claimed;
		for (size_t t = 0; t < found->second.size(); ++t)
		{
			for (std::string_view item : found->second[t].items)
			{
				const bool childrenOnly = !item.empty() && item[0] == '*';
				if (childrenOnly) item.remove_prefix(1);
				const auto it = mEntryByName.find(std::string(item));
				if (it == mEntryByName.end()) continue;
				resolved[t].push_back(Item{ mEntries[it->second].node, childrenOnly });
				claimed.insert(mEntries[it->second].node);
			}
		}
		std::vector<const GUIElementNode*> unclaimed;
		for (const auto& child : host.node->children)
			if (child.element && !claimed.contains(&child) && !dynamic_cast<GUIInvisible*>(child.element.get())) unclaimed.push_back(&child);

		ImGui::PushID("card");
		ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.30f, 0.09f, 0.12f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
		ImGui::Dummy(ImVec2(12.f, 1.f));
		ImGui::SameLine();
		ImGui::BeginGroup();
		if (ImGui::BeginTabBar("##cardTabs", ImGuiTabBarFlags_None))
		{
			for (size_t t = 0; t < found->second.size(); ++t)
			{
				if (resolved[t].empty()) continue;
				if (ImGui::BeginTabItem(found->second[t].name))
				{
					ImGui::PopStyleColor(3); // the widgets inside keep their stock colours
					for (const auto& item : resolved[t])
					{
						if (item.childrenOnly)
							for (const auto& child : item.node->children) { ImGui::PushID(&child); renderElement(child, 2.f); ImGui::PopID(); }
						else { ImGui::PushID(item.node); renderElement(*item.node, 2.f); ImGui::PopID(); }
					}
					ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.30f, 0.09f, 0.12f, 1.f));
					ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
					ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
					ImGui::EndTabItem();
				}
			}
			if (!unclaimed.empty() && ImGui::BeginTabItem("Other"))
			{
				for (const auto* node : unclaimed) { ImGui::PushID(node); renderElement(*node, 2.f); ImGui::PopID(); }
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndGroup();
		ImGui::PopStyleColor(3);
		ImGui::PopID();
		return true;
	}

	// star + existing widget (+ its companion "... Settings" rows). Used by Favourites and Browse all.
	void renderLiveRow(int entryIndex, bool withCompanions)
	{
		const Entry& e = mEntries[entryIndex];
		const bool isCard = withCompanions && cardTable().contains(e.idName);
		if (!isCard && drawnAlready(*e.node)) return;   // already on screen inside another row
		ImGui::PushID(e.node);
		const float y0 = ImGui::GetCursorPosY();
		starButton(entryIndex, (float)GUIFrameHeight);
		ImGui::SameLine();
		if (withCompanions && cardTable().contains(e.idName))
		{
			// an option with a tabbed card: its own tick + name on one line, then the card
			IGUIElement* el = e.node->element.get();
			const bool on = el->quickKind() == IGUIElement::QuickKind::Toggle && el->quickIsOn();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float glyph = (float)GUIFrameHeight - 2.f;
			if (ImGui::InvisibleButton("##cardToggle", ImVec2(glyph, glyph))) activate(entryIndex);
			tooltipFor(e);
			drawGlyph(ImGui::GetWindowDrawList(), p, glyph, e, on);
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(e.label.c_str());
			const std::string hk = hotkeyText(e);
			if (!hk.empty()) { ImGui::SameLine(); ImGui::TextColored(kDim, "%s", hk.c_str()); }
			renderCard(entryIndex);
			ImGui::PopID();
			return;
		}
		markDrawn(*e.node);
		e.node->element->render(*mHotkeyRenderer);
		if (ImGui::GetCursorPosY() <= y0 + 0.5f) ImGui::NewLine();
		if (withCompanions)
			for (const auto* c : e.companions) renderElement(*c, 15.f + ImGui::GetStyle().ItemSpacing.x);
		ImGui::PopID();
	}

	// =========================================================================================== result rows
	// Two lines: [glyph] label (matched letters amber) ... state, hotkey, star  /  dim location.
	// Click the glyph = toggle / run. Click the row = select it and open / close its controls underneath.
	void renderResultRow(int entryIndex, const std::vector<int>* matched, bool selected, int rowIndex, bool selectable)
	{
		const Entry& e = mEntries[entryIndex];
		IGUIElement* el = e.node->element.get();
		const auto kind = el->quickKind();
		const bool on = kind == IGUIElement::QuickKind::Toggle && el->quickIsOn();
		const float lineH = ImGui::GetTextLineHeight();
		const float rowH = lineH * 2.f + 6.f;
		const float width = ImGui::GetContentRegionAvail().x;

		ImGui::PushID(rowIndex);
		const ImVec2 p = ImGui::GetCursorScreenPos();
		// the maroon row colours apply to this Selectable only, so an opened option's own widgets keep their look
		ImGui::PushStyleColor(ImGuiCol_Header, kSelBg);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSelBgHover);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelBg);
		ImGui::SetNextItemAllowOverlap();
		const bool rowClicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(width, rowH));
		ImGui::PopStyleColor(3);
		if (rowClicked)
		{
			if (selectable) mSelected = rowIndex;
			mExpandedEntry = (mExpandedEntry == entryIndex) ? -1 : entryIndex;
		}
		tooltipFor(e);
		if (selected && mScrollToSelected) { ImGui::SetScrollHereY(0.5f); mScrollToSelected = false; }
		const ImVec2 after = ImGui::GetCursorScreenPos();

		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (selected) dl->AddRectFilled(p, ImVec2(p.x + 2.f, p.y + rowH), ImGui::GetColorU32(kAmber));

		// glyph = the quick action
		const float glyph = lineH + 2.f;
		ImGui::SetCursorScreenPos(ImVec2(p.x + 6.f, p.y + 2.f));
		if (ImGui::InvisibleButton("##act", ImVec2(glyph, glyph)))
		{
			if (!activate(entryIndex)) mExpandedEntry = (mExpandedEntry == entryIndex) ? -1 : entryIndex;
		}
		drawGlyph(dl, ImVec2(p.x + 6.f, p.y + 2.f), glyph, e, on);

		// right cluster, laid out right to left
		float rx = p.x + width - 4.f;
		rx -= 15.f;
		ImGui::SetCursorScreenPos(ImVec2(rx, p.y + 1.f));
		starButton(entryIndex, glyph);
		const std::string hk = hotkeyText(e);
		if (!hk.empty())
		{
			rx -= ImGui::CalcTextSize(hk.c_str()).x + 10.f;
			dl->AddText(ImVec2(rx, p.y + 3.f), ImGui::GetColorU32(ImVec4(0.85f, 0.79f, 0.63f, 1.f)), hk.c_str());
		}
		const char* state = e.heading ? "" : (kind == IGUIElement::QuickKind::Toggle ? (on ? "ON" : "off") : (kind == IGUIElement::QuickKind::Button ? "run" : ""));
		if (state[0])
		{
			rx -= ImGui::CalcTextSize(state).x + 12.f;
			dl->AddText(ImVec2(rx, p.y + 3.f), ImGui::GetColorU32(on ? kAmber : kDim2), state);
		}

		// label + matched letters
		const float lx = p.x + 6.f + glyph + 7.f;
		dl->PushClipRect(ImVec2(lx, p.y), ImVec2(rx - 6.f, p.y + rowH), true);
		dl->AddText(ImVec2(lx, p.y + 3.f), ImGui::GetColorU32(ImGuiCol_Text), e.label.c_str());
		if (matched)
		{
			const char* text = e.label.c_str();
			const ImU32 amber = ImGui::GetColorU32(kAmber);
			for (const int i : *matched)
			{
				if (i < 0 || i >= (int)e.label.size()) continue;
				const float x = ImGui::CalcTextSize(text, text + i).x;
				dl->AddText(ImVec2(lx + x, p.y + 3.f), amber, text + i, text + i + 1);
			}
		}
		dl->PopClipRect();

		// line two: where it lives, and how much is under it
		std::string second = e.path;
		if (e.subCount > 0) second += std::format("   ({} setting{})", e.subCount, e.subCount == 1 ? "" : "s");
		dl->PushClipRect(ImVec2(lx, p.y), ImVec2(p.x + width - 4.f, p.y + rowH), true);
		dl->AddText(ImVec2(lx, p.y + 3.f + lineH + 1.f), ImGui::GetColorU32(kDim), second.c_str());
		dl->PopClipRect();

		ImGui::SetCursorScreenPos(after);

		if (mExpandedEntry == entryIndex)
		{
			// the option's own widget, children included, plus its settings block - in place
			ImGui::PushID("expanded");
			const ImVec2 a = ImGui::GetCursorScreenPos();
			if (drawnAlready(*e.node)) ImGui::TextColored(kDim, "   Its controls are already shown above.");
			else if (!renderCard(entryIndex))
			{
				renderElement(*e.node, 12.f);
				for (const auto* c : e.companions) renderElement(*c, 12.f);
			}
			const ImVec2 b = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 4.f, a.y), ImVec2(a.x + 4.f, b.y - 3.f), ImGui::GetColorU32(kAmber), 2.f);
			ImGui::Spacing();
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	void renderResults()
	{
		if (mHits.empty())
		{
			ImGui::Spacing();
			ImGui::TextColored(kDim, "No option matches that. Esc clears the box - or look in Browse all.");
			return;
		}
		for (int i = 0; i < (int)mHits.size(); ++i)
		{
			renderResultRow(mHits[i].entry, &mHits[i].idx, i == mSelected, i, true);
		}
	}

	void renderActive()
	{
		const auto active = activeEntries();
		ImGui::SeparatorText(std::format("Currently on ({})", active.size()).c_str());
		if (active.empty()) { ImGui::TextColored(kDim, "Nothing is switched on."); return; }
		ImGui::TextColored(kDim, "Click the tick to switch one off.");
		for (int i = 0; i < (int)active.size(); ++i) renderResultRow(active[i], nullptr, false, 10000 + i, false);
	}

	// =========================================================================================== Favourites
	void renderFavourites()
	{
		auto favourites = resolve(mFavourites);
		if (favourites.empty())
		{
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextColored(kDim, "No favourites in this game yet. Click the star next to any option in Browse all or in a search result, or press Ctrl+P on a search result.");
			ImGui::PopTextWrapPos();
		}
		// A favourite that sits inside another favourite (a setting of a starred option, or a starred option's own
		// "... Settings" block) is shown there, not a second time on a row of its own.
		{
			std::unordered_set<const GUIElementNode*> inside;
			const auto addInside = [&inside](const auto& self, const GUIElementNode& n, bool root) -> void
				{
					if (!root) inside.insert(&n);
					for (const auto& c : n.children) self(self, c, false);
				};
			for (const int i : favourites)
			{
				addInside(addInside, *mEntries[i].node, true);
				for (const auto* c : mEntries[i].companions) addInside(addInside, *c, false);
			}
			std::erase_if(favourites, [&](int i) { return inside.contains(mEntries[i].node); });
		}
		// grouped by where each one lives; order inside a group is the order they were starred
		std::stable_sort(favourites.begin(), favourites.end(), [this](int a, int b)
			{
				const Entry& x = mEntries[a]; const Entry& y = mEntries[b];
				return x.category != y.category ? x.category < y.category : x.path < y.path;
			});
		const std::string* lastPath = nullptr;
		for (const int i : favourites)
		{
			const Entry& e = mEntries[i];
			if (!lastPath || *lastPath != e.path) { ImGui::SeparatorText(e.path.c_str()); lastPath = &e.path; }
			renderLiveRow(i, true);
		}

		const auto active = activeEntries();
		ImGui::SeparatorText(std::format("Currently on ({})", active.size()).c_str());
		if (active.empty()) ImGui::TextColored(kDim, "Nothing is switched on.");
		for (int i = 0; i < (int)active.size(); ++i) renderResultRow(active[i], nullptr, false, 20000 + i, false);
	}

	// =========================================================================================== Browse all
	void renderCategory(const Category& c)
	{
		for (const auto* node : c.roots)
		{
			const auto found = mEntryByNode.find(node);
			if (found != mEntryByNode.end()) renderLiveRow(found->second, false); // siblings are all drawn anyway
			else { ImGui::PushID(node); renderElement(*node, 15.f + ImGui::GetStyle().ItemSpacing.x); ImGui::PopID(); }
		}
	}

	void renderBrowse()
	{
		if (mCategories.empty()) { ImGui::TextColored(kDim, "No options for this game."); return; }
		if (mBrowseCategory >= (int)mCategories.size()) mBrowseCategory = 0;

		// category chips, wrapped by hand
		const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		for (int i = -1; i < (int)mCategories.size(); ++i)
		{
			const std::string label = i < 0 ? std::format("All {}###all", mEntries.size())
				: std::format("{} {}###cat{}", mCategories[i].name, mCategories[i].optionCount, i);
			const float w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.f;
			if (i >= 0)
			{
				ImGui::SameLine();
				if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();
			}
			const bool selected = mBrowseCategory == i;
			if (selected) { ImGui::PushStyleColor(ImGuiCol_Button, kTileHover); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1)); }
			if (ImGui::Button(label.c_str())) mBrowseCategory = i;
			if (selected) ImGui::PopStyleColor(2);
		}
		ImGui::Spacing();

		if (mBrowseCategory < 0)
		{
			for (const auto& c : mCategories) { ImGui::SeparatorText(c.name.c_str()); ImGui::PushID(&c); renderCategory(c); ImGui::PopID(); }
		}
		else renderCategory(mCategories[mBrowseCategory]);
	}

	// =========================================================================================== frame
	HCMPaletteGUI::FrameResult render(GameState game, const std::function<void()>& renderStatusText, float maxWindowHeight, bool cursorShowing)
	{
		HCMPaletteGUI::FrameResult out;
		if (!mIndexedGame.has_value() || !(mIndexedGame.value() == game))
		{
			rebuildIndex(game);
			mIndexedGame = game;
			// Favourites when this game has any, Browse all for someone who has starred nothing yet
			if (!mTabPickedByUser) mPendingTab = resolve(mFavourites).empty() ? TabBrowse : TabFavourites;
		}

		const ImGuiStyle& style = ImGui::GetStyle();
		mDrawn.clear();

		// ---- status line: the existing header text, then the two buttons at the right
		renderStatusText();
		{
			const int activeCount = (int)activeEntries().size();
			const std::string activeLabel = std::format("{} active###hcmActive", activeCount);
			const float w1 = ImGui::CalcTextSize("Classic layout").x + style.FramePadding.x * 2.f;
			const float w2 = ImGui::CalcTextSize(activeLabel.c_str(), nullptr, true).x + style.FramePadding.x * 2.f;
			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w1 - w2 - style.ItemSpacing.x);
			if (ImGui::SmallButton("Classic layout")) out.switchToClassic = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the original HCM window (one long list).\nA \"New layout\" button there brings this one back.");
			ImGui::SameLine();
			if (activeCount > 0) ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
			const bool clicked = ImGui::SmallButton(activeLabel.c_str());
			if (activeCount > 0) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything that is switched on right now. Click to list it.");
			if (clicked) mShowActive = !mShowActive;
		}

		renderSearchBar(cursorShowing);
		const bool searching = !mQuery.empty();

		// ---- tabs. Typing shows results whatever the tab; picking a tab leaves the results.
		const int tabBefore = mTab;
		const bool tabForced = mPendingTab >= 0;
		ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.30f, 0.09f, 0.12f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.55f, 0.23f, 0.29f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kAmber);
		if (ImGui::BeginTabBar("##hcmViews", ImGuiTabBarFlags_None))
		{
			const int favouriteCount = (int)resolve(mFavourites).size();
			const std::string labels[2] = { std::format("Favourites ({})###hcmTabFav", favouriteCount), std::format("Browse all ({})###hcmTabBrowse", mEntries.size()) };
			for (int i = 0; i < 2; ++i)
			{
				const ImGuiTabItemFlags flags = (mPendingTab == i) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
				if (ImGui::BeginTabItem(labels[i].c_str(), nullptr, flags))
				{
					if (mPendingTab < 0 || mPendingTab == i) mTab = i;
					ImGui::EndTabItem();
				}
			}
			mPendingTab = -1;
			ImGui::EndTabBar();
		}
		ImGui::PopStyleColor(4);
		if (mTab != tabBefore && !tabForced) mTabPickedByUser = true;
		if (mTab != tabBefore && (searching || mShowActive))
		{
			// a tab was clicked while results were showing: go to the tab (the box is no longer active, so its
			// text can be cleared from here)
			mShowActive = false;
			mClearQueryFrames = 2;
			mExpandedEntry = -1;
		}

		// ---- body: scrolls on its own; as tall as its content up to what is left under the cap
		const float bodyMax = std::max(80.f, maxWindowHeight - mFixedHeight);
		const float bodyHeight = std::clamp(mBodyContentHeight, 30.f, bodyMax);
		ImGui::BeginChild("##hcmBody", ImVec2(0.f, bodyHeight), false, ImGuiWindowFlags_None);
		{
			if (!mQuery.empty()) renderResults();
			else if (mShowActive) renderActive();
			else if (mTab == TabFavourites) renderFavourites();
			else renderBrowse();
			mBodyContentHeight = ImGui::GetCursorPosY() + 2.f;
		}
		ImGui::EndChild();

		// ---- footer
		ImGui::Separator();
		if (!mQuery.empty())
			ImGui::TextColored(kDim, "Up/Down move  Enter toggle/run  Tab open  Right complete  Ctrl+P favourite  Esc clear");
		else if (mTab == TabBrowse && !mShowActive)
			ImGui::TextColored(kDim, "Every option of this game, by category. Star = favourite.");
		else if (mTab == TabFavourites && !mShowActive)
			ImGui::TextColored(kDim, "Your starred options with their full controls. Star again to remove.");
		else
			ImGui::TextColored(kDim, "Everything that is switched on. Click the tick to switch one off.");

		applyPendingFavouriteToggles();

		const float total = ImGui::GetCursorPosY() + style.WindowPadding.y;
		mFixedHeight = std::max(60.f, total - bodyHeight);
		out.desiredWindowHeight = total - bodyHeight + std::clamp(mBodyContentHeight, 30.f, bodyMax);
		return out;
	}

	void onWindowHidden()
	{
		mFocusRequested.store(false, std::memory_order_release);
		mRefocusSearch = false;
		mSearchActive = false;
		releaseKeyboard();
	}
};


HCMPaletteGUI::HCMPaletteGUI(std::shared_ptr<GUIElementStore> store, std::shared_ptr<HotkeyRenderer> hotkeyRenderer,
	std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<ControlServiceContainer> control)
	: pimpl(std::make_unique<Impl>(store, hotkeyRenderer, settings, control))
{
}

HCMPaletteGUI::~HCMPaletteGUI() = default;

HCMPaletteGUI::FrameResult HCMPaletteGUI::render(GameState game, const std::function<void()>& renderStatusText, float maxWindowHeight, bool cursorShowing)
{
	return pimpl->render(game, renderStatusText, maxWindowHeight, cursorShowing);
}

void HCMPaletteGUI::onWindowHidden()
{
	pimpl->onWindowHidden();
}
