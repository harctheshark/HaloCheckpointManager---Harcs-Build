#include "pch.h"
#include "MasterTickrate.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMakeOrGetCheat.h"
#include "GameTickEventHook.h"
#include "ScopedThreadSuspender.h"
#include "GameDataVersion.h"
#include <Windows.h>
#include <atomic>

// The master tickrate lives in the "game time globals" struct ( *(halo2.dll+0x15FE008) ): an int16 tickrate
// at +0x02 and a seconds-per-tick float at +0x04 (set together). 343's build runs at 60; the flip button
// toggles these two fields between 60/(1/60) and 30/(1/30).
//
// BOTH the flip button and the "Custom Tickrate (Hz)" input do the full job: as well as writing the tickrate
// int16 + dt, they match the collision "tickrate scalar" and force a live collision rebuild so the change is
// felt immediately (the button picks 30/60, the input picks an arbitrary rate):
//
//   * SCALAR: halo2.dll+0x70DBFA is `mulss xmm6,[rip->flt_180C32AA0]` in the Havok collision-shape builder -
//     the ×30.0 that 343 hardcoded (Season 7 read the live tickrate here). We repoint that rip-disp to a float
//     WE own (a page allocated near the module so rip+disp32 reaches it) and write (float)rate into it, so newly
//     built shapes bake radius × rate. See memory halo2dll-tickrate-scalar / Season7Physics_BuildTimeCache_HANDOFF.
//   * INSTANT: the scalar is baked into each shape at construction and cached (obj collision @ +0xB4), never
//     re-read per tick, so existing shapes keep the old value. The game's own per-object "recreate collision"
//     routine sub_180705B20 (RVA 0x705B20: remove body from Havok world -> tear down shape -> rebuild -> re-add)
//     rebuilds one object with the current scalar. We walk the object header table ourselves (ptr @ 0x18B7398,
//     capacity 2048, 12-byte entries; game accessor sub_1808D7000 @ 0x8D7000 returns 0 for empty slots) and call
//     it for every live object with a collision body, on the game thread (GameTickEventHook), so the new scalar
//     takes effect without a level reload. (Self-contained: ObjectTableRange isn't wired for Halo2.)
//
// Season7Physics patches the same 0x70DBFA instruction with a fixed 60.0; the custom scalar takes precedence
// (SettingsStateAndEvents::customTickrateScalarActive) and Season7Physics stands down while it's set.
namespace
{
	// Repoints the collision-radius scalar `mulss xmm6,[rip+disp32]` at halo2.dll+0x70DBFA to read a float we own,
	// letting us set the scalar to any value by writing that float. allocNear keeps the float within rip+disp32
	// reach of the site. Self-contained raw-Win32, gated on the v1.3528 opcode - mirrors UncapDropShadows.
	class TickrateScalarPatcher
	{
		static constexpr uint32_t kSiteRVA = 0x70DBFA;                       // mulss xmm6,[rip+disp32]
		static constexpr uint8_t  kOpcode[4] = { 0xF3, 0x0F, 0x59, 0x35 };   // build gate (opcode + modrm)
		static constexpr uint8_t  kDefaultDisp[4] = { 0x9E, 0x4E, 0x52, 0x00 }; // -> flt_180C32AA0 (30.0), stock

		uintptr_t base = 0;
		float* scalarSlot = nullptr; // our near float; the mulss reads this once we repoint
		bool applied = false;

		static void* allocNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO si; GetSystemInfo(&si);
			uintptr_t gran = si.dwAllocationGranularity;
			uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
			uintptr_t hi = target + 0x70000000ULL;
			uintptr_t startp = target & ~(gran - 1);
			for (uintptr_t p = startp; p > lo; p -= gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			for (uintptr_t p = startp + gran; p < hi; p += gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
			return nullptr;
		}

		// write to executable .text with other threads suspended (avoid a torn/half-written disp being executed)
		static void writeText(uintptr_t addr, const void* data, size_t len)
		{
			ScopedThreadSuspender suspend;
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len);
			VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}

	public:
		bool isApplied() const { return applied; }
		uintptr_t moduleBase() const { return base; }

		// false (no throw) if halo2.dll isn't loaded yet; throws if the site doesn't match build 1.3528.
		// halo2.dll can be UNLOADED and loaded again (end a game, start a new map): then our patch went with the old image,
		// `base` points at freed memory and the new image is unpatched. Drop the stale state (the near page may be out of
		// rip+disp32 range of the new image, so free it) - the caller re-applies. Never touch the old address again.
		bool stillLoaded() const { return base && (uintptr_t)GetModuleHandleA("halo2.dll") == base; }
		void forgetStale()
		{
			if (!applied || stillLoaded()) return;
			PLOG_INFO << "MasterTickrate: halo2.dll was reloaded; dropping the stale scalar patch";
			if (scalarSlot) { VirtualFree(scalarSlot, 0, MEM_RELEASE); scalarSlot = nullptr; }
			applied = false; base = 0;
		}

		bool apply()
		{
			forgetStale();
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;

			uintptr_t site = base + kSiteRVA;
			if (memcmp((void*)site, kOpcode, sizeof(kOpcode)) != 0)
				throw HCMRuntimeException("Tickrate scalar: opcode at halo2.dll+0x70DBFA doesn't match build 1.3528; refusing to patch.");

			scalarSlot = (float*)allocNear(base, 0x1000);
			if (!scalarSlot)
				throw HCMRuntimeException("Tickrate scalar: failed to allocate a near page for the scalar float.");
			*scalarSlot = 30.0f; // sane value until the first setScalar

			int64_t rel = (int64_t)(uintptr_t)scalarSlot - (int64_t)(site + 8); // rip = end of the 8-byte insn
			if (rel > INT32_MAX || rel < INT32_MIN)
			{
				VirtualFree(scalarSlot, 0, MEM_RELEASE); scalarSlot = nullptr;
				throw HCMRuntimeException("Tickrate scalar: near page out of rip+disp32 range (unexpected).");
			}
			int32_t disp = (int32_t)rel;
			writeText(site + 4, &disp, sizeof(disp)); // rewrite only the disp32 (bytes 4..7)
			applied = true;
			return true;
		}

		void setScalar(float f) { if (scalarSlot) *scalarSlot = f; } // 4-byte aligned store; read by the mulss

		void revert()
		{
			if (stillLoaded())   // a reloaded / unloaded halo2.dll has no patch to undo - and its old address is freed memory
				writeText(base + kSiteRVA + 4, kDefaultDisp, sizeof(kDefaultDisp)); // restore -> flt_180C32AA0 (30.0)
			if (scalarSlot) { VirtualFree(scalarSlot, 0, MEM_RELEASE); scalarSlot = nullptr; }
			applied = false;
		}
	};
// ================================================================================================
// THE OTHER GAMES. Halo 2 keeps its timing struct behind a MODULE GLOBAL, which is why the pointer
// data above works. It is the ODD ONE OUT: in Halo 3, ODST, Halo 4, Reach and Campaign Evolved the
// pointer is THREAD-LOCAL, and the field offsets inside the struct are not the same either.
//
//   game        struct pointer                     tickrate i16   dt f32
//   Halo 2      module global halo2.dll+0x15FE008      +0x02        +0x04
//   Halo 3      TLS slot 0xC8                          +0x04        +0x08
//   Halo 3 ODST TLS slot 0xC8                          +0x04        +0x08
//   Halo 4      TLS slot 0x90                          +0x06        +0x08
//   Halo Reach  TLS slot 0xA0                          +0x06        +0x08
//   HaloCER     sim TLS +0x98                          +0x06        (probed)
//
// ⚠⚠⚠ ASSUMING HALO 2's LAYOUT ELSEWHERE IS DESTRUCTIVE, NOT MERELY WRONG. In Halo 3, +0x02 is a
// WORD BITFIELD the engine manipulates with bts/btr - writing a tickrate there corrupts flags.
//
// ⚠ Halo 1 (CE) is deliberately absent. Its timing struct exists but has NO rate/dt pair at all -
// only a game_speed float and leftover-time accumulators. CE's 30 Hz is structural, not a variable,
// so it needs a different lever entirely and is not wired here.
//
// ⚠ Halo 2 is ALSO the only game with a collision "tickrate scalar" - user-confirmed - so the games
// below need the field write and nothing else.
// ================================================================================================

	struct TlsTickrateLayout
	{
		uint32_t     slot;       // byte offset into the module's TLS block holding the struct pointer
		uint32_t     rateOff;    // int16 tickrate within the struct
		uint32_t     dtOff;      // float seconds-per-tick; 0 = derive it at runtime, see probeDt
		const wchar_t* module;
	};

	// ODST "Season 5" (halo3odst.dll 1.2094): the game-time-globals TLS slot is 0xD0, not 0xC8 (the struct's own
	// field offsets +4/+8 are unchanged - same engine function, only the slot numbering moved). Set once from the
	// constructor; validateGtg still proves the struct (sane rate + dt == 1/rate) before anything is written.
	inline std::atomic<bool> gOdstSeason5Slots{ false };

	constexpr TlsTickrateLayout tlsLayoutFor(GameState::Value g)
	{
		switch (g)
		{
		case GameState::Value::Halo3:     return { 0xC8, 4, 8, L"halo3.dll" };
		case GameState::Value::Halo3ODST: return { 0xC8, 4, 8, L"halo3odst.dll" };
		case GameState::Value::Halo4:     return { 0x90, 6, 8, L"halo4.dll" };
		case GameState::Value::HaloReach: return { 0xA0, 6, 8, L"haloreach.dll" };
			// CER's dt is NOT established, so it is probed from the rate rather than assumed.
		case GameState::Value::HaloCER:   return { 0x98, 6, 0, L"HaloSimulation_tag_release.dll" };
		default:                          return { 0, 0, 0, nullptr };
		}
	}

	inline TlsTickrateLayout tlsLayoutForBuild(GameState::Value g)
	{
		TlsTickrateLayout L = tlsLayoutFor(g);
		if (g == GameState::Value::Halo3ODST && gOdstSeason5Slots.load()) L.slot = 0xD0;
		return L;
	}

	// _tls_index, read from the module's PE TLS DIRECTORY rather than a hardcoded RVA. This is the
	// loader's own contract, so it cannot be moved by a game update the way a signature site can -
	// exactly the reasoning HCEAnchors uses for the same field on Campaign Evolved.
	inline bool tlsIndexOf(HMODULE mod, uint32_t& outIndex)
	{
		if (!mod) return false;
		__try
		{
			auto base = (const uint8_t*)mod;
			auto dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
			if (!dir.VirtualAddress) return false;
			auto tls = (const IMAGE_TLS_DIRECTORY64*)(base + dir.VirtualAddress);
			if (!tls->AddressOfIndex) return false;
			outIndex = *(const uint32_t*)(uintptr_t)tls->AddressOfIndex;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// dt must satisfy dt == 1/rate; that relationship is what identifies the field. Used for CER,
	// whose dt offset was never established, and as a self-check everywhere else.
	inline bool dtMatches(uintptr_t gtg, uint32_t off, int16_t rate)
	{
		if (rate <= 0) return false;
		__try
		{
			const float f = *(const float*)(gtg + off);
			const float want = 1.0f / (float)rate;
			return f > 0.0f && fabsf(f - want) <= want * 1e-3f;   // RELATIVE: 1e-6 absolute is meaningless at high rates
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline bool probeDt(uintptr_t gtg, int16_t rate, uint32_t& outOff)
	{
		for (uint32_t o = 0; o <= 0x30; o += 4)
			if (dtMatches(gtg, o, rate)) { outOff = o; return true; }
		return false;
	}

	// A candidate is only accepted when the struct is INITIALISED, the rate is sane, and dt is exactly
	// its reciprocal. That triple is what tells a real game-time-globals apart from whatever else the
	// slot might hold after a game update - so a stale slot number fails safe instead of corrupting
	// the clock.
	inline bool validateGtg(uintptr_t gtg, const TlsTickrateLayout& L, int16_t& outRate, uint32_t& outDt)
	{
		if (!gtg || gtg < 0x10000) return false;
		__try
		{
			if (*(const uint8_t*)gtg != 1) return false;                 // not initialised
			const int16_t rate = *(const int16_t*)(gtg + L.rateOff);
			if (rate < 1 || rate > 32766) return false;   // == the input validator's own ceiling
			outRate = rate;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }

		if (L.dtOff && dtMatches(gtg, L.dtOff, outRate)) { outDt = L.dtOff; return true; }
		return probeDt(gtg, outRate, outDt);                             // CER, or a layout that moved
	}

	// ⚠ POD-ONLY, and deliberately so: a function containing __try cannot also require C++ object
	// unwinding (C2712). Every guarded read/write is isolated in one of these so the callers are free
	// to use vectors and std::format.
	inline uintptr_t gtgFromTeb(const void* teb, uint32_t tlsIndex, uint32_t slot)
	{
		__try
		{
			auto tlsArray = *(uintptr_t* const*)((const uint8_t*)teb + 0x58);   // ThreadLocalStoragePointer
			if (!tlsArray) return 0;
			const uintptr_t block = tlsArray[tlsIndex];
			if (!block) return 0;
			return *(const uintptr_t*)(block + slot);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	inline bool writeGtgFields(uintptr_t gtg, uint32_t rateOff, int16_t rate, uint32_t dtOff, float dt)
	{
		__try
		{
			*(int16_t*)(gtg + rateOff) = rate;
			*(float*)(gtg + dtOff) = dt;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Every thread's TLS block for this module, filtered to the ones actually holding a valid
	// game-time-globals. ⚠ NOT just the first: the render side binds this block too, and writing only
	// one thread leaves the others running at the old rate.
	inline void collectGtgPointers(const TlsTickrateLayout& L, std::vector<uintptr_t>& out)
	{
		out.clear();
		if (!L.module) return;
		HMODULE mod = GetModuleHandleW(L.module);
		uint32_t tlsIndex = 0;
		if (!tlsIndexOf(mod, tlsIndex)) return;

		using NtQIT = NTSTATUS(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
		static NtQIT ntQueryThread = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
		if (!ntQueryThread) return;

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return;
		THREADENTRY32 te{}; te.dwSize = sizeof(te);
		const DWORD pid = GetCurrentProcessId();
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!th) continue;

				struct { PVOID ExitStatus; PVOID TebBaseAddress; PVOID p1, p2, p3, p4; } tbi{};
				if (ntQueryThread(th, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBaseAddress)
				{
					const uintptr_t gtg = gtgFromTeb(tbi.TebBaseAddress, tlsIndex, L.slot);
					if (gtg && std::find(out.begin(), out.end(), gtg) == out.end())
						out.push_back(gtg);
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
	}
} // namespace


template <GameState::Value gameT>
class MasterTickrateImpl : public IMasterTickrateImpl
{
private:
	GameState mGame;

	ScopedCallback<ActionEvent> mFlipCallback;
	// fires when the user commits a value in the "Custom Tickrate (Hz)" input
	ScopedCallback<eventpp::CallbackList<void(int&)>> mCustomRateChangedCallback;
	// fires when the user arms/disarms the "Master Tickrate" gate; disarming restores the stock tickrate
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mEnableChangedCallback;
	// re-apply the custom rate on level load (the game resets the timing struct each load)
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	// Season 7 Physics toggled while a custom rate owns the collision scalar (Halo 2): re-pick the scalar (2026-11-06)
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mSeason7ChangedCallback;
	// runs each game tick on the GAME thread; performs the deferred collision rebuild at a safe point
	std::unique_ptr<ScopedCallback<GameTickEvent>> mGameTickCallback;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<GameTickEventHook> gameTickEventHookWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> masterTickratePointer;   // int16 @ timingStruct + 0x02
	std::shared_ptr<MultilevelPointer> masterTickrateDtPointer; // float @ timingStruct + 0x04

	TickrateScalarPatcher mScalar;
	std::atomic<bool> mPendingRebuild{ false };

	// The engine's stock collision scalar is 30.0 (flt_180C32AA0). Every tick change forces a live rebuild of the existing
	// collision shapes - also when the scalar itself does not change (flipping to 30 Hz, whose scalar == stock), so
	// shapes baked by something else (Season 7 Physics' 60) never outlive a switch. The rebuild only ever runs from
	// onGameTick while the game is fully in play (never mid-transition), under the SEH guard.
	static constexpr float kStockScalar = 30.0f;
	float mLastAppliedScalar = kStockScalar; // scalar the live collision shapes were last built with (stock at launch)

	// --- game functions/data we call to force a live collision rebuild (halo2.dll v1.3528 RVAs) ---
	using RecreateCollisionFn = uint32_t(__fastcall*)(uint16_t objectIndex);   // sub_180705B20 (self-no-ops if none)
	using GetObjectFn         = uintptr_t(__fastcall*)(uintptr_t headerEntry); // sub_1808D7000 (returns 0 if empty)
	static constexpr uint32_t  kRecreateCollisionRVA = 0x705B20;
	static constexpr uint32_t  kGetObjectRVA         = 0x8D7000;
	static constexpr uint8_t   kRecreateProbe[6] = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x20 };  // push rsi; sub rsp,20h
	static constexpr uint8_t   kGetObjectProbe[6] = { 0x8B, 0x51, 0x08, 0x48, 0x8B, 0x0D }; // mov edx,[rcx+8];mov rcx,[rip]
	static constexpr uint32_t  kObjectHeaderPtrRVA = 0x18B7398;  // -> qword_1818B7398 (object header table pointer)
	static constexpr uintptr_t kHeaderDataOffset   = 0x48;       // *(header+0x48) = offset from header to first entry
	static constexpr uintptr_t kHeaderCountOffset  = 0x40;       // *(header+0x40) = used/high-water object count
	static constexpr uint32_t  kHeaderStride       = 12;         // 12-byte header entries
	static constexpr uint32_t  kMaxObjects         = 2048;       // MAXIMUM_OBJECTS: array capacity + iteration clamp
	static constexpr uintptr_t kHavokComponentOffset = 0xB4;     // obj+0xB4 = collision body index (0xFFFFFFFF = none)

	// Write the tickrate int16 AND the matching seconds-per-tick (dt = 1/rate) together. dt = 1/rate is what keeps
	// 1x game speed (rate ticks/sec × 1/rate sec/tick = 1 sim-sec per real-sec). Caller must hold the game lock.
	void writeRate(int16_t newRate)
	{
		float newDt = 1.0f / (float)newRate;   // non-const: MultilevelPointer::writeData takes T*

		if constexpr (gameT == GameState::Value::Halo2)
		{
			if (!masterTickratePointer->writeData(&newRate))
				throw HCMRuntimeException(std::format("Failed to write master tickrate: {}", MultilevelPointer::GetLastError()));
			if (!masterTickrateDtPointer->writeData(&newDt))
				throw HCMRuntimeException(std::format("Failed to write tickrate dt: {}", MultilevelPointer::GetLastError()));
			return;
		}
		else
		{
			const auto L = tlsLayoutForBuild(gameT);
			std::vector<uintptr_t> targets;
			collectGtgPointers(L, targets);

			int written = 0;
			for (uintptr_t gtg : targets)
			{
				int16_t curRate = 0; uint32_t dtOff = 0;
				if (!validateGtg(gtg, L, curRate, dtOff)) continue;   // not a real timing struct - skip it
				if (writeGtgFields(gtg, L.rateOff, newRate, dtOff, newDt)) ++written;
			}

			if (!written)
				throw HCMRuntimeException(
					"Could not find the game's timing struct. Load a level first; if it is loaded, this "
					"build of the game has moved it and Master Tickrate refuses rather than writing to "
					"the wrong place.");
		}
	}

	// The rate currently in effect, or 0 if it cannot be read.
	int16_t readRate()
	{
		if constexpr (gameT == GameState::Value::Halo2)
		{
			int16_t r = 0;
			return masterTickratePointer->readData(&r) ? r : (int16_t)0;
		}
		else
		{
			const auto L = tlsLayoutForBuild(gameT);
			std::vector<uintptr_t> targets;
			collectGtgPointers(L, targets);
			for (uintptr_t gtg : targets)
			{
				int16_t r = 0; uint32_t dtOff = 0;
				if (validateGtg(gtg, L, r, dtOff)) return r;
			}
			return 0;
		}
	}

	// Reflect `rate` in the "Custom Tickrate (Hz)" box so it always shows the real chosen rate (e.g. after the 30/60
	// flip, or after a level load re-reads the game). Sets BOTH the committed value and the display copy directly -
	// NOT via UpdateValueWithInput - so it updates the box without re-firing onSetCustomRate.
	void setBox(int rate)
	{
		if (auto settings = settingsWeak.lock())
		{
			settings->customTickrate->GetValue() = rate;
			settings->customTickrate->GetValueDisplay() = rate;
		}
	}

	// Point the collision scalar at `rate` and take precedence over Season7Physics. May defer (no-op) if halo2.dll
	// isn't loaded yet; throws only on a build mismatch. Returns true only if the scalar actually CHANGED from the
	// value the current collision shapes were built with (i.e. a live rebuild is worth doing).
	bool applyScalarForRate(int rate)
	{
		// ⚠ HALO 2 ONLY - user-confirmed: it is the only game with a collision tickrate scalar. On the other
		// wired games the field write IS the whole job, and claiming customTickrateScalarActive here would
		// make Season7Physics (a Halo 2 cheat) stand down because of something the user did in Halo 3.
		if constexpr (gameT != GameState::Value::Halo2) return false;
		else {
		if (auto settings = settingsWeak.lock())
			settings->customTickrateScalarActive = true; // custom scalar owns 0x70DBFA now; Season7Physics stands down
		if (!mScalar.apply()) return false; // module not loaded yet
		float newScalar = scalarFor(rate);
		bool changed = (newScalar != mLastAppliedScalar);
		mScalar.setScalar(newScalar);
		mLastAppliedScalar = newScalar;
		return changed;
		}
	}

	// The arming checkbox is gone: the 60/30 button and the value box always apply. Until a rate is picked the game runs
	// at its own rate (60) and the box just mirrors it. The last rate picked for THIS game (each game has its own
	// instance) is re-applied on every later level load and mission restart this session (onMCCStateChanged). Kept here,
	// not read back from the box: the box is shared by every game, and another game's load mirrors its own rate into it.
	int mUserRate = 0;   // 0 = none picked yet
	bool isArmed() { return true; }

	// Arm/disarm gate. Arming just reveals the controls (nothing applies until the user picks a rate). Disarming
	// returns the game to its stock 60 Hz tickrate and releases the collision scalar (so Season 7 physics can reclaim
	// it), and stops the level-load re-arm.
	void onEnableChanged(bool& enabled)
	{
		if (enabled) return;
		try
		{
			if (auto settings = settingsWeak.lock()) settings->customTickrateScalarActive = false;
			if (mScalar.isApplied()) mScalar.revert();
			auto mccStateHook = mccStateHookWeak.lock();
			if (mccStateHook && mccStateHook->isGameCurrentlyPlaying(mGame))
				writeRate((int16_t)60); // stock sim rate for every wired game (Halo 1 CE, the only 30-tick title, is not wired)
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "MasterTickrate disarm skipped: " << ex.what(); }
	}

	// The collision scalar for a custom rate. Stock MCC multiplies by a constant 30.0 whatever the rate (the 30 Hz Xbox value,
	// wrong at 60 Hz); Season 7 Physics is the scalar matching the tickrate (60.0 at 60 Hz). A custom rate used to ALWAYS take
	// the matching value, so once a rate was picked the Season 7 toggle did nothing for the rest of the session (user,
	// 2026-11-06: "can't get Season 7 Physics to work at all"). Now the toggle decides: on = the rate, off = stock 30.
	float scalarFor(int rate)
	{
		auto settings = settingsWeak.lock();
		const bool season7 = settings && settings->season7PhysicsToggle->GetValue();
		return season7 ? (float)rate : 30.0f;
	}

	void onSeason7Changed(bool& on)
	{
		if constexpr (gameT != GameState::Value::Halo2) return;
		else {
		if (mUserRate <= 0 || !mScalar.isApplied() || !mScalar.stillLoaded()) return;   // no custom rate owns the scalar: Season7Physics handles it
		const float s = scalarFor(mUserRate);
		if (s == mLastAppliedScalar) return;
		mScalar.setScalar(s);
		mLastAppliedScalar = s;
		mPendingRebuild.store(true);   // rebuild the existing collision on the next in-play tick
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(std::format("Season 7 Physics {} at {} Hz (rebuilding collision).", on ? "on" : "off", mUserRate));
		}
		catch (HCMRuntimeException&) {}
		}
	}

	// user typed an arbitrary rate: set tickrate+dt, match the collision scalar, and queue a live rebuild.
	void onSetCustomRate(int& newRate)
	{
		PLOG_DEBUG << "MasterTickrate onSetCustomRate: " << newRate;
		if (!isArmed()) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			// ⚠ SIX instances of this class now share ONE customTickrate/flip event. Without this gate a
			// single keystroke prints five messages - one real, four from games that aren't even running.
			if (mccStateHook->getCurrentMCCState().currentGameState != mGame) return;
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				messagesGUI->addMessage("Load a level first to change the tickrate.");
				return;
			}

			int16_t rate16 = (int16_t)newRate; // validator restricts to 3..32766 (fits the signed int16 field; 1-2 crash)
			writeRate(rate16);
			mUserRate = rate16;   // re-applied on every later level load / restart of this game
			// every tick change queues a live collision rebuild (Halo 2: the only game with the scalar). It runs from
			// onGameTick only while the level is fully in play, under the SEH guard - never mid-transition.
			applyScalarForRate(newRate);
			mPendingRebuild.store(true); // every tick change rebuilds the existing shapes, on the next in-play game tick (safe point)

			messagesGUI->addMessage(std::format("Master tickrate set to {} Hz{}.", (int)rate16, (gameT == GameState::Value::Halo2) ? " (rebuilding collision)" : ""));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// The game resets the timing struct (int16/dt) on every level load. If a custom rate is active, restore it and
	// re-assert the scalar, and (when this changes the rate) queue a collision rebuild for the next in-play tick.
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState != PlayState::Ingame) return;

		// ⚠ ORDER MATTERS. customTickrate is the box AND the stored value, so reflecting the game's
		// post-reset rate into it BEFORE the re-apply would overwrite the rate we are about to re-apply -
		// the re-apply would then dutifully write back the 60 the game just reset to.
		auto settings = settingsWeak.lock();
		if (!settings || mUserRate <= 0)
		{
			// no rate picked for this game yet: it keeps its own (60); just show the real tickrate (it resets on every load)
			const int16_t real = readRate(); if (real > 0) setBox(real);
			return;
		}
		// ⚠ NOT customTickrateScalarActive: that flag is set only inside the Halo-2-only scalar branch,
		// so gating on it made this re-apply dead code for Halo 3, ODST, Halo 4, Reach and CER - the rate
		// silently reverted to 60 on every map load while the box still showed the user's value.
		try
		{
			// the last rate picked for this game, not the shared box (another game's load may have mirrored its own rate into it)
			const int rate = mUserRate;
			const int16_t before = readRate();
			writeRate((int16_t)rate);
			applyScalarForRate(rate);
			// a tick change (every load comes up at 60): rebuild collision too, on the next in-play game tick - the
			// rebuild waits for the level to be fully in play (onGameTick), so it never runs mid-transition
			if (before != (int16_t)rate) mPendingRebuild.store(true);
			setBox(rate); // armed: we just re-applied the chosen rate, so show it (overrides the reset value above)
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "MasterTickrate re-arm skipped: " << ex.what(); }
	}

	// Raw object-table walk + engine collision rebuild, isolated in its own function with only POD locals so it can be
	// SEH-wrapped: if the object header table is mid-rebuild during a level transition, a fault here is caught and the
	// rebuild abandoned instead of crashing the game. Returns objects rebuilt, or -1 if a structured exception fired.
	static int rebuildAllCollisionSEH(uintptr_t base, GetObjectFn getObject, RecreateCollisionFn recreate)
	{
		__try
		{
			uintptr_t headerPtr = *(uintptr_t*)(base + kObjectHeaderPtrRVA); // qword_1818B7398 (object header table)
			if (!headerPtr || IsBadReadPtr((void*)headerPtr, 0x50)) return 0; // table not up / not readable
			uintptr_t dataRegion = headerPtr + *(uintptr_t*)(headerPtr + kHeaderDataOffset); // first header entry
			uint32_t count = *(uint32_t*)(headerPtr + kHeaderCountOffset);
			if (count > kMaxObjects) count = kMaxObjects; // clamp to the array's real capacity (never read past it)

			int rebuilt = 0;
			for (uint32_t i = 0; i < count; ++i)
			{
				uintptr_t objData = getObject(dataRegion + (uintptr_t)kHeaderStride * i); // 0 for empty/deleted slots
				if (!objData) continue;
				if (*(uint32_t*)(objData + kHavokComponentOffset) == 0xFFFFFFFF) continue; // no collision body
				recreate((uint16_t)i);
				++rebuilt;
			}
			return rebuilt;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// 1 = both engine functions match build 1.3528, 0 = mismatch, -1 = the image faulted on read (POD-only, SEH-wrapped)
	static int probesMatchSEH(uintptr_t base)
	{
		__try
		{
			return (memcmp((void*)(base + kRecreateCollisionRVA), kRecreateProbe, sizeof(kRecreateProbe)) == 0
				&& memcmp((void*)(base + kGetObjectRVA), kGetObjectProbe, sizeof(kGetObjectProbe)) == 0) ? 1 : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	// runs on the GAME thread each tick; when a rebuild is queued, recreate collision for every live object.
	void onGameTick(uint32_t)
	{
		if (!mPendingRebuild.load()) return;
		if (!mScalar.isApplied()) { mPendingRebuild.store(false); return; }   // no scalar patch (not Halo 2 / not loaded): nothing to rebuild
		// NEVER walk the object table unless a game is fully in-play. During a level transition the table can be
		// half-built, and calling the engine's collision teardown/rebuild against it can crash. This (plus the SEH
		// guard below) is the core stability fix for the level-switch crash. The request STAYS queued until the level
		// is in play (a picked rate re-applied as a level loads rebuilds once it is playable).
		if (auto h = mccStateHookWeak.lock(); !h || !h->isGameCurrentlyPlaying(mGame)) return;
		// the patched halo2.dll must still be THE loaded one: ending a game and starting a new map can reload it, and
		// reading the cached base then faulted here (crash 2026-10-30 13:20, MasterTickrate.cpp onGameTick). The
		// request stays queued; the level-load re-arm re-patches the new image and this tick runs after it.
		if (!mScalar.stillLoaded()) { mScalar.forgetStale(); return; }
		mPendingRebuild.store(false);
		uintptr_t base = mScalar.moduleBase();
		if (!base) return;

		// build gate: the two functions we call must match build 1.3528 before we jump into them (SEH-guarded read)
		const int probe = probesMatchSEH(base);
		if (probe <= 0)
		{
			PLOG_ERROR << (probe < 0 ? "MasterTickrate: halo2.dll not readable; skipping collision rebuild."
			                         : "MasterTickrate: object-fn prologue mismatch; skipping collision rebuild.");
			return;
		}

		auto getObject = reinterpret_cast<GetObjectFn>(base + kGetObjectRVA);
		auto recreate  = reinterpret_cast<RecreateCollisionFn>(base + kRecreateCollisionRVA);
		int rebuilt = rebuildAllCollisionSEH(base, getObject, recreate);
		if (rebuilt < 0)
			PLOG_ERROR << "MasterTickrate: collision rebuild faulted (object table mid-transition?); skipped safely.";
		else
			PLOG_DEBUG << "MasterTickrate rebuilt collision for " << rebuilt << " objects";
	}

	void onFlip()
	{
		PLOG_DEBUG << "MasterTickrate onFlip";
		if (!isArmed()) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			// ⚠ SIX instances of this class now share ONE customTickrate/flip event. Without this gate a
			// single keystroke prints five messages - one real, four from games that aren't even running.
			if (mccStateHook->getCurrentMCCState().currentGameState != mGame) return;
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				messagesGUI->addMessage("Load a level first to change the tickrate.");
				return;
			}

			const int16_t currentRate = readRate();
			if (currentRate <= 0)
				throw HCMRuntimeException("Could not read the current tickrate - load a level first.");

			// flip: anything at (about) 60 -> 30, otherwise -> 60. Like the custom input, the button also matches
			// the collision scalar and rebuilds so 30/60 physics is consistent (not just game speed).
			int16_t newRate = (currentRate >= 45) ? (int16_t)30 : (int16_t)60;
			writeRate(newRate);
			mUserRate = newRate;   // re-applied on every later level load / restart of this game
			// every tick change queues a live collision rebuild, also to 30 (== the stock scalar). The level-switch crash
			// this once avoided is handled where the rebuild runs: onGameTick only while fully in play, SEH-guarded.
			applyScalarForRate(newRate);
			mPendingRebuild.store(true); // every tick change rebuilds the existing shapes, on the next in-play game tick (safe point)
			setBox(newRate); // update the "Custom Tickrate (Hz)" box so it shows the flipped rate (30/60), not the old value

			messagesGUI->addMessage(std::format("Master tickrate set to {} Hz{}.", newRate, (gameT == GameState::Value::Halo2) ? " (rebuilding collision)" : ""));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	MasterTickrateImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mFlipCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->masterTickrateFlipEvent, [this]() { onFlip(); }),
		mCustomRateChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->customTickrate->valueChangedEvent, [this](int& n) { onSetCustomRate(n); }),
		mEnableChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->masterTickrateEnabled->valueChangedEvent, [this](bool& e) { onEnableChanged(e); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		mSeason7ChangedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->season7PhysicsToggle->valueChangedEvent, [this](bool& on) { onSeason7Changed(on); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		gameTickEventHookWeak(resolveDependentCheat(GameTickEventHook)),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		if constexpr (gameT == GameState::Value::Halo3ODST)
			gOdstSeason5Slots.store(isOdst2094(dicon, game));

		// ⚠ Pointer data exists for HALO 2 ONLY - every other game locates the struct through TLS at
		// write time, so resolving these would throw and take the whole cheat down with it.
		if constexpr (gameT == GameState::Value::Halo2)
		{
			auto ptr = dicon.Resolve<PointerDataStore>().lock();
			masterTickratePointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrate), mGame);
			masterTickrateDtPointer = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(masterTickrateDt), mGame);
		}

		// ⚠ HALO 2 ONLY, for two reasons. The callback exists solely to drive the collision-scalar
		// rebuild, which no other game has - and GameTickEventHook's switch has NO HaloCER case and no
		// default, so on CER its pimpl is null and getGameTickEvent() dereferences it. Subscribing
		// elsewhere is useless; on CER it is a crash on construction.
		if constexpr (gameT == GameState::Value::Halo2)
		{
			if (auto gth = gameTickEventHookWeak.lock())
				mGameTickCallback = gth->getGameTickEvent()->subscribe([this](uint32_t t) { onGameTick(t); });
		}
	}

	~MasterTickrateImpl()
	{
		try { if (mScalar.isApplied()) mScalar.revert(); }
		catch (...) {}
		if (auto settings = settingsWeak.lock()) settings->customTickrateScalarActive = false;
	}
};


MasterTickrate::MasterTickrate(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	case GameState::Value::Halo3:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo3>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo3ODST>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo4:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::Halo4>>(gameImpl, dicon);
		break;
	case GameState::Value::HaloReach:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::HaloReach>>(gameImpl, dicon);
		break;
	case GameState::Value::HaloCER:
		pimpl = std::make_unique<MasterTickrateImpl<GameState::Value::HaloCER>>(gameImpl, dicon);
		break;

	// ⚠ Halo 1 is absent ON PURPOSE, not by omission. Its timing struct carries no rate/dt pair at
	// all - CE's 30 Hz is baked into the engine rather than stored - so there is nothing here to
	// write. See the layout table above.
	default:
		throw HCMInitException("MasterTickrate not impl for this game");
	}
}

MasterTickrate::~MasterTickrate()
{
	PLOG_DEBUG << "~" << getName();
}
