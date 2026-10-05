#pragma once
#include "pch.h"
#include <TlHelp32.h>
#include <vector>
#include <mutex>

// ⚠⚠ ONE PROCESS-WIDE LOCK FOR EVERY HCM THREAD SUSPENDER (added 2026-10-05, UncapRenderSections review).
// SuspendThread is ASYNCHRONOUS. Two suspenders walking their thread lists at the same moment on different threads
// can each suspend the other - and then nothing ever resumes either of them, or the game threads they had already
// frozen: MCC hangs for good. Every ScopedThreadSuspender takes this lock BEFORE its thread snapshot and releases it
// only AFTER its last ResumeThread; UncapClusterLimit's hand-rolled suspendOthers()/resumeOthers() take it too.
// A suspender that is blocked waiting for the lock can safely be suspended by the holder (it is not suspending
// anything yet), so two suspenders can never freeze each other.
// Rules for holders: only memory work inside a window (as before) and NEVER wait for another thread while holding it.
// Recursive, so an accidental nested suspender on the same thread re-enters instead of deadlocking.
// (safetyhook freezes threads by itself while it installs/removes a hook; that path does not take this lock.)
inline std::recursive_mutex& hcmThreadSuspensionMutex()
{
	static std::recursive_mutex m;
	return m;
}

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

		// (mLock, the FIRST member, already holds hcmThreadSuspensionMutex() here - see the note at the top.)

		// Pass 1: collect the target thread ids. Nothing is suspended yet, so allocating is safe.
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te{}; te.dwSize = sizeof(te);
			if (Thread32First(snap, &te))
			{
				mSnapshotOk = true;
				do {
					if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
						mIds.push_back(te.th32ThreadID);
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
		}

		// Size handle storage NOW, so the suspend loop writes into it by index and never grows it. resize, not
		// reserve + push_back: see the Debug-iterator note above.
		mSuspended.resize(mIds.size(), nullptr);
		mSuspendedIdStore.resize(mIds.size(), 0);

		// Pass 2: suspend. No heap/virtual allocation, and no container API, from here until we resume.
		const DWORD* ids = mIds.data();
		HANDLE* out = mSuspended.data();
		DWORD* outIds = mSuspendedIdStore.data();
		mSuspendedRaw = out;      // for suspendedHandles(): captured here so the accessor never touches the vector
		mSuspendedIdsRaw = outIds;
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
					outIds[mCount] = ids[i];
					out[mCount++] = h;
				}
				else
				{
					CloseHandle(h);
					if (stillRunning(ids[i])) ++mMissed;   // a live thread we could not stop (exited ones are fine)
				}
			}
			else if (GetLastError() != ERROR_INVALID_PARAMETER) ++mMissed;   // INVALID_PARAMETER = it already exited
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

	// Read-only view of the threads this instance currently holds suspended, for callers that inspect them INSIDE
	// the window (UncapRenderSections' Rip/stack quiescence gate calls GetThreadContext on each). A raw pointer and a
	// count on purpose: no container API inside the window (see the note at the top). Every handle was opened with
	// THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT. Valid only while this instance is alive; never resume or close them.
	const HANDLE* suspendedHandles() const { return mSuspendedRaw; }
	const DWORD* suspendedIds() const { return mSuspendedIdsRaw; }   // parallel to suspendedHandles()
	size_t suspendedCount() const { return mCount; }

	// Did this window really stop every other thread the snapshot saw? False if the snapshot failed (then NOTHING
	// is suspended) or a thread that is still alive could not be opened/suspended. Threads created after the
	// snapshot are not covered - a caller that must know enumerates the live threads inside the window itself
	// (UncapRenderSections does, with NtGetNextThread). Most callers only write a few bytes and do not need it.
	bool snapshotOk() const { return mSnapshotOk; }
	size_t missedCount() const { return mMissed; }

private:
	// Inside pass 2 (bare syscalls only): is thread `id` still running? Unknown counts as running.
	static bool stillRunning(DWORD id)
	{
		HANDLE q = OpenThread(SYNCHRONIZE, FALSE, id);
		if (!q) return GetLastError() != ERROR_INVALID_PARAMETER;
		const bool running = WaitForSingleObject(q, 0) != WAIT_OBJECT_0;
		CloseHandle(q);
		return running;
	}

	// ⚠ DECLARED FIRST ON PURPOSE: members are constructed in declaration order and destroyed in reverse, so the
	// process-wide lock is taken before the constructor body (pass 1) and released after the destructor body has
	// resumed every thread (and after the vectors below are freed).
	std::unique_lock<std::recursive_mutex> mLock{ hcmThreadSuspensionMutex() };
	std::vector<DWORD> mIds;         // ⚠ a member ON PURPOSE - see the note at the top
	std::vector<HANDLE> mSuspended;
	std::vector<DWORD> mSuspendedIdStore;   // ids of the suspended threads, parallel to mSuspended (pre-sized too)
	HANDLE* mSuspendedRaw = nullptr; // mSuspended.data(), captured before pass 2 (the vector never grows after that)
	DWORD* mSuspendedIdsRaw = nullptr;
	size_t mCount = 0;               // handles actually suspended; mSuspended is pre-sized to the id count
	size_t mMissed = 0;              // live threads from the snapshot that could not be suspended
	bool mSnapshotOk = false;
};
