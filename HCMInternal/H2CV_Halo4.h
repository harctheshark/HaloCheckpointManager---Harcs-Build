#pragma once
// Halo 4 (halo4.dll), MCC build 1.3528 (PE timestamp 0x68A0E7BF) - what the bounce viewer reads of it (2026-11-21).
// Layout found statically (halo4.dll IDB) and confirmed live: Documents\Halo Mod And Tools\H4 Collision Direct Read\ (probes) and
// the memory note h4-collision-direct-read. Halo 4 keeps Reach's collision_bsp layout and segmented tag addresses for the
// STRUCTURE; its instanced geometry has no collision_bsp at all (Havok shapes only - not read here yet).
// There is no Halo 2 body in Halo 4 (no h3sb): the viewer shows Halo 4's own player.
// All reads are SEH-guarded (Mem.h).
#include "H2CV_Halo2.h"
#include "H2CV_Collision.h"
#include <cstdint>
#include <string>
#include <vector>

namespace h4
{
	uintptr_t base();           // halo4.dll module base (0 when not loaded)
	bool buildOk();             // PE timestamp matches the 1.3528 build

	// the player camera ([halo4+0x27C5F70]+0x47BED4: position, forward +0x28, up +0x34, field of view +0x78 - horizontal for a
	// 16:9 picture, converted to vertical here)
	h2::Camera readCamera();                  // the RENDER camera record halo4+0x10BEFCC (vertical fov +0x28) - what the viewer draws with
	h2::Camera readCamera(float aspect);      // the player camera record (0.18 wu off the eye, its fov not the drawn one) - not used
	uint32_t gameTick();        // halo4+0x487C6F0 (60 a second)
	uint32_t activeBspMask();   // halo4+0x4967A18
	std::string mapName();      // the level code (halo4+0x2AFF89F), lower case, file-name safe; "" when unreadable

	// the local player: biped +0x64 (Halo 4 holds it 0.1172 above the collision floor - feet = that minus kFeetDrop), velocity +0x88
	constexpr float kFeetDrop = 0.1172f;
	bool readPlayer(h2::Vec3& feet, h2::Vec3& vel, bool* inVehicle = nullptr);
	// the local player's velocity (biped +0x88, the field HCM's Force Launch writes) - the Halo 2 bounce's launch (2026-11-21)
	bool writePlayerVelocity(const h2::Vec3& vel);
	// the player unit's crouch amount (+0x7E4: 0 standing .. ~0.98 crouched, recorded live 2026-11-21); -1 when unreadable
	float crouchAmount();
	// the local player's biped datum ([halo4+0x4969D40] + 0xA4), 0xFFFFFFFF when none
	uint32_t playerDatum();

	// the game thread's halo4 TLS block (its +0x270 picks the resource view that holds the level): this thread's when it resolves,
	// else found by walking the process's threads (cached). 0 when none.
	uintptr_t gameTls();

	// one Bsp3 per loaded structure bsp in `mask` (0 = all): the structure collision_bsp only (igDefs / igInst empty), with
	// non-collidable-material surfaces and the seam faces between loaded bsps flagged 0x10 (as Reach's reader does)
	// INSTANCED GEOMETRY (2026-11-21): Halo 4 keeps it as Havok shapes only. sub_1800EE43C gives every loaded structure bsp up to
	// 5 fixed rigid bodies (halo4+0xED7A20, bsp * 5 + n) from its resource's Havok packfiles: body 0 = the structure (layer 1),
	// the others hkpStaticCompoundShapes of the placed instances on layers 28 / 29 / 30 (live: the player's layer 11 collides
	// with 1, 28, 29 by the world's hkpGroupFilter, world +0xC0, lookup table +0x4C). igGatherTick walks the bodies the player
	// collides with (not body 0: the structure comes from the collision bsp) through Havok's own shape container interface
	// (getContainer vt +0x70, getFirstKey +0x10, getNextKey +0x18, getChildShape +0x28: the static compound transforms each
	// triangle by its instance on the way out), a slice of keys per tick. GAME THREAD ONLY: those calls write Havok's
	// per-thread profiler stream. igTriangles hands out the last complete gather (world space, 9 floats a triangle).
	void igGatherTick();
	bool igTriangles(uint32_t mask, std::vector<float>& out, uint64_t* version = nullptr, std::string* log = nullptr);
	// the published gather's version for `mask` (0: none yet); it changes only when the triangles themselves change
	uint64_t igVersion(uint32_t mask);
	// the gathered triangles as instanced-geometry collision bsps for the bounce cache: exact duplicates and triangles lying on
	// a structure surface left out, welded (shared edges = seams), each connected piece wound outward (its signed volume), in
	// chunks of at most 4095 surfaces (the bounce scan's instance keys hold 12 bits of surface). Any thread.
	void igToCollBsps(const std::vector<float>& tris, const std::vector<h2cv::CollisionWorld::Bsp3>& structure,
		std::vector<h2cv::CollBsp>& out, std::string& log);
	struct BspStats { int bsps = 0; size_t surfaces = 0, nonCollidable = 0, seamLeftOut = 0, instances = 0; };
	bool readCollisionBsps(uintptr_t gameTls, uint32_t mask, std::vector<h2cv::CollisionWorld::Bsp3>& out, std::string& log, BspStats* stats = nullptr);
}
