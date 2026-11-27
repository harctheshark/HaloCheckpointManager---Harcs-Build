#include "pch.h"
#include "UncapClusterLimit.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include <Windows.h>
#include "ScopedThreadSuspender.h"
#include "H2QuietWindow.h"           // the quiescence gate shared by the Halo 2 uncap features
#include "H2SafeWrite.h"             // guarded reads / writes of halo2.dll (it can be unloaded under us)
#include <vector>
#include <mutex>

// ============================================================================
//  Halo 2 (MCC halo2.dll 1.3528) - beat the 128 region-clusters-per-region wall.
//  Ports the proven sapien fix (sapien.reloc5.exe: 170+ clusters, geo renders).
//
//  Two stages layered in ONE toggle:
//   STAGE 1  in-slack relocation of the projbuf index/volume arrays + volume cap
//            512->496 (fits every stock projbuf incl. the 0x22978 shadow one).
//   STAGE 2  the actual >128 capability:
//     - subpart-mask pool: relocate coll+0x270 (was 512 inline dwords) to a heap
//       buffer + raise budget to 4096  (lea->mov + cap imm + one clear lea->mov).
//     - item cluster-bitvector: relocate coll+0xA74 (512 items x 16B = 128 bits)
//       to a heap buffer at 32B/item (256 bits): getter rewritten in-place, the two
//       inline setters + the alloc/clear body detoured to caves, walker 4->8 dwords.
//     - clusters_to_region_clusters map (coll+0x30): reader movsx->movzx + getter
//       cave (0xFF sentinel -> -1) so cluster indices 128..254 don't sign-wrap.
//     - region cluster cap 0x797E64: 0x80 -> 0xFF (255).
//
//  Pointer slots (coll+0x270 / +0xA74) live in the .data collection and the ctor
//  never rewrites them, so we set them once and re-arm on each Halo 2 (re)entry.
//  Buffers are our own VirtualAlloc (never handed to the engine's allocator/free).
//
//  The 255 ceiling is the byte-wide clusters_to_region_clusters map (coll+0x30, 0xFF=NONE):
//  going higher would need that map widened byte->word plus a wider bitvector/walker.
//
//  HOW IT IS WRITTEN (2026-11-18 rework; the bytes are the same as before, proven by uncap_harness against the dll):
//   1. PLAN, with the game running: allocate the buffers and the cave page, build the caves, and read + CHECK every one
//      of the ~45 sites against the stock 1.3528 bytes. Any mismatch throws here - nothing has been written yet. (Before,
//      one byte was checked; a missed field was only logged, and the patch carried on half-applied.)
//   2. COMMIT, in ONE quiet window (H2QuietWindow.h: every other thread suspended, none of them in or returning into the
//      renderer or our cave): the sites are compared with the plan once more, then written. Memory and bare system calls
//      only - the first version built std::vectors and logged in here, and a thread frozen while holding the heap or
//      the logger's lock would have hung the game for good.
//   3. A write that fails is undone inside the same window. Every write is guarded (H2SafeWrite.h).
//  The revert is the same thing backwards, and it is ONLY done in a quiet window: without one the patch simply stays on
//  (the "ordered drain" of the first version went ahead anyway, mid-frame, and then freed the cave a thread could be in).
//  MCC unloads halo2.dll when a game ends, so what we hold is re-checked against the mapped image before every use.
// ============================================================================

namespace
{
	constexpr uint32_t CAM = 0x15FE020;      // camera visibility collection (.data)
	constexpr uint32_t SHA = 0x1600B30;      // shadow visibility collection (.data)
	constexpr uint32_t COLLS[] = { CAM, SHA };

	constexpr uint32_t OFF_SUBPART_PTR = 0x270;   // was inline subpart pool[512dw]
	constexpr uint32_t OFF_ITEMBV_PTR = 0xA74;    // was inline item bitvector[512][16B]
	constexpr uint32_t SUBPART_BUFDW = 4096;      // new subpart pool budget (dwords)
	constexpr uint32_t ITEMBV_ITEMS = 512;        // item count (k_maximum_region_memory_items)
	constexpr uint32_t ITEMBV_STRIDE = 32;        // 256 bits/item
	constexpr uint32_t SUBPART_BUFBYTES = SUBPART_BUFDW * 4;         // 0x4000
	constexpr uint32_t ITEMBV_BUFBYTES = ITEMBV_ITEMS * ITEMBV_STRIDE; // 0x4000

	constexpr uint32_t CLUSTERCAP_IMM_RVA = 0x797E64; // stock byte 0x80 -> 0xFF
	constexpr size_t   CAVE_PAGE = 0x1000;

	// clusters sub-collection (coll+0x10) gates how many clusters get SUBMITTED (stock cap 128 - the
	// one sub-coll UncapVisibilityLimits deliberately leaves alone). Resize per-map from the GAME heap
	// (sub_1800769E0) so the engine frees the bigger arrays normally on teardown.
	constexpr uint32_t CLUSTER_SUBCOLL_OFF = 0x10;
	constexpr int      SUBCOLL_NEWCAP = 2048;
	constexpr uint32_t GAME_ALLOC = 0x769E0;   // sub_1800769E0(size)->ptr
	constexpr uint32_t GAME_FREE = 0x76E30;    // sub_180076E30(ptr)

	// ---- the stock 1.3528 bytes of the sites that are replaced whole (read from the shipped halo2.dll) ----
	constexpr uint8_t kGetterStock[18] = { 0x48,0x0F,0xBF,0xC2,0x48,0xC1,0xE0,0x04,0x48,0x05,0x74,0x0A,0x00,0x00,0x48,0x03,0xC1,0xC3 };   // 0x709D20
	constexpr uint8_t kSetter1Stock[8] = { 0x42,0x09,0x94,0x87,0x74,0x0A,0x00,0x00 };   // 0x707E5C  or [rdi+r8*4+0A74h],edx
	constexpr uint8_t kSetter2Stock[8] = { 0x42,0x09,0x84,0x86,0x74,0x0A,0x00,0x00 };   // 0x709BDF  or [rsi+r8*4+0A74h],eax
	constexpr uint8_t kAllocBodyStock[82] = {                                             // 0x709CB4..0x709D06
		0x33,0xC9,0x49,0x8D,0x81,0xA8,0x00,0x00,0x00,0x48,0x03,0xC0,0x41,0x8B,0xD1,0x48,0x03,0xD2,0x41,0x89,0x0C,0xC0,0x41,0x8B,
		0xC1,0x49,0x89,0x8C,0xD0,0x78,0x0A,0x00,0x00,0x41,0x89,0x8C,0xD0,0x74,0x0A,0x00,0x00,0x49,0x0F,0xBF,0xD2,0x48,0xC1,0xFA,
		0x05,0x49,0x0F,0xBF,0xC9,0x48,0x8D,0x0C,0x8A,0xBA,0x01,0x00,0x00,0x00,0x4D,0x8D,0x04,0x88,0x41,0x0F,0xBF,0xCA,0x83,0xE1,
		0x1F,0xD3,0xE2,0x41,0x09,0x90,0x74,0x0A,0x00,0x00 };
	constexpr uint8_t kMapGetterStock[10] = { 0x48,0x0F,0xBF,0xC2,0x0F,0xBE,0x44,0x08,0x30,0xC3 };   // 0x7092F0
	static_assert(sizeof(kAllocBodyStock) == 0x709D06 - 0x709CB4, "alloc/clear body length");

	// quiet logger: routes to HCM's PLOG at debug level. NEVER called inside a suspension window.
	struct Dbg
	{
		static Dbg& get() { static Dbg d; return d; }
		void logf(const char* fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); wvsprintfA(b, fmt, ap); va_end(ap); PLOG_DEBUG << "[UncapClusterLimit] " << b; }
	};

	// pack helpers
	static void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24)); }

	// One planned write: POD, sized for the largest site (the 82-byte alloc/clear body), so the commit touches no heap.
	struct Write { uintptr_t addr; uint8_t len; uint8_t orig[88]; uint8_t patch[88]; };

	// ============================================================================================ INSIDE a window
	// Everything below this line runs with every other thread suspended: POD only, memory + bare system calls, no heap,
	// no logging, no container calls (raw pointers and counts).

	void drainProj(uintptr_t proj, uint32_t volCount)
	{
		if (proj) { *(uint16_t*)(proj + 0xA6C) = 0; *(uint32_t*)(proj + volCount) = 0; }
	}
	// Make everything the code could still read from the OTHER layout empty: the collections' cursors and the projection
	// buffers' cluster / volume counts. The next frame's pass rebuilds them.
	void drainCounts(uintptr_t base, bool newLayout)
	{
		__try
		{
			const uint32_t volCount = newLayout ? 0x2860 : 0x1970;
			for (uint32_t c : COLLS)
			{
				const uintptr_t coll = base + c;
				*(uint16_t*)(coll + 0xA70) = 0;   // subpart pool cursor
				*(uint16_t*)(coll + 0xA72) = 0;   // item count
				drainProj(*(uintptr_t*)coll, volCount);   // coll[0] = its projection buffer
			}
			drainProj(base + 0x1603640, volCount); drainProj(base + 0x1625FC0, volCount);   // the two projection buffers in .data
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// APPLY. The cluster cap is the LAST code write. 0 = written; 1 = the image is gone or a site no longer holds what was
	// planned (nothing written); 2 = a write failed and everything was put back; 3 = ...and putting it back failed too.
	int commitWindow(uintptr_t base, Write* code, size_t nCode, Write* slots, size_t nSlots)
	{
		if (nCode == 0 || !h2safe::moduleMappedAt(base)) return 1;
		for (size_t i = 0; i < nCode; ++i) if (!h2safe::equals(code[i].addr, code[i].orig, code[i].len)) return 1;
		// the slots hold live inline data until now (the first 8 bytes of the pool / bit-vector): keep what is there
		for (size_t i = 0; i < nSlots; ++i) if (!h2safe::copy(slots[i].orig, (const void*)slots[i].addr, slots[i].len)) return 1;

		size_t c = 0, s = 0;
		while (c + 1 < nCode && h2safe::patch(code[c].addr, code[c].patch, code[c].len)) ++c;                    // the code, cap excluded
		bool ok = c + 1 == nCode;
		if (ok) { while (s < nSlots && h2safe::copy((void*)slots[s].addr, slots[s].patch, slots[s].len)) ++s; ok = s == nSlots; }   // pointer slots
		if (ok) ok = h2safe::patch(code[nCode - 1].addr, code[nCode - 1].patch, code[nCode - 1].len);           // the cap, last
		if (ok) { drainCounts(base, true); return 0; }

		bool back = true;   // stock over everything, last first (a site that was never written just gets its own bytes again)
		for (size_t i = nSlots; i-- > 0;) back = h2safe::copy((void*)slots[i].addr, slots[i].orig, slots[i].len) && back;
		for (size_t i = nCode; i-- > 0;) back = h2safe::patch(code[i].addr, code[i].orig, code[i].len) && back;
		return back ? 2 : 3;
	}

	// REVERT, in the order the first version drained: cap -> 128, live counts emptied, the code (last first), the stale
	// old-layout counts, the pointer slots. 0 = stock again; 1 = the image is gone or the sites no longer hold our patch
	// (nothing written); 2 = a restore failed and our patch was put back (still ON); 3 = an unknown mix.
	int restoreWindow(uintptr_t base, Write* code, size_t nCode, Write* slots, size_t nSlots)
	{
		if (nCode == 0 || !h2safe::moduleMappedAt(base)) return 1;
		for (size_t i = 0; i < nCode; ++i) if (!h2safe::equals(code[i].addr, code[i].patch, code[i].len)) return 1;

		const Write& cap = code[nCode - 1];
		bool ok = h2safe::patch(cap.addr, cap.orig, cap.len);
		if (ok)
		{
			drainCounts(base, true);
			for (size_t i = nCode - 1; ok && i-- > 0;) ok = h2safe::patch(code[i].addr, code[i].orig, code[i].len);
		}
		if (ok)
		{
			drainCounts(base, false);
			for (size_t i = nSlots; ok && i-- > 0;) ok = h2safe::copy((void*)slots[i].addr, slots[i].orig, slots[i].len);
		}
		if (ok) return 0;

		bool fwd = true;   // back to the known "on" state, written as an apply writes it
		for (size_t i = 0; i + 1 < nCode; ++i) fwd = h2safe::patch(code[i].addr, code[i].patch, code[i].len) && fwd;
		for (size_t i = 0; i < nSlots; ++i) fwd = h2safe::copy((void*)slots[i].addr, slots[i].patch, slots[i].len) && fwd;
		fwd = h2safe::patch(cap.addr, cap.patch, cap.len) && fwd;
		drainCounts(base, true);
		return fwd ? 2 : 3;
	}

	// The clusters sub-collection: cap u32 @+0 ; count u16 @+4 ; A @+8 (2*cap) ; B @+0x10 (4*cap) ; C @+0x18 (8*cap) ; D @+0x20 (2*cap)
	struct Swap { uintptr_t subp; uint16_t cnt; bool done; void* n[4]; uintptr_t o[4]; };
	constexpr size_t kSwapElem[4] = { 2, 4, 8, 2 };

	// Copy the live entries into the bigger arrays and repoint. What is there NOW is used (count and array pointers are
	// read here, not when the arrays were allocated). A sub-collection that changed, or a fault, leaves `done` false.
	void commitSwaps(Swap* swaps, size_t n)
	{
		__try
		{
			for (size_t k = 0; k < n; ++k)
			{
				Swap& s = swaps[k];
				const uint32_t cap = *(uint32_t*)s.subp; const uint16_t cnt = *(uint16_t*)(s.subp + 4);
				if (cap == 0 || cap >= (uint32_t)SUBCOLL_NEWCAP || cnt > cap) continue;
				for (int i = 0; i < 4; ++i) s.o[i] = *(uintptr_t*)(s.subp + 8 + 8 * i);
				for (int i = 0; i < 4; ++i) memcpy(s.n[i], (void*)s.o[i], (size_t)cnt * kSwapElem[i]);
				for (int i = 0; i < 4; ++i) *(uintptr_t*)(s.subp + 8 + 8 * i) = (uintptr_t)s.n[i];
				*(uint32_t*)s.subp = SUBCOLL_NEWCAP;
				s.cnt = cnt; s.done = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// ============================================================================================ the patcher
	class Patcher
	{
		uintptr_t base = 0;
		bool applied = false;
		void* cavePage = nullptr;              // executable, within +-2GB of halo2.dll
		size_t caveUsed = 0;
		std::vector<void*> bufs;               // the live patch's data buffers (item-bv + subpart, per coll)
		std::vector<void*> freeLater;          // buffers / cave pages NOTHING references any more (reverted in a quiet window,
		                                       //   or their halo2.dll is gone): freed at the next apply or with the feature
		std::vector<void*> visOld;             // orphaned original sub-coll arrays (game-heap, deferred free)
		std::vector<Write> code;               // every code write of the live patch, in apply order (the cap is the last)
		std::vector<Write> slots;              // the pointer slots in .data

		void* gameAlloc(size_t n) { return ((void* (*)(int64_t))(base + GAME_ALLOC))((int64_t)n); }
		void  gameFree(void* p) { if (p) ((void (*)(void*))(base + GAME_FREE))(p); }
		// Free last cycle's orphaned (game-heap) sub-coll arrays via the engine's deallocator. SEH-guarded backstop:
		// if a stale pointer from a torn-down map ever reaches here (the game bulk-frees its arena on teardown), the
		// game free would AV on decommitted memory - catch it and drop the list rather than crash.
		void  drainVisOld()
		{
			if (base && (uintptr_t)GetModuleHandleA("halo2.dll") == base)
			{
				void** arr = visOld.data(); size_t n = visOld.size(); uintptr_t gf = base + GAME_FREE;
				__try { for (size_t i = 0; i < n; ++i) if (arr[i]) ((void(*)(void*))gf)(arr[i]); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			visOld.clear();
		}

		// ---------------------------------------------------------------- planning (threads live; may allocate, may throw)
		[[noreturn]] void notStock(const char* tag, uintptr_t addr)
		{
			throw HCMRuntimeException(std::format("Uncap Cluster Limit: halo2.dll is not build 1.3528, or something else changed it ({} at +0x{:X}) - nothing was changed", tag, addr - base));
		}
		// expect == nullptr: the caller has just matched the bytes itself (planField)
		void planWrite(uintptr_t addr, const uint8_t* expect, const uint8_t* ours, size_t len, const char* tag)
		{
			Write w{};
			if (len == 0 || len > sizeof(w.patch)) throw HCMRuntimeException("UncapClusterLimit: internal error (patch length)");
			w.addr = addr; w.len = (uint8_t)len;
			if (!h2safe::copy(w.orig, (const void*)addr, len) || (expect && memcmp(w.orig, expect, len) != 0)) notStock(tag, addr);
			for (const Write& o : code)
				if (addr < o.addr + o.len && o.addr < addr + len)
					throw HCMRuntimeException(std::format("UncapClusterLimit: internal error (patch sites overlap at +0x{:X})", addr - base));
			memcpy(w.patch, ours, len);
			code.push_back(w);
		}
		void planBytes(uint32_t rva, std::initializer_list<uint8_t> stock, std::initializer_list<uint8_t> ours, const char* tag)
		{
			if (stock.size() != ours.size()) throw HCMRuntimeException("UncapClusterLimit: internal error (byte patch sizes)");
			planWrite(base + rva, stock.begin(), ours.begin(), ours.size(), tag);
		}
		// the 4-byte LE `stock` somewhere in [rva, rva+9] becomes `ours`
		void planField(uint32_t rva, uint32_t stock, uint32_t ours, const char* tag)
		{
			uint8_t win[13];
			if (h2safe::copy(win, (const void*)(base + rva), sizeof(win)))
				for (int off = 0; off <= 9; ++off)
				{
					uint32_t v; memcpy(&v, win + off, 4);
					if (v == stock) { planWrite(base + rva + off, nullptr, (const uint8_t*)&ours, 4, tag); return; }
				}
			notStock(tag, base + rva);
		}
		void* allocNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO si; GetSystemInfo(&si); uintptr_t gran = si.dwAllocationGranularity;
			uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL, hi = target + 0x70000000ULL;
			uintptr_t startp = target & ~(gran - 1);
			for (uintptr_t p = startp; p > lo; p -= gran) if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
			for (uintptr_t p = startp + gran; p < hi; p += gran) if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
			return nullptr;
		}
		// write `c` into the cave page (nothing jumps there yet); returns its VA. jmpBack (if nonzero) appended as jmp rel32.
		uintptr_t emitCave(std::vector<uint8_t> c, uintptr_t jmpBack)
		{
			uintptr_t caveVA = (uintptr_t)cavePage + caveUsed;
			if (jmpBack)
			{
				uintptr_t after = caveVA + c.size() + 5;
				int32_t rel = (int32_t)(jmpBack - after);
				c.push_back(0xE9); put32(c, (uint32_t)rel);
			}
			if (caveUsed + c.size() > CAVE_PAGE) throw HCMRuntimeException("UncapClusterLimit: internal error (cave page full)");
			memcpy((void*)caveVA, c.data(), c.size());
			caveUsed += c.size() + 16;               // pad between caves
			caveUsed = (caveUsed + 15) & ~size_t(15);
			return caveVA;
		}
		// detour: `len` stock bytes at rva (>=5) become jmp->caveVA + nop pad
		void planDetour(uint32_t rva, const uint8_t* stock, size_t len, uintptr_t caveVA, const char* tag)
		{
			const uintptr_t siteVA = base + rva;
			std::vector<uint8_t> jb; jb.push_back(0xE9); put32(jb, (uint32_t)(int32_t)(caveVA - (siteVA + 5)));
			while (jb.size() < len) jb.push_back(0x90);
			planWrite(siteVA, stock, jb.data(), len, tag);
		}
		void planSlot(uintptr_t addr, uint64_t val)
		{
			Write w{}; w.addr = addr; w.len = 8; memcpy(w.patch, &val, 8);   // orig is read inside the window
			slots.push_back(w);
		}

		// ---- STAGE 1 in-slack table (all RVAs verified vs 1.3528) ----
		void planStage1()
		{
			constexpr uint32_t IDX_OLD = 0x1770, VCNT_OLD = 0x1970, VARR_OLD = 0x1974;
			constexpr uint32_t IDX_NEW = 0x2460, VCNT_NEW = 0x2860, VARR_NEW = 0x2864;
			constexpr uint32_t DELTA = VARR_NEW - VARR_OLD;
			const uint32_t idx[] = { 0x70826A,0x7082B9,0x709381,0x709390,0x797E94,0x7997F8,0x799992,0x799A2B };
			const uint32_t vcnt[] = { 0x797EE4,0x797F0C,0x799406,0x7998E1,0x7999AD,0x7999BF };
			const uint32_t varr[] = { 0x7094AA,0x70950D,0x797F13,0x7999C6,0x79A3FD };
			for (uint32_t r : idx) planField(r, IDX_OLD, IDX_NEW, "IDX");
			for (uint32_t r : vcnt) planField(r, VCNT_OLD, VCNT_NEW, "VCNT");
			for (uint32_t r : varr) planField(r, VARR_OLD, VARR_NEW, "VARR");
			planField(0x797F53, 0x1978, 0x1978 + DELTA, "SUBF1978");
			planField(0x797FC6, 0x197C, 0x197C + DELTA, "SUBF197C");
			planField(0x797F60, 0x1994, 0x1994 + DELTA, "SUBF1994");
			planField(0x7F1994, 0x19EC, 0x19EC + DELTA, "SUBF19EC");
			for (uint32_t r : { 0x797EC5u,0x798027u,0x799949u,0x7999F3u }) planField(r, 0x200, 496, "volcap");
		}

		// Allocates the buffers + cave and fills `code` / `slots`. Throws with nothing written to the game.
		void planAll()
		{
			cavePage = allocNear(base, CAVE_PAGE);
			if (!cavePage) throw HCMRuntimeException("UncapClusterLimit: near-cave alloc failed.");
			caveUsed = 0;

			struct CollBuf { uintptr_t coll; void* sub; void* itm; };
			std::vector<CollBuf> cb;
			for (uint32_t c : COLLS)
			{
				void* sub = VirtualAlloc(0, SUBPART_BUFBYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (sub) bufs.push_back(sub);
				void* itm = VirtualAlloc(0, ITEMBV_BUFBYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (itm) bufs.push_back(itm);
				if (!sub || !itm) throw HCMRuntimeException("UncapClusterLimit: buffer alloc failed.");
				cb.push_back({ base + c, sub, itm });   // VirtualAlloc memory is zero-filled
			}

			// ---------- STAGE 1: in-slack index/volume relocation ----------
			planStage1();

			// ---------- STAGE 2 code patches (cluster cap planned LAST, below) ----------
			// subpart pool: lea->mov + budget 512->4096 + clear lea->mov
			planBytes(0x70931C, { 0x8D }, { 0x8B }, "subpart lea");            // sub_180709300: lea rax,[rcx+270h] -> mov
			planField(0x709316, 0x200, SUBPART_BUFDW, "subpart cap");
			planBytes(0x708132, { 0x8D }, { 0x8B }, "subpart clear lea");      // sub_180707F50: lea rcx,[rdi+270h] -> mov

			// item bitvector: getter rewritten in-place (18B -> 16B + nops), stride 16->32
			{
				std::vector<uint8_t> g = { 0x48,0x0F,0xBF,0xC2, 0x48,0xC1,0xE0,0x05, 0x48,0x03,0x81,0x74,0x0A,0x00,0x00, 0xC3 };
				while (g.size() < sizeof(kGetterStock)) g.push_back(0x90);
				planWrite(base + 0x709D20, kGetterStock, g.data(), g.size(), "item bit-vector getter");
			}
			// setter#1 (sub_1807078A0): stride *4->*8, then detour the 'or' to a deref cave
			planBytes(0x707E52, { 0x88 }, { 0xC8 }, "setter 1 stride");        // lea r8,[rax+rcx*4] -> *8
			{
				// cave: push rax; mov rax,[rdi+0A74h]; or [rax+r8*4], edx; pop rax ; jmp back to 0x707E64
				// (the stock `or` clobbered no registers, so preserve rax rather than assume it's dead)
				std::vector<uint8_t> c = { 0x50,  0x48,0x8B,0x87,0x74,0x0A,0x00,0x00,  0x42,0x09,0x14,0x80,  0x58 };
				uintptr_t cave = emitCave(c, base + 0x707E64);
				planDetour(0x707E5C, kSetter1Stock, sizeof(kSetter1Stock), cave, "setter 1");
			}
			// setter#2 (sub_1807098C0): stride *4->*8, then detour the 'or' to a deref cave (scratch rcx).
			// instr `4C 8D 04 82` @0x709BD4 -> SIB byte (82) is at 0x709BD7 (not the modrm at 0x709BD6).
			planBytes(0x709BD7, { 0x82 }, { 0xC2 }, "setter 2 stride");        // lea r8,[rdx+rax*4] -> *8  (SIB 82->C2)
			{
				// cave: push rcx; mov rcx,[rsi+0A74h]; or [rcx+r8*4], eax; pop rcx ; jmp back to 0x709BE7
				std::vector<uint8_t> c = { 0x51,  0x48,0x8B,0x8E,0x74,0x0A,0x00,0x00,  0x42,0x09,0x04,0x81,  0x59 };
				uintptr_t cave = emitCave(c, base + 0x709BE7);
				planDetour(0x709BDF, kSetter2Stock, sizeof(kSetter2Stock), cave, "setter 2");
			}
			// alloc/clear+bitset (sub_180709C90 body 0x709CB4..0x709D06): detour to cave.
			//  in: r8=coll, r9d=item, r10w=bit ; out: eax=item ; then ret.
			{
				// NB: original block (0x709CB4..0x709D06) clobbers rax/rcx/rdx/r8 only - it PRESERVES r11.
				// Use exactly those scratch regs (r8 for the bit) so LTCG callers keeping a value in r11
				// across this call aren't corrupted.
				std::vector<uint8_t> c;
				c.insert(c.end(), { 0x49,0x8B,0x80,0x74,0x0A,0x00,0x00 });    // mov rax,[r8+0A74h]  (ptr)
				c.insert(c.end(), { 0x49,0x0F,0xBF,0xC9 });                   // movsx rcx,r9w        (item)
				c.insert(c.end(), { 0x48,0xC1,0xE1,0x05 });                   // shl rcx,5            (32*item)
				c.insert(c.end(), { 0x48,0x03,0xC1 });                        // add rax,rcx          (bitvector base)
				c.insert(c.end(), { 0x33,0xC9 });                            // xor ecx,ecx
				c.insert(c.end(), { 0x48,0x89,0x08 });                       // mov [rax],rcx        (zero 32 bytes)
				c.insert(c.end(), { 0x48,0x89,0x48,0x08 });                  // mov [rax+8],rcx
				c.insert(c.end(), { 0x48,0x89,0x48,0x10 });                  // mov [rax+10h],rcx
				c.insert(c.end(), { 0x48,0x89,0x48,0x18 });                  // mov [rax+18h],rcx
				c.insert(c.end(), { 0x49,0x0F,0xBF,0xD2 });                   // movsx rdx,r10w       (bit)
				c.insert(c.end(), { 0x41,0x89,0xD0 });                       // mov r8d,edx          (save bit)
				c.insert(c.end(), { 0xC1,0xEA,0x05 });                       // shr edx,5            (bit>>5)
				c.insert(c.end(), { 0x48,0x8D,0x04,0x90 });                  // lea rax,[rax+rdx*4]  (dword addr)
				c.insert(c.end(), { 0xBA,0x01,0x00,0x00,0x00 });             // mov edx,1
				c.insert(c.end(), { 0x44,0x89,0xC1 });                       // mov ecx,r8d          (bit)
				c.insert(c.end(), { 0x83,0xE1,0x1F });                       // and ecx,1Fh
				c.insert(c.end(), { 0xD3,0xE2 });                            // shl edx,cl           (1<<(bit&31))
				c.insert(c.end(), { 0x09,0x10 });                            // or [rax],edx         (set bit)
				c.insert(c.end(), { 0x44,0x89,0xC8 });                       // mov eax,r9d          (return item)
				c.insert(c.end(), { 0xC3 });                                 // ret
				uintptr_t cave = emitCave(c, 0);                             // self-contained (ret), no jmp back
				planDetour(0x709CB4, kAllocBodyStock, sizeof(kAllocBodyStock), cave, "item alloc/clear body");   // 82 bytes replaced
			}
			// walker (sub_1807F1610): read 8 dwords instead of 4. Two `cmp bx,4` (66 83 FB 04) at
			// 0x7F1707 (imm @0x7F170A) and 0x7F1718 (imm @0x7F171B).
			planBytes(0x7F170A, { 0x04 }, { 0x08 }, "walker count 1");         // cmp bx,4 -> 8
			planBytes(0x7F171B, { 0x04 }, { 0x08 }, "walker count 2");         // cmp bx,4 -> 8

			// char map: reader movsx->movzx + getter cave (0xFF -> -1)
			planBytes(0x708A99, { 0x0F,0xBE }, { 0x0F,0xB6 }, "cluster map reader");   // movsx->movzx byte [rcx+rbp+30h]
			{
				// getter sub_1807092F0 (10B): movsx rax,dx ; movzx eax,[rax+rcx+30h] ; if ==0xFF -> -1 ; ret
				std::vector<uint8_t> c = {
					0x48,0x0F,0xBF,0xC2,                 // movsx rax,dx
					0x0F,0xB6,0x44,0x08,0x30,            // movzx eax,[rax+rcx+30h]
					0x3C,0xFF,                           // cmp al,0FFh
					0x75,0x03,                           // jne +3
					0x83,0xC8,0xFF,                      // or eax,-1  (NONE)
					0xC3 };                              // ret
				uintptr_t cave = emitCave(c, 0);
				planDetour(0x7092F0, kMapGetterStock, sizeof(kMapGetterStock), cave, "cluster map getter");
			}
			FlushInstructionCache(GetCurrentProcess(), cavePage, CAVE_PAGE);

			// ---------- pointer slots (written AFTER the accessors, so they deref correctly) ----------
			for (auto& e : cb)
			{
				planSlot(e.coll + OFF_SUBPART_PTR, (uint64_t)e.sub);
				planSlot(e.coll + OFF_ITEMBV_PTR, (uint64_t)e.itm);
			}

			// ---------- cluster cap LAST (128 -> 255) ----------
			planBytes(CLUSTERCAP_IMM_RVA, { 0x80 }, { 0xFF }, "cluster cap");
		}

		// a plan that was never written (or was undone inside its window): nothing references the buffers or the cave
		void discardPlan()
		{
			for (void* p : bufs) VirtualFree(p, 0, MEM_RELEASE);
			bufs.clear();
			if (cavePage) { VirtualFree(cavePage, 0, MEM_RELEASE); cavePage = nullptr; }
			caveUsed = 0; code.clear(); slots.clear();
		}

		// ---------------------------------------------------------------- which image is this about?
		bool patchLive() const   // our halo2.dll is mapped and every code site still holds our bytes
		{
			if (!h2safe::moduleMappedAt(base)) return false;
			for (const Write& w : code) if (!h2safe::equals(w.addr, w.patch, w.len)) return false;
			return !code.empty();
		}
		bool stockAgain() const   // a fresh image at the same address
		{
			if (!h2safe::moduleMappedAt(base)) return false;
			for (const Write& w : code) if (!h2safe::equals(w.addr, w.orig, w.len)) return false;
			return true;
		}
		// Drop the state without touching the game. unreferenced: nothing can reach the buffers / cave any more -> they
		// are freed later. Otherwise they are LEAKED on purpose (code we no longer understand may still point at them).
		void forget(bool unreferenced)
		{
			if (unreferenced) { for (void* p : bufs) freeLater.push_back(p); if (cavePage) freeLater.push_back(cavePage); }
			bufs.clear(); cavePage = nullptr; caveUsed = 0; code.clear(); slots.clear(); applied = false;
		}
		// MCC unloads halo2.dll when a game ends and loads it again (often elsewhere) for the next one; the "left Halo 2"
		// event can come before or after that. What we hold must describe the image that is mapped NOW.
		void syncModule(uintptr_t now)
		{
			if (!applied) return;
			if (now == base && patchLive()) return;
			const bool unreferenced = now != base || stockAgain();
			Dbg::get().logf("the patched halo2.dll (%p) is %s - state dropped", (void*)base, unreferenced ? "gone" : "no longer as we left it (buffers leaked)");
			forget(unreferenced);
		}

		// ---------------------------------------------------------------- the two operations
		void patchCode()
		{
			Dbg& d = Dbg::get();
			d.logf("===== APPLY (base=%p) =====", (void*)base);
			try { planAll(); }
			catch (...) { discardPlan(); throw; }

			// every site was just read as stock, so nothing can reach what an earlier revert left behind: free it
			for (void* p : freeLater) VirtualFree(p, 0, MEM_RELEASE);
			freeLater.clear();

			// ONE quiet window: no thread is executing, or will return into, the renderer or our cave (H2QuietWindow.h) -
			// stage 1 MOVES arrays one frame's visibility pass first writes and then reads (crash 2026-11-11, halo2+0x7093D7)
			int tries = 0, rc = -1;
			{
				Write* const c = code.data(); const size_t nc = code.size();
				Write* const s = slots.data(); const size_t ns = slots.size();
				const uintptr_t caveLo = (uintptr_t)cavePage;
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 600, tries, caveLo, caveLo + CAVE_PAGE);
				if (window) rc = commitWindow(base, c, nc, s, ns);
			}   // the window closes here: everything runs again

			if (rc == 0)
			{
				applied = true;
				d.logf("===== APPLY done (%u code sites, %u slots, cave used 0x%X, quiet window after %d tries) =====", (unsigned)code.size(), (unsigned)slots.size(), (unsigned)caveUsed, tries);
				return;
			}
			if (rc == 3)
			{
				forget(false);   // an unknown mix may reference the buffers and the cave: leaked, never freed
				throw HCMRuntimeException("Uncap Cluster Limit: a write to halo2.dll failed and could not be undone - restart the game.");
			}
			discardPlan();
			if (rc < 0)
			{
				d.logf("  no quiet window in %d tries - nothing patched", tries);
				throw HCMRuntimeException("Uncap Cluster Limit: could not find a safe moment to patch (the game never left its visibility pass). Nothing was changed - toggle it off and on again.");
			}
			if (rc == 1) throw HCMRuntimeException("Uncap Cluster Limit: halo2.dll changed or was unloaded while waiting - nothing was changed.");
			throw HCMRuntimeException("Uncap Cluster Limit: a write to halo2.dll failed and was undone - nothing was changed.");
		}

		// Grow the clusters sub-collection (coll+0x10) to SUBCOLL_NEWCAP using game-heap arrays. Idempotent;
		// per-map (the engine reallocates it at 128 each map, so we re-grow on each apply).
		void resizeClusterSubcoll()
		{
			Dbg& d = Dbg::get();

			// Phase 1 (threads LIVE): allocate + zero the new bigger game-heap arrays. gameAlloc takes a heap lock a
			// suspended thread could be holding, so it MUST run outside the suspend window. Zeroing means a read past
			// the live count (up to the raised cap) never hits uninitialised game-heap garbage.
			std::vector<Swap> swaps;
			for (uint32_t c : COLLS)
			{
				uintptr_t subp = 0; uint32_t cap = 0; uint16_t cnt = 0;
				if (!h2safe::copy(&subp, (const void*)(base + c + CLUSTER_SUBCOLL_OFF), 8) || !subp) continue;
				if (!h2safe::copy(&cap, (const void*)subp, 4) || !h2safe::copy(&cnt, (const void*)(subp + 4), 2)) continue;
				if (cap == 0 || cap >= (uint32_t)SUBCOLL_NEWCAP || cnt > cap) continue;   // already grown / not ready
				Swap s{}; s.subp = subp;
				bool ok = true;
				for (int i = 0; i < 4; ++i) { s.n[i] = gameAlloc(kSwapElem[i] * SUBCOLL_NEWCAP); ok = ok && s.n[i]; }
				if (!ok) { for (int i = 0; i < 4; ++i) gameFree(s.n[i]); d.logf("  subcoll grow: gameAlloc fail"); continue; }
				for (int i = 0; i < 4; ++i) memset(s.n[i], 0, kSwapElem[i] * SUBCOLL_NEWCAP);
				swaps.push_back(s);
			}
			if (swaps.empty()) return;

			// Phase 2: copy the live entries and repoint, in a quiet window - no thread can still hold the old arrays, or add
			// an entry between the count read and the copy. Without a window nothing is swapped (the first version went
			// ahead with a plain suspension).
			int tries = 0; bool quiet = false;
			{
				Swap* const sw = swaps.data(); const size_t n = swaps.size();
				const uintptr_t caveLo = (uintptr_t)cavePage;
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 300, tries, caveLo, cavePage ? caveLo + CAVE_PAGE : 0);
				quiet = window != nullptr;
				if (quiet) commitSwaps(sw, n);
			}

			bool allDone = true;
			for (Swap& s : swaps)
			{
				if (s.done)
				{
					for (int i = 0; i < 4; ++i) visOld.push_back((void*)s.o[i]);
					d.logf("  clusters sub-coll: -> %d (cnt %u, quiet window after %d tries)", SUBCOLL_NEWCAP, s.cnt, tries);
				}
				else
				{
					allDone = false;
					for (int i = 0; i < 4; ++i) gameFree(s.n[i]);   // never installed
					d.logf("  clusters sub-coll NOT grown (%s)", quiet ? "it changed before the window" : "no quiet window");
				}
			}
			if (!quiet)
				throw HCMRuntimeException("Uncap Cluster Limit is on, but this map's cluster list could not be grown (the game never left its renderer). Toggle it off and on again.");
			(void)allDone;
		}

	public:
		bool isApplied() const { return applied; }

		bool apply()
		{
			const uintptr_t now = (uintptr_t)GetModuleHandleA("halo2.dll");
			syncModule(now);
			base = now;
			if (!base) return false;
			drainVisOld();   // free last cycle's orphaned sub-coll arrays (via the game's own deallocator)

			// ---- the .text patches + buffers are done ONCE (they persist across maps) ----
			if (!applied) patchCode();

			// ---- per-map: grow the clusters sub-collection (game-heap, re-created at 128 each map) ----
			resizeClusterSubcoll();
			return true;
		}

		// Back to stock. Returns normally when nothing of ours is installed any more; THROWS (patch still on, nothing
		// touched) when no safe moment came up.
		void revert()
		{
			syncModule((uintptr_t)GetModuleHandleA("halo2.dll"));
			if (!applied) return;
			Dbg& d = Dbg::get(); d.logf("===== REVERT =====");

			int tries = 0, rc = -1;
			{
				Write* const c = code.data(); const size_t nc = code.size();
				Write* const s = slots.data(); const size_t ns = slots.size();
				const uintptr_t caveLo = (uintptr_t)cavePage;
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 600, tries, caveLo, caveLo + CAVE_PAGE);
				if (window) rc = restoreWindow(base, c, nc, s, ns);
			}

			if (rc < 0)
			{
				d.logf("  no quiet window in %d tries - still on", tries);
				throw HCMRuntimeException("Uncap Cluster Limit: could not find a safe moment to restore the stock limit (the game never left its renderer). It is still ON - toggle it off again.");
			}
			if (rc == 2) throw HCMRuntimeException("Uncap Cluster Limit: a write to halo2.dll failed while restoring - it is still ON.");
			if (rc == 3)
			{
				forget(false);
				throw HCMRuntimeException("Uncap Cluster Limit: a write to halo2.dll failed and could not be undone - restart the game.");
			}
			if (rc == 1)   // unloaded, or changed, between the check above and the window
			{
				d.logf("  the image went away before the window - state dropped");
				forget(!h2safe::moduleMappedAt(base));
				return;
			}

			// stock again, restored with no thread in the renderer or the cave: nothing references the buffers or the cave
			forget(true);
			// the enlarged clusters sub-coll stays (harmless with cap back at 128; engine frees per map);
			// free the orphaned ORIGINAL sub-coll arrays via the game's deallocator now.
			drainVisOld();
			d.logf("===== REVERT done (quiet window after %d tries) =====", tries);
		}

		// A map (re)loaded / we left the game: the game already freed last map's orphaned arrays in its per-map arena
		// teardown, so DON'T hand them to the game deallocator again (double-free). Just drop the stale handles.
		void discardVisOldNoFree() { visOld.clear(); }

		// Left Halo 2. If its dll is already gone the state goes with it; if it is still mapped with our patch in it, the
		// state is KEPT (the first version forgot it, and then could neither revert nor recognise its own patch).
		void onLeave()
		{
			discardVisOldNoFree();
			syncModule((uintptr_t)GetModuleHandleA("halo2.dll"));
		}

		~Patcher()
		{
			try { revert(); } catch (...) {}
			// Still installed (no safe moment, or a restore that failed): the game's code references the buffers and the
			// cave, so they are LEAKED on purpose - they are VirtualAlloc'd, not part of HCMInternal.dll.
			if (applied) return;
			for (void* p : freeLater) VirtualFree(p, 0, MEM_RELEASE);
		}
	};
} // namespace


template <GameState::Value gameT>
class UncapClusterLimitImpl : public IUncapClusterLimitImpl
{
private:
	GameState mGame;
	std::mutex mPatchMutex;   // the toggle arrives on the render thread and on hotkeys, MCC's state event on MCC's thread
	Patcher mPatcher;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	void onToggle(bool& newValue)
	{
		std::scoped_lock lock(mPatchMutex);
		try
		{
			if (newValue) { if (!mPatcher.apply()) { lockOrThrow(messagesGUIWeak, m); m->addMessage("Uncap Cluster Limit: load Halo 2 first, then toggle on."); return; } }
			else mPatcher.revert();
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			// turning it off did not happen: make the checkbox say so (set without firing the event again)
			if (!newValue && mPatcher.isApplied())
				try { lockOrThrow(settingsWeak, settings); settings->uncapClusterLimitToggle->GetValueDisplay() = true; settings->uncapClusterLimitToggle->GetValue() = true; } catch (...) {}
			return;
		}
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Region cluster limit raised to 255" : "Region cluster limit restored"); }
		catch (HCMRuntimeException&) {}
	}

	void onMCCStateChanged(const MCCState& s)
	{
		std::scoped_lock lock(mPatchMutex);
		try
		{
			if (s.currentGameState != mGame) { mPatcher.onLeave(); return; } // left Halo 2 -> drop handles (game frees its arrays)
			// Only (re)apply once we're FULLY in-game. During a save-and-quit / loading screen the game is tearing
			// down or rebuilding its cluster arrays, and touching them mid-transition is unsafe - so we wait.
			if (s.currentPlayState != PlayState::Ingame) return;
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapClusterLimitToggle->GetValue())
			{
				// map (re)load: the previous map's orphaned game-heap arrays were freed by the game's teardown, so
				// drop them WITHOUT re-freeing (apply()'s drainVisOld would otherwise double-free them).
				mPatcher.discardVisOldNoFree();
				mPatcher.apply();
			}
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "UncapClusterLimit MCCState: " << ex.what(); }
	}

public:
	UncapClusterLimitImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapClusterLimitToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}
	~UncapClusterLimitImpl()
	{
		std::scoped_lock lock(mPatchMutex);
		try { mPatcher.revert(); } catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


UncapClusterLimit::UncapClusterLimit(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<UncapClusterLimitImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException("UncapClusterLimit not impl for this game");
	}
}
UncapClusterLimit::~UncapClusterLimit() { PLOG_DEBUG << "~" << getName(); }
