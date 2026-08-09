#pragma once
#include "pch.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IOptionalCheat.h"

// ================================================================================================================
// Halo Campaign Evolved freecam extras: Theater Camera Noclip and Gimbal Lock Bypass.
//
// Both come from a community trainer config for this exact game build, and both were VERIFIED against the shipped
// HaloSimulation_tag_release.dll before being wired up - every "off" byte string in that config matches the binary
// exactly, including an 11-byte sequence, which is not something that happens by coincidence.
//
// NOCLIP is a single float. The camera's collision distance lives at sim RVA 0x9E56F8; driving it hugely negative
// makes the theater/free camera pass through geometry, and writing 0 restores stock behaviour. No code is patched.
//
// GIMBAL LOCK BYPASS is four byte patches plus one code cave:
//   * Two sites clamp the camera's pitch against a min and a max. The patches neuter those clamps - site 1 by
//     killing the >=max branch and making vmaxss compare a register against itself, site 2 by replacing the
//     vmaxss / cmpltss+blendvps pair with plain moves.
//   * The cave handles wrap-around: it converts pitch to a turn count, wraps it into range, and zeroes three
//     vectors when the camera passes vertical - which is what stops the view flipping when you look straight up
//     or down. It is 144 bytes, position-independent apart from one jump, and re-executes the instruction it
//     displaced before returning.
//
// ⚠ WHY THE CAVE PAGE IS NEVER FREED. A game thread can be executing inside it at any instant - it sits on the
// camera update path, which runs every frame. Freeing it on disable would be the same class of crash the hook
// graveyard exists to prevent (see HookGraveyard.h). Disabling restores the game's original bytes so nothing new
// can enter, and the page is deliberately leaked. One page, once per session.
//
// ⚠ EVERY PATCH IS BYTE-GUARDED. The site must read back exactly the bytes the config says it should before
// anything is written, and the guard is re-checked before restoring. HaloCampaignEvolved ships no version
// resource, so a game update cannot be detected any other way - a mismatch disables the feature and says so
// rather than corrupting instructions.
// ================================================================================================================
class HCEFreecamExtras : public IOptionalCheat
{
private:
	class HCEFreecamExtrasImpl;
	std::unique_ptr<HCEFreecamExtrasImpl> pimpl;

public:
	HCEFreecamExtras(GameState game, IDIContainer& dicon);
	~HCEFreecamExtras();
	virtual std::string_view getName() override { return nameof(HCEFreecamExtras); }
};
