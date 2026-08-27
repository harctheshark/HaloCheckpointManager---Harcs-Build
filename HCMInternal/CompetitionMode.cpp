#include "pch.h"
#include "CompetitionMode.h"
#include "H2CompetitionData.h"
#include "GameTickEventHook.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "IMCCStateHook.h"
#include "GlobalKill.h"
#include "RenderTextHelper.h"
#include "ScopedImFontScaler.h"
#include "IMakeOrGetCheat.h"
#include "DirectXRenderEvent.h"
#include "imgui.h"

using namespace H2Competition;

namespace
{
	// halo2.dll base, refreshed when the game state changes rather than per frame.
	std::atomic<uintptr_t> sHalo2Base{ 0 };

	template<typename T>
	inline bool readAt(uintptr_t address, T& out)
	{
		if (address < 0x10000) return false;
		out = *reinterpret_cast<T*>(address);
		return true;
	}

	// The engine stores gamertags as wchar_t[32] with no guarantee of a terminator in the last slot, so we
	// bound the scan at the declared capacity. Names are ASCII in practice; anything outside it is replaced
	// rather than dropped so a mangled read is visible instead of silently rendering an empty row.
	std::string readPlayerName(uintptr_t namePtr)
	{
		std::string result;
		result.reserve(kNameCapacity);
		for (int i = 0; i < kNameCapacity; ++i)
		{
			wchar_t wc = *reinterpret_cast<wchar_t*>(namePtr + (i * sizeof(wchar_t)));
			if (wc == L'\0') break;
			result.push_back((wc >= 0x20 && wc < 0x7F) ? static_cast<char>(wc) : '?');
		}
		return result;
	}

	// Reads one coherent snapshot of the match. Returns nullopt if we are not in a live game engine.
	std::optional<Snapshot> readSnapshot()
	{
		const uintptr_t base = sHalo2Base.load(std::memory_order_acquire);
		if (!base) return std::nullopt;

		// --- game variant: gametype and whether this is team play at all -------------------------------
		uintptr_t variantCtx = 0;
		if (!readAt(base + kVariantCtxRVA, variantCtx) || !variantCtx) return std::nullopt;
		const uintptr_t variant = variantCtx + kVariantOffset;

		Snapshot snap;
		int32_t gametype = 0, varFlags = 0;
		if (!readAt(variant + kOffGametype, gametype)) return std::nullopt;
		if (gametype == GT_None) return std::nullopt;   // not in a multiplayer engine
		readAt(variant + kOffVarFlags, varFlags);
		readAt(variant + kOffScoreToWin, snap.scoreToWin);

		snap.gametype = gametype;
		snap.scoreIsTime = gametypeScoreIsTime(gametype);
		snap.teamPlay = (varFlags & 1) != 0;   // variant flags bit0. This bit hard-gates BOTH of the engine's
		                                       // own team accessors, so it is exactly the right FFA/team switch.

		// --- game engine globals: scores ---------------------------------------------------------------
		uintptr_t geg = 0;
		if (!readAt(base + kGameEngineGlobalsRVA, geg) || !geg) return std::nullopt;

		readAt(geg + kOffTeamExistsMask, snap.teamExistsMask);
		readAt(geg + kOffRoundTimer, snap.roundTimeRemaining);
		if (snap.roundTimeRemaining < 0) snap.roundTimeRemaining = 0;   // engine clamps negatives too

		for (int t = 0; t < kMaxTeams; ++t)
			readAt(geg + kTeamStatBase + (kTeamStatStride * t) + (2 * Stat_Score), snap.teamScore[t]);

		// --- player array ------------------------------------------------------------------------------
		// ⚠⚠ THE TRAP: in the IDA-derived note `base = *(qword_180E80A28 + 72) + qword_180E80A28`, the token
		// qword_180E80A28 is the VALUE STORED AT that address, not the address itself. Using the address walks
		// unrelated memory - which reads as a full 16 slots of non-zero salt and garbage names. Dereference the
		// global first, exactly as H2ArmorColour::resolveMyEmblemObject does.
		uintptr_t ctx = 0;
		if (!readAt(base + kPlayersCtxRVA, ctx) || !ctx) return std::nullopt;
		if (IsBadReadPtr((void*)(ctx + 72), 8)) return std::nullopt;
		uintptr_t arrayRel = 0;
		if (!readAt(ctx + 72, arrayRel) || !arrayRel) return std::nullopt;
		const uintptr_t players = arrayRel + ctx;   // self-relative
		if (players < 0x10000 || IsBadReadPtr((void*)players, kPlayerStride)) return std::nullopt;

		for (int i = 0; i < kMaxPlayers; ++i)
		{
			const uintptr_t p = players + (static_cast<uintptr_t>(kPlayerStride) * i);

			int16_t salt = 0;
			if (!readAt(p + kOffSalt, salt) || salt == 0) continue;   // salt 0 == free slot

			uint16_t flags = 0;
			readAt(p + kOffFlags, flags);
			if (flags & 0x2) continue;   // player has left; the engine's own iterator skips these

			PlayerRow row;
			row.playerIndex = i;
			row.name = readPlayerName(p + kOffName);
			if (row.name.empty()) continue;

			int8_t team = -1;
			readAt(p + kOffTeam, team);
			row.team = (team == (int8_t)0xFF) ? -1 : team;

			uint32_t unit = 0xFFFFFFFF;
			readAt(p + kOffUnitDatum, unit);
			row.alive = (unit != 0xFFFFFFFF);

			const uintptr_t stats = geg + kPlayerStatBase + (static_cast<uintptr_t>(kPlayerStatStride) * i);
			readAt(stats + (2 * Stat_Score), row.score);
			readAt(stats + (2 * Stat_Kills), row.kills);
			readAt(stats + (2 * Stat_Deaths), row.deaths);
			readAt(stats + (2 * Stat_Assists), row.assists);

			snap.players.push_back(std::move(row));
		}

		// Highest score first, then by slot so equal scores do not flicker between frames.
		std::ranges::sort(snap.players, [](const PlayerRow& a, const PlayerRow& b)
			{
				if (a.score != b.score) return a.score > b.score;
				return a.playerIndex < b.playerIndex;
			});

		// Defensive: the variant's team-play bit is the engine's own switch, but if it reads false while the
		// players themselves carry more than one distinct team index, believe the players. Without this a
		// misread flag collapses a team match into a single free-for-all list, which is a much more confusing
		// failure than the reverse.
		if (!snap.teamPlay)
		{
			std::set<int8_t> distinct;
			for (const auto& p : snap.players)
				if (p.team >= 0) distinct.insert(p.team);
			if (distinct.size() > 1) snap.teamPlay = true;
		}

		snap.valid = true;
		return snap;
	}

	inline ImU32 toU32(const SimpleMath::Vector4& c) { return ImGui::ColorConvertFloat4ToU32({ c.x, c.y, c.z, c.w }); }
	inline ImU32 withAlpha(ImU32 col, float mul)
	{
		ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
		c.w *= mul;
		return ImGui::ColorConvertFloat4ToU32(c);
	}
}

template<GameState::Value gameT>
class CompetitionModeImpl : public CompetitionModeUntemplated
{
private:
	// Both callbacks are non-null ONLY while the feature is enabled and Halo 2 is the running game, so an
	// observer who leaves the overlay off pays nothing per frame and nothing per tick.
	std::unique_ptr<ScopedCallback<RenderEvent>> mRenderEventCallback;
	std::shared_ptr<RenderEvent> mRenderEvent;

	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickEventCallback;
	std::shared_ptr<ObservedEvent<GameTickEvent>> mGameTickEvent;

	ScopedCallback<ToggleEvent> competitionModeToggleEventCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;

	// Snapshot handoff. The tick thread writes, the render thread reads; sharing a plain struct across the two
	// is how you get a torn scoreboard, so it goes through a mutex-guarded copy.
	std::mutex mSnapshotLock;
	Snapshot mSnapshot;

	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	// Creates or tears down the per-frame and per-tick callbacks. Also the only place halo2.dll's base is
	// refreshed - it is stable for a session, but re-reading it on enable means a game restart cannot leave us
	// holding a stale module handle.
	void toggleCompetitionMode(bool enabled)
	{
		if (enabled)
		{
			sHalo2Base.store((uintptr_t)GetModuleHandleW(L"halo2.dll"), std::memory_order_release);
			if (!sHalo2Base.load(std::memory_order_acquire))
			{
				PLOG_ERROR << "CompetitionMode: halo2.dll not loaded, cannot enable";
				return;
			}

			// ObservedEvent hands back the ScopedCallback itself - it counts subscribers, so it must do the
			// subscribing. Constructing a ScopedCallback directly against it does not compile.
			if (!mGameTickEventCallback)
				mGameTickEventCallback = mGameTickEvent->subscribe([this](uint32_t tick) { onGameTick(tick); });

			if (!mRenderEventCallback)
				mRenderEventCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2 screenSize) { onRenderEvent(screenSize); });
		}
		else
		{
			PLOG_DEBUG << "disabling CompetitionMode for game: " << ((GameState)gameT).toString();
			mRenderEventCallback.reset();
			mGameTickEventCallback.reset();
			std::scoped_lock lock(mSnapshotLock);
			mSnapshot = Snapshot{};
		}
	}

	void onMCCStateChangedEvent(const MCCState& newState)
	{
		try
		{
			lockOrThrow(settingsWeak, settings);
			toggleCompetitionMode(newState.currentGameState == (GameState)gameT
				&& settings->competitionModeToggle->GetValue());
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
			toggleCompetitionMode(false);
		}
	}

	void onCompetitionModeToggleEvent(bool& newValue)
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			toggleCompetitionMode(mccStateHook->isGameCurrentlyPlaying((GameState)gameT) && newValue);
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
			toggleCompetitionMode(false);
		}
	}

	void onGameTick(uint32_t)
	{
		if (GlobalKill::isKillSet()) return;
		try
		{
			auto snap = readSnapshot();
			std::scoped_lock lock(mSnapshotLock);
			mSnapshot = snap.value_or(Snapshot{});
		}
		catch (...)
		{
			std::scoped_lock lock(mSnapshotLock);
			mSnapshot = Snapshot{};
		}
	}

	// Per-side layout knobs, resolved once so left and right can differ in every respect.
	struct SideStyle
	{
		SimpleMath::Vector2 offset;
		float panelWidth = 420.f;
		float fontSize = 20.f;
		ImU32 accent = 0;
	};

	// Measures a string exactly the way RenderTextHelper draws it. This has to push the SAME scaled font:
	// ScopedImFontScaler sets pFont->Scale = size/16 and pushes it, so CalcTextSize is only meaningful INSIDE
	// that scope. Measuring outside and scaling by a ratio measures the wrong font altogether - which is what
	// made the title overflow its band and the stat columns pack together.
	static ImVec2 measure(const std::string& text, float fontSize)
	{
		ScopedImFontScaler scaler{ fontSize };
		return ImGui::CalcTextSize(text.c_str());
	}

	// Draws one panel. `rightSide` only chooses which screen edge the panel is anchored to - the INTERNAL
	// layout is identical on both sides (accent bar left, name left, stats right), so the two panels read as
	// the same component twice rather than a mirrored pair. `force` draws the panel even when the team has no
	// players, which is how you lay the overlay out before a match fills up.
	void drawPanel(const Snapshot& snap, int teamIndex, bool rightSide, bool force, const SideStyle& style,
		const SimpleMath::Vector2& screenSize, const std::shared_ptr<SettingsStateAndEvents>& settings)
	{
		const float scale = settings->competitionModeScale->GetValue();
		const float font = style.fontSize * scale;
		const float titleFont = font * 1.35f;
		const float smallFont = font * 0.72f;
		const float pad = font * 0.55f;
		const float rowH = font * 1.45f;
		const float panelW = style.panelWidth * scale;

		const bool showKD = settings->competitionModeShowKD->GetValue();
		const bool showRatio = settings->competitionModeShowKDRatio->GetValue();   // independent of Show K/D
		const bool showHeaders = settings->competitionModeShowColumnHeaders->GetValue();
		const bool outline = settings->competitionModeOutlineText->GetValue();

		std::vector<const PlayerRow*> rows;
		for (const auto& p : snap.players)
			if (!snap.teamPlay || p.team == teamIndex) rows.push_back(&p);
		if (snap.teamPlay && rows.empty() && !force) return;

		// ---- columns, in the order SCORE -> K/D -> RATIO ----------------------------------------------
		// Each column is as wide as the WIDER of its header and its widest plausible value. Sizing on the
		// values alone is what let "KILLS/DEATHS" run into the next column, since the headers are far wider
		// than the numbers under them. Samples (not live values) keep the columns from twitching per frame.
		struct Column { std::string header; std::string sample; float w = 0.f; float right = 0.f; };
		std::vector<Column> cols;
		cols.push_back({ std::string(scoreColumnLabel(snap.gametype)), snap.scoreIsTime ? "00:00" : "00000" });
		if (showKD)    cols.push_back({ "K/D",   "00/00" });
		if (showRatio) cols.push_back({ "RATIO", "00.00" });

		const float gap = font * 1.15f;   // clear air between SCORE / K/D / RATIO
		float statsW = 0.f;
		for (auto& c : cols)
		{
			c.w = (std::max)(measure(c.header, smallFont).x, measure(c.sample, font).x);
			statsW += c.w;
		}
		if (cols.size() > 1) statsW += gap * (cols.size() - 1);

		// Band height is measured, with headroom - the glyph box RenderTextHelper paints runs slightly taller
		// than CalcTextSize reports, which is what let the title spill out of the coloured strip.
		const float titleTextH = measure("Ag", titleFont).y;
		const float titleH = titleTextH * 1.25f + pad * 1.6f;
		const float headerH = showHeaders ? (measure("Ag", smallFont).y * 1.2f + pad * 0.5f) : 0.f;
		const float bodyH = rowH * (std::max)((size_t)1, rows.size()) + pad;
		const float totalH = titleH + headerH + bodyH;

		const float x0 = rightSide ? (screenSize.x - style.offset.x - panelW) : style.offset.x;
		const float y0 = style.offset.y;
		const float x1 = x0 + panelW;

		auto* dl = ImGui::GetBackgroundDrawList();
		const ImU32 bg = toU32(settings->competitionModeBackgroundColour->GetValue());
		const ImU32 text = toU32(settings->competitionModeTextColour->GetValue());
		const ImU32 dim = withAlpha(text, 0.55f);
		const float round = font * 0.25f;

		dl->AddRectFilled({ x0, y0 }, { x1, y0 + totalH }, bg, round);
		dl->AddRectFilled({ x0, y0 }, { x1, y0 + titleH }, withAlpha(style.accent, 0.35f), round);
		const float accentW = font * 0.2f;
		dl->AddRectFilled({ x0, y0 }, { x0 + accentW, y0 + totalH }, style.accent);   // always the left edge

		const float inset = accentW + pad;
		auto draw = [&](const std::string& str, float xAnchor, float y, float size, ImU32 col, bool rightAlign)
			{
				const float w = measure(str, size).x;
				const float x = rightAlign ? (xAnchor - w) : xAnchor;
				if (outline) RenderTextHelper::drawOutlinedText(str, { x, y }, col, size);
				else         RenderTextHelper::drawText(str, { x, y }, col, size);
			};

		// Lay the stat block out from the inner edge; columns then run in declaration order.
		float cx = x1 - inset - statsW;
		for (auto& c : cols) { c.right = cx + c.w; cx += c.w + gap; }
		const float nameAnchor = x0 + inset;

		// ---- title band ----
		const char* title = snap.teamPlay ? (teamIndex == 0 ? "RED" : "BLUE") : "PLAYERS";
		const std::string total = snap.teamPlay
			? formatScore(snap.teamScore[teamIndex], snap.scoreIsTime)
			: std::string(gametypeName(snap.gametype));
		const float titleY = y0 + (titleH - titleTextH) * 0.5f;
		draw(title, x0 + inset, titleY, titleFont, text, false);
		draw(total, x1 - inset, titleY, titleFont, style.accent, true);

		// ---- column headers ----
		if (showHeaders)
		{
			const float hy = y0 + titleH + pad * 0.15f;
			draw("PLAYER", nameAnchor, hy, smallFont, dim, false);
			for (const auto& c : cols) draw(c.header, c.right, hy, smallFont, dim, true);
		}

		// ---- player rows ----
		float y = y0 + titleH + headerH + pad * 0.4f;
		for (const auto* p : rows)
		{
			// Dead players dim rather than vanish, so the row order stays stable on screen.
			const ImU32 col = p->alive ? text : withAlpha(text, 0.45f);
			draw(p->name, nameAnchor, y, font, col, false);

			size_t ci = 0;
			draw(formatScore(p->score, snap.scoreIsTime), cols[ci++].right, y, font, col, true);
			if (showKD)
				draw(std::format("{}/{}", p->kills, p->deaths), cols[ci++].right, y, font, dim, true);
			if (showRatio)
			{
				const float ratio = (p->deaths > 0) ? (float)p->kills / (float)p->deaths : (float)p->kills;
				draw(std::format("{:.2f}", ratio), cols[ci++].right, y, font, dim, true);
			}
			y += rowH;
		}
	}

	SideStyle styleFor(bool right, const std::shared_ptr<SettingsStateAndEvents>& settings)
	{
		SideStyle st;
		if (right)
		{
			st.offset = settings->competitionModeRightOffset->GetValue();
			st.panelWidth = settings->competitionModeRightPanelWidth->GetValue();
			st.fontSize = settings->competitionModeRightFontSize->GetValue();
			st.accent = toU32(settings->competitionModeRightColour->GetValue());
		}
		else
		{
			st.offset = settings->competitionModeLeftOffset->GetValue();
			st.panelWidth = settings->competitionModeLeftPanelWidth->GetValue();
			st.fontSize = settings->competitionModeLeftFontSize->GetValue();
			st.accent = toU32(settings->competitionModeLeftColour->GetValue());
		}
		return st;
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		try
		{
			lockOrThrow(settingsWeak, settings);
			// No toggle check here on purpose: this callback only exists while the feature is on and Halo 2 is
			// the running game (see toggleCompetitionMode).

			Snapshot snap;
			{
				std::scoped_lock lock(mSnapshotLock);
				snap = mSnapshot;
			}
			if (!snap.valid) return;

			const bool forceBoth = settings->competitionModeForceBothPanels->GetValue();
			if (snap.teamPlay || forceBoth)
			{
				// Draw a team only if the engine itself considers it scoreable - a clear bit in the mask means
				// "no such team", not "score is zero" (the engine's getter returns -1 for those). The force
				// toggle bypasses that so both sides can be positioned before a match fills up.
				if ((snap.teamExistsMask & (1 << 0)) || forceBoth)
					drawPanel(snap, 0, false, forceBoth, styleFor(false, settings), screenSize, settings);
				if ((snap.teamExistsMask & (1 << 1)) || forceBoth)
					drawPanel(snap, 1, true, forceBoth, styleFor(true, settings), screenSize, settings);
			}
			else
			{
				// Free-for-all: a single list, on the left, so the observer's muscle memory holds.
				drawPanel(snap, 0, false, false, styleFor(false, settings), screenSize, settings);
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	CompetitionModeImpl(GameState game, IDIContainer& dicon)
		: runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		competitionModeToggleEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->competitionModeToggle->valueChangedEvent,
			[this](bool& n) { onCompetitionModeToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& n) { onMCCStateChangedEvent(n); }),
		mRenderEvent(dicon.Resolve<RenderEvent>())
	{
		auto gameTickEventHook = resolveDependentCheat(GameTickEventHook);
		mGameTickEvent = gameTickEventHook->getGameTickEvent();
	}

	~CompetitionModeImpl()
	{
		PLOG_DEBUG << "~CompetitionModeImpl";
	}
};

CompetitionMode::CompetitionMode(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<CompetitionModeImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException("CompetitionMode is Halo 2 Classic only");
	}
}

CompetitionMode::~CompetitionMode() = default;
