#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - INFINITE AMMO / BOTTOMLESS CLIP and ONE SHOT KILL.
//
// Both write "malleable properties" - per-unit knobs the engine keeps ON THE PLAYER OBJECT. Each slot is
//     { value, string_id, override, hasOverride }
// and every accessor is literally `return hasOverride ? override : value`. So we write the override and set the
// flag, which beats anything that later rewrites only the base value.
//
// ⚠⚠ THESE ARE NOT THE PLAYER-TRAITS STRUCT, AND THAT MATTERS. The obvious target is c_player_traits
// (player + 0x4C48), which really does contain an "infinite ammo" byte at +0xA1 with a clean 4-value enum
// (0 unchanged / 1 off / 2 on / 3 bottomless_clip). It is a dead end: an exhaustive disassembly of all 327,025
// .pdata functions found NO gameplay reader of that byte anywhere in the image - only two script-binding
// getters, the section compare, and the network serialiser. The ammo system reads THESE structs instead, which
// is why the feature lives here. Do not "simplify" this by writing the traits byte.
//
// PROOF THIS IS THE PATH THE GAME ACTUALLY READS: weapon_has_infinite_ammo (exe+0x02870A10) resolves a weapon's
// owning unit, adds 0xEAC, and calls the accessor at exe+0x028F7890 - and it has FIFTEEN callers across the
// weapon/ammo/magazine code.
//
// ⚠ ONE SHOT KILL IS AN OUTGOING MULTIPLIER, VERIFIED, NOT ASSUMED. Halo 5 splits the two directions into
// different structs. weapon_damage_scalar's consumer takes an attacker record (responsible player datum + team,
// friendly-fire path returns 0) and its caller multiplies the damage dealt. The INCOMING knobs live elsewhere
// and the engine's own explanation string for them reads "Any damage taken is divided by this number." Writing
// the wrong one would have made the player take a fortune in damage instead of dealing it.
//
// ⚠⚠ RESPAWN DESTROYS THE EFFECT, BY CONSTRUCTION. A respawn builds a NEW biped, and the unit constructor
// (exe+0x027F8600 -> exe+0x028F7B00) hardcodes infinite_ammo = 0, bottomless_clip = 0 and zeroes every
// override/hasOverride. The OBJECT POINTER also changes. So this cannot be a one-shot write: both cheats
// re-resolve the object and re-apply every frame while enabled, and treat a changed pointer as a new unit.
//
// ⚠⚠ THERE ARE TWO LIVE COPIES OF THE PLAYER BIPED and they are byte-identical, so no amount of sampling can
// tell them apart - only the object-write gate can. We go through H5GetPlayerState::getPlayerObject(), whose
// TLS resolver already prefers the gated (authoritative) simulation thread. Writing the mirror instead would
// look exactly like the cheat silently doing nothing. See the banner in H5GetPlayerState.cpp.
//
// ⚠ NOT YET PROVEN BY A FIRING TEST. Every offset, id and accessor here was derived from disassembly and read
// back live, and the live player biped reported weapon_damage_scalar base 1.0 with id 0xFDCEB810 exactly where
// predicted. What nobody has done is fire a weapon with these bytes set. The failure mode if the inference is
// wrong is benign - these are in-range writes to a data struct the engine rewrites wholesale, so the worst
// realistic outcome is that the cheat does nothing.
// ================================================================================================================

// Infinite Ammo and Bottomless Clip. Two toggles, two adjacent properties - bottomless clip additionally stops
// the magazine draining, so when both are on it is the strictly stronger one.
class H5InfiniteAmmo : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5InfiniteAmmo(GameState game, IDIContainer& dicon);
	~H5InfiniteAmmo();
	std::string_view getName() override { return nameof(H5InfiniteAmmo); }
};

// One Shot Kill - scales the damage the player DEALS. Weapon, melee and grenade are three independent
// multipliers, so "one tap anything" needs all three; a single scalar would leave melee and nades untouched.
class H5OneShotKill : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5OneShotKill(GameState game, IDIContainer& dicon);
	~H5OneShotKill();
	std::string_view getName() override { return nameof(H5OneShotKill); }
};
