#include "pch.h"
#include "GetCurrentRNG.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include <TlHelp32.h>
#include <vector>
#include <algorithm>
#include <chrono>

// ================================================================================================================
// THE SCENARIO RNG SEED.
//
// Blam's RNG is a linear congruential generator using the Numerical Recipes "ranqd1" constants:
//
//     seed = seed * 0x19660D + 0x3C6EF35F
//
// ⚠⚠ THERE ARE **TWO** SEEDS IN EVERY BLAM GAME, AND ONLY ONE OF THEM IS THE ONE ANYONE WANTS.
// Halo 2 is the Rosetta stone: its "random math" game-state block is 8 bytes and the initialiser fills the
// two halves differently -
//
//     block+0x0  <- a CONSTANT      -> the DETERMINISTIC seed. Part of the game state, saved into and
//                                      restored from checkpoints, and therefore the thing that decides how a
//                                      scenario actually plays out - weapon drops, AI behaviour, the lot.
//     block+0x4  <- QPC ^ thread id -> a LOCAL, non-deterministic seed. Reseeded from the clock, never
//                                      persisted. It churns constantly and means nothing across runs.
//
// **This feature displays the DETERMINISTIC one.** A viewer showing the local seed would look identical -
// a number that changes as you play - while being worthless for the purpose, so the distinction is worth
// far more than the code it costs.
//
// WHERE EACH GAME KEEPS IT
// ------------------------
// Halo 1 keeps both as plain module globals, so it resolves through InternalPointerData.xml like anything
// else (currentRNG -> halo1.dll+0x2EA74F8, the const-initialised half; +0x2EA74F4 is its local sibling -
// that adjacency is the "second half of 8 byte stretch" the XML comment mentions).
//
// The H3-era engine (Halo 3, ODST, Reach, Halo 4) does NOT. There the deterministic seed lives inside the
// game-state block, reached through THREAD-LOCAL STORAGE, and MultilevelPointer has no TLS specialisation -
// it only has ExeOffset / BaseOffset / ModuleOffset / Resolved. So those games cannot be expressed in the
// XML at all and are walked here instead.
//
// THE CHAIN, read out of Reach's own code rather than guessed (rva 0x0832 7A, one of 180 such sites):
//
//     mov  ecx, [rip -> _tls_index]      ; from the PE TLS directory, NOT a hardcoded rva
//     mov  rax, gs:[0x58]                ; TEB -> ThreadLocalStoragePointer
//     mov  r8d, 0x658                    ; <-- the slot
//     mov  rax, [rax + rcx*8]            ; -> this module's TLS block
//     mov  rcx, [r8 + rax]               ; -> *(block + 0x658)   the game-state random-math block
//     imul r8d, dword ptr [rcx], 0x19660D
//     add  r8d, 0x3C6EF35F
//     mov  dword ptr [rcx], r8d          ; seed is at +0x00 of that block, read-modify-written in place
//
// ⚠ NOTE THE ADDRESSING: the slot goes into a REGISTER and the load is `[r8 + rax]` with NO displacement.
// A census looking for `[reg + disp]` operands therefore finds NOTHING and concludes the slot does not
// exist - which is exactly what happened while deriving this. Counting could not settle it; disassembling
// one site could. Do not "verify" this offset by pattern-counting.
//
// ⚠ The TLS helpers below are a deliberate second copy of the ones in MasterTickrate.cpp, which walks the
// same structure for a different slot. They are ~40 lines, MasterTickrate is a working and subtle feature,
// and this file is its only other consumer - so a shared header would mean editing a proven file for
// tidiness alone. If a THIRD consumer appears, extract them then.
// ================================================================================================================
namespace
{
	struct TlsSeedLayout
	{
		const wchar_t* module;
		uint32_t slot;        // byte offset into the module's TLS block
		uint32_t seedOffset;  // byte offset of the seed within the pointed-to block
	};

	// Games whose deterministic seed is behind TLS rather than a module global. A game absent from here
	// resolves through pointer data instead (Halo 1), and a game in neither place simply has no viewer.
	// ⚠ Only Reach is shipped. Halo 3 / ODST / Halo 4 use the same shape but their slots are NOT assumed -
	// each needs deriving and verifying against its own binary before being added here.
	constexpr bool tlsSeedLayoutFor(GameState::Value g, TlsSeedLayout& out)
	{
		switch (g)
		{
		case GameState::Value::HaloReach: out = { L"haloreach.dll", 0x658, 0x00 }; return true;
		default:                          return false;
		}
	}

	// ⚠ POD-ONLY, and deliberately so: a function containing __try cannot also require C++ object
	// unwinding (C2712). Same constraint MasterTickrate.cpp and HCEGetPlayerState.cpp both document.

	// _tls_index read from the module's PE TLS DIRECTORY rather than a hardcoded RVA. That is the loader's
	// own contract, so unlike a byte signature it cannot be moved by a game update.
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

	// *(block + slot) for one thread. Zero means this thread has no block for the module, or the slot is
	// not populated on it - both are normal and not errors.
	inline uintptr_t slotPointerFromTeb(const void* teb, uint32_t tlsIndex, uint32_t slot)
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

	inline bool readDwordAt(uintptr_t address, DWORD& out)
	{
		if (!address || address < 0x10000) return false;
		__try { out = *(const DWORD*)address; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// ⚠⚠ THIS IS THE EXPENSIVE ONE AND IT MUST NOT RUN PER FRAME.
	// CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) walks every thread on the SYSTEM, and this then does an
	// OpenThread + NtQueryInformationThread for each one in our process. That is milliseconds. The 2D info
	// panel calls its providers from RenderEvent, so calling this from there costs a frame - it did exactly
	// that on first test, dropping Reach from its cap to sub-30 fps.
	//
	// The caller therefore uses it ONCE to learn WHICH THREAD carries the block, then re-derives through
	// that thread's TEB every frame instead, which is three dereferences. See the cache in the impl below.
	//
	// TLS is per THREAD, so this has to ask other threads: the thread calling it is HCM's own, which has no
	// game TLS block at all.
	bool findSeedThread(const TlsSeedLayout& layout, uintptr_t& outTeb, uint32_t& outTlsIndex, DWORD& outSeed)
	{
		outTeb = 0; outTlsIndex = 0; outSeed = 0;

		HMODULE mod = GetModuleHandleW(layout.module);
		uint32_t tlsIndex = 0;
		if (!tlsIndexOf(mod, tlsIndex)) return false;

		using NtQIT = NTSTATUS(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
		static NtQIT ntQueryThread = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
		if (!ntQueryThread) return false;

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;

		bool found = false;
		std::vector<uintptr_t> seenBlocks;
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
					const uintptr_t block = slotPointerFromTeb(tbi.TebBaseAddress, tlsIndex, layout.slot);
					DWORD value = 0;
					if (block && readDwordAt(block + layout.seedOffset, value))
					{
						if (!found)
						{
							found = true;
							outTeb = (uintptr_t)tbi.TebBaseAddress;
							outTlsIndex = tlsIndex;
							outSeed = value;
						}
						else if (std::find(seenBlocks.begin(), seenBlocks.end(), block) == seenBlocks.end()
							&& value != outSeed)
						{
							// Every thread holding the slot should point at the SAME game-state block. If
							// that stops being true the slot is probably wrong for this build - say so once
							// rather than silently picking a winner.
							PLOG_WARNING << "GetCurrentRNG: threads disagree on the seed (" << outSeed
								<< " vs " << value << "). Using the first. If this is persistent the TLS "
								"slot may be wrong for this build of the game.";
						}
						seenBlocks.push_back(block);
					}
				}
				CloseHandle(th);
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return found;
	}
}

class GetCurrentRNG::GetCurrentRNGImpl
{
private:
	//data
	GameState mGame;
	std::shared_ptr<MultilevelPointer> currentRNG;   // module-global games only (Halo 1)
	TlsSeedLayout mTlsLayout{};
	bool mUsesTls = false;

	// ⚠ THE WHOLE POINT OF THIS CACHE IS THAT THE THREAD SCAN IS A PER-FRAME COST WE CANNOT PAY.
	// We remember WHICH THREAD carries the game's TLS block, not the block address itself, and walk
	// TEB -> TLS array -> block -> slot again on every call. That re-derivation is three dereferences, so
	// it is free, and because it is a re-derivation rather than a remembered pointer it CANNOT go stale:
	// when the scenario tears down, the slot reads back null and we fall through to a rescan.
	//
	// Caching the resolved block pointer instead would be the obvious optimisation and would be wrong -
	// after a level transition that address can be freed and reused, and a value read out of it would look
	// entirely plausible. Re-deriving sidesteps that class of bug rather than trying to detect it.
	uintptr_t mCachedTeb = 0;
	uint32_t mCachedTlsIndex = 0;

	// ⚠ AND THE CACHE ALONE IS NOT ENOUGH. At the main menu, during a load, and between scenarios there is
	// no block to find, so the cache stays empty and EVERY frame would reach the thread scan - the same
	// per-frame cost, just moved to a different situation. Failing to resolve is the NORMAL state for a
	// per-frame feature (see the note in HCEGetCameraData about transient throws), so the retry has to be
	// rate limited rather than treated as exceptional. Four attempts a second is imperceptible as a cost
	// and still picks the seed up within a frame or two of a scenario starting.
	std::chrono::steady_clock::time_point mLastScanAttempt{};
	static constexpr std::chrono::milliseconds kRescanInterval{ 250 };

	// ---- the level-load latch. See the contract on getLevelLoadRNG in the header. ----
	bool mLoadSeedLatched = false;
	DWORD mLoadSeed = 0;
	uint32_t mLoadSeedTick = 0;
	uint32_t mLastTick = UINT32_MAX;   // sentinel: no tick seen yet, so the first one cannot look like a restart

	// The read path without the exception, so the latch can try and quietly fail on a tick where the level
	// is not up yet. getCurrentRNG() is this plus a throw.
	bool tryReadSeed(DWORD& out)
	{
		if (!mUsesTls)
			return currentRNG && currentRNG->readData(&out);

		if (mCachedTeb)
		{
			const uintptr_t block = slotPointerFromTeb((const void*)mCachedTeb, mCachedTlsIndex, mTlsLayout.slot);
			if (block && readDwordAt(block + mTlsLayout.seedOffset, out))
				return true;
			mCachedTeb = 0;
			mCachedTlsIndex = 0;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now - mLastScanAttempt < kRescanInterval) return false;
		mLastScanAttempt = now;

		uintptr_t teb = 0; uint32_t tlsIndex = 0; DWORD seed = 0;
		if (!findSeedThread(mTlsLayout, teb, tlsIndex, seed)) return false;

		mCachedTeb = teb;
		mCachedTlsIndex = tlsIndex;
		out = seed;
		return true;
	}

public:
	GetCurrentRNGImpl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		mUsesTls = tlsSeedLayoutFor(game, mTlsLayout);

		// Only resolve pointer data for the games that actually have an entry. Asking for one that does not
		// exist throws "pointerData was null", which would disable the viewer on every TLS game.
		if (!mUsesTls)
			currentRNG = dicon.Resolve<PointerDataStore>().lock()->getData<std::shared_ptr<MultilevelPointer>>(nameof(currentRNG), game);
	}

	DWORD getCurrentRNG()
	{
		DWORD seed = 0;
		if (!tryReadSeed(seed))
			throw HCMRuntimeException(mUsesTls
				? "Could not read the RNG seed. This is normal at the main menu or while loading - the game's "
				  "random-math block only exists once a scenario is running."
				: "Could not read currentRNG");
		return seed;
	}

	DWORD getLevelLoadRNG(uint32_t currentGameTick)
	{
		// ⚠ THE ONLY RE-ARM IS THE TICK COUNTER RETURNING TO ZERO, and it is compared against the PREVIOUS
		// tick rather than against the latched one. Testing `mLoadSeedTick != 0` instead would look right and
		// be wrong: having latched at tick 0 on one level, the next level's tick 0 would not re-arm and the
		// display would keep showing the previous level's seed forever.
		if (currentGameTick == 0 && mLastTick != 0)
			mLoadSeedLatched = false;
		mLastTick = currentGameTick;

		if (!mLoadSeedLatched)
		{
			DWORD seed = 0;
			if (tryReadSeed(seed))
			{
				mLoadSeedLatched = true;
				mLoadSeed = seed;
				mLoadSeedTick = currentGameTick;
				PLOG_DEBUG << "GetCurrentRNG: latched level-load seed " << seed << " at game tick " << currentGameTick;
			}
		}

		if (!mLoadSeedLatched)
			throw HCMRuntimeException("Waiting for the level to start before capturing its RNG seed.");

		return mLoadSeed;
	}

	uint32_t getLevelLoadRNGTick() { return mLoadSeedTick; }

	~GetCurrentRNGImpl()
	{
		PLOG_DEBUG << "~GetCurrentRNGImpl";
	}
};

GetCurrentRNG::GetCurrentRNG(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<GetCurrentRNG::GetCurrentRNGImpl>(game, dicon)) {}
GetCurrentRNG::~GetCurrentRNG() { PLOG_DEBUG << "~" << getName(); }
DWORD GetCurrentRNG::getCurrentRNG() { return pimpl->getCurrentRNG(); }
DWORD GetCurrentRNG::getLevelLoadRNG(uint32_t currentGameTick) { return pimpl->getLevelLoadRNG(currentGameTick); }
uint32_t GetCurrentRNG::getLevelLoadRNGTick() { return pimpl->getLevelLoadRNGTick(); }
