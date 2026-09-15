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
	constexpr uintptr_t kMsCentreX        = 0x0C;        // translation column of the 3x4 transform
	constexpr uintptr_t kMsCentreY        = 0x1C;
	constexpr uintptr_t kMsCentreZ        = 0x2C;
	constexpr uintptr_t kProxyStride      = 0x80;
	constexpr uintptr_t kProxyPosition    = 0x00;
	// ⚠ THE VELOCITY THE CONTROLLER ACTUALLY INTEGRATES. Identified the same way obj+0x248 was - scoring
	// every float triple in the element against measured d(position)/dt over 52.68 wu of walking:
	// +0x40 scored 0.150, +0x60 scored 0.151 (it mirrors +0x40), everything else scored 1.000 (no
	// correlation at all). Writing (0,0,12) here produced a clean ballistic arc - 12.7 wu of rise with
	// gravity decaying the velocity 9.75 -> 0 -> negative - so this field is an INPUT, unlike obj+0x248
	// which accepts writes and is never read by anything.
	constexpr uintptr_t kProxyVelocity    = 0x40;
	constexpr uint32_t  kMaxArrayElements = 4096;

	// ⚠⚠ MATCH THE NEAREST WITH A CLEAR MARGIN - DO NOT MATCH "EXACTLY ONE WITHIN A WINDOW".
	// Measured live: the player's own element matches EXACTLY (residual 0.000000), while the next nearest
	// body sits about 1.36 wu away. But that gap is situational - Blue Team AI walk right up to you, and a
	// 0.5 wu window caught FOUR candidates in one sample, which made the teleport refuse at random.
	// So: take the closest, require it to be essentially exact, and require the runner-up to be clearly
	// farther. That is robust to a teammate standing next to you AND to the few milliseconds of movement
	// between reading the player position and scanning the array.
	constexpr float kMatchAccept     = 0.25f;   // the true residual is ~0; this only absorbs read skew
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
	constexpr float kMatchMinGap     = 0.10f;   // runner-up must be this much WORSE than the winner

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
	uintptr_t resolveCharacterProxy(SimpleMath::Vector3& outCentreOffset)
	{
		const uintptr_t exeBase = getExeBase();
		const uintptr_t object = getPlayerObject();

		float playerPos[3]{};
		if (!sehCopy(playerPos, (const void*)(object + kObjectPublishedPosition), sizeof(playerPos)))
			throw HCMRuntimeException("Could not read the Halo 5 player position");

		uintptr_t world = 0;
		if (!readAt(exeBase + kRvaPhysicsWorld, world) || !world)
			throw HCMRuntimeException("The Halo 5 physics world is not loaded");

		// --- 1. the player's motion state, for the centre offset ---
		uintptr_t msArray = 0;
		if (!readAt(world + kWorldMsArray, msArray) || !msArray)
			throw HCMRuntimeException("The Halo 5 motion state array is null");

		const size_t msBytes = readableBytesFrom(msArray, (size_t)kMaxArrayElements * kMsStride);
		const uint32_t msCount = (uint32_t)(msBytes / kMsStride);
		if (!msCount) throw HCMRuntimeException("The Halo 5 motion state array is not readable");

		uintptr_t msElement = 0;
		float msBest = FLT_MAX, msSecond = FLT_MAX;
		for (uint32_t i = 0; i < msCount; ++i)
		{
			const uintptr_t el = msArray + (uintptr_t)i * kMsStride;
			float t[3]{};
			if (!sehCopy(t, (const void*)(el + kMsTranslation), sizeof(t))) continue;
			const float d = dist3(t, playerPos);
			if (d < msBest) { msSecond = msBest; msBest = d; msElement = el; }
			else if (d < msSecond) { msSecond = d; }
		}
		// ⚠ NO AMBIGUITY CHECK HERE, DELIBERATELY - and it is not an oversight.
		// All we want from the motion state is the CENTRE OFFSET, the body-centre-to-object-origin delta.
		// That is a property of the BIPED, not of which biped: a teammate standing next to you is another
		// Spartan with the same dimensions and therefore the same offset (measured 0.0017, -0.0018, 0.3625).
		// Picking the wrong one of two adjacent Spartans changes the answer by nothing measurable, so
		// demanding separation here only produced refusals with no safety benefit. The PROXY match below is
		// the identity-critical one, and that one still fails closed.
		if (!msElement || msBest > kMatchAccept)
			throw HCMRuntimeException(std::format(
				"Could not find the player's motion state (nearest {:.3f}). This is normal at a menu, during "
				"a load, or while dead - try again once you are in control.",
				msBest == FLT_MAX ? -1.f : msBest));

		float cx = 0.f, cy = 0.f, cz = 0.f;
		if (!readAt(msElement + kMsCentreX, cx) || !readAt(msElement + kMsCentreY, cy)
			|| !readAt(msElement + kMsCentreZ, cz))
			throw HCMRuntimeException("Could not read the Halo 5 centre-of-mass offset");

		// Sanity-gate it anyway. A body-centre delta is a fraction of a biped's height; anything larger
		// means we matched something that is not a character, and a wrong offset here would silently shift
		// every teleport. Cheap insurance against the check we just removed.
		if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz)
			|| std::abs(cx) > 2.f || std::abs(cy) > 2.f || std::abs(cz) > 2.f)
			throw HCMRuntimeException(std::format(
				"The Halo 5 centre-of-mass offset looks wrong ({:.3f}, {:.3f}, {:.3f})", cx, cy, cz));

		outCentreOffset = { cx, cy, cz };

		// --- 2. the proxy element whose position is exactly objectPosition + centreOffset ---
		uintptr_t proxyArray = 0;
		if (!readAt(world + kWorldProxyArray, proxyArray) || !proxyArray)
			throw HCMRuntimeException("The Halo 5 character controller array is null");

		const size_t pBytes = readableBytesFrom(proxyArray, (size_t)kMaxArrayElements * kProxyStride);
		const uint32_t pCount = (uint32_t)(pBytes / kProxyStride);
		if (!pCount) throw HCMRuntimeException("The Halo 5 character controller array is not readable");

		const float want[3] = { playerPos[0] + cx, playerPos[1] + cy, playerPos[2] + cz };
		uintptr_t proxy = 0;
		float pBest = FLT_MAX, pSecond = FLT_MAX;
		for (uint32_t k = 0; k < pCount; ++k)
		{
			const uintptr_t el = proxyArray + (uintptr_t)k * kProxyStride;
			float p[3]{};
			if (!sehCopy(p, (const void*)(el + kProxyPosition), sizeof(p))) continue;
			const float d = dist3(p, want);
			if (d < pBest) { pSecond = pBest; pBest = d; proxy = el; }
			else if (d < pSecond) { pSecond = d; }
		}
		// Identity-critical: a wrong element here launches or teleports a teammate instead of you, so this
		// one still fails closed. But on the RELATIVE test - see kMatchMinGap. Ours reads 0.000; a body
		// 0.4 wu away reads 0.400, which is a clear win, not an ambiguity.
		if (!proxy || pBest > kMatchAccept)
			throw HCMRuntimeException(std::format(
				"Could not find the player's character controller (nearest {:.3f}). This is normal at a menu, "
				"during a load, or while dead - try again once you are in control.",
				pBest == FLT_MAX ? -1.f : pBest));

		if (pSecond - pBest < kMatchMinGap)
			throw HCMRuntimeException(std::format(
				"Two bodies are indistinguishably close to you ({:.3f} vs {:.3f}) - move a step and try "
				"again.", pBest, pSecond == FLT_MAX ? -1.f : pSecond));

		return proxy;
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

		mCachedProxy = resolveCharacterProxy(mCachedCentreOffset);
		outCentreOffset = mCachedCentreOffset;

		// Remember which array it came out of, so the next frame can prove the world has not been rebuilt.
		uintptr_t world = 0, proxyArray = 0;
		if (readAt(getExeBase() + kRvaPhysicsWorld, world) && world)
			readAt(world + kWorldProxyArray, proxyArray);
		mCachedProxyArray = proxyArray;

		return mCachedProxy;
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
int32_t H5GetPlayerState::getCommittedZoneSetIndex() { return pimpl->getCommittedZoneSetIndex(); }
void H5GetPlayerState::requestZoneSetSwitch(int32_t index) { pimpl->requestZoneSetSwitch(index); }
void H5GetPlayerState::requestCheckpoint(uint32_t mode) { pimpl->requestCheckpoint(mode); }
void H5GetPlayerState::teleportPlayerTo(SimpleMath::Vector3 target) { pimpl->teleportPlayerTo(target); }

SimpleMath::Vector3 H5GetPlayerState::teleportPlayerBy(SimpleMath::Vector3 offset)
{
	const auto current = pimpl->getPlayerPosition();
	const auto target = current + offset;
	pimpl->teleportPlayerTo(target);
	return target;
}

SimpleMath::Vector3 H5GetPlayerState::getPlayerVelocity() { return pimpl->getPlayerVelocity(); }
void H5GetPlayerState::setPlayerVelocity(SimpleMath::Vector3 v) { pimpl->setPlayerVelocity(v); }
void H5GetPlayerState::invalidateProxyCache() noexcept { pimpl->invalidateProxyCache(); }
void H5GetPlayerState::modifyPlayerVelocity(const std::function<SimpleMath::Vector3(SimpleMath::Vector3)>& fn)
{
	pimpl->modifyPlayerVelocity(fn);
}
