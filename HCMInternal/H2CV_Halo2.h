#pragma once
// Halo 2 (MCC, halo2.dll build 1.3528, PE timestamp 0x68A0F0F2) game + Havok 2 access.
// Every offset below was verified in the decompiled code during the ghosting investigation
// (C:\Users\hurri\Documents\Halo Mod And Tools\H2 Ghosting Research\notes\, esp. G8-live-logger.md, E-halo-bridge.md).
// All reads are SEH-guarded (Mem.h): the render thread reads while the simulation thread writes.
#include <cstdint>
#include <vector>
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
	}
	namespace vt
	{
		constexpr uintptr_t RIGID_BODY      = 0xB71058;
		constexpr uintptr_t STAB_BOX_MOTION = 0xB73080;
		constexpr uintptr_t WELDER_AGENT    = 0xB6F788;
		constexpr uintptr_t WELDER_AGENT_S  = 0xB6F7D0;
		constexpr uintptr_t SWEEP_AGENT     = 0xB6E1C8;
		constexpr uintptr_t GSK_AGENT       = 0xB6ECB0;
		constexpr uintptr_t NULL_AGENT_INST = 0xE1A510;
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
	uintptr_t tagData(uint16_t tagIndex, const TagPools& p); // via the meta-header tag instance table

	// ---- objects ----
	struct ObjectRef { uintptr_t obj; uint32_t datum; uint8_t type; };
	void enumerateObjects(std::vector<ObjectRef>& out);
	uintptr_t objectFromDatum(uint32_t datum);
	uintptr_t localPlayerUnitObject();

	// ---- Havok ----
	uintptr_t havokEntityForObject(uintptr_t obj, bool wantDynamic = true);
	struct Capsule { Vec3 a{}, b{}; float r = 0; bool ok = false; };
	// world-space capsule of an entity whose shape is (sweep-wrapped) hkCapsuleShape, at Havok's current pose
	Capsule entityCapsule(uintptr_t entity);

	struct PairInfo { uintptr_t entry = 0, agent = 0; int32_t S = 0; bool found = false; bool agentIsWelder = false; };
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

	uint32_t gameTick();
}
