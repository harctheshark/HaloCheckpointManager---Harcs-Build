#pragma once
#include "pch.h"
#include <TlHelp32.h>
#include <vector>

// RAII helper: suspends every OTHER thread in this process for the lifetime of the object, then
// resumes them all in the destructor.
//
// Why: several cheats patch live halo2.dll code / memory. Reverting those patches while the game's
// render thread is executing the affected code races it and can crash the game (an access violation
// in halo2.dll). Suspending the other threads around the (memory-only) revert closes that window.
// UncapClusterLimit already did this by hand; this is the shared, reusable version.
//
// IMPORTANT: while an instance is alive, do NOT allocate or free heap OR virtual memory
// (new/delete/malloc/free/VirtualAlloc/VirtualFree). A suspended thread may hold the relevant lock,
// which would deadlock this thread. Do any allocation/free BEFORE constructing or AFTER destroying
// the suspender; only plain memory writes (VirtualProtect + memcpy/assignment) are safe in between.
// To make the suspender itself safe, thread ids are gathered FIRST (allocating freely) and only then
// suspended, so no allocation happens once any thread is suspended.
class ScopedThreadSuspender
{
public:
	ScopedThreadSuspender()
	{
		const DWORD me = GetCurrentThreadId();
		const DWORD pid = GetCurrentProcessId();

		// Pass 1: collect the target thread ids. Nothing is suspended yet, so allocating is safe.
		std::vector<DWORD> ids;
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te{}; te.dwSize = sizeof(te);
			if (Thread32First(snap, &te))
				do {
					if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
						ids.push_back(te.th32ThreadID);
				} while (Thread32Next(snap, &te));
			CloseHandle(snap);
		}

		// Pre-size handle storage so the suspend loop below performs no heap allocation.
		mSuspended.reserve(ids.size());

		// Pass 2: suspend. No heap/virtual allocation must happen from here until we resume.
		for (DWORD id : ids)
		{
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
			if (h)
			{
				if (SuspendThread(h) != (DWORD)-1)
				{
					// SuspendThread is ASYNCHRONOUS - it increments the suspend count but the target
					// may keep executing user code until it next enters the kernel. Force it to have
					// actually stopped before we start patching by reading its context; GetThreadContext
					// blocks until the thread is genuinely suspended.
					CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
					GetThreadContext(h, &ctx);
					mSuspended.push_back(h);
				}
				else CloseHandle(h);
			}
		}
	}

	~ScopedThreadSuspender()
	{
		for (HANDLE h : mSuspended) { ResumeThread(h); CloseHandle(h); }
		mSuspended.clear();
	}

	ScopedThreadSuspender(const ScopedThreadSuspender&) = delete;
	ScopedThreadSuspender& operator=(const ScopedThreadSuspender&) = delete;

private:
	std::vector<HANDLE> mSuspended;
};
