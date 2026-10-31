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
}
