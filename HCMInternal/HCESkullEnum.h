#pragma once
#include "pch.h"
#include "HotkeysEnum.h"
#include "boost\preprocessor.hpp"

// ================================================================================================================
// Halo Campaign Evolved skull table.
//
// HCE's skulls are NOT the MCC skulls: 56 of them, addressed by BIT INDEX into one contiguous bitfield at
// *(tls + 0x60) + 0x1EBE0 (7 bytes), and 25 of them have no SkullEnum equivalent anywhere in HCM. Rather than
// stretch SkullEnum / GUISkullToggle - files every MCC game shares - HCE gets its own self-contained table and
// widget. The hotkeys live in their own HCE_SKULL_HOTKEYS macro for the same reason MCC's do: BOOST_PP_TUPLE_SIZE
// counts at most 64 per tuple, and HotkeyDefinitions.h sums the macros instead of counting one combined list.
//
// THE ARRAY INDEX IS THE BIT INDEX. This is HCM_Evolved gameplay.py's SKULLS tuple in declaration order - do not
// sort, insert or remove entries. Display order is alphabetical and is applied at render time.
//
// ⚠ RENAMING A DISPLAY NAME IS FREE. RENAMING ITS HOTKEY ENUMERATOR IS NOT. HCE_SKULL_HOTKEYS still carries the
// original identifiers (hceSkullSupermanHotkey is Cowbell's, hceSkullThirdPersonHotkey is Perspective's, and so
// on) because RebindableHotkey serialises through magic_enum::enum_name - those strings are on-disk keys, and
// renaming one silently drops that user's binding. A display name is a label; an enumerator is a key. The pairing
// is by BIT INDEX via kHCESkullHotkeys, so the two lists never needed to agree on wording.
//
// ⚠ ONLY 42 OF THE 56 BITS ARE REAL SKULLS, AND THE NAMES CAME FROM THE GAME, NOT THE PORT.
//
// The table originally used HCM_Evolved's SKULLS tuple for both order and names. The ORDER is correct - confirmed
// in game, where ticking bit 11 lights up Cowbell - but several NAMES were wrong, and 14 of the bits are not
// skulls the shipped game exposes at all. HCM_Evolved's list also invented entries ("Superman", "Fragile") that
// appear nowhere in the game's own EBlamGameSkulls enum, which is how the mismatch went unnoticed.
//
// So displayName/toolTip are now the MAIN MENU's wording, and inMenu marks the 42 the menu actually offers.
// GUIHCESkullToggle renders only those; the rest stay in the table because THE ARRAY INDEX IS THE BIT INDEX and
// removing a row would silently renumber every skull after it.
//
// Cross-checked three ways: the menu itself, the game's EBlamGameSkulls enum strings in HaloCampaignEvolved.exe,
// and the existing bit order. Internal enum names differ from menu names in several places - Acrophobia is
// BootsOffTheGround, Perspective is ThirdPerson, Johnny Ammo Seed is JohnnyAmmoTree - so neither list alone was
// enough.
// ================================================================================================================

struct HCESkullInfo
{
	const char* displayName;
	const char* toolTip;
	bool        inMenu;      // false = the bit exists but the shipped game never offers it; hidden from the GUI
};

static constexpr HCESkullInfo kHCESkulls[] =
{
	/*  0 */ { "Iron",                 "Dying in co-op resets you to your last checkpoint; dying while solo restarts the level.", true },
	/*  1 */ { "Black Eye",            "Your shields will only recharge when you melee enemies.", true },
	/*  2 */ { "Tough Luck",           "Enemies always go berserk, constantly dive out of the way, and never run away from a fight.", true },
	/*  3 */ { "Catch",                "Enemies throw and drop more grenades.", true },
	/*  4 */ { "Fog",                  "Motion Tracker is disabled.", true },
	/*  5 */ { "Famine",               "Weapons dropped by enemies contain half the ammo.", true },
	/*  6 */ { "Thunderstorm",         "Upgrades the rank of most enemies.", true },
	/*  7 */ { "Tilt",                 "Enhances both positive and negative damage multipliers.", true },
	/*  8 */ { "Mythic",               "Enemies have increased health.", true },
	/*  9 */ { "Assassin",             "Not offered by the shipped game's skull menu.", false },
	/* 10 */ { "Blind",                "HUD and Weapons do not appear onscreen.", true },
	/* 11 */ { "Cowbell",              "Physics mass of objects is reduced, making them more easily displaced.", true },
	/* 12 */ { "Grunt Birthday Party", "Grunt headshots produce legendary celebrations.", true },
	/* 13 */ { "IWHBYD",               "It would have been your dialogue.", true },
	/* 14 */ { "Custom Red",           "Not offered by the shipped game's skull menu.", false },
	/* 15 */ { "Custom Yellow",        "Not offered by the shipped game's skull menu.", false },
	/* 16 */ { "Custom Blue",          "Not offered by the shipped game's skull menu.", false },
	/* 17 */ { "Angry",                "Enemies fire their weapons faster and more frequently.", true },
	/* 18 */ { "Bandana",              "Infinite ammo and grenades.", true },
	/* 19 */ { "Bonded Pair",          "Not offered by the shipped game's skull menu.", false },
	/* 20 */ { "Boom",                 "Explosion radius is increased by 2x.", true },
	/* 21 */ { "Envy",                 "Not offered by the shipped game's skull menu.", false },
	/* 22 */ { "Eye Patch",            "All auto-aim is disabled for all weapons.", true },
	/* 23 */ { "Foreign",              "Players cannot use weapons from another faction.", true },
	/* 24 */ { "Ghost",                "Enemies no longer flinch when taking fire.", true },
	/* 25 */ { "Grunt Funeral",        "Grunts die in a blaze of plasma.", true },
	/* 26 */ { "Jacked",               "Not offered by the shipped game's skull menu.", false },
	/* 27 */ { "Malfunction",          "Upon respawn a random element of your HUD no longer appears.", true },
	/* 28 */ { "Masterblaster",        "Not offered by the shipped game's skull menu.", false },
	/* 29 */ { "Pinata",               "Punching enemies makes them drop inert grenades.", true },
	/* 30 */ { "Recession",            "Every shot expends twice the normal amount of ammo.", true },
	/* 31 */ { "Scarab",               "Not offered by the shipped game's skull menu.", false },
	/* 32 */ { "So Angry",             "Not offered by the shipped game's skull menu.", false },
	/* 33 */ { "Swarm",                "Not offered by the shipped game's skull menu.", false },
	/* 34 */ { "That's Just... Wrong", "Enemies have increased awareness of the player.", true },
	/* 35 */ { "They Come Back",       "Flood Combat Forms reanimated by Infection Forms are much more dangerous.", true },
	/* 36 */ { "Acrophobia",           "The Corps: Now issue rifles AND wings...", true },
	/* 37 */ { "Adaptation",           "Randomizes the enemy factions present in the mission.", true },
	/* 38 */ { "Reload",               "Randomizes the pre-placed weapons for the mission.", true },
	/* 39 */ { "Spore Visibility",     "Not offered by the shipped game's skull menu.", false },
	/* 40 */ { "Night Vision",         "Not offered by the shipped game's skull menu.", false },
	/* 41 */ { "Lights Out",           "Not offered by the shipped game's skull menu.", false },
	/* 42 */ { "Speed Limit",          "Take your time, enjoy the sights! Disables sprint.  (mapping inferred - see the header note)", true },
	/* 43 */ { "Pop",                  "Shooting Flood in their weak point causes them to explode.", true },
	/* 44 */ { "Armistice",            "Enemy factions never fight one another.", true },
	/* 45 */ { "Endurance Spec",       "UNSC vehicles cannot be damaged.  (mapping inferred - see the header note)", true },
	/* 46 */ { "Give & Take",          "Firing your weapon drains your shield, but dealing damage to enemies recharges it.", true },
	/* 47 */ { "Stowed Reload",        "Weapons continue to reload while stowed.", true },
	/* 48 */ { "Hip Fire",             "All weapon zoom functionality is disabled.", true },
	/* 49 */ { "Temperamental",        "Allies will attack the player more quickly if the player kills a friendly.", true },
	/* 50 */ { "Floor is Lava",        "Take continuous damage whenever touching the \"ground\".", true },
	/* 51 */ { "Magnified",            "60-round magazines for the AR are now standard-issue.", true },
	/* 52 */ { "Johnny Ammo Seed",     "When reloading you lose the ammo remaining in the discarded clip.", true },
	/* 53 */ { "Leadhead",             "Headshots no longer provide extra damage or instant kills.", true },
	/* 54 */ { "Efficient",            "Enemies killed while the player is at full health drop more ammo.", true },
	/* 55 */ { "Perspective",          "Changes the player camera to third-person.", true },
};

constexpr size_t kHCESkullCount = std::size(kHCESkulls);
static_assert(kHCESkullCount == 56, "HCE skull table must stay in sync with HCM_Evolved gameplay.py SKULLS");

// Bit index -> the hotkey that toggles that skull. Generated straight from HCE_SKULL_HOTKEYS rather than retyped,
// so entry N here is always entry N of that macro - the only ordering left to get wrong is the macro against
// kHCESkulls above, and both are documented as bit order. All are unbound by default; users assign them from the
// skull widget's "..." button.
//
// The FOR_EACH just qualifies each bare enumerator with its enum class name. TUPLE_TO_SEQ shares the same 64-entry
// ceiling as TUPLE_SIZE, which is the reason HCE_SKULL_HOTKEYS is its own macro in the first place.
#define HCE_QUALIFY_SKULL_HOTKEY(r, data, elem) RebindableHotkeyEnum::elem,
static constexpr RebindableHotkeyEnum kHCESkullHotkeys[] =
{
	BOOST_PP_SEQ_FOR_EACH(HCE_QUALIFY_SKULL_HOTKEY, ~, BOOST_PP_TUPLE_TO_SEQ((HCE_SKULL_HOTKEYS)))
};
#undef HCE_QUALIFY_SKULL_HOTKEY

static_assert(std::size(kHCESkullHotkeys) == kHCESkullCount,
	"HCE_SKULL_HOTKEYS must have exactly one hotkey per skull, in bit order");

// 56 bits, LSB first => exactly 7 bytes. maintain_enabled_toggles reads/writes literally this many.
constexpr size_t kHCESkullByteCount = (kHCESkullCount + 7) / 8;
