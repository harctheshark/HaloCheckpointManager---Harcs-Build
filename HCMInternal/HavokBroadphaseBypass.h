#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// HAVOK BROADPHASE BYPASS - Halo 2, Halo 3, ODST, Reach, Halo 4, Halo 2 Anniversary MP (groundhog) and Halo Campaign
// Evolved. (Halo 5 has its own, H5OutOfBoundsBypass; Halo 1 has no Havok.)
//
// When a Havok body leaves the physics world's broadphase box, every one of these engines DELETES the owning object
// (Halo 5 logs it as "rigid body has exited broadphase ... The associated object has been deleted"). This stops ONLY
// that deletion. The separate "went outside of the world" player kill timer is deliberately left alone.
//
// How each engine does it, and why the patch differs per game (all re-derived and adversarially verified; RE in
// Documents\Halo Mod And Tools\Havok Broadphase Bypass\<game>\):
//   * H3 / ODST / Reach / H2A-MP / CER: a border-phantom callback sets an "exited" bit on the body's component, the
//     per-body post-step clears it, asks the engine's own exemption predicate, and otherwise CALLs object_delete.
//     We replace that 5-byte call with ONE 5-byte NOP; execution continues on the path the delete would return to.
//   * Halo 4: same shape, but we flip the exemption branch (jnz -> jmp, 1 byte) so every object takes the engine's
//     own exempt (no-delete) path.
//   (What the exemption predicate spares differs per game - network-predicted objects in Halo 2, mode-gated objects
//   elsewhere - and was not pinned down for every engine. The patch does not depend on it.)
//   * Halo 2: Bungie's own Havok-3 border. The handler ends in a TAIL JUMP to object_delete after the epilogue has
//     already restored rbx/rsp - NOPing it like Halo 5 would fall through into the epilogue twice and crash. We flip
//     the exemption branch (jne -> jmp, 1 byte) onto the engine's own exempt exit instead.
// No engine re-fires every tick after a skipped delete: H3/ODST/Reach/H4/H2A-MP/CER clear the "exited" bit BEFORE the
// delete call, and Halo 2's handler IS Havok's edge-triggered collidableAdded callback (it sets comp->flags |= 0x1000
// "in border" once when the overlap begins; collidableRemoved clears it).
// Not every body is affected: KEYFRAMED/FIXED bodies are never flagged in the H3-era engines, and Halo 2 still erases
// objects past +/-32768 wu and AI units that reach terminal velocity outside the world - all left untouched.
//
// ⚠ THE SITE IS FOUND BY SIGNATURE, never by RVA: one unique match per image, every relative operand wildcarded (and
// the patched opcode itself, so a patch left by an earlier session can be adopted), and the byte at the site is
// re-checked (E8 call / 0x75 jnz = stock, 0F / EB = ours) before anything is written. CER has no version resource at
// all, and signatures also let the patch work on builds we have no dll for. We restore the bytes we READ, and only if
// the site still holds exactly what we wrote.
//
// ⚠ A body outside the broadphase collides with nothing out there - that is the point - so the world will not catch an
// object again until it comes back inside. Objects that keep falling are still erased by the separate +/-32768 wu
// position limit (Halo 2) - untouched on purpose.
// ================================================================================================================
class HavokBroadphaseBypass : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HavokBroadphaseBypass(GameState game, IDIContainer& dicon);
	~HavokBroadphaseBypass();
	std::string_view getName() override { return nameof(HavokBroadphaseBypass); }
};
