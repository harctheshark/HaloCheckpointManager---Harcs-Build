#include "pch.h"
#include "HCEGetPlayerState.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include <TlHelp32.h>

// ================================================================================================================
// Halo Campaign Evolved state resolution. Ported from HCM_Evolved (memory.py, action_sections/common.py).
//
// THE ONE THING THAT IS NOT A STRAIGHT PORT - which TEB we read.
//
// The reference tool is an EXTERNAL process, so "the game thread" was never ambiguous: it enumerated the target's
// threads, found the one whose win32 start address is simBase + game_thread_entrypoint, and read that thread's
// TEB. HCM runs INSIDE the process, on the D3D12 present thread and on its own hotkey threads. Those threads have
// their OWN TLS block for this module, and its slots contain either null or a completely different allocation.
// NtCurrentTeb()->ThreadLocalStoragePointer therefore produces plausible-looking pointers that are simply wrong.
// So we reproduce the thread walk even though we're in-process; only the final read is a plain dereference
// instead of ReadProcessMemory.
//
// Caching: the TEB is cached (the game thread is stable for a session, and CreateToolhelp32Snapshot is not cheap)
// but tlsArray/tlsBase are re-read on EVERY access. ntdll reallocates TEB.ThreadLocalStoragePointer in
// LdrpHandleTlsData whenever a module with TLS loads, so a cached tlsBase can silently go stale mid-session;
// the TEB pointer itself cannot. That is two extra loads, which is free next to what the callers then do.
//
// EVERY TLS-reached offset is a POINTER SLOT - deref it, then apply the second offset. The one exception is
// ai_enabled (tls + 0x40): the slot points straight at the boolean, there is no second offset.
// ================================================================================================================

namespace
{
	// ntdll, resolved by name: NtQueryInformationThread is not in any import library we link.
	// Named distinctly from the winternl.h declaration on purpose.
	typedef LONG(NTAPI* HceNtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

	constexpr ULONG kThreadBasicInformation = 0;
	constexpr ULONG kThreadQuerySetWin32StartAddress = 9;

	// Our own definition - winternl.h does not reliably expose this one.
	struct HceThreadBasicInformation
	{
		LONG      ExitStatus;
		PVOID     TebBaseAddress;
		ULONG_PTR UniqueProcessId;
		ULONG_PTR UniqueThreadId;
		ULONG_PTR AffinityMask;
		LONG      Priority;
		LONG      BasePriority;
	};

	// Same idea as the reference tool's D3DHook GameMemory::IsReadable: ask the memory manager rather than
	// poking the page. IsBadWritePtr in particular is not safe to use for this - it WRITES to test.
	bool regionAllows(uintptr_t address, size_t size, bool needWrite)
	{
		if (address == 0 || size == 0) return false;
		const uintptr_t end = address + size;
		if (end < address) return false;

		uintptr_t cursor = address;
		while (cursor < end)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery((void*)cursor, &mbi, sizeof(mbi)) == 0) return false;
			if (mbi.State != MEM_COMMIT) return false;
			if (mbi.Protect & PAGE_GUARD) return false;
			if (mbi.Protect == PAGE_NOACCESS) return false;

			constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
				| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
				| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

			if ((mbi.Protect & (needWrite ? writable : readable)) == 0) return false;

			const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (regionEnd <= cursor) return false;
			cursor = regionEnd;
		}
		return true;
	}

	// SEH only. No C++ objects with destructors may live in these two - MSVC rejects __try in a function that
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
}


bool HCEGetPlayerState::tryReadRaw(uintptr_t address, void* out, size_t size)
{
	// NOTE: deliberately NO regionAllows()/VirtualQuery precheck on the READ path.
	// sehCopy already makes a bad read harmless - catching the access violation IS the safety mechanism - so the
	// VirtualQuery was a kernel transition per read that bought nothing. It cost real frames: one position fetch
	// walks TEB -> tls index -> tls array -> tls base -> player datum -> object -> havok index -> physics root ->
	// table -> entry -> floats, and the info overlay did that for position AND velocity every refresh.
	// Writes KEEP the precheck (tryWriteRaw): scribbling on a wrong-but-committed page is not recoverable by SEH.
	if (address == 0 || size == 0) return false;
	return sehCopy(out, (const void*)address, size);
}

bool HCEGetPlayerState::tryWriteRaw(uintptr_t address, const void* in, size_t size)
{
	if (!regionAllows(address, size, true)) return false;
	return sehCopy((void*)address, in, size);
}


class HCEGetPlayerState::HCEGetPlayerStateImpl
{
private:
	GameState mGame;

	// XML data. Pure PointerDataStore lookups - the constructor deliberately touches no game memory, because
	// HaloSimulation_tag_release.dll is not guaranteed to be loaded when cheats are constructed
	// (see the SAFETY note in HCECheckpointDetours.cpp).
	std::shared_ptr<MultilevelPointer> mTlsIndex;             // ModuleOffset -> ADDRESS of the tls index int32
	std::shared_ptr<MultilevelPointer> mGameThreadEntrypoint; // ModuleOffset -> the thread start address to match
	std::shared_ptr<MultilevelPointer> mPhysicsTableSlot;     // ModuleOffset -> ADDRESS of the physics root pointer
	std::shared_ptr<MultilevelPointer> mCurrentLevel;         // ModuleOffset -> ADDRESS of the level name string
	std::shared_ptr<MultilevelPointer> mCurrentBSP;           // ModuleOffset -> ADDRESS of the bsp int32
	std::shared_ptr<MultilevelPointer> mTickCounterSlot;      // ModuleOffset -> ADDRESS of the tick counter POINTER

	int64_t mTebTlsArrayOffset = 0x58;
	int64_t mAiEnabledTlsOffset = 0x40;
	int64_t mSkullArrayTlsOffset = 0x60;
	int64_t mSkullArrayOffset = 0x1EBE0;
	int64_t mPlayerDatumTlsOffset = 0x30;
	int64_t mPlayerDatumOffset = 0x98;
	int64_t mObjectTableTlsOffset = 0x20;
	int64_t mObjectTableOffset = 0x50;
	int64_t mObjectTableStride = 0x18;
	int64_t mObjectTableEntryOffset = 0x10;
	int64_t mHavokIndexOffset = 0xCC;
	int64_t mPhysicsRootOffset = 0x50;
	int64_t mPhysicsEntryStride = 0xC0;
	int64_t mPhysicsEntryOffset = 0x30;
	int64_t mPhysicsEntryOffset2 = 0x40;
	int64_t mFreecamToggleTlsOffset = 0xB8;
	int64_t mFreecamToggleOffset = 0x9C8;
	int64_t mCameraEntriesTlsOffset = 0x148;
	int64_t mCameraEntryStride = 0x1AC;

	std::mutex mCacheMutex;
	uintptr_t mCachedTeb = 0;
	std::chrono::steady_clock::time_point mLastFailedWalk{};

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;

	static uintptr_t resolveOrThrow(const std::shared_ptr<MultilevelPointer>& mlp, const char* what)
	{
		if (!mlp) throw HCMRuntimeException(std::format("No HaloCER pointer data for {}", what));
		uintptr_t address = 0;
		if (!mlp->resolve(&address) || address == 0)
			throw HCMRuntimeException(std::format("Could not resolve HaloCER {}: {}", what, MultilevelPointer::GetLastError()));
		return address;
	}

	// Enumerate our own threads and take the first whose win32 start address is the sim's game thread entry.
	// Matches memory.py::_find_game_thread_teb, including "first match wins, no duplicate check".
	uintptr_t findGameThreadTeb() // throws HCMRuntimeException
	{
		const uintptr_t target = resolveOrThrow(mGameThreadEntrypoint, nameof(hceGameThreadEntrypoint));

		static HceNtQueryInformationThread_t queryThread = nullptr;
		if (!queryThread)
		{
			HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			if (ntdll) queryThread = (HceNtQueryInformationThread_t)GetProcAddress(ntdll, "NtQueryInformationThread");
			if (!queryThread) throw HCMRuntimeException("Could not resolve ntdll!NtQueryInformationThread");
		}

		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) throw HCMRuntimeException("Could not snapshot the process threads");

		const DWORD ourPid = GetCurrentProcessId();
		uintptr_t foundTeb = 0;

		THREADENTRY32 te{};
		te.dwSize = sizeof(te);
		if (Thread32First(snapshot, &te))
		{
			do
			{
				if (te.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID)) continue;
				if (te.th32OwnerProcessID != ourPid) continue;

				// THREAD_QUERY_INFORMATION first, then the limited right - same order as the reference tool.
				HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!thread) thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!thread) continue;

				PVOID startAddress = nullptr;
				if (queryThread(thread, kThreadQuerySetWin32StartAddress, &startAddress, sizeof(startAddress), nullptr) == 0
					&& (uintptr_t)startAddress == target)
				{
					HceThreadBasicInformation tbi{};
					if (queryThread(thread, kThreadBasicInformation, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBaseAddress)
						foundTeb = (uintptr_t)tbi.TebBaseAddress;
				}

				CloseHandle(thread);
				if (foundTeb) break;
			} while (Thread32Next(snapshot, &te));
		}

		CloseHandle(snapshot);

		if (!foundTeb) throw HCMRuntimeException("Could not find the Halo Campaign Evolved game thread (is a level loaded?)");
		return foundTeb;
	}

public:
	HCEGetPlayerStateImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>())
	{
		// explicit cast: GameState has operator==(GameState) AND operator Value(), so comparing a GameState
		// straight against a Value is ambiguous (C2666).
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEGetPlayerState only supports Halo Campaign Evolved");

		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		mTlsIndex = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTlsIndex), mGame);
		mGameThreadEntrypoint = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceGameThreadEntrypoint), mGame);
		mPhysicsTableSlot = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hcePhysicsTable), mGame);
		mCurrentLevel = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCurrentLevel), mGame);
		mCurrentBSP = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCurrentBSP), mGame);
		mTickCounterSlot = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceTickCounterSlot), mGame);

		// Structural offsets. Anything the reference tool hard-codes in python is here too, so the whole layout
		// is maintainable from one file rather than being scattered through C++.
#define hceOffset(member, name) member = *ptr->getData<std::shared_ptr<int64_t>>(nameof(name), mGame)
		hceOffset(mTebTlsArrayOffset, hceTebTlsArrayOffset);
		hceOffset(mAiEnabledTlsOffset, hceAiEnabledTlsOffset);
		hceOffset(mSkullArrayTlsOffset, hceSkullArrayTlsOffset);
		hceOffset(mSkullArrayOffset, hceSkullArrayOffset);
		hceOffset(mPlayerDatumTlsOffset, hcePlayerDatumTlsOffset);
		hceOffset(mPlayerDatumOffset, hcePlayerDatumOffset);
		hceOffset(mObjectTableTlsOffset, hceObjectTableTlsOffset);
		hceOffset(mObjectTableOffset, hceObjectTableOffset);
		hceOffset(mObjectTableStride, hceObjectTableStride);
		hceOffset(mObjectTableEntryOffset, hceObjectTableEntryOffset);
		hceOffset(mHavokIndexOffset, hceHavokIndexOffset);
		hceOffset(mPhysicsRootOffset, hcePhysicsRootOffset);
		hceOffset(mPhysicsEntryStride, hcePhysicsEntryStride);
		hceOffset(mPhysicsEntryOffset, hcePhysicsEntryOffset);
		hceOffset(mPhysicsEntryOffset2, hcePhysicsEntryOffset2);
		hceOffset(mFreecamToggleTlsOffset, hceFreecamToggleTlsOffset);
		hceOffset(mFreecamToggleOffset, hceFreecamToggleOffset);
		hceOffset(mCameraEntriesTlsOffset, hceCameraEntriesTlsOffset);
		hceOffset(mCameraEntryStride, hceCameraEntryStride);
#undef hceOffset
	}

	void invalidateCache()
	{
		std::scoped_lock lock(mCacheMutex);
		mCachedTeb = 0;
	}

	uintptr_t getSimModuleBase() // throws
	{
		HMODULE sim = GetModuleHandleW(mGame.toModuleName().c_str());
		if (!sim) throw HCMRuntimeException("HaloSimulation_tag_release.dll is not loaded");
		return (uintptr_t)sim;
	}

	uintptr_t getGameThreadTeb() // throws
	{
		std::scoped_lock lock(mCacheMutex);
		if (mCachedTeb) return mCachedTeb;

		// A full-system thread snapshot is not free. If the last walk failed (typical at a menu, before the
		// game thread exists), don't retry more than once a second - the maintain passes call in at 250 ms.
		const auto now = std::chrono::steady_clock::now();
		if (mLastFailedWalk.time_since_epoch().count() != 0 && (now - mLastFailedWalk) < std::chrono::milliseconds(1000))
			throw HCMRuntimeException("Halo Campaign Evolved game thread not available");

		try
		{
			mCachedTeb = findGameThreadTeb();
		}
		catch (HCMRuntimeException)
		{
			mLastFailedWalk = now;
			throw;
		}
		mLastFailedWalk = {};
		return mCachedTeb;
	}

	uintptr_t getTlsBase() // throws
	{
		const uintptr_t teb = getGameThreadTeb();

		int32_t tlsIndex = 0;
		if (!HCEGetPlayerState::tryReadRaw(resolveOrThrow(mTlsIndex, nameof(hceTlsIndex)), &tlsIndex, sizeof(tlsIndex)))
			throw HCMRuntimeException("Could not read the HaloCER TLS index");
		if (tlsIndex < 0 || tlsIndex > 0xFFFF) throw HCMRuntimeException(std::format("Implausible HaloCER TLS index: {}", tlsIndex));

		uintptr_t tlsArray = 0;
		if (!HCEGetPlayerState::tryReadRaw(teb + mTebTlsArrayOffset, &tlsArray, sizeof(tlsArray)) || !tlsArray)
		{
			// The thread we cached is gone (or was never the right one). Force a re-walk next time.
			// getGameThreadTeb has already released mCacheMutex, so re-taking it here is safe.
			invalidateCache();
			throw HCMRuntimeException("Could not read the HaloCER game thread's TLS array");
		}

		uintptr_t tlsBase = 0;
		if (!HCEGetPlayerState::tryReadRaw(tlsArray + (uintptr_t)tlsIndex * 8, &tlsBase, sizeof(tlsBase)) || !tlsBase)
			throw HCMRuntimeException("Could not read the HaloCER TLS block");

		return tlsBase;
	}

	// Reads a pointer slot hanging off the TLS block. Every TLS offset except ai_enabled needs this.
	uintptr_t readTlsSlot(int64_t slotOffset, const char* what) // throws
	{
		const uintptr_t tls = getTlsBase();
		uintptr_t value = 0;
		if (!HCEGetPlayerState::tryReadRaw(tls + slotOffset, &value, sizeof(value)) || !value)
			throw HCMRuntimeException(std::format("HaloCER {} is not available right now", what));
		return value;
	}

	uintptr_t getAiEnabledAddress() { return readTlsSlot(mAiEnabledTlsOffset, "AI state"); }

	uintptr_t getSkullFlagsAddress() { return readTlsSlot(mSkullArrayTlsOffset, "skull state") + mSkullArrayOffset; }

	uintptr_t getFreecamToggleAddress() { return readTlsSlot(mFreecamToggleTlsOffset, "camera state") + mFreecamToggleOffset; }

	uintptr_t getActiveCameraEntry() // throws
	{
		const uintptr_t cameraEntries = readTlsSlot(mCameraEntriesTlsOffset, "camera list");

		// The slot BASE is the entry; the qword AT the slot is only the liveness test. First live slot wins.
		for (int slot = 0; slot < 4; ++slot)
		{
			const uintptr_t candidate = cameraEntries + (uintptr_t)slot * mCameraEntryStride;
			uintptr_t probe = 0;
			if (HCEGetPlayerState::tryReadRaw(candidate, &probe, sizeof(probe)) && probe != 0)
				return candidate;
		}
		throw HCMRuntimeException("No active HaloCER camera was found");
	}

	uint32_t getPlayerDatum() // throws
	{
		const uintptr_t playerTable = readTlsSlot(mPlayerDatumTlsOffset, "player table");
		uint32_t datum = 0;
		if (!HCEGetPlayerState::tryReadRaw(playerTable + mPlayerDatumOffset, &datum, sizeof(datum)))
			throw HCMRuntimeException("Could not read the HaloCER player datum");
		return datum;
	}

	uintptr_t getPlayerObject() // throws
	{
		const uint32_t datum = getPlayerDatum();
		const uint16_t playerIndex = (uint16_t)(datum & 0xFFFF);
		if (playerIndex == 0xFFFF) throw HCMRuntimeException("The player is dead or not spawned");

		const uintptr_t p1 = readTlsSlot(mObjectTableTlsOffset, "object table root");

		uintptr_t table = 0;
		if (!HCEGetPlayerState::tryReadRaw(p1 + mObjectTableOffset, &table, sizeof(table)) || !table)
			throw HCMRuntimeException("Could not read the HaloCER object table");

		uintptr_t playerObject = 0;
		const uintptr_t entry = table + (uintptr_t)playerIndex * mObjectTableStride + mObjectTableEntryOffset;
		if (!HCEGetPlayerState::tryReadRaw(entry, &playerObject, sizeof(playerObject)) || !playerObject)
			throw HCMRuntimeException("Could not resolve the HaloCER player object");
		return playerObject;
	}

	uintptr_t getPlayerPhysicsEntry() // throws
	{
		const uintptr_t playerObject = getPlayerObject();

		// SIGNED int16. Negative means the biped currently has no havok body (mid-load, in a vehicle transition).
		int16_t havokIndex = 0;
		if (!HCEGetPlayerState::tryReadRaw(playerObject + mHavokIndexOffset, &havokIndex, sizeof(havokIndex)))
			throw HCMRuntimeException("Could not read the HaloCER havok index");
		if (havokIndex < 0) throw HCMRuntimeException("The player has no physics body right now");

		// The one non-TLS link in this chain: the physics table is module-relative.
		const uintptr_t physicsSlot = resolveOrThrow(mPhysicsTableSlot, nameof(hcePhysicsTable));
		uintptr_t physicsRoot = 0;
		if (!HCEGetPlayerState::tryReadRaw(physicsSlot, &physicsRoot, sizeof(physicsRoot)) || !physicsRoot)
			throw HCMRuntimeException("Could not read the HaloCER physics root");

		uintptr_t physicsTable = 0;
		if (!HCEGetPlayerState::tryReadRaw(physicsRoot + mPhysicsRootOffset, &physicsTable, sizeof(physicsTable)) || !physicsTable)
			throw HCMRuntimeException("Could not read the HaloCER physics table");

		const uintptr_t entryAddress = physicsTable + (uintptr_t)havokIndex * mPhysicsEntryStride;

		uintptr_t intermediate = 0;
		if (!HCEGetPlayerState::tryReadRaw(entryAddress + mPhysicsEntryOffset, &intermediate, sizeof(intermediate)) || !intermediate)
			throw HCMRuntimeException("Could not read the HaloCER physics entry");

		uintptr_t physicsEntry = 0;
		if (!HCEGetPlayerState::tryReadRaw(intermediate + mPhysicsEntryOffset2, &physicsEntry, sizeof(physicsEntry)) || !physicsEntry)
			throw HCMRuntimeException("Could not resolve the HaloCER player physics");

		return physicsEntry;
	}

	std::string getCurrentLevelName() // throws
	{
		const uintptr_t address = resolveOrThrow(mCurrentLevel, nameof(hceCurrentLevel));
		// A STRING, not an index and not a pointer. The reference tool reads at most 40 bytes.
		char buffer[41]{};
		if (!HCEGetPlayerState::tryReadRaw(address, buffer, sizeof(buffer) - 1))
			throw HCMRuntimeException("Could not read the HaloCER level name");
		buffer[sizeof(buffer) - 1] = '\0';

		std::string out;
		for (char c : buffer)
		{
			if (c == '\0') break;
			if (c < 0x20 || c > 0x7E) break; // stop at the first non-printable rather than showing garbage
			out.push_back(c);
		}
		if (out.empty()) throw HCMRuntimeException("No HaloCER level is loaded");
		return out;
	}

	int32_t getCurrentBSP() // throws
	{
		const uintptr_t address = resolveOrThrow(mCurrentBSP, nameof(hceCurrentBSP));
		int32_t value = 0;
		if (!HCEGetPlayerState::tryReadRaw(address, &value, sizeof(value)))
			throw HCMRuntimeException("Could not read the HaloCER current BSP");
		return value;
	}

	int32_t getTickCounter() // throws
	{
		// A POINTER to the counter, not the counter. Reading it in place is what made HCEStateHook report
		// "Loading" forever before it was fixed - see HCEStateHook.cpp.
		const uintptr_t slot = resolveOrThrow(mTickCounterSlot, nameof(hceTickCounterSlot));
		uintptr_t target = 0;
		if (!HCEGetPlayerState::tryReadRaw(slot, &target, sizeof(target)) || !target)
			throw HCMRuntimeException("Could not read the HaloCER tick counter pointer");
		int32_t value = 0;
		if (!HCEGetPlayerState::tryReadRaw(target, &value, sizeof(value)))
			throw HCMRuntimeException("Could not read the HaloCER tick counter");
		return value;
	}

	// physicsEntry field offsets. Kept as constants rather than XML because they are all the same struct and
	// are only meaningful together (see the teleport sequence below).
	static constexpr int64_t kMotionTranslation = 0x1A0;
	static constexpr int64_t kMotionSecondary = 0x1B0;
	static constexpr int64_t kPosition = 0x1C0;
	static constexpr int64_t kVelocity = 0x230;
	static constexpr int64_t kMotionExtra1 = 0x250;
	static constexpr int64_t kMotionExtra2 = 0x260;
	// playerObject field offsets
	static constexpr int64_t kInterp0 = 0x20;
	static constexpr int64_t kInterp1 = 0x30;
	static constexpr int64_t kOrigin = 0x44;

	static SimpleMath::Vector3 read3(uintptr_t address, const char* what) // throws
	{
		float v[3]{};
		if (!HCEGetPlayerState::tryReadRaw(address, v, sizeof(v)))
			throw HCMRuntimeException(std::format("Could not read HaloCER {}", what));
		return SimpleMath::Vector3(v[0], v[1], v[2]);
	}

	static void write3(uintptr_t address, const SimpleMath::Vector3& value, const char* what) // throws
	{
		// One contiguous 12-byte blob, exactly like the reference tool's struct.pack("<3f", ...).
		const float v[3] = { value.x, value.y, value.z };
		if (!HCEGetPlayerState::tryWriteRaw(address, v, sizeof(v)))
			throw HCMRuntimeException(std::format("Could not write HaloCER {}", what));
	}

	SimpleMath::Vector3 getPlayerPosition() { return read3(getPlayerPhysicsEntry() + kPosition, "player position"); }
	SimpleMath::Vector3 getPlayerVelocity() { return read3(getPlayerPhysicsEntry() + kVelocity, "player velocity"); }

	// Both at once, resolving the (long) TLS->object->physics chain ONCE. The info overlay wants both every
	// refresh; calling the two accessors above walked the entire chain twice per frame for no reason.
	void getPlayerPositionAndVelocity(SimpleMath::Vector3& outPosition, SimpleMath::Vector3& outVelocity)
	{
		const uintptr_t entry = getPlayerPhysicsEntry();
		outPosition = read3(entry + kPosition, "player position");
		outVelocity = read3(entry + kVelocity, "player velocity");
	}

	void setPlayerVelocity(SimpleMath::Vector3 velocity)
	{
		write3(getPlayerPhysicsEntry() + kVelocity, velocity, "player velocity");
	}

	// The teleport. This is NOT a single position write: the havok body's centre and the biped object's origin
	// sit at different points, and the delta between them has to be preserved or the biped snaps back / desyncs
	// from its physics body on the next tick. Reproduces player_camera.py::_teleport_player_to_coordinates
	// including the deliberate asymmetry - the OBJECT fields get the centre-compensated worldTranslation while
	// the MOTION fields at 0x1B0/0x1C0/0x250/0x260 get the raw target.
	void teleportPlayerTo(SimpleMath::Vector3 target) // throws
	{
		const uintptr_t physicsEntry = getPlayerPhysicsEntry();
		const uintptr_t playerObject = getPlayerObject();

		const SimpleMath::Vector3 current = read3(physicsEntry + kPosition, "player position");
		const SimpleMath::Vector3 translation = read3(physicsEntry + kMotionTranslation, "physics transform");

		const SimpleMath::Vector3 centerOffset = current - translation;
		const SimpleMath::Vector3 worldTranslation = target - centerOffset;

		const SimpleMath::Vector3 origin = read3(playerObject + kOrigin, "biped origin");
		const SimpleMath::Vector3 interp0 = read3(playerObject + kInterp0, "biped interpolation");
		const SimpleMath::Vector3 interp1 = read3(playerObject + kInterp1, "biped interpolation");

		const SimpleMath::Vector3 objectDelta = worldTranslation - origin;

		// Order matters; abort on the first failure like the reference does.
		write3(playerObject + kInterp0, interp0 + objectDelta, "biped interpolation");
		write3(playerObject + kInterp1, interp1 + objectDelta, "biped interpolation");
		write3(playerObject + kOrigin, worldTranslation, "biped origin");
		write3(physicsEntry + kMotionTranslation, worldTranslation, "physics transform");
		write3(physicsEntry + kMotionSecondary, target, "physics position");
		write3(physicsEntry + kPosition, target, "physics position");
		write3(physicsEntry + kMotionExtra1, target, "physics position");
		write3(physicsEntry + kMotionExtra2, target, "physics position");
	}
};


HCEGetPlayerState::HCEGetPlayerState(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEGetPlayerStateImpl>(game, dicon))
{
}

HCEGetPlayerState::~HCEGetPlayerState()
{
	PLOG_VERBOSE << "~" << getName();
}

uintptr_t HCEGetPlayerState::getSimModuleBase() { return pimpl->getSimModuleBase(); }
uintptr_t HCEGetPlayerState::getGameThreadTeb() { return pimpl->getGameThreadTeb(); }
uintptr_t HCEGetPlayerState::getTlsBase() { return pimpl->getTlsBase(); }
uintptr_t HCEGetPlayerState::getAiEnabledAddress() { return pimpl->getAiEnabledAddress(); }
uintptr_t HCEGetPlayerState::getSkullFlagsAddress() { return pimpl->getSkullFlagsAddress(); }
uintptr_t HCEGetPlayerState::getFreecamToggleAddress() { return pimpl->getFreecamToggleAddress(); }
uintptr_t HCEGetPlayerState::getActiveCameraEntry() { return pimpl->getActiveCameraEntry(); }
uint32_t  HCEGetPlayerState::getPlayerDatum() { return pimpl->getPlayerDatum(); }
uintptr_t HCEGetPlayerState::getPlayerObject() { return pimpl->getPlayerObject(); }
uintptr_t HCEGetPlayerState::getPlayerPhysicsEntry() { return pimpl->getPlayerPhysicsEntry(); }
SimpleMath::Vector3 HCEGetPlayerState::getPlayerPosition() { return pimpl->getPlayerPosition(); }
SimpleMath::Vector3 HCEGetPlayerState::getPlayerVelocity() { return pimpl->getPlayerVelocity(); }
void HCEGetPlayerState::getPlayerPositionAndVelocity(SimpleMath::Vector3& outPosition, SimpleMath::Vector3& outVelocity) { pimpl->getPlayerPositionAndVelocity(outPosition, outVelocity); }
void HCEGetPlayerState::setPlayerVelocity(SimpleMath::Vector3 velocity) { pimpl->setPlayerVelocity(velocity); }
void HCEGetPlayerState::teleportPlayerTo(SimpleMath::Vector3 target) { pimpl->teleportPlayerTo(target); }
std::string HCEGetPlayerState::getCurrentLevelName() { return pimpl->getCurrentLevelName(); }
int32_t HCEGetPlayerState::getCurrentBSP() { return pimpl->getCurrentBSP(); }
int32_t HCEGetPlayerState::getTickCounter() { return pimpl->getTickCounter(); }
void HCEGetPlayerState::invalidateCache() { pimpl->invalidateCache(); }
