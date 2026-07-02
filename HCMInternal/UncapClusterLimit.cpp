#include "pch.h"
#include "UncapClusterLimit.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include <Windows.h>
#include <tlhelp32.h>
#include <vector>
#include <array>
#include <atomic>

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
//  TOGGLE-OFF is drained in a crash-safe order: cluster cap -> 128 FIRST, then live
//  counts zeroed (so stock code walks nothing for a frame), then code reverted, then
//  pointer slots restored, then buffers freed deferred. This kills the "first tick
//  sees a >128 count against 128-sized stock arrays" crash.
//
//  The 255 ceiling is the byte-wide clusters_to_region_clusters map (coll+0x30, 0xFF=NONE):
//  going higher would need that map widened byte->word plus a wider bitvector/walker.
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

	// clusters sub-collection (coll+0x10) gates how many clusters get SUBMITTED (stock cap 128 - the
	// one sub-coll UncapVisibilityLimits deliberately leaves alone). Resize per-map from the GAME heap
	// (sub_1800769E0) so the engine frees the bigger arrays normally on teardown.
	constexpr uint32_t CLUSTER_SUBCOLL_OFF = 0x10;
	constexpr int      SUBCOLL_NEWCAP = 2048;
	constexpr uint32_t GAME_ALLOC = 0x769E0;   // sub_1800769E0(size)->ptr
	constexpr uint32_t GAME_FREE = 0x76E30;    // sub_180076E30(ptr)

	// quiet logger: routes to HCM's PLOG at debug level (no separate file, no crash-VEH). The unused
	// members (base/critical/cave*/veh) keep the call sites compiling with zero behavioural cost.
	struct Dbg
	{
		uintptr_t base = 0, caveStart = 0, caveSize = 0;
		std::atomic<bool> critical{ false };
		void* veh = nullptr;
		static Dbg& get() { static Dbg d; return d; }
		void open() {}
		void logf(const char* fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); wvsprintfA(b, fmt, ap); va_end(ap); PLOG_DEBUG << "[UncapClusterLimit] " << b; }
	};

	// pack helpers
	static void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24)); }
	static void put64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i))); }

	class Patcher
	{
		uintptr_t base = 0;
		bool applied = false;
		void* cavePage = nullptr;              // executable, within +-2GB of halo2.dll
		size_t caveUsed = 0;
		std::vector<void*> bufs;               // our data buffers (item-bv + subpart, per coll)
		std::vector<void*> bufsOld;            // deferred-free after revert
		std::vector<void*> visOld;             // orphaned original sub-coll arrays (game-heap, deferred free)

		void* gameAlloc(size_t n) { return ((void* (*)(int64_t))(base + GAME_ALLOC))((int64_t)n); }
		void  gameFree(void* p) { if (p) ((void (*)(void*))(base + GAME_FREE))(p); }
		void  drainVisOld() { if (base && GetModuleHandleA("halo2.dll")) for (void* p : visOld) gameFree(p); visOld.clear(); }

		// Grow the clusters sub-collection (coll+0x10) to SUBCOLL_NEWCAP using game-heap arrays. Idempotent;
		// per-map (the engine reallocates it at 128 each map, so we re-grow on each apply). Layout:
		//   cap u32 @+0 ; count u16 @+4 ; A @+8 (2*cap) ; B @+0x10 (4*cap) ; C @+0x18 (8*cap) ; D @+0x20 (2*cap)
		void resizeClusterSubcoll()
		{
			Dbg& d = Dbg::get();
			for (uint32_t c : COLLS)
			{
				uintptr_t subp = *(uintptr_t*)(base + c + CLUSTER_SUBCOLL_OFF);
				if (!subp) continue;
				uint32_t cap = *(uint32_t*)subp; uint16_t cnt = *(uint16_t*)(subp + 4);
				if (cap == 0 || cap >= (uint32_t)SUBCOLL_NEWCAP || cnt > cap) continue;   // already grown / not ready
				uintptr_t oA = *(uintptr_t*)(subp + 8), oB = *(uintptr_t*)(subp + 0x10), oC = *(uintptr_t*)(subp + 0x18), oD = *(uintptr_t*)(subp + 0x20);
				void* nA = gameAlloc(2 * SUBCOLL_NEWCAP); void* nB = gameAlloc(4 * SUBCOLL_NEWCAP);
				void* nC = gameAlloc(8 * SUBCOLL_NEWCAP); void* nD = gameAlloc(2 * SUBCOLL_NEWCAP);
				if (!(nA && nB && nC && nD)) { gameFree(nA); gameFree(nB); gameFree(nC); gameFree(nD); d.logf("  subcoll grow: gameAlloc fail"); continue; }
				memcpy(nA, (void*)oA, (size_t)cnt * 2); memcpy(nB, (void*)oB, (size_t)cnt * 4);
				memcpy(nC, (void*)oC, (size_t)cnt * 8); memcpy(nD, (void*)oD, (size_t)cnt * 2);
				*(uintptr_t*)(subp + 8) = (uintptr_t)nA; *(uintptr_t*)(subp + 0x10) = (uintptr_t)nB;
				*(uintptr_t*)(subp + 0x18) = (uintptr_t)nC; *(uintptr_t*)(subp + 0x20) = (uintptr_t)nD;
				*(uint32_t*)subp = SUBCOLL_NEWCAP;
				visOld.push_back((void*)oA); visOld.push_back((void*)oB); visOld.push_back((void*)oC); visOld.push_back((void*)oD);
				d.logf("  clusters sub-coll 0x%X: %u -> %d (cnt %u)", (unsigned)c, cap, SUBCOLL_NEWCAP, cnt);
			}
		}
		// byte-field undo: (addr, original bytes)
		std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> undo;
		// pointer-slot undo: (addr, original 8 bytes)
		std::vector<std::pair<uintptr_t, std::array<uint8_t, 8>>> slotUndo;

		static void writeRaw(uintptr_t addr, const void* data, size_t len)
		{
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len); VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
		}
		void patchBytes(uintptr_t addr, std::initializer_list<uint8_t> bytes)
		{
			std::vector<uint8_t> old(bytes.size());
			memcpy(old.data(), (void*)addr, bytes.size());
			undo.emplace_back(addr, old);
			std::vector<uint8_t> nb(bytes);
			writeRaw(addr, nb.data(), nb.size());
		}
		// find the 4-byte LE `stock` in [rva, rva+9], patch to `ours`. returns false if not found.
		bool patchField(uint32_t rva, uint32_t stock, uint32_t ours, const char* tag)
		{
			uint8_t s[4] = { (uint8_t)stock,(uint8_t)(stock >> 8),(uint8_t)(stock >> 16),(uint8_t)(stock >> 24) };
			for (int off = 0; off <= 9; ++off)
			{
				uintptr_t a = base + rva + off;
				if (memcmp((void*)a, s, 4) == 0)
				{
					std::vector<uint8_t> old(4); memcpy(old.data(), (void*)a, 4);
					undo.emplace_back(a, old);
					writeRaw(a, &ours, 4);
					return true;
				}
			}
			Dbg::get().logf("  patchField MISS %s rva=0x%X", tag, rva);
			return false;
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
		// write `code` into the cave page; returns its VA. jmpBack (if nonzero) appended as jmp rel32.
		uintptr_t emitCave(std::vector<uint8_t> code, uintptr_t jmpBack)
		{
			uintptr_t caveVA = (uintptr_t)cavePage + caveUsed;
			if (jmpBack)
			{
				uintptr_t after = caveVA + code.size() + 5;
				int32_t rel = (int32_t)(jmpBack - after);
				code.push_back(0xE9); put32(code, (uint32_t)rel);
			}
			writeRaw(caveVA, code.data(), code.size());
			caveUsed += code.size() + 16;               // pad between caves
			caveUsed = (caveUsed + 15) & ~size_t(15);
			return caveVA;
		}
		// detour: replace `origLen` bytes at siteVA (>=5) with jmp->caveVA + nop pad
		void detour(uintptr_t siteVA, size_t origLen, uintptr_t caveVA)
		{
			std::vector<uint8_t> old(origLen); memcpy(old.data(), (void*)siteVA, origLen);
			undo.emplace_back(siteVA, old);
			std::vector<uint8_t> jb; jb.push_back(0xE9); put32(jb, (uint32_t)(int32_t)(caveVA - (siteVA + 5)));
			while (jb.size() < origLen) jb.push_back(0x90);
			writeRaw(siteVA, jb.data(), jb.size());
		}
		void storeSlot(uintptr_t addr, uint64_t val)
		{
			std::array<uint8_t, 8> old{}; memcpy(old.data(), (void*)addr, 8);
			slotUndo.emplace_back(addr, old);
			DWORD o; VirtualProtect((void*)addr, 8, PAGE_READWRITE, &o);
			*(uint64_t*)addr = val; VirtualProtect((void*)addr, 8, o, &o);
		}

		// live-code patching races the render thread (proven: an AV fired mid-apply). Suspend every other
		// thread in the process for the (memory-only) patch window. Buffers/caves are alloc'd BEFORE this
		// (VirtualAlloc needs locks a suspended thread might hold); patching itself only touches memory.
		std::vector<HANDLE> suspended;
		void suspendOthers()
		{
			suspended.clear();
			DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) return;
			THREADENTRY32 te{}; te.dwSize = sizeof(te);
			if (Thread32First(snap, &te))
				do {
					if (te.th32OwnerProcessID == pid && te.th32ThreadID != me)
					{
						HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
						if (h) { if (SuspendThread(h) != (DWORD)-1) suspended.push_back(h); else CloseHandle(h); }
					}
				} while (Thread32Next(snap, &te));
			CloseHandle(snap);
		}
		void resumeOthers() { for (HANDLE h : suspended) { ResumeThread(h); CloseHandle(h); } suspended.clear(); }

	public:
		bool apply()
		{
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;
			Dbg& d = Dbg::get(); d.open(); d.base = base;
			drainVisOld();   // free last cycle's orphaned sub-coll arrays (via the game's own deallocator)

			// ---- the .text patches + buffers are done ONCE (they persist across maps) ----
			if (!applied)
			{
			d.logf("===== APPLY (base=%p) =====", (void*)base);
			d.critical.store(true);

			// free any buffers queued from a prior revert
			for (void* p : bufsOld) VirtualFree(p, 0, MEM_RELEASE);
			bufsOld.clear();

			// wrong-build guard: verify the cluster-cap byte before we touch anything
			if (*(uint8_t*)(base + CLUSTERCAP_IMM_RVA) != 0x80)
			{
				d.critical.store(false);
				throw HCMRuntimeException("UncapClusterLimit: halo2.dll doesn't match build 1.3528 (cluster-cap byte); refusing.");
			}

			cavePage = allocNear(base, 0x1000);
			if (!cavePage) { d.critical.store(false); throw HCMRuntimeException("UncapClusterLimit: near-cave alloc failed."); }
			d.caveStart = (uintptr_t)cavePage; d.caveSize = 0x1000;
			caveUsed = 0; undo.clear(); slotUndo.clear();

			// allocate our data buffers BEFORE suspending threads (VirtualAlloc needs locks a
			// suspended thread could be holding).
			struct CollBuf { uintptr_t coll; void* sub; void* itm; };
			std::vector<CollBuf> cb;
			for (uint32_t c : COLLS)
			{
				void* sub = VirtualAlloc(0, SUBPART_BUFBYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				void* itm = VirtualAlloc(0, ITEMBV_BUFBYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (!sub || !itm) { d.critical.store(false); throw HCMRuntimeException("UncapClusterLimit: buffer alloc failed."); }
				memset(sub, 0, SUBPART_BUFBYTES); memset(itm, 0, ITEMBV_BUFBYTES);
				bufs.push_back(sub); bufs.push_back(itm);
				cb.push_back({ base + c, sub, itm });
			}

			// suspend every other thread for the memory-only patch window (avoids the render thread
			// executing a half-written detour / getter rewrite).
			suspendOthers();

			// ---------- STAGE 1: in-slack index/volume relocation ----------
			applyStage1();

			// ---------- STAGE 2 code patches (cluster cap raised LAST, below) ----------
			// subpart pool: lea->mov + budget 512->4096 + clear lea->mov
			patchBytes(base + 0x70931C, { 0x8B });                 // sub_180709300: lea rax,[rcx+270h] -> mov
			patchField(0x709316, 0x200, SUBPART_BUFDW, "subpart cap");
			patchBytes(base + 0x708132, { 0x8B });                 // sub_180707F50: lea rcx,[rdi+270h] -> mov

			// item bitvector: getter rewritten in-place (18B -> 16B + nops), stride 16->32
			{
				std::vector<uint8_t> g = { 0x48,0x0F,0xBF,0xC2, 0x48,0xC1,0xE0,0x05, 0x48,0x03,0x81,0x74,0x0A,0x00,0x00, 0xC3 };
				std::vector<uint8_t> old(18); memcpy(old.data(), (void*)(base + 0x709D20), 18);
				undo.emplace_back(base + 0x709D20, old);
				while (g.size() < 18) g.push_back(0x90);
				writeRaw(base + 0x709D20, g.data(), g.size());
			}
			// setter#1 (sub_1807078A0): stride *4->*8, then detour the 'or' to a deref cave
			patchBytes(base + 0x707E52, { 0xC8 });                 // lea r8,[rax+rcx*4] -> *8
			{
				// cave: push rax; mov rax,[rdi+0A74h]; or [rax+r8*4], edx; pop rax ; jmp back to 0x707E64
				// (the stock `or` clobbered no registers, so preserve rax rather than assume it's dead)
				std::vector<uint8_t> c = { 0x50,  0x48,0x8B,0x87,0x74,0x0A,0x00,0x00,  0x42,0x09,0x14,0x80,  0x58 };
				uintptr_t cave = emitCave(c, base + 0x707E64);
				detour(base + 0x707E5C, 8, cave);
			}
			// setter#2 (sub_1807098C0): stride *4->*8, then detour the 'or' to a deref cave (scratch rcx).
			// instr `4C 8D 04 82` @0x709BD4 -> SIB byte (82) is at 0x709BD7 (not the modrm at 0x709BD6).
			patchBytes(base + 0x709BD7, { 0xC2 });                 // lea r8,[rdx+rax*4] -> *8  (SIB 82->C2)
			{
				// cave: push rcx; mov rcx,[rsi+0A74h]; or [rcx+r8*4], eax; pop rcx ; jmp back to 0x709BE7
				std::vector<uint8_t> c = { 0x51,  0x48,0x8B,0x8E,0x74,0x0A,0x00,0x00,  0x42,0x09,0x04,0x81,  0x59 };
				uintptr_t cave = emitCave(c, base + 0x709BE7);
				detour(base + 0x709BDF, 8, cave);
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
				detour(base + 0x709CB4, 0x709D06 - 0x709CB4, cave);          // 82 bytes replaced
			}
			// walker (sub_1807F1610): read 8 dwords instead of 4. Two `cmp bx,4` (66 83 FB 04) at
			// 0x7F1707 (imm @0x7F170A) and 0x7F1718 (imm @0x7F171B).
			patchBytes(base + 0x7F170A, { 0x08 });                 // cmp bx,4 -> 8
			patchBytes(base + 0x7F171B, { 0x08 });                 // cmp bx,4 -> 8

			// char map: reader movsx->movzx + getter cave (0xFF -> -1)
			patchBytes(base + 0x708A99, { 0x0F,0xB6 });            // movsx->movzx byte [rcx+rbp+30h]
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
				detour(base + 0x7092F0, 10, cave);
			}

			// ---------- pointer slots (AFTER accessors patched so they deref correctly) ----------
			for (auto& e : cb)
			{
				storeSlot(e.coll + OFF_SUBPART_PTR, (uint64_t)e.sub);
				storeSlot(e.coll + OFF_ITEMBV_PTR, (uint64_t)e.itm);
			}

			// ---------- cluster cap LAST (128 -> 255) ----------
			patchBytes(base + CLUSTERCAP_IMM_RVA, { 0xFF });

			resumeOthers();
			for (auto& e : cb) d.logf("  coll 0x%X: subpart=%p itembv=%p", (unsigned)(e.coll - base), e.sub, e.itm);

			applied = true; d.critical.store(false);
			d.logf("===== APPLY done (%u byte-fields, %u slots, cave used 0x%X) =====", (unsigned)undo.size(), (unsigned)slotUndo.size(), (unsigned)caveUsed);
			} // end if (!applied)

			// ---- per-map: grow the clusters sub-collection (game-heap, re-created at 128 each map) ----
			resizeClusterSubcoll();
			return true;
		}

		void revert()
		{
			if (!applied) return;
			Dbg& d = Dbg::get(); d.logf("===== REVERT (ordered drain) =====");
			bool live = base && GetModuleHandleA("halo2.dll");
			d.critical.store(true);
			if (live) suspendOthers();   // same race protection for the drain

			// 1) cluster cap -> 128 FIRST, so no new region can exceed the stock arrays.
			const uint8_t stockCap = 0x80;
			if (live) writeRaw(base + CLUSTERCAP_IMM_RVA, &stockCap, 1);

			// 2) zero the live per-collection counts/cursors so stock code walks nothing for a frame.
			if (live)
				for (uint32_t c : COLLS)
				{
					uintptr_t coll = base + c;
					*(uint16_t*)(coll + 0xA70) = 0;   // subpart pool cursor
					*(uint16_t*)(coll + 0xA72) = 0;   // item count
					uintptr_t proj = *(uintptr_t*)coll; // coll[0] = projbuf
					if (proj) { *(uint16_t*)(proj + 0xA6C) = 0; *(uint16_t*)(proj + 0x2860) = 0; } // cluster & (in-slack) volume counts
				}

			// 3) revert all code byte-fields (reverse order) - accessors go back to inline/stock.
			if (live)
				for (auto it = undo.rbegin(); it != undo.rend(); ++it)
					writeRaw(it->first, it->second.data(), it->second.size());
			undo.clear();

			// 4) restore the pointer slots' original inline bytes.
			if (live)
				for (auto it = slotUndo.rbegin(); it != slotUndo.rend(); ++it)
				{
					DWORD o; VirtualProtect((void*)it->first, 8, PAGE_READWRITE, &o);
					memcpy((void*)it->first, it->second.data(), 8); VirtualProtect((void*)it->first, 8, o, &o);
				}
			slotUndo.clear();

			if (live) resumeOthers();

			// the enlarged clusters sub-coll stays (harmless with cap back at 128; engine frees per map);
			// free the orphaned ORIGINAL sub-coll arrays via the game's deallocator now.
			drainVisOld();

			// 5) free buffers deferred (engine may still touch them this frame); cave page freed now.
			for (void* p : bufs) bufsOld.push_back(p);
			bufs.clear();
			if (cavePage) { VirtualFree(cavePage, 0, MEM_RELEASE); cavePage = nullptr; }

			applied = false; d.critical.store(false);
			d.logf("===== REVERT done =====");
		}

		void onLeave() { applied = false; undo.clear(); slotUndo.clear(); for (void* p : bufs) bufsOld.push_back(p); bufs.clear(); cavePage = nullptr; }
		~Patcher() { try { revert(); } catch (...) {} for (void* p : bufsOld) VirtualFree(p, 0, MEM_RELEASE); }

	private:
		// ---- STAGE 1 in-slack table (all RVAs verified vs 1.3528) ----
		void applyStage1()
		{
			constexpr uint32_t IDX_OLD = 0x1770, VCNT_OLD = 0x1970, VARR_OLD = 0x1974;
			constexpr uint32_t IDX_NEW = 0x2460, VCNT_NEW = 0x2860, VARR_NEW = 0x2864;
			constexpr uint32_t DELTA = VARR_NEW - VARR_OLD;
			const uint32_t idx[] = { 0x70826A,0x7082B9,0x709381,0x709390,0x797E94,0x7997F8,0x799992,0x799A2B };
			const uint32_t vcnt[] = { 0x797EE4,0x797F0C,0x799406,0x7998E1,0x7999AD,0x7999BF };
			const uint32_t varr[] = { 0x7094AA,0x70950D,0x797F13,0x7999C6,0x79A3FD };
			for (uint32_t r : idx) patchField(r, IDX_OLD, IDX_NEW, "IDX");
			for (uint32_t r : vcnt) patchField(r, VCNT_OLD, VCNT_NEW, "VCNT");
			for (uint32_t r : varr) patchField(r, VARR_OLD, VARR_NEW, "VARR");
			patchField(0x797F53, 0x1978, 0x1978 + DELTA, "SUBF1978");
			patchField(0x797FC6, 0x197C, 0x197C + DELTA, "SUBF197C");
			patchField(0x797F60, 0x1994, 0x1994 + DELTA, "SUBF1994");
			patchField(0x7F1994, 0x19EC, 0x19EC + DELTA, "SUBF19EC");
			for (uint32_t r : { 0x797EC5u,0x798027u,0x799949u,0x7999F3u }) patchField(r, 0x200, 496, "volcap");
		}
	};
} // namespace


template <GameState::Value gameT>
class UncapClusterLimitImpl : public IUncapClusterLimitImpl
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
			if (newValue) { if (!mPatcher.apply()) { lockOrThrow(messagesGUIWeak, m); m->addMessage("Uncap Cluster Limit: load Halo 2 first, then toggle on."); return; } }
			else mPatcher.revert();
		}
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {} return; }
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Region cluster limit raised to 255" : "Region cluster limit restored"); }
		catch (HCMRuntimeException&) {}
	}

	void onMCCStateChanged(const MCCState& s)
	{
		try
		{
			if (s.currentGameState != mGame) { mPatcher.onLeave(); return; }
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapClusterLimitToggle->GetValue()) mPatcher.apply();
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
	~UncapClusterLimitImpl() { try { mPatcher.revert(); } catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); } }
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
