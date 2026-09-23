#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) INVISIBLE GEOMETRY overlay.
//
// Draws the INSTANCED collision geometry that blocks the player but is never rendered - invisible blockers, the
// sphere-collision shells around props, the tube inside a50's gravity lift. Instanced geometry only: the structure
// BSP is HCEBspOverlay's job, and this overlay never reads it.
//
// WHERE IT LIVES (every struct walked out of this build's own tag definitions; each closes on its declared size)
//
//   structure_bsp (776 B)  +0x1FC  'instanced geometry instances'  block, 148 B each (the PLACEMENTS)
//                          +0x264  render geometry meshes            block, 60 B each (parts count at +0x00)
//                          +0x2E4  raw resources count, +0x2E8 address (item 0 only, as the engine reads it)
//                          +0x300  'use resource items' - must be 0, else the engine reads the resource manager
//   raw resource (36 B)    +0x18   'instanced geometries definitions' block, 324 B each
//   definition (324 B)     +0x08   collision bsp, INLINE, always the SMALL layout (surfaces +0x48 / edges +0x54 /
//                                  vertices +0x60 relative to it; strides 14 / 12 / 16)
//                          +0x140  mesh index (short) into the render geometry meshes
//   placement (148 B)      +0x00 scale, +0x04 forward, +0x10 left, +0x1C up, +0x28 position, +0x34 definition index,
//                          +0x36 flags (bit0 'render only'), +0x64 sphere centre, +0x70 radius, +0x90 name string_id
//   world = position + scale * (x*forward + y*left + z*up)  - the inverse of the engine's own world-to-local
//   transform in its instanced ray test.
//
// WHAT "INVISIBLE" MEANS HERE - MEASURED, NOT GUESSED
//
// The collision surface flag bit1 (INVISIBLE). It is the engine's own meaning: its collision leaf test rejects
// exactly these surfaces when a query asks to ignore invisible geometry, while Havok still builds them, so they
// are solid to the player. Measured on the shipped tag data of all 122 structure BSPs: 364 instanced definitions
// (1,095 placements) carry it, every one on ALL of its surfaces, and every one is a UE collision-only component
// (*spherecollision*, *invisible_blocker*, invisible_collision*). On a50 it finds the gravity-lift tube: bsp
// ship_start_zone, placements named grav_lift_approach_spherecollision1..8, a ring of box panels in the shaft.
//
// Cross-check, OR'd in: a definition whose render mesh has ZERO parts (and is not the pathfinding navmesh). Game-
// wide it selects exactly the same 364 definitions (0 disagreements). It is there so a future build that stops
// setting the flag still works, and it is guarded: a BSP with no render meshes at all, or where more than half the
// definitions would fire, has it disabled (render data stripped), and the census line says so.
//
// Measured NOT to work, so not used: instance flags (bit0 'render only' is the OPPOSITE - drawn, not solid),
// definition flags, pathfinding/imposter policy, part types, collision/shader materials.
//
// The same collision surfaces are stored twice on these definitions (a quad and its plane-negated twin), so faces
// and edges are de-duplicated per definition - drawing both would z-fight and double the blend.
//
// Rebuilt on a WORKER thread whenever the loaded BSP set changes (zone switches included), never on the render
// thread. Every rebuild logs a per-BSP census and every invisible placement by name, so a level where this draws
// nothing is diagnosable rather than mysterious.
// ================================================================================================================
class HCEInvisibleGeometryOverlay : public IOptionalCheat
{
private:
	class HCEInvisibleGeometryOverlayImpl;
	std::unique_ptr<HCEInvisibleGeometryOverlayImpl> pimpl;

public:
	HCEInvisibleGeometryOverlay(GameState game, IDIContainer& dicon);
	~HCEInvisibleGeometryOverlay();
	std::string_view getName() override { return nameof(HCEInvisibleGeometryOverlay); }
};
