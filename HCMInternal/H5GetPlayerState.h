#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge (halo5forge.exe) ONLY.
//
// The single place that knows how to reach Halo 5's game state, in the same role HCEGetPlayerState plays for
// Halo Campaign Evolved. Everything else Halo5-side resolves this cheat and asks it for an address.
//
// Why it exists: like HCE, almost every Halo 5 datum is reached through the SIMULATION THREAD's thread-local
// storage block, and MultilevelPointer has no TLS specialisation. The walk lives here once.
//
// ⚠⚠ TWO THREAD HAZARDS, both verified live and both capable of producing plausible-but-wrong results:
//
//  1. TLS VALIDITY. On any thread that is not a simulation thread,
//     NtCurrentTeb()->ThreadLocalStoragePointer[_tls_index] yields a pointer that reads fine and means nothing.
//     Every accessor here checks the player/object globals are non-null before trusting the block.
//
//  2. THE OBJECT-WRITE GATE, [tls + 0x24] & 1. Halo 5 guards object mutation with a per-thread gate
//     (read by the engine at exe+0x14CE840; the scope helpers are exe+0x28166F0 / exe+0x297DA40).
//     TWO threads carry valid player globals and they resolve to DIFFERENT copies of the object array -
//     roughly 0x8E0000 apart, because the engine keeps several parallel object arrays for client
//     prediction/rollback. Picking "the first thread with non-null globals" can therefore hand you the
//     NON-AUTHORITATIVE copy. Prefer the gated thread. Engine object calls made from an ungated thread fault.
//
// EVERY method here throws HCMRuntimeException when the chain cannot be resolved. That is the NORMAL case at a
// menu, during a load, and while the player is dead - callers must treat it as transient.
// ================================================================================================================
class H5GetPlayerState : public IOptionalCheat
{
private:
	class H5GetPlayerStateImpl;
	std::unique_ptr<H5GetPlayerStateImpl> pimpl;

public:
	H5GetPlayerState(GameState game, IDIContainer& dicon);
	~H5GetPlayerState();
	std::string_view getName() override { return nameof(H5GetPlayerState); }

	// ---- raw chain accessors. All throw HCMRuntimeException. ----

	uintptr_t getExeBase();           // halo5forge.exe base - Halo 5 has no separate simulation module
	uintptr_t getTlsBase();           // a simulation thread's TLS block, PREFERRING the gated one
	bool      hasObjectWriteGate();   // [tls + 0x24] & 1 for the block getTlsBase() returned

	uintptr_t getPlayerGlobals();     // *(tls + 0x1560)
	uintptr_t getObjectGlobals();     // *(tls + 0x4B68)
	uintptr_t getPlayerArray();       // *(playerGlobals + 0x58)

	uint32_t  getPlayerDatum();       // *(uint32*)(playerArray + 0x24). 0xFFFFFFFF == no player.
	// *( *(objectGlobals + 0x58) + (datum & 0xFFFF) * *(objectGlobals + 0x20) + 0x10 )
	// ⚠ the element STRIDE is a runtime value at objectGlobals+0x20, not a constant.
	uintptr_t getPlayerObject();

	// *(tls + 0x15A8). The save-request block; +0x00 is the mode the engine consumes and clears.
	uintptr_t getSaveRequestAddress();

	// ---- convenience reads. All throw HCMRuntimeException. ----

	// ⚠ object + 0x224 is the PUBLISHED position, not the authority. It is written by exactly one
	// instruction in the whole engine (exe+0x281E909, inside object_set_position at exe+0x281E8C0) and
	// only while the player is MOVING - a stationary player's position is never rewritten. Reading it is
	// correct; WRITING it is not, and produces a one-frame flash followed by a snap back.
	SimpleMath::Vector3 getPlayerPosition();

	// The CHARACTER CONTROLLER's position - the authority Force Teleport writes. Prefer this over
	// getPlayerPosition() for any read-then-write: the object's published position only refreshes while
	// the player is MOVING, so it is stale right after a teleport and frozen entirely while paused.
	SimpleMath::Vector3 getProxyPosition();

	// The camera the game is ACTUALLY RENDERING FROM, and its forward vector.
	// ⚠ NOT getCameraPosition() - that reads the player's EYE and therefore tracks the player and only the
	// player. The observer is the published render camera, so it is correct during cinematics, death cams,
	// scripted fly-throughs and Forge, and it resolves even at a menu with no player spawned.
	SimpleMath::Vector3 getObserverPosition();
	SimpleMath::Vector3 getObserverForward();
	SimpleMath::Vector3 getPlayerAim();        // playerArray + 0x44, unit vector

	// playerArray + 0x38 - the EYE position, and the camera the 3D overlays render from. Verified live:
	// equal to the object position in X and Y to the last decimal, +0.60..0.66 in Z varying with stance.
	// ⚠ It follows the PLAYER, so it does not track a cutscene or vehicle camera. No render-camera POV has
	// been found for Halo 5; this is the best available source and is correct for ordinary play.
	SimpleMath::Vector3 getCameraPosition();

	// The player's live FOV setting in DEGREES, read from the engine rather than configured in HCM.
	// Found by differential scan across an in-game slider change 60 -> 120; see the .cpp.
	// ⚠ Treated as HORIZONTAL. The slider's 60..120 range is the usual horizontal convention (a 60 degree
	// VERTICAL minimum would be ~90 horizontal, which no game offers as its narrowest setting).
	float getCameraFovDegrees();

	// The game's own simulation kind, read from a static string at exe+0x5A23920: "local" for campaign,
	// otherwise a distributed-client session. Checkpoint/revert are only meaningful when local.
	std::string getSimulationKind();
	bool isLocalSimulation();

	// ---- zone sets ---------------------------------------------------------------------------------------
	// The scenario globals are [exe + 0x05A62538]; the zone set table hangs off them:
	//     array = [globals + 0x264],  stride 0x218,  count = [globals + 0x274]
	//     entry + 0x00  u32    name hash
	//     entry + 0x04  char[] INLINE null-terminated ASCII name  (no string table lookup needed)
	int32_t     getZoneSetCount();
	std::string getZoneSetName(int32_t index);   // throws if out of range

	// Switch state, decoded from switch_zone_set (exe + 0x005E8770):
	//     exe + 0x050B0255   byte   1 while a switch is in flight, 0 otherwise
	//     exe + 0x050B03B4   int32  the zone set index that switch is heading to
	// ⚠ The pending index is NOT cleared when a switch completes - only the flag is. So once the flag
	// returns to 0 the index still names the zone set that was switched TO, which is what makes it usable
	// as the committed one. Do not treat a stale-looking index as "nothing pending"; gate on the FLAG.
	bool        isPreparingZoneSet() noexcept;
	std::string getPreparedZoneSetName() noexcept;   // EMPTY unless preparing
	// The committed (currently active) zone set, read from the engine's own global at exe+0x04757CB0.
	// ⚠ Do NOT try to derive this from the pending index: every path that clears the preparing flag also
	// stores -1 over that index, so once a switch finishes it holds nothing. See the .cpp for the four
	// store sites and for how the committed global was identified.
	std::string getCommittedZoneSetName();

	// The map's internal name, e.g. "w1_unconfirmed_reports" - the scenario's own name, not a display title.
	//
	// ⚠ RETURNS "" UNTIL IT IS KNOWN, AND THAT IS NORMAL. There is no pointer chain to this string (zone
	// set names do not carry the level prefix, nothing in the scenario globals points at it, and no
	// ASLR-stable global holds its address), so it is recovered by one memory scan per level, run on a
	// worker thread. Callers should show a placeholder while it is empty rather than treat it as an error.
	// Never blocks; safe to call every frame.
	std::string getMapName() noexcept;
	int32_t     getCommittedZoneSetIndex();   // -1 when no zone set is active

	// Request a switch to the given zone set. A PURE DATA WRITE - it sets the same three globals the
	// engine's own switch_zone_set sets and lets the main loop pick the request up on its next tick.
	// No engine call and no thread affinity requirement; see the .cpp for the store sites and write order.
	void requestZoneSetSwitch(int32_t index);

	// ---- writes. All throw HCMRuntimeException. ----

	// FORCE TELEPORT. Calls the engine's own teleport worker (exe+0x0088FC10) rather than writing any
	// position field, because every position field downstream of the mover is a published copy and writing
	// one desyncs the character controller from the renderer.
	//
	// ⚠⚠ THE POSITION POINTER MUST BE PASSED IN **BOTH** rdx AND r8. exe+0x0088FC59 is `mov r14, r8`, and
	// at exe+0x0088FD14 the worker falls back to reading the position from [r14] when [rbx+0x30] == -1,
	// which is the state our player object is in. Passing it only in rdx null-faults at exe+0x0088FD43;
	// passing it only in r8 null-faults deeper, at exe+0x006798AD.
	//
	// ⚠ MUST run on the simulation thread WITH the object-write gate - see the class comment. Queue it
	// through H5GameThreadPump rather than calling directly.
	void teleportPlayerTo(SimpleMath::Vector3 target);
	SimpleMath::Vector3 teleportPlayerBy(SimpleMath::Vector3 offset);

	// VELOCITY, for Force Launch and Acrophobia. This is the character controller's own velocity at
	// proxy+0x40 - the field the engine actually integrates.
	//
	// ⚠ NOT obj+0x248. That one is a published mirror: a write to it persists untouched and the player
	// never moves, because nothing reads it back. Confirmed live - it held (0,0,8) for three seconds with
	// the player standing still. proxy+0x40 was identified by correlating every triple in the element
	// against measured d(position)/dt (0.150 vs 1.000 for everything else) and writing (0,0,12) to it
	// produced a clean 12.7 wu ballistic arc.
	SimpleMath::Vector3 getPlayerVelocity();

	// ---- READOUT VARIANTS. nullopt instead of throwing. ----
	// ⚠ USE THESE FROM ANYTHING THAT POLLS EVERY FRAME (the 2D info overlay). Being at a menu, dead or
	// mid-load is an ordinary state, not an error, and reporting it via HCMRuntimeException costs a full
	// stack walk (HCMExceptionBase's ctor calls std::stacktrace::current(), which takes dbghelp's global
	// lock) plus two synchronous PLOG_ERROR writes - per frame. One day of logs contained 15,700 of them.
	//
	// ⚠ THEY SELF-HEAL AND NEVER LATCH OFF. Internally a failed resolve only delays the next ATTEMPT, and
	// any success clears that instantly, so a caller that keeps asking every frame starts getting values
	// again on its own as soon as the player is controllable. Do not add a "disabled" flag on top of this;
	// the readout is expected to come back without the user touching anything.
	std::optional<SimpleMath::Vector3> tryGetPlayerVelocity() noexcept;
	std::optional<SimpleMath::Vector3> tryGetProxyPosition() noexcept;
	void setPlayerVelocity(SimpleMath::Vector3 velocity);
	// Read-modify-write with a SINGLE proxy resolution - use this for anything running per frame.
	void modifyPlayerVelocity(const std::function<SimpleMath::Vector3(SimpleMath::Vector3)>& fn);
	// Drop the cached character-controller address, forcing the next access to re-resolve from scratch.
	// Per-frame features should call this after ANY failure: death, a BSP or zone set switch and a level
	// load all rebuild the physics world, and the cached address does not survive that.
	void invalidateProxyCache() noexcept;

	// FORCE CHECKPOINT. Pure data write - no engine call, nothing to deadlock: *(uint32*)saveRequest = mode.
	// mode 1 = game_save (with timeout), 2 = no_timeout, 3 = immediate, 4 = cinematic_skip.
	// The engine consumes the request and zeroes it, which is how we confirm it landed.
	void requestCheckpoint(uint32_t mode);
};
