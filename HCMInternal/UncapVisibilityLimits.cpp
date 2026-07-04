#include "pch.h"
#include "UncapVisibilityLimits.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
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
		void  drainVisOld()       { for (void* p : visOld) gameFree(p); visOld.clear(); }

		// Grow one visibility sub-collection: allocate the 4 bigger arrays from the GAME heap
		// (sub_1800769E0) so the engine frees them normally on per-map teardown. Idempotent.
		// The original (smaller) arrays become orphaned; the engine no longer references them.
		void resizeSub(uintptr_t coll, uint32_t subOff, const char* which)
		{
			UncapDbg& d = UncapDbg::get();
			uintptr_t subp = *(uintptr_t*)(base + coll + subOff);
			if (!subp) { d.logf("  resizeSub %s: subp NULL, skip", which); return; }
			uint32_t oldCap = *(uint32_t*)subp; uint16_t cnt = *(uint16_t*)(subp + 4);
			d.logf("  resizeSub %s subp=%p cap=%u cnt=%u", which, (void*)subp, oldCap, cnt);
			if (oldCap == 0 || oldCap >= (uint32_t)VIS_NEWCAP) { d.logf("    skip (cap)"); return; }
			if (cnt > oldCap) { d.logf("    skip (cnt>cap)"); return; }
			uintptr_t oA = *(uintptr_t*)(subp + 8), oB = *(uintptr_t*)(subp + 0x10), oC = *(uintptr_t*)(subp + 0x18), oD = *(uintptr_t*)(subp + 0x20);
			void* nA = gameAlloc(2 * VIS_NEWCAP); void* nB = gameAlloc(4 * VIS_NEWCAP); void* nC = gameAlloc(8 * VIS_NEWCAP); void* nD = gameAlloc(2 * VIS_NEWCAP);
			if (!(nA && nB && nC && nD)) { d.logf("    gameAlloc fail"); return; }
			d.track((uintptr_t)nA, 2 * VIS_NEWCAP, "vis A"); d.track((uintptr_t)nB, 4 * VIS_NEWCAP, "vis B");
			d.track((uintptr_t)nC, 8 * VIS_NEWCAP, "vis C"); d.track((uintptr_t)nD, 2 * VIS_NEWCAP, "vis D");
			memcpy(nA, (void*)oA, (size_t)cnt * 2); memcpy(nB, (void*)oB, (size_t)cnt * 4); memcpy(nC, (void*)oC, (size_t)cnt * 8); memcpy(nD, (void*)oD, (size_t)cnt * 2);
			*(uintptr_t*)(subp + 8) = (uintptr_t)nA; *(uintptr_t*)(subp + 0x10) = (uintptr_t)nB; *(uintptr_t*)(subp + 0x18) = (uintptr_t)nC; *(uintptr_t*)(subp + 0x20) = (uintptr_t)nD;
			*(uint32_t*)subp = VIS_NEWCAP;
			// the original (smaller) arrays are now orphaned - free them with the engine's own deallocator,
			// deferred one cycle (they're safe to free once nothing references them; bounded, never accumulates).
			visOld.push_back((void*)oA); visOld.push_back((void*)oB); visOld.push_back((void*)oC); visOld.push_back((void*)oD);
			d.logf("    resized -> cap %d, game-heap arrays %p/%p/%p/%p (4 originals queued for free)", VIS_NEWCAP, nA, nB, nC, nD);
		}

		// Render-state: fully-committed VirtualAlloc (the arena can't grow), repoint. Never freed by engine.
		void resizeRenderStateLive()
		{
			UncapDbg& d = UncapDbg::get();
			uintptr_t d0 = *(uintptr_t*)(base + RS_PTR);
			if (!d0) { d.logf("  rs-live: no datum (not in a map yet)"); return; }
			uint32_t maxOld = *(uint32_t*)(d0 + 32), elem = *(uint32_t*)(d0 + 36), dataOff = *(uint32_t*)(d0 + 72), bvOffOld = *(uint32_t*)(d0 + 80);
			d.rsDatum = d0; d.rsMax = maxOld; d.rsElem = elem;
			d.logf("  rs-live: datum=%p max=%u elem=%u dataOff=%u bvOff=%u", (void*)d0, maxOld, elem, dataOff, bvOffOld);
			if (maxOld >= (uint32_t)RS_NEWMAX) { d.logf("    skip (already big - our block)"); return; }
			if (elem == 0 || elem > 4096 || dataOff < 32 || dataOff > 256 || bvOffOld != dataOff + maxOld * elem) { d.logf("    SANITY FAIL, skip"); return; }
			uint32_t newBvOff = dataOff + (uint32_t)RS_NEWMAX * elem;
			uint32_t bvNew = 4 * ((RS_NEWMAX + 31) >> 5), bvOld = 4 * ((maxOld + 31) >> 5);
			uint32_t size = newBvOff + bvNew;
			void* block = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (!block) { d.logf("    VirtualAlloc FAILED, skip"); return; }
			d.track((uintptr_t)block, size, "rs datum");
			memcpy(block, (void*)d0, dataOff);
			*(uint32_t*)((uintptr_t)block + 32) = RS_NEWMAX;
			*(uint32_t*)((uintptr_t)block + 72) = dataOff;
			*(uint32_t*)((uintptr_t)block + 80) = newBvOff;
			memcpy((uint8_t*)block + dataOff, (void*)(d0 + dataOff), (size_t)maxOld * elem);
			memset((uint8_t*)block + dataOff + (size_t)maxOld * elem, 0, (size_t)(RS_NEWMAX - maxOld) * elem);
			memcpy((uint8_t*)block + newBvOff, (void*)(d0 + bvOffOld), bvOld);
			memset((uint8_t*)block + newBvOff + bvOld, 0, bvNew - bvOld);
			if (rsActive) rsOld.push_back(rsActive);
			rsOriginal = d0; rsActive = block;
			*(uintptr_t*)(base + RS_PTR) = (uintptr_t)block;
			d.rsDatum = (uintptr_t)block; d.rsMax = RS_NEWMAX;
			d.logf("    repointed render-state -> %p (max 2048, VirtualAlloc, committed %u)", block, size);
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
			static const char* names[] = { "extra5486","idx(objects)","extra5488" };
			for (uint32_t c : COLLS) { int k = 0; for (uint32_t s : SUBS) resizeSub(c, s, names[k++]); }
			resizeRenderStateLive();
			d.inCriticalOp.store(false);
			d.logf("===== APPLY done =====");
			return true;
		}

		// toggle-off: put the render-state pointer back to the arena datum (same map). Visibility arrays
		// stay enlarged - harmless and the engine frees them correctly on teardown.
		void revert()
		{
			UncapDbg& d = UncapDbg::get();
			d.logf("===== REVERT =====");
			d.inCriticalOp.store(true);

			// SHUTDOWN path: the game's render thread is live and dereferences the render-state pointer
			// every frame. Repointing it back to the original arena datum races that thread, AND the
			// original may have been freed/reused by the game since we captured it (observed: render
			// thread read 0xFFFF... mid-revert -> game crash). During teardown we therefore touch NOTHING
			// live - leave our still-valid VirtualAlloc block installed and LEAK it (and any deferred
			// blocks). The game keeps a consistent render-state; the block persists fine after HCM unloads.
			if (GlobalKill::isKillSet())
			{
				rsActive = nullptr; rsOriginal = 0;   // drop our handles WITHOUT freeing (leak)
				rsOld.clear();                         // ditto for deferred blocks - do not VirtualFree them
				d.inCriticalOp.store(false); d.clearRanges();
				d.logf("===== REVERT skipped (shutdown - left installed, leaked) =====");
				return;
			}

			if (base && GetModuleHandleA("halo2.dll"))
			{
				// Repoint the render-state pointer back to the arena datum with other threads
				// suspended: the render thread reads this pointer every frame, so swapping it
				// underneath a running thread can crash the game. ONLY the pointer write goes inside
				// the suspend window - the game-heap frees (drainVisOld -> game deallocator) must stay
				// OUTSIDE it, since a suspended thread could hold the game heap lock. (ScopedThreadSuspender.)
				{
					ScopedThreadSuspender suspend;
					if (rsActive && rsOriginal && *(uintptr_t*)(base + RS_PTR) == (uintptr_t)rsActive)
						*(uintptr_t*)(base + RS_PTR) = rsOriginal;
				}
				drainVisOld();   // free orphaned originals now (collection no longer references them)
			}
			if (rsActive) rsOld.push_back(rsActive);
			rsActive = nullptr; rsOriginal = 0;
			d.inCriticalOp.store(false); d.clearRanges();
			d.logf("===== REVERT done =====");
		}

		// leaving Halo 2: the arena datum is gone; just drop our render-state block (freed next apply/dtor).
		void onLeave() { if (rsActive) rsOld.push_back(rsActive); rsActive = nullptr; rsOriginal = 0; }

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
			if (rsActive) VirtualFree(rsActive, 0, MEM_RELEASE);
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
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Visibility + render-state raised (live; reapplies each map)" : "Visibility limits restored"); }
		catch (HCMRuntimeException&) {}
	}

	// Per-map: reapply on (re)entering Halo 2 (structures are rebuilt each map); drop our block on leaving.
	void onMCCStateChanged(const MCCState& s)
	{
		try
		{
			if (s.currentGameState != mGame) { mPatcher.onLeave(); return; }
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapVisibilityLimitsToggle->GetValue()) mPatcher.apply();
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
