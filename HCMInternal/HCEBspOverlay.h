#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) structure-BSP overlay.
//
// Draws the level's TRUE structure BSP collision surfaces - by default only the INVISIBLE ones, which are the
// faces that block the player but are never rendered. That is the "where are the walls actually" question, and
// there is no way to answer it in game otherwise: every debug global that would have drawn structure geometry is
// compiled out of the shipped build (debug_structure_invisible, debug_structure, debug_bsp,
// debug_structure_surface_references, the whole collision_debug_* family - all present by NAME with address 0).
//
// WHERE THE GEOMETRY LIVES, AND WHY IT IS NOT WHERE YOU WOULD EXPECT.
//
// The HCE structure_bsp tag root has NO collision bsp block. The struct walk closes exactly at its declared 0x308
// with no such field, and the retained older scenario_structure_bsp_v0 struct (which DOES carry `collision bsp` at
// +0x30) confirms it was moved rather than never existing. It now lives in the BSP's raw RESOURCE:
//
//     structure_bsp + 0x2E8  ->  resource  +0x00 small collision bsp   (u16 indices, 14/12/16 strides)
//                                          +0x0C large collision bsp   (u32 indices, 20/24/20 strides)
//                                          +0x18 instanced geometry    <- DELIBERATELY NEVER READ, see below
//
// EXCLUDING INSTANCED GEOMETRY IS THE ENTIRE +0x18 OMISSION. True structure and instanced geometry are different
// blocks of the same resource, so "true BSP only" is not a filter - it is simply not reading that field.
// ⚠ On a live HCE campaign map the collision is overwhelmingly instanced (a Havok walk measured roughly 3.7M
// instanced triangles against a 239-surface structural shell). So this overlay drawing very little is a REAL
// possible outcome, not a bug - it would mean the geometry in question was authored as instanced. The surface
// count is logged on every rebuild precisely so that case is diagnosable rather than mysterious.
//
// INVISIBLE IS A REAL PER-SURFACE FLAG, not a heuristic. This build carries a genuine surface_flags definition
// shared by both surface record variants: bit0 two-sided, bit1 INVISIBLE, bit2 climbable, bit3 breakable,
// bit4 invalid, bit5 conveyor, bit6 slip, bit7 plane-negated, bit8 pathfinding-only. So unlike the trigger
// overlay's kill/BSP-switch colouring, nothing here is guessed from a name.
//
// DRAWING. Surfaces are edge-ring N-gons, not triangles: walk from surface.first_edge following forward_edge or
// reverse_edge depending on which side the surface sits, then triangulate. ⚠ That winding convention is the
// classic Blam one and is UNVERIFIED on this build, so every ring walk is guarded (iteration cap + closure check)
// and a surface whose ring does not close contributes its edges only. Wireframe therefore always works even if
// the face reconstruction is wrong, which is why wireframe is the default render style.
//
// Coordinates need no conversion at all: collision vertices are Blam world units in the same frame as trigger
// volumes, so they feed HCM's IRenderer3D directly. The 304.8 cm / Y-negation pair only applies to the UE camera.
// ================================================================================================================
class HCEBspOverlay : public IOptionalCheat
{
private:
	class HCEBspOverlayImpl;
	std::unique_ptr<HCEBspOverlayImpl> pimpl;

public:
	HCEBspOverlay(GameState game, IDIContainer& dicon);
	~HCEBspOverlay();
	std::string_view getName() override { return nameof(HCEBspOverlay); }
};
