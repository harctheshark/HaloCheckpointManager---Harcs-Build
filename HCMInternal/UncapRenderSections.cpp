#include "pch.h"
#include "UncapRenderSections.h"
#include "UncapRenderSectionsTable.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <array>
#include <vector>

// See UncapRenderSections.h for what this does and why. The procedure follows spec.md "HCM runtime apply / revert"
// (Documents\Halo Mod And Tools\H2 Uncap Render Sections\), which is authoritative; the numbered steps below are its.
//
// ⚠ RULES FOR EVERY "INSIDE THE WINDOW" BLOCK (between constructing a ScopedThreadSuspender and destroying it):
//   no heap or virtual allocation/free, no VirtualProtect, no logging, no container API, no exceptions, no
//   function-local static initialisation. Only plain memory reads/writes plus bare syscalls that take no user-mode
//   lock (GetThreadContext, VirtualQuery, NtGetNextThread, GetThreadId, WaitForSingleObject(h, 0), CloseHandle on a
//   thread handle). Everything that can fault runs in a POD-only __try helper (C2712), and every precondition is
//   re-checked under suspension before a single byte is written.
//
// THREADING: every apply/revert runs on ONE worker thread owned by the Impl. The event handlers only record what was
// asked for and wake it - they never patch on the caller's thread, because the toggle event also fires on the
// render (Present) thread when a preset loads, and the MCC state event fires on MCC's own thread; a multi-second
// gate loop or the revert's camera-pass wait must never run on either.

namespace
{
	namespace URT = UncapRenderSectionsTable;
	using URT::FieldKind;

	constexpr int       kApplyAttempts = 500;      // quiescence-gate tries for an apply (spec)
	constexpr int       kRevertAttempts = 1000;    // quiescence-gate tries for revert phase 2 (spec)
	constexpr int       kSimpleWindowAttempts = 50;// tries for a COMPLETE window for the cap-only writes and the free scan
	constexpr ULONGLONG kResetWaitMs = 2000;       // revert phase 2a: how long to wait for a camera-pass reset
	constexpr ULONGLONG kFreeGraceMs = 5000;       // a superseded block is never freed sooner than this after its revert
	constexpr uintptr_t kRefMargin = 0x1000;       // the free scan also treats addresses this close to a block as references
	constexpr uintptr_t kNearWindow = 0x70000000;  // allocNear search window (spec)
	constexpr size_t    kRowCount = URT::kRowCount;
	constexpr size_t    kPageCount = URT::kCodePageCount;
	constexpr size_t    kPageSize = 0x1000;

	constexpr const char* kOnMessage = "Uncap Render Sections on: Halo 2 can now draw 4096 render sections a frame (stock 850)";
	constexpr const char* kOffMessage = "Uncap Render Sections off: back to the stock 850 render sections";
	constexpr const char* kPendingMessage = "Uncap Render Sections: the cap is back at the stock 850 (safe to keep playing), but the "
		"renderer never paused long enough to move its arrays back. It finishes turning off by itself at the next level load "
		"(or toggle it on and off again).";
	constexpr const char* kTooDenseMessage = "Uncap Render Sections: the cap is back at the stock 850 (safe to keep playing), but too "
		"many render sections are in view to move the arrays back safely right now. Look at a quieter area (or the sky) and "
		"toggle it on and off again - it also finishes by itself at the next level load.";
	constexpr const char* kNotRenderingMessage = "Uncap Render Sections: the cap is back at the stock 850 (safe). Halo 2 is not "
		"drawing the world right now, so it finishes turning off by itself when you are back in game.";
	constexpr const char* kBusyMessage = "Uncap Render Sections: the render thread never paused long enough - nothing was changed. "
		"Toggle it off and on again to retry.";
	constexpr const char* kBakedMessage = "Uncap Render Sections: this halo2.dll already has Uncap Render Sections baked in - "
		"nothing to do (HCM leaves a baked dll alone)";
	constexpr const char* kUnrecognisedMessage = "Uncap Render Sections: unrecognised modified halo2.dll (its render-section "
		"code points inside the dll but does not match the baked uncap) - nothing was changed";
	constexpr const char* kAdoptedOnMessage = "Uncap Render Sections is still on from an earlier HCM session - the checkbox now "
		"shows it (untick it to turn it off)";

	// ---- compile-time checks on the GENERATED table: a spec revision that breaks an assumption fails the build ----
	constexpr size_t countKind(FieldKind k) { size_t n = 0; for (size_t i = 0; i < kRowCount; ++i) if (URT::kRows[i].kind == k) ++n; return n; }
	constexpr size_t findKind(FieldKind k) { for (size_t i = 0; i < kRowCount; ++i) if (URT::kRows[i].kind == k) return i; return kRowCount; }
	constexpr size_t findRva(uint32_t rva) { for (size_t i = 0; i < kRowCount; ++i) if (URT::kRows[i].rva == rva) return i; return kRowCount; }
	constexpr bool pageListed(uint32_t page) { for (size_t i = 0; i < kPageCount; ++i) if (URT::kCodePages[i] == page) return true; return false; }
	constexpr bool rowsWellFormed()
	{
		for (size_t i = 0; i < kRowCount; ++i)
		{
			const URT::Row& r = URT::kRows[i];
			if (r.len == 0 || r.len > sizeof(r.stock)) return false;
			if (!(r.fieldLen == 4 || (r.fieldLen == 1 && r.kind == FieldKind::Rel8))) return false;
			if (r.fieldOff + r.fieldLen > r.len) return false;
			if (!pageListed((r.rva + r.fieldOff) & ~0xFFFu) || !pageListed((r.rva + r.fieldOff + r.fieldLen - 1) & ~0xFFFu)) return false;
		}
		return true;
	}
	constexpr size_t kCapRow = findKind(FieldKind::Cap);
	constexpr size_t kRel8Row = findKind(FieldKind::Rel8);
	constexpr size_t kAdoptRow = findRva(URT::kAdoptLeaRva);
	static_assert(countKind(FieldKind::Cap) == 1 && countKind(FieldKind::Rel8) == 1, "need exactly one cap row and one companion row");
	static_assert(kAdoptRow < kRowCount, "the adopt lea (0x7ED887) is not in the table");
	static_assert(URT::kRows[kAdoptRow].kind == FieldKind::Rip && URT::kRows[kAdoptRow].k == 0 && URT::kRows[kAdoptRow].fieldLen == 4,
		"the adopt lea must be a rip-relative lea of block+0");
	static_assert(rowsWellFormed(), "a generated row is malformed, or its field page is missing from kCodePages");
	static_assert(URT::kRows[kCapRow].k == URT::kN && URT::kN + URT::kSlack - 1 <= 0x7FFF, "cap must be N, and N+slack-1 must fit the int16 section index");
	static_assert(URT::kEntries == URT::kN + URT::kSlack && URT::kBlockOffExt == URT::kSectionStride * URT::kEntries
		&& URT::kBlockOffSii == URT::kBlockOffExt + URT::kEntries && URT::kBlockSize == URT::kBlockOffSii + URT::kEntries, "block layout");
	static_assert(URT::kCopyInEntries == URT::kStockCap + URT::kSlack && URT::kCopyInEntries <= URT::kEntries, "copy-in range");
	static_assert(URT::kCopyBackEntries == URT::kStockCap
		&& URT::kOldSectionsRva + URT::kSectionStride * URT::kCopyBackEntries <= URT::kCountRva, "copy-back must not reach the counts");
	static_assert(URT::kRevertCameraMax + URT::kSlack == URT::kStockCap, "revert gate bound");

	// The quiescence gate is only sound if every row a SUSPENDED thread could execute against a stale base register lies
	// inside a hazard range: each sections-base-relative row (DataRel/BlockConst/ImmDiv4), the cap, the companion and
	// the three leas that load those bases (0x7ED887 lea r15 in sub_1807ED850, 0x7EDF84 lea rdx in sub_1807EDF80,
	// 0x7ED7AD sub_1807ED7A0's memcpy-destination lea). Mirrors the generator's check (gen_hcm_table.py), so a spec
	// revision that adds such a row outside the listed functions, or narrows a range, fails the build too.
	constexpr uint32_t kBaseLeaRvas[] = { 0x7ED887, 0x7EDF84, 0x7ED7AD };
	constexpr bool isBaseLea(uint32_t rva) { for (uint32_t b : kBaseLeaRvas) if (b == rva) return true; return false; }
	constexpr bool spanInHazard(uint32_t lo, uint32_t hi)
	{
		for (size_t i = 0; i < URT::kHazardCount; ++i) if (URT::kHazards[i].lo <= lo && hi <= URT::kHazards[i].hi) return true;
		return false;
	}
	constexpr bool hazardCoverageOk()
	{
		for (uint32_t b : kBaseLeaRvas)
		{
			const size_t i = findRva(b);
			if (i >= kRowCount || URT::kRows[i].kind != FieldKind::Rip) return false;
		}
		for (size_t i = 0; i < kRowCount; ++i)
		{
			const URT::Row& r = URT::kRows[i];
			const bool needs = r.kind == FieldKind::DataRel || r.kind == FieldKind::BlockConst || r.kind == FieldKind::ImmDiv4
				|| r.kind == FieldKind::Cap || r.kind == FieldKind::Rel8 || isBaseLea(r.rva);
			if (needs && !spanInHazard(r.rva, r.rva + r.len)) return false;
		}
		return true;
	}
	static_assert(hazardCoverageOk(), "a base-relative row, the cap, the companion or a base-loading lea lies outside every hazard range");

	// ============================================================================================ field formulas
	struct FieldPlan
	{
		uintptr_t addr;      // B + rva + fieldOff
		uint8_t   len;       // 4, or 1 for the companion rel8
		uint8_t   ours[4];   // our value for this (B, S)
		uint8_t   stock[4];  // the stock field
	};
	using Plan = std::array<FieldPlan, kRowCount>;

	// int64 so a value outside int32 is SEEN, not wrapped. False = not exact (ImmDiv4 remainder).
	bool fieldValue(const URT::Row& r, uintptr_t B, uintptr_t S, int64_t& out)
	{
		const int64_t b = (int64_t)B, s = (int64_t)S, k = (int64_t)r.k;
		switch (r.kind)
		{
		case FieldKind::Rip:        out = (s + k) - (b + (int64_t)r.rva + (int64_t)r.len); return true;
		case FieldKind::ImageRva:   out = (s - b) + k; return true;
		case FieldKind::DataRel:    out = (b + k) - s; return true;
		case FieldKind::BlockConst: out = k; return true;
		case FieldKind::ImmDiv4:    { const int64_t d = (b + k) - s; if (d % 4 != 0) return false; out = d / 4; return true; }
		case FieldKind::Cap:        out = k; return true;
		case FieldKind::Rel8:       out = k; return true;
		}
		return false;
	}

	// Step (3): every row's field for (B, S), each asserted to fit its type. Allocates nothing.
	bool buildPlan(uintptr_t B, uintptr_t S, Plan& plan, std::string* why)
	{
		for (size_t i = 0; i < kRowCount; ++i)
		{
			const URT::Row& r = URT::kRows[i];
			FieldPlan& f = plan[i];
			f.addr = B + r.rva + r.fieldOff;
			f.len = r.fieldLen;
			memset(f.ours, 0, sizeof(f.ours));
			memset(f.stock, 0, sizeof(f.stock));
			memcpy(f.stock, r.stock + r.fieldOff, r.fieldLen);
			int64_t v = 0;
			const bool exact = fieldValue(r, B, S, v);
			const bool fits = r.fieldLen == 4 ? (v >= INT32_MIN && v <= INT32_MAX) : (v >= 0 && v <= 0x7F);
			if (!exact || !fits)
			{
				if (why) *why = std::format("{} (+0x{:X}): field value {} does not fit (block 0x{:X}, module 0x{:X})", r.id, r.rva, v, S, B);
				return false;
			}
			if (r.fieldLen == 4) { const int32_t v32 = (int32_t)v; memcpy(f.ours, &v32, 4); }
			else f.ours[0] = (uint8_t)v;
		}
		return true;
	}

	// ============================================================================================ SEH helpers (POD only)
	bool sehCopy(void* dst, const void* src, size_t n)
	{
		__try { memcpy(dst, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehRead32(uintptr_t addr, int32_t* out)
	{
		__try { *out = *(const volatile int32_t*)addr; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehSizeOfImage(uintptr_t B, uint32_t* out)
	{
		__try
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)B;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(B + (uint32_t)dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
			*out = nt->OptionalHeader.SizeOfImage;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehWriteField(const FieldPlan* f, bool ours)
	{
		__try { memcpy((void*)f->addr, ours ? f->ours : f->stock, f->len); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// ============================================================================================ lock-free checks
	// VirtualQuery and GetThreadContext are bare syscalls (no user-mode lock), so everything here is safe INSIDE the window.
	bool protWritable(DWORD p)
	{
		if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
		p &= 0xFF;
		return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
	}

	bool rangeWritable(uintptr_t a, size_t n)
	{
		const uintptr_t end = a + n;
		while (a < end)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !protWritable(mbi.Protect)) return false;
			a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		}
		return true;
	}

	// The halo2.dll instance at B is still mapped (MCC can unload a game dll while we wait).
	bool moduleMapped(uintptr_t B)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		return VirtualQuery((LPCVOID)(B + URT::kAdoptLeaRva), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
			&& mbi.Type == MEM_IMAGE && (uintptr_t)mbi.AllocationBase == B;
	}

	bool codePagesWritable(uintptr_t B)
	{
		for (size_t i = 0; i < kPageCount; ++i)
			if (!rangeWritable(B + URT::kCodePages[i], kPageSize)) return false;
		return true;
	}

	bool copyBackTargetsWritable(uintptr_t B)
	{
		return rangeWritable(B + URT::kOldSectionsRva, (size_t)URT::kSectionStride * URT::kCopyBackEntries)
			&& rangeWritable(B + URT::kOldExtRva, URT::kCopyBackEntries)
			&& rangeWritable(B + URT::kOldSiiRva, URT::kCopyBackEntries);
	}

	// Is S the start of a private read-write allocation big enough to be one of our blocks?
	bool blockLooksOurs(uintptr_t S)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		return VirtualQuery((LPCVOID)S, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
			&& (uintptr_t)mbi.AllocationBase == S && (mbi.Protect & 0xFF) == PAGE_READWRITE && mbi.RegionSize >= URT::kBlockSize;
	}

	// Row i's whole instruction equals stock (plan == nullptr) or stock with our field for `plan`.
	bool insnIs(uintptr_t B, size_t i, const Plan* plan)
	{
		const URT::Row& r = URT::kRows[i];
		uint8_t want[sizeof(r.stock)];
		uint8_t now[sizeof(r.stock)];
		memcpy(want, r.stock, r.len);
		if (plan) memcpy(want + r.fieldOff, (*plan)[i].ours, r.fieldLen);
		return sehCopy(now, (const void*)(B + r.rva), r.len) && memcmp(now, want, r.len) == 0;
	}

	bool allRowsStock(uintptr_t B)
	{
		for (size_t i = 0; i < kRowCount; ++i) if (!insnIs(B, i, nullptr)) return false;
		return true;
	}

	bool allRowsOurs(uintptr_t B, const Plan& plan)   // fully applied (cap N)
	{
		for (size_t i = 0; i < kRowCount; ++i) if (!insnIs(B, i, &plan)) return false;
		return true;
	}

	bool rowsOursCapStock(uintptr_t B, const Plan& plan)   // revert phase 1 done (cap back at 850)
	{
		for (size_t i = 0; i < kRowCount; ++i) if (!insnIs(B, i, i == kCapRow ? nullptr : &plan)) return false;
		return true;
	}

	// Revert gate: every section index this frame (camera indices plus each light-cache replay cam..cam+31) is < 850.
	bool countsAllowCopyBack(uintptr_t B)
	{
		const int32_t cam = *(const volatile int32_t*)(B + URT::kCameraCountRva);
		const int32_t cnt = *(const volatile int32_t*)(B + URT::kCountRva);
		return cam >= 0 && cam <= (int32_t)URT::kRevertCameraMax && cnt >= 0 && cnt <= (int32_t)URT::kStockCap;
	}

	// ============================================================================================ quiescence gate
	struct GateStats
	{
		int attempts = 0, ripHits = 0, stackHits = 0, stackUnknown = 0, stackFaults = 0, contextFails = 0, notReady = 0, countsHigh = 0;
		int unsuspended = 0;     // the snapshot failed, a live thread could not be suspended, or one appeared since
		int censusUnknown = 0;   // the live-thread census could not run (the window was still used)
	};

	constexpr uint32_t hazardLo() { uint32_t v = 0xFFFFFFFFu; for (size_t i = 0; i < URT::kHazardCount; ++i) if (URT::kHazards[i].lo < v) v = URT::kHazards[i].lo; return v; }
	constexpr uint32_t hazardHi() { uint32_t v = 0; for (size_t i = 0; i < URT::kHazardCount; ++i) if (URT::kHazards[i].hi > v) v = URT::kHazards[i].hi; return v; }
	constexpr uint32_t kHazardLo = hazardLo();   // one compare rejects almost every stack qword
	constexpr uint32_t kHazardHi = hazardHi();
	static_assert(URT::kHazardCount > 0 && kHazardLo < kHazardHi, "hazard ranges");

	bool inHazard(uint64_t a, uintptr_t B)
	{
		const uint64_t r = a - (uint64_t)B;   // wraps huge for a < B
		if (r - kHazardLo >= (uint64_t)(kHazardHi - kHazardLo)) return false;
		for (size_t i = 0; i < URT::kHazardCount; ++i)
			if (r >= URT::kHazards[i].lo && r < URT::kHazards[i].hi) return true;
		return false;
	}

	// Scans [lo, hi) qword by qword IN PLACE, upward only. 1 = a hazard address found, 0 = clean, -1 = faulted (the
	// bound below keeps it inside committed memory, so a fault is not expected; it counts as a hit).
	int sehScanStack(uintptr_t lo, uintptr_t hi, uintptr_t B)
	{
		__try
		{
			for (const volatile uint64_t* q = (const volatile uint64_t*)lo; (uintptr_t)q + 8 <= hi; ++q)
				if (inHazard(*q, B)) return 1;
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	// INSIDE the window: [Rsp & ~7, top of the committed region holding Rsp) - the thread's whole live stack. Never
	// reads below Rsp (touching another thread's guard page strips PAGE_GUARD and breaks its stack growth), and never
	// unwinds (RtlLookupFunctionEntry/RtlVirtualUnwind can take ntdll locks a suspended thread may hold).
	bool liveStack(uint64_t rspIn, uintptr_t& lo, uintptr_t& hi)
	{
		lo = (uintptr_t)rspIn & ~(uintptr_t)7;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery((LPCVOID)lo, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
			return false;
		hi = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;   // = NT_TIB.StackBase for a normal stack
		return true;
	}

	// INSIDE the window. True = this suspended thread is executing, or will return into, one of the hazard functions
	// (it could hold the old/new sections base in a register, or be part-way through writing an entry). Unknown = true.
	// The WHOLE live stack is scanned with no qword cap: callee trees under sub_1807ED850's late calls reach a
	// 0x80068-byte frame, so no fixed depth is provably enough (spec).
	bool threadInHazard(HANDLE h, uintptr_t B, GateStats& st)
	{
		alignas(16) CONTEXT ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (!GetThreadContext(h, &ctx)) { ++st.contextFails; return true; }
		if (inHazard(ctx.Rip, B)) { ++st.ripHits; return true; }
		uintptr_t lo = 0, hi = 0;
		if (!liveStack(ctx.Rsp, lo, hi)) { ++st.stackUnknown; return true; }
		const int r = sehScanStack(lo, hi, B);
		if (r > 0) { ++st.stackHits; return true; }
		if (r < 0) { ++st.stackFaults; return true; }
		return false;
	}

	bool anyThreadInHazard(const ScopedThreadSuspender& suspend, uintptr_t B, GateStats& st)
	{
		const HANDLE* hs = suspend.suspendedHandles();   // raw pointer + count: no container API in the window
		const size_t n = suspend.suspendedCount();
		for (size_t i = 0; i < n; ++i)
			if (threadInHazard(hs[i], B, st)) return true;
		return false;
	}

	// ---- live-thread census: the suspender only examines the threads its snapshot listed ----
	using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, PHANDLE next);

	// OUTSIDE any window (GetProcAddress can take the loader lock).
	NtGetNextThreadFn resolveNtGetNextThread()
	{
		const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		return nt ? (NtGetNextThreadFn)GetProcAddress(nt, "NtGetNextThread") : nullptr;
	}

	// INSIDE the window. Walks this process's live threads (NtGetNextThread: kernel-side enumeration into handles, no
	// buffer, no user-mode lock). 1 = every live thread other than the caller is one of the suspended `ids`; 0 = a live
	// thread is NOT suspended (created after the snapshot by a thread that was not frozen yet) - retry; -1 = could not
	// enumerate (not used to refuse: it would make a machine where the call misbehaves unable to ever apply).
	int censusAllSuspended(NtGetNextThreadFn next, const DWORD* ids, size_t n)
	{
		if (!next) return -1;
		constexpr LONG kNoMoreEntries = (LONG)0x8000001AL;
		const DWORD me = GetCurrentThreadId();
		HANDLE cur = nullptr;
		int result = 1;
		bool ended = false;
		for (int guard = 0; guard < 100000; ++guard)
		{
			HANDLE nxt = nullptr;
			const LONG status = next(GetCurrentProcess(), cur, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, 0, 0, &nxt);
			if (cur) CloseHandle(cur);
			cur = nullptr;
			if (status != 0)
			{
				if (status != kNoMoreEntries) result = -1;
				ended = true;
				break;
			}
			cur = nxt;
			const DWORD tid = GetThreadId(nxt);
			if (tid == me) continue;
			bool known = false;
			for (size_t i = 0; i < n && !known; ++i) known = ids[i] == tid;
			if (known) continue;
			if (WaitForSingleObject(nxt, 0) == WAIT_OBJECT_0) continue;   // already exited
			if (tid == 0) { result = -1; continue; }                       // could not identify it
			result = 0;
			break;
		}
		if (cur) CloseHandle(cur);
		if (!ended && result == 1) result = -1;
		return result;
	}

	// INSIDE the window: did it really stop every other thread? The snapshot must have worked, every live thread it
	// listed must be suspended, and none may have appeared since. False = do not use this window (retry).
	bool threadsAccountedFor(const ScopedThreadSuspender& s, NtGetNextThreadFn next, GateStats& st)
	{
		if (!s.snapshotOk() || s.missedCount() != 0) { ++st.unsuspended; return false; }
		const int c = censusAllSuspended(next, s.suspendedIds(), s.suspendedCount());
		if (c == 0) { ++st.unsuspended; return false; }
		if (c < 0) ++st.censusUnknown;
		return true;
	}

	// ---- reference scan for the deferred free ----
	struct FreeCand { uintptr_t lo; uintptr_t hi; uintptr_t S; bool referenced; };

	inline void markRef(uint64_t v, FreeCand* c, size_t n)
	{
		for (size_t i = 0; i < n; ++i) if (v - c[i].lo < c[i].hi - c[i].lo) c[i].referenced = true;
	}

	int sehScanRefs(uintptr_t lo, uintptr_t hi, FreeCand* c, size_t n)
	{
		__try
		{
			for (const volatile uint64_t* q = (const volatile uint64_t*)lo; (uintptr_t)q + 8 <= hi; ++q) markRef(*q, c, n);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}

	// INSIDE the window: marks every candidate block that this suspended thread holds an address in or near - in any
	// general-purpose or XMM register, or anywhere on its live stack. False = the thread could not be examined.
	bool threadRefScan(HANDLE h, FreeCand* c, size_t n)
	{
		alignas(16) CONTEXT ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
		if (!GetThreadContext(h, &ctx)) return false;
		const uint64_t gpr[] = { ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
			ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15, ctx.Rip };
		for (const uint64_t v : gpr) markRef(v, c, n);
		for (int i = 0; i < 16; ++i)
		{
			markRef((uint64_t)ctx.FltSave.XmmRegisters[i].Low, c, n);
			markRef((uint64_t)ctx.FltSave.XmmRegisters[i].High, c, n);
		}
		uintptr_t lo = 0, hi = 0;
		if (!liveStack(ctx.Rsp, lo, hi)) return false;
		return sehScanRefs(lo, hi, c, n) == 0;
	}

	// ============================================================================================ the windows (POD only)
	struct WindowArgs { uintptr_t B; uintptr_t S; const FieldPlan* plan; };

	// Puts every field back to stock: cap first, companion, then the rest in reverse. 1 = done, 2 = faulted too.
	int sehRollbackToStock(const FieldPlan* p)
	{
		__try
		{
			memcpy((void*)p[kCapRow].addr, p[kCapRow].stock, p[kCapRow].len);
			memcpy((void*)p[kRel8Row].addr, p[kRel8Row].stock, p[kRel8Row].len);
			for (size_t i = kRowCount; i-- > 0;)
				if (i != kCapRow && i != kRel8Row) memcpy((void*)p[i].addr, p[i].stock, p[i].len);
			return 1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
	}

	// APPLY step (6), every other thread suspended and the gate passed. 0 = written; 1 = a write faulted and every
	// field was put back to stock (S unreferenced); 2 = that rollback faulted as well (rows in an unknown mix).
	int sehApplyWindow(const WindowArgs* a)
	{
		__try
		{
			// 882 = 850 + 32: any in-flight stale index < 882 then reads exactly the bytes stock would have read.
			memcpy((void*)a->S, (const void*)(a->B + URT::kOldSectionsRva), (size_t)URT::kSectionStride * URT::kCopyInEntries);
			memcpy((void*)(a->S + URT::kBlockOffExt), (const void*)(a->B + URT::kOldExtRva), URT::kCopyInEntries);
			memcpy((void*)(a->S + URT::kBlockOffSii), (const void*)(a->B + URT::kOldSiiRva), URT::kCopyInEntries);
			const FieldPlan* p = a->plan;
			for (size_t i = 0; i < kRowCount; ++i)
				if (i != kCapRow && i != kRel8Row) memcpy((void*)p[i].addr, p[i].ours, p[i].len);
			memcpy((void*)p[kRel8Row].addr, p[kRel8Row].ours, p[kRel8Row].len);   // companion
			memcpy((void*)p[kCapRow].addr, p[kCapRow].ours, p[kCapRow].len);      // the cap LAST
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return sehRollbackToStock(a->plan);
	}

	// Puts every non-cap field back to OURS (the phase-1 state). 2 = done, 3 = faulted too.
	int sehRollForwardToOurs(const FieldPlan* p)
	{
		__try
		{
			for (size_t i = 0; i < kRowCount; ++i)
				if (i != kCapRow) memcpy((void*)p[i].addr, p[i].ours, p[i].len);
			return 2;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 3; }
	}

	// REVERT phase 2 (c), every other thread suspended, gate + counts passed. 0 = done; 1 = faulted before any row
	// was touched (rows still ours, cap 850 = phase-1 state); 2 = faulted while restoring rows but they were all put
	// back to ours (phase-1 state); 3 = rows in an unknown mix.
	int sehRevertWindow(const WindowArgs* a)
	{
		volatile int rowsTouched = 0;
		__try
		{
			// ONLY 850: copying 882 would clobber count, camera count and the visibility_collection pointer.
			memcpy((void*)(a->B + URT::kOldSectionsRva), (const void*)a->S, (size_t)URT::kSectionStride * URT::kCopyBackEntries);
			memcpy((void*)(a->B + URT::kOldExtRva), (const void*)(a->S + URT::kBlockOffExt), URT::kCopyBackEntries);
			memcpy((void*)(a->B + URT::kOldSiiRva), (const void*)(a->S + URT::kBlockOffSii), URT::kCopyBackEntries);
			rowsTouched = 1;
			const FieldPlan* p = a->plan;
			memcpy((void*)p[kRel8Row].addr, p[kRel8Row].stock, p[kRel8Row].len);   // reverse of apply: companion...
			for (size_t i = kRowCount; i-- > 0;)                                    // ...then the 59 fields backwards
				if (i != kCapRow && i != kRel8Row) memcpy((void*)p[i].addr, p[i].stock, p[i].len);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return rowsTouched ? sehRollForwardToOurs(a->plan) : 1;
	}

	// ============================================================================================ outside the window
	// Step (4): VirtualProtect the code pages OUTSIDE the suspend window; restore afterwards (or on unwind).
	// unprotect() may be called again (a gate retry after another patcher re-protected a shared page): it re-opens the
	// pages but keeps the protection saved the FIRST time, which is what restore() puts back.
	class PageUnprotect
	{
		uintptr_t mB = 0;
		std::array<DWORD, kPageCount> mOld{};
		std::array<bool, kPageCount> mDone{};
	public:
		// every listed page that intersects [a, a+n); a == 0 = all of them
		bool unprotect(uintptr_t B, uintptr_t a = 0, size_t n = 0)
		{
			mB = B;
			for (size_t i = 0; i < kPageCount; ++i)
			{
				const uintptr_t page = B + URT::kCodePages[i];
				if (a && !(page < a + n && page + kPageSize > a)) continue;
				DWORD old = 0;
				if (!VirtualProtect((LPVOID)page, kPageSize, PAGE_EXECUTE_READWRITE, &old)) { restore(); return false; }
				if (!mDone[i]) { mOld[i] = old; mDone[i] = true; }
			}
			return true;
		}
		void flush()
		{
			for (size_t i = 0; i < kPageCount; ++i)
				if (mDone[i]) FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(mB + URT::kCodePages[i]), kPageSize);
		}
		void restore()
		{
			for (size_t i = 0; i < kPageCount; ++i)
				if (mDone[i]) { DWORD ignored = 0; VirtualProtect((LPVOID)(mB + URT::kCodePages[i]), kPageSize, mOld[i], &ignored); mDone[i] = false; }
		}
		~PageUnprotect() { restore(); }
	};

	// Step (3): a zero-filled PAGE_READWRITE block near the module - upward from `target` (the image end) first, then
	// downward from below the module base - within +-kNearWindow of target.
	void* allocNear(uintptr_t B, uintptr_t target, size_t size)
	{
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
		const uintptr_t hi = target + kNearWindow;
		const uintptr_t lo = target > kNearWindow + gran ? target - kNearWindow : gran;
		for (uintptr_t p = (target + gran - 1) & ~(gran - 1); p + size <= hi; p += gran)
			if (void* m = VirtualAlloc((LPVOID)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
		for (uintptr_t p = (B & ~(gran - 1)) - gran; p >= lo; p -= gran)
			if (void* m = VirtualAlloc((LPVOID)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
		return nullptr;
	}

	// Revert phase 2 (a), NO suspension: wait until the camera count is seen to DROP, i.e. the camera-pass reset
	// (0x7EE01C) ran after the cap went back to 850, so every later entry was built under the stock cap. Reading 0
	// for the whole wait means rendering is idle, which counts as met. A non-zero count that never changes at all means
	// Halo 2 is loaded but not drawing the world (loading screen, menu): Frozen. Sampled continuously rather than every
	// 1 ms: a 1 ms Sleep can be 15 ms on a coarse timer and alias a short camera pass, never seeing the drop.
	enum class ResetWait { Seen, Idle, Busy, Frozen, ModuleGone, Stopped };
	ResetWait waitForCameraPassReset(uintptr_t B, const std::atomic<bool>& stop, const std::atomic<bool>& superseded)
	{
		const uintptr_t at = B + URT::kCameraCountRva;
		int32_t prev = 0;
		if (!sehRead32(at, &prev)) return ResetWait::ModuleGone;
		bool sawNonZero = prev != 0;
		bool changed = false;
		const ULONGLONG start = GetTickCount64();
		for (uint32_t spin = 1;; ++spin)
		{
			int32_t cur = 0;
			if (!sehRead32(at, &cur)) return ResetWait::ModuleGone;
			if (cur < prev) return ResetWait::Seen;
			if (cur != prev) changed = true;
			if (cur != 0) sawNonZero = true;
			prev = cur;
			if ((spin & 0x3FF) == 0)
			{
				if (stop.load(std::memory_order_relaxed) || superseded.load(std::memory_order_relaxed)) return ResetWait::Stopped;
				if (GetTickCount64() - start >= kResetWaitMs) return !sawNonZero ? ResetWait::Idle : changed ? ResetWait::Busy : ResetWait::Frozen;
			}
			YieldProcessor();
		}
	}

	// Step (1): what is at the 61 rows of the halo2.dll mapped at B?
	enum class Found { Unreadable, AllStock, OursCapRaised, OursCapStock, Baked, BakedUnrecognised, Foreign };
	struct Inspection { Found found = Found::Unreadable; uintptr_t sCand = 0; uint32_t sizeOfImage = 0; size_t mismatchRow = kRowCount; };

	Inspection inspect(uintptr_t B)
	{
		Inspection in;
		if (!sehSizeOfImage(B, &in.sizeOfImage)) return in;
		for (size_t i = 0; i < kRowCount && in.mismatchRow == kRowCount; ++i)
			if (!insnIs(B, i, nullptr)) in.mismatchRow = i;
		if (in.mismatchRow == kRowCount) { in.found = Found::AllStock; return in; }

		// S_cand = B + 0x7ED887 + 7 + disp32 (the lea r15,[sections] at the top of render_visible_section_add)
		const URT::Row& lea = URT::kRows[kAdoptRow];
		int32_t disp = 0;
		if (!sehRead32(B + lea.rva + lea.fieldOff, &disp)) return in;
		in.sCand = (uintptr_t)((int64_t)(B + lea.rva + lea.len) + (int64_t)disp);
		Plan plan{};

		// (1a) FIRST: a block inside the image = a baked dll. The formulas depend only on S - B, so a baked dll would
		// also pass the adopt test below - and adopting it would let a toggle-off "revert" the bake and VirtualFree an
		// image address. Never adopted, never applied, never queued for freeing.
		if (in.sCand >= B + URT::kStockSizeOfImage && in.sCand < B + in.sizeOfImage)
		{
			in.found = buildPlan(B, in.sCand, plan, nullptr) && allRowsOurs(B, plan) ? Found::Baked : Found::BakedUnrecognised;
			return in;
		}

		// (1b) our own live patch (an earlier HCM session, or a module that persisted across a "left Halo 2" event)
		in.found = Found::Foreign;
		if (!buildPlan(B, in.sCand, plan, nullptr) || !blockLooksOurs(in.sCand)) return in;
		for (size_t i = 0; i < kRowCount; ++i)
			if (i != kCapRow && !insnIs(B, i, &plan)) { in.mismatchRow = i; return in; }
		if (insnIs(B, kCapRow, &plan)) in.found = Found::OursCapRaised;
		else if (insnIs(B, kCapRow, nullptr)) in.found = Found::OursCapStock;
		else in.mismatchRow = kCapRow;
		return in;
	}

	std::string foreignReason(const Inspection& in)
	{
		if (in.mismatchRow >= kRowCount) return "its patch sites are unreadable";
		const URT::Row& r = URT::kRows[in.mismatchRow];
		return std::format("{} at +0x{:X} is neither stock 1.3528 nor this feature's own patch", r.id, r.rva);
	}

	std::string gateSummary(const GateStats& st)
	{
		return std::format("rip {}, stack {}, stackUnknown {}, stackFault {}, ctx {}, notReady {}, countsHigh {}, unsuspended {}, censusUnknown {}",
			st.ripHits, st.stackHits, st.stackUnknown, st.stackFaults, st.contextFails, st.notReady, st.countsHigh, st.unsuspended, st.censusUnknown);
	}

	// ============================================================================================ the patcher
	// Not thread-safe on its own: only the Impl's worker thread calls it (and ~Impl, after joining the worker).
	class Patcher
	{
		const std::atomic<bool>& mStop;        // HCM is closing: give up waiting (only ever leaves a SAFE state behind)
		const std::atomic<bool>& mSuperseded;  // the user asked for the opposite meanwhile: give up waiting, quietly
		NtGetNextThreadFn mNtGetNextThread = resolveNtGetNextThread();   // resolved here, never inside a window
		uintptr_t mBase = 0;              // the halo2.dll instance the flags below describe
		uintptr_t mS = 0;                 // our block, while any row references it
		bool mApplied = false;            // all 61 rows ours (cap N)
		bool mRevertPending = false;      // 60 rows ours, cap back at 850 (revert phase 1 done, phase 2 not yet)
		uintptr_t mBakeNoticeBase = 0;    // the module + notice ("baked in" / "unrecognised") last shown: preset loads
		const char* mBakeNoticeMsg = nullptr;   // fire the toggle event even when the value is unchanged, so say it once

		std::string bakeNoticeOnce(const char* msg)
		{
			if (mBakeNoticeBase == mBase && mBakeNoticeMsg == msg) return "";
			mBakeNoticeBase = mBase;
			mBakeNoticeMsg = msg;
			return msg;
		}
		struct PendingBlock { uintptr_t S; ULONGLONG queuedAt; };
		std::vector<PendingBlock> mPendingFree;   // superseded blocks: see drainPendingFree()

		bool stopping() const { return mStop.load(std::memory_order_relaxed) || mSuperseded.load(std::memory_order_relaxed); }
		bool superseded() const { return mSuperseded.load(std::memory_order_relaxed) && !mStop.load(std::memory_order_relaxed); }

		void queueFree(uintptr_t S)
		{
			if (!S) return;
			for (const PendingBlock& p : mPendingFree) if (p.S == S) return;
			mPendingFree.push_back(PendingBlock{ S, GetTickCount64() });
		}

		void adopt(uintptr_t S)
		{
			mS = S;
			std::erase_if(mPendingFree, [S](const PendingBlock& p) { return p.S == S; });   // referenced by live code again - never freed
			PLOG_INFO << "UncapRenderSections: adopted a live patch, block 0x" << std::hex << S << " (module 0x" << mBase << ")";
		}

		// Runs fn() inside a window in which EVERY other thread is provably stopped (the snapshot worked, nothing was
		// missed, nothing appeared since). fn follows the window rules. False = no such window came up.
		template <class Fn> bool inCompleteWindow(Fn&& fn)
		{
			for (int attempt = 0; attempt < kSimpleWindowAttempts; ++attempt)
			{
				bool done = false;
				{
					ScopedThreadSuspender suspend;
					GateStats ignored{};
					if (threadsAccountedFor(suspend, mNtGetNextThread, ignored)) { fn(suspend); done = true; }
				}
				if (done) return true;
				Sleep(1);
			}
			return false;
		}

		// Step (2), called only once every row has been verified stock: from then on no instruction can load a pending
		// block's address again. A reader resumed from the revert window can still HOLD it, though - in a callee-saved
		// register across calls (sub_1807F1610 keeps the base in r12 across 0x7F16C8..0x7F1740; sub_1809786A0 keeps
		// S+entry in r12 across ~20 calls and dereferences it at 0x978916/0x978CFE), spilled on its stack, or in a
		// frame-local list. So a block is freed only when (a) its revert finished at least kFreeGraceMs ago and (b) one
		// complete suspend window shows no thread with an address in or near it in any register or anywhere on its live
		// stack. Anything else stays queued for a later drain: at worst one 0x33000-byte block is leaked, never freed
		// under a reader.
		void drainPendingFree()
		{
			if (mPendingFree.empty()) return;
			const ULONGLONG now = GetTickCount64();
			std::vector<FreeCand> cand;
			for (const PendingBlock& p : mPendingFree)
				if (now - p.queuedAt >= kFreeGraceMs) cand.push_back(FreeCand{ p.S - kRefMargin, p.S + URT::kBlockSize + kRefMargin, p.S, false });
			if (cand.empty()) return;
			FreeCand* const c = cand.data();   // raw pointer + count: no container API inside the window
			const size_t n = cand.size();
			bool scanned = false;
			inCompleteWindow([&](const ScopedThreadSuspender& suspend)
			{
				const HANDLE* hs = suspend.suspendedHandles();
				const size_t threads = suspend.suspendedCount();
				bool ok = true;
				for (size_t i = 0; i < threads && ok; ++i) ok = threadRefScan(hs[i], c, n);
				scanned = ok;
			});
			if (!scanned) { PLOG_INFO << "UncapRenderSections: deferred block free skipped (no complete thread scan) - retried later"; return; }
			size_t freed = 0, held = 0;
			for (const FreeCand& f : cand)
			{
				if (f.referenced) { ++held; continue; }
				VirtualFree((LPVOID)f.S, 0, MEM_RELEASE);
				std::erase_if(mPendingFree, [&f](const PendingBlock& p) { return p.S == f.S; });
				++freed;
			}
			PLOG_INFO << std::format("UncapRenderSections: freed {} superseded block(s), {} still referenced and kept, {} pending", freed, held, mPendingFree.size());
		}

		// Forget the state (touch no memory); the block goes to pendingFree.
		void dropState()
		{
			queueFree(mS);
			mS = 0;
			mApplied = false;
			mRevertPending = false;
		}

		// Follow the module: a different base, an unloaded dll, or our lea no longer pointing at mS (MCC unloaded and
		// reloaded the dll at the same address between two events) all mean the state we hold is gone.
		void syncModule()
		{
			const uintptr_t now = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (now != mBase) { dropState(); mBase = now; return; }
			if (!now || !mS) return;
			const URT::Row& lea = URT::kRows[kAdoptRow];
			int32_t disp = 0;
			if (!sehRead32(now + lea.rva + lea.fieldOff, &disp) || (uintptr_t)((int64_t)(now + lea.rva + lea.len) + (int64_t)disp) != mS)
				dropState();
		}

		// revert-pending -> applied: only the cap moves (850 -> N); the arrays are already relocated, so no gate.
		void raiseCapFromPending()
		{
			Plan plan{};
			std::string why;
			if (!buildPlan(mBase, mS, plan, &why)) throw HCMRuntimeException("Uncap Render Sections: " + why);
			const FieldPlan& cap = plan[kCapRow];
			PageUnprotect pages;
			if (!pages.unprotect(mBase, cap.addr, cap.len)) throw HCMRuntimeException("Uncap Render Sections: could not unprotect the cap page");
			int r = 4;   // 0 written, 1 rows not in the expected state, 2 not writable / unmapped, 3 write fault, 4 no complete window
			inCompleteWindow([&](const ScopedThreadSuspender&)   // the 4-byte cap is not naturally aligned: never let a thread read it torn
			{
				if (!moduleMapped(mBase)) r = 2;
				else if (!rowsOursCapStock(mBase, plan)) r = 1;
				else if (!rangeWritable(cap.addr, cap.len)) r = 2;
				else r = sehWriteField(&cap, true) ? 0 : 3;
			});
			pages.flush();
			pages.restore();
			if (r != 0) throw HCMRuntimeException(std::format("Uncap Render Sections: could not raise the cap again (code {}) - nothing was changed", r));
			mRevertPending = false;
			mApplied = true;
		}

		// REVERT PHASE 1: the cap back to 850. No gate: a thread already past the old cap check still writes the new arrays.
		void revertPhase1(const Plan& plan)
		{
			const FieldPlan& cap = plan[kCapRow];
			PageUnprotect pages;
			if (!pages.unprotect(mBase, cap.addr, cap.len)) throw HCMRuntimeException("Uncap Render Sections: could not unprotect the cap page - still on");
			int r = 4;
			inCompleteWindow([&](const ScopedThreadSuspender&)
			{
				if (!moduleMapped(mBase)) r = 2;
				else if (!allRowsOurs(mBase, plan)) r = 1;
				else if (!rangeWritable(cap.addr, cap.len)) r = 2;
				else r = sehWriteField(&cap, false) ? 0 : 3;
			});
			pages.flush();
			pages.restore();
			if (r != 0) throw HCMRuntimeException(std::format("Uncap Render Sections: could not restore the cap (code {}) - still on", r));
			mApplied = false;
			mRevertPending = true;
		}

	public:
		Patcher(const std::atomic<bool>& stop, const std::atomic<bool>& superseded) : mStop(stop), mSuperseded(superseded) {}

		bool tracking() const { return mApplied || mRevertPending; }

		// Each returns the message to show ("" = nothing to say) or throws HCMRuntimeException.
		std::string apply(bool fromToggle)
		{
			syncModule();
			if (!mBase) return fromToggle ? "Uncap Render Sections: turns on when Halo 2 is loaded" : "";
			if (mApplied) return "";
			if (mRevertPending) { raiseCapFromPending(); return kOnMessage; }

			// (1) GUARD before touching anything
			const Inspection in = inspect(mBase);
			switch (in.found)
			{
			case Found::Unreadable:
				throw HCMRuntimeException("Uncap Render Sections: could not read halo2.dll - nothing was changed");
			case Found::Baked:
				PLOG_INFO << "UncapRenderSections: baked dll (block at image+0x" << std::hex << (in.sCand - mBase) << ") - no-op";
				return fromToggle ? bakeNoticeOnce(kBakedMessage) : "";
			case Found::BakedUnrecognised:
				PLOG_WARNING << "UncapRenderSections: modified dll, lea target image+0x" << std::hex << (in.sCand - mBase) << " but rows do not match - no-op";
				return fromToggle ? bakeNoticeOnce(kUnrecognisedMessage) : "";
			case Found::Foreign:
				throw HCMRuntimeException(std::format("Uncap Render Sections: this halo2.dll is not build 1.3528 or something else patched it ({}) - nothing was changed", foreignReason(in)));
			case Found::OursCapRaised:   // left live by an earlier HCM session, or the module persisted across onLeave
				adopt(in.sCand);
				mApplied = true;
				return fromToggle ? "Uncap Render Sections on (it was still active from earlier - picked it up)" : "";
			case Found::OursCapStock:    // an earlier session's revert stopped after phase 1
				adopt(in.sCand);
				mRevertPending = true;
				raiseCapFromPending();
				return kOnMessage;
			case Found::AllStock:
				break;
			}

			// (2) every row is verified stock, so nothing can load a deferred block's address any more (aged + scanned)
			drainPendingFree();

			// (3) the block, and every field value asserted to fit
			void* blk = allocNear(mBase, mBase + in.sizeOfImage, URT::kBlockSize);
			if (!blk) throw HCMRuntimeException("Uncap Render Sections: could not allocate the section block near halo2.dll - nothing was changed");
			const uintptr_t S = (uintptr_t)blk;
			Plan plan{};
			std::string why;
			if (!buildPlan(mBase, S, plan, &why))
			{
				VirtualFree(blk, 0, MEM_RELEASE);
				throw HCMRuntimeException("Uncap Render Sections: " + why + " - nothing was changed");
			}

			// (4) unprotect the code pages OUTSIDE the suspend window
			PageUnprotect pages;
			if (!pages.unprotect(mBase))
			{
				VirtualFree(blk, 0, MEM_RELEASE);
				throw HCMRuntimeException("Uncap Render Sections: could not unprotect halo2.dll's code pages - nothing was changed");
			}

			// (5) QUIESCENCE GATE + (6) the write, in the first window where every other thread is stopped and none is
			// in a hazard function
			const WindowArgs args{ mBase, S, plan.data() };
			GateStats st{};
			int result = -1;         // -1 = never found a quiet window
			bool fatal = false;      // the module went away or its bytes changed under us
			bool reprotect = false;  // another patcher re-protected a shared code page: re-open it (outside the window)
			bool pagesLost = false;  // ...and that failed
			while (result < 0 && !fatal && st.attempts < kApplyAttempts && !stopping())
			{
				++st.attempts;
				if (reprotect && !pages.unprotect(mBase)) { pagesLost = true; break; }
				reprotect = false;
				{
					ScopedThreadSuspender suspend;
					if (!moduleMapped(mBase) || !allRowsStock(mBase)) fatal = true;
					else if (!codePagesWritable(mBase)) { ++st.notReady; reprotect = true; }
					else if (!threadsAccountedFor(suspend, mNtGetNextThread, st)) {}
					else if (!anyThreadInHazard(suspend, mBase, st)) result = sehApplyWindow(&args);
				}
				if (result < 0 && !fatal) Sleep(1);
			}
			// (7) resume (above), flush, restore protections
			pages.flush();
			pages.restore();
			PLOG_INFO << std::format("UncapRenderSections apply: result {} fatal {} pagesLost {} after {} attempt(s) ({}); module 0x{:X} block 0x{:X}",
				result, fatal, pagesLost, st.attempts, gateSummary(st), mBase, S);

			if (result == 0)
			{
				mS = S;
				mApplied = true;
				return kOnMessage;
			}
			if (result == 2)   // rows in an unknown mix: the block may be referenced, so it is leaked, never freed
				throw HCMRuntimeException("Uncap Render Sections: a write faulted AND its rollback faulted - halo2.dll may be inconsistent; restart the game");
			VirtualFree(blk, 0, MEM_RELEASE);   // never written to the code, or rolled back to stock under suspension
			if (fatal) throw HCMRuntimeException("Uncap Render Sections: halo2.dll changed or unloaded while waiting - nothing was changed");
			if (result == 1) throw HCMRuntimeException("Uncap Render Sections: a write faulted and was rolled back - nothing was changed");
			if (pagesLost) throw HCMRuntimeException("Uncap Render Sections: could not re-open halo2.dll's code pages - nothing was changed");
			if (stopping()) return "";   // HCM is closing, or the user turned it off meanwhile: nothing was written
			throw HCMRuntimeException(kBusyMessage);
		}

		std::string revert()
		{
			syncModule();
			if (!mBase) return "";
			if (!mApplied && !mRevertPending)
			{
				// Our patch can be live without us knowing it (an earlier HCM session, or a module that persisted across a
				// "left Halo 2" event): recognise it and take it back down.
				const Inspection in = inspect(mBase);
				if (in.found == Found::OursCapRaised) { adopt(in.sCand); mApplied = true; }
				else if (in.found == Found::OursCapStock) { adopt(in.sCand); mRevertPending = true; }
				else return "";   // stock, or a bake (never reverted): turning it off is a silent no-op
			}

			Plan plan{};
			std::string why;
			if (!buildPlan(mBase, mS, plan, &why)) throw HCMRuntimeException("Uncap Render Sections: " + why);

			// PHASE 1: cap first (stock-equivalent and safe from here on, even if phase 2 never completes)
			if (mApplied) revertPhase1(plan);

			// PHASE 2 (a): a camera-pass reset after phase 1
			const ResetWait wait = waitForCameraPassReset(mBase, mStop, mSuperseded);
			PLOG_INFO << "UncapRenderSections revert: camera-pass wait " << (int)wait;
			if (wait == ResetWait::ModuleGone) { dropState(); mBase = 0; return ""; }
			if (wait == ResetWait::Stopped) return "";   // HCM closing / turned back on: phase-1 state, safe indefinitely
			if (wait == ResetWait::Frozen) throw HCMRuntimeException(kNotRenderingMessage);
			if (wait == ResetWait::Busy) throw HCMRuntimeException(kPendingMessage);

			// PHASE 2 (b)+(c): gated copy-back and row restore
			PageUnprotect pages;
			if (!pages.unprotect(mBase)) throw HCMRuntimeException(kPendingMessage);
			const WindowArgs args{ mBase, mS, plan.data() };
			GateStats st{};
			int result = -1;
			bool fatal = false;
			bool reprotect = false;
			bool pagesLost = false;
			while (result < 0 && !fatal && st.attempts < kRevertAttempts && !stopping())
			{
				++st.attempts;
				if (reprotect && !pages.unprotect(mBase)) { pagesLost = true; break; }
				reprotect = false;
				{
					ScopedThreadSuspender suspend;
					if (!moduleMapped(mBase) || !rowsOursCapStock(mBase, plan)) fatal = true;
					else if (!codePagesWritable(mBase)) { ++st.notReady; reprotect = true; }
					else if (!copyBackTargetsWritable(mBase)) ++st.notReady;
					else if (!countsAllowCopyBack(mBase)) ++st.countsHigh;
					else if (!threadsAccountedFor(suspend, mNtGetNextThread, st)) {}
					else if (!anyThreadInHazard(suspend, mBase, st)) result = sehRevertWindow(&args);
				}
				if (result < 0 && !fatal) Sleep(1);
			}
			// (d) resume (above), flush, restore protections
			pages.flush();
			pages.restore();
			PLOG_INFO << std::format("UncapRenderSections revert: result {} fatal {} pagesLost {} after {} attempt(s) ({}); module 0x{:X} block 0x{:X}",
				result, fatal, pagesLost, st.attempts, gateSummary(st), mBase, mS);

			if (result == 0)
			{
				queueFree(mS);   // a reader may still hold the block's base: freed only once aged AND unreferenced (drainPendingFree)
				mS = 0;
				mRevertPending = false;
				return kOffMessage;
			}
			if (result == 3)
			{
				mS = 0;   // leaked on purpose: rows in an unknown mix may still reference it
				mRevertPending = false;
				throw HCMRuntimeException("Uncap Render Sections: a write faulted AND its rollback faulted - halo2.dll may be inconsistent; restart the game");
			}
			if (fatal)
			{
				if (!moduleMapped(mBase)) { dropState(); mBase = 0; return ""; }
				// Someone else rewrote our rows. Stop tracking them (no retry at every level load); the block is leaked,
				// never freed, since what is there now may still reference it.
				mS = 0;
				mApplied = false;
				mRevertPending = false;
				throw HCMRuntimeException("Uncap Render Sections: halo2.dll's patch sites changed under HCM - left as they are (HCM no longer touches them)");
			}
			// Timeout, a fault rolled forward, or a stop: the phase-1 state, safe indefinitely; finished at the next Ingame.
			if (stopping()) return "";
			if (st.countsHigh * 2 > st.attempts) throw HCMRuntimeException(kTooDenseMessage);
			throw HCMRuntimeException(kPendingMessage);
		}

		// Ingame (or HCM start) with the toggle OFF: finish a revert left pending (or a revert whose phase 1 failed),
		// pick up a patch an earlier HCM session left live, and free superseded blocks. `adoptedOn` = a live patch from
		// earlier was adopted and left on: the caller makes the checkbox show it.
		std::string settleOff(bool& adoptedOn)
		{
			adoptedOn = false;
			syncModule();
			if (!mBase) return "";
			if (tracking()) return revert();
			const Inspection in = inspect(mBase);
			switch (in.found)
			{
			case Found::AllStock:
				drainPendingFree();   // every row verified stock
				return "";
			case Found::OursCapRaised:
				adopt(in.sCand);
				mApplied = true;
				adoptedOn = true;
				return kAdoptedOnMessage;
			case Found::OursCapStock:   // an earlier session was turning it off and stopped after phase 1: finish that
				adopt(in.sCand);
				mRevertPending = true;
				return revert();
			default:
				return "";   // baked / unrecognised / foreign: a toggle-on reports those
			}
		}

		// Left Halo 2 / the module went away: forget, touch no memory (the guard re-adopts it if the module persisted).
		void onLeave()
		{
			dropState();
			mBase = 0;
		}

		// HCM teardown: the patch stays live and every block is leaked on purpose - the game keeps using it after
		// HCMInternal's objects are gone, and a later HCM session adopts it through the guard.
		void abandon()
		{
			if (mS)
			{
				PLOG_INFO << "UncapRenderSections: HCM closing - leaving the patch live (block 0x" << std::hex << mS << " leaked on purpose)";
			}
			mS = 0;
			mApplied = false;
			mRevertPending = false;
			mPendingFree.clear();   // leaked too: a persisted module or a reader may still reference one of them
			mBase = 0;
		}
	};

	// ============================================================================================ shared state
	// Everything an event callback can touch. The Impl owns it through a shared_ptr and the callbacks hold only a
	// weak_ptr: eventpp can still invoke a callback it had already dispatched when ~Impl unsubscribes (it checks the
	// node's removed-counter, then calls with no lock held), and such a late call then either finds the Core expired or
	// keeps it alive for its own duration - never a use-after-free. (If a late call ends up the last owner, ~Core runs
	// on that thread: it only frees memory.)
	class Core
	{
	public:
		const GameState mGame;
		const std::weak_ptr<IMessagesGUI> mMessagesWeak;
		const std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;

		std::atomic<bool> mStop{ false };          // HCM is closing (declared before mPatcher, which refers to it)
		std::atomic<bool> mSuperseded{ false };    // the request being worked on was overtaken by the opposite toggle
		Patcher mPatcher{ mStop, mSuperseded };    // WORKER THREAD ONLY (and ~Impl after joining the worker)

		Core(GameState game, std::weak_ptr<IMessagesGUI> messages, std::weak_ptr<SettingsStateAndEvents> settings)
			: mGame(game), mMessagesWeak(std::move(messages)), mSettingsWeak(std::move(settings)) {}

		// ---- event side: record and wake, never patch, never block for long (reqMutex is only ever held briefly) ----
		void onToggle(bool value)
		{
			try
			{
				std::scoped_lock lock(mReqMutex);
				if (mShuttingDown) return;
				mDesired = value;
				mToggleRequested = true;
				if (mBusy && value != mBusyTarget) mSuperseded.store(true);
				mReqCv.notify_one();
			}
			catch (...) {}   // never let anything escape into eventpp / the setting's thread
		}

		void onState(const MCCState& s)
		{
			try
			{
				std::scoped_lock lock(mReqMutex);
				if (mShuttingDown) return;
				if (s.currentGameState != mGame) { mLeaveRequested = true; mIngameRequested = false; }   // a leave overtakes an Ingame not yet handled
				else if (s.currentPlayState == PlayState::Ingame) mIngameRequested = true;
				else return;   // loading screens: nothing to do (the arrays are rebuilt from zero every camera pass)
				mReqCv.notify_one();
			}
			catch (...) {}
		}

		// Reads the toggle AFTER the callbacks are subscribed: an event in between only repeats the same value.
		void primeDesired()
		{
			auto settings = mSettingsWeak.lock();
			std::scoped_lock lock(mReqMutex);
			if (settings) mDesired = settings->uncapRenderSectionsToggle->GetValue();
		}

		void requestShutdown()
		{
			{
				std::scoped_lock lock(mReqMutex);
				mShuttingDown = true;
			}
			mStop.store(true);
			mReqCv.notify_all();
		}

		// ---- the worker thread ----
		void workerLoop()
		{
			for (;;)
			{
				bool leave = false, ingame = false, toggle = false, startup = false, want = false;
				{
					std::unique_lock lock(mReqMutex);
					mReqCv.wait(lock, [this] { return mShuttingDown || mLeaveRequested || mIngameRequested || mToggleRequested || mStartupRequested; });
					if (mShuttingDown) return;
					leave = std::exchange(mLeaveRequested, false);
					ingame = std::exchange(mIngameRequested, false);
					toggle = std::exchange(mToggleRequested, false);
					startup = std::exchange(mStartupRequested, false);
					want = mDesired;   // the LATEST toggle value, read under the lock the toggle handler takes
					mBusy = true;
					mBusyTarget = want;
					mSuperseded.store(false);
				}

				std::string msg;
				bool adoptedOn = false;
				try
				{
					if (leave) mPatcher.onLeave();
					if (toggle) msg = want ? mPatcher.apply(true) : mPatcher.revert();
					else if (ingame) msg = want ? mPatcher.apply(false) : mPatcher.settleOff(adoptedOn);
					else if (startup && !want) msg = mPatcher.settleOff(adoptedOn);
				}
				catch (HCMRuntimeException& ex) { msg = ex.what(); }   // HCMExceptionBase already logged it
				catch (const std::exception& ex) { msg = std::string("Uncap Render Sections: ") + ex.what(); PLOG_ERROR << msg; }
				catch (...) { PLOG_ERROR << "UncapRenderSections: unexpected exception on the worker thread"; }

				{
					std::scoped_lock lock(mReqMutex);
					mBusy = false;
					if (adoptedOn)
					{
						// Make the checkbox show the adopted patch - unless the user toggled meanwhile (that request is
						// handled next and decides). Set without firing the event, like FarClipDistance does.
						if (mToggleRequested || mShuttingDown) msg.clear();
						else
						{
							mDesired = true;
							if (auto settings = mSettingsWeak.lock())
							{
								settings->uncapRenderSectionsToggle->GetValueDisplay() = true;
								settings->uncapRenderSectionsToggle->GetValue() = true;
							}
						}
					}
				}
				say(msg);
			}
		}

	private:
		std::mutex mReqMutex;                 // guards everything below; never held across patching
		std::condition_variable mReqCv;
		bool mDesired = false;                // the latest toggle value
		bool mToggleRequested = false;        // a toggle event arrived (its messages are the user-facing ones)
		bool mIngameRequested = false;        // MCC reached Ingame in Halo 2
		bool mLeaveRequested = false;         // MCC left Halo 2
		bool mStartupRequested = true;        // first job: pick up a patch an earlier HCM session left live
		bool mShuttingDown = false;
		bool mBusy = false;                   // the worker is running a request...
		bool mBusyTarget = false;             // ...that drives towards this toggle value

		void say(const std::string& msg)
		{
			if (msg.empty()) return;
			PLOG_INFO << msg;
			try { if (auto m = mMessagesWeak.lock()) m->addMessage(msg); }
			catch (...) {}
		}
	};
}


class UncapRenderSections::Impl
{
private:
	std::shared_ptr<Core> mCore;   // declared FIRST: the callbacks below capture a weak_ptr to it
	std::thread mWorker;

	// Declared LAST so they are destroyed FIRST (and ~Impl removes them explicitly before anything else).
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mStateCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mCore(std::make_shared<Core>(game, dicon.Resolve<IMessagesGUI>(), dicon.Resolve<SettingsStateAndEvents>())),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapRenderSectionsToggle->valueChangedEvent,
			[weak = std::weak_ptr<Core>(mCore)](bool& v) { if (auto c = weak.lock()) c->onToggle(v); }),
		mStateCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[weak = std::weak_ptr<Core>(mCore)](const MCCState& s) { if (auto c = weak.lock()) c->onState(s); })
	{
		mCore->primeDesired();
		try { mWorker = std::thread([core = mCore] { core->workerLoop(); }); }
		catch (const std::exception& ex) { throw HCMInitException(std::format("UncapRenderSections: could not start its worker thread: {}", ex.what())); }
	}

	~Impl()
	{
		// UNSUBSCRIBE FIRST: MCC's state hook and the setting outlive this cheat. A call eventpp had already dispatched
		// can still arrive; it only reaches the Core through its weak_ptr and sees mShuttingDown.
		mStateCallback.removeCallback();
		mToggleCallback.removeCallback();
		// An apply/revert still waiting for a quiet window gives up now, leaving a SAFE state (apply: nothing written;
		// revert: cap 850 on the relocated arrays) instead of holding up HCM's teardown for seconds.
		mCore->requestShutdown();
		if (mWorker.joinable()) mWorker.join();
		mCore->mPatcher.abandon();   // the worker is gone: HCM unload leaves the patch LIVE (spec); a later session re-adopts it
	}
};


UncapRenderSections::UncapRenderSections(GameState game, IDIContainer& dicon)
{
	switch (game)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<Impl>(game, dicon);
		break;
	default:
		throw HCMInitException("UncapRenderSections not impl for this game (Halo 2 only)");
	}
}

UncapRenderSections::~UncapRenderSections() { PLOG_VERBOSE << "~" << getName(); }
