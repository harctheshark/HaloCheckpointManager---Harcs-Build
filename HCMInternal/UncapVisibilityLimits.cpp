#include "pch.h"
#include "UncapVisibilityLimits.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include "H2QuietWindow.h"
#include "GlobalKill.h"
#include <Windows.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <atomic>

// ============================================================================
//  Halo 2 (MCC halo2.dll 1.3528) - raise per-frame object-render budgets, crash-free.
//
//  Root cause of all prior crashes (found via the VEH log below + RE):
//   * Render-state pool uses a BUMP/ARENA allocator (sub_1806AF520) - asking it for a big
//     block overruns the fixed arena -> heap corruption. Fix: build our own fully-committed
//     VirtualAlloc datum. Arena datums are NEVER individually freed (destroy just clears a
//     flag), so repointing to our block is safe on teardown.
//   * Visibility sub-collection arrays come from the REAL game heap (sub_1800769E0) and ARE
//     freed per-map. Repointing them to VirtualAlloc made the engine free a non-heap block
//     -> heap corruption. Fix: allocate the bigger arrays from sub_1800769E0 too, so the
//     engine frees them normally.
//
//  Both structures are PER-MAP, so we (re)apply on toggle and on each Halo 2 (re)entry.
//  A vectored exception handler logs every AV + every step to %LOCALAPPDATA%\HCM_uncap_debug.log
// ============================================================================

namespace
{
	constexpr uint32_t CAM = 0x15FE020;   // camera visibility collection
	constexpr uint32_t SHA = 0x1600B30;   // shadow visibility collection
	constexpr uint32_t SUBS[] = { 0x18, 0x20, 0x28 }; // extra5486, idx(objects), extra5488 (NOT clusters@0x10)
	constexpr uint32_t COLLS[] = { CAM, SHA };
	constexpr int VIS_NEWCAP = 2048;

	constexpr uint32_t RS_PTR = 0x165C4F8;       // qword_18165C4F8 (render-state datum ptr)
	constexpr uint32_t GAME_ALLOC = 0x769E0;     // sub_1800769E0(size)->ptr (real locked game heap)
	constexpr uint32_t GAME_FREE  = 0x76E30;     // sub_180076E30(ptr)  - the matching deallocator
	constexpr int RS_NEWMAX = 2048;

	// ---------------- crash-hunter instrumentation ----------------
	struct TrackedRange { uintptr_t start; size_t size; char tag[40]; };
	struct UncapDbg
	{
		uintptr_t base = 0, modSize = 0x2A38000;
		uintptr_t rsDatum = 0; uint32_t rsMax = 0, rsElem = 0;
		TrackedRange ranges[160]; std::atomic<int> rangeCount{ 0 };
		HANDLE hLog = INVALID_HANDLE_VALUE;
		PVOID veh = nullptr;
		std::atomic<bool> inCriticalOp{ false };

		static UncapDbg& get() { static UncapDbg d; return d; }
		void openLog()
		{
			if (hLog != INVALID_HANDLE_VALUE) return;
			char path[MAX_PATH]; DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
			if (!n || n >= MAX_PATH - 32) return;
			strcat_s(path, MAX_PATH, "\\HCM_uncap_debug.log");
			hLog = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		}
		void raw(const char* s) { if (hLog != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(hLog, s, (DWORD)strlen(s), &w, nullptr); FlushFileBuffers(hLog); } }
		void logf(const char* fmt, ...)
		{
			char buf[512]; SYSTEMTIME t; GetLocalTime(&t);
			int p = wsprintfA(buf, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
			va_list ap; va_start(ap, fmt); wvsprintfA(buf + p, fmt, ap); va_end(ap);
			strcat_s(buf, "\r\n"); raw(buf); PLOG_INFO << "[UncapDbg] " << buf;
		}
		void track(uintptr_t s, size_t sz, const char* tag)
		{
			int i = rangeCount.load(); if (i >= 160) return;
			ranges[i].start = s; ranges[i].size = sz; lstrcpynA(ranges[i].tag, tag, 40); rangeCount.store(i + 1);
		}
		void clearRanges() { rangeCount.store(0); }
	};

	LONG WINAPI uncapVEH(EXCEPTION_POINTERS* ep)
	{
		if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
		UncapDbg& d = UncapDbg::get();
		if (d.hLog == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
		uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
		uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
		ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
		bool ripInModule = d.base && rip >= d.base && rip < d.base + d.modSize;
		int hit = -1; uintptr_t hitOff = 0; int rc = d.rangeCount.load();
		for (int i = 0; i < rc && i < 160; i++)
			if (fault >= d.ranges[i].start && fault < d.ranges[i].start + d.ranges[i].size) { hit = i; hitOff = fault - d.ranges[i].start; break; }
		if (!ripInModule && hit < 0) return EXCEPTION_CONTINUE_SEARCH;
		char buf[700];
		const char* op = d.inCriticalOp.load() ? " (DURING our apply/revert!)" : "";
		int p = wsprintfA(buf, "\r\n==== ACCESS VIOLATION ====%s\r\n  rip=%p", op, (void*)rip);
		if (ripInModule) p += wsprintfA(buf + p, "  halo2.dll+0x%X", (unsigned)(rip - d.base));
		p += wsprintfA(buf + p, "\r\n  %s addr=%p", rw == 1 ? "WRITE" : rw == 8 ? "EXEC" : "READ", (void*)fault);
		if (hit >= 0) p += wsprintfA(buf + p, "  >>> inside OUR buffer '%s' +0x%X (size 0x%X)", d.ranges[hit].tag, (unsigned)hitOff, (unsigned)d.ranges[hit].size);
		else p += wsprintfA(buf + p, "  (not in any tracked buffer)");
		p += wsprintfA(buf + p, "\r\n  rs datum=%p max=%u elem=%u\r\n", (void*)d.rsDatum, d.rsMax, d.rsElem);
		d.raw(buf);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	class Patcher
	{
		uintptr_t base = 0;
		void* rsActive = nullptr;   // our current VirtualAlloc render-state datum
		uintptr_t rsOriginal = 0;   // arena datum it replaced (revert, same-map only)
		std::vector<void*> rsOld;   // superseded VirtualAlloc render-state datums, freed deferred
		std::vector<void*> visOld;  // orphaned original visibility arrays, freed (game heap) deferred

		void* gameAlloc(size_t n) { return ((void* (*)(int64_t))(base + GAME_ALLOC))((int64_t)n); }
		void  gameFree(void* p)   { if (p) ((void (*)(void*))(base + GAME_FREE))(p); }
		// Free last cycle's orphaned (game-heap) originals via the engine's own deallocator. SEH-guarded backstop: if
		// we ever reach here with pointers from a torn-down map (the game bulk-frees its arena on teardown), the game
		// free would AV on decommitted memory - catch it and just drop the list instead of crashing the process.
		void  drainVisOld()
		{
			void** arr = visOld.data(); size_t n = visOld.size(); uintptr_t gf = base + GAME_FREE;
			__try { for (size_t i = 0; i < n; ++i) if (arr[i]) ((void(*)(void*))gf)(arr[i]); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			visOld.clear();
		}

		// ---- 2026-11-11: plan (allocate, threads live) -> commit (copy + repoint, inside a QUIET WINDOW) ----
		// These arrays and the render-state pool are read and written all through a frame's render work. The first version
		// copied and repointed them with the game running (no suspension at all): an entry added between the copy and the
		// repoint was lost, and a thread already holding the old array kept writing there while everything else read the
		// new one. Now the bigger blocks are allocated first (the game heap / VirtualAlloc take locks, so never inside a
		// window), and the copy + repoint happen in a window where no thread is inside the renderer (H2QuietWindow.h) - the
		// counts are read THERE, so nothing can be added in between. Logging happens after the window.
		struct SubPlan
		{
			const char* which = ""; uintptr_t subp = 0; uint32_t oldCap = 0; uint16_t cnt = 0;
			void* n[4] = {}; uintptr_t o[4] = {};
			int result = 0;   // 0 nothing to do, 1 planned, 2 committed, 3 the sub-collection changed under us (not touched)
		};
		struct RsPlan
		{
			uintptr_t d0 = 0; uint32_t maxOld = 0, elem = 0, dataOff = 0, bvOffOld = 0, newBvOff = 0, bvNew = 0, bvOld = 0, size = 0;
			void* block = nullptr;
			int result = 0;   // as SubPlan
		};

		// Threads LIVE. Allocate the 4 bigger arrays from the GAME heap (sub_1800769E0) so the engine frees them normally on
		// per-map teardown, zeroed so a read up to the raised cap never sees heap garbage.
		void planSub(uintptr_t coll, uint32_t subOff, const char* which, SubPlan& p)
		{
			UncapDbg& d = UncapDbg::get();
			p.which = which;
			p.subp = *(uintptr_t*)(base + coll + subOff);
			if (!p.subp) { d.logf("  resizeSub %s: subp NULL, skip", which); return; }
			p.oldCap = *(uint32_t*)p.subp; p.cnt = *(uint16_t*)(p.subp + 4);
			d.logf("  resizeSub %s subp=%p cap=%u cnt=%u", which, (void*)p.subp, p.oldCap, p.cnt);
			if (p.oldCap == 0 || p.oldCap >= (uint32_t)VIS_NEWCAP) { d.logf("    skip (cap)"); return; }
			if (p.cnt > p.oldCap) { d.logf("    skip (cnt>cap)"); return; }
			const size_t sz[4] = { 2 * (size_t)VIS_NEWCAP, 4 * (size_t)VIS_NEWCAP, 8 * (size_t)VIS_NEWCAP, 2 * (size_t)VIS_NEWCAP };
			for (int i = 0; i < 4; ++i) p.n[i] = gameAlloc(sz[i]);
			if (!(p.n[0] && p.n[1] && p.n[2] && p.n[3]))
			{
				for (int i = 0; i < 4; ++i) { gameFree(p.n[i]); p.n[i] = nullptr; }
				d.logf("    gameAlloc fail"); return;
			}
			for (int i = 0; i < 4; ++i) memset(p.n[i], 0, sz[i]);
			p.result = 1;
		}
		// INSIDE the quiet window: memory only.
		static void commitSub(SubPlan& p)
		{
			if (p.result != 1) return;
			const uint32_t cap = *(uint32_t*)p.subp; const uint16_t cnt = *(uint16_t*)(p.subp + 4);
			if (cap != p.oldCap || cnt > cap) { p.result = 3; return; }
			p.cnt = cnt;
			for (int i = 0; i < 4; ++i) p.o[i] = *(uintptr_t*)(p.subp + 8 + 8 * i);
			memcpy(p.n[0], (void*)p.o[0], (size_t)cnt * 2); memcpy(p.n[1], (void*)p.o[1], (size_t)cnt * 4);
			memcpy(p.n[2], (void*)p.o[2], (size_t)cnt * 8); memcpy(p.n[3], (void*)p.o[3], (size_t)cnt * 2);
			for (int i = 0; i < 4; ++i) *(uintptr_t*)(p.subp + 8 + 8 * i) = (uintptr_t)p.n[i];
			*(uint32_t*)p.subp = VIS_NEWCAP;
			p.result = 2;
		}
		// Threads LIVE again: bookkeeping, frees, log.
		void finishSub(SubPlan& p)
		{
			UncapDbg& d = UncapDbg::get();
			if (p.result == 2)
			{
				d.track((uintptr_t)p.n[0], 2 * VIS_NEWCAP, "vis A"); d.track((uintptr_t)p.n[1], 4 * VIS_NEWCAP, "vis B");
				d.track((uintptr_t)p.n[2], 8 * VIS_NEWCAP, "vis C"); d.track((uintptr_t)p.n[3], 2 * VIS_NEWCAP, "vis D");
				// the original (smaller) arrays are now orphaned - freed with the engine's own deallocator, deferred one cycle
				for (int i = 0; i < 4; ++i) visOld.push_back((void*)p.o[i]);
				d.logf("    %s resized -> cap %d (cnt %u), game-heap arrays %p/%p/%p/%p (4 originals queued for free)", p.which, VIS_NEWCAP, p.cnt, p.n[0], p.n[1], p.n[2], p.n[3]);
			}
			else if (p.result == 1 || p.result == 3)
			{
				for (int i = 0; i < 4; ++i) { gameFree(p.n[i]); p.n[i] = nullptr; }   // never installed
				d.logf("    %s NOT resized (%s)", p.which, p.result == 3 ? "it changed before the window" : "no window");
			}
		}

		// Render-state: fully-committed VirtualAlloc (the arena can't grow), repoint. Never freed by engine.
		void planRenderState(RsPlan& r)
		{
			UncapDbg& d = UncapDbg::get();
			r.d0 = *(uintptr_t*)(base + RS_PTR);
			if (!r.d0) { d.logf("  rs-live: no datum (not in a map yet)"); return; }
			r.maxOld = *(uint32_t*)(r.d0 + 32); r.elem = *(uint32_t*)(r.d0 + 36); r.dataOff = *(uint32_t*)(r.d0 + 72); r.bvOffOld = *(uint32_t*)(r.d0 + 80);
			d.rsDatum = r.d0; d.rsMax = r.maxOld; d.rsElem = r.elem;
			d.logf("  rs-live: datum=%p max=%u elem=%u dataOff=%u bvOff=%u", (void*)r.d0, r.maxOld, r.elem, r.dataOff, r.bvOffOld);
			if (r.maxOld >= (uint32_t)RS_NEWMAX) { d.logf("    skip (already big - our block)"); return; }
			if (r.elem == 0 || r.elem > 4096 || r.dataOff < 32 || r.dataOff > 256 || r.bvOffOld != r.dataOff + r.maxOld * r.elem) { d.logf("    SANITY FAIL, skip"); return; }
			r.newBvOff = r.dataOff + (uint32_t)RS_NEWMAX * r.elem;
			r.bvNew = 4 * ((RS_NEWMAX + 31) >> 5); r.bvOld = 4 * ((r.maxOld + 31) >> 5);
			r.size = r.newBvOff + r.bvNew;
			r.block = VirtualAlloc(0, r.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // zero-filled
			if (!r.block) { d.logf("    VirtualAlloc FAILED, skip"); return; }
			r.result = 1;
		}
		// INSIDE the quiet window: memory only. The pool is copied as it is NOW and the pointer swapped.
		static void commitRenderState(RsPlan& r, uintptr_t base)
		{
			if (r.result != 1) return;
			if (*(uintptr_t*)(base + RS_PTR) != r.d0 || *(uint32_t*)(r.d0 + 32) != r.maxOld || *(uint32_t*)(r.d0 + 36) != r.elem
				|| *(uint32_t*)(r.d0 + 72) != r.dataOff || *(uint32_t*)(r.d0 + 80) != r.bvOffOld) { r.result = 3; return; }
			uint8_t* const block = (uint8_t*)r.block;
			memcpy(block, (void*)r.d0, r.dataOff);
			*(uint32_t*)(block + 32) = RS_NEWMAX;
			*(uint32_t*)(block + 72) = r.dataOff;
			*(uint32_t*)(block + 80) = r.newBvOff;
			memcpy(block + r.dataOff, (void*)(r.d0 + r.dataOff), (size_t)r.maxOld * r.elem);     // the rest is already zero
			memcpy(block + r.newBvOff, (void*)(r.d0 + r.bvOffOld), r.bvOld);
			*(uintptr_t*)(base + RS_PTR) = (uintptr_t)block;
			r.result = 2;
		}
		void finishRenderState(RsPlan& r)
		{
			UncapDbg& d = UncapDbg::get();
			if (r.result == 2)
			{
				d.track((uintptr_t)r.block, r.size, "rs datum");
				if (rsActive) rsOld.push_back(rsActive);
				rsOriginal = r.d0; rsActive = r.block;
				d.rsDatum = (uintptr_t)r.block; d.rsMax = RS_NEWMAX;
				d.logf("    repointed render-state -> %p (max 2048, VirtualAlloc, committed %u)", r.block, r.size);
			}
			else if (r.result == 1 || r.result == 3)
			{
				VirtualFree(r.block, 0, MEM_RELEASE); r.block = nullptr;   // never installed
				d.logf("    render-state NOT repointed (%s)", r.result == 3 ? "it changed before the window" : "no window");
			}
		}

	public:
		// (re)apply for the current map. Idempotent + safe to call repeatedly (toggle / each map entry).
		bool apply()
		{
			UncapDbg& d = UncapDbg::get();
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;
			d.openLog(); d.base = base;
			if (!d.veh) d.veh = AddVectoredExceptionHandler(1, uncapVEH);
			d.logf("===== APPLY (base=%p) =====", (void*)base);
			for (void* p : rsOld) VirtualFree(p, 0, MEM_RELEASE);
			rsOld.clear();
			drainVisOld();   // free last cycle's orphaned visibility arrays via the engine's deallocator
			d.clearRanges();
			d.inCriticalOp.store(true);

			// 1. plan: every allocation, with the game running
			static const char* names[] = { "extra5486","idx(objects)","extra5488" };
			SubPlan subs[6]; int nSubs = 0;
			for (uint32_t c : COLLS) { int k = 0; for (uint32_t s : SUBS) planSub(c, s, names[k++], subs[nSubs++]); }
			RsPlan rs; planRenderState(rs);
			bool anything = rs.result == 1;
			for (int i = 0; i < nSubs; ++i) anything = anything || subs[i].result == 1;

			// 2. commit: copy + repoint in a window where no thread is inside the renderer
			int tries = 0; bool quiet = true;
			if (anything)
			{
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 800, tries);
				quiet = window != nullptr;
				if (quiet)
				{
					for (int i = 0; i < nSubs; ++i) commitSub(subs[i]);
					commitRenderState(rs, base);
				}
			}   // the window closes here: everything runs again

			// 3. bookkeeping, frees and the log
			for (int i = 0; i < nSubs; ++i) finishSub(subs[i]);
			finishRenderState(rs);
			d.inCriticalOp.store(false);
			if (!quiet)
			{
				d.logf("===== APPLY: no quiet window in %d tries - nothing changed =====", tries);
				throw HCMRuntimeException("Uncap Visibility Limits: could not find a safe moment to change the limits (the game never left its renderer). Nothing was changed - toggle it off and on again.");
			}
			if (anything) d.logf("===== APPLY done (quiet window after %d tries) =====", tries);
			else d.logf("===== APPLY done (nothing to do) =====");
			return true;
		}

		// toggle-off. NOTHING is swapped back while a map is running (2026-11-11): the enlarged arrays and render-state pool
		// are valid for the rest of the map, and the old pool cannot take back entries made beyond its 256 slots - the old
		// revert repointed to it anyway, with whatever was in it when the limits were raised. The stock limits return when the
		// next map builds its own structures (onMCCStateChanged applies only while the toggle is on).
		void revert()
		{
			UncapDbg& d = UncapDbg::get();
			d.logf("===== REVERT =====");
			if (GlobalKill::isKillSet())
			{
				// SHUTDOWN: touch nothing live; leave our still-valid block installed and LEAK it (and any deferred blocks).
				rsActive = nullptr; rsOriginal = 0;   // drop our handles WITHOUT freeing (leak)
				rsOld.clear();                         // ditto for deferred blocks - do not VirtualFree them
				d.clearRanges();
				d.logf("===== REVERT skipped (shutdown - left installed, leaked) =====");
				return;
			}
			// the orphaned originals were replaced in a quiet window, so nothing references them: free them now
			if (base && GetModuleHandleA("halo2.dll")) drainVisOld();
			d.logf("===== REVERT done (enlarged structures stay until the next map) =====");
		}

		// A map (re)loaded / we left the game: the game already freed last map's orphaned (game-heap) arrays as part
		// of its per-map arena teardown, so we must NOT call the game deallocator on them again (double-free -> the
		// halo2 free() AV we saw at level-select). Just drop our stale handles; the game already reclaimed the memory.
		void discardVisOldNoFree() { visOld.clear(); }

		// leaving Halo 2: the arena datum is gone; just drop our render-state block (freed next apply/dtor).
		void onLeave() { discardVisOldNoFree(); if (rsActive) rsOld.push_back(rsActive); rsActive = nullptr; rsOriginal = 0; }

		~Patcher()
		{
			// Remove our crash-hunter vectored exception handler BEFORE HCMInternal.dll unloads.
			// If it stays registered it points into the DLL, which gets unmapped right after teardown
			// completes - then the very next exception anywhere in MCC calls this now-dangling handler
			// and crashes the game. (This was the "almost fully shut down, then crashed" case.)
			UncapDbg& dbg = UncapDbg::get();
			if (dbg.veh) { RemoveVectoredExceptionHandler(dbg.veh); dbg.veh = nullptr; }

			// On shutdown, don't touch game memory or free our blocks (the render thread may still be
			// using them). revert() above already dropped our handles; leak the rest. Only free during
			// a normal (non-shutdown) teardown.
			if (GlobalKill::isKillSet())
				return;

			if (base && GetModuleHandleA("halo2.dll")) drainVisOld();   // free orphaned originals via game heap
			for (void* p : rsOld) VirtualFree(p, 0, MEM_RELEASE);
			// our render-state block may still be the one the game is using (toggle-off no longer swaps it back): then it
			// must outlive us - leak it
			const bool inUse = rsActive && base && GetModuleHandleA("halo2.dll") && *(uintptr_t*)(base + RS_PTR) == (uintptr_t)rsActive;
			if (rsActive && !inUse) VirtualFree(rsActive, 0, MEM_RELEASE);
		}
	};
} // namespace


template <GameState::Value gameT>
class UncapVisibilityLimitsImpl : public IUncapVisibilityLimitsImpl
{
private:
	GameState mGame;
	Patcher mPatcher;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	void onToggle(bool& newValue)
	{
		try
		{
			if (newValue) { if (!mPatcher.apply()) { lockOrThrow(messagesGUIWeak, m); m->addMessage("Uncap Visibility: load Halo 2 first, then toggle on."); return; } }
			else mPatcher.revert();
		}
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {} return; }
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Visibility + render-state raised (live; reapplies each map)" : "Visibility limits off - the stock limits return on the next map load"); }
		catch (HCMRuntimeException&) {}
	}

	// Per-map: reapply on (re)entering Halo 2 (structures are rebuilt each map); drop our block on leaving.
	void onMCCStateChanged(const MCCState& s)
	{
		try
		{
			if (s.currentGameState != mGame) { mPatcher.onLeave(); return; } // left Halo 2 -> drop handles (game frees its arrays)
			// Only (re)apply once we're FULLY in-game. During a save-and-quit / loading screen the game is tearing
			// down or rebuilding its visibility arrays, and touching them mid-transition is unsafe - so we wait.
			// Leaving to menu already dropped our handles via onLeave above, so the enlarged limit is naturally gone
			// until we re-apply here on the new map.
			if (s.currentPlayState != PlayState::Ingame) return;
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapVisibilityLimitsToggle->GetValue())
			{
				// This is a map (re)load: the previous map's orphaned game-heap arrays were already freed by the
				// game's own per-map teardown, so drop them WITHOUT re-freeing (apply()'s drainVisOld would otherwise
				// double-free them -> the level-select crash).
				mPatcher.discardVisOldNoFree();
				mPatcher.apply();
			}
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "UncapVisibilityLimits MCCState: " << ex.what(); }
	}

public:
	UncapVisibilityLimitsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapVisibilityLimitsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}
	~UncapVisibilityLimitsImpl() { try { mPatcher.revert(); } catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); } }
};


UncapVisibilityLimits::UncapVisibilityLimits(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<UncapVisibilityLimitsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException("UncapVisibilityLimits not impl for this game");
	}
}
UncapVisibilityLimits::~UncapVisibilityLimits() { PLOG_DEBUG << "~" << getName(); }
