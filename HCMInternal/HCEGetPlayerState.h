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
	uintptr_t getFreecamToggleAddress();// *(tls + 0xB8) + 0x9C8. One byte, 1 = freecam on.
	uintptr_t getActiveCameraEntry();   // first non-null of (*(tls + 0x148) + slot*0x1AC), slot 0..3

	uint32_t  getPlayerDatum();         // *(uint32*)(*(tls + 0x30) + 0x98). low word 0xFFFF == dead.
	uintptr_t getPlayerObject();        // *( *( *(tls+0x20) + 0x50 ) + (datum & 0xFFFF)*0x18 + 0x10 )
	uintptr_t getPlayerPhysicsEntry();  // hybrid TLS + module chain, see the .cpp

	// ---- convenience reads/writes built on the above. All throw HCMRuntimeException. ----

	SimpleMath::Vector3 getPlayerPosition();   // physicsEntry + 0x1C0
	SimpleMath::Vector3 getPlayerVelocity();   // physicsEntry + 0x230
	// Both at once, walking the TLS->object->physics chain only ONCE (the info overlay wants both every refresh).
	void getPlayerPositionAndVelocity(SimpleMath::Vector3& outPosition, SimpleMath::Vector3& outVelocity);
	void setPlayerVelocity(SimpleMath::Vector3 velocity);
	void teleportPlayerTo(SimpleMath::Vector3 target); // the full 8-write sequence, see the .cpp

	// ---- module-relative game info (no TLS). All throw HCMRuntimeException. ----

	std::string getCurrentLevelName();  // simBase + 0xCA2F00 is a NUL-terminated ASCII STRING, not an index
	int32_t getCurrentBSP();            // simBase + 0x9A14E0 is an int32 read DIRECTLY (no deref)
	int32_t getTickCounter();           // simBase + 0x12944C8 is a POINTER to the int32

	// ---- guarded raw memory helpers, shared by every HCE cheat ----
	// Never throw; return false instead. Use these for anything in the game's address space.
	static bool tryReadRaw(uintptr_t address, void* out, size_t size);
	static bool tryWriteRaw(uintptr_t address, const void* in, size_t size);

	// Called by dependents when a write/read failed, so the next attempt re-walks the thread list instead of
	// reusing a TEB that belongs to a thread that has gone away.
	void invalidateCache();
};
