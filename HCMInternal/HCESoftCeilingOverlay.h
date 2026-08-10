#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved soft ceiling overlay.
//
// The HaloCER counterpart of MCC's SoftCeilingOverlay, and deliberately the same feature: draw the invisible
// barrier surfaces that push you back, coloured by what they DO to you. Same three types, same three colour
// settings, same bipeds/vehicles filter, same front/back/both culling - all of which is reuse rather than
// re-invention, because the underlying tag data turned out to be byte-identical in shape.
//
// WHY NONE OF THIS IS GUESSED
// ---------------------------
// HaloSimulation_tag_release.dll ships the Blam tag STRUCT DEFINITIONS, so every offset, every enum option and
// every flag bit was walked out of the binary rather than carried over from MCC and hoped for. The walk closes
// exactly on each declared element size, and the scenario walk additionally lands on three offsets already
// verified from real instructions (structure bsps 0x60, soft ceilings 0x248, trigger volumes 0x278) - so
// nothing between them can be wrong either. Full derivation, including the enum option strings, is in
// InternalPointerData.xml under the SOFT CEILINGS heading.
//
// The two records, joined by name string_id exactly as MCC's GetSoftCeilingData joins them:
//
//     scenario + 0x248   scenario_soft_ceilings_block   flags, name, type   <- the ignore mask and the enable bit
//     sddt     + 0x40    structure_soft_ceiling_block   name, type, tris    <- the geometry
//
// ⚠ THE TYPE IS ON BOTH RECORDS, so colouring does NOT depend on the join succeeding. If a level ever has a
// geometry record with no scenario counterpart, that soft ceiling still draws in its correct type colour; it
// just cannot be filtered by ignore-flags or shown as disabled. MCC's implementation treats that same case as a
// hard error and fails the whole overlay - this one does not, because there is no reason to lose every barrier
// over one unmatched name.
//
// WHAT THIS HAS THAT MCC'S DOES NOT: LIVE ENABLED/DISABLED STATE
// --------------------------------------------------------------
// soft_ceiling_enable(name, bool) toggles a bit in a runtime mask at *(tls + 0x430), indexed by the SCENARIO
// block index, and the consumer returns "this soft ceiling does not exist" whenever the bit is clear (see
// HCEDisableBarriers.cpp, which decoded that mask and owns writing it). So a barrier that a mission script has
// switched off can be shown as switched off instead of drawn as though it were still there - which is the
// difference between "the game let me through here" and "HCM is drawing the wrong thing".
//
// ⚠ DISABLE BARRIERS ZEROES THAT MASK. With it on, every soft ceiling correctly reads as disabled and the
// overlay will draw them all in the disabled colour. That is not a bug and it is the honest answer: the
// barriers really are off. Turn the disabled colour up if you want to keep seeing where they were.
//
// The mask is read through HCEGetPlayerState's game-thread TLS walk, not NtCurrentTeb, for the same reason
// HCEDisableBarriers does it - the render thread's TLS block is a different, wrong one.
//
// COORDINATES: soft ceiling triangle vertices are Blam world units in the same frame as trigger volumes and
// collision geometry, so they feed IRenderer3D directly with no conversion. The 304.8 cm / Y-negation pair
// applies only to the UE camera, never to tag geometry - see HCETriggerOverlay.cpp's header for why mixing
// those two frames is the one thing that must never happen.
//
// RENDERING: the D3D12 IRenderer3D only, via Render3DEventProvider - there is no ImGui fallback, because a
// barrier is an arbitrary triangle soup rather than a handful of boxes. If the 3D renderer is unavailable the
// toggle says so rather than silently drawing nothing.
// ================================================================================================================
class HCESoftCeilingOverlay : public IOptionalCheat
{
private:
	class HCESoftCeilingOverlayImpl;
	std::unique_ptr<HCESoftCeilingOverlayImpl> pimpl;

public:
	HCESoftCeilingOverlay(GameState game, IDIContainer& dicon);
	~HCESoftCeilingOverlay();
	std::string_view getName() override { return nameof(HCESoftCeilingOverlay); }
};
