#pragma once
#include "pch.h"
#include <Windows.h>
#include <cstring>

// ================================================================================================================================
// GUARDED reads and writes of game memory for the features that patch halo2.dll - 2026-11-18.
//
// WHY: MCC UNLOADS a game dll when the game ends (and loads it again, often at another address, for the next one). A
// feature that kept the module base and wrote "the stock bytes back" later wrote into unmapped memory:
// HCMInternal_CRASHDUMP_20261118_043820 - H2ShadowResolution's destructor, memcpy to halo2.dll+0xE136A0 with halo2.dll
// already gone from the module list. Checking GetModuleHandle first is not enough by itself (MCC can unload between the
// check and the write), so every write also goes through a __try: a fault becomes "false", never a crash.
//
// Everything here is POD-only and takes no user-mode lock (VirtualQuery / VirtualProtect / FlushInstructionCache are bare
// system calls), so it may be used INSIDE a thread-suspension window (see ScopedThreadSuspender.h). GetModuleHandle is
// NOT (loader lock): call that before the window, and use moduleMappedAt() + a bytes check inside it.
// ================================================================================================================================
namespace h2safe
{
	// An image is still mapped with its allocation starting at `base` (which image is the caller's bytes check).
	inline bool moduleMappedAt(uintptr_t base)
	{
		if (!base) return false;
		MEMORY_BASIC_INFORMATION mbi{};
		return VirtualQuery((LPCVOID)(base + 0x1000), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
			&& mbi.Type == MEM_IMAGE && (uintptr_t)mbi.AllocationBase == base;
	}

	// memcpy that reports a fault instead of raising it (either side may be game memory)
	inline bool copy(void* dst, const void* src, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// the n bytes at addr are readable and equal `expect`
	inline bool equals(uintptr_t addr, const void* expect, size_t n)
	{
		__try { return memcmp((const void*)addr, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Write n bytes over code / read-only data: unprotect, guarded copy, put the protection back, flush.
	// False = the pages could not be opened (nothing written) or the copy faulted.
	inline bool patch(uintptr_t addr, const void* src, size_t n)
	{
		DWORD old = 0;
		if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
		const bool ok = copy((void*)addr, src, n);
		DWORD ignored = 0;
		VirtualProtect((LPVOID)addr, n, old, &ignored);
		FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, n);
		return ok;
	}
}
