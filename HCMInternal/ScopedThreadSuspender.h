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
//
// ⚠⚠ THE SUSPENDER USED TO BREAK THIS RULE ITSELF, on every construction (found 2026-09-22). The thread-id list
// was a LOCAL of the constructor, so it was destroyed - operator delete -> HeapFree - when the constructor
// returned, which is AFTER pass 2 has frozen every other thread. Every caller therefore started its "safe"
// window with a heap free. Both vectors are now MEMBERS: C++ destroys members after the destructor BODY has run,
// i.e. after every thread has been resumed.
//
// ⚠ Inside the window the code also avoids container APIs and iterators (raw pointers and an index only). In a
// Debug build (_ITERATOR_DEBUG_LEVEL=2) creating a vector iterator or calling push_back takes the CRT's global
// debug-iterator lock - a user-mode lock that any frozen HCM thread could be holding. Release is unaffected, but
// the difference costs nothing.
class ScopedThreadSuspender
{
public:
	ScopedThreadSuspender()
	{
		const DWORD me = GetCurrentThreadId();
		const DWORD pid = GetCurrentProcessId();

		// Pass 1: collect the target thread ids. Nothing is suspended yet, so allocating is safe.
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te{}; te.dwSize = sizeof(te);
			if (Thread32First(snap, &te))
				do {
					if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
						mIds.push_back(te.th32ThreadID);
				} while (Thread32Next(snap, &te));
			CloseHandle(snap);
		}

		// Size handle storage NOW, so the suspend loop writes into it by index and never grows it. resize, not
		// reserve + push_back: see the Debug-iterator note above.
		mSuspended.resize(mIds.size(), nullptr);

		// Pass 2: suspend. No heap/virtual allocation, and no container API, from here until we resume.
		const DWORD* ids = mIds.data();
		HANDLE* out = mSuspended.data();
		const size_t n = mIds.size();
		for (size_t i = 0; i < n; ++i)
		{
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
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
					out[mCount++] = h;
				}
				else CloseHandle(h);
			}
		}
	}

	~ScopedThreadSuspender()
	{
		// Raw pointer and index, not a range-for - see the Debug-iterator note above.
		HANDLE* hs = mSuspended.data();
		for (size_t i = 0; i < mCount; ++i) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
		mCount = 0;
		// mIds and mSuspended are freed by their member destructors, which run after this body - i.e. only once
		// every thread has been resumed.
	}

	ScopedThreadSuspender(const ScopedThreadSuspender&) = delete;
	ScopedThreadSuspender& operator=(const ScopedThreadSuspender&) = delete;

private:
	std::vector<DWORD> mIds;         // ⚠ a member ON PURPOSE - see the note at the top
	std::vector<HANDLE> mSuspended;
	size_t mCount = 0;               // handles actually suspended; mSuspended is pre-sized to the id count
};
