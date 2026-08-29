#pragma once
#include <string>
#include <string_view>
#include <unordered_set>

// ================================================================================================================
// Halo Campaign Evolved - the trigger volumes a speedrun actually has to hit.
//
// Transcribed from the community "Completion Requirements" documents (one per mission), which describe, in
// order, what the mission's completion scripts test before granting game_won.
//
// ONE FLAT SET, NOT PER-LEVEL. Only one level is ever loaded, so a name from another mission simply never
// matches anything in the current scenario's trigger block - a per-level map would be more code for no
// behavioural difference. It also means the filter degrades gracefully if a level is ever misidentified.
//
// The documents also list things that are NOT trigger volumes: script globals (game_won, dialog_gen3_disabled),
// encounter names (enc6_3), object/button states (brig_keyes_control_group=1), and continuous conditions
// (elevator_1 >= 0.5). Those are deliberately kept where they are plausibly volume-shaped and dropped where they
// are clearly not - an entry that matches no volume costs nothing, so the list errs towards inclusion. What it
// must never do is MISS a real volume, because then a runner trusts an overlay that is hiding a requirement.
//
// ⚠ These are Halo 1 mission names. HaloCER's scenarios reuse the classic names, which is why this works at all.
// If a future build renames volumes, this list goes stale SILENTLY - the filter would just show fewer volumes.
// That is why the GUI element says "Speedrun triggers only" rather than anything that implies completeness.
// ================================================================================================================

namespace HCESpeedrunTriggerNames
{
	// Case-insensitive comparison, because scenario names are not reliably lower case.
	struct CaseInsensitiveHash
	{
		using is_transparent = void;
		size_t operator()(std::string_view s) const noexcept
		{
			size_t h = 1469598103934665603ull;                       // FNV-1a
			for (unsigned char c : s)
			{
				h ^= (unsigned char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
				h *= 1099511628211ull;
			}
			return h;
		}
	};

	struct CaseInsensitiveEqual
	{
		using is_transparent = void;
		bool operator()(std::string_view a, std::string_view b) const noexcept
		{
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
				if (x >= 'A' && x <= 'Z') x += 32;
				if (y >= 'A' && y <= 'Z') y += 32;
				if (x != y) return false;
			}
			return true;
		}
	};

	inline const std::unordered_set<std::string, CaseInsensitiveHash, CaseInsensitiveEqual>& all()
	{
		static const std::unordered_set<std::string, CaseInsensitiveHash, CaseInsensitiveEqual> names =
		{
			// 01 - The Pillar of Autumn
			"tv_bridge_trigger_1", "tv_bridge_cinematic", "tv_bsp5_6",
			"tv_final_trigger_1", "tv_final_trigger_2", "tv_final_trigger_6", "tv_final_trigger_11",
			"sq_lifepod_2_anti", "tv_end_trigger",

			// 02 - Halo
			"first_arrival", "cave_floor_entrance", "cave_exit_fat",
			"cliff", "rubble", "river",
			"cliff_1", "cliff_2", "cliff_all",
			"cliff_marine_entrance_2", "cliff_marine_entrance_4", "cliff_inside_left_bottom",
			"rubble_1", "rubble_2", "rubble_arrival",
			"river_1", "river_2",

			// 03 - Truth and Reconciliation
			"area3_trigger", "area4_trigger", "area5_trigger", "gravity_trigger",
			"grav_muster_top_hall", "muster_bay_top_entrance", "hangar_trigger", "control_migration",
			"prison_trigger", "prison_after_reyes_saved_trigger", "enc_prison_break_in",
			"hangar_migration", "hangar_control_approach_trigger",

			// 04 - The Silent Cartographer
			"valley_back", "shafta_lock_door", "shafta_ledge_approach_trigger", "shafta_switch",
			"shafta_entrance",

			// 05 - Assault on the Control Room
			"game_win_trigger",

			// 06 - 343 Guilty Spark
			"tv_cleanup_lift_d_shaft_entrance", "tv_player_on_surface_swamp_b",
			"swamp_b_trigger_4", "swamp_b_trigger_final",

			// 07 - The Library
			"tv_act_lift2", "tv_act_lift3", "tv_vo_floor4_index",

			// 08 - Two Betrayals
			"tv_e61_a_trigger", "tv_e66_a_trigger",

			// 09 - Keyes
			"0_to_1_transition_trigger", "section4", "section5", "section6", "section7",
			"enc6_3", "enc6_4", "enc6_3_cin", "enc7_2", "enc7_2b", "enc7_2c",
			"sq_outro_banshee1", "sq_outro_banshee2",

			// 10 - The Maw
			"tv_enc7_0", "tv_enc7_0_2", "tv_enc7_1", "tv_get_aboard", "tv_grand_finale",

			// ======================= BONUS MISSIONS (E10, E20, E30) =======================
			// These have no community completion-requirement document, so unlike the ten campaign lists above
			// they were derived MECHANICALLY from the missions' own HaloScript: a tracer starts at the script
			// containing (game_won) and walks BACKWARDS through two edges - scripts that (set ...) a global
			// appearing in a blocking sleep_until, and scripts that (wake ...) it - collecting every
			// volume_test_* on the transitive chain.
			//
			// ★ THE METHOD IS VALIDATED: run against a15 it emits exactly the 8 volumes in the Pillar of Autumn
			// block above - zero extra, zero missing. So "gating" as the community docs define it IS the
			// transitive script-dependency chain of game_won, and this reproduces it.
			//
			// ⚠ DEPENDENCY CHAIN ONLY - THE MINIMUM SET. An earlier version of this block also listed volumes
			// that gate a door/lift/grav-bridge the player cannot physically bypass. That was WRONG for this
			// filter: those are places you happen to pass through, not things the completion scripts require,
			// and in game they cluttered E10 badly. This filter answers "what must I actually trigger", so it
			// carries the same thing the campaign lists carry - the transitive dependency chain of game_won -
			// and nothing else.
			//
			// ⚠ Same-named volumes exist across levels (e.g. tv_final_encounter is on both E10 and E20). The
			// set is flat and only one level is ever loaded, so that is harmless.
			// ⚠ UNVERIFIED: co-op-only branches were not enumerated (several chains test
			//   game_coop_player_count / game_difficulty_get_real; the volumes below are common to all branches).

			// E10 - Boarding Action  (cinematics\e10\e10_boarding.cinematic; unlocks E20)
			// Genuinely ONE volume: e10_start has no sleep_until spine, so the chain is
			// tv_change_of_plans_start -> start_change_of_plans -> e10_signals.cinematic -> (game_won).
			// It is testable from mission start, which is why it shows as active immediately.
			// ⚠ UNVERIFIED whether the one-volume route is actually WALKABLE: E10 sets its zone set once in the
			// start fixup (set_1_docking_bay) and never switches it again, whereas the final-encounter fixup
			// would have loaded set_4_geneforge_main_lab. Streaming may not have the destination resident.
			"tv_change_of_plans_start",

			// E20 - The Most Dangerous Game  (obj_survivehunt, "hunters hunted"; unlocks E30)
			// Non-volume conditions on the same chain: gr_final_encounter_wv1/wv2/wv3 must all reach 0, and
			// the control-room console dc_final_interact must be used.
			"tv_settlement_end", "tv_prison_room_bottom", "tv_marine_cells",
			"tv_final_encounter_save", "tv_final_encounter", "tv_end_mission",

			// E30 - Heavy Burden  ("Protect the Burden"; escorts device dm_bop)
			// Non-volume conditions: dm_bop must reach r_burden_pos_final, gr_space3_dogfight must reach 0.
			// ⚠ tv_space_3_trigger is an OR with (>= (device_get_position dm_bop) .1), so it is skippable in
			// principle - listed anyway because showing a volume you may not need costs nothing.
			//
			// E30 is the one mission where the minimum really is most of the level, and for a structural reason:
			// mission_3_start arms nothing, and e30_insertion_point_fixups matches the start point EXACTLY (=,
			// not the <= that E10/E20 use), so nothing downstream is pre-armed - every beat is woken solely by
			// the previous beat's script. Three chunks still fall out of the chain:
			//   - beat 1 before Control        (begin_beat1 wakes `control` immediately)
			//   - tv_end_beat_2                (rr wakes beat3_trigger BEFORE it waits on that volume)
			//   - the entire beat-3 tram sequence (both hydro switches, the tram button, the dome station,
			//     e24..e34) - begin_beat3 wakes beat4_trigger immediately, so tv_space_trans_to_1 is live the
			//     moment you enter tv_began_beat3.
			// Hard floors that are NOT volumes and cannot be rushed: dm_bop advances on the device's own clock
			// and fully STOPS at .55 until the Harmony vignette ends, and marine-rescue waves b and c are
			// kill-gated with no timeout.
			"tv_act_control", "tv_control_encounter", "tv_end_beat_1",
			"tv_act_rr", "tv_began_beat3",
			"tv_space_trans_to_1", "tv_space_3_trigger",
		};
		return names;
	}

	inline bool isSpeedrunTrigger(const std::string& volumeName)
	{
		return all().find(volumeName) != all().end();
	}

	// BSP / zone-set switching volumes. Halo's scenario format has no "this volume switches BSP" flag - BSP and
	// zone-set changes are driven by SCRIPT, so the only signal available from tag data is the naming
	// convention the level designers used (e.g. "tv_bsp5_6" on The Pillar of Autumn).
	//
	// ⚠ This is therefore a HEURISTIC, not a fact read out of the tag. It can miss a differently-named switch
	// volume, and it can flag a volume that merely has "bsp" in its name for another reason. It is only used to
	// pick a COLOUR, so a wrong answer is cosmetic.
	// Kill volumes. ⚠ CURRENTLY A NAME HEURISTIC, pending the scenario block that marks them authoritatively
	// (the same way zone-set switches turned out to have their own block referencing volumes by index). Halo's
	// convention is names like "kill", "kill_soft", "kill_volume_*". Replace this the moment the real block is
	// known - a heuristic here can both miss a kill volume and flag an innocent one.
	inline bool isKillTrigger(const std::string& volumeName)
	{
		std::string lower;
		lower.reserve(volumeName.size());
		for (unsigned char c : volumeName) lower.push_back((char)(c >= 'A' && c <= 'Z' ? c + 32 : c));

		return lower.find("kill") != std::string::npos
			|| lower.find("death") != std::string::npos;
	}

	// ⚠ MUST BE TESTED BEFORE isBspOrZoneSetTrigger, which is a SUBSTRING test on "zone_set" and therefore
	// also matches "begin_zone_set:...". Testing the plain one first puts every begin volume in the commit
	// bucket. The engine ships both authoring prefixes as literals ("zone_set:" @0x1808506D8,
	// "begin_zone_set:" @0x180850750) with ZERO code references, so the naming convention is real but
	// TOOL-side only - the scenario's +0x29C switch block is the authority, and this is only a fallback for
	// volumes that block does not cover.
	inline bool isBeginZoneSetTrigger(const std::string& volumeName)
	{
		std::string lower;
		lower.reserve(volumeName.size());
		for (unsigned char c : volumeName) lower.push_back((char)(c >= 'A' && c <= 'Z' ? c + 32 : c));

		return lower.find("begin_zone_set") != std::string::npos
			|| lower.find("beginzoneset") != std::string::npos;
	}

	inline bool isBspOrZoneSetTrigger(const std::string& volumeName)
	{
		std::string lower;
		lower.reserve(volumeName.size());
		for (unsigned char c : volumeName) lower.push_back((char)(c >= 'A' && c <= 'Z' ? c + 32 : c));

		return lower.find("bsp") != std::string::npos
			|| lower.find("zoneset") != std::string::npos
			|| lower.find("zone_set") != std::string::npos;
	}
}
