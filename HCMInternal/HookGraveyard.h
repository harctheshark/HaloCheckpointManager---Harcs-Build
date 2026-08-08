#pragma once
#include "pch.h"
#include "safetyhook.hpp"

// ================================================================================================================
// PARKING HOOKS INSTEAD OF DESTROYING THEM, AT SHUTDOWN.
//
// Destroying a safetyhook hook does TWO things: it restores the target's original bytes, and it frees the
// trampoline page holding the relocated prologue (plus, for a mid hook, the stub that saves context and calls our
// handler). The first is required. The second is what kills the game.
//
// MEASURED, TWICE, with first-chance exception logging armed during teardown:
//
//     [thread 41852] ModuleMidHook::detach: detaching hook: HaloSimulation_tag_release.dll
//     [thread 26268] FIRST-CHANCE EXCEPTION 0xC0000005 at <no module - unmapped or freed memory>
//                    - tried to EXECUTE 0x7FFDCBB00072
//
// A GAME thread was executing inside the trampoline at the instant HCM's thread freed it. safetyhook freezes
// threads and rewrites an instruction pointer sitting in the relocated PROLOGUE, but a thread parked further in -
// in the stub, or returning through it - is not covered, and the page goes away underneath it.
//
// Downstream that reads as a crash with no HCM anywhere in the callstack: the access violation is swallowed by a
// __except above it (NVIDIA Streamline wraps the swapchain when DLSS/frame generation is on), Present returns
// E_ABORT, and UE raises LowLevelFatalError from D3D12Util.cpp. It is intermittent because it needs a thread to be
// inside the trampoline during the microsecond it is freed.
//
// So at shutdown: disable() restores the original bytes, so no new thread can enter, and the hook object is then
// parked here forever so its trampoline stays mapped for anyone already inside. The cost is a few pages leaked in
// a process that is about to lose HCM anyway - the same trade HCETriggerActivity makes for its stub page, and the
// one ~D3D12Hook makes when it abandons GPU resources rather than free memory the GPU may still be reading.
//
// ⚠ SHUTDOWN ONLY. A normal detach still destroys, because a live HCM re-attaches and must not leak a page per
// toggle.
//
// ⚠ THE RENDER-THREAD HOOKS MATTER MOST. D3D12Hook's Present / ResizeBuffers / ExecuteCommandLists detours sit on
// the hottest code in the process; the odds of a thread being inside one at teardown are far better than even.
// Those bypassed ModuleHook entirely (they assign `presentHook = {}` directly), which is why this lives in its own
// header rather than staying private to ModuleHook.cpp.
// ================================================================================================================
namespace HookGraveyard
{
	namespace detail
	{
		inline std::mutex& mutex() { static std::mutex m; return m; }

		// ⚠ HEAP-ALLOCATED AND NEVER DELETED, ON PURPOSE. A plain container would be destroyed by the CRT at
		// DLL_PROCESS_DETACH, which destroys every hook it holds, frees every trampoline, and reintroduces the
		// exact crash this exists to prevent - just a few milliseconds later, where it is even harder to see.
		// Leaking the container IS the mechanism. Only the pointer is static, and its destructor is trivial.
		inline std::vector<safetyhook::InlineHook>*& inlineHooks()
		{
			static auto* v = new std::vector<safetyhook::InlineHook>();
			return v;
		}
		inline std::vector<safetyhook::MidHook>*& midHooks()
		{
			static auto* v = new std::vector<safetyhook::MidHook>();
			return v;
		}
	}

	// Restores the original bytes and parks the hook forever. Returns false if it could not be disabled, in which
	// case the caller still owns it and must decide what to do (destroying it is the old, crash-prone behaviour -
	// but leaving a hook enabled while its handler unmaps is worse).
	inline bool park(safetyhook::InlineHook& hook)
	{
		if (!hook) return true;                       // nothing installed
		if (auto result = hook.disable(); !result) return false;

		std::scoped_lock lock(detail::mutex());
		detail::inlineHooks()->push_back(std::move(hook));
		return true;
	}

	inline bool park(safetyhook::MidHook& hook)
	{
		if (!hook) return true;
		if (auto result = hook.disable(); !result) return false;

		std::scoped_lock lock(detail::mutex());
		detail::midHooks()->push_back(std::move(hook));
		return true;
	}

	inline size_t parkedCount()
	{
		std::scoped_lock lock(detail::mutex());
		return detail::inlineHooks()->size() + detail::midHooks()->size();
	}
}
