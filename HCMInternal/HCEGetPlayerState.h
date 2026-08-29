#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) ONLY.
//
// The single place that knows how to reach HCE's game state. Everything else HCE-side (Freeze AI, skulls,
// teleport, launch, freecam, the 2D info overlay) resolves this cheat and asks it for an address.
//
// Why it exists at all: almost every HCE datum is reached through the GAME THREAD's thread-local storage block,
// and MultilevelPointer has no TLS specialisation (only ExeOffset / BaseOffset / ModuleOffset / Resolved).
// Rather than teach MultilevelPointer, PointerDataParser and PointerDataParserInstantiators about TLS - which
// would touch code every MCC game shares - the walk lives here once and the raw offsets come from
// InternalPointerData.xml as plain int64_t entries.
//
// EVERY method here throws HCMRuntimeException when the chain cannot be resolved. That is the NORMAL case at a
// menu, during a load, and while the player is dead - callers are expected to treat it as transient.
// ================================================================================================================
class HCEGetPlayerState : public IOptionalCheat
{
private:
	class HCEGetPlayerStateImpl;
	std::unique_ptr<HCEGetPlayerStateImpl> pimpl;

public:
	HCEGetPlayerState(GameState game, IDIContainer& dicon);
	~HCEGetPlayerState();
	std::string_view getName() override { return nameof(HCEGetPlayerState); }

	// ---- raw chain accessors. All throw HCMRuntimeException. ----

	uintptr_t getSimModuleBase();       // HaloSimulation_tag_release.dll base
	uintptr_t getGameThreadTeb();       // TEB of the thread whose win32 start address is simBase + game_thread_entrypoint
	uintptr_t getTlsBase();             // that thread's TLS block for the sim module

	uintptr_t getAiEnabledAddress();    // *(tls + 0x40). The BYTE lives AT that address (no second offset). 0 = frozen.
	uintptr_t getSkullFlagsAddress();   // *(tls + 0x60) + 0x1EBE0. 56 bits = 7 bytes, LSB first.
	// *(tls + 0xB8) + 0x9C8. Start of THREE contiguous bytes the engine's toggle writes as a unit:
	// +0x9C8 master (1 = on), +0x9C9 always 0, +0x9CA follows the master. Writing only the master leaves the
	// camera detached when freecam goes off - see the derivation in HCEFreecam::applyFreecam.
	uintptr_t getFreecamToggleAddress();
	uintptr_t getActiveCameraEntry();   // first non-null of (*(tls + 0x148) + slot*0x1AC), slot 0..3
	// *(tls + 0x4E8) + *(int32*)(cameraEntry + 0x180) * 0x410. The freecam's real pose lives here:
	// position +0x154, forward +0x17C, up +0x188 (nine floats). Re-resolve after a revert, never cache.
	uintptr_t getActiveObserver();
	// *(tls + 0xA8) + 0x05 - the engine's own cinematic-in-progress flag. Covers scripted cutscenes AND
	// prerendered video. Never throws; false when unavailable.
	bool isCinematicPlaying() noexcept;
	uintptr_t getPlayerControlEntry();  // *(tls + 0xB8) + 0x80 + 0x198*localPlayer. s_player_control.

	uint32_t  getPlayerDatum();         // *(uint32*)(*(tls + 0x30) + 0x98). low word 0xFFFF == dead.
	uintptr_t getPlayerObject();        // *( *( *(tls+0x20) + 0x50 ) + (datum & 0xFFFF)*0x18 + 0x10 )
	uintptr_t getPlayerPhysicsEntry();  // hybrid TLS + module chain, see the .cpp

	// ---- convenience reads/writes built on the above. All throw HCMRuntimeException. ----

	SimpleMath::Vector3 getPlayerPosition();   // physicsEntry + 0x1C0
	SimpleMath::Vector3 getPlayerVelocity();   // physicsEntry + 0x230

	// (x = yaw, y = pitch) in RADIANS, from s_player_control + 0x14 / +0x18 - eight contiguous bytes, the same
	// shape MCC's GetPlayerViewAngle uses. Right-handed, Z-up, +pitch = looking up, yaw measured from +X toward
	// +Y, and in the SAME world frame as the position/velocity above (the engine builds these angles with
	// atan2 over world-space position differences). yaw is normalised into [0, 2pi), pitch is bounded +-pi/2.
	SimpleMath::Vector2 getPlayerViewAngle();

	// Everything the 3D overlays need, resolving the TLS block ONCE. outCameraPosition is the RENDER camera
	// (camera entry + 0x20), so it follows freecam; outViewAngle is the player-control yaw/pitch above.
	void getCameraView(SimpleMath::Vector3& outCameraPosition, SimpleMath::Vector2& outViewAngle);
	// Both at once, walking the TLS->object->physics chain only ONCE (the info overlay wants both every refresh).
	void getPlayerPositionAndVelocity(SimpleMath::Vector3& outPosition, SimpleMath::Vector3& outVelocity);
	void setPlayerVelocity(SimpleMath::Vector3 velocity);
	void teleportPlayerTo(SimpleMath::Vector3 target); // the full 8-write sequence, see the .cpp

	// Relative variants. Each resolves the TLS->object->physics chain ONCE, which is why they live here rather
	// than being composed out of a get + a set by the caller. Both return the resulting absolute value so the
	// caller can report it.
	SimpleMath::Vector3 teleportPlayerBy(SimpleMath::Vector3 offset);   // offset added to the CURRENT position
	SimpleMath::Vector3 addPlayerVelocity(SimpleMath::Vector3 delta);   // delta ADDED to the current velocity

	// Turns a (forward, right, up) triple into a world-space vector using a yaw/pitch from getPlayerViewAngle.
	// Pure math, no game memory - static so ForceTeleport and ForceLaunch cannot drift apart.
	//
	// Safe to use for BOTH a position offset and a velocity delta: the view angle and the position/velocity
	// fields are the same right-handed Z-up world frame. That is not an assumption - the engine builds these
	// angles with atan2 over world-space position differences (the autoaim path subtracts the stored yaw from
	// atan2(dy,dx) of a target-minus-eye vector), and the position it compares against is the same object origin
	// that teleportPlayerTo writes alongside the havok translation. No 304.8 scaling and no Y negation are
	// involved anywhere on this path; that mapping exists only between the sim and the UE5 render camera.
	//
	// ignoreVerticalLook flattens the look direction onto the horizon.
	static SimpleMath::Vector3 lookRelativeOffset(SimpleMath::Vector2 viewAngle, SimpleMath::Vector3 forwardRightUp, bool ignoreVerticalLook);

	// ---- module-relative game info (no TLS). All throw HCMRuntimeException. ----

	std::string getCurrentLevelName();  // simBase + 0xCA2F00 is a NUL-terminated ASCII STRING, not an index
	// ⚠ MISNAMED: simBase + 0x9A14E0 is the current ZONE SET index, not a BSP index - it is what HaloScript's
	// current_zone_set returns. Read DIRECTLY (no deref). The name is kept because settings and hotkeys
	// serialise by name; getCurrentZoneSetName() below is the honest accessor.
	int32_t getCurrentBSP();
	// The current zone set's printable name, resolved through the scenario's 'zone sets' block (+0xD0,
	// 304-byte elements, literal char[256] at +0x04). Throws when no scenario is loaded, when the index is -1,
	// or when the block cannot be reached. Never returns an empty string.
	std::string getCurrentZoneSetName();
	// True when the switch this index names has actually COMPLETED: the index is published before the BSPs
	// finish loading, so it means "switching to", not "finished". Never throws; false when it cannot tell.
	bool isCurrentZoneSetFullyLoaded() noexcept;
	int32_t getTickCounter();           // simBase + 0x12944C8 is a POINTER to the int32

	// ---- guarded raw memory helpers, shared by every HCE cheat ----
	// Never throw; return false instead. Use these for anything in the game's address space.
	static bool tryReadRaw(uintptr_t address, void* out, size_t size);
	static bool tryWriteRaw(uintptr_t address, const void* in, size_t size);

	// Called by dependents when a write/read failed, so the next attempt re-walks the thread list instead of
	// reusing a TEB that belongs to a thread that has gone away.
	void invalidateCache();
};
