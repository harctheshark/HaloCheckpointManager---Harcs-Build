#pragma once
// Halo 3 (halo3.dll) and Halo 3: ODST (halo3odst.dll), MCC build 1.3528 - game + Havok 4.5 access.
//   halo3.dll     md5 3dd4e0b3339fb017e2cc52620a1a3e08  PE timestamp 0x68A0EDFC  SizeOfImage 0x4768000
//   halo3odst.dll md5 35e70ec3b380110eb542e965c76e0104  PE timestamp 0x68A0F232  SizeOfImage 0x4797000
// Static RE: C:\Users\hurri\Documents\Halo Mod And Tools\H3 ODST Collision Viewer Research\REPORT_H3_ODST.md and the notes in
// "H3 ODST Collision Viewer Work\NOTES.md". Every offset is per game (struct Layout in Halo3.cpp) - ODST is the same engine code
// with shifted offsets, never port by offset. All reads are SEH-guarded (Mem.h).
//
// Tag memory: a tag "pointer" is a u32 count/offset pair {count, offset}; the address is  tagHeap + 4 * offset  (the engine
// adds the offset to a HINSTANCE-typed base, i.e. x4 - the research report's "tagHeap + u32" is that value x4).
#include "H2CV_Halo2.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace h3
{
	using h2::Vec3;

	// runtime object header type byte (HCM ObjectTypes.h Halo3ObjectType; ODST has the same order)
	enum ObjType : uint8_t { Biped = 0, Vehicle = 1, Weapon = 2, Equipment = 3, Garbage = 4, Projectile = 5, Scenery = 6,
	                         Machine = 7, Control = 8, SoundScenery = 9, Crate = 10, Creature = 11, Giant = 12, EffectScenery = 13 };

	bool odst();                // the attached game is ODST
	uintptr_t base();           // module base of halo3.dll / halo3odst.dll (0 when not loaded)
	bool buildOk();             // PE timestamp + SizeOfImage match the 1.3528 build this was reversed on (cached per module base)
	uint32_t gameTick();
	std::string levelCode();

	// ---- cameras / render ----
	h2::Camera readObserverCamera();                        // Present-time fallback only (FOV assumed vertical)
	// Per-view render struct passed to the first-person pass (rcx): camera @view+8 {pos, forward @+12, up @+24, vertical fov @+40,
	// near/far @+100/+104}, render_projection @view+152 {world_to_view real_matrix4x3 @+0, projection 4x4 @+120}. The projection is
	// already built for the depth convention in use (reversed-Z when the engine byte is set), so it is used as stored.
	bool readViewFrame(uintptr_t view, h2::GameFrame& out);
	bool depthReversed();                                   // engine byte: reversed-Z in use (D32_FLOAT_S8X24 depth set)
	uintptr_t viewUser(uintptr_t view);                     // user / output index of a view (view+10228), 0xFFFFFFFF when unreadable

	// ---- threads ----
	// Some engine tables (breakable-surface rows, resource-backed collision bsps) are reached through the module's implicit-TLS
	// block. captureSimTls() is called from a sim-thread callback (HCM's game tick event); readers use that block when it was
	// captured, else the calling thread's own block.
	void captureSimTls();
	bool haveSimTls();

	// ---- tags ----
	uintptr_t heap();                                       // tag heap base
	uintptr_t scenario();                                   // scenario tag data
	uintptr_t tagData(uint32_t tagIndex);                   // tag instance array -> tag data address (tag datum / index low word)
	// tag block {u32 count, u32 offset}: first element address (heap + 4*offset) and element count; false if unreadable
	bool block(uintptr_t blockAddr, uintptr_t& first, uint32_t& count);

	// ---- structure bsps ----
	uint32_t activeBspMask();                               // bit i set = scenario structure bsp i is loaded / has physics
	struct BspRes
	{
		int index = -1; uintptr_t sbsp = 0;
		uintptr_t data = 0;                                 // collision resource struct (variant @0; small bsp @+4; large @+0x10; IG defs @+0x1C)
		uintptr_t worldBsp = 0; bool large = false;         // collision_bsp struct (8 blocks of {count, offset, def}) and its variant
		uintptr_t igDefs = 0; uint32_t igDefStride = 0;     // instanced-geometry definitions inside the resource; each holds an inline SMALL bsp at +20
		uintptr_t igInstances = 0; uint32_t igInstanceCount = 0;   // sbsp instance block, 0x78-byte records: real_matrix4x3 @+0, definition index u16 @+52
		std::string how;                                    // resolution path, for the log
	};
	// Resolve scenario structure bsp `index` to its collision data. `why` receives the failure reason.
	bool resolveBsp(int index, BspRes& out, std::string* why = nullptr);
	uintptr_t igDefBsp(const BspRes& r, uint32_t defIndex);
	// BSP SEAMS (2026-11-02): the closing faces at a structure bsp's border with its neighbour (level transitions). A collision
	// material's seam index (sbsp collision materials +0x28, 0x18-byte rows, i16 @+0x14) points into the sbsp's seam identifiers
	// (+0x10, 0x28-byte rows, a 16-byte id @+0); a surface of that material is no collision while the bsp holding the same id is
	// loaded too (h3sb leaves exactly these out: "seam surfaces (borders with other loaded BSPs) left out"). Same layout in ODST.
	struct SeamInfo { std::vector<int16_t> matSeam; std::vector<std::array<uint32_t, 4>> ids; };
	bool seamInfo(uintptr_t sbsp, SeamInfo& out);

	// Breakable-surface row of (bsp, group): 12 dwords = 384 bits, bit set = intact. False when the bsp has no row (nothing breakable /
	// pool unreadable) - callers then treat every breakable as intact.
	bool breakableRow(int bsp, uint32_t group, uint32_t out[12]);

	// kill volumes: trigger volume boxes of scenario kill triggers (corner position + 3 orthonormal axes + full extents)
	struct VolumeBox { Vec3 fwd{}, up{}, pos{}, ext{}; };
	bool killVolumes(std::vector<VolumeBox>& out);

	// ---- out-of-bounds / kill volume deaths (Halo 3 and ODST) ----
	// OUT OF BOUNDS (player update sub_1800E4E58, counter write at +0xE55BF - confirmed in CE):
	//   counter player+0x45 (byte), players data array = *(TLS + 0x10), player+0x28 = unit
	//   while the counter is 0 the test only runs on ticks where (tick + player index) % 16 == 0, then every tick
	//   test: the unit's physics point AND that point + 0.2 wu up are both inside solid (collision flags 0x14809)
	//   +1 per tick that holds; UNLIKE Halo 2 it is NOT reset when you get back out - it only clears when the unit is
	//   gone (death / respawn). So the 16 ticks are a budget for the whole life.
	//   kill when counter > round(0.5 s * ticks/s) (sub_1800F3A88) -> sub_18034D520(unit)
	// KILL VOLUMES (sub_1801B8E00): one volume tested per object per tick: (tick + (object index & 0xFFFF)) % count,
	//   count = scenario +0x484, disabled bits *(TLS + 0x418) (set = disabled), test sub_18016F644(trigger, object)
	struct Death
	{
		bool valid = false;
		int counter = 0, limit = 15, ticksPerSec = 30, nextCheck = 0; uint32_t playerIndex = 0;
		int kvCount = 0, kvEnabled = 0, kvChecking = -1;
		std::vector<std::pair<int, int>> kvInside;   // (kill volume, ticks until its turn)
		std::string note;                            // why it is not valid / how the player was found
	};
	bool readDeath(Death& out);
	// soft ceilings / soft kills / slip surfaces of a structure bsp (its structure design 'sddt' tag): triangles, type = ceiling type word
	struct SoftTri { Vec3 v[3]; uint16_t type = 0; };
	bool softCeilings(int bspIndex, std::vector<SoftTri>& out);

	// ---- objects / players ----
	void enumerateObjects(std::vector<h2::ObjectRef>& out);
	uintptr_t objectFromDatum(uint32_t datum);
	uintptr_t localPlayerUnitObject();                      // user 0's unit (HCM "playerDatum")
	int localPlayerUnits(uintptr_t out[4]);                 // by user slot; only slot 0 is wired (see NOTES.md)
	uint32_t objectDefinitionTag(uintptr_t obj);
	bool objectHidden(uintptr_t obj);

	// ---- Havok 4.5 ----
	uintptr_t world();                                      // hkpWorld*
	struct WorldTime { float tol = 0, stepStart = 0, stepEnd = 0; bool ok = false; };
	WorldTime worldTime();                                  // collision tolerance + current step interval (hkpProcessCollisionInput)
	int havokBodies(uintptr_t obj, uintptr_t out[8]);       // rigid bodies of an object's havok component
	// motion: transform @+0x110 (3 rotation columns + translation @+0x140), centre of mass 0 / 1 @+0x150 / +0x160, linear velocity @+0x1E0
	struct MotionState { bool ok = false; uint8_t type = 0; Vec3 com0{}, com1{}, v{}, w{}; h2::Xf xf; };
	bool readMotion(uintptr_t body, MotionState& out);
	h2::Capsule bodyCapsuleLocal(uintptr_t body);           // the body's shape if it is a capsule (body-local), else !ok
	int shapeVtRva(uintptr_t body);                         // diagnostics: vtable RVA of the body's top shape, -1 unreadable

	// ---- object shapes (scenery / crates / machines) ----
	struct ShapeLines { std::vector<h2::Vec3> a, b; };      // world-space line list (a[i] -> b[i])
	// Appends the body's shape (box, sphere, capsule, convex hull, lists, transform wrappers) as world-space lines. Returns the number of
	// shapes it could not draw (MOPP-backed shapes need the engine's own shape-container calls).
	int bodyShapeLines(uintptr_t body, ShapeLines& out);

	// ---- TIM / ghost sampling (sim thread preferred) ----
	// One 1n-track child of a (body, environment mopp) pair. cmd & 0xE == 4 means the child carries a separating-normal cache.
	struct Tim3
	{
		uint32_t key = 0; uint8_t cmd = 0, agent = 0; bool hasTim = false;
		float time = -1.f;                   // time stamp the cached normal / w refer to (bit pattern -1 = invalid)
		Vec3 n{}; float w = 0;               // cached separating normal and distance along it
	};
	struct Pair3
	{
		bool found = false; uintptr_t entry = 0, partner = 0, partnerEntity = 0;
		int bsp = -1;                        // scenario structure bsp index of the environment shape (-1 unknown)
		uint8_t agentType = 0, cmd = 0; int sectors = 0;
		bool verified = false;               // the entry's collidable pointers matched the expected layout
		std::string note;
		std::vector<Tim3> kids;
	};
	struct Ghost3
	{
		bool ok = false; std::string note;
		uintptr_t obj = 0, body = 0;
		MotionState m; Vec3 halo{}; bool haloOk = false;
		h2::Capsule cap;                     // body-local capsule
		int motionShapeVt = -1;
		std::vector<Pair3> pairs;
		WorldTime t;
		uint32_t tick = 0;
	};
	bool sampleGhost(uintptr_t unit, Ghost3& out);
	void dumpPairBytes(const Ghost3& g, std::string& out);  // hex dump of the first pair entry + sector (diagnostics for the log)
}
