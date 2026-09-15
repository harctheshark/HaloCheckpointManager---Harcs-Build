#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <string_view>

// ================================================================================================================
// Halo 5 - the trigger volumes that a SPEEDRUN actually has to touch, per level.
//
// A Halo 5 mission is a linear chain of goals. Each goal table in the level script declares exactly ONE
// `gotoVolume`, and entering that volume is the whole advance condition to the goal named in `next`. The long
// SleepUntil chains inside each goal's Start() are narrative and encounter sequencing running on their own
// thread - they gate DOORS and CINEMATICS, but they are not part of the advance. So the minimum completion path
// for a level is precisely: its ordered list of gotoVolumes, plus whatever volume the final goal ends on.
//
// That is what this table holds. It is read out of the decompiled `ucsh` script tags, not guessed, and every
// name here was confirmed present in that level's own scenario tag string table.
//
// ⚠ NOT EVERY GOAL ADVANCES ON A VOLUME. Across the 15 mapped levels the engine exposes FOUR advance
// mechanisms - gotoVolume, GoalCompleteCurrent(), GoalCompleteTask(<goal>) and GoalComplete(<goal>) - none of
// which are defined anywhere in the 112-script corpus, so all four are engine-side. Only the first is a
// trigger volume and therefore only the first can appear here. A goal that advances on a Warden kill or a
// script flag has NO row in this table, so a level's rows are a SUBSET of its minimum path, not all of it.
// The per-level .md in H5_Speedrun_Notes carries the full path including the non-volume steps.
//
// ⚠ GENERATED from H5_Speedrun_Notes/data/*.json by scratchpad/gen_speedrun_header.py. Every row below was
// re-hashed from its name and confirmed present in that level's own scenario tag string table before being
// written; the generator refuses to emit a header if any row fails either check.
// ================================================================================================================
namespace H5SpeedrunTriggers
{
	struct Entry
	{
		uint32_t hash;
		const char* name;
		const char* level;     // the scenario this volume belongs to
		const char* mission;   // in-game mission name, "unidentified" where it could not be established
		int step;              // 1-based position in that level's completion path
		const char* kind;      // WHY this volume matters - see the list below
		const char* toGoal;    // goal this advances to; empty for the end trigger
		const char* zoneSet;   // zone set entered, empty when unchanged
	};

	// `kind` records WHY the volume matters - they are not all the same thing:
	//   gotoVolume        the goal table's own advance volume (79 of them - the common case)
	//   endMission        entering it leads to EndMission()
	//   endMissionChain   one link of a multi-volume chain that ends the mission
	//   goalComplete / goalCompleteCurrent / goalCompleteTask
	//                     the goal advances by an ENGINE CALL, and this volume is what gates that call
	//   deviceUse         the step is a device interaction; this volume is where it happens
	//
	// Sorted by hash so lookup is a binary search. A volume name is unique to its scenario, so a flat
	// table needs no level detection; the two names shared by w2_campsite and w2_campsite_return are
	// the same geography visited twice and appear once.
	inline constexpr std::array<Entry, 100> kEntries{ {
		{ 0x0CFD003Du, "tv_end_underbelly_1", "w2_tsunami", "Battle of Sunaion", 6, "gotoVolume", "goal_underbelly_2", "w2_tsunami_underbelly" },
		{ 0x1204414Cu, "tv_ending_start", "w3_citadel", "The Breaking", 9, "endMission", "", "" },
		{ 0x15429B99u, "tv_end_missionstart", "w2_tsunami", "Battle of Sunaion", 1, "gotoVolume", "goal_burgertown", "w2_tsunami_burgertown" },
		{ 0x155F1E7Au, "tv_snapshut", "w2_plateau", "Alliance", 8, "gotoVolume", "goal_Temp_Bypass", "plateau_bowl" },
		{ 0x17BE60E0u, "tv_goal_coliseum", "w3_innerworld", "Guardians", 4, "gotoVolume", "goal_coliseum", "zn_factory" },
		{ 0x198E5EF5u, "tv_end_tangletown", "w2_tsunami", "Battle of Sunaion", 3, "gotoVolume", "goal_turrettown", "w2_tsunami_turrettown" },
		{ 0x1B3699F7u, "tv_goal_temple2", "w3_builder", "Reunion", 3, "gotoVolume", "goal_temple2", "zn_marsh" },
		{ 0x235F8BAEu, "tv_goal_09_vault", "w3_halsey", "Osiris", 8, "gotoVolume", "goal_halsey_vault", "zn_vault" },
		{ 0x2789B21Fu, "tv_goal_docks", "w3_builder", "Reunion", 6, "gotoVolume", "goal_docks", "zn_docks" },
		{ 0x29D40D95u, "tv_walkway_stage4", "w3_innerworld", "Guardians", 9, "endMissionChain", "", "zn_end" },
		{ 0x2A4D0CB9u, "tv_goal_08_cavalier", "w3_arrival", "Genesis", 6, "gotoVolume", "goal_arrival_cavalier", "zn_gateway" },
		{ 0x2B420311u, "tv_goal_falls", "w2_grotto", "Swords of Sanghelios", 2, "gotoVolume", "goal_sinkhole", "w2_grotto_falls" },
		{ 0x3149ABA4u, "tv_drygrannis_arrival", "w1_meridian", "Meridian Station", 5, "gotoVolume", "goal_drygrannis_arrival", "zs_040" },
		{ 0x3A6BE115u, "tv_entered_temple", "w2_plateau", "Alliance", 7, "goalCompleteTask", "goal_Tomb", "plateau_bowl" },
		{ 0x3C12658Eu, "tv_goal_03_steps", "w3_halsey", "Osiris", 2, "gotoVolume", "goal_halsey_steps", "" },
		{ 0x3E6F26D4u, "tv_goal_keyhole", "w2_grotto", "Swords of Sanghelios", 4, "gotoVolume", "goal_armory", "w2_grotto_keyhole" },
		{ 0x3F04B392u, "tv_end_trans_underbelly", "w2_tsunami", "Battle of Sunaion", 5, "gotoVolume", "goal_underbelly_1", "w2_tsunami_underbelly" },
		{ 0x3F24B30Fu, "tv_burgertown_complete", "w2_tsunami", "Battle of Sunaion", 2, "gotoVolume", "goal_tangletown", "w2_tsunami_burgertown" },
		{ 0x4143AB3Au, "tv_bridge_open", "w1_meridian", "Meridian Station", 4, "gotoVolume", "goal_bowl_2_drive", "zs_030" },
		{ 0x44250D8Fu, "tv_goal_04_cave", "w3_arrival", "Genesis", 3, "gotoVolume", "goal_arrival_cave", "zn_road" },
		{ 0x44594B17u, "tv_goal_02_landing", "w3_arrival", "Genesis", 1, "gotoVolume", "goal_arrival_landing", "zn_road" },
		{ 0x44FFF552u, "tv_hunter_init", "w4_station", "Blue Team", 6, "gotoVolume", "goal_hunter", "zs_03" },
		{ 0x494912C8u, "tv_goal_throne", "w3_citadel", "The Breaking", 8, "gotoVolume", "goal_throne", "zn_highground" },
		{ 0x4A87E6F9u, "tv_goal_patrol", "w3_builder", "Reunion", 1, "gotoVolume", "goal_patrol", "zn_outcrop" },
		{ 0x4C77C9D7u, "tv_walkway_stage3", "w3_innerworld", "Guardians", 8, "endMissionChain", "", "zn_end" },
		{ 0x51400192u, "tv_goal_07_grasslands", "w3_arrival", "Genesis", 5, "gotoVolume", "goal_arrival_grasslands", "zn_crossing" },
		{ 0x52C6DA55u, "tv_goal_flashback1", "w3_citadel", "The Breaking", 2, "gotoVolume", "goal_flashback1", "zn_lowground" },
		{ 0x55B993D0u, "tv_goal_aftermath", "w3_builder", "Reunion", 4, "gotoVolume", "goal_aftermath", "zn_marsh" },
		{ 0x584F9D20u, "tv_goal_vtol", "w3_builder", "Reunion", 8, "gotoVolume", "goal_vtol", "zn_canyon" },
		{ 0x5B114F91u, "tv_goal_temple3", "w3_builder", "Reunion", 7, "gotoVolume", "goal_temple3", "zn_to_canyon" },
		{ 0x5E3D2376u, "tv_walkway_stage1", "w3_innerworld", "Guardians", 6, "endMissionChain", "", "zn_end" },
		{ 0x67C01222u, "tv_goal_cavalier", "w3_citadel", "The Breaking", 7, "gotoVolume", "goal_cavalier", "zn_highground" },
		{ 0x69B3ED52u, "tv_flight_init", "w4_station", "Blue Team", 8, "gotoVolume", "goal_flight", "zs_04" },
		{ 0x6C48DF7Fu, "tv_goal_08_hallway", "w3_halsey", "Osiris", 7, "gotoVolume", "goal_halsey_hallway", "zn_hill" },
		{ 0x6C95FB71u, "tv_halsey", "w2_campsite", "unidentified", 2, "deviceUse", "", "w2_campsite" },
		{ 0x6E52D900u, "tv_goal_landing", "w2_grotto", "Swords of Sanghelios", 1, "gotoVolume", "goal_falls", "w2_grotto_intro_to_landing" },
		{ 0x6FC767EAu, "tv_palmer", "w2_campsite", "unidentified", 3, "endMission", "", "w2_campsite" },
		{ 0x70E70E48u, "tv_goal_scaffold", "w1_evacuation", "Evacuation", 5, "gotoVolume", "goal_controlRoom", "elevator_top" },
		{ 0x733420C9u, "tv_goal_03_road", "w3_arrival", "Genesis", 2, "gotoVolume", "goal_arrival_road", "zn_road" },
		{ 0x7874BC85u, "tv_goal_gondola", "w3_innerworld", "Guardians", 1, "gotoVolume", "goal_gondola", "zn_road" },
		{ 0x79A7190Eu, "tv_ur_mission_end", "w1_unconfirmed_reports", "Unconfirmed", 7, "endMission", "", "" },
		{ 0x7A01CE29u, "tv_hangar_escape", "w4_station", "Blue Team", 10, "endMission", "", "" },
		{ 0x7B73B4F1u, "tv_goal_09_gateway", "w3_arrival", "Genesis", 7, "endMission", "", "" },
		{ 0x8212EB4Du, "tv_exit_dustoff", "w2_plateau", "Alliance", 17, "endMission", "", "" },
		{ 0x82A185A6u, "tv_ur_upper_mine_complete", "w1_unconfirmed_reports", "Unconfirmed", 4, "gotoVolume", "goal_lava_lift", "zs030_lava_elevator" },
		{ 0x8478F376u, "tv_goal_10_control_room", "w3_halsey", "Osiris", 9, "endMission", "", "" },
		{ 0x859E338Au, "tv_goal_drive", "w1_evacuation", "Evacuation", 1, "gotoVolume", "goal_Outpost", "outpost" },
		{ 0x86AD3D7Bu, "tv_plaza", "w2_plateau", "Alliance", 11, "gotoVolume", "goal_SentryBoss", "plateau_crevice" },
		{ 0x86BCB8DBu, "tv_reactor_init", "w4_station", "Blue Team", 7, "gotoVolume", "goal_reactor", "zs_03_b" },
		{ 0x8D517634u, "tv_goal_arena", "w3_citadel", "The Breaking", 4, "gotoVolume", "goal_arena", "zn_midground" },
		{ 0x90C46CC7u, "tv_goal_hallway2", "w3_citadel", "The Breaking", 6, "gotoVolume", "goal_hallway2", "zn_highground" },
		{ 0x915A7D5Au, "tv_goal_plaza", "w2_grotto", "Swords of Sanghelios", 8, "gotoVolume", "goal_arbiter", "w2_grotto_courtyard_b" },
		{ 0x923CB44Eu, "tv_goal_05_crossing", "w3_arrival", "Genesis", 4, "gotoVolume", "goal_arrival_crossing", "zn_crossing" },
		{ 0x9471B50Eu, "nar_enter_station2", "w1_miningtown", "Meridian Station town hub (script name \"w1_hub_m", 2, "goalComplete", "goal_clues", "miningtown" },
		{ 0x98DF705Fu, "tv_ghostcave_start", "w2_plateau", "Alliance", 6, "gotoVolume", "goal_Bowl", "" },
		{ 0x9BCF54D7u, "tv_end_cave_goal", "w2_plateau", "Alliance", 2, "gotoVolume", "goal_SentryShipEncounter", "plateau_start" },
		{ 0x9CE85AA8u, "tv_goal_return", "w2_plateau", "Alliance", 13, "goalCompleteTask", "goal_Map", "plateau_sentry" },
		{ 0xA1681D48u, "tv_intro_spawn", "w4_station", "Blue Team", 1, "gotoVolume", "goal_intro_halls_fight", "zs_01" },
		{ 0xA4DD5775u, "tv_walkway_autobash", "w3_innerworld", "Guardians", 10, "endMission", "", "zn_end" },
		{ 0xAB9E09C1u, "tv_breach_ender", "w2_plateau", "Alliance", 1, "gotoVolume", "goal_Caves", "plateau_start" },
		{ 0xAFD05A86u, "tv_shipyard_init", "w4_station", "Blue Team", 3, "gotoVolume", "goal_shipyard", "zs_02" },
		{ 0xB0CC49A3u, "tv_goal_armory", "w2_grotto", "Swords of Sanghelios", 5, "gotoVolume", "goal_airlock", "w2_grotto_keyhole" },
		{ 0xB0D75455u, "tv_objcon_ob_20", "w2_plateau", "Alliance", 14, "goalCompleteTask", "goal_Artifact", "plateau_sentry" },
		{ 0xB0EA258Cu, "tv_builder_end", "w3_builder", "Reunion", 10, "endMission", "", "" },
		{ 0xB19024AEu, "tv_goal_arbiter", "w2_grotto", "Swords of Sanghelios", 9, "endMission", "", "" },
		{ 0xB2F29361u, "tv_goal_sinkhole", "w2_grotto", "Swords of Sanghelios", 3, "gotoVolume", "goal_keyhole", "w2_grotto_sinkhole" },
		{ 0xB3C85851u, "tv_goal_06_overlook", "w3_halsey", "Osiris", 5, "gotoVolume", "goal_halsey_overlook", "zn_airlock" },
		{ 0xB40EFD13u, "tv_hangar_banshee_exit", "w4_station", "Blue Team", 9, "gotoVolume", "goal_hangar", "zs_05" },
		{ 0xB59D6C8Fu, "tv_goal_gate", "w3_innerworld", "Guardians", 2, "gotoVolume", "goal_gate", "zn_lilbowl" },
		{ 0xB5F8A5C8u, "tv_end_arcade", "w2_tsunami", "Battle of Sunaion", 9, "gotoVolume", "goal_destructionalley", "w2_tsunami_destruction" },
		{ 0xB8B1F9CBu, "tv_goal_07_hill", "w3_halsey", "Osiris", 6, "gotoVolume", "goal_halsey_hill", "zn_hill" },
		{ 0xBC6AA8A0u, "tv_goal_underground", "w3_innerworld", "Guardians", 5, "gotoVolume", "goal_underground", "zn_factory" },
		{ 0xBC72F8C0u, "tv_goal_tower", "w3_citadel", "The Breaking", 3, "gotoVolume", "goal_tower", "zn_midground" },
		{ 0xBE004B1Au, "tv_goal_02_cliffside", "w3_halsey", "Osiris", 1, "gotoVolume", "goal_halsey_cliffside", "zn_cliff" },
		{ 0xBE1FB9D3u, "tv_preplaza", "w2_plateau", "Alliance", 10, "gotoVolume", "goal_Awakening", "plateau_crevice" },
		{ 0xC04DCDD5u, "tv_end_trans_arcade", "w2_tsunami", "Battle of Sunaion", 8, "gotoVolume", "goal_arcade", "w2_tsunami_arcade" },
		{ 0xC1A83D0Bu, "tv_oupost_end", "w1_meridian", "Meridian Station", 2, "gotoVolume", "goal_bowl_1_drive", "zs_020" },
		{ 0xC345E9D3u, "tv_hollow_objcon_80", "w2_plateau", "Alliance", 4, "gotoVolume", "goal_Ramp", "plateau_start" },
		{ 0xCAD348D0u, "tv_walkway_stage2", "w3_innerworld", "Guardians", 7, "endMissionChain", "", "zn_end" },
		{ 0xCB717F2Cu, "tv_bridge_start", "w1_meridian", "Meridian Station", 3, "gotoVolume", "goal_bridge", "zs_020" },
		{ 0xCDEDF441u, "tv_goal_hallway1", "w3_citadel", "The Breaking", 1, "gotoVolume", "goal_hallway1", "zn_lowground" },
		{ 0xD00F750Eu, "tv_ur_lava_lift_end", "w1_unconfirmed_reports", "Unconfirmed", 5, "gotoVolume", "goal_arena", "zs040_bridge_arena" },
		{ 0xD319E29Fu, "tv_liftoff_passed", "w2_plateau", "Alliance", 3, "gotoVolume", "goal_Hollow", "plateau_start" },
		{ 0xD4F82CE7u, "tv_tc_ship_goal", "w4_station", "Blue Team", 2, "gotoVolume", "goal_tech_center", "zs_01" },
		{ 0xD6F5EED1u, "tv_transition_to_bowl", "w2_plateau", "Alliance", 5, "gotoVolume", "goal_PreBowl", "plateau_transition" },
		{ 0xDE6EC0E9u, "tv_tunnels_init_01", "w4_station", "Blue Team", 4, "gotoVolume", "goal_tunnels", "zs_02_b" },
		{ 0xDEB12DA3u, "tv_mer_end_gate_open", "w1_meridian", "Meridian Station", 7, "endMission", "", "" },
		{ 0xE27F8858u, "tv_thru_2nd_gate", "w1_miningtown", "Meridian Station town hub (script name \"w1_hub_m", 1, "gotoVolume", "goal_sloan_intro", "miningtown" },
		{ 0xE48675C6u, "tv_goal_04_crag", "w3_halsey", "Osiris", 3, "gotoVolume", "goal_halsey_crag", "" },
		{ 0xE63C62BFu, "tv_goal_05_airlock", "w3_halsey", "Osiris", 4, "gotoVolume", "goal_halsey_airlock", "zn_crack" },
		{ 0xE6EFC4A8u, "tv_goal_caves", "w3_builder", "Reunion", 5, "gotoVolume", "goal_caves", "zn_docks" },
		{ 0xEB9882ACu, "tv_end_destructionalley", "w2_tsunami", "Battle of Sunaion", 10, "gotoVolume", "goal_bldg_interior", "w2_tsunami_destruction" },
		{ 0xEBCAE3A5u, "tv_end_pelican", "w1_evacuation", "Evacuation", 6, "endMission", "", "" },
		{ 0xED2681CEu, "tv_end_bldg_interior", "w2_tsunami", "Battle of Sunaion", 11, "gotoVolume", "goal_trans_final", "w2_tsunami_finale" },
		{ 0xEEAB6784u, "tv_ur_ship_path_checkpoint", "w1_unconfirmed_reports", "Unconfirmed", 1, "gotoVolume", "goal_chief_ship", "zs010_chiefship" },
		{ 0xF70B7177u, "tv_goal_finalwalk", "w3_builder", "Reunion", 9, "gotoVolume", "goal_finalwalk", "zn_grasslands" },
		{ 0xF959280Eu, "tv_goal_cores", "w3_innerworld", "Guardians", 3, "gotoVolume", "goal_cores", "zn_bowl" },
		{ 0xF97FB057u, "tv_ur_airlock_trans", "w1_unconfirmed_reports", "Unconfirmed", 3, "gotoVolume", "goal_upper_mines", "zs020_cave" },
		{ 0xFD334AF7u, "tv_tunnel_fall_in", "w4_station", "Blue Team", 5, "gotoVolume", "goal_tunnels_landed", "zs_03" },
		{ 0xFF21CFAEu, "tv_goal_encampment", "w3_builder", "Reunion", 2, "gotoVolume", "goal_encampment", "zn_outcrop" },
	} };

	inline const Entry* find(uint32_t hash) noexcept
	{
		const auto it = std::lower_bound(kEntries.begin(), kEntries.end(), hash,
			[](const Entry& e, uint32_t h) { return e.hash < h; });
		return (it != kEntries.end() && it->hash == hash) ? &*it : nullptr;
	}

	inline bool isSpeedrun(uint32_t hash) noexcept { return find(hash) != nullptr; }

	// That volume's 1-based step within ITS OWN level's path, or 0 when it is not a speedrun volume.
	inline int stepOf(uint32_t hash) noexcept
	{
		const Entry* e = find(hash);
		return e ? e->step : 0;
	}

	// How many volume steps the given level has. 0 for a level that has not been mapped.
	inline int stepsInLevel(std::string_view level) noexcept
	{
		int n = 0;
		for (const auto& e : kEntries) if (level == e.level) ++n;
		return n;
	}

	inline int totalSteps() noexcept { return (int)kEntries.size(); }
}
