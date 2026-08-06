#pragma once
#include "pch.h"
#include <shared_mutex>   // not pulled in by pch.h
#include <chrono>

// ================================================================================================================
// IMAGE RESIDENCY GUARD - "is any thread still executing code inside HCMInternal.dll?"
//
// WHY THIS EXISTS. HCMInternal is injected, installs detours all over the host process, and is then
// FreeLibrary'd when HCM closes. If any thread is still inside one of those detours at the instant the image is
// unmapped, the next instruction it executes is unmapped memory. The Windows Application event log on this
// machine records that happening at least 23 times - faulting module "HCMInternal.dll_unloaded", exception
// 0xC0000005, across BOTH Halo Campaign Evolved and MCC, with fifteen-plus MCC occurrences at one identical
// fault offset (i.e. one specific site, not diffuse bad luck). This is a measured, reproducing bug, not a
// theoretical one.
//
// Until now the ONLY thing between "a thread is in our code" and "the image is gone" was a Sleep(200) in
// dllmain. That is a guess, not a guarantee: a detour that blocks - and several do, on the loader lock, on a
// game mutex, or simply by calling into the game - can easily outlive it.
//
// THE CONTRACT
//   * Every hook entry point that runs HCM code takes a SHARED lock as its FIRST statement, for its whole body.
//   * dllmain takes the EXCLUSIVE lock after RealMain has returned (so every hook has been removed and no new
//     entrant is possible) and before FreeLibraryAndExitThread.
//   * The lock therefore answers exactly one question: has everyone left?
//
// ⚠ WHAT THIS CANNOT CLOSE. A thread that has decremented the shared count but has not yet executed its
// final `ret` is still inside our image for a few instructions. No counter can cover its own release. That
// residual is why dllmain keeps a short sleep AFTER the drain - the drain removes the unbounded wait (a
// blocked detour), and the sleep covers the bounded one (a handful of instructions).
//
// ⚠ RE-ENTRANCY. The WndProc is re-entrant on the same thread (the present path pumps messages). MSVC's
// shared_mutex is an SRWLOCK and is NOT recursive: a nested shared acquire deadlocks the moment a writer
// queues. ScopedImageResidency therefore uses a thread_local depth counter and only touches the mutex at
// depth 0.
//
// ⚠ LIFETIME. The mutex is a function-local static, so it is constructed on first use and never destroyed
// before the image is - a namespace-scope object could be destroyed by CRT teardown while a detour still
// wanted it.
// ================================================================================================================
namespace ImageResidency
{
	inline std::shared_mutex& guard()
	{
		static std::shared_mutex g;
		return g;
	}

	inline thread_local int tlsDepth = 0;

	// Shared-locks the guard for the lifetime of the object, unless this thread already holds it.
	class ScopedImageResidency
	{
	public:
		ScopedImageResidency()
		{
			mOutermost = (tlsDepth == 0);
			++tlsDepth;
			if (mOutermost) guard().lock_shared();
		}

		~ScopedImageResidency()
		{
			--tlsDepth;
			if (mOutermost) guard().unlock_shared();
		}

		ScopedImageResidency(const ScopedImageResidency&) = delete;
		ScopedImageResidency& operator=(const ScopedImageResidency&) = delete;

	private:
		bool mOutermost = false;
	};

	// Blocks until no thread is inside a guarded entry point. Call ONLY after every hook has been removed,
	// otherwise a new entrant can arrive while we wait. Bounded: if a detour is genuinely wedged we would
	// rather unload late than never, so the caller is told whether the drain succeeded.
	inline bool drain(unsigned timeoutMilliseconds)
	{
		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(timeoutMilliseconds);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (guard().try_lock())
			{
				guard().unlock();
				return true;
			}
			Sleep(5);
		}
		return false;
	}
}
