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
