#include "pch.h"
#include "HCEGetPlayerState.h"
#include "HCEAnchors.h"
#include "HCESkyFix.h"   // requestAreaReArm: a seated teleport must put the streaming area back
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include <TlHelp32.h>
#include <cmath>

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
	// NOTE: deliberately NO VirtualQuery precheck. sehCopy already makes a bad read harmless - catching the access
	// violation IS the safety mechanism - so the VirtualQuery was a kernel transition per read that bought nothing.
	// It cost real frames: one position fetch walks TEB -> tls index -> tls array -> tls base -> player datum ->
	// object -> havok index -> physics root -> table -> entry -> floats, and the info overlay did that for
	// position AND velocity every refresh.
	if (address == 0 || size == 0) return false;
	return sehCopy(out, (const void*)address, size);
}

bool HCEGetPlayerState::tryWriteRaw(uintptr_t address, const void* in, size_t size)
{
	// The write path used to gate on regionAllows() while the read path did not. That asymmetry cost us a real
	// bug: invulnerability's cannot_die write was the FIRST HCE feature to ever exercise this function, it
	// returned false silently, and a silent false is indistinguishable from a successful write to every caller.
	// Every address reaching here has been produced by a fully null-checked pointer walk out of the engine's own
	// object table, so the VirtualQuery bought no safety that sehCopy does not already provide - it only added a
	// kernel transition and a silent failure mode.
	if (address == 0 || size == 0) return false;
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

	// s_game_time_globals: tls + 0x98 -> struct, tick at +0xC. See getTickCounter for why this is NOT
	// hceTickCounterSlot.
	int64_t mGameTimeGlobalsTlsOffset = 0x98;
	int64_t mGameTimeOffset = 0x0C;
	int64_t mPlayerDatumOffset = 0x98;
	int64_t mObjectTableTlsOffset = 0x20;
	int64_t mObjectTableOffset = 0x50;
	int64_t mObjectTableStride = 0x18;
	int64_t mObjectTableEntryOffset = 0x10;
	int64_t mHavokIndexOffset = 0xCC;
	// currentVehicleDatum on the biped - the seat object the player is riding. -1 disables the promotion in
	// getControlledObject entirely. Measured at 0x14 by scanForSeatField; see InternalPointerData.xml.
	int64_t mBipedVehicleDatumOffset = 0x14;
	int64_t mPhysicsRootOffset = 0x50;
	int64_t mPhysicsEntryStride = 0xC0;
	int64_t mPhysicsEntryOffset = 0x30;
	int64_t mPhysicsEntryOffset2 = 0x40;
	int64_t mFreecamToggleTlsOffset = 0xB8;
	int64_t mFreecamToggleOffset = 0x9C8;
	int64_t mCameraEntriesTlsOffset = 0x148;
	int64_t mCameraEntryStride = 0x1AC;
	// ⚠⚠ +0x20 IS NOT A POSITION AND NEVER WAS. Kept only because the pointer data still carries it and
	// removing an entry is a separate change; getCameraView no longer reads it. The camera entry's +0x20 is a
	// WORD (`00215D0D mov word ptr [rdi+0x20], r9w`) and +0x28 holds a vftable pointer
	// (`00215C96 lea rax,[rip+0x66c703]` -> 0x8823A0, whose first qwords are real code addresses), so a
	// 12-byte float3 read at +0x20 straddles a vptr and returns garbage. Symptom: "Teleport to Camera" sent
	// the player to a bogus world point; Renderer3DImplD3D12.cpp:808 had already noticed the same value was
	// wrong and worked around it locally instead of fixing the source.
	int64_t mCameraPositionOffset = 0x20;

	// THE REAL CAMERA POSITION lives in "observer globals", not in the camera entry:
	//     observerIndex = *(int32*)(cameraEntry + 0x180)
	//     observer      = *(tls + 0x4E8) + observerIndex * 0x410
	//     position      = observer + 0x154   (three floats: x, y, z)
	// Proven live rather than by construction: the engine builds its camera descriptor from exactly this
	// field every frame - `0000CF0C imul rdi, r14, 0x410`, then `0000CFA1/CFAA/CFB3 vmovss xmm2/3/4,
	// [rdi+rbx+0x154/+0x158/+0x15C]` feeding the descriptor stores at 0xCFD5/CFDA/CFDF.
	// ⚠ DO NOT "simplify" THIS TO cameraEntry+0x50. That field holds the same value but is written ONCE at
	// construction (`00215CE3 vmovsd [rdi+0x50], xmm0`) and has NO reader anywhere in the module - a freecam
	// that has moved since the entry was built would teleport the player to where the camera STARTED.
	int64_t mCameraObserverIndexOffset = 0x180;
	int64_t mObserverGlobalsTlsOffset = 0x4E8;
	int64_t mObserverStride = 0x410;
	int64_t mObserverPositionOffset = 0x154;
	// s_player_control. The TLS slot is the SAME 0xB8 block the freecam toggle byte lives in (its +0x9C8 is
	// the last flag byte of these player-control globals); it is named separately because it is a different
	// structure inside that block, not because it is a different pointer.
	int64_t mPlayerControlTlsOffset = 0xB8;
	int64_t mPlayerControlArrayOffset = 0x80;
	int64_t mPlayerControlStride = 0x198;
	int64_t mPlayerControlUnitDatumOffset = 0x00;
	int64_t mPlayerControlYawOffset = 0x14;
	int64_t mPlayerControlPitchOffset = 0x18;

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
		hceOffset(mGameTimeGlobalsTlsOffset, hceGameTimeGlobalsTlsOffset);
		hceOffset(mGameTimeOffset, hceGameTimeOffset);
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
		hceOffset(mCameraPositionOffset, hceCameraPositionOffset);
		hceOffset(mPlayerControlTlsOffset, hcePlayerControlTlsOffset);
		hceOffset(mPlayerControlArrayOffset, hcePlayerControlArrayOffset);
		hceOffset(mPlayerControlStride, hcePlayerControlStride);
		hceOffset(mBipedVehicleDatumOffset, hceBipedVehicleDatumOffset);
		hceOffset(mPlayerControlUnitDatumOffset, hcePlayerControlUnitDatumOffset);
		hceOffset(mPlayerControlYawOffset, hcePlayerControlYawOffset);
		hceOffset(mPlayerControlPitchOffset, hcePlayerControlPitchOffset);
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

		// First point in the process where the sim is known to be loaded, so it is where the byte-signature
		// anchors are resolved. Idempotent and keyed on the module base: a no-op after the first call, and it
		// re-arms by itself if the module is ever reloaded at a different address.
		//
		// ⚠ NOTHING CONSUMES THESE YET - deliberately observation-only for now. The signatures were verified
		// offline to reproduce the very addresses InternalPointerData.xml already carries; logging them from a
		// live build is the cheap confirmation of that before any feature depends on them. See HCEAnchors.h.
		HCEAnchors::resolveAll((uintptr_t)sim);

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

	// Same walk as readTlsSlot but starting from an ALREADY resolved tls block, so a caller that wants two
	// things out of the TLS pays for the TEB/index/array/base walk once. See getCameraView.
	uintptr_t readSlotOf(uintptr_t tls, int64_t slotOffset, const char* what) // throws
	{
		uintptr_t value = 0;
		if (!HCEGetPlayerState::tryReadRaw(tls + slotOffset, &value, sizeof(value)) || !value)
			throw HCMRuntimeException(std::format("HaloCER {} is not available right now", what));
		return value;
	}

	uintptr_t activeCameraEntryOf(uintptr_t tls) // throws
	{
		const uintptr_t cameraEntries = readSlotOf(tls, mCameraEntriesTlsOffset, "camera list");

		// The slot BASE is the entry; the qword AT the slot is only the liveness test (it is the director
		// object's vtable pointer). First live slot wins.
		for (int slot = 0; slot < 4; ++slot)
		{
			const uintptr_t candidate = cameraEntries + (uintptr_t)slot * mCameraEntryStride;
			uintptr_t probe = 0;
			if (HCEGetPlayerState::tryReadRaw(candidate, &probe, sizeof(probe)) && probe != 0)
				return candidate;
		}
		throw HCMRuntimeException("No active HaloCER camera was found");
	}

	uintptr_t getActiveCameraEntry() { return activeCameraEntryOf(getTlsBase()); }

	// s_player_control for the local player. Campaign is always index 0, but the array is scanned for the slot
	// whose unit datum matches the player's so a co-op/observer setup cannot silently give us slot 0's angles.
	// Falls back to slot 0 if no slot matches (the datum is 0xFFFFFFFF while dead / mid-load).
	uintptr_t playerControlEntryOf(uintptr_t tls) // throws
	{
		const uintptr_t globals = readSlotOf(tls, mPlayerControlTlsOffset, "player control state");
		const uintptr_t table = globals + mPlayerControlArrayOffset;

		uint32_t wantDatum = 0xFFFFFFFFu;
		try { wantDatum = playerDatumOf(tls); }
		catch (HCMRuntimeException) { /* dead or mid-load: slot 0 is still the right guess */ }

		if (wantDatum != 0xFFFFFFFFu)
		{
			for (int i = 0; i < 4; ++i)
			{
				const uintptr_t candidate = table + (uintptr_t)i * mPlayerControlStride;
				uint32_t datum = 0;
				if (HCEGetPlayerState::tryReadRaw(candidate + mPlayerControlUnitDatumOffset, &datum, sizeof(datum))
					&& datum == wantDatum)
					return candidate;
			}
		}
		return table;
	}

	uintptr_t getPlayerControlEntry() { return playerControlEntryOf(getTlsBase()); }

	// yaw and pitch are eight CONTIGUOUS bytes (a real_euler_angles3d whose roll at +0x1C is always 0 for the
	// player), so they come back as one read.
	SimpleMath::Vector2 viewAngleOf(uintptr_t control) // throws
	{
		float angles[2]{};
		if (mPlayerControlPitchOffset == mPlayerControlYawOffset + 4)
		{
			if (!HCEGetPlayerState::tryReadRaw(control + mPlayerControlYawOffset, angles, sizeof(angles)))
				throw HCMRuntimeException("Could not read the HaloCER view angle");
		}
		else
		{
			if (!HCEGetPlayerState::tryReadRaw(control + mPlayerControlYawOffset, &angles[0], sizeof(float))
				|| !HCEGetPlayerState::tryReadRaw(control + mPlayerControlPitchOffset, &angles[1], sizeof(float)))
				throw HCMRuntimeException("Could not read the HaloCER view angle");
		}

		// Radians. Anything outside these bounds means we are reading the wrong thing (or a half-written
		// struct), and silently returning it would point every 3D overlay in a random direction.
		if (!std::isfinite(angles[0]) || !std::isfinite(angles[1])
			|| std::abs(angles[0]) > 7.f || std::abs(angles[1]) > 1.58f)
			throw HCMRuntimeException("The HaloCER view angle is not available right now");

		return SimpleMath::Vector2(angles[0], angles[1]);
	}

	SimpleMath::Vector2 getPlayerViewAngle() { return viewAngleOf(playerControlEntryOf(getTlsBase())); }

	void getCameraView(SimpleMath::Vector3& outCameraPosition, SimpleMath::Vector2& outViewAngle) // throws
	{
		const uintptr_t tls = getTlsBase();     // ONE walk for both, see the PERF note at the top of this file
		const uintptr_t cameraEntry = activeCameraEntryOf(tls);

		// See the offsets block above for why this does NOT read cameraEntry + mCameraPositionOffset.
		int32_t observerIndex = -1;
		if (!HCEGetPlayerState::tryReadRaw(cameraEntry + mCameraObserverIndexOffset, &observerIndex, sizeof(observerIndex)))
			throw HCMRuntimeException("Could not read the HaloCER camera's observer index");

		// The index is a slot into a small fixed table (the block is 0x1050 bytes at stride 0x410, so four
		// entries). Bounds-check it rather than trusting it: this value scales a pointer, and the engine
		// itself leaves it at -1 until the camera is bound to an observer.
		if (observerIndex < 0 || (int64_t)observerIndex * mObserverStride >= 0x1050)
			throw HCMRuntimeException("The HaloCER camera is not bound to an observer right now");

		const uintptr_t observers = readSlotOf(tls, mObserverGlobalsTlsOffset, "observer globals");
		const uintptr_t observer = observers + (uintptr_t)observerIndex * mObserverStride;

		float coords[3]{};
		if (!HCEGetPlayerState::tryReadRaw(observer + mObserverPositionOffset, coords, sizeof(coords)))
			throw HCMRuntimeException("Could not read the HaloCER camera position");

		// A camera that has never been positioned reads as exactly the origin. Returning it would look like a
		// successful read and teleport the player into the void, which is the failure this whole change is
		// about - so treat it as "not available yet" instead.
		if (!std::isfinite(coords[0]) || !std::isfinite(coords[1]) || !std::isfinite(coords[2])
			|| (coords[0] == 0.f && coords[1] == 0.f))
			throw HCMRuntimeException("The HaloCER camera position is not available right now");

		outCameraPosition = SimpleMath::Vector3(coords[0], coords[1], coords[2]);

		outViewAngle = viewAngleOf(playerControlEntryOf(tls));
	}

	uint32_t playerDatumOf(uintptr_t tls) // throws
	{
		const uintptr_t playerTable = readSlotOf(tls, mPlayerDatumTlsOffset, "player table");
		uint32_t datum = 0;
		if (!HCEGetPlayerState::tryReadRaw(playerTable + mPlayerDatumOffset, &datum, sizeof(datum)))
			throw HCMRuntimeException("Could not read the HaloCER player datum");
		return datum;
	}

	uint32_t getPlayerDatum() { return playerDatumOf(getTlsBase()); }

	// Datum -> object address. Split out of getPlayerObject so the seat lookup below can reuse it. Returns false
	// rather than throwing, because a failed lookup there means "no vehicle", not "something is wrong".
	bool tryObjectFromDatum(uint32_t datum, uintptr_t& out)
	{
		out = 0;
		const uint16_t index = (uint16_t)(datum & 0xFFFF);
		if (datum == 0xFFFFFFFFu || index == 0xFFFF) return false;

		uintptr_t p1 = 0;
		try { p1 = readTlsSlot(mObjectTableTlsOffset, "object table root"); }
		catch (HCMRuntimeException&) { return false; }

		uintptr_t table = 0;
		if (!HCEGetPlayerState::tryReadRaw(p1 + mObjectTableOffset, &table, sizeof(table)) || !table) return false;

		const uintptr_t entry = table + (uintptr_t)index * mObjectTableStride + mObjectTableEntryOffset;
		return HCEGetPlayerState::tryReadRaw(entry, &out, sizeof(out)) && out != 0;
	}

	uintptr_t getPlayerObject() // throws
	{
		const uint32_t datum = getPlayerDatum();
		const uint16_t playerIndex = (uint16_t)(datum & 0xFFFF);
		if (playerIndex == 0xFFFF) throw HCMRuntimeException("The player is dead or not spawned");

		uintptr_t playerObject = 0;
		if (!tryObjectFromDatum(datum, playerObject))
			throw HCMRuntimeException("Could not resolve the HaloCER player object");
		return playerObject;
	}

	// ============================================================================================================
	// THE OBJECT THE PLAYER IS ACTUALLY DRIVING - which is not always their biped.
	//
	// Sat in a vehicle (or another biped's seat) the player's biped has NO havok body; the SEAT OBJECT owns the
	// physics. Teleport and launch both write through getPlayerPhysicsEntry, so without this they either did
	// nothing or failed with "The player has no physics body right now" - the biped being moved is not the thing
	// that is moving.
	//
	// MCC already does exactly this promotion in GetObjectPhysics, using GetBipedsVehicleDatum to read the biped's
	// currentVehicleDatum field. HaloCER simply never had that lookup: bipedDataFields has entries for every MCC
	// title and none for HaloCER. Like MCC's, this accepts a biped seat-object as well as a vehicle - a biped can
	// carry seats, and both expose physics identically.
	//
	// ⚠ FAILS SOFT, DELIBERATELY. Offset unknown, read failed, null datum, unresolvable object, seat with no
	// physics body - every one of those returns the biped rather than throwing. The offset is the single part of
	// this that is inferred rather than measured (MCC's Halo 1 has currentVehicleDatum at 0xD8 and HaloCER is a
	// Halo 1 sim), so a wrong value MUST degrade to the previous behaviour instead of moving an arbitrary object.
	// The havok-index check is what makes that true: a garbage datum will not resolve to something with a live
	// physics body.
	// ============================================================================================================
	// ⚠ TEMPORARY. Finds the seat field instead of guessing it, then comes out again.
	//
	// hceBipedVehicleDatumOffset was seeded with 0xD8 because that is where MCC's Halo 1 keeps
	// currentVehicleDatum and HaloCER is a Halo 1 sim. It is not: with 0xD8 the promotion never fired once, so
	// the value there does not resolve to a seated object.
	//
	// Rather than try another number, this walks the biped's object header and reports EVERY dword that resolves
	// through the object table to some other object with a live physics body. Run it once on foot and once in a
	// vehicle: the offset that only appears in the second list is the field. That is a measurement, which is what
	// this needed in the first place.
	//
	// Latched to a handful of runs - it is a linear scan of the object header on a path teleport/launch use.
	void scanForSeatField(uintptr_t biped)
	{
		static std::atomic<int> runsLeft{ 6 };
		if (runsLeft.fetch_sub(1, std::memory_order_relaxed) <= 0) return;

		PLOG_INFO << "HaloCER seat-field scan: biped object @ " << std::format("0x{:X}", biped)
			<< " -- candidates below are (offset -> object with a physics body). Capture this ON FOOT and again "
			"IN A VEHICLE; the offset that appears only in the vehicle capture is currentVehicleDatum.";

		// The player's own position, to measure candidates against. Read straight off the biped's physics entry
		// rather than through getPlayerPosition, which would re-enter getControlledObject.
		SimpleMath::Vector3 me{};
		bool haveMe = false;
		{
			uintptr_t e = 0;
			if (tryPhysicsEntryForObject(biped, e))
				haveMe = HCEGetPlayerState::tryReadRaw(e + kPosition, &me, sizeof(me));
		}

		int found = 0;
		for (int64_t off = 0; off < 0x400; off += 4)
		{
			uint32_t datum = 0;
			if (!HCEGetPlayerState::tryReadRaw(biped + off, &datum, sizeof(datum))) continue;

			// ⚠ A DATUM IS NOT JUST ANY DWORD. Only the low 16 bits index the table, so without checking the salt
			// in the high half every float and small counter in the header "resolves" - the first pass reported
			// 0x3F800000 (which is 1.0f) as a candidate. A real datum has a non-zero, non-0xFFFF salt AND a
			// non-zero index.
			const uint16_t index = (uint16_t)(datum & 0xFFFF);
			const uint16_t salt  = (uint16_t)(datum >> 16);
			if (index == 0 || index == 0xFFFF) continue;
			if (salt == 0 || salt == 0xFFFF) continue;

			uintptr_t obj = 0;
			if (!tryObjectFromDatum(datum, obj) || obj == biped) continue;

			int16_t havok = -1;
			if (!HCEGetPlayerState::tryReadRaw(obj + mHavokIndexOffset, &havok, sizeof(havok))) continue;
			if (havok < 0) continue;   // no physics body - not something teleport/launch could move anyway

			// THE DECIDING TEST. Whatever you are riding is where you are. Anything metres away is something the
			// biped merely references (a target, a nearby actor); the seat object sits on top of us.
			std::string where = "  distance unknown";
			bool close = false;
			if (haveMe)
			{
				uintptr_t e = 0;
				SimpleMath::Vector3 p{};
				if (tryPhysicsEntryForObject(obj, e) && HCEGetPlayerState::tryReadRaw(e + kPosition, &p, sizeof(p)))
				{
					const float d = (p - me).Length();
					close = (d < 12.f);
					where = std::format("  distance {:.2f}{}", d, close ? "   <== CO-LOCATED, likely the seat" : "");
				}
			}

			PLOG_INFO << "  candidate +" << std::format("0x{:X}", off)
				<< "  datum " << std::format("0x{:08X}", datum)
				<< "  object " << std::format("0x{:X}", obj)
				<< "  havokIndex " << havok << where;
			found++;
		}
		if (!found) PLOG_INFO << "  (no candidates - the player may not be seated, or the seat datum is not a "
			"plain dword in the first 0x400 bytes of the object)";
	}

	uintptr_t getControlledObject() // throws
	{
		const uintptr_t biped = getPlayerObject();
		if (mBipedVehicleDatumOffset < 0) return biped;

		uint32_t vehicleDatum = 0xFFFFFFFFu;
		if (!HCEGetPlayerState::tryReadRaw(biped + mBipedVehicleDatumOffset, &vehicleDatum, sizeof(vehicleDatum)))
			return biped;

		uintptr_t seat = 0;
		if (!tryObjectFromDatum(vehicleDatum, seat)) { scanForSeatField(biped); return biped; }

		// The seat must own a physics body, or promoting buys nothing. This doubles as the sanity check on the
		// offset itself.
		int16_t seatHavok = -1;
		if (!HCEGetPlayerState::tryReadRaw(seat + mHavokIndexOffset, &seatHavok, sizeof(seatHavok)) || seatHavok < 0)
		{
			scanForSeatField(biped);
			return biped;
		}

		LOG_ONCE(PLOG_INFO << "HaloCER: the player is in a seat, so teleport/launch will act on the seat object "
			"rather than the biped - the same promotion MCC does via GetBipedsVehicleDatum.");
		return seat;
	}

	// Same walk as getPlayerPhysicsEntry but for an arbitrary object, and non-throwing: the seat scan asks this
	// about objects that may well have no physics at all, where a failure is an answer rather than an error.
	bool tryPhysicsEntryForObject(uintptr_t object, uintptr_t& out)
	{
		out = 0;
		if (!object) return false;

		int16_t havokIndex = -1;
		if (!HCEGetPlayerState::tryReadRaw(object + mHavokIndexOffset, &havokIndex, sizeof(havokIndex))) return false;
		if (havokIndex < 0) return false;

		uintptr_t physicsSlot = 0;
		try { physicsSlot = resolveOrThrow(mPhysicsTableSlot, nameof(hcePhysicsTable)); }
		catch (HCMRuntimeException&) { return false; }

		uintptr_t physicsRoot = 0, physicsTable = 0, intermediate = 0;
		if (!HCEGetPlayerState::tryReadRaw(physicsSlot, &physicsRoot, sizeof(physicsRoot)) || !physicsRoot) return false;
		if (!HCEGetPlayerState::tryReadRaw(physicsRoot + mPhysicsRootOffset, &physicsTable, sizeof(physicsTable)) || !physicsTable) return false;

		const uintptr_t entryAddress = physicsTable + (uintptr_t)havokIndex * mPhysicsEntryStride;
		if (!HCEGetPlayerState::tryReadRaw(entryAddress + mPhysicsEntryOffset, &intermediate, sizeof(intermediate)) || !intermediate) return false;
		return HCEGetPlayerState::tryReadRaw(intermediate + mPhysicsEntryOffset2, &out, sizeof(out)) && out != 0;
	}

	uintptr_t getPlayerPhysicsEntry() // throws
	{
		const uintptr_t playerObject = getControlledObject();

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
		// s_game_time_globals lives in game-state POOL 4, which the checkpoint revert restores wholesale, so
		// this value REWINDS with a revert - matching what the MCC games display.
		//
		// ⚠ Do NOT go back to hceTickCounterSlot for this. That is the base of game-state POOL 5, and the
		// revert (sub_18019D730) memcpys the whole 64 KiB pool ASIDE, restores the game state over the top,
		// then memcpys the pool BACK verbatim - so nothing in pool 5 can ever rewind, by construction. Its
		// first dword is the campaign metagame play clock, which ticks once per game tick (which is exactly
		// why it passed for a tick counter until someone reverted) but must never rewind, or save-scumming
		// would rewind metagame scoring.
		const uintptr_t gameTimeGlobals = readTlsSlot(mGameTimeGlobalsTlsOffset, "game time globals");

		// +0x00 is the "initialised" bool the engine's own game_time_get_seconds checks before using +0xC.
		uint8_t initialised = 0;
		if (!HCEGetPlayerState::tryReadRaw(gameTimeGlobals, &initialised, sizeof(initialised)) || !initialised)
			throw HCMRuntimeException("HaloCER game time is not running yet");

		int32_t value = 0;
		if (!HCEGetPlayerState::tryReadRaw(gameTimeGlobals + mGameTimeOffset, &value, sizeof(value)))
			throw HCMRuntimeException("Could not read the HaloCER game time");
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

	// Read-modify-write on ONE chain walk. Composing this out of getPlayerVelocity + setPlayerVelocity would
	// walk TLS->object->physics twice and could straddle a physics tick between the two.
	SimpleMath::Vector3 addPlayerVelocity(SimpleMath::Vector3 delta) // throws
	{
		const uintptr_t entry = getPlayerPhysicsEntry();
		const SimpleMath::Vector3 result = read3(entry + kVelocity, "player velocity") + delta;
		write3(entry + kVelocity, result, "player velocity");
		return result;
	}

	// The teleport. This is NOT a single position write: the havok body's centre and the biped object's origin
	// sit at different points, and the delta between them has to be preserved or the biped snaps back / desyncs
	// from its physics body on the next tick. Reproduces player_camera.py::_teleport_player_to_coordinates
	// including the deliberate asymmetry - the OBJECT fields get the centre-compensated worldTranslation while
	// the MOTION fields at 0x1B0/0x1C0/0x250/0x260 get the raw target.
	// relative == true means "value is an offset from the CURRENT position" - resolved here rather than by the
	// caller so the whole thing still costs one chain walk and cannot straddle a physics tick. Returns the
	// absolute position that was actually written.
	// Moves ONE object and its own physics entry to target. Split out so a seated player can be moved as a pair:
	// each object's writes must be derived from ITS OWN physics, which is the part that was wrong when the
	// promotion first landed.
	void teleportObjectTo(uintptr_t object, uintptr_t physicsEntry, SimpleMath::Vector3 target) // throws
	{
		const SimpleMath::Vector3 current = read3(physicsEntry + kPosition, "object position");
		const SimpleMath::Vector3 translation = read3(physicsEntry + kMotionTranslation, "physics transform");

		const SimpleMath::Vector3 centerOffset = current - translation;
		const SimpleMath::Vector3 worldTranslation = target - centerOffset;

		const SimpleMath::Vector3 origin = read3(object + kOrigin, "object origin");
		const SimpleMath::Vector3 interp0 = read3(object + kInterp0, "object interpolation");
		const SimpleMath::Vector3 interp1 = read3(object + kInterp1, "object interpolation");

		const SimpleMath::Vector3 objectDelta = worldTranslation - origin;

		// Order matters; abort on the first failure like the reference does.
		write3(object + kInterp0, interp0 + objectDelta, "object interpolation");
		write3(object + kInterp1, interp1 + objectDelta, "object interpolation");
		write3(object + kOrigin, worldTranslation, "object origin");
		write3(physicsEntry + kMotionTranslation, worldTranslation, "physics transform");
		write3(physicsEntry + kMotionSecondary, target, "physics position");
		write3(physicsEntry + kPosition, target, "physics position");
		write3(physicsEntry + kMotionExtra1, target, "physics position");
		write3(physicsEntry + kMotionExtra2, target, "physics position");
	}

	SimpleMath::Vector3 teleportPlayerInternal(SimpleMath::Vector3 value, bool relative) // throws
	{
		const uintptr_t controlled = getControlledObject();   // the vehicle, when seated
		const uintptr_t biped = getPlayerObject();
		const uintptr_t controlledEntry = getPlayerPhysicsEntry();

		const SimpleMath::Vector3 current = read3(controlledEntry + kPosition, "player position");
		const SimpleMath::Vector3 target = relative ? (current + value) : value;

		teleportObjectTo(controlled, controlledEntry, target);

		// ⚠ THE BIPED HAS TO COME TOO, AND THIS IS WHY.
		//
		// Seated, the promotion above moves the VEHICLE - which is what the user asked for - but the player's
		// biped is a separate object with its own physics body, and the engine's world-streaming source follows
		// the BIPED. Move only the vehicle and the streaming system still believes you are stood where you were:
		// it tears down the area you have arrived in, and the sky unloads with it. Reported as "teleport my
		// banshee and the sky deloads".
		//
		// The game re-seats the biped on its own a tick later, but the streaming source samples before that
		// happens, so the window is enough to lose the area. Writing both now closes it. Deriving the biped's
		// writes from the BIPED's physics entry (not the vehicle's) is the other half - the first version of this
		// stamped biped object fields with vehicle-derived values, which is how the two ended up inconsistent.
		if (controlled != biped)
		{
			uintptr_t bipedEntry = 0;
			if (tryPhysicsEntryForObject(biped, bipedEntry))
				teleportObjectTo(biped, bipedEntry, target);
			// No else: a seated biped with no physics body of its own is legitimate, and the vehicle move above
			// has already done the useful work. Failing the whole teleport over it would be worse.

			// ⚠ AND PUT THE STREAMING AREA BACK. Writing the vehicle's position moves it without World Partition
			// seeing an enter or exit, so the area the player occupied tears down and the sky unloads - even for a
			// ONE-FOOT teleport, which is what rules out distance as the cause. On foot this does not happen, so
			// the vehicle is what carries the streaming occupancy while seated.
			//
			// Only reached when a seat was actually promoted to (controlled != biped), so an on-foot teleport is
			// untouched. A no-op unless Sky Fix is enabled - see the note on requestAreaReArm.
			HCESkyFix::requestAreaReArm();
		}

		return target;
	}

	void teleportPlayerTo(SimpleMath::Vector3 target) { teleportPlayerInternal(target, false); }
	SimpleMath::Vector3 teleportPlayerBy(SimpleMath::Vector3 offset) { return teleportPlayerInternal(offset, true); }
};


// Pure math. Same basis MCC's ForceTeleport/ForceLaunch build, and the same one the engine itself uses: HCE's
// sub_1802767D0 derives an up vector from a forward vector as (-f.z*f.x/r, -f.z*f.y/r, r) with r = |f.xy|,
// which is algebraically identical to right x forward below.
SimpleMath::Vector3 HCEGetPlayerState::lookRelativeOffset(SimpleMath::Vector2 viewAngle, SimpleMath::Vector3 forwardRightUp, bool ignoreVerticalLook)
{
	const float yaw = viewAngle.x;
	const float pitch = viewAngle.y;

	// Flattening onto the horizon is exact here, not a renormalisation: forward.xy is literally
	// cos(pitch) * (cos(yaw), sin(yaw)), so dropping the cos(pitch) factor IS the horizontal direction. MCC
	// instead divides by |x| + |y|, which is an L1 norm and leaves the vector between 0.71 and 1.0 long - so
	// "forward 5" there actually moves 3.5 to 5. Doing it this way also removes MCC's divide-by-zero when the
	// player is looking straight up or straight down.
	SimpleMath::Vector3 forward = ignoreVerticalLook
		? SimpleMath::Vector3(std::cos(yaw), std::sin(yaw), 0.f)
		: SimpleMath::Vector3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
	forward.Normalize();

	// right = forward x UnitZ. That cross product degenerates at both poles; substitute its exact limit rather
	// than MCC's arbitrary UnitX, and cover the DOWN pole too (MCC only tests against +UnitZ, so looking
	// straight down silently zeroes the right and up components there).
	SimpleMath::Vector3 right = std::abs(forward.z) > 0.9999f
		? SimpleMath::Vector3(std::sin(yaw), -std::cos(yaw), 0.f)
		: forward.Cross(SimpleMath::Vector3::UnitZ);
	right.Normalize();

	SimpleMath::Vector3 up = right.Cross(forward);
	up.Normalize();

	return (forward * forwardRightUp.x)   // forward component of user input
		+ (right * forwardRightUp.y)      // right component of user input
		+ (up * forwardRightUp.z);        // up component of user input
}


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
uintptr_t HCEGetPlayerState::getPlayerControlEntry() { return pimpl->getPlayerControlEntry(); }
SimpleMath::Vector2 HCEGetPlayerState::getPlayerViewAngle() { return pimpl->getPlayerViewAngle(); }
void HCEGetPlayerState::getCameraView(SimpleMath::Vector3& outCameraPosition, SimpleMath::Vector2& outViewAngle) { pimpl->getCameraView(outCameraPosition, outViewAngle); }
uint32_t  HCEGetPlayerState::getPlayerDatum() { return pimpl->getPlayerDatum(); }
uintptr_t HCEGetPlayerState::getPlayerObject() { return pimpl->getPlayerObject(); }
uintptr_t HCEGetPlayerState::getPlayerPhysicsEntry() { return pimpl->getPlayerPhysicsEntry(); }
SimpleMath::Vector3 HCEGetPlayerState::getPlayerPosition() { return pimpl->getPlayerPosition(); }
SimpleMath::Vector3 HCEGetPlayerState::getPlayerVelocity() { return pimpl->getPlayerVelocity(); }
void HCEGetPlayerState::getPlayerPositionAndVelocity(SimpleMath::Vector3& outPosition, SimpleMath::Vector3& outVelocity) { pimpl->getPlayerPositionAndVelocity(outPosition, outVelocity); }
void HCEGetPlayerState::setPlayerVelocity(SimpleMath::Vector3 velocity) { pimpl->setPlayerVelocity(velocity); }
void HCEGetPlayerState::teleportPlayerTo(SimpleMath::Vector3 target) { pimpl->teleportPlayerTo(target); }
SimpleMath::Vector3 HCEGetPlayerState::teleportPlayerBy(SimpleMath::Vector3 offset) { return pimpl->teleportPlayerBy(offset); }
SimpleMath::Vector3 HCEGetPlayerState::addPlayerVelocity(SimpleMath::Vector3 delta) { return pimpl->addPlayerVelocity(delta); }
std::string HCEGetPlayerState::getCurrentLevelName() { return pimpl->getCurrentLevelName(); }
int32_t HCEGetPlayerState::getCurrentBSP() { return pimpl->getCurrentBSP(); }
int32_t HCEGetPlayerState::getTickCounter() { return pimpl->getTickCounter(); }
void HCEGetPlayerState::invalidateCache() { pimpl->invalidateCache(); }
