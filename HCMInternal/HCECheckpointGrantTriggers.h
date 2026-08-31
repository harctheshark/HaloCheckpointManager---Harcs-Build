#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include "HCESpeedrunTriggerNames.h"   // reuses its case-insensitive comparators

// ================================================================================================================
// Halo Campaign Evolved - trigger volumes whose entry GRANTS A CHECKPOINT.
//
// Derived MECHANICALLY from the shipped HaloScript (13 .hsc files extracted from the scenario tags), not from
// community documents. A volume is listed iff entering it causes a save, by one of four proven mechanisms:
//
//   1. same-script   (sleep_until ..(volume_test_* V)..)  ...  (game_save*)
//   2. vol-wrapper   (f_save_at_trigger V) / _2_triggers / _3_triggers  - V is the ARGUMENT; the library
//                    helper does (sleep_until (volume_test_players V) 1) (f_game_save).
//   3. cross-script  the gate wakes a script that saves - e.g. a30 routes almost everything through a block of
//                    (script dormant save_<name> (f_game_save)) stubs.
//   4. cinematic     the gate reaches f_play_cinematic / f_manual_cinematic_play / pseudo_cinematic. EVERY
//                    cinematic is a save->play->game_revert sandwich: cinematic_skip_start calls
//                    game_save_cinematic_skip before the movie. These checkpoints are real but unlisted
//                    anywhere in the level scripts.
//
// THE NOTE IS THE PRODUCT. An empty note means "entering this volume checkpoints you, full stop - on every
// difficulty, at any player count". A non-empty note is the extra thing that must ALSO be true, and is what the
// overlay prints top-left when the volume is hit.
//
// DIFFICULTY SEMANTICS - THE ONE TRAP THAT INVERTS THE ANSWER:
//   game_difficulty_get      CLAMPS easy to normal. Across the whole corpus it is compared against
//                            normal/heroic/legendary 87 times and against "easy" ZERO times.
//                            So (= (game_difficulty_get) "normal") is TRUE ON EASY *AND* NORMAL.
//   game_difficulty_get_real DOES distinguish easy (19 comparisons against it).
//   Only four volumes in the whole game have a difficulty-dependent checkpoint, and they are noted below with
//   the function each one actually used. NOTHING here is gated on co-op or player count - that was searched
//   for explicitly (game_is_cooperative / game_coop_player_count) and there are zero hits on any grant path.
//
// FLAT MAP, NOT PER-LEVEL - same argument as HCESpeedrunTriggerNames.h: only one level is ever loaded. This was
// verified rather than assumed: across all 373 grant names there are ZERO collisions between levels, so no name
// can pick up another mission's note.
//
// STALENESS: extracted from the build shipped 2026-08-17. HCE has no version resource, so if a future update
// renames volumes this list degrades silently into showing fewer grants - it can never invent one.
// ================================================================================================================

namespace HCECheckpointGrantTriggers
{
	using Hash  = HCESpeedrunTriggerNames::CaseInsensitiveHash;
	using Equal = HCESpeedrunTriggerNames::CaseInsensitiveEqual;

	// volume name -> condition note. EMPTY note == unconditional grant, nothing to display.
	inline const std::unordered_map<std::string, std::string_view, Hash, Equal>& all()
	{
		static const std::unordered_map<std::string, std::string_view, Hash, Equal> grants =
		{
			// ---- 01 The Pillar of Autumn : 21 grants (19 unconditional, 2 conditional) ----
			{ "tv_airlock_2_trigger_1", "" },
			{ "tv_boom_trigger_1", "" },
			{ "tv_bridge_cinematic", "" },
			{ "tv_bsp1_2", "" },
			{ "tv_bsp5_6", "" },
			{ "tv_cafeteria_trigger_1", "" },
			{ "tv_containment_1_trigger_1", "" },
			{ "tv_containment_2_trigger_1", "" },
			{ "tv_crossfire_trigger_1", "" },
			{ "tv_cryo_search_trigger_1", "" },
			{ "tv_cryo_tech_red_sq", "only after the look-calibration tutorial" },
			{ "tv_end_trigger", "" },
			{ "tv_final_trigger_11", "" },
			{ "tv_final_trigger_6", "" },
			{ "tv_flank_trigger_1", "" },
			{ "tv_knot_trigger_1", "" },
			{ "tv_lifepod_1_trigger_1", "" },
			{ "tv_loop_trigger_1", "" },
			{ "tv_motiontracker_1", "" },
			{ "tv_red_square", "only after the look-calibration tutorial" },
			{ "tv_tunnel_trigger_close", "" },

			// ---- 02 Halo : 14 grants (13 unconditional, 1 conditional) ----
			{ "after_lightbridge_trigger", "" },
			{ "cave_encounter_cp_trigger", "" },
			{ "cave_floor_exit", "" },
			{ "cliff_1", "" },
			{ "cliff_2", "" },
			{ "cliff_all", "only after the marines below are reached" },
			{ "cliff_arrival", "" },
			{ "cliff_inside_left_bottom", "" },
			{ "first_arrival", "" },
			{ "river_1", "" },
			{ "river_2", "" },
			{ "rubble_1", "" },
			{ "rubble_2", "" },
			{ "rubble_arrival", "" },

			// ---- 03 Truth and Reconciliation : 43 grants (26 unconditional, 17 conditional) ----
			{ "area1_save_trigger", "" },
			{ "area3_trigger", "" },
			{ "area3_trigger_a", "only once the area is cleared" },
			{ "area4_grunts_m_trigger", "only once the squad has taken losses" },
			{ "area4_marines_ledge_trigger", "only once the squad has taken losses" },
			{ "area4_marines_middle_trigger", "only once the squad has taken losses" },
			{ "area4_reins_trigger", "only once the area is cleared" },
			{ "area4_trigger", "" },
			{ "area5_dropship_trigger", "only once most of the enemies are dead" },
			{ "area5_trigger", "" },
			{ "canyon_nav_reached5", "only once the area is cleared" },
			{ "control_migration", "" },
			{ "gravity_trigger", "" },
			{ "hangar_control_approach_trigger", "only after the switch or door is operated" },
			{ "hangar_entrance", "only once the hangar fight has started" },
			{ "hangar_l_hall_trigger", "" },
			{ "hangar_left_hall_pillars", "" },
			{ "hangar_migration", "" },
			{ "hangar_second_reins_trigger", "only once the area is cleared" },
			{ "hangar_second_trigger_a", "" },
			{ "hangar_second_trigger_b", "" },
			{ "hangar_third_safe_a", "only once the area is cleared" },
			{ "hangar_third_trigger_a", "" },
			{ "hangar_third_trigger_c", "" },
			{ "hangar_third_trigger_d", "only once the area is cleared" },
			{ "hangar_trigger", "" },
			{ "mus_hangar_ledgehall", "" },
			{ "muster_bay_top_entrance", "" },
			{ "muster_floor_trigger", "" },
			{ "prison_after_reyes_saved_trigger", "only after the switch or door is operated" },
			{ "prison_control_to_prison_hallway_trigger", "" },
			{ "prison_entrance_follow_player_trigger", "only once the area is cleared" },
			{ "prison_save_trigger", "" },
			{ "prison_to_prison_control_trigger", "" },
			{ "prison_trigger", "" },
			{ "tv_cellblock_b", "" },
			{ "tv_control_main_door", "" },
			{ "tv_control_observation_door", "" },
			{ "tv_hangar_floor_1_wave_b", "Easy and Normal only" },
			{ "tv_hangar_floor_1_wave_d", "" },
			{ "tv_hangar_floor_1_wave_e", "Easy and Normal only" },
			{ "tv_hangar_floor_1_wave_f", "only once most of the enemies are dead" },
			{ "tv_lobby_exit", "" },

			// ---- 04 The Silent Cartographer : 17 grants (7 unconditional, 10 conditional) ----
			{ "beach_lz_base", "only once the area is cleared" },
			{ "shaft_b_wide_trigger", "only once the area is cleared" },
			{ "shafta_ante_inv_checkpoint", "" },
			{ "shafta_beach", "only after Shaft A is unlocked and the area is cleared" },
			{ "shafta_enable_locker_elite", "" },
			{ "shafta_inv_1", "only once the area is cleared" },
			{ "shafta_inv_3", "only once the area is cleared" },
			{ "shafta_inv_4", "" },
			{ "shafta_inv_5", "only once the area is cleared" },
			{ "shafta_ledge_trigger", "" },
			{ "shafta_mind", "" },
			{ "shafta_room_a2", "" },
			{ "shafta_switch", "only after the switch or door is operated" },
			{ "shafta_u_entry", "" },
			{ "shafta_window_inv", "only after Shaft A is unlocked and the area is cleared" },
			{ "valley_back", "only after the switch or door is operated" },
			{ "valley_entrance", "only once the area is cleared" },

			// ---- 05 Assault on the Control Room : 45 grants (37 unconditional, 8 conditional) ----
			{ "a2_bottom_save", "" },
			{ "a2_top_a_save", "" },
			{ "a2_top_b_save", "" },
			{ "a_bridge_save", "" },
			{ "b3_bottom_a_save", "" },
			{ "b3_bottom_b_save", "" },
			{ "b3_bridge_reins_trigger", "" },
			{ "b3_bridge_save", "" },
			{ "b3_top_save", "" },
			{ "b4_a_save", "" },
			{ "b4_b_trigger", "only once the area is cleared" },
			{ "b4_bridge_reins_trigger_a", "" },
			{ "b4_bridge_reins_trigger_b", "only once the area is cleared" },
			{ "b4_bridge_save", "" },
			{ "b5_a_save", "" },
			{ "b5_hall_trigger", "" },
			{ "c1_top_a_save", "" },
			{ "c_bridge_banshee_trigger", "only once the area is cleared" },
			{ "c_bridge_banshee_trigger_safety", "only once the area is cleared" },
			{ "c_bridge_save", "" },
			{ "control_trigger", "" },
			{ "control_trigger_save", "only once the area is cleared" },
			{ "crev_a_save", "" },
			{ "crev_b_save", "" },
			{ "crevasse_trigger", "" },
			{ "ext_a_area_c_trigger_a", "" },
			{ "ext_a_area_c_trigger_b", "" },
			{ "ext_a_save", "" },
			{ "ext_a_trigger", "" },
			{ "ext_b_a_trigger_a", "" },
			{ "ext_b_c_trigger_a", "only once the area is cleared" },
			{ "ext_b_c_trigger_d", "only once the area is cleared" },
			{ "game_win_trigger", "" },
			{ "tv_arena_d_cleanup", "" },
			{ "tv_arena_d_save", "" },
			{ "tv_arena_e_cleanup", "" },
			{ "tv_arena_f_cleanup", "" },
			{ "tv_checkpoint_bridge_a_pre-gold", "" },
			{ "tv_ext_b_b_trigger_spiral_entrance", "" },
			{ "tv_ext_b_b_trigger_spiral_exit", "" },
			{ "tv_player_past_bunker_trees", "only once the area is cleared" },
			{ "tv_wraith_engaged", "" },
			{ "tv_zig_trigger_player_past_1st_switchback", "" },
			{ "tv_zig_trigger_player_past_2nd_switchback", "" },
			{ "zig_migration_trigger", "" },

			// ---- 06 343 Guilty Spark : 34 grants (26 unconditional, 8 conditional) ----
			{ "bay_a_lab_a_bot_trigger", "" },
			{ "bay_b_trigger", "" },
			{ "control_table_cinematic_trigger", "" },
			{ "control_table_trigger", "" },
			{ "enc_swamp_a_trigger_01", "" },
			{ "enc_swamp_a_trigger_02", "only once the area is cleared" },
			{ "hall_d_bottom_trigger", "" },
			{ "int_a_trigger", "" },
			{ "int_a_trigger_elevator_a_arrival", "only after the switch or door is operated" },
			{ "int_b_bay_a_trigger", "" },
			{ "int_b_hall_a", "" },
			{ "int_b_hall_a_b", "only once the area is cleared" },
			{ "int_b_hall_b_trigger", "" },
			{ "int_b_hall_b_trigger_b", "" },
			{ "int_b_hall_e_continuousspawner_trigger2", "" },
			{ "int_b_hall_f_trigger", "" },
			{ "int_b_hall_f_trigger_b", "" },
			{ "int_b_hall_g_trigger", "" },
			{ "int_b_hall_g_trigger_b", "" },
			{ "int_b_lift_d_trigger", "" },
			{ "int_bay_d", "" },
			{ "int_lift_infector_trigger", "" },
			{ "int_lift_infector_trigger1", "only once the area is cleared" },
			{ "int_lift_top_placement_trigger", "only once the area is cleared" },
			{ "mission_start", "" },
			{ "swamp_b_trigger_1", "" },
			{ "swamp_b_trigger_4", "" },
			{ "swamp_b_trigger_final", "only after the last-wave dialogue finishes" },
			{ "tv_bay_e_bridgespawn", "only once the area is cleared" },
			{ "tv_cleanup_lift_d_shaft_entrance", "" },
			{ "tv_cleanup_post_broken_lightbridge_lab_entrance", "" },
			{ "tv_cleanup_pre_final_bay_bay_entrance", "" },
			{ "tv_cleanup_sterilizer_midpoint", "" },
			{ "tv_lift_a_bottom_entrance", "only after the switch or door is operated" },

			// ---- 07 The Library : 15 grants (13 unconditional, 2 conditional) ----
			{ "tv_act_343_floor_3_intro", "" },
			{ "tv_act_343_hallway_e", "" },
			{ "tv_act_343_small_room", "" },
			{ "tv_act_broken_door", "" },
			{ "tv_act_final_chapter", "" },
			{ "tv_act_hallway_b", "" },
			{ "tv_act_hallway_door_e_op", "only when 343 reaches the door" },
			{ "tv_act_large_room_b", "" },
			{ "tv_act_lift2", "" },
			{ "tv_act_lift3", "" },
			{ "tv_act_middle_entrance", "" },
			{ "tv_act_pre_tank_intro", "" },
			{ "tv_hallway_door_b_op", "only when 343 reaches the door" },
			{ "tv_lift_1_start", "" },
			{ "tv_vo_floor4_index", "" },

			// ---- 08 Two Betrayals : 37 grants (28 unconditional, 9 conditional) ----
			{ "tv_canyon3_spawn_land_trigger", "only while in the vehicle" },
			{ "tv_e3_trigger", "only after the switch or door is operated" },
			{ "tv_e50_a_trigger", "only while in the vehicle" },
			{ "tv_e52_a_trigger", "only once the area is cleared" },
			{ "tv_e61_a_trigger", "only after pulse generator 3 is down" },
			{ "tv_e62_a_trigger", "only while in the vehicle" },
			{ "tv_e8_trigger", "only after the conduits are destroyed" },
			{ "tv_extra_room_1_entrance", "only once the area is cleared" },
			{ "tv_save_checkpoint1", "" },
			{ "tv_save_checkpoint2", "" },
			{ "tv_save_checkpoint20", "" },
			{ "tv_save_checkpoint21", "" },
			{ "tv_save_checkpoint22", "" },
			{ "tv_save_checkpoint23", "" },
			{ "tv_save_checkpoint24", "" },
			{ "tv_save_checkpoint25", "" },
			{ "tv_save_checkpoint26", "" },
			{ "tv_save_checkpoint27", "" },
			{ "tv_save_checkpoint29", "" },
			{ "tv_save_checkpoint2a", "" },
			{ "tv_save_checkpoint3", "" },
			{ "tv_save_checkpoint30", "" },
			{ "tv_save_checkpoint31", "" },
			{ "tv_save_checkpoint32", "" },
			{ "tv_save_checkpoint33", "" },
			{ "tv_save_checkpoint33a", "" },
			{ "tv_save_checkpoint34", "" },
			{ "tv_save_checkpoint3a", "" },
			{ "tv_save_checkpoint5", "" },
			{ "tv_save_checkpoint6", "" },
			{ "tv_save_checkpoint7", "" },
			{ "tv_save_checkpoint8", "" },
			{ "tv_save_checkpoint9", "" },
			{ "tv_save_checkpoint_24b", "" },
			{ "tv_save_checkpoint_zigg_banshee", "only while in the vehicle" },
			{ "tv_save_checkpoint_zigg_banshee_ground", "" },
			{ "tv_save_checkpoint_zigg_halfway", "" },

			// ---- 09 Keyes : 49 grants (46 unconditional, 3 conditional) ----
			{ "0_to_1_transition_trigger", "" },
			{ "enc6_3_cin", "" },
			{ "enc7_2", "only while in the vehicle" },
			{ "enc7_2b", "only while in the vehicle" },
			{ "enc7_2c", "only while in the vehicle" },
			{ "jump_unsafe", "" },
			{ "save1_2", "" },
			{ "save1_5", "" },
			{ "save3_2", "" },
			{ "save3_3", "" },
			{ "save3_4", "" },
			{ "save3_5", "" },
			{ "save3_6", "" },
			{ "save3_7", "" },
			{ "save3_8", "" },
			{ "save4", "" },
			{ "save4_2", "" },
			{ "save4_3", "" },
			{ "save4_4", "" },
			{ "save4_5", "" },
			{ "save4_6", "" },
			{ "save4_7", "" },
			{ "save4_8", "" },
			{ "save4_9", "" },
			{ "save5", "" },
			{ "save5_2", "" },
			{ "save5_2b", "" },
			{ "save5_3", "" },
			{ "save5_3b", "" },
			{ "save5_4", "" },
			{ "save5_5", "" },
			{ "save5_6", "" },
			{ "save5_7", "" },
			{ "save6", "" },
			{ "save6_2", "" },
			{ "save7", "" },
			{ "save7_1", "" },
			{ "save7_2", "" },
			{ "save7_2b", "" },
			{ "save7_2c", "" },
			{ "save7_3", "" },
			{ "save7_5", "" },
			{ "save7_6", "" },
			{ "save7_6c", "" },
			{ "save_point3_1", "" },
			{ "save_point3_2", "" },
			{ "save_point4_1", "" },
			{ "save_point4_2", "" },
			{ "save_point5_1", "" },

			// ---- 10 The Maw : 27 grants (22 unconditional, 5 conditional) ----
			{ "tv_cinematic_bridge", "only after the bridge-entry dialogue finishes" },
			{ "tv_enc1_1", "" },
			{ "tv_enc1_2", "" },
			{ "tv_enc1_4", "" },
			{ "tv_enc2_1", "" },
			{ "tv_enc2_2", "Easy only; otherwise kill the Hunters first" },
			{ "tv_enc2_4", "only once most of the enemies are dead" },
			{ "tv_enc2_6", "" },
			{ "tv_enc2_6b", "only once the area is cleared" },
			{ "tv_enc2_7", "" },
			{ "tv_enc3_1", "" },
			{ "tv_enc3_2", "" },
			{ "tv_enc3_3", "" },
			{ "tv_enc3_4", "" },
			{ "tv_enc3_5", "" },
			{ "tv_enc3_5b", "" },
			{ "tv_enc3_6", "" },
			{ "tv_enc4_1", "" },
			{ "tv_enc4_3", "" },
			{ "tv_engine_room", "only after the engine exhausts are destroyed" },
			{ "tv_grand_finale", "" },
			{ "tv_section1", "" },
			{ "tv_section2", "" },
			{ "tv_section3", "" },
			{ "tv_section4", "" },
			{ "tv_section5", "" },
			{ "tv_section6", "" },

			// ---- E10 Boarding Action : 14 grants (9 unconditional, 5 conditional) ----
			{ "tv_act_dock_9_clock", "only once the area is cleared" },
			{ "tv_act_exit_hunters", "" },
			{ "tv_change_of_plans_start", "" },
			{ "tv_cinematic_four", "" },
			{ "tv_encounter_10", "only once the area is cleared" },
			{ "tv_encounter_12_5", "only once the area is cleared" },
			{ "tv_encounter_16", "" },
			{ "tv_encounter_18", "" },
			{ "tv_encounter_19", "only once the area is cleared" },
			{ "tv_fly_away_dropship_a", "" },
			{ "tv_johnson_final_encounter", "only once the area is cleared" },
			{ "tv_johnson_rr_2nd_sniper_post", "" },
			{ "tv_kill_encounter_17", "" },
			{ "tv_trigger_cine_two", "" },

			// ---- E20 bonus mission : 19 grants (7 unconditional, 12 conditional) ----
			{ "tv_arena", "" },
			{ "tv_arena_unblip", "only after the shipping-line objective ends" },
			{ "tv_armory", "only once the area is cleared" },
			{ "tv_bridge_encounter", "only once the area is cleared" },
			{ "tv_bridge_switch", "only once the area is cleared" },
			{ "tv_brute_ambush", "only once the area is cleared" },
			{ "tv_brute_cells", "only once the area is cleared" },
			{ "tv_control_exterior", "only once the area is cleared" },
			{ "tv_end_mission", "" },
			{ "tv_final_encounter", "only once the area is cleared" },
			{ "tv_final_encounter_save", "" },
			{ "tv_jackal_jungle", "only once the area is cleared" },
			{ "tv_marine_cells_save", "" },
			{ "tv_observation_vignette", "" },
			{ "tv_old_arena_reins", "only once the area is cleared" },
			{ "tv_prison_exterior_start", "only once the jackal jungle is cleared" },
			{ "tv_prison_room_bottom", "" },
			{ "tv_settlement_obj_swap", "only once the area is cleared" },
			{ "tv_vo_setup_make_peace", "" },

			// ---- E30 bonus mission : 38 grants (32 unconditional, 6 conditional) ----
			{ "tv_act_garden_1", "" },
			{ "tv_act_garden_2", "" },
			{ "tv_act_gravlift_redux", "" },
			{ "tv_act_hydro_entry_hall_2", "" },
			{ "tv_act_hydro_entry_redux", "only once the area is cleared" },
			{ "tv_act_john_overview_vista", "" },
			{ "tv_act_rr", "" },
			{ "tv_act_rr_approach", "only once the area is cleared" },
			{ "tv_beat1_e1_act_overview", "" },
			{ "tv_bridge_redux_wave_2", "" },
			{ "tv_control_encounter", "" },
			{ "tv_e3_vista_wave2", "" },
			{ "tv_e8_entry_rein", "only once the area is cleared" },
			{ "tv_end_beat_1", "only once the area is cleared" },
			{ "tv_end_beat_2", "" },
			{ "tv_end_garden_2", "" },
			{ "tv_end_garden_redux_2", "" },
			{ "tv_end_post_garden_high", "" },
			{ "tv_hydro_bridge_c", "only once the area is cleared" },
			{ "tv_hydro_bridge_redux_cp", "" },
			{ "tv_hydro_entry_0", "" },
			{ "tv_mid_garden_redux_2", "" },
			{ "tv_mid_rr_approach", "" },
			{ "tv_space_1_e1", "" },
			{ "tv_space_2_e8", "" },
			{ "tv_space_2_trigger", "" },
			{ "tv_space_3_trigger", "" },
			{ "tv_space_trans_to_1", "" },
			{ "tv_transit_e24_e25", "" },
			{ "tv_transit_e26", "" },
			{ "tv_transit_e27", "" },
			{ "tv_transit_e28_e29_left", "" },
			{ "tv_transit_e28_e29_right", "" },
			{ "tv_transit_e30", "only after the switch or door is operated" },
			{ "tv_transit_e31", "" },
			{ "tv_transit_e32", "" },
			{ "tv_transit_e33", "" },
			{ "tv_transit_e34_a", "" },

		};
		return grants;
	}

	struct Grant
	{
		bool isGrant = false;          // does entering this volume checkpoint you at all?
		std::string_view note;         // empty => unconditional, print nothing
		bool conditional() const { return isGrant && !note.empty(); }
	};

	// ONE lookup answers both questions HCM asks.
	inline Grant lookup(const std::string& volumeName)
	{
		const auto& m = all();
		auto it = m.find(volumeName);
		if (it == m.end()) return {};
		return { true, it->second };
	}

	inline bool isCheckpointGrantTrigger(const std::string& volumeName)
	{
		return all().find(volumeName) != all().end();
	}

	// Empty view for "no note" - both for an unconditional grant and for a volume that is not a grant.
	// Callers that must tell those apart should use lookup().
	inline std::string_view conditionNote(const std::string& volumeName)
	{
		auto it = all().find(volumeName);
		return it == all().end() ? std::string_view{} : it->second;
	}

	inline bool hasConditionNote(const std::string& volumeName)
	{
		return !conditionNote(volumeName).empty();
	}
}
