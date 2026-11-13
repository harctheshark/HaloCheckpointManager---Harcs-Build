#pragma once
#include "pch.h"
#include "ScopedThreadSuspender.h"
#include <memory>

// ================================================================================================================================
// QUIET WINDOW for live changes to Halo 2's per-frame render data (halo2.dll 1.3528) - 2026-11-11.
//
// WHY: the "Uncap" features (cluster limit, visibility limits, drop shadows) change WHERE the renderer keeps data it builds
// and reads within one frame - by patching the code that addresses it, or by swapping the arrays it points at. Suspending
// the other threads is not enough: a thread stopped INSIDE the frame's render work resumes with half of that frame done the
// old way and the rest done the new way (crash 2026-11-11: halo2+0x7093D7, one millisecond after Uncap Cluster Limit
// applied - the patched reader looked in the new place for an index the unpatched writer had put in the old one), or with
// the old address still in a register.
//
// WHAT: open() suspends every other thread and accepts the window only if NO thread is executing, or will return into, the
// code below - its Rip and every value on its live stack are checked (GetThreadContext / VirtualQuery are bare system
// calls, safe while everything is frozen). A stale value that happens to look like one of these addresses only costs a
// retry at another moment of the frame. The caller then does MEMORY-ONLY work and lets the window go.
//
// The ranges (RVAs). The main thread's frame is main loop sub_18067A220 -> sub_18095FEC0 -> sub_180960230 -> sub_1807E1600
// -> ... -> sub_1807EB210 (drop shadows) -> sub_1807EA5A0 (visibility) -> sub_180707F50, so a return address into the
// renderer module is on its stack for the whole of the frame's render work; the sim tick is NOT a hazard (these are
// render-only structures) except for the few object functions that touch the render-state pool.
// ================================================================================================================================
namespace H2Quiet
{
	struct Range { uint32_t lo, hi; };
	inline constexpr Range kHazards[] = {
		{ 0x7E0000, 0x7F8000 },   // the renderer: frame pipeline, visibility pass sub_1807EA5A0, drop shadows sub_1807EB210,
		                          //   render-state pool code (0x7E3B30..0x7E542C, 0x7EC100), fade manager 0x7F6FE0 / 0x7F7400,
		                          //   item bit-vector walker sub_1807F1610
		{ 0x95FEC0, 0x960038 },   // sub_18095FEC0: the main loop's render call
		{ 0x960230, 0x96041E },   // sub_180960230
		{ 0x069540, 0x069636 },   // sub_180069540: its other caller
		{ 0x847EF0, 0x8483E1 },   // sub_180847EF0: the other way into the frame pipeline (sub_1807E2D50)
		{ 0x707000, 0x70A100 },   // the visibility collections (sub_180707F50, sub_180709340, accessors, init / dispose)
		{ 0x797E00, 0x79B000 },   // the projection buffer builders
		{ 0x973DE0, 0x973F00 },   // the rasterizer scratch pool sub_180973DE0
		// the render-state pool's users outside the renderer, and the object code that calls into it
		{ 0x720A60, 0x720FB3 }, { 0x71E930, 0x71EF2E }, { 0x720740, 0x7208F0 }, { 0x721B80, 0x721D7F },
		{ 0x690590, 0x690647 }, { 0x977A90, 0x9780BB }, { 0x8D9BA0, 0x8D9C7A }, { 0x8DA380, 0x8DAA8F }, { 0x9014E0, 0x90190C },
	};

	// extraLo..extraHi: the caller's own code cave (a thread inside it must not have it changed or taken away)
	inline bool addressIsHazard(uintptr_t a, uintptr_t base, uintptr_t extraLo, uintptr_t extraHi)
	{
		if (a >= extraLo && a < extraHi) return true;
		if (a < base) return false;
		const uintptr_t rva = a - base;
		for (const Range& r : kHazards) if (rva >= r.lo && rva < r.hi) return true;
		return false;
	}

	// INSIDE a window: POD-only (SEH), no locks, no heap. h needs THREAD_GET_CONTEXT. "Cannot tell" counts as a hazard.
	inline bool threadInHazard(HANDLE h, uintptr_t base, uintptr_t extraLo, uintptr_t extraHi)
	{
		CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(h, &ctx)) return true;
		if (addressIsHazard(ctx.Rip, base, extraLo, extraHi)) return true;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery((void*)ctx.Rsp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return true;
		const uintptr_t lo = ctx.Rsp & ~(uintptr_t)7, hi = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;   // its whole live stack
		__try { for (uintptr_t p = lo; p + 8 <= hi; p += 8) if (addressIsHazard(*(const uintptr_t*)p, base, extraLo, extraHi)) return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return true; }
		return false;
	}

	// INSIDE a window: every other thread really stopped, and none of them in the hazard code
	inline bool windowIsQuiet(const ScopedThreadSuspender& s, uintptr_t base, uintptr_t extraLo, uintptr_t extraHi)
	{
		if (!s.snapshotOk() || s.missedCount() != 0) return false;
		const HANDLE* hs = s.suspendedHandles(); const size_t n = s.suspendedCount();   // raw: no container calls in the window
		for (size_t i = 0; i < n; ++i) if (threadInHazard(hs[i], base, extraLo, extraHi)) return false;
		return true;
	}

	// Tries up to maxTries windows. Returns the OPEN quiet window (every other thread suspended until it is destroyed), or
	// null when none came up (nothing is suspended). `tries` = how many windows it took. Call from a thread that holds no
	// lock the game's threads need, and do memory-only work while the window is open (see ScopedThreadSuspender.h).
	inline std::unique_ptr<ScopedThreadSuspender> open(uintptr_t base, int maxTries, int& tries, uintptr_t extraLo = 0, uintptr_t extraHi = 0)
	{
		for (tries = 1; tries <= maxTries; ++tries)
		{
			auto s = std::make_unique<ScopedThreadSuspender>();   // allocated before its constructor suspends anything
			if (windowIsQuiet(*s, base, extraLo, extraHi)) return s;
			s.reset();                                            // resumes everything, then frees
			Sleep(tries < 80 ? 1 : 2);                            // another moment of the frame next time
		}
		tries = maxTries;
		return nullptr;
	}
}
