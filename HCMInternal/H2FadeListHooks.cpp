#include "pch.h"
#include "H2FadeListHooks.h"
#include "H2FadeLists.h"
#include "H2SafeWrite.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include <atomic>
#include <mutex>

// ============================================================================
//  WHY HOOKS, AND NOT PATCHES (2026-11-19)
//  The engine keeps a view's dynamic lights and drop shadows in "fade lists" of 32 entries, and its two list functions
//  cannot go past 32 by patching: add (sub_1807F6FE0) tracks which of a frame's inputs it has taken in a 32-bit mask,
//  and both functions are compiled unrolled around the 32-entry layout. The first Uncap Drop Shadows patched ~45
//  offsets and caps inside them - which silently changed the LIGHTS' lists too (the functions are shared) and still
//  stopped at 32. So both functions are replaced whole: h2fade::add / h2fade::process are the same logic with the
//  capacity and the limit as parameters (proven bit-identical to the engine at its own settings on 1.8 million random
//  frames - H2 Live Probe\uncap_harness\fade_diff.cpp), running on HCM's own lists.
//
//  WHAT A DETOUR DOES. The engine passes the list it wants worked on: lights qword_18165FA60 + 0x484 * view, shadows
//  qword_181660C70 + 0x484 * view (4 views each). For a kind that is switched on, add / process run on HCM's list for
//  that (kind, view) instead and the engine's own list is kept empty; for anything else the engine's function runs
//  untouched. The route is decided in add and kept for the process that follows it, so a switch between the two calls
//  of a frame can never process a list that was not filled.
//
//  WHY NO CALLER'S BUFFER CAN OVERFLOW. add removes every entry that is not among this frame's inputs before it appends,
//  so a list never holds more than the frame's inputs - and process writes one record per entry into the buffer the
//  inputs came in. Whatever the capacity, the output is never longer than the input. (Lights: the engine gathers at
//  most 128 a frame into a 128-record buffer. Shadows: 32 into 32, or what Uncap Drop Shadows relocates.)
//
//  The detours run on the game's own thread. They read three engine globals exactly as the engine's functions do: the
//  frame stamp (+0x165C154), byte_180E13485 and the pause byte ([qword_1815F4928] + 10).
// ============================================================================

namespace H2FadeListHooks
{
	namespace
	{
		constexpr uint32_t kAddRva = 0x7F6FE0, kProcessRva = 0x7F7400;
		const uint8_t kAddBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x45, 0x33, 0xE4, 0x48 };
		const uint8_t kProcessBytes[] = { 0x40, 0x53, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x89, 0x64, 0x24, 0x70, 0x33, 0xDB, 0x38, 0x1D, 0x6F, 0xC0 };
		constexpr uint32_t kListsRva[2] = { 0x165FA60, 0x1660C70 };   // lights, shadows
		constexpr uint32_t kListStride = 0x484, kListCountAt = 0x480;
		constexpr uint32_t kStampRva = 0x165C154, kEnabledRva = 0xE13485, kTimeGlobalsRva = 0x15F4928;
		constexpr int kViews = 4;
		constexpr int kEngineLimit[2] = { 3, 4 };

		std::mutex sMutex;   // acquire / release / configure
		int sUsers = 0;
		std::unique_ptr<ModuleInlineHook> sAddHook, sProcessHook;
		std::atomic<ModuleInlineHook*> sAdd{ nullptr }, sProcess{ nullptr };   // what the detours call through
		std::atomic<bool> sOn[2] = {};
		std::atomic<int> sLimit[2] = { 3, 4 };
		std::atomic<int> sListed[2] = {}, sShining[2] = {};
		std::atomic<uintptr_t> sBase{ 0 };   // the halo2.dll the lists were last recognised in

		// ---- the game thread's own state ----
		h2fade::Entry sEntries[2][kViews][kCapacity];
		int sCount[2][kViews] = {};
		bool sRouted[2][kViews] = {};

		bool inLists(uintptr_t base, uintptr_t list, int& kind, int& view)
		{
			if (!base) return false;
			for (int k = 0; k < 2; ++k)
			{
				const uintptr_t first = base + kListsRva[k];
				if (list < first || list >= first + (uintptr_t)kListStride * kViews) continue;
				if ((list - first) % kListStride) return false;
				kind = k; view = (int)((list - first) / kListStride);
				return true;
			}
			return false;
		}
		// which of HCM's lists is this? The dll is unloaded and loaded again between games, so a miss re-reads its base once.
		bool classify(uintptr_t list, int& kind, int& view)
		{
			if (inLists(sBase.load(std::memory_order_relaxed), list, kind, view)) return true;
			const uintptr_t now = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (now == sBase.load(std::memory_order_relaxed)) return false;
			sBase.store(now, std::memory_order_relaxed);
			return inLists(now, list, kind, view);
		}

		void __fastcall addDetour(void* list, const h2fade::Input* inputs, int count)
		{
			int kind = 0, view = 0;
			const bool ours = classify((uintptr_t)list, kind, view);
			if (ours && sOn[kind].load(std::memory_order_relaxed) && count >= 0 && count <= 256)
			{
				if (!sRouted[kind][view]) { sRouted[kind][view] = true; sCount[kind][view] = 0; }
				// the engine's own list stays empty while HCM's runs, so it starts clean when it is given back
				*(volatile int32_t*)((uintptr_t)list + kListCountAt) = 0;
				uint32_t taken[8];
				h2fade::add(sEntries[kind][view], sCount[kind][view], kCapacity, inputs, count,
					*(const volatile uint32_t*)(sBase.load(std::memory_order_relaxed) + kStampRva), false, taken);
				return;
			}
			if (ours) sRouted[kind][view] = false;
			if (ModuleInlineHook* h = sAdd.load()) h->getInlineHook().call<void>(list, inputs, count);
		}

		int __fastcall processDetour(void* list, h2fade::Input* output, long long limit, float step)
		{
			int kind = 0, view = 0;
			if (classify((uintptr_t)list, kind, view) && sRouted[kind][view])
			{
				const uintptr_t base = sBase.load(std::memory_order_relaxed);
				const uintptr_t time = *(const volatile uintptr_t*)(base + kTimeGlobalsRva);
				const bool paused = !*(const volatile uint8_t*)(base + kEnabledRva) || (time && *(const volatile uint8_t*)(time + 10));
				int lim = sLimit[kind].load(std::memory_order_relaxed);
				if (lim < 1) lim = 1; else if (lim > kCapacity) lim = kCapacity;
				const int n = h2fade::process(sEntries[kind][view], sCount[kind][view], output, lim, step,
					*(const volatile uint32_t*)(base + kStampRva), paused);
				if (view == 0)
				{
					int shining = 0;
					for (int i = 0; i < n; ++i) shining += output[i].target > 0.09f;
					sListed[kind].store(n, std::memory_order_relaxed);
					sShining[kind].store(shining, std::memory_order_relaxed);
				}
				return n;
			}
			if (ModuleInlineHook* h = sProcess.load()) return h->getInlineHook().call<int>(list, output, limit, step);
			return 0;
		}

		// 1 = both functions hold 1.3528's first bytes (or already carry our hooks), 0 = halo2.dll is not loaded, -1 = another build
		int recognise()
		{
			const uintptr_t base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return 0;
			const bool hooked = sAddHook && sProcessHook && sAddHook->isHookInstalled() && sProcessHook->isHookInstalled();
			if (!hooked && !(h2safe::equals(base + kAddRva, kAddBytes, sizeof(kAddBytes)) && h2safe::equals(base + kProcessRva, kProcessBytes, sizeof(kProcessBytes))))
				return -1;
			sBase.store(base, std::memory_order_relaxed);
			return 1;
		}

		// under sMutex: the hooks are wanted while any kind is on (and, once wanted, re-attach by themselves when the dll is loaded again)
		int updateAttachment()
		{
			if (!sAddHook || !sProcessHook) return 0;
			const bool want = sOn[0].load() || sOn[1].load();
			// ORDER, both ways: process first. Attaching: with only process hooked, a frame's add is the engine's, nothing
			// is routed, and the detour passes straight through. Detaching: with process already gone, a frame whose add
			// was still HCM's is finished by the engine's process on its own (emptied) list - one frame without lights /
			// shadows. The other order would let HCM's process hand out a list that this frame's add never touched.
			if (!want)
			{
				sProcessHook->setWantsToBeAttached(false);
				sAddHook->setWantsToBeAttached(false);
				return 1;
			}
			const int r = recognise();
			if (r <= 0) return r;   // another build: never hooked. Not loaded: the users ask again when a level is playing
			sProcessHook->setWantsToBeAttached(true);
			sAddHook->setWantsToBeAttached(true);
			return 1;
		}
	}

	void acquire()
	{
		std::scoped_lock lock(sMutex);
		if (sUsers++ > 0) return;
		try
		{
			auto addAt = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kAddRva });
			auto processAt = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kProcessRva });
			sAddHook = ModuleInlineHook::make(L"halo2.dll", addAt, (void*)&addDetour);
			sProcessHook = ModuleInlineHook::make(L"halo2.dll", processAt, (void*)&processDetour);
			sAdd = sAddHook.get();
			sProcess = sProcessHook.get();
		}
		catch (const std::exception& ex)
		{
			PLOG_ERROR << "H2FadeListHooks: could not create the hooks: " << ex.what();
			sAdd = nullptr; sProcess = nullptr; sAddHook.reset(); sProcessHook.reset();
		}
	}

	void release()
	{
		std::scoped_lock lock(sMutex);
		if (--sUsers > 0) return;
		sUsers = 0;
		sOn[0] = false; sOn[1] = false;
		if (sProcessHook) sProcessHook->setWantsToBeAttached(false);   // process first (see updateAttachment)
		if (sAddHook) sAddHook->setWantsToBeAttached(false);
		sAdd = nullptr; sProcess = nullptr;
		sAddHook.reset(); sProcessHook.reset();
		sListed[0] = sListed[1] = 0; sShining[0] = sShining[1] = 0;
	}

	int configure(Kind kind, bool on, int limit)
	{
		std::scoped_lock lock(sMutex);
		if (on && recognise() < 0)
		{
			PLOG_ERROR << "H2FadeListHooks: halo2.dll's list functions are not build 1.3528's - nothing hooked";
			return -1;
		}
		sLimit[kind] = on ? limit : kEngineLimit[kind];
		sOn[kind] = on && sAddHook && sProcessHook;
		if (!on) { sListed[kind] = 0; sShining[kind] = 0; }
		const int r = updateAttachment();
		PLOG_INFO << "H2FadeListHooks: " << (kind == Lights ? "lights" : "shadows") << (on ? " on, limit " : " off (") << limit << (on ? "" : ")") << " -> " << r;
		return r < 0 ? -1 : 1;
	}

	int listed(Kind kind) { return sListed[kind].load(std::memory_order_relaxed); }
	int shining(Kind kind) { return sShining[kind].load(std::memory_order_relaxed); }
}
