#pragma once
#include "pch.h"

// ================================================================================================================
// Halo 2 CLASSIC (halo2.dll) multiplayer scoreboard data, for Competition Mode.
//
// ⚠⚠ TARGET IS halo2.dll = HALO 2 CLASSIC. md5 afd5c77177c0, 15,807,960 bytes, preferred base 0x180000000.
// NOT Halo 2 Anniversary and NOT the `groundhog` folder - those are a different engine entirely and none of
// these offsets apply there. HCM's GameState::Halo2MP (=4) is that other engine; this file is GameState::Halo2.
//
// Every offset below was derived from the shipped halo2.dll by disassembly, cross-checked by three independent
// passes and a consolidation pass, then re-derived from raw disassembly for the load-bearing three (name, team,
// team score). The RVAs quoted in comments are the functions the value was read out of, so any of this can be
// re-verified without redoing the search.
//
// ★ SELF-CHECK THAT THE BASE AND STRIDE ARE RIGHT: the engine's unit-object-datum sits at player+0x2C == 44,
//   which is exactly the `kPlayerBipedOff = 44` that H2ArmorColour.cpp already ships and relies on. Two
//   independently-derived features agreeing on that field is what pins the array base and the 548 stride.
//
// ⚠ A 548-byte stride means ANY offset >= 0x224 belongs to a different structure. The scores do NOT live in
//   the player entry - they are in "game engine globals", a separate allocation. Do not merge the two.
// ================================================================================================================

namespace H2Competition
{
	// ---- player array ---------------------------------------------------------------------------------------
	// base = *(uint64*)(halo2.dll + 0xE80A28 + 72) + (halo2.dll + 0xE80A28)
	// This is the same array H2ArmorColour.cpp walks; the +72 indirection and the self-relative add are the
	// engine's standard "data array" header, not something we invented.
	inline constexpr uintptr_t kPlayersCtxRVA = 0xE80A28;
	inline constexpr int       kPlayerStride  = 548;    // 0x224. Census over all 471 xrefs: highest byte touched
	                                                    // anywhere is +547, so the struct closes exactly.
	inline constexpr int       kMaxPlayers    = 16;

	inline constexpr int kOffSalt      = 0x000;  // int16.  0 == slot free. THE occupancy test.
	inline constexpr int kOffFlags     = 0x006;  // uint16. bit1 (0x2) == player has left; the engine's own
	                                             //         iterator sub_1806A9720 skips on it, so we do too.
	inline constexpr int kOffUnitDatum = 0x02C;  // uint32. 0xFFFFFFFF == dead/no biped. (== H2ArmorColour's 44)
	inline constexpr int kOffName      = 0x03C;  // wchar_t[32]. Proven from raw disasm, not decompilation:
	                                             //   1806AAB85 imul rdx,rcx,224h / add rdx,3Ch / add rdx,rbx
	                                             //   1806898D7 lea r9,[rsi+3Ch]   (scoreboard row emitter)
	inline constexpr int kNameCapacity = 32;
	inline constexpr int kOffTeam      = 0x0B8;  // int8. 0xFF == none, else 0..7. Read four times in
	                                             // scoreboard_build and once in the HUD; decisively, the score
	                                             // adder sub_180744730 reads it to pick which team to credit.

	// ---- game engine globals (SCORES LIVE HERE, NOT IN THE PLAYER ENTRY) ------------------------------------
	// geg = *(void**)(halo2.dll + 0xE80500).  The engine's own accessors take (geg + 836) as their base and
	// then add 4 / 1156; we fold those constants in, which is why the numbers below are 840 and 1992.
	//   sub_180744FD0: return *(uint16*)(a1 + 2*(stat + 36*player) + 4);      a1 = geg+836  ->  840 + 72*p
	//   sub_180745020: return *(uint16*)(a1 + 2*(stat + 12*team)   + 1156);   a1 = geg+836  -> 1992 + 24*t
	// Consistency check that these two agree: 4 + 16*72 == 1156 exactly, i.e. the team array begins precisely
	// where the 16-player array ends.
	inline constexpr uintptr_t kGameEngineGlobalsRVA = 0xE80500;

	inline constexpr int kPlayerStatBase   = 840;   // + 72*playerIndex + 2*stat
	inline constexpr int kPlayerStatStride = 72;
	inline constexpr int kTeamStatBase     = 1992;  // + 24*teamIndex   + 2*stat
	inline constexpr int kTeamStatStride   = 24;
	inline constexpr int kMaxTeams         = 8;     // sub_18068FEE0 hard-gates on index <= 7

	// Stat slot indices. These are NOT guesswork: the 5th argument of the stat adder sub_180744730 is an index
	// into the 54-entry statistic-NAME descriptor table at RVA 0xDF9D90 (stride 24), proven by its consumer
	// sub_180688480 indexing that table as word_180DF9D9C[12*a3] for clamp bounds. Dumping the table yields
	// 6='rounds-won' 7='kills' 8='assists' 9='deaths' 10='betrayals' 11='suicides', and each live slot is
	// written by an award site carrying its own name index. So every one of these is backed by an English
	// string in the binary rather than by inference.
	enum StatSlot : int
	{
		Stat_Score     = 0,   // sub_18068E220 (#local_player_score) and sub_18068E750 (#local_team_score)
		Stat_Kills     = 2,   // written with name-index 7
		Stat_Deaths    = 3,   // written with name-index 9
		Stat_Suicides  = 4,   // written with name-index 11
		Stat_Betrayals = 5,   // written with name-index 10
		Stat_Assists   = 6,   // written with name-index 8
		Stat_RoundsWon = 7,   // written with name-index 6
	};

	// ⚠⚠ TRAP: geg + 2192 also has a 24-byte stride and is indexed the same way, but it is PER-PLAYER respawn
	// data, not team scores. sub_18068FB10 clears it with `v3[24*playerIndex + 2192] = 0`. Do not confuse it
	// with the team stat array at 1992.

	inline constexpr int kOffTeamActiveMask = 0x0C;   // uint16 bitmask, bit t = team t is in play
	inline constexpr int kOffTeamExistsMask = 0x0E;   // uint16 bitmask, bit t = team t has a valid score.
	                                                  // THIS is the render gate: sub_18068E750 returns -1 for a
	                                                  // team whose bit is clear, so a clear bit means "do not
	                                                  // draw this team", NOT "score is zero".
	inline constexpr int kOffState          = 0x6C;   // int16, 1 = in progress
	inline constexpr int kOffRoundCounter   = 0x6E;   // int16
	inline constexpr int kOffGameStartTime  = 0x70;   // int32, game ticks
	inline constexpr int kOffRoundTimer     = 0xE0;   // int16 round time remaining; negatives clamp to 0
	inline constexpr int kOffGameOverState  = 0x103C; // int32: 1 = in progress, 2 = ended

	// ---- game variant --------------------------------------------------------------------------------------
	// variant = *(uint64*)(halo2.dll + 0xE80A78) + 716.
	//   sub_1806A6500: return qword_180E80A78 + 8;   sub_1806A8380: return sub_1806A6500() + 708;   -> +716
	// Field names below are the variant decoder's own parameter names (sub_1808909F0 reads each by name).
	inline constexpr uintptr_t kVariantCtxRVA = 0xE80A78;
	inline constexpr int       kVariantOffset = 716;

	inline constexpr int kOffGametype   = 0x44; // int32, "variant-game-engine-index"
	inline constexpr int kOffVarFlags   = 0x48; // uint32, "flags". bit0 = TEAM PLAY.
	inline constexpr int kOffScoreToWin = 0x50;
	inline constexpr int kOffTimeLimit  = 0x54;
	inline constexpr int kOffTeamScoring = 0xA4; // 0 = sum of players, 1 = min, 2 = max

	enum Gametype : int32_t
	{
		GT_None        = 0,
		GT_CTF         = 1,
		GT_Slayer      = 2,
		GT_Oddball     = 3,
		GT_KingOfHill  = 4,
		GT_Juggernaut  = 7,
		GT_Territories = 8,
		GT_Assault     = 9,
		GT_Medic       = 11,
	};

	// Whether the score renders as MM:SS rather than an integer. This is not a style choice - it mirrors the
	// engine's own formatter at RVA 0x68E630, whose complete body is:
	//     v4 = *(_DWORD*)(sub_1806A8380() + 68);                              // 68 == 0x44 == gametype
	//     if (((v4 - 3) & 0xFFFFFFFA) != 0 || v4 == 7) return swprintf(a2, L"%d", a1);
	//     else return format_as_time(a1, a2);
	// 0xFFFFFFFA == ~5, so (v4-3) may only have bits 0 and 2 set => v4 in {3,4,7,8}, minus the explicit v4==7.
	// Hence exactly Oddball(3), King of the Hill(4) and Territories(8) display as time.
	constexpr bool gametypeScoreIsTime(int32_t gametype)
	{
		return gametype == GT_Oddball || gametype == GT_KingOfHill || gametype == GT_Territories;
	}

	constexpr std::string_view gametypeName(int32_t gametype)
	{
		switch (gametype)
		{
		case GT_CTF:         return "Capture the Flag";
		case GT_Slayer:      return "Slayer";
		case GT_Oddball:     return "Oddball";
		case GT_KingOfHill:  return "King of the Hill";
		case GT_Juggernaut:  return "Juggernaut";
		case GT_Territories: return "Territories";
		case GT_Assault:     return "Assault";
		case GT_Medic:       return "Medic";
		default:             return "Unknown";
		}
	}

	// What the per-player number means for this gametype, for the column header.
	constexpr std::string_view scoreColumnLabel(int32_t gametype)
	{
		switch (gametype)
		{
		case GT_CTF:         return "CAPS";
		case GT_Assault:     return "BOMBS";
		case GT_Oddball:     return "TIME";
		case GT_KingOfHill:  return "TIME";
		case GT_Territories: return "TIME";
		default:             return "SCORE";
		}
	}

	// ---- snapshot types ------------------------------------------------------------------------------------
	struct PlayerRow
	{
		std::string name;        // converted from the engine's wchar_t[32]
		int8_t  team = -1;       // -1 == none
		int16_t score = 0;
		int16_t kills = 0;
		int16_t deaths = 0;
		int16_t assists = 0;
		bool    alive = false;   // unitObjectDatum != 0xFFFFFFFF
		int     playerIndex = 0; // slot index, for stable sorting
	};

	struct Snapshot
	{
		bool     valid = false;
		bool     teamPlay = false;      // variant flags bit0
		int32_t  gametype = GT_None;
		bool     scoreIsTime = false;
		int32_t  scoreToWin = 0;
		int16_t  roundTimeRemaining = 0;
		uint16_t teamExistsMask = 0;
		std::array<int16_t, kMaxTeams> teamScore{};
		std::vector<PlayerRow> players;
	};

	// Formats a score for display, honouring the engine's time-vs-integer rule.
	inline std::string formatScore(int16_t score, bool asTime)
	{
		if (!asTime) return std::to_string(score);
		int s = score < 0 ? 0 : (int)score;
		return std::format("{}:{:02}", s / 60, s % 60);
	}
}
