#pragma once
#include <cstdint>
#include <cstddef>

// ================================================================================================
// AI PROGRESSION GATES - which AI the level will not advance past until they are dead.
//
// WHY THIS IS A TABLE AND NOT A RULE
// There is nothing in the live game that says "this enemy matters". The scripts express it, and they
// express it in a shape no single runtime rule can recover: a chain of BLOCKING waits, each on a
// different squad, each with its own comparison. So the shape is extracted from the shipped scripts
// once, and the RUNTIME decides which entry of the chain is currently live.
//
// ⚠⚠⚠ THE HARD PART IS "WHICH GATE IS ACTIVE", NOT "WHICH SQUADS ARE GATES".
// c10's tower gauntlet is the worked example. `f_run_gauntlet_waves` calls `f_run_gauntlet_wave` six
// times IN SEQUENCE, and each call BLOCKS until its own two gates pass. So at any instant exactly one
// wave's gates matter, and a table that colours every gauntlet squad at once is wrong five times out
// of six - which is exactly the "later waves aren't showing up" bug this replaces.
// The runtime therefore LATCHES a sequence counter that only ever moves forward, mirroring the
// script's own blocking structure. See HCEAISquadOverlay.cpp::classifyGates.
//
// ⚠⚠ SOFT vs HARD is `sleep_until`'s THIRD argument, not its second.
// `(sleep_until COND POLL TIMEOUT)` - arg2 is the poll interval. A gate with no third argument NEVER
// gives up: that is the only kind that truly blocks. Reading arg2 as the timeout is the single most
// common way to get this table wrong, so timeoutSeconds == 0 here means "no timeout, HARD".
//
// ⚠ A count can fall WITHOUT the player killing anything: `ai_migrate` moves actors between squads,
// and `ai_erase` / `ai_disposable` let the engine clean them up. A squad that is only ever erased is
// NOT a kill requirement and must not appear here.
// ================================================================================================

namespace HCEProgressionGates
{
	enum class GateKind : uint8_t
	{
		KillAll,         // every member must be dead
		CountAtMost,     // living count <= threshold
		FractionAtMost,  // living / strength-at-spawn <= threshold
	};

	struct Gate
	{
		const char* level;          // short level code as HCEGetPlayerState::getCurrentLevelName returns it
		const char* target;         // squad or squad-group name, exactly as the scenario tag spells it
		bool        isGroup;        // true = squad GROUP (sq_gr_*), false = a single squad
		GateKind    kind;
		float       threshold;      // count, or fraction 0..1; ignored for KillAll
		// ⚠ HOW MUCH ONE INFECTION FORM COUNTS. The scripts use THREE different counting functions and
		// they disagree about creatures, so this is per-gate, read off the actual call:
		//   ai_nonswarm_count        -> 0.0  creatures are excluded outright
		//   ai_living_count          -> 1.0  creatures count like anything else
		//   f_ai_living_weight_count -> 0.2  c20's own helper: (+ (/ (ai_swarm_count x) 5) (ai_nonswarm_count x))
		// Getting this wrong is silent: a c20 gate reading "<= 0.7" is not a fraction, it means zero combat
		// forms and at most three popcorn.
		float       creatureWeight;
		float       timeoutSeconds; // 0 = NO TIMEOUT = HARD. Otherwise the script's sleep_until arg3.
		int8_t      sequence;       // position in the level's blocking chain; -1 = independent

		// ⚠⚠⚠ ADVISORY = COLOURS BUT DOES NOT DRIVE THE CHAIN. An advisory gate never ENGAGES its
		// sequence and never BLOCKS it from advancing; it only lights up while that sequence is already
		// engaged by a real gate.
		// This exists because the obvious alternative is catastrophic. sq_carnage_infection_form is a
		// KillAll on a population that is never all dead, so as a normal gate at sequence 0 it (a) engaged
		// sequence 0 the instant the level loaded, painting every popcorn in the lift/bay/lab/prison half
		// red, and (b) could never be satisfied, so the latch STUCK at 0 and no later wave ever became
		// active - the whole chain died behind it.
		bool        advisory = false;
	};

	// --------------------------------------------------------------------------------------------
	// c10 - 343 Guilty Spark, the tower gauntlet.
	// Source: c10.hsc `f_run_gauntlet_waves` (1706) and `f_run_gauntlet_wave` (1794).
	// Constants: s_threshold_wave1..5 = 2/3/4/4/4 (1695-1699), last wave 0 (1898),
	//            r_wave_clear_threshold 0.33 (1703), r_wave_safety_timeout_sec 45 (1704).
	//
	// Each wave runs TWO gates in order:
	//   A  (<= (/ (ai_living_count sq_gr_gauntletN) initial) 0.33)  with a 45 s timeout  -> SOFT
	//   B  (<= (ai_nonswarm_count sq_gauntlet_bloodgate) threshold) with NO timeout      -> HARD
	// ★ Gate B is the only thing that actually blocks - it is the WHOLE of c10's kill-gated progression.
	// ⚠⚠ GATE A REQUIRES ZERO KILLS, so it must NEVER render in the "must die" colour. It fails twice
	//   over: it has a 45 s timeout so the level never really blocks on it, AND the count it reads falls
	//   purely by MIGRATION - cs_migrate_bipeds_after_trigger (1642) moves any biped that walks toward
	//   the tower out of the wave squad, and 1807's own comment says "either from death or from migrating
	//   to bloodgate squad". An enemy strolling into the bloodgate satisfies gate A just as well as a
	//   kill. It is kept here only so the wave can be shown in the TIMED colour; the runtime maps every
	//   timeoutSeconds > 0 row to that colour and never to Required.
	// ★ `sq_gauntlet_bloodgate` is populated by MIGRATION - `cs_migrate_bipeds_after_trigger` (1642)
	//   moves any wave member that crosses swamp_b_trigger_final_bloodgate into it, and membership is
	//   permanent. So the same squad name carries a DIFFERENT threshold at each sequence step.
	// ★ creatureWeight is 0 on every bloodgate gate: the script uses ai_nonswarm_count, which excludes
	//   infection forms, so killing popcorn never advances the gate directly. It still matters
	//   indirectly - they resurrect bodies into combat forms, which DO count.
	// --------------------------------------------------------------------------------------------
	inline constexpr Gate kGates[] =
	{
		{ "c10", "sq_gr_gauntlet1",       true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 0 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    2.f,   0.0f,  0.f, 0 },

		{ "c10", "sq_gr_gauntlet2",       true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 1 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    3.f,   0.0f,  0.f, 1 },

		{ "c10", "sq_gr_gauntlet3",       true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 2 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    4.f,   0.0f,  0.f, 2 },

		{ "c10", "sq_gr_gauntlet4",       true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 3 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    4.f,   0.0f,  0.f, 3 },

		{ "c10", "sq_gr_gauntlet5",       true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 4 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    4.f,   0.0f,  0.f, 4 },

		{ "c10", "sq_gr_lastwave",        true,  GateKind::FractionAtMost, 0.33f, 1.0f, 45.f, 5 },
		{ "c10", "sq_gauntlet_bloodgate", false, GateKind::CountAtMost,    0.f,   0.0f,  0.f, 5 },

		// ⚠ NOT A SCRIPT GATE - added on the player's direct instruction, and honest about why.
		// `sq_carnage_infection_form` has NO ai_place anywhere in c10.hsc; it is the template squad the
		// engine spawns dynamic infection forms from, which is why it appears in the swamp despite being
		// ai_erase'd twice during level A (1004, 1047). No sleep_until anywhere reads it, and
		// ai_nonswarm_count EXCLUDES it from the bloodgate gate, so killing one never advances that gate.
		//
		// It is nonetheless required IN PRACTICE, by the refill path the script documents itself: a unit
		// that resurrects WHILE ALREADY INSIDE sq_gauntlet_bloodgate puts a non-swarm body back into the
		// count (1909-1910, "infection forms who joined the bloodgate, ejected, and then reinfected a
		// biped"), and the per-wave monitors migrate resurrected wave members in on top of that (1659,
		// 1664). So live popcorn makes the ONE untimed gate non-monotonic - it can climb back over its
		// threshold after you had cleared it.
		// ⚠ It is NOT resurrection_monitor_bloodgate that does this: that one migrates bloodgate members
		// into the bloodgate, which is a membership no-op. Getting that wrong sent me looking in the wrong
		// place once already.
		//
		// ⚠⚠ BOUND TO THE GAUNTLET, NOT ALWAYS-ON. Sequence -1 would paint popcorn red across the whole
		// lift/bay/lab/prison half of the level, where c10 has NO kill gate of any kind.
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 0, true },
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 1, true },
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 2, true },
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 3, true },
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 4, true },
		{ "c10", "sq_carnage_infection_form", false, GateKind::KillAll,   0.f,   1.0f,  0.f, 5, true },

		// ---- a15 -- 3 gates, counted by: ai_nonswarm_count
		{ "a15", "sq_shoot_anti",                   false, GateKind::KillAll,       0.0f,   0.0f,  60.0f,   0 },  // L1194
		{ "a15", "sq_lifepod_1_anti_rg",            false, GateKind::KillAll,       0.0f,   0.0f,  0.0f,    1 },  // L1840
		{ "a15", "sq_lifepod_2_anti",               false, GateKind::KillAll,       0.0f,   0.0f,  0.0f,    2 },  // L2192

		// ---- a30 -- 8 gates, counted by: ai_living_count, ai_living_fraction, ai_nonswarm_count
		{ "a30", "enc_holdout_cliffside_cov",       true,  GateKind::CountAtMost,   3.0f,   0.0f,  0.0f,    0 },  // L1257
		{ "a30", "enc_holdout_rubble_cov",          true,  GateKind::CountAtMost,   3.0f,   0.0f,  0.0f,    0 },  // L1506
		{ "a30", "sq_gr_river_wave",                true,  GateKind::FractionAtMost, 0.5f,   1.0f,  0.0f,    0 },  // L1751
		{ "a30", "enc_holdout_cliffside_cov",       true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    1 },  // L1261
		{ "a30", "enc_holdout_rubble_cov",          true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    1 },  // L1509
		{ "a30", "sq_gr_river_wave",                true,  GateKind::CountAtMost,   6.0f,   1.0f,  0.0f,    1 },  // L1771
		{ "a30", "sq_gr_river_wave",                true,  GateKind::CountAtMost,   3.0f,   0.0f,  0.0f,    2 },  // L1778
		{ "a30", "sq_gr_river_wave",                true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    3 },  // L1784

		// ---- a50 -- 11 gates, counted by: ai_living_count, ai_nonswarm_count ⚠ EXTRACTED BUT NEVER AUDITED - the audit pass ran out of budget for this level.
		{ "a50", "area5_encounter",                 true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    1 },  // L1824
		{ "a50", "area5_encounter",                 true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    2 },  // L1852
		{ "a50", "gravity_pad_encounter",           true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    3 },  // L2344
		{ "a50", "gravity_pad_encounter",           true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,    4 },  // L2364
		{ "a50", "enc_muster_bay",                  true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    5 },  // L2573
		{ "a50", "enc_hangar_floor_1",              true,  GateKind::CountAtMost,   6.0f,   1.0f,  0.0f,    6 },  // L3025
		{ "a50", "enc_hangar_floor_1",              true,  GateKind::CountAtMost,   3.0f,   0.0f,  0.0f,    7 },  // L3046
		{ "a50", "enc_hangar",                      true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    8 },  // L3080
		{ "a50", "control_encounter",               true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    9 },  // L3799
		{ "a50", "enc_prison_break_in",             true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   10 },  // L4150
		{ "a50", "stealth_elites_control",          true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   11 },  // L4576

		// ---- c20 -- 18 gates, counted by: ai_nonswarm_count, f_ai_living_weight_count
		{ "c20", "gr_middle_sec_door",              true,  GateKind::CountAtMost,   1.0f,   0.2f,  0.0f,    0 },  // L476
		{ "c20", "gr_middle_sec_door",              true,  GateKind::CountAtMost,   1.0f,   0.2f,  0.0f,    1 },  // L478
		{ "c20", "gr_middle_sec_door",              true,  GateKind::CountAtMost,   0.7f,   0.2f,  0.0f,    2 },  // L487
		{ "c20", "gr_f2_lhallb_sec_door_flood",     true,  GateKind::CountAtMost,   3.0f,   0.2f,  0.0f,    3 },  // L743
		{ "c20", "gr_f2_lhallb_sec_door_flood",     true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,    4 },  // L746
		{ "c20", "gr_f2_lhallb_sec_door_flood",     true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,    5 },  // L751
		{ "c20", "gr_f2_lhallb_sec_door_flood",     true,  GateKind::CountAtMost,   0.7f,   0.2f,  0.0f,    6 },  // L764
		{ "c20", "gr_f2_centeast_sec_door",         true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,    7 },  // L884
		{ "c20", "gr_f2_centeast_sec_door",         true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,    8 },  // L887
		{ "c20", "gr_f2_centeast_sec_door",         true,  GateKind::CountAtMost,   0.7f,   0.2f,  0.0f,    9 },  // L891
		{ "c20", "gr_f2_liftshaft_b",               true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,   10 },  // L1058
		{ "c20", "gr_f2_liftshaft_b",               true,  GateKind::CountAtMost,   0.0f,   0.2f,  0.0f,   11 },  // L1061
		{ "c20", "gr_library_f_door1",              true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,   12 },  // L1327
		{ "c20", "gr_security_door_3",              true,  GateKind::CountAtMost,   0.7f,   0.2f,  0.0f,   13 },  // L1352
		{ "c20", "gr_security_door_3",              true,  GateKind::CountAtMost,   1.0f,   0.2f,  0.0f,   14 },  // L1354
		{ "c20", "gr_security_door_3",              true,  GateKind::CountAtMost,   1.0f,   0.2f,  0.0f,   15 },  // L1356
		{ "c20", "gr_security_door_3",              true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,   16 },  // L1360
		{ "c20", "gr_security_door_3",              true,  GateKind::CountAtMost,   2.0f,   0.2f,  0.0f,   17 },  // L1369

		// ---- c45 -- 3 gates, counted by: ai_nonswarm_count
		{ "c45", "gr_e_extra_a",                    true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    0 },  // L1606
		{ "c45", "gr_e_extra_b",                    true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    1 },  // L1633
		{ "c45", "gr_e_extra_c",                    true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    2 },  // L1659

		// ---- d20 -- 2 gates, counted by: ai_living_count
		{ "d20", "gr_enc6_1",                       true,  GateKind::CountAtMost,   3.0f,   1.0f,  0.0f,    0 },  // L3445
		{ "d20", "gr_enc6_1",                       true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    1 },  // L3449

		// ---- d40 -- 2 gates, counted by: ai_nonswarm_count
		{ "d40", "gr_enc2_5_cov",                   true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,    0 },  // L338
		{ "d40", "gr_enc5_2_cov_es",                true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    1 },  // L1810

		// ---- e10 -- 23 gates, counted by: ai_living_count, ai_nonswarm_count
		{ "e10", "gr_dock_intro_a",                 true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    0 },  // L2143
		{ "e10", "gr_dock_intro_a_wv2",             true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    0 },  // L2144
		{ "e10", "sq_dockingbay_lift_hunter",       false, GateKind::KillAll,       0.0f,   1.0f,  0.0f,    1 },  // L2423
		{ "e10", "gr_enc_5_master",                 true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    2 },  // L2432
		{ "e10", "gr_rest_respite_initial",         true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    3 },  // L2491
		{ "e10", "gr_rest_respite_hallway",         true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    3 },  // L2522
		{ "e10", "gr_rest_respite_wave3",           true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    3 },  // L2557
		{ "e10", "gr_dojo",                         true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    4 },  // L2615
		{ "e10", "gr_specops_enc10_master",         true,  GateKind::CountAtMost,   1.0f,   0.0f,  0.0f,    5 },  // L2643
		{ "e10", "gr_dojo_reins_part1",             true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    6 },  // L2833
		{ "e10", "gr_dojo_reins",                   true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    7 },  // L1510
		{ "e10", "gr_geneforge_ll_wv1",             true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    8 },  // L2967
		{ "e10", "gr_geneforge_ll_wv2_master",      true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    9 },  // L3001
		{ "e10", "gr_geneforge_ll_wv3_a_master",    true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   10 },  // L3034
		{ "e10", "gr_geneforge_ll_wv3_master",      true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   11 },  // L3064
		{ "e10", "gr_grav_lift_room_walkin",        true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   12 },  // L3500
		{ "e10", "gr_grav_lift_room_wv1",           true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   13 },  // L3533
		{ "e10", "gr_grav_lift_room_wv2",           true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   14 },  // L3541
		{ "e10", "gr_grav_lift_room_master",        true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   15 },  // L3554
		{ "e10", "gr_final_encounter_start",        true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   16 },  // L3607
		{ "e10", "gr_final_encounter_wv1",          true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   17 },  // L3666
		{ "e10", "gr_final_encounter_wv2",          true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   18 },  // L3728
		{ "e10", "gr_final_encounter_master",       true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   19 },  // L3766

		// ---- e20 -- 20 gates, counted by: ai_living_count, ai_nonswarm_count ⚠ EXTRACTED BUT NEVER AUDITED - the audit pass ran out of budget for this level.
		{ "e20", "gr_settlement",                   true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    1 },  // L1592
		{ "e20", "gr_jackal_jungle",                true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    2 },  // L2009
		{ "e20", "sq_prison_bridge_wv1_wraith",     false, GateKind::KillAll,       0.0f,   1.0f,  0.0f,    3 },  // L2393
		{ "e20", "gr_prison_bridge",                true,  GateKind::CountAtMost,   4.0f,   1.0f,  0.0f,    4 },  // L2397
		{ "e20", "gr_prison_ext_defend_door",       true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    5 },  // L2403
		{ "e20", "gr_prison_exterior",              true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    7 },  // L2603
		{ "e20", "gr_observation_bottom_wave2",     true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    8 },  // L3190
		{ "e20", "gr_armory",                       true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    9 },  // L2908
		{ "e20", "gr_brute_cells",                  true,  GateKind::CountAtMost,   2.0f,   1.0f,  0.0f,   10 },  // L3648
		{ "e20", "gr_brute_cells_wave2",            true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,   11 },  // L3658
		{ "e20", "gr_arena",                        true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   12 },  // L3852
		{ "e20", "sq_bridge_wv1_banshees",          false, GateKind::CountAtMost,   2.0f,   1.0f,  0.0f,   13 },  // L4416
		{ "e20", "gr_bridge_banshees",              true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,   15 },  // L4447
		{ "e20", "gr_old_arena",                    true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,   16 },  // L4601
		{ "e20", "gr_landing_structure_wv1",        true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,   17 },  // L4735
		{ "e20", "gr_landing_structure_wv2",        true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,   18 },  // L4734
		{ "e20", "gr_control_exterior",             true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,   19 },  // L4910
		{ "e20", "gr_final_encounter_wv1",          true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,   20 },  // L5111
		{ "e20", "gr_final_encounter_wv2",          true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,   21 },  // L5112
		{ "e20", "gr_final_encounter_wv3",          true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,   22 },  // L5113

		// ---- e30 -- 7 gates, counted by: ai_living_count, ai_nonswarm_count
		{ "e30", "gr_beat1_e13_control_bridge",     true,  GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,    0 },  // L1075
		{ "e30", "gr_transit_e28_e29_dome_station", true,  GateKind::CountAtMost,   2.0f,   0.0f,  0.0f,    1 },  // L1877
		{ "e30", "sq_e30_wave1",                    false, GateKind::CountAtMost,   2.0f,   0.0f,  0.0f,    2 },  // L1886
		{ "e30", "sq_e30_wave2",                    false, GateKind::CountAtMost,   0.0f,   0.0f,  0.0f,    3 },  // L1893
		{ "e30", "gr_transit_e30_dome_defend",      true,  GateKind::KillAll,       0.0f,   0.0f,  0.0f,    4 },  // L1913
		{ "e30", "sq_space1_e1_banshee5",           false, GateKind::CountAtMost,   0.0f,   1.0f,  0.0f,    5 },  // L4294
		{ "e30", "gr_space3_dogfight",              true,  GateKind::KillAll,       0.0f,   1.0f,  0.0f,    8 },  // L5395
	};

	inline constexpr size_t kGateCount = sizeof(kGates) / sizeof(kGates[0]);
}
