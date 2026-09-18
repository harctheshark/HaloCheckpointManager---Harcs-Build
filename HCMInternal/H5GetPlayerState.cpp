#include "pch.h"
#include "H5GetPlayerState.h"
#include "RuntimeExceptionHandler.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include <TlHelp32.h>

// ================================================================================================================
// Halo 5: Forge state resolution. Every offset here was derived live against halo5forge.exe and is an RVA into
// the exe itself - Halo 5 has no separate simulation module.
//
// WHICH THREAD. Like HaloCER, the state hangs off a simulation thread's TLS block, and HCM runs on the D3D12
// present thread and its own hotkey threads. Unlike HaloCER we cannot identify the thread by its win32 start
// address, so we enumerate threads and score their TLS blocks:
//     playerGlobals (tls+0x1560) and objectGlobals (tls+0x4B68) both non-null  -> it is a sim thread
//     [tls+0x24] & 1                                                          -> it also holds the write gate
// and PREFER a gated one.
//
// ⚠⚠ WHY THE GATE DECIDES CORRECTNESS, NOT JUST PERMISSION. Measured live: TWO threads carry valid player
// globals, and they resolve to DIFFERENT player objects about 0x8E0000 apart - the engine keeps several
// parallel object arrays for client prediction/rollback. So "the first thread with non-null globals" is not
// merely a permissions gamble, it can be the wrong COPY of the world. The gated thread is the authoritative one.
//
// Caching: nothing is cached. TEB.ThreadLocalStoragePointer is reallocated by ntdll's LdrpHandleTlsData whenever
// a module with TLS loads, and the gate moves between threads across a level load. Two loads is free next to
// what callers then do.
// ================================================================================================================

namespace
{
	typedef LONG(NTAPI* H5NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	constexpr ULONG kThreadBasicInformation = 0;

	struct H5ThreadBasicInformation
	{
		LONG      ExitStatus;
		PVOID     TebBaseAddress;
		ULONG_PTR UniqueProcessId;
		ULONG_PTR UniqueThreadId;
		ULONG_PTR AffinityMask;
		LONG      Priority;
		LONG      BasePriority;
	};

	// SEH only. No C++ objects with destructors may live in here - MSVC rejects __try in a function that
	// requires object unwinding (C2712).
	bool sehCopy(void* dest, const void* src, size_t size)
	{
		__try
		{
			memcpy(dest, src, size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool sehWrite(void* dest, const void* src, size_t size)
	{
		__try
		{
			memcpy(dest, src, size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	template<typename T>
	bool readAt(uintptr_t addr, T& out)
	{
		if (!addr) return false;
		return sehCopy(&out, (const void*)addr, sizeof(T));
	}

	// ---- Halo 5 RVAs, all verified live against halo5forge.exe ----------------------------------------------
	constexpr uintptr_t kRvaTlsIndex      = 0x05F1D56C;  // uint32 _tls_index
	constexpr uintptr_t kRvaSimKindString = 0x05A23920;  // "local" / distributed-client kind, a C string
	// The worker behind object_teleport / FT_SetObjectPosition. See the contract in the header - the position
	// pointer goes in BOTH rdx and r8.
	constexpr uintptr_t kRvaTeleportWorker = 0x0088FC10;

	constexpr uintptr_t kTlsObjectWriteGate = 0x0024;
	constexpr uintptr_t kTlsPlayerGlobals   = 0x1560;
	constexpr uintptr_t kTlsObjectGlobals   = 0x4B68;
	constexpr uintptr_t kTlsSaveRequest     = 0x15A8;

	constexpr uintptr_t kPlayerArrayFromGlobals = 0x58;
	constexpr uintptr_t kPlayerDatumInArray     = 0x24;
	constexpr uintptr_t kPlayerAimInArray       = 0x44;
	// The EYE position, verified live: identical to the object position in X and Y to the last decimal, and
	// +0.60..0.66 in Z, drifting with stance. That is a camera, not a copy of the object origin.
	constexpr uintptr_t kPlayerCameraInArray    = 0x38;

	constexpr uintptr_t kObjectTableFromGlobals  = 0x58;
	constexpr uintptr_t kObjectStrideFromGlobals = 0x20;   // ⚠ runtime value, NOT a constant
	constexpr uintptr_t kObjectEntryToObject     = 0x10;

	constexpr uintptr_t kObjectPublishedPosition = 0x224;

	// ---- the OBSERVER, i.e. the camera actually being rendered from ------------------------------------
	// ⚠⚠ THIS IS NOT getCameraPosition(). That one reads playerArray+0x38, which is the player's EYE - it
	// matches the player object in X and Y to the last decimal and sits +0.60 WU above it in Z, drifting
	// with stance. It therefore tracks the player and ONLY the player, which is useless the moment the
	// camera is not on the player: cinematics, death cams, a scripted fly-through, Forge.
	//
	// The observer is the engine's published render camera. Measured live while sitting at a menu with no
	// player spawned: observer (-2382.632, 1174.216, -794.190) while the player eye read (0,0,0) - 2772 WU
	// apart, and the observer still valid when the player was not. That is the separation this exists for.
	//
	//     observers = [tls + 0x1B0]                 the observer array
	//     director  = [tls + 0x198]                 the camera director
	//     watched   = int32 [director + 0x1AC]      which observer is being rendered (0..3)
	//     observer  = observers + watched * 0x4A0
	//     observer + 0x15C   3f   position
	//     observer + 0x184   3f   forward (unit)
	constexpr uintptr_t kTlsDirectors = 0x198;
	constexpr uintptr_t kTlsObservers = 0x1B0;
	constexpr uintptr_t kDirectorWatched = 0x1AC;
	constexpr uintptr_t kObserverStride = 0x4A0;
	constexpr uintptr_t kObserverPosition = 0x15C;
	constexpr uintptr_t kObserverForward = 0x184;

	// ---- zone sets. All RVAs into halo5forge.exe, derived live. -----------------------------------------
	constexpr uintptr_t kRvaScenarioGlobals = 0x05A62538;   // pointer to the scenario globals singleton
	constexpr uintptr_t kGlobalsZoneArray   = 0x264;        // -> zone set array
	constexpr uintptr_t kGlobalsZoneCount   = 0x274;        // int32 count
	constexpr uintptr_t kZoneSetStride      = 0x218;        // 536 bytes per zone set
	constexpr uintptr_t kZoneSetNameOffset  = 0x04;         // INLINE ascii, not a string id
	constexpr uintptr_t kRvaZonePreparing   = 0x050B0255;   // byte: 1 while a switch is in flight
	constexpr uintptr_t kRvaZonePending     = 0x050B03B4;   // int32: the index that switch targets
	// ⚠ THE COMMITTED ZONE SET. Found by static analysis, not guesswork - see getCommittedZoneSetName.
	// exe+0x00A7A810 is a one-instruction getter that returns exactly this, and the ONLY instruction in
	// the whole image that stores a real value into it is exe+0x00A7932F, immediately after the engine
	// logs "switching to". -1 means no zone set is active.
	constexpr uintptr_t kRvaZoneCommitted   = 0x04757CB0;   // int32: the zone set currently active
	constexpr uintptr_t kRvaZoneRequestBits = 0x050B0254;   // byte: bit 2 = "a zone set switch is requested"

	// ★ THE LIVE FIELD OF VIEW, in DEGREES. Found by differential scan: with the in-game slider at 60 every
	// float in the process matching any encoding of 60 was recorded, then re-read after moving the slider to
	// 120 and kept only if it moved to the CORRESPONDING encoding of 120. Nineteen addresses survived; two
	// were static in the module - this one in degrees, and 0x06148FBC in radians. The rest were per-view
	// heap copies at unstable addresses.
	// ⚠ No projection SCALE (tan or 1/tan of the half angle) tracked the change anywhere in 6GB, which says
	// the projection matrix is built per frame straight into a GPU constant buffer and never kept somewhere
	// readable. So this scalar is the only camera parameter available - there is no matrix to borrow.
	constexpr uintptr_t kRvaFieldOfViewDegrees = 0x0590E210;

	// worker(rcx = datum, rdx = &position, r8 = &forward, r9 = 0, arg5 = 1, arg6 = setFacing)
	//
	// ⚠⚠ R8 IS THE FORWARD DIRECTION, NOT A SECOND POSITION. An earlier reading of this function had it as
	// "the position must be passed in both rdx and r8", and a live test of that form did teleport cleanly.
	// A later adversarial decode refuted the interpretation: the parented branch transforms r8 with
	// 0x00677A20, a pure 3x3 rotate with NO translation term, whereas the engine's real point transform
	// (0x00677B80) adds [rcx+0x28..0x30]. So r8 is a direction.
	//
	// r8 must still be READABLE - 0x0088FD43 (`movsd xmm0,[r14]`) is unguarded for units, which is why
	// passing NULL there null-faults, and why FT_SetObjectPosition (which zeroes r8) cannot be used.
	//
	// ARG6 IS THE ONE THAT MATTERS. It gates the facing update (0x0133A970). With arg6 = 1 the vector in r8
	// is applied as the unit's facing - so passing the POSITION there writes raw world coordinates into
	// unit+0x384/+0x38C/+0x3A8/+0x3B0/+0x3D8. The engine's own FT_SetObjectPosition passes arg6 = 0.
	// We therefore pass a genuine forward vector AND arg6 = 0: the read is satisfied, nothing is applied.
	typedef void(*fnTeleportWorker)(uint32_t datum, const float* position, const float* forward, void* unused,
		char alwaysOne, char setFacing);

	constexpr uintptr_t kObjectForwardVector = 0x230;   // unit forward, verified orthonormal live

	// ============================================================================================
	// ★★★ FORCE TELEPORT: WRITE THE CHARACTER-CONTROLLER PROXY. NOTHING ELSE.
	//
	// Confirmed working in game: "it did work, it stored it until i moved. just like the MCC games do,
	// no crash, no desync, its perfect."
	//
	// This is the same shape MCC uses (ForceTeleport.cpp -> getObjectPositionMutableAndVisual): resolve the
	// PHYSICS AUTHORITY by pointer chain and store 12 bytes into it. No engine call, no pump, no code patch.
	//
	// ⚠⚠⚠ EVERY OTHER TARGET IN THIS ENGINE IS A PUBLISHED MIRROR, AND ALL OF THEM HAVE BEEN TRIED:
	//   obj+0x224        one-frame flash then snap back  ("flashing where i should be but its overwritten")
	//   obj+0x248 (vel)  write persists untouched, player never moves - nothing consumes it
	//   motion state     desynced the player and then KILLED him ("the game straight up killed me")
	//   engine calls     two separate attempts faulted inside the engine and took the process down
	// Do not revisit any of them. The proxy is upstream of all four - write it and they all follow.
	//
	// HOW IT WAS FOUND (so this is reproducible rather than magic): a hardware data breakpoint on the motion
	// state's translation caught two writers; the same heap pointer appeared in both (rsi in one, rdi in the
	// other), and it held the player's position with a PERFECTLY CONSTANT offset while walking - 35 samples,
	// spread 0.0000. That constant is the centre-of-mass delta, the same quantity HaloCER compensates for.
	//
	// THE CHAIN (verified 6/6 against the watchpoint's answer, and it needs no scan of the address space):
	//     world      = [exe + 0x5FB1CD8]
	//     msArray    = [world + 0x20]        element stride 0x90, translation at +0x30
	//     centreOff  = (ms+0x0C, ms+0x1C, ms+0x2C)     the transform's translation COLUMN
	//     proxyArray = [world + 0x118]       element stride 0x80, position at +0x00
	//     proxy      = the element whose position == objectPosition + centreOff
	//
	// ⚠ The two lookups are bounded reads of ONE array each with an exact key, and both FAIL CLOSED unless
	// exactly one element matches. That is deliberate: another body within the tolerance must abort the
	// teleport rather than move somebody else.
	constexpr uintptr_t kRvaPhysicsWorld  = 0x5FB1CD8;   // -> the physics world singleton
	constexpr uintptr_t kWorldMsArray     = 0x20;        // -> motion state array
	constexpr uintptr_t kWorldProxyArray  = 0x118;       // -> character controller proxy array
	constexpr uintptr_t kMsStride         = 0x90;
	constexpr uintptr_t kMsTranslation    = 0x30;
	// ⚠⚠ kMsCentreX/Y/Z (body +0x0C/+0x1C/+0x2C) WERE HERE AND ARE DELIBERATELY GONE. That triple is the
	// translation column of the body's 3x4 transform - a BODY-LOCAL vector. It happens to equal the
	// world-space centre delta while the basis is near identity, which is why it looked right on an upright
	// player, but on a body whose basis is flipped it comes back with an inverted axis: measured
	// (0.0015, -0.0015, +0.2750) where the true delta was (-0.0022, 0.0000, -0.2750). Fed to the old
	// distance matcher that produced a 0.55 residual against the CORRECT element - a refusal with nothing
	// else within a kilometre. The centre offset is now measured as motionCentreOfMass - bodyTranslation,
	// which is exact in every orientation. Do not put these back.
	constexpr uintptr_t kProxyStride      = 0x80;
	constexpr uintptr_t kProxyPosition    = 0x00;
	// ⚠ THE VELOCITY THE CONTROLLER ACTUALLY INTEGRATES. Identified the same way obj+0x248 was - scoring
	// every float triple in the element against measured d(position)/dt over 52.68 wu of walking:
	// +0x40 scored 0.150, +0x60 scored 0.151 (it mirrors +0x40), everything else scored 1.000 (no
	// correlation at all). Writing (0,0,12) here produced a clean ballistic arc - 12.7 wu of rise with
	// gravity decaying the velocity 9.75 -> 0 -> negative - so this field is an INPUT, unlike obj+0x248
	// which accepts writes and is never read by anything.
	constexpr uintptr_t kProxyVelocity    = 0x40;
	// ⚠ kMatchAccept / kMatchMinGap are GONE. They were the distance-scan tolerances: accept the
	// nearest body within 0.25 wu, and refuse if the runner-up was within 0.10 wu of it. The resolve is
	// an exact identity match now, so there is no tolerance to tune - and tuning them was never the fix.

	constexpr uint32_t  kMaxArrayElements = 4096;

	// ⚠⚠ MATCH THE NEAREST WITH A CLEAR MARGIN - DO NOT MATCH "EXACTLY ONE WITHIN A WINDOW".
	// Measured live: the player's own element matches EXACTLY (residual 0.000000), while the next nearest
	// body sits about 1.36 wu away. But that gap is situational - Blue Team AI walk right up to you, and a
	// 0.5 wu window caught FOUR candidates in one sample, which made the teleport refuse at random.
	// So: take the closest, require it to be essentially exact, and require the runner-up to be clearly
	// farther. That is robust to a teammate standing next to you AND to the few milliseconds of movement
	// between reading the player position and scanning the array.
	// ---- DETERMINISTIC PROXY RESOLUTION ---------------------------------------------------------------
	// ⚠⚠ THESE REPLACED A DISTANCE SCAN. resolveCharacterProxy used to find the player's body by scanning
	// both arrays for the element NEAREST to (objectPosition + centreOffset) and refusing when the runner-up
	// was within 0.10 wu - which is why Force Teleport / Force Launch failed whenever anything was standing
	// close to you, and why one day of logs held ~15,700 throws out of this function.
	//
	// The engine has an exact answer. hknp keeps a per-body property map on the world; key 0x2009 is a
	// straight bodyId -> OWNING OBJECT DATUM array. So "which body is mine" becomes an integer equality test
	// instead of a distance comparison, and a teammate, a dropped weapon or a vehicle can never be a
	// candidate no matter how close they stand - they carry a different datum.
	//
	// Verified live, in a level, on a real player: exactly ONE body carried the player's datum (body 45,
	// motion 27), the component back-reference matched, and motion+0x28 round-tripped to the body id.
	constexpr uintptr_t kRvaHavokComponents = 0x05FB1E08; // -> blam data array of havok components
	constexpr uintptr_t kHcElementStride    = 0x20;       // u32 element stride, in the array HEADER
	constexpr uintptr_t kHcBound            = 0x4C;       // ⚠ i32 high-water bound. NOT +0x30 (max count) -
	                                                      //   the engine's own data_try_get checks +0x4C.
	constexpr uintptr_t kHcData             = 0x58;       // -> element storage
	constexpr uintptr_t kObjectHavokComponent = 0x0330;   // u32 component datum on the object (~0 = none)
	constexpr uintptr_t kComponentOwnerObject = 0x0030;   // u32 object datum on the component (back-ref)

	constexpr uintptr_t kWorldPropTable  = 0x98;   // void** open-addressed table of per-body properties
	constexpr uintptr_t kWorldPropMask   = 0xA4;   // i32 mask (table size - 1)
	constexpr uint32_t  kPropBodyObject   = 0x2009; // bodyId -> object datum
	constexpr uint32_t  kPropBodyComponent = 0x2001; // bodyId -> component datum (fallback)
	constexpr uintptr_t kPropBitmap = 0x08, kPropSize = 0x18, kPropValues = 0x20;
	constexpr uintptr_t kWorldBodyCount   = 0x2C;  // u32, masked with 0x3FFFFFFF
	constexpr uintptr_t kBodyMotionId     = 0x68;  // u32; 0 means the body has no motion (static/keyframed)
	constexpr uintptr_t kMotionAttachedBody = 0x28; // u32 back-reference to a body id

	// ⚠⚠ THIS IS A **RELATIVE** GAP, NOT AN ABSOLUTE SEPARATION. The first version demanded the runner-up be
	// at least 0.75 wu away, and in real play that refused constantly - the user: "getting a lot of errors
	// when TP/launching that something is next to me, thats quite common". Of course it is: Blue Team follow
	// you around and stand inside a metre of you for most of the campaign.
	//
	// Absolute separation was the wrong test. The player's own element matches EXACTLY (residual 0.000000,
	// because obj+0x224 is DERIVED from the proxy), while another body's residual is simply how far away it
	// is. So candidates of (0.000, 0.400) are not ambiguous at all - 0.000 is obviously ours - yet the old
	// rule threw them out. What actually makes a match ambiguous is two candidates being near-exact
	// TOGETHER, so compare them to EACH OTHER: take the nearest, require it to be essentially exact, and
	// require a clear gap to the runner-up.

	// ⚠ IN-PROCESS, A READ PAST THE END OF THE ARRAY IS AN ACCESS VIOLATION, not a short read. The tools
	// that found all this ran externally, where an over-long ReadProcessMemory simply fails. Bound every
	// array read by the actual committed region before touching it.
	size_t readableBytesFrom(uintptr_t addr, size_t want) noexcept
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return 0;
		if (mbi.State != MEM_COMMIT) return 0;
		const bool readable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
			| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		if (!readable || (mbi.Protect & PAGE_GUARD)) return 0;
		const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		const size_t avail = (regionEnd > addr) ? (size_t)(regionEnd - addr) : 0;
		return (avail < want) ? avail : want;
	}

	bool sehWriteVec3(uintptr_t dest, float x, float y, float z) noexcept
	{
		__try
		{
			float* p = (float*)dest;
			p[0] = x; p[1] = y; p[2] = z;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline float dist3(const float* a, const float* b) noexcept
	{
		const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}
}


class H5GetPlayerState::H5GetPlayerStateImpl
{
private:
	GameState mGame;

public:
	// ⚠ NO HOOK, NO PUMP, NO CODE PATCH. The teleport is a pointer chain and a 12-byte store, so nothing
	// here installs anything into the game. The previous version patched exe+0x0282F16C to run an engine
	// call on the simulation thread; that whole apparatus is gone along with the crashes it caused.
	H5GetPlayerStateImpl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5GetPlayerState only supports Halo 5: Forge");
	}

	uintptr_t getExeBase()
	{
		static uintptr_t cached = 0;
		if (cached) return cached;
		cached = (uintptr_t)GetModuleHandleW(nullptr);
		if (!cached) throw HCMRuntimeException("Could not resolve the halo5forge.exe base");
		return cached;
	}

	// Returns the TLS block of a simulation thread, preferring one that holds the object-write gate.
	uintptr_t getTlsBase()
	{
		const uintptr_t exeBase = getExeBase();
		uint32_t tlsIndex = 0;
		if (!readAt(exeBase + kRvaTlsIndex, tlsIndex))
			throw HCMRuntimeException("Could not read Halo 5's _tls_index");

		static H5NtQueryInformationThread_t ntQuery = nullptr;
		if (!ntQuery)
		{
			HMODULE nt = GetModuleHandleW(L"ntdll.dll");
			if (nt) ntQuery = (H5NtQueryInformationThread_t)GetProcAddress(nt, "NtQueryInformationThread");
			if (!ntQuery) throw HCMRuntimeException("Could not resolve NtQueryInformationThread");
		}

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE)
			throw HCMRuntimeException("Could not snapshot Halo 5's threads");

		const DWORD pid = GetCurrentProcessId();
		uintptr_t best = 0;        // a sim thread without the gate
		uintptr_t bestGated = 0;   // a sim thread WITH the gate - always preferred

		THREADENTRY32 te{}; te.dwSize = sizeof(te);
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!th) continue;

				H5ThreadBasicInformation tbi{};
				const LONG st = ntQuery(th, kThreadBasicInformation, &tbi, sizeof(tbi), nullptr);
				CloseHandle(th);
				if (st != 0 || !tbi.TebBaseAddress) continue;

				// TEB + 0x58 = ThreadLocalStoragePointer
				uintptr_t tlsArray = 0;
				if (!readAt((uintptr_t)tbi.TebBaseAddress + 0x58, tlsArray) || !tlsArray) continue;

				uintptr_t tlsBase = 0;
				if (!readAt(tlsArray + 8ull * tlsIndex, tlsBase) || !tlsBase) continue;

				uintptr_t playerGlobals = 0, objectGlobals = 0;
				if (!readAt(tlsBase + kTlsPlayerGlobals, playerGlobals) || !playerGlobals) continue;
				if (!readAt(tlsBase + kTlsObjectGlobals, objectGlobals) || !objectGlobals) continue;

				uint32_t gate = 0;
				readAt(tlsBase + kTlsObjectWriteGate, gate);
				if (gate & 1u) { bestGated = tlsBase; break; }
				if (!best) best = tlsBase;
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);

		const uintptr_t chosen = bestGated ? bestGated : best;
		if (!chosen)
			throw HCMRuntimeException("No Halo 5 simulation thread found (at a menu, or still loading?)");
		return chosen;
	}

	bool hasObjectWriteGate()
	{
		uint32_t gate = 0;
		if (!readAt(getTlsBase() + kTlsObjectWriteGate, gate)) return false;
		return (gate & 1u) != 0;
	}

	uintptr_t getPlayerGlobals()
	{
		uintptr_t v = 0;
		if (!readAt(getTlsBase() + kTlsPlayerGlobals, v) || !v)
			throw HCMRuntimeException("Halo 5 player globals were null");
		return v;
	}

	uintptr_t getObjectGlobals()
	{
		uintptr_t v = 0;
		if (!readAt(getTlsBase() + kTlsObjectGlobals, v) || !v)
			throw HCMRuntimeException("Halo 5 object globals were null");
		return v;
	}

	uintptr_t getPlayerArray()
	{
		uintptr_t v = 0;
		if (!readAt(getPlayerGlobals() + kPlayerArrayFromGlobals, v) || !v)
			throw HCMRuntimeException("Halo 5 player array was null");
		return v;
	}

	uint32_t getPlayerDatum()
	{
		uint32_t d = 0;
		if (!readAt(getPlayerArray() + kPlayerDatumInArray, d))
			throw HCMRuntimeException("Could not read the Halo 5 player datum");
		if (d == 0xFFFFFFFFu)
			throw HCMRuntimeException("No Halo 5 player (dead, at a menu, or loading)");
		return d;
	}

	uintptr_t getPlayerObject()
	{
		const uintptr_t objectGlobals = getObjectGlobals();
		const uint32_t datum = getPlayerDatum();

		uintptr_t table = 0, stride = 0;
		if (!readAt(objectGlobals + kObjectTableFromGlobals, table) || !table)
			throw HCMRuntimeException("Halo 5 object table was null");
		// ⚠ the stride is a runtime value; do not hard-code it.
		if (!readAt(objectGlobals + kObjectStrideFromGlobals, stride) || !stride)
			throw HCMRuntimeException("Halo 5 object table stride was zero");

		const uintptr_t entry = table + (uintptr_t)(datum & 0xFFFFu) * stride;
		uintptr_t object = 0;
		if (!readAt(entry + kObjectEntryToObject, object) || !object)
			throw HCMRuntimeException("Halo 5 player object was null");
		return object;
	}

	uintptr_t getSaveRequestAddress()
	{
		uintptr_t v = 0;
		if (!readAt(getTlsBase() + kTlsSaveRequest, v) || !v)
			throw HCMRuntimeException("Halo 5 save-request block was null");
		return v;
	}

	SimpleMath::Vector3 getPlayerPosition()
	{
		float p[3]{};
		if (!sehCopy(p, (const void*)(getPlayerObject() + kObjectPublishedPosition), sizeof(p)))
			throw HCMRuntimeException("Could not read the Halo 5 player position");
		return SimpleMath::Vector3(p[0], p[1], p[2]);
	}

	SimpleMath::Vector3 getPlayerAim()
	{
		float a[3]{};
		if (!sehCopy(a, (const void*)(getPlayerArray() + kPlayerAimInArray), sizeof(a)))
			throw HCMRuntimeException("Could not read the Halo 5 player aim");
		return SimpleMath::Vector3(a[0], a[1], a[2]);
	}

	// The player's eye/camera position. Tracks the PLAYER, so like the HaloCER fallback camera it will not
	// follow a cutscene or a vehicle camera - but Halo 5 exposes no render-camera POV we have found, and for
	// an overlay drawn while you walk around this is correct.
	SimpleMath::Vector3 getCameraPosition()
	{
		float c[3]{};
		if (!sehCopy(c, (const void*)(getPlayerArray() + kPlayerCameraInArray), sizeof(c)))
			throw HCMRuntimeException("Could not read the Halo 5 camera position");
		return SimpleMath::Vector3(c[0], c[1], c[2]);
	}

	// The player's live FOV setting, in degrees. See kRvaFieldOfViewDegrees.
	float getCameraFovDegrees()
	{
		float f = 0.f;
		if (!readAt(getExeBase() + kRvaFieldOfViewDegrees, f))
			throw HCMRuntimeException("Could not read the Halo 5 field of view");
		if (!std::isfinite(f) || f < 10.f || f > 170.f)
			throw HCMRuntimeException(std::format("The Halo 5 field of view looks wrong ({:.2f})", f));
		return f;
	}

	std::string getSimulationKind()
	{
		char buf[64]{};
		if (!sehCopy(buf, (const void*)(getExeBase() + kRvaSimKindString), sizeof(buf) - 1))
			throw HCMRuntimeException("Could not read the Halo 5 simulation kind");
		buf[sizeof(buf) - 1] = '\0';
		return std::string(buf);
	}

	// ---- zone sets -----------------------------------------------------------------------------------
	uintptr_t getScenarioGlobals() // throws
	{
		uintptr_t g = 0;
		if (!readAt(getExeBase() + kRvaScenarioGlobals, g) || !g)
			throw HCMRuntimeException("No Halo 5 scenario is loaded");
		return g;
	}

	int32_t getZoneSetCount() // throws
	{
		int32_t n = 0;
		if (!readAt(getScenarioGlobals() + kGlobalsZoneCount, n) || n <= 0)
			throw HCMRuntimeException("Halo 5 reports no zone sets");
		return n;
	}

	std::string getZoneSetName(int32_t index) // throws
	{
		const int32_t count = getZoneSetCount();
		if (index < 0 || index >= count)
			throw HCMRuntimeException(std::format("Zone set index {} is out of range (0..{})", index, count - 1));

		uintptr_t arr = 0;
		if (!readAt(getScenarioGlobals() + kGlobalsZoneArray, arr) || !arr)
			throw HCMRuntimeException("The Halo 5 zone set array is null");

		// The name is stored INLINE as plain ascii - no string table to resolve.
		char buf[65]{};
		if (!sehCopy(buf, (const void*)(arr + (uintptr_t)index * kZoneSetStride + kZoneSetNameOffset), 64))
			throw HCMRuntimeException("Could not read the Halo 5 zone set name");
		buf[64] = '\0';
		std::string out;
		for (int i = 0; i < 64; ++i)
		{
			const char c = buf[i];
			if (c == '\0') break;
			if (c < 0x20 || c > 0x7E) break;
			out.push_back(c);
		}
		if (out.empty()) out = std::format("zone set {}", index);
		return out;
	}

	bool isPreparingZoneSet() noexcept
	{
		try
		{
			uint8_t flag = 0;
			if (!readAt(getExeBase() + kRvaZonePreparing, flag)) return false;
			return flag != 0;
		}
		catch (...) { return false; }
	}

	int32_t getPendingZoneSetIndex() // throws
	{
		int32_t i = 0;
		if (!readAt(getExeBase() + kRvaZonePending, i))
			throw HCMRuntimeException("Could not read the Halo 5 pending zone set index");
		return i;
	}

	std::string getPreparedZoneSetName() noexcept
	{
		try
		{
			if (!isPreparingZoneSet()) return {};
			return getZoneSetName(getPendingZoneSetIndex());
		}
		catch (...) { return {}; }
	}

	// ⚠⚠ THE COMMITTED ZONE SET IS ITS OWN GLOBAL. DO NOT DERIVE IT FROM THE PENDING INDEX.
	//
	// Two earlier attempts both failed, and both failed for the same reason - the pending index carries no
	// information at all once a switch is done:
	//   1. "when idle, pending == committed"     -> readout pinned to the first zone set, never followed.
	//   2. "latch pending on the preparing 1->0 edge" -> same symptom, because by the time the flag drops
	//      the index has ALREADY been destroyed.
	// Static analysis settles it. EVERY path that clears the preparing flag also stores 0xFFFFFFFF into
	// the pending index - exe+0x005E60FF, +0x005E8115, +0x005E87D1 and +0x005E891E, all four of them. So
	// whenever isPreparingZoneSet() is false the pending index is -1, and any scheme built on reading it
	// afterwards is reading a value the engine just wiped.
	//
	// The engine keeps the real thing at exe+0x04757CB0:
	//   * exe+0x00A7A810 is a one-instruction getter - `mov eax, [0x04757CB0]; ret`.
	//   * exe+0x00A7A870, the "is this zone set already active?" test switch_zone_set uses to decide
	//     whether a switch is even needed, compares its argument straight against it.
	//   * of the 19 references to it in the image, exactly ONE stores a real value: exe+0x00A7932F,
	//     `mov [0x04757CB0], esi`, directly after the engine logs "switching to" with that same index and
	//     immediately before it indexes [globals+0x264] + index*0x218 - the very array we read names from.
	//     Every other store writes -1, i.e. teardown.
	// So it is authoritative, it is correct from the moment the level loads (no attach-order caveat, which
	// is what the old code could never solve), and it needs no latch.
	std::string getCommittedZoneSetName() // throws
	{
		int32_t index = -1;
		if (!readAt(getExeBase() + kRvaZoneCommitted, index))
			throw HCMRuntimeException("Could not read the Halo 5 committed zone set index");
		if (index < 0)
			throw HCMRuntimeException("No Halo 5 zone set is currently active");
		return getZoneSetName(index);
	}

	int32_t getCommittedZoneSetIndex() // throws
	{
		int32_t index = -1;
		if (!readAt(getExeBase() + kRvaZoneCommitted, index))
			throw HCMRuntimeException("Could not read the Halo 5 committed zone set index");
		return index;
	}

	// ⚠⚠ PURE DATA WRITE - WE DO NOT CALL switch_zone_set. Read this before "simplifying" it into a call.
	//
	// The engine's own switch_zone_set (exe+0x005E8770) does almost nothing itself. On the branch that
	// actually starts a switch it just sets three globals:
	//     exe+0x005E87DD   or    byte  [0x050B0254], 4      request bit
	//     exe+0x005E87E6   mov   byte  [0x050B0255], 1      preparing
	//     exe+0x005E87ED   mov   dword [0x050B03B4], index  target
	// and the work happens later, on the engine's own thread: the main loop at exe+0x005E7D2F polls the
	// preparing flag and calls main_switch_structure_bsp (exe+0x005E8820) when it is set, which consumes
	// all three and clears them. So writing the three globals requests a switch exactly as the engine
	// would, while leaving the actual BSP work on the thread that is supposed to do it.
	//
	// That matters here: every engine call tried in this title faulted and killed the process, and every
	// plain store worked. This is the same shape as teleport and launch - no call, no pump, no thread
	// affinity requirement.
	//
	// The two calls in switch_zone_set we deliberately skip are exe+0x01C0CCF0 (a notify taking the
	// "did we start one" bool) and exe+0x02075F80, which is gated behind exe+0x0135F060 and replicates the
	// switch to remote clients. Skipping the latter is correct for our use: HCM only drives the local
	// session, and forcing a replicated switch from a client is exactly the desync we do not want.
	//
	// ⚠ WRITE ORDER IS LOAD-BEARING. The engine's main loop can run between our stores, so the target
	// index must be in place BEFORE the flag that makes the loop read it. Index, then request bit, then
	// the preparing flag last.
	void requestZoneSetSwitch(int32_t index) // throws
	{
		const int32_t count = getZoneSetCount();   // also proves a scenario is loaded
		if (index < 0 || index >= count)
			throw HCMRuntimeException(std::format("Zone set index {} is out of range (0..{})", index, count - 1));

		if (isPreparingZoneSet())
			throw HCMRuntimeException("A Halo 5 zone set switch is already in progress - wait for it to finish");

		const uintptr_t exe = getExeBase();

		if (!sehWrite((void*)(exe + kRvaZonePending), &index, sizeof(index)))
			throw HCMRuntimeException("Could not write the Halo 5 pending zone set index");

		uint8_t bits = 0;
		if (!readAt(exe + kRvaZoneRequestBits, bits))
			throw HCMRuntimeException("Could not read the Halo 5 zone set request flags");
		bits |= 4;
		if (!sehWrite((void*)(exe + kRvaZoneRequestBits), &bits, sizeof(bits)))
			throw HCMRuntimeException("Could not write the Halo 5 zone set request flags");

		const uint8_t preparing = 1;
		if (!sehWrite((void*)(exe + kRvaZonePreparing), &preparing, sizeof(preparing)))
			throw HCMRuntimeException("Could not arm the Halo 5 zone set switch");
	}

	void requestCheckpoint(uint32_t mode)
	{
		const uintptr_t req = getSaveRequestAddress();
		if (!sehWrite((void*)req, &mode, sizeof(mode)))
			throw HCMRuntimeException("Could not write the Halo 5 checkpoint request");
	}

	// Resolve the player's character-controller proxy. See the block at the top of this file for how this
	// chain was found and why it is the only correct write target.
	//
	// Returns the proxy address and fills outCentreOffset with the body-centre-to-object-origin delta, which
	// the caller must add to any world position it wants the player to end up at.
	//
	// ⚠ FAILS CLOSED. Anything ambiguous throws rather than guessing: a wrong element here teleports some
	// other body (a dropped weapon, a teammate, a vehicle) instead of the player.
	// hknp's per-body property map is open-addressed, 16-byte entries {u16 key; ...; void* value @ +0x08}.
	// The hash is the engine's own (exe+0x00C9CAD0): ((key & 0xFFFF) >> 4) * 0x9E3779B1.
	// ⚠ CALLED FRESH EVERY RESOLVE, ON PURPOSE. Caching the returned payload pointer would leave a dangling
	// pointer the moment hknp reallocates the property array - and because HCM runs in-process, a reused
	// allocation could even validate against a stale body id. The probe is at most 16 reads.
	uintptr_t lookupBodyProperty(uintptr_t table, uint32_t mask, uint32_t key)
	{
		const uint32_t k16 = key & 0xFFFFu;
		uint32_t i = (uint32_t)((k16 >> 4) * 0x9E3779B1u) & mask;
		for (uint32_t probe = 0; probe <= mask; ++probe)
		{
			const uintptr_t e = table + (uintptr_t)i * 16;
			uint32_t k = 0;
			if (!readAt(e, k)) return 0;
			k &= 0xFFFFu;
			if (k == 0xFFFFu) return 0;          // empty slot - the key is genuinely absent
			if (k == k16)
			{
				uintptr_t v = 0;
				return readAt(e + 8, v) ? v : 0;
			}
			i = (i + 1) & mask;
		}
		return 0;
	}

	// Resolve the player's character-controller motion element, and the body-centre-to-object-origin delta.
	//
	// ⚠⚠ THIS IS AN IDENTITY MATCH, NOT A SEARCH. It used to scan both physics arrays for the element
	// nearest to (objectPosition + centreOffset) and refuse when the runner-up was within 0.10 wu, which is
	// why Force Teleport and Force Launch failed whenever anything stood close to you. hknp already knows
	// which body belongs to which object; we just ask it. Nothing that belongs to somebody else can be a
	// candidate now, at any distance.
	uintptr_t resolveCharacterProxy(SimpleMath::Vector3& outCentreOffset)
	{
		const uintptr_t exeBase = getExeBase();
		const uintptr_t object = getPlayerObject();
		const uint32_t playerDatum = getPlayerDatum();

		uintptr_t world = 0;
		if (!readAt(exeBase + kRvaPhysicsWorld, world) || !world)
			throw HCMRuntimeException("The Halo 5 physics world is not loaded");

		// --- 1. the player's havok component, and its back-reference ---
		uint32_t compDatum = 0;
		if (!readAt(object + kObjectHavokComponent, compDatum) || compDatum == 0xFFFFFFFFu)
			throw HCMRuntimeException("The player has no physics body right now. This is normal for a moment "
				"after spawning, and while riding a vehicle.");

		uintptr_t hc = 0;
		if (!readAt(exeBase + kRvaHavokComponents, hc) || !hc)
			throw HCMRuntimeException("The Halo 5 havok component array is not loaded");

		uint32_t hcStride = 0; int32_t hcBound = 0; uintptr_t hcData = 0;
		if (!readAt(hc + kHcElementStride, hcStride) || !hcStride
			|| !readAt(hc + kHcBound, hcBound)
			|| !readAt(hc + kHcData, hcData) || !hcData)
			throw HCMRuntimeException("Could not read the Halo 5 havok component array");

		const uint32_t hcIndex = compDatum & 0xFFFFu;
		if (hcBound <= 0 || (int32_t)hcIndex >= hcBound)
			throw HCMRuntimeException("The player's physics component index is out of range");

		// ⚠ KEEP THIS CHECK. Component datums are recycled, so a stale one can land on a live component
		// owned by something else. Requiring the component to point BACK at our object is what makes that
		// harmless. It is two reads; do not drop it as redundant.
		uint32_t owner = 0;
		if (!readAt(hcData + (uintptr_t)hcIndex * hcStride + kComponentOwnerObject, owner))
			throw HCMRuntimeException("Could not read the Halo 5 physics component");
		if (owner != playerDatum)
			throw HCMRuntimeException("The player's physics component does not point back at the player - it "
				"has most likely just been recycled. Try again.");

		// --- 2. ask hknp which body belongs to this object ---
		uintptr_t propTable = 0; int32_t propMask = 0;
		if (!readAt(world + kWorldPropTable, propTable) || !propTable
			|| !readAt(world + kWorldPropMask, propMask) || propMask <= 0)
			throw HCMRuntimeException("Could not read the Halo 5 physics property map");

		uint32_t wantValue = playerDatum;
		uintptr_t prop = lookupBodyProperty(propTable, (uint32_t)propMask, kPropBodyObject);
		if (!prop)
		{
			// Fall back to the component map - same shape, one more indirection, same guarantee.
			prop = lookupBodyProperty(propTable, (uint32_t)propMask, kPropBodyComponent);
			wantValue = compDatum;
		}
		if (!prop)
			throw HCMRuntimeException("Halo 5's physics body-to-owner map is missing");

		uintptr_t bitmap = 0, values = 0, bodies = 0, motions = 0;
		uint32_t propSize = 0, bodyCount = 0;
		if (!readAt(prop + kPropBitmap, bitmap) || !bitmap
			|| !readAt(prop + kPropValues, values) || !values
			|| !readAt(prop + kPropSize, propSize)
			|| !readAt(world + kWorldBodyCount, bodyCount)
			|| !readAt(world + kWorldMsArray, bodies) || !bodies
			|| !readAt(world + kWorldProxyArray, motions) || !motions)
			throw HCMRuntimeException("Could not read the Halo 5 physics body tables");

		bodyCount &= 0x3FFFFFFFu;
		const uint32_t n = (propSize < bodyCount) ? propSize : bodyCount;
		if (!n || n > kMaxArrayElements * 4u)
			throw HCMRuntimeException("Halo 5's physics body count is out of range");

		// Two bulk copies (~16KB) instead of a read per element - the old scan touched about 1MB per call,
		// every frame, which is most of why this function showed up in the profile at all.
		std::vector<uint8_t> bm((n + 7u) / 8u);
		std::vector<uint32_t> vals(n);
		if (!sehCopy(bm.data(), (const void*)bitmap, bm.size())
			|| !sehCopy(vals.data(), (const void*)values, (size_t)n * sizeof(uint32_t)))
			throw HCMRuntimeException("Could not read the Halo 5 physics body owner map");

		uint32_t foundBody = 0, foundMotion = 0, matches = 0;
		for (uint32_t b = 0; b < n; ++b)
		{
			if (!((bm[b >> 3] >> (b & 7)) & 1u)) continue;     // body slot not in use
			if (vals[b] != wantValue) continue;                // belongs to someone else
			uint32_t motionId = 0;
			if (!readAt(bodies + (uintptr_t)b * kMsStride + kBodyMotionId, motionId)) continue;
			if (!motionId) continue;                           // static / keyframed: nothing to move
			++matches;
			foundBody = b;
			foundMotion = motionId;
		}

		// ⚠ STILL FAILS CLOSED - but the only possible ambiguity now is between the PLAYER'S OWN bodies
		// (a ragdoll, say). A teammate, a dropped weapon or a vehicle can never reach this point.
		if (matches == 0)
			throw HCMRuntimeException("The player has no moveable physics body right now. This is normal "
				"while dead, mid-spawn, or riding a vehicle.");
		if (matches > 1)
			throw HCMRuntimeException(std::format(
				"The player has {} moveable physics bodies right now, so there is no single one to move.",
				matches));

		const uintptr_t body = bodies + (uintptr_t)foundBody * kMsStride;
		const uintptr_t motion = motions + (uintptr_t)foundMotion * kProxyStride;

		// hknp's own back-reference, so a stale motion index cannot slip through.
		uint32_t attached = 0;
		if (!readAt(motion + kMotionAttachedBody, attached) || attached != foundBody)
			throw HCMRuntimeException("The player's physics body and motion disagree. Try again.");

		// --- 3. the centre offset, MEASURED rather than inferred ---
		// ⚠ NOT kMsCentreX/Y/Z. That triple is the translation column of the body's 3x4 transform - a
		// BODY-LOCAL vector - and it only equals the world-space delta while the basis is near identity.
		// Measured on a body with a flipped basis it comes back with an inverted axis, producing a 0.55
		// residual against the correct element and a refusal with nothing else anywhere near the player.
		// This subtraction is exact in every orientation.
		float com[3]{}, tr[3]{};
		if (!sehCopy(com, (const void*)(motion + kProxyPosition), sizeof(com))
			|| !sehCopy(tr, (const void*)(body + kMsTranslation), sizeof(tr)))
			throw HCMRuntimeException("Could not read the Halo 5 centre-of-mass offset");
		for (int i = 0; i < 3; ++i)
			if (!std::isfinite(com[i]) || !std::isfinite(tr[i]))
				throw HCMRuntimeException("The Halo 5 centre-of-mass offset is not finite");

		outCentreOffset = { com[0] - tr[0], com[1] - tr[1], com[2] - tr[2] };
		return motion;
	}

	// FORCE TELEPORT. One 12-byte store into the physics authority - see the block at the top of this file.
	// ⚠ No engine call, no pump, and no requirement to be on the simulation thread: this is a plain store,
	// exactly like MCC's ForceTeleport, and it is safe from HCM's hotkey thread.
	// ⚠ CACHED. Teleport used to do a cold resolve every press, which meant it re-ran the array scan - and
	// therefore re-rolled the "something is next to you" dice - on every single use. Going through the
	// cache means the scan happens ONCE and every later press revalidates with a 12-byte read instead.
	// With Display 2D Game Info open the cache is already warm (it polls velocity at 30Hz), so in practice
	// the scan never runs at teleport time at all.
	void teleportPlayerTo(SimpleMath::Vector3 target)
	{
		SimpleMath::Vector3 centreOffset{};
		const uintptr_t proxy = proxyCachedOrResolve(centreOffset);

		if (!sehWriteVec3(proxy + kProxyPosition,
			target.x + centreOffset.x, target.y + centreOffset.y, target.z + centreOffset.z))
			throw HCMRuntimeException("Could not write the Halo 5 character controller position");

		// We have just desynced the proxy from the published object position on purpose. Trust the cached
		// address for a moment so a second teleport does not re-resolve against the stale one.
		mCacheTrustedUntil = std::chrono::steady_clock::now() + kCacheTrustWindow;
	}

	// Resolves the observer currently being rendered from. See the offsets block for the chain and for why
	// this is NOT the same thing as getCameraPosition().
	// ⚠ Independent of the player: it resolves at a menu, during a cinematic and while dead, which is
	// exactly when the player-derived camera is useless.
	uintptr_t resolveObserver()
	{
		const uintptr_t tls = getTlsBase();
		if (!tls) throw HCMRuntimeException("No Halo 5 simulation thread TLS block");

		uintptr_t directors = 0, observers = 0;
		if (!readAt(tls + kTlsDirectors, directors) || !directors)
			throw HCMRuntimeException("The Halo 5 camera director block is null (no level loaded?)");
		if (!readAt(tls + kTlsObservers, observers) || !observers)
			throw HCMRuntimeException("The Halo 5 observer block is null (no level loaded?)");

		int32_t watched = 0;
		if (!readAt(directors + kDirectorWatched, watched)) watched = 0;
		// ⚠ Clamp rather than trust: a garbage index here would read an arbitrary address as a camera.
		if (watched < 0 || watched > 3) watched = 0;

		return observers + (uintptr_t)watched * kObserverStride;
	}

	SimpleMath::Vector3 getObserverPosition()
	{
		float p[3]{};
		if (!sehCopy(p, (const void*)(resolveObserver() + kObserverPosition), sizeof(p)))
			throw HCMRuntimeException("Could not read the Halo 5 observer position");
		return { p[0], p[1], p[2] };
	}

	SimpleMath::Vector3 getObserverForward()
	{
		float f[3]{};
		if (!sehCopy(f, (const void*)(resolveObserver() + kObserverForward), sizeof(f)))
			throw HCMRuntimeException("Could not read the Halo 5 observer forward");
		return { f[0], f[1], f[2] };
	}

	// ★ THE POSITION THE CHARACTER CONTROLLER IS ACTUALLY AT - the authority we write, not the object's
	// published mirror.
	//
	// ⚠⚠ USE THIS, NOT getPlayerPosition(), FOR ANYTHING THAT READS-THEN-WRITES A POSITION. The object's
	// published position (kObjectPublishedPosition) is only refreshed while the player is MOVING, so:
	//   * two relative teleports in quick succession both read the SAME stale position and target the same
	//     point - which is exactly "Force Teleport is not additive";
	//   * while PAUSED the mirror never refreshes at all, so a relative teleport computed from it goes
	//     nowhere no matter how many times it is pressed.
	// The proxy is refreshed by the controller itself and is correct in both cases.
	SimpleMath::Vector3 getProxyPosition()
	{
		SimpleMath::Vector3 centreOffset{};
		const uintptr_t proxy = proxyCachedOrResolve(centreOffset);
		float p[3]{};
		if (!sehCopy(p, (const void*)(proxy + kProxyPosition), sizeof(p)))
			throw HCMRuntimeException("Could not read the Halo 5 character controller position");
		// Undo the centre compensation teleportPlayerTo applies, so this and teleportPlayerTo speak the
		// same coordinate space and a read/modify/write round-trips exactly.
		return { p[0] - centreOffset.x, p[1] - centreOffset.y, p[2] - centreOffset.z };
	}

	// The velocity the character controller integrates - see kProxyVelocity. Unlike the position, this
	// needs no centre compensation: it is a rate, not a point.
	// ⚠ CACHED RESOLVE. The 2D info overlay reads this EVERY FRAME, and a full resolve scans ~1MB across
	// two arrays. proxyCachedOrResolve revalidates with a single 12-byte read and only re-scans on drift.
	SimpleMath::Vector3 getPlayerVelocity()
	{
		const uintptr_t proxy = proxyCachedOrResolve();
		float v[3]{};
		if (!sehCopy(v, (const void*)(proxy + kProxyVelocity), sizeof(v)))
			throw HCMRuntimeException("Could not read the Halo 5 player velocity");
		return { v[0], v[1], v[2] };
	}

	// ⚠ THE READOUT VARIANTS. Same values, but nullopt instead of an exception when the player is not
	// currently resolvable. Anything that polls every frame and can simply show a dash should use these:
	// an "expected" failure at a menu is not an error, and routing it through HCMRuntimeException costs a
	// stack walk plus two disk writes EVERY FRAME (see the backoff note above for the measured damage).
	// The throwing versions stay exactly as they are for Force Teleport / Force Launch, which must fail
	// closed and must tell the user WHY.
	std::optional<SimpleMath::Vector3> tryGetPlayerVelocity() noexcept
	{
		SimpleMath::Vector3 centreOffset{};
		uintptr_t proxy = 0;
		if (!tryProxyCachedOrResolve(centreOffset, proxy)) return std::nullopt;
		float v[3]{};
		if (!sehCopy(v, (const void*)(proxy + kProxyVelocity), sizeof(v))) return std::nullopt;
		return SimpleMath::Vector3{ v[0], v[1], v[2] };
	}

	std::optional<SimpleMath::Vector3> tryGetProxyPosition() noexcept
	{
		SimpleMath::Vector3 centreOffset{};
		uintptr_t proxy = 0;
		if (!tryProxyCachedOrResolve(centreOffset, proxy)) return std::nullopt;
		float p[3]{};
		if (!sehCopy(p, (const void*)(proxy + kProxyPosition), sizeof(p))) return std::nullopt;
		return SimpleMath::Vector3{ p[0] - centreOffset.x, p[1] - centreOffset.y, p[2] - centreOffset.z };
	}

	// Cached for the same reason as teleportPlayerTo - this is Force Launch's write path.
	void setPlayerVelocity(SimpleMath::Vector3 velocity)
	{
		const uintptr_t proxy = proxyCachedOrResolve();
		if (!sehWriteVec3(proxy + kProxyVelocity, velocity.x, velocity.y, velocity.z))
			throw HCMRuntimeException("Could not write the Halo 5 player velocity");
	}

	// ⚠ PER-FRAME CALLERS MUST NOT RE-RESOLVE. resolveCharacterProxy scans two arrays (~576KB + ~512KB);
	// that is nothing once per keypress and far too much 60 times a second. So cache the address and
	// revalidate it with a single 12-byte read: the proxy must still sit at objectPosition + centreOffset.
	// Anything else (death, respawn, level load, the arrays being rebuilt) fails that check and forces one
	// full re-resolve.
	uintptr_t mCachedProxy = 0;
	uintptr_t mCachedProxyArray = 0;
	SimpleMath::Vector3 mCachedCentreOffset{};

	// ⚠⚠ GRACE WINDOW AFTER WE OURSELVES MOVE THE PLAYER. Without this, back-to-back teleports are unsafe.
	// obj+0x224 is only republished while the player is MOVING - which is exactly what "it stored it until
	// i moved" means. So immediately after a teleport the proxy is at the new position while the object
	// still reports the old one, and the usual revalidation (proxy ?= objPos + centre) fails. The
	// re-resolve that follows would then scan for a body near the STALE position, where our proxy no
	// longer is - at best it throws, at worst it locks onto whatever else is standing back there.
	// Inside this window we trust the cached address without the position test. That is sound because we
	// just wrote to it and the container identity check below still has to pass.
	std::chrono::steady_clock::time_point mCacheTrustedUntil{};
	static constexpr std::chrono::milliseconds kCacheTrustWindow{ 2000 };

	// ⚠⚠ FAILURE BACKOFF. NOT A LATCH - READ THIS BEFORE CHANGING IT.
	//
	// THE PROBLEM IT SOLVES. Whenever the player is unresolvable - at a menu, during a load, while dead,
	// mid-revert - the cache revalidation fails, and every caller then ran a FULL re-resolve: two linear
	// scans over ~1MB of physics arrays, which then threw at the very end. The 2D info overlay asks every
	// frame, so that ran 60 times a second. Measured in one day of logs: 15,700 throws out of
	// resolveCharacterProxy, 5,065 of them "Two bodies are indistinguishably close to you (0.000 vs
	// 0.000)" - which is simply what the scan reports when the player position reads as zero because there
	// is no player.
	//
	// AND EVERY ONE OF THOSE THROWS IS EXPENSIVE. HCMExceptionBase's constructor does
	// std::to_string(std::stacktrace::current()) - a full stack walk and symbolisation, which takes
	// dbghelp's global lock - and then writes two PLOG_ERROR lines to disk. Doing that 60 times a second
	// from the render path is not "a noisy log", it is a per-frame stall.
	//
	// WHAT THIS DOES. After a failed resolve, wait a little before paying for another one. The wait grows
	// with consecutive failures and is capped, so a long stay at a menu costs ~1 attempt a second instead
	// of 60.
	//
	// ⚠⚠ IT MUST NEVER BECOME A DISABLE. The 2D info overlay is expected to come back BY ITSELF the moment
	// the player is controllable again - a readout that needs a manual toggle to recover is useless. So:
	// the backoff only ever delays a RETRY, never suppresses one permanently, and any success clears it
	// instantly (see noteProxyResolveSucceeded). There is deliberately no "give up" state.
	std::chrono::steady_clock::time_point mProxyRetryAfter{};
	uint32_t mProxyFailureStreak = 0;
	static constexpr std::chrono::milliseconds kProxyBackoffFirst{ 100 };
	static constexpr std::chrono::milliseconds kProxyBackoffMax{ 1000 };

	// True while we are inside the wait window after a failure. noexcept and free - two loads.
	bool proxyResolveIsBackedOff() const noexcept
	{
		return mProxyFailureStreak != 0 && std::chrono::steady_clock::now() < mProxyRetryAfter;
	}

	void noteProxyResolveFailed() noexcept
	{
		if (mProxyFailureStreak < 32) ++mProxyFailureStreak;   // saturate; this only feeds the shift below
		auto wait = kProxyBackoffFirst * (1u << (std::min)(mProxyFailureStreak - 1, 4u));
		if (wait > kProxyBackoffMax) wait = kProxyBackoffMax;
		mProxyRetryAfter = std::chrono::steady_clock::now() + wait;
	}

	// ⚠ Clears the backoff COMPLETELY, not partially. One good resolve means the player is back, so the
	// next failure should get the short wait again rather than inheriting a long one from an old menu.
	void noteProxyResolveSucceeded() noexcept
	{
		mProxyFailureStreak = 0;
		mProxyRetryAfter = {};
	}

	// ⚠⚠ THE POSITION CHECK ALONE IS NOT A SAFE REVALIDATION ACROSS A BSP / ZONE SET SWITCH.
	// Switching BSP tears down and rebuilds the physics world, so mCachedProxy is left pointing at freed
	// memory. sehCopy makes an UNMAPPED page harmless, but a page the allocator has since REUSED reads
	// back fine - and the position gate below accepts anything within 1.0 wu, which some unrelated float
	// triple in a recycled block can easily satisfy. We would then write 12 bytes of velocity into a
	// stranger's allocation every frame.
	//
	// So identity-check the container first: re-read world -> proxy array and require the cached address
	// to still be an element of THAT array. A rebuild moves the array pointer, which fails the check and
	// forces one clean re-resolve. Two pointer reads - nothing next to the scan it protects.
	uintptr_t proxyCachedOrResolve() { SimpleMath::Vector3 ignored{}; return proxyCachedOrResolve(ignored); }

	uintptr_t proxyCachedOrResolve(SimpleMath::Vector3& outCentreOffset)
	{
		if (mCachedProxy)
		{
			uintptr_t world = 0, proxyArray = 0;
			const bool arrayStillOurs =
				readAt(getExeBase() + kRvaPhysicsWorld, world) && world
				&& readAt(world + kWorldProxyArray, proxyArray) && proxyArray
				&& proxyArray == mCachedProxyArray
				&& mCachedProxy >= proxyArray
				&& ((mCachedProxy - proxyArray) % kProxyStride) == 0;

			// We moved the player ourselves a moment ago - the object position has not caught up yet, so the
			// position test below would wrongly reject a proxy we know is still ours. See kCacheTrustWindow.
			if (arrayStillOurs && std::chrono::steady_clock::now() < mCacheTrustedUntil)
			{
				outCentreOffset = mCachedCentreOffset;
				return mCachedProxy;
			}

			if (arrayStillOurs)
			{
				const uintptr_t object = getPlayerObject();
				float playerPos[3]{}, proxyPos[3]{};
				if (sehCopy(playerPos, (const void*)(object + kObjectPublishedPosition), sizeof(playerPos))
					&& sehCopy(proxyPos, (const void*)(mCachedProxy + kProxyPosition), sizeof(proxyPos)))
				{
					const float want[3] = { playerPos[0] + mCachedCentreOffset.x,
											playerPos[1] + mCachedCentreOffset.y,
											playerPos[2] + mCachedCentreOffset.z };
					// Generous: the player can be mid-launch when this runs, so only reject a proxy that has
					// clearly stopped being ours rather than one lagging a frame behind.
					if (dist3(proxyPos, want) < 1.0f)
					{
						outCentreOffset = mCachedCentreOffset;
						return mCachedProxy;
					}
				}
			}
			invalidateProxyCache();
		}

		// Reaching here means a FULL re-resolve - the expensive path. Record the outcome so repeated
		// failures back off (see mProxyRetryAfter). Note the cache is already 0 on every path that gets
		// here after a failure, which is what lets tryProxyCachedOrResolve gate on "cache empty AND backed
		// off" without ever skipping a cache that would have worked.
		try
		{
			mCachedProxy = resolveCharacterProxy(mCachedCentreOffset);
		}
		catch (...)
		{
			mCachedProxy = 0;          // explicit: never leave a stale address behind a failed resolve
			noteProxyResolveFailed();
			throw;
		}
		noteProxyResolveSucceeded();
		outCentreOffset = mCachedCentreOffset;

		// Remember which array it came out of, so the next frame can prove the world has not been rebuilt.
		uintptr_t world = 0, proxyArray = 0;
		if (readAt(getExeBase() + kRvaPhysicsWorld, world) && world)
			readAt(world + kWorldProxyArray, proxyArray);
		mCachedProxyArray = proxyArray;

		return mCachedProxy;
	}

	// ⚠ THE PER-FRAME ENTRY POINT. Use this, not proxyCachedOrResolve, from anything that runs every frame.
	//
	// Non-throwing in the caller's frame, and - crucially - it does not even ENTER the throwing path while
	// backed off, so a player sitting at a menu costs two atomic-free comparisons per frame instead of a
	// ~1MB scan plus a stack-walked, disk-logged exception.
	//
	// The gate is "cache is empty AND we are inside a backoff window". A populated cache always goes
	// through, because revalidating it is two pointer reads and a 12-byte compare - there is nothing to
	// throttle, and throttling it would make the readout stutter for no reason.
	//
	// ⚠ RECOVERY IS AUTOMATIC AND IMMEDIATE. Nothing here latches. The first frame after the backoff window
	// expires attempts a real resolve, and a success clears the streak outright, so the overlay repopulates
	// on its own within at most kProxyBackoffMax of the player becoming controllable again.
	bool tryProxyCachedOrResolve(SimpleMath::Vector3& outCentreOffset, uintptr_t& outProxy) noexcept
	{
		if (!mCachedProxy && proxyResolveIsBackedOff())
			return false;

		try
		{
			outProxy = proxyCachedOrResolve(outCentreOffset);
			return outProxy != 0;
		}
		catch (...)
		{
			// proxyCachedOrResolve already recorded the failure and armed the backoff.
			return false;
		}
	}

	// ================================================================================================
	// MAP / SCENARIO INTERNAL NAME
	//
	// WHY THIS IS A SCAN AND NOT A POINTER CHAIN. There is no cheap path to it - all three obvious ones
	// were tried against a live process and all three failed:
	//   * zone set names do NOT carry the level prefix. w2_grotto's happen to be "w2_grotto_*", which
	//     looks like a rule until you load w1_unconfirmed_reports and get "zs005_landing",
	//     "zs010_chiefship", "cin090_bridge" - longest common prefix is the empty string.
	//   * no qword anywhere in scenario globals[0x0000..0x4000] dereferences to level-ish ASCII.
	//   * the "maps\<name>.map" string IS resident and unique, but every qword holding its address lives
	//     in the same heap block as the string. Nothing inside the exe image points at it, so there is no
	//     ASLR-stable RVA to put in InternalPointerData.xml.
	// What is left is finding the string itself.
	//
	// ⚠⚠ SO IT RUNS EXACTLY ONCE PER LEVEL, ON A WORKER THREAD, NEVER ON THE RENDER PATH. getMapName() is
	// called from the 2D overlay's ~30Hz update; it only ever reads the cache and, at most, kicks off a
	// scan. It never blocks and never scans inline. Returning "" simply means "not known yet".
	//
	// ⚠ THE CACHE IS KEYED ON THE SCENARIO GLOBALS POINTER. That is what actually changes when a level is
	// torn down and rebuilt, so it is the level identity. Do not key it on zone set or BSP state, both of
	// which change WITHIN a level.
	//
	// ⚠ THE THREAD MUST NOT TOUCH `this`. HCM stays resident across sessions and this Impl can be
	// destroyed while a scan is in flight, so everything the worker uses lives in a shared_ptr it owns a
	// reference to. Do not "simplify" this by capturing the Impl.
	struct MapNameState
	{
		std::mutex m;
		std::string name;              // guarded by m
		uintptr_t derivedFrom = 0;     // guarded by m - the scenario globals it was scanned for
		std::atomic<bool> scanning{ false };
	};
	std::shared_ptr<MapNameState> mMapNameState = std::make_shared<MapNameState>();

	static void scanForMapName(std::shared_ptr<MapNameState> state, uintptr_t forGlobals) noexcept
	{
		std::string found;
		try
		{
			// "maps\" + name + ".map". Distinctive enough that one pass is enough, and short enough that
			// a straight memmem over committed private memory is the whole algorithm.
			static const char kPrefix[] = "maps\\";
			MEMORY_BASIC_INFORMATION mbi{};
			uintptr_t addr = 0;
			std::vector<char> buf;
			while (found.empty() && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
			{
				const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
				const DWORD prot = mbi.Protect & 0xFF;
				const bool readable = prot == PAGE_READONLY || prot == PAGE_READWRITE
					|| prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE;
				// Private committed memory only - the map name is heap tag data, never image or mapped file.
				if (mbi.State == MEM_COMMIT && readable && mbi.Type == MEM_PRIVATE
					&& mbi.RegionSize <= 64u * 1024u * 1024u)
				{
					buf.resize(mbi.RegionSize);
					SIZE_T got = 0;
					if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, buf.data(), mbi.RegionSize, &got) && got)
					{
						const char* b = buf.data();
						const char* end = b + got;
						for (const char* p = b; (p = (const char*)memchr(p, 'm', end - p)) != nullptr; ++p)
						{
							if ((size_t)(end - p) < sizeof(kPrefix)) break;
							if (memcmp(p, kPrefix, sizeof(kPrefix) - 1) != 0) continue;
							const char* n = p + sizeof(kPrefix) - 1;
							const char* q = n;
							// Explicit char classes rather than isalnum: no locale dependence, no extra header,
							// and map names are only ever [a-z0-9_] in practice.
							while (q < end && q - n < 64
								&& ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')
									|| (*q >= '0' && *q <= '9') || *q == '_')) ++q;
							if (q > n && (size_t)(end - q) >= 4 && memcmp(q, ".map", 4) == 0)
							{
								found.assign(n, q - n);
								break;
							}
						}
					}
				}
				if (next <= addr) break;
				addr = next;
			}
		}
		catch (...) { found.clear(); }

		{
			std::scoped_lock lk(state->m);
			state->name = found;
			state->derivedFrom = forGlobals;   // record even on failure, so we do not rescan every frame
		}
		state->scanning.store(false, std::memory_order_release);
	}

	// Returns "" until the scan lands (or if it found nothing). Never blocks.
	std::string getMapName() noexcept
	{
		uintptr_t globals = 0;
		if (!readAt(getExeBase() + kRvaScenarioGlobals, globals) || !globals)
			return {};

		{
			std::scoped_lock lk(mMapNameState->m);
			if (mMapNameState->derivedFrom == globals)
				return mMapNameState->name;      // may be "" if the scan genuinely found nothing
		}

		bool expected = false;
		if (mMapNameState->scanning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			auto state = mMapNameState;
			std::thread(&H5GetPlayerStateImpl::scanForMapName, state, globals).detach();
		}
		return {};
	}

	// Drop the cache so the next access does a full re-resolve. Cheap, and always safe to call.
	void invalidateProxyCache() noexcept
	{
		mCachedProxy = 0;
		mCachedProxyArray = 0;
		mCachedCentreOffset = {};
		mCacheTrustedUntil = {};   // an explicit invalidate outranks the post-teleport grace window
	}

	// Read-modify-write against the cached proxy. Used by Acrophobia every frame.
	void modifyPlayerVelocity(const std::function<SimpleMath::Vector3(SimpleMath::Vector3)>& fn)
	{
		const uintptr_t proxy = proxyCachedOrResolve();
		float v[3]{};
		if (!sehCopy(v, (const void*)(proxy + kProxyVelocity), sizeof(v)))
			throw HCMRuntimeException("Could not read the Halo 5 player velocity");
		const SimpleMath::Vector3 out = fn({ v[0], v[1], v[2] });
		if (!sehWriteVec3(proxy + kProxyVelocity, out.x, out.y, out.z))
			throw HCMRuntimeException("Could not write the Halo 5 player velocity");
	}
};


H5GetPlayerState::H5GetPlayerState(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<H5GetPlayerStateImpl>(game, dicon))
{
}

H5GetPlayerState::~H5GetPlayerState() { PLOG_VERBOSE << "~" << getName(); }

uintptr_t H5GetPlayerState::getExeBase() { return pimpl->getExeBase(); }
uintptr_t H5GetPlayerState::getTlsBase() { return pimpl->getTlsBase(); }
bool      H5GetPlayerState::hasObjectWriteGate() { return pimpl->hasObjectWriteGate(); }
uintptr_t H5GetPlayerState::getPlayerGlobals() { return pimpl->getPlayerGlobals(); }
uintptr_t H5GetPlayerState::getObjectGlobals() { return pimpl->getObjectGlobals(); }
uintptr_t H5GetPlayerState::getPlayerArray() { return pimpl->getPlayerArray(); }
uint32_t  H5GetPlayerState::getPlayerDatum() { return pimpl->getPlayerDatum(); }
uintptr_t H5GetPlayerState::getPlayerObject() { return pimpl->getPlayerObject(); }
uintptr_t H5GetPlayerState::getSaveRequestAddress() { return pimpl->getSaveRequestAddress(); }
SimpleMath::Vector3 H5GetPlayerState::getPlayerPosition() { return pimpl->getPlayerPosition(); }
SimpleMath::Vector3 H5GetPlayerState::getProxyPosition() { return pimpl->getProxyPosition(); }
SimpleMath::Vector3 H5GetPlayerState::getObserverPosition() { return pimpl->getObserverPosition(); }
SimpleMath::Vector3 H5GetPlayerState::getObserverForward() { return pimpl->getObserverForward(); }
SimpleMath::Vector3 H5GetPlayerState::getPlayerAim() { return pimpl->getPlayerAim(); }
SimpleMath::Vector3 H5GetPlayerState::getCameraPosition() { return pimpl->getCameraPosition(); }
float H5GetPlayerState::getCameraFovDegrees() { return pimpl->getCameraFovDegrees(); }
std::string H5GetPlayerState::getSimulationKind() { return pimpl->getSimulationKind(); }
bool H5GetPlayerState::isLocalSimulation() { return pimpl->getSimulationKind() == "local"; }
int32_t H5GetPlayerState::getZoneSetCount() { return pimpl->getZoneSetCount(); }
std::string H5GetPlayerState::getZoneSetName(int32_t index) { return pimpl->getZoneSetName(index); }
bool H5GetPlayerState::isPreparingZoneSet() noexcept { return pimpl->isPreparingZoneSet(); }
std::string H5GetPlayerState::getPreparedZoneSetName() noexcept { return pimpl->getPreparedZoneSetName(); }
std::string H5GetPlayerState::getCommittedZoneSetName() { return pimpl->getCommittedZoneSetName(); }
std::string H5GetPlayerState::getMapName() noexcept { return pimpl->getMapName(); }
int32_t H5GetPlayerState::getCommittedZoneSetIndex() { return pimpl->getCommittedZoneSetIndex(); }
void H5GetPlayerState::requestZoneSetSwitch(int32_t index) { pimpl->requestZoneSetSwitch(index); }
void H5GetPlayerState::requestCheckpoint(uint32_t mode) { pimpl->requestCheckpoint(mode); }
void H5GetPlayerState::teleportPlayerTo(SimpleMath::Vector3 target) { pimpl->teleportPlayerTo(target); }

SimpleMath::Vector3 H5GetPlayerState::teleportPlayerBy(SimpleMath::Vector3 offset)
{
	// ⚠ PROXY, NOT THE OBJECT'S PUBLISHED POSITION. See getProxyPosition: the published mirror only refreshes
	// while the player is moving, so basing the delta on it made back-to-back teleports land on the same
	// point (not additive) and made relative teleport a no-op while paused.
	const auto current = pimpl->getProxyPosition();
	const auto target = current + offset;
	pimpl->teleportPlayerTo(target);
	return target;
}

SimpleMath::Vector3 H5GetPlayerState::getPlayerVelocity() { return pimpl->getPlayerVelocity(); }
std::optional<SimpleMath::Vector3> H5GetPlayerState::tryGetPlayerVelocity() noexcept { return pimpl->tryGetPlayerVelocity(); }
std::optional<SimpleMath::Vector3> H5GetPlayerState::tryGetProxyPosition() noexcept { return pimpl->tryGetProxyPosition(); }
void H5GetPlayerState::setPlayerVelocity(SimpleMath::Vector3 v) { pimpl->setPlayerVelocity(v); }
void H5GetPlayerState::invalidateProxyCache() noexcept { pimpl->invalidateProxyCache(); }
void H5GetPlayerState::modifyPlayerVelocity(const std::function<SimpleMath::Vector3(SimpleMath::Vector3)>& fn)
{
	pimpl->modifyPlayerVelocity(fn);
}
