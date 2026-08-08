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
// displayName is FREE TO DIFFER from the skull's full name, and for three entries it does: the hotkey column costs
// 30px out of a ~240px table cell, so "Grunt Birthday Party", "Spore Visibility" and "Johnny Ammo Tree" are
// shortened, with the full name moved to the front of the tooltip. MCC's GUISkullToggle does the same thing to its
// own two Birthday Party skulls. What must NOT follow suit is the matching HCE_SKULL_HOTKEYS enumerator:
// RebindableHotkey serialises through magic_enum::enum_name, so those names are on-disk keys and renaming one
// silently drops that user's binding. A display name is a label; an enumerator is a key.
//
// The reference tool labels its own section "Skulls (Some do not work!)" - several of these bits are inert in the
// shipped game. That is the game's behaviour, not a porting defect.
// ================================================================================================================

struct HCESkullInfo
{
	const char* displayName;
	const char* toolTip;
};

static constexpr HCESkullInfo kHCESkulls[] =
{
	/*  0 */ { "Iron",                 "Death carries a heavy price." },
	/*  1 */ { "Black Eye",            "Your shields only recharge when you melee enemies." },
	/*  2 */ { "Tough Luck",           "Enemies always go berserk, always dive out of the way, never flee." },
	/*  3 */ { "Catch",                "Enemies throw and drop more grenades." },
	/*  4 */ { "Fog",                  "The motion tracker is disabled." },
	/*  5 */ { "Famine",               "Weapons dropped by AI have half the ammo they normally would." },
	/*  6 */ { "Thunderstorm",         "Upgrades the rank of most enemies." },
	/*  7 */ { "Tilt",                 "Enemy resistances and weaknesses are increased." },
	/*  8 */ { "Mythic",               "Enemies have increased health." },
	/*  9 */ { "Assassin",             "All enemies in the game are permanently cloaked." },
	/* 10 */ { "Blind",                "HUD and weapon do not display onscreen." },
	/* 11 */ { "Superman",             "Halo Campaign Evolved skull." },
	/* 12 */ { "Grunt Birthday",       "Grunt Birthday Party. Grunt headshots lead to glorious celebrations." },
	/* 13 */ { "IWHBYD",               "Rare combat dialogue becomes more common." },
	/* 14 */ { "Red",                  "Halo Campaign Evolved skull." },
	/* 15 */ { "Yellow",               "Halo Campaign Evolved skull." },
	/* 16 */ { "Blue",                 "Halo Campaign Evolved skull." },
	/* 17 */ { "Angry",                "Halo Campaign Evolved skull." },
	/* 18 */ { "Bandanna",             "Infinite ammo, grenades and equipment." },
	/* 19 */ { "Bonded Pair",          "Co-op skull: when one player dies, the other gets a large damage boost." },
	/* 20 */ { "Boom",                 "Explosion radius is doubled." },
	/* 21 */ { "Envy",                 "Chief trades his flashlight for Active Camo." },
	/* 22 */ { "Eye Patch",            "Auto aim features disabled for all weapons." },
	/* 23 */ { "Foreign",              "Players cannot pick up or use opposing factions' weapons." },
	/* 24 */ { "Ghost",                "AI no longer flinch from attacks." },
	/* 25 */ { "Grunt Funeral",        "Grunts die in a blaze of glory. And plasma." },
	/* 26 */ { "Jacked",               "Players can only enter ground vehicles by boarding." },
	/* 27 */ { "Malfunction",          "Every time you respawn, a random element of your HUD is disabled." },
	/* 28 */ { "Masterblaster",        "Co-op skull: roles switch after a number of enemy kills." },
	/* 29 */ { "Pinata",               "Punching enemies makes them drop grenades." },
	/* 30 */ { "Recession",            "Every shot is worth twice the ammo." },
	/* 31 */ { "Scarab",               "All player-held weapons fire Scarab Gun beams." },
	/* 32 */ { "So Angry",             "Enraged enemies explode like a grenade if not dispatched within seconds." },
	/* 33 */ { "Swarm",                "Hunters are much stronger." },
	/* 34 */ { "That's Just Wrong",    "Increases enemy awareness of the player." },
	/* 35 */ { "They Come Back",       "Reanimated Flood Combat Forms are much more dangerous." },
	/* 36 */ { "Acrophobia",           "The Corps: now issuing rifles AND wings." },
	/* 37 */ { "Adaptation",           "Halo Campaign Evolved skull." },
	/* 38 */ { "Reload",               "Halo Campaign Evolved skull." },
	/* 39 */ { "Spore Vision",         "Spore Visibility. Halo Campaign Evolved skull." },
	/* 40 */ { "Night Vision",         "Halo Campaign Evolved skull." },
	/* 41 */ { "Lights Out",           "Halo Campaign Evolved skull." },
	/* 42 */ { "Riskrun",              "Halo Campaign Evolved skull." },
	/* 43 */ { "Pop",                  "Halo Campaign Evolved skull." },
	/* 44 */ { "Armistice",            "Halo Campaign Evolved skull." },
	/* 45 */ { "Fragile",              "Halo Campaign Evolved skull." },
	/* 46 */ { "Give and Take",        "Halo Campaign Evolved skull." },
	/* 47 */ { "Stow and Grow",        "Halo Campaign Evolved skull." },
	/* 48 */ { "Hip Fire",             "Halo Campaign Evolved skull." },
	/* 49 */ { "Temperamental",        "Halo Campaign Evolved skull." },
	/* 50 */ { "Floor Is Lava",        "Halo Campaign Evolved skull." },
	/* 51 */ { "Magnified",            "Halo Campaign Evolved skull." },
	/* 52 */ { "Johnny Ammo",          "Johnny Ammo Tree. Halo Campaign Evolved skull." },
	/* 53 */ { "Leadhead",             "Halo Campaign Evolved skull." },
	/* 54 */ { "Efficient",            "Halo Campaign Evolved skull." },
	/* 55 */ { "Third Person",         "Halo Campaign Evolved skull." },
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
