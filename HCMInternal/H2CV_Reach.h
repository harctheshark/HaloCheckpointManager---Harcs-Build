#pragma once
// Halo: Reach (haloreach.dll), MCC build 1.3528 (PE timestamp 0x68A0EFE1) - what the collision viewer reads of it.
// Reach has no collision_bsp: the world's collision is Havok (structure MOPP -> hkpBvCompressedMeshShape), decoded by
// HCM's Havok debugger on the game thread (HavokDebuggerImpl.cpp, HavokDebuggerBridge.h) and handed over as triangles.
// The render camera, its clip planes and the game's main depth buffer come from Poorboy's PoorboyWorldLines (Poorboys
// Checkpoint Manager 1.4.0, MIT): camera record haloreach+0xC9FAE0 {pos, forward @+12, up @+24, vertical fov @+0x28,
// viewport int16[4] @+0x38 (top, left, bottom, right), near/far @+0x64/+0x68}; the main depth is surface 9 of the
// rasterizer's surface table (set_depth_stencil_surface 0x2743A4, first-person pass 0x26EA78), reversed float depth.
// All reads are SEH-guarded (Mem.h).
#include "H2CV_Halo2.h"
#include "H2CV_Collision.h"      // CollBsp / CollisionWorld::Bsp3 (the bounce viewer's Reach reader hands back the Halo 3 shapes)
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace reach
{
	uintptr_t base();           // haloreach.dll module base (0 when not loaded)
	bool buildOk();             // PE timestamp matches the 1.3528 build

	// the render camera (Present time: the camera of the frame being presented). vfov in radians; near/far from the record
	h2::Camera readCamera();
	// the viewport the record was built for (pixels; 0 when unreadable) - its aspect ratio
	bool readViewport(int& width, int& height);

	// the game's main depth buffer (surface 9): its texture, one reference for the caller (null when not found or not
	// a depth view of `device`). Reversed float depth (1 at the near plane, 0 far away).
	ID3D11Texture2D* mainDepth(ID3D11Device* device, std::string* why = nullptr);   // why: each lookup step, for the HUD

	// the loaded map, as Poorboy's mod names its world file: the last part of the scenario path at haloreach+0x2A1F517
	// ("...\maps\m50" -> "m50"), lower case; "" when unreadable
	std::string mapName();
	std::string mapNameDiag();      // the raw bytes at both name addresses (when mapName() comes back empty)
	// the structure BSPs loaded right now: bit i = BSP i (currentBSPSet, haloreach+0x4E2FB2C); 0 = unknown / mid-load
	uint32_t activeBspMask();

	// ---- the map's collision, read directly from tag / resource memory (no Havok, no world file) ----
	// Layout: Documents\Halo Mod And Tools\Reach Collision Direct Read\REPORT_REACH_DIRECT_READ.md (from Poorboy's h3sb.dll world
	// builder and haloreach!sub_180014124). Plain Halo-3-style collision_bsp winged-edge data, no compression.
	// Built like Halo 2's CollisionWorld::classify: one line per entry of each collision_bsp's EDGE TABLE (already unique: no
	// welding, no dedupe), coloured by the flags of the surfaces either side; the occluders are the solid surfaces, fanned.
	enum LineKind : uint8_t { Solid = 0, Invisible = 1, Breakable = 2 };
	struct Polys
	{
		std::vector<h2::Vec3> v;            // occluder polygons' vertices, world space (solid surfaces only)
		std::vector<uint32_t> start;        // polygon i = v[start[i] .. start[i+1])
		std::vector<uint8_t> instance;      // per polygon: 1 = instanced geometry, 0 = the structure
		std::vector<uint8_t> bsp;           // per polygon: its structure BSP
		std::vector<h2::Vec3> lineV;        // edges, two points each
		std::vector<uint8_t> lineKind;      // per edge: LineKind
		std::vector<uint8_t> lineInstance;  // per edge
		std::vector<uint8_t> lineBsp;       // per edge
		size_t count() const { return start.empty() ? 0 : start.size() - 1; }
	};
	// every BSP in `mask` (0 = all): the structure and its placed instanced geometry. gameTls = a TLS block (TLS[TlsIndex])
	// whose +0x338 picks a resource view holding the level - the MAIN game thread's (thisThreadTls() on the render path).
	// Any thread (only reads). `log` gets one line per BSP; false when nothing could be read.
	bool readCollision(uintptr_t gameTls, uint32_t mask, Polys& out, std::string& log);
	// the calling thread's haloreach TLS block (TLS[TlsIndex], TlsIndex = u32 haloreach+0xC17B18), 0 if none
	uintptr_t thisThreadTls();

	// the game tick (u32 @ +0x0C of the game-time struct *(gameTLS + 0xA0); tick rate i16 @+6) read through the game thread's TLS
	// block, found by a thread walk (H2CV_ReachOob). 0 when it can't be found / read. Render thread.
	uint32_t gameTick();

	// ---- the bounce viewer's geometry (2026-11-04): the same Bsp3 shape CollisionWorld gives Halo 3 / ODST ----
	// One Bsp3 per LOADED structure bsp (mask bit i = scenario bsp i; 0 = all): `world` = the structure collision_bsp parsed with
	// planes / surfaces (plane, flip, firstEdge, material, flags) / edges (v0 v1 fwd rev left right, CollBsp::kNone for "no
	// surface") / vertices, small (16-bit) or large (32-bit) layout; `igDefs` + `igOk` = the instanced-geometry definitions'
	// collision bsps (by definition index); `igInst` = ALL the bsp's placements in table order (def = -1 for the ones the game
	// does not collide with: render only, no physics block, bad scale / transform / definition) with m[0] scale, m[1..9] the
	// rotation r[0..8] as stored, m[10..12] the position, so that h2cv::igTransform(m, v) = ((x r0 + y r3 + z r6) s + px, ...) -
	// exactly the game's world formula (see the comment in readCollisionBsps).
	// Flags fix-ups (mirroring what Poorboy's h3sb leaves out): surfaces whose collision material (sbsp +0x58, i16 @+20) is < 0
	// get flags |= 0x10, and so do the seam faces between LOADED bsps (a coplanar, opposite-facing face of another loaded bsp
	// holding its centre). Any thread (only reads game memory); gameTls as for readCollision.
	struct BspStats { int bsps = 0; size_t surfaces = 0, instances = 0, instancesLive = 0, nonCollidable = 0, seamLeftOut = 0; };
	bool readCollisionBsps(uintptr_t gameTls, uint32_t mask, std::vector<h2cv::CollisionWorld::Bsp3>& out, std::string& log, BspStats* stats = nullptr);
}
