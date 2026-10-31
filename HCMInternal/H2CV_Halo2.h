#pragma once
// Halo 2 (MCC, halo2.dll build 1.3528, PE timestamp 0x68A0F0F2) game + Havok 2 access.
// Every offset below was verified in the decompiled code during the ghosting investigation
// (C:\Users\hurri\Documents\Halo Mod And Tools\H2 Ghosting Research\notes\, esp. G8-live-logger.md, E-halo-bridge.md).
// All reads are SEH-guarded (Mem.h): the render thread reads while the simulation thread writes.
#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#include <DirectXMath.h>

namespace h2
{
	using Vec3 = DirectX::XMFLOAT3;

	namespace rva
	{
		constexpr uintptr_t PLAYERS_GLOBALS = 0xE80A20;  // local player datum = *(u32*)(pg + 28 + 4*user)
		constexpr uintptr_t PLAYERS_ARRAY   = 0xE80A28;  // data array, 548-B elements, +44 = unit datum
		constexpr uintptr_t OBJ_HEADER      = 0x18B7398; // data array, 12-B entries {salt u16, ?, type u8 @+3, data offset u32 @+8}
		constexpr uintptr_t OBJ_POOL        = 0x18B7360; // object = offset + ((pool + 0x57) & ~0xF)
		constexpr uintptr_t HAVOK_COMPS     = 0x1648940; // data array, 192-B havok components
		constexpr uintptr_t WORLD           = 0x15FDFE0; // hkWorld*
		constexpr uintptr_t BSP_ENTITY      = 0xDFCBC8;  // structure-BSP hkRigidBody* (fixed)
		constexpr uintptr_t MESH_ENTITY     = 0xDFCBD0;  // MCC static mesh hkRigidBody* (fixed)
		constexpr uintptr_t GAME_TIME       = 0x15FE008; // +2 i16 ticks/s, +8 u32 tick
		constexpr uintptr_t SCENARIO        = 0xE6F768;  // scenario tag data* (trigger volumes block +0x108, kill volumes block +0x230)
		constexpr uintptr_t KILL_DISABLED   = 0x1650F18; // u32* bit vector: bit set = kill volume disabled (kill_volume_disable)
		constexpr uintptr_t OOB_LIMIT_SEC   = 0xC3273C;  // float 0.5: seconds inside solid geometry before the kill
		constexpr uintptr_t FN_TRIGGER_TEST = 0x678FD0;  // bool __fastcall(int16 trigger_volume, uint32 object_datum)
		constexpr uintptr_t SBSP            = 0xE6F770;  // structure_bsp tag data*
		constexpr uintptr_t TAG_POOL_A      = 0xE80AB0;  // tag data pools (tag block dataOffset >= 0 -> A, < 0 -> B)
		constexpr uintptr_t TAG_POOL_B      = 0xE80AC0;
		constexpr uintptr_t META_HEADER     = 0x15E4B58; // *-> tag meta header (tag instance table)
		constexpr uintptr_t CAMERA          = 0x15F297C; // camera struct (no deref): pos +0, fwd +0x20, up +0x2C, hfov +0x38
		constexpr uintptr_t VERTICAL_FOV    = 0x1997714; // float radians
		constexpr uintptr_t NEAR_CLIP       = 0x19948C4;
		constexpr uintptr_t FAR_CLIP        = 0x19948C8;
		// render frame (notes\W1-render-frame.md)
		constexpr uintptr_t FRAME           = 0x1996A10; // current s_frame (792 B, no getter); camera @+0x18, projection @+0x100
		constexpr uintptr_t FRAME_TEXCAM    = 0x1996A17; // s_frame+7 is_texture_camera (skip those views)
		constexpr uintptr_t FRAME_CAMERA    = 0x1996A28; // render_camera: point, forward, up, ..., vfov +0x28, viewport i16 top,left,bottom,right +0x30, near +0x40, far +0x44
		constexpr uintptr_t FRAME_W2V       = 0x1996B10; // render_projection.world_to_view (real_matrix4x3: scale, forward, left, up, position)
		constexpr uintptr_t FRAME_PROJ      = 0x1996B88; // render_projection.projection_matrix (4x4, row-vector, STANDARD z; the depth buffer stores 1 - z/w)
		constexpr uintptr_t FP_PASS         = 0x7E0C60;  // sub_1807E0C60 first-person pass: at ENTRY the world depth is complete (biped incl.), bloom done, no FP/HUD yet
		constexpr uintptr_t SCENE_RTV       = 0x197EE60; // ID3D11RenderTargetView* scene colour (RGBA8, LDR)
		constexpr uintptr_t SCENE_DSV       = 0x197EE70; // ID3D11DepthStencilView* world depth (D32_FLOAT_S8X24_UINT, reversed-Z)
		// breakable surfaces (world BSP): sub_18071ABE0(instance -1, index) reads bit `index` of row
		// block + 1 + ctx*32 (bit set = intact); sub_18071AC80(-1, index, 0) clears it when the glass is destroyed
		constexpr uintptr_t BREAKABLE_BLOCK = 0x1648960; // qword ptr to the runtime block (byte 0 = enable flag)
		constexpr uintptr_t BREAKABLE_CTX   = 0xDF9D74;  // i16 structure-BSP context row (-1 = none)
		// baked scenery in the level shape (getChildShape key categories 3 / 4, sub_180793760)
		constexpr uintptr_t SCENERY_TABLE   = 0x165B440; // qword ptr to u32[] : object datum per scenery index (key & 0xFFFF)
		constexpr uintptr_t TAG_INSTANCES   = 0x15E4B30; // qword ptr to the tag instance array (16 B entries, data offset i32 @+8)
		constexpr uintptr_t TAG_DATA_BASE   = 0x15E4B38; // qword: tag data base the instance offsets are relative to
	}
	namespace vt
	{
		constexpr uintptr_t RIGID_BODY      = 0xB71058;
		constexpr uintptr_t STAB_BOX_MOTION = 0xB73080;
		// the other hkRigidMotion subclasses a phmo rigid body can get (sub_1809CEDB0 switch on the motion type); all share
		// the hkRigidMotion layout (v +0x50, PREV +0x70, CUR +0x80, rotation columns +0x90, origin +0xC0), see Halo2.cpp readMotion
		constexpr uintptr_t SPHERE_MOTION      = 0xB71088; // hkSphereMotion           (vInt @+0xF0)
		constexpr uintptr_t STAB_SPHERE_MOTION = 0xB71188; // hkStabilizedSphereMotion (vInt @+0xF0)
		constexpr uintptr_t BOX_MOTION         = 0xB72F80; // hkBoxMotion              (vInt @+0xD0)
		constexpr uintptr_t KEYFRAMED_MOTION   = 0xB73180; // hkKeyframedRigidMotion
		constexpr uintptr_t FIXED_MOTION       = 0xB73288; // hkFixedRigidMotion
		// pair-agent containers a multi-shape body (vehicle) goes through before the leaf welder agents
		constexpr uintptr_t LIST_AGENT         = 0xB6F478; // hkListAgent (child agents: array @+0x18, count @+0x20, parallel to the list shape's children)
		constexpr uintptr_t LIST_AGENT_S       = 0xB6F4C0; // hkSymmetricAgent<hkListAgent>
		constexpr uintptr_t TRANSFORM_AGENT    = 0xB6F3E8; // hkTransformAgent (nested agent @+0x18)
		constexpr uintptr_t TRANSFORM_AGENT_S  = 0xB6F430; // hkSymmetricAgent<hkTransformAgent>
		constexpr uintptr_t MOPP_AGENT         = 0xB6F8B8; // hkMoppAgent (children {key u32, pad, agent} 16 B array @+0x18, count @+0x20; key = list child index)
		constexpr uintptr_t MOPP_AGENT_S       = 0xB6F900; // hkSymmetricAgent<hkMoppAgent>
		constexpr uintptr_t WELDER_AGENT    = 0xB6F788;
		constexpr uintptr_t WELDER_AGENT_S  = 0xB6F7D0;
		constexpr uintptr_t SWEEP_AGENT     = 0xB6E1C8;
		constexpr uintptr_t GSK_AGENT       = 0xB6ECB0;
		constexpr uintptr_t NULL_AGENT_INST = 0xE1A510;
		// further agent classes (RTTI, halo2.dll 1.3528). Only the ones above carry TIMs we can read; these are named in the
		// info panel when an object pair ends in one of them.
		constexpr uintptr_t CAPSULE_CAPSULE_AGENT = 0xB6F0D8; // hkCapsuleCapsuleAgent (no cached separating distance)
		constexpr uintptr_t BOX_BOX_AGENT         = 0xB6ED28; // hkBoxBoxAgent
		constexpr uintptr_t BV_AGENT              = 0xB6EDF8; // hkBvAgent
		constexpr uintptr_t BV_AGENT_S            = 0xB6EE70;
		constexpr uintptr_t BVTREE_AGENT          = 0xB6F6D0; // hkBvTreeAgent
		constexpr uintptr_t BVTREE_AGENT_S        = 0xB6F718;
		constexpr uintptr_t SHAPE_COLL_AGENT      = 0xB6F508; // hkShapeCollectionAgent
		constexpr uintptr_t SHAPE_COLL_AGENT_S    = 0xB6F550;
		constexpr uintptr_t CONVEX_WELDER_AGENT   = 0xB6F640; // hkConvexWelderAgent (not the ConvexConvex one above)
		constexpr uintptr_t CONVEX_WELDER_AGENT_S = 0xB6F688;
		constexpr uintptr_t SPHERE_WELDER_AGENT   = 0xB6F828; // hkSphereConvexWelderAgent
		constexpr uintptr_t SPHERE_WELDER_AGENT_S = 0xB6F870;
		constexpr uintptr_t MULTISPHERE_AGENT     = 0xB6F5B0; // hkMultiSphereAgent
		constexpr uintptr_t MULTISPHERE_AGENT_S   = 0xB6F5F8;
		// Halo's shape collection over ONE collision_bsp (ctor sub_180791F60, getType 2): +0x18 = collision_bsp*,
		// getChildShape sub_180792E30 builds the triangle of surface `key` (the key IS the surface index). Used for every
		// collision-model / instanced-geometry physics block: block+0x00 this collection, +0x70 a MOPP over it (child +0x18),
		// +0x50 an hkConvexWelderShape (type 20) whose +0x18 is that MOPP (sub_18077AFC0).
		constexpr uintptr_t COLL_BSP_SHAPE        = 0xB3C538;
	}

	// object types (runtime header byte)
	enum ObjType : uint8_t { Biped = 0, Vehicle = 1, Weapon = 2, Equipment = 3, Scenery = 6, Machine = 7, Crate = 11 };

	uintptr_t base();
	// true when the loaded halo2.dll matches the build every offset was verified on (vtable-slot self-check)
	bool buildOk();

	struct Camera { Vec3 pos{}, fwd{}, up{}; float vfov = 0, nearClip = 0, farClip = 0; bool ok = false; };
	Camera readCamera();

	// the game's own per-view parameters, read inside the render pass
	struct GameFrame
	{
		bool ok = false, textureCamera = false;
		float w2v[13]{};      // world_to_view
		float proj[16]{};     // projection 4x4 (row-vector)
		int16_t vp[4]{};      // top, left, bottom, right
		Vec3 pos{}, fwd{}, up{}; float vfov = 0, nearClip = 0, farClip = 0;
	};
	bool readFrame(GameFrame& out);

	// ---- tag data ----
	struct TagPools { uintptr_t a = 0, b = 0; };
	bool tagPools(TagPools& out);
	uintptr_t resolveTagOffset(int32_t dataOffset, const TagPools& p);
	// tag_block {u32 count, i32 dataOffset}
	bool resolveBlock(uintptr_t blockAddr, const TagPools& p, uintptr_t& first, uint32_t& count);
	uintptr_t structureBsp();
	// world BSP breakable state: 256 bits (bit set = intact). False when unreadable (no level, context -1);
	// callers then treat every breakable as intact.
	bool readWorldBreakableRow(uint32_t out[8]);
	uintptr_t tagData(uint16_t tagIndex, const TagPools& p); // via the meta-header tag instance table

	// ---- objects ----
	// hdrFlags = object header entry byte +2: 0x10 = being deleted (object_delete, sub_1808D4D10 `or [hdr+2], 10h`;
	// every object walk in the engine skips it, e.g. sub_1808D70C0 / sub_180794590 type 3-4)
	struct ObjectRef { uintptr_t obj; uint32_t datum; uint8_t type; uint8_t hdrFlags; };
	void enumerateObjects(std::vector<ObjectRef>& out);
	uintptr_t objectFromDatum(uint32_t datum);
	// world-space node matrix of an object (real_matrix4x3: scale, forward, left, up, position), current sim pose.
	// false when the object has no node block, the index is out of range, or the read faults
	bool objectNodeMatrix(uintptr_t obj, uint16_t node, float out[13]);
	// object flags u32 @obj+4 bit 0 = hidden (sub_1808D70C0 / sub_1808D7130: `test byte [obj+4], 1`)
	bool objectHidden(uintptr_t obj);
	// Runtime region state (sub_1808D6940): i16 size @obj+0x11C, i16 offset @obj+0x11E, count = size / 10, array of
	// `count` bytes at obj+off = CURRENT model-permutation index of each model (hlmt) region, 0xFF/-1 = none.
	// Collision itself (sub_180794590, key types 3/4) accepts a bsp only when state[region] == its permutation.
	// Returns the region count copied into out (<= maxN); 0 when the object has no state block / unreadable.
	int objectRegionState(uintptr_t obj, int8_t* out, int maxN);
	uintptr_t localPlayerUnitObject();
	// every local player's unit object by user slot 0..3 (0 = empty slot); returns the number found.
	// Table = `*(u32*)(players_globals + 28 + 4*user)` (sub_18069CFF0), four dwords (the sibling table at +12 is the
	// other 4-entry array, the counts live at +8/+10). Falls back to localPlayerUnitObject() in slot 0.
	int localPlayerUnits(uintptr_t out[4]);

	// ---- vehicles ----
	// The Vehicle object (header type 1) a unit is attached to, or 0. The unit's parent object datum is obj+0x14
	// (object_attach, sub_1808D2050: child+0x14 = parent datum, child+0x18 = marker index; sub_1808D46E0 walks it up to
	// the root). Any seat (driver / passenger / gunner) counts. Walks up to 4 parents so a unit on a seated turret still
	// resolves. *vehicleDatum receives the vehicle's object datum.
	uintptr_t vehicleOfUnit(uintptr_t unit, uint32_t* vehicleDatum = nullptr);
	// object definition tag: index (low word of the u32 at obj+0) and group (u32 of the tag instance, 0 if unreadable)
	uint32_t objectDefinitionTag(uintptr_t obj, uint32_t* group = nullptr);

	// ---- Havok ----
	uintptr_t havokEntityForObject(uintptr_t obj, bool wantDynamic = true);
	struct Capsule { Vec3 a{}, b{}; float r = 0; bool ok = false; };
	// world-space capsule of an entity whose shape is (sweep-wrapped) hkCapsuleShape, at Havok's current pose
	Capsule entityCapsule(uintptr_t entity);

	// hkRigidMotion state of a dynamic entity (all read in ONE copy of motion+0x30..+0xE0)
	//   +0x30 local centre of mass, +0x50 v, +0x70 PREV.xyz/.w, +0x80 CUR.xyz (centre of mass) / time (+0x8C),
	//   +0x90/+0xA0/+0xB0 rotation columns, +0xC0 transform translation (= body origin = CUR - R*com, sub_1809DFEB0),
	//   +0xD0 vInt (box motions) / +0xF0 (sphere motions)
	struct Motion
	{
		enum Kind : uint8_t { None, StabBox, Box, StabSphere, Sphere, Keyframed } kind = None;
		Vec3 com{}, v{}, prev{}, cur{}, origin{}, vint{};
		Vec3 rc[3]{ {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };   // rotation matrix columns (body-local -> world)
		float prevW = 0, time = 0; bool ok = false;
	};
	const char* motionKindName(Motion::Kind k);
	// anyDynamic = false: only the biped's hkStabilizedBoxMotion (as before). true: every dynamic motion class a phmo body
	// can have (sphere / stabilized sphere / box / stabilized box / keyframed); fixed motions are always rejected.
	bool readMotion(uintptr_t entity, Motion& out, bool anyDynamic = false);

	// Convex = hkConvexSweepConvexAgent / hkGskConvexConvexAgent: a leaf that carries a TIM (same layout as a welder child)
	enum class AgentKind : uint8_t { Unknown, Welder, List, Transform, Mopp, Convex };
	AgentKind agentKind(uintptr_t agent);
	const char* agentClassName(uintptr_t agent);   // short Havok class name for the info panel ("?" if unknown)
	struct PairInfo { uintptr_t entry = 0, agent = 0; int32_t S = 0; bool found = false; bool agentIsWelder = false; AgentKind kind = AgentKind::Unknown; };
	PairInfo findBspPair(uintptr_t entity);

	struct Child
	{
		uint32_t key = 0; uintptr_t agent = 0;
		enum Kind : uint8_t { Sweep, Gsk, Null, Other } kind = Other;
		int32_t manifold = 0; uint8_t dimA = 0, dimB = 0;
		uint16_t ids[4]{};   // GSK cache vertex ids (child+0x18): A ids first, then B (polygon ring indices)
		Vec3 normal{}; float tim = 0;
		// polygon feature the TIM / last GSK run refers to: -1 none, 0 vertex, 1 edge, 2 face; featA/featB ring indices
		int feature = -1; int featA = -1, featB = -1;
	};
	bool readChildren(uintptr_t welderAgent, std::vector<Child>& out);
	// The AABB the welder last queried its MOPP with (child update sub_1809A9AC0): the convex body's AABB in the
	// other body's space, grown by 1.5 x the collision tolerance (input+0x10 * (1 + 0.5)), stored at agent +0x30 (min)
	// / +0x40 (max). A polygon gets a child exactly when its MOPP bounds overlap this box. Re-queried only when a face
	// of the fresh box has moved more than 0.5 x tolerance from the stored one, so the stored box IS the last query.
	bool readWelderQueryBox(uintptr_t welderAgent, Vec3& mn, Vec3& mx);
	// the world's collision tolerance (hkWorld +0x120 -> collision input +0x10; the welder and the broadphase both grow
	// their boxes by multiples of it: broadphase insertion sub_1809D1650 uses 0.5x). 0 when unreadable.
	float collisionTolerance();
	// The body's box in the world broadphase (hk3AxisSweep at hkWorld +0x110, vtable 0xB764F0) - the box that decides
	// whether a pair with another body exists. Decoded like sub_1809FC760: nodes {u16 minY,minZ,maxY,maxZ,minX,maxX; pad;
	// handle*} 24 B at +0x40 (count +0x48); endpoint arrays X/Y/Z at +0x50/+0x60/+0x70 (4 B: u16 value, u16 node);
	// coordinate = value / scale(+0x30) - offset(+0x10). The body's handle is its collidable +0x20 (= entity +0x38),
	// whose first u32 is normally the node index (verified against the node's handle pointer, else searched).
	bool broadphaseAabb(uintptr_t entity, Vec3& mn, Vec3& mx);

	// Per-level grid numbers (Collision Viewer info panel).
	//   Broadphase (hk3AxisSweep): coordinates are stored as 16-bit integers q = (p + offset) * scale; a box's min is rounded
	//     DOWN to an even value and its max UP to an odd one (sub_1809F94D0: & 0xFFFE / | 1), so boxes snap outward to a grid
	//     of 2 / scale wu, and the whole range covers 65535 / scale wu per axis.
	//   Level MOPP (the level shape's tree, code at shape +0x20: origin xyz +0x0, scale +0xC; sub_1809BC420): coordinates are
	//     integers q = (p - origin) * scale; the query starts at 65536 / scale wu cells and each deeper tree level refines
	//     by 256x, down to 1 / scale.
	struct LevelGrids
	{
		bool bpOk = false; Vec3 bpMin{}, bpMax{}, bpCell{};
		bool moppOk = false; Vec3 moppOrigin{}; float moppScale = 0;
		float tolerance = 0;
	};
	bool levelGrids(LevelGrids& g);

	// Backstep parameters (hkWorld, read by the island backstep pass sub_1809E3110 / solver sub_1809E38A0):
	//   For each pair, S = entry +0x18. With S >= 0 the pair is re-collided at the step's end and a backstep happens when a
	//   contact is DEEPER than the threshold  thr(S) = base + S * perS  (contact distance < thr). Then S += 1 and the solver
	//   bisects the step's time (halving from 0.5 until the step is <= timeRes) for the latest time t at which no contact is
	//   below thr(S) (the new S), and rewinds the body to t (motion setTime, vtable +0xE8). Rewound fraction f = 1 - t.
	//   If even the earliest time still penetrates: S = -3 (stuck). With no contact below thr(S - 1), S is lowered by one.
	//   base = world +0x2C4, perS = world +0x2C8, timeRes = world +0x2CC.
	struct BackstepParams { float base = 0, perS = 0, timeRes = 0; bool ok = false; };
	bool backstepParams(BackstepParams& p);
	// one sweep / GSK agent read on its own (an object pair leaf); false if the agent is not one of those
	bool readConvexAgent(uintptr_t agent, Child& out);
	// Offset save state: write a saved TIM state into a live convex agent (sweep / GSK): the GSK simplex cache (ids +
	// dims, agent+0x18..+0x21), the cached separating normal (+0x30) and the TIM (+0x3C). Refuses anything that is not a
	// convex agent, and agents that are in contact right now (manifold > 0: the engine is resolving that pair).
	bool writeConvexState(uintptr_t agent, const Child& saved);

	// ---- objects the subject touches (Believed poses - objects) ----
	// Pair entries of `entity` against every OTHER rigid body (not the level BSP / MCC mesh): crates, machines, vehicles,
	// dynamic scenery, other bipeds. Same pair arrays findBspPair scans.
	struct ObjPair { uintptr_t entry = 0, agent = 0, other = 0; int32_t S = 0; };
	int findObjectPairs(uintptr_t entity, std::vector<ObjPair>& out);
	// object owning a rigid body (scan of the object table + havokBodies); false if none
	bool objectOfEntity(uintptr_t entity, ObjectRef& out);

	uint32_t gameTick();

	// ---- kill volumes (out-of-bounds deaths) ----
	// sub_18075F620(object): ONE kill volume is tested per object per tick, chosen round-robin:
	//     i = (game_time + (object_index & 0xFFFF)) % kill_volume_count
	// skipped when disabled (bit set in *KILL_DISABLED); otherwise its trigger volume is tested against the object
	// (sub_180678FD0: point = object+0x30, local = R^T (p - position), inside when 0 < local < extents) and a hit kills
	// the unit (sub_1809292F8: unit flags |= 0x8000). So inside volume k you die on the first tick with
	// (game_time + object_index) % count == k - between 0 and count-1 ticks after entering.
	struct KillVolume
	{
		int16_t trigger = -1;           // scenario trigger volume index
		bool enabled = true;            // not disabled by script
		bool attached = false;          // trigger volume follows an object (geometry not drawn)
		Vec3 pos{}, fwd{}, left{}, up{}, ext{};   // box = pos + fwd*x + left*y + up*z, 0 <= x,y,z <= ext
	};
	bool readKillVolumes(std::vector<KillVolume>& out);
	// The game's own test (sub_180678FD0(trigger_volume, object datum) -> bool): the same call the kill check makes, so
	// it settles "am I inside" exactly, whatever point / attachment rules it uses. Read-only; -1 if it faulted.
	int gameTriggerVolumeTest(int16_t trigger, uint32_t objectDatum);
	// the point the game tests for an object (object +0x30)
	bool killTestPoint(uintptr_t object, Vec3& out);

	// Out-of-bounds kill (player update sub_1806A2D80; found with the CE probes): separate from kill volumes.
	//   test   sub_1808F1D10(unit): the unit's Havok body point is inside solid collision (collision test 0x14800005)
	//          - which is what being out of the level is: outside the BSP counts as solid
	//   counter player+0x21C (byte): +1 per tick the test says "inside solid", reset to 0 the first tick it says no
	//   start  while the counter is 0 the test only runs on ticks where (game_time + player index) % 16 == 0, so it can
	//          take up to 15 ticks to notice you; once counting it runs every tick
	//   kill   counter > round(0.5 s * ticks per second) (= 15 at 30 Hz) -> sub_1808FAF30 marks the unit (damage flags
	//          +0x10A |= 0x20) and the object update kills it with the globals' kill damage
	//   => from going out of bounds to the kill: 16 ticks of counting + 0..15 ticks waiting for the first check
	struct OobTimer { bool valid = false; int counter = 0, limit = 15, ticksPerSec = 30, nextCheck = 0; uint32_t playerIndex = 0; };
	OobTimer readOobTimer(int user);
	uint32_t localPlayerUnitDatum(int user);       // NONE (0xFFFFFFFF) when no unit

	// ---- Havok bodies of any object (vehicles) ----
	// Rigid bodies of an object's havok component (up to 8), the component's PRIMARY record first (comp+0x18 = signed index of
	// the record whose first phmo rigid body has the lowest node index, -1 = root; written at the end of sub_18070C400 and read
	// by the vehicle builder sub_18070E000). Returns the count; out[] holds hkRigidBody pointers (dynamic or fixed).
	int havokBodies(uintptr_t obj, uintptr_t out[8]);

	// rigid transform (hkTransform layout): p -> c[0]*x + c[1]*y + c[2]*z + t
	struct Xf { Vec3 c[3]{ {1, 0, 0}, {0, 1, 0}, {0, 0, 1} }; Vec3 t{ 0, 0, 0 }; };
	Vec3 xfPoint(const Xf& x, const Vec3& p);
	Vec3 xfDir(const Xf& x, const Vec3& d);          // rotation only
	Xf xfCompose(const Xf& a, const Xf& b);          // a o b (apply b first)
	Xf motionPose(const Motion& m);                  // body-local -> world
	// the current Havok transform of any rigid body (dynamic, keyframed or fixed motion: motion +0x90..+0xC0)
	bool entityPose(uintptr_t entity, Xf& out);

	// One convex primitive of a body's shape, geometry in BODY-LOCAL space (leaf transform shapes already applied).
	//   capsule (a == b: a sphere): segment a-b grown by r.
	//   polytope (box / convex vertices): core vertices `verts`, outward face rings `faces`, convex radius r. `wire` = the
	//   vertices of the hull grown by r (planes pushed out by r), what is drawn.
	struct ShapeFace { std::vector<uint16_t> ring; Vec3 n{}; };
	struct ShapePrim
	{
		bool capsule = true;
		Vec3 a{}, b{}; float r = 0;
		std::vector<Vec3> verts, wire;
		std::vector<ShapeFace> faces;
		std::vector<std::pair<uint16_t, uint16_t>> edges;   // unique face-ring edges
		int hkType = 0;            // raw hkShape type of the primitive (4 sphere, 6 box, 7 capsule, 8 convex vertices, 9 multi-sphere)
		bool swept = false;        // wrapped in hkConvexSweepShape (23)
		uintptr_t agent = 0;       // pair sub-agent that collides this primitive against the BSP (0 = unknown); check agentKind()
	};
	// a Halo collision_bsp reached through a welder agent: the agent's children are keyed by surface index
	struct BspLeaf { uintptr_t bsp = 0; uintptr_t agent = 0; Xf xf; };
	struct BodyShape
	{
		std::vector<ShapePrim> prims;
		std::vector<BspLeaf> bsps;   // only filled by readShapeTree (objects); readBodyShape callers ignore it
		int skipped = 0;           // shapes of a type the walker does not draw (triangle, MOPP-only mesh, ...)
		std::string desc;          // e.g. "list(10): 3 sweep>convex, 1 box; skipped 2"
		uintptr_t root = 0; int rootType = 0;
	};
	// Walks the entity's shape tree (list / transform / MOPP-BV / sweep wrappers down to sphere, capsule, box, convex
	// vertices, multi-sphere leaves) in parallel with the (entity, BSP) pair agent tree `rootAgent` (0 = no agent info),
	// so every leaf knows which welder agent holds its TIMs. False when no leaf could be read.
	bool readBodyShape(uintptr_t entity, uintptr_t rootAgent, BodyShape& out);
	// same walk from any shape with a starting transform (prims come out in that transform's space, i.e. world space when
	// `xf` is the body / node pose). Also records welder-shape / MOPP / collision-bsp collections reached through a welder
	// agent as BspLeaf. False when nothing was read.
	bool readShapeTree(uintptr_t shape, uintptr_t rootAgent, const Xf& xf, BodyShape& out);

	// Baked scenery in the level shape: a level-welder child of key category 3 (physics model rigid body) or 4 (collision
	// model) -> the shape the engine wraps in a transform for it, and that transform (object node matrix, scale ignored, as
	// sub_180791C10 does). Ported from sub_180793760.
	struct BakedShape { uintptr_t shape = 0, obj = 0; uint32_t datum = 0xFFFFFFFF; uint8_t type = 0xFF; int category = 0; Xf xf; std::string why; };
	bool bakedSceneryShape(uint32_t key, BakedShape& out);
	// the capsule in BODY-LOCAL space (entityCapsule without the pose applied)
	Capsule entityCapsuleLocal(uintptr_t entity);
}
