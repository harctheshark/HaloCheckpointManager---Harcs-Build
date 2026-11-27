#include "pch.h"
#include "UncapDropShadows.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include "H2QuietWindow.h"
#include "H2SafeWrite.h"
#include "H2FadeListHooks.h"
#include <Windows.h>
#include <mutex>
#include <vector>
#include <cstring>

// ============================================================================
//  Halo 2 drop-shadow uncap (MCC halo2.dll, build 1.3528).  RE parity map with
//  the sapien original is in memory hcm-h2-shadow-uncap-port / visibility-projection-cap.
//
//  Stock pipeline:
//    sub_1807EB210  = drop-shadow render: collects up to 32 visible casters into a
//                     STACK buffer (v27, 20-byte entries), feeds the persistent fade
//                     manager, then draws. Collection cap `cmp r12,20h` @0x7EB429.
//    unk_181660C70  = fade manager: 4 categories x 1156 (0x484); each = 32 entries x
//                     36 bytes + count @+0x480. Add=sub_1807F6FE0, process=sub_1807F7400.
//    sub_180973DE0  = rasterizer scratch pool: 256KB bump allocator (cap @0x973E1F),
//                     buffer unk_1819CBF54. Too-small pool de-renders sky/water when
//                     many shadows queue.
//
//  We relocate the collection + rasterizer buffers to larger heap allocations. The engine
//  references those buffers through a handful of `lea` instructions; we replace each `lea`
//  with a `jmp` to a code cave that loads the heap address (`mov reg, imm64`) and jumps
//  back. Each `lea` runs exactly once per call, so the cave fires once - no per-iteration
//  reset. The collection cap and the rasterizer cap / memset size are in-place byte patching.
//
//  2026-11-19 (v2): THE FADE MANAGER IS NO LONGER PATCHED. Its two functions are shared with the dynamic lights' lists, and
//  could not pass 32 by patching (add tracks the frame's inputs in a 32-bit mask - inputs past the 32nd alias earlier
//  ones). The shadows' lists are now HCM's own 128-entry lists (H2FadeListHooks.cpp, H2FadeLists.h - the engine's logic,
//  proven identical at its own settings), and "Max Shadows" sets both the collection cap and how many may shine. The
//  ~45 patches the first version made inside those functions (counts +0x480 -> +0x10E0, stride, the list caps, the
//  fade-force cave) and the manager relocation are gone. Why 30 was the default: the user saw shadows glitch at 32 and
//  above with the first version - whether that was the 32-bit mask (gone now) or something further down the draw is not
//  known; raise Max Shadows to find out.
// ============================================================================

namespace
{
	// ---- sizes ----
	constexpr int     kNewCap = 120;                     // buffer layout width (was 32)
	constexpr uint8_t kEnforcedCap = 30;                 // actual enforced cap (held under the model_group 32 wall)
	constexpr uint32_t kNewCount = (uint32_t)(kNewCap * 36);          // 0x10E0  (count field offset)
	constexpr uint32_t kNewStride = (uint32_t)(kNewCap * 36 + 4);     // 0x10E4  (per-category stride)
	constexpr size_t kMgrSize = (size_t)kNewStride * 4;  // 4 categories
	constexpr size_t kCollSize = 0x1000;                 // 120*20 = 2400, one page
	// Rasterizer scratch pool (sub_180973DE0, stock 256KB). Kept MODEST at 2MB: it wasn't the shadow limiter
	// in halo2.dll, and a large cap (8/32MB) made the engine grow its downstream D3D buffer pool without bound
	// (MCC -> 10GB, persisting across map reload). 2MB = 8x headroom with the D3D growth kept small. Don't
	// raise this further without watching MCC's memory.
	constexpr size_t   kRastSize = 0x200000;             // 2 MB (was 0x40000 = 256KB)
	constexpr uint32_t kRastCap  = 0x200000;             // pool cap to match the buffer

	// ---- buffer selector for caves ----
	enum BufSel { SEL_MGR, SEL_COLL, SEL_COLL8, SEL_RAST };

	struct CaveSpec
	{
		uint32_t siteRVA;     // address of the lea to replace
		uint8_t  leaLen;      // 5 or 7
		uint8_t  movOp0, movOp1; // mov reg,imm64 prefix (REX + B8+r)
		BufSel   sel;
		uint32_t backRVA;     // instruction after the lea
	};

	// mov rax = 48 B8 | rcx = 48 B9 | rdx = 48 BA | rbx = 48 BB | r15 = 49 BF
	constexpr CaveSpec kCaves[] = {
		// collection write ptr (v27): lea r15,[rsp+38h] -> collBuf+8
		{ 0x7EB2DD, 5, 0x49, 0xBF, SEL_COLL8, 0x7EB2E2 },
		// collection base for mgr-add (v26): lea rdx,[rsp+30h] -> collBuf
		{ 0x7EB37F, 5, 0x48, 0xBA, SEL_COLL,  0x7EB384 },
		// collection base for mgr-process: lea rdx,[rsp+30h] -> collBuf
		{ 0x7EB394, 5, 0x48, 0xBA, SEL_COLL,  0x7EB399 },
		// collection draw ptr (v27): lea rbx,[rsp+38h] -> collBuf+8
		{ 0x7EB3B6, 5, 0x48, 0xBB, SEL_COLL8, 0x7EB3BB },
		// rasterizer pool resolve: lea rcx, unk_1819CBF54 -> rastBuf
		{ 0x973E47, 7, 0x48, 0xB9, SEL_RAST,  0x973E4E },
		// rasterizer pool reset/memset: lea rcx, unk_1819CBF54 -> rastBuf
		{ 0x973E86, 7, 0x48, 0xB9, SEL_RAST,  0x973E8D },
	};

	// ---- in-place dword replacements (scan a short window for an exact LE uint32) ----
	struct DwordPatch { uint32_t rva; uint8_t scanLen; uint32_t oldVal; uint32_t newVal; };
	constexpr uint32_t kCountSites[] = {
		// sub_1807F6FE0 (35)
		0x7F7000,0x7F706D,0x7F70C8,0x7F70CF,0x7F7134,0x7F714A,0x7F715A,0x7F716F,0x7F717F,0x7F7186,
		0x7F71B7,0x7F71CD,0x7F71DD,0x7F71F2,0x7F7202,0x7F7209,0x7F723B,0x7F7251,0x7F7261,0x7F7276,
		0x7F7286,0x7F728D,0x7F72A4,0x7F72C0,0x7F72D6,0x7F72E6,0x7F72FB,0x7F730B,0x7F7312,0x7F7355,
		0x7F737C,0x7F7392,0x7F73A2,0x7F73B7,0x7F73C7,
		// sub_1807F7400 (5)
		0x7F742D,0x7F7446,0x7F7537,0x7F788B,0x7F7C54,
	};

	// ---- cap byte (0x20 -> 0x78), with the imm8's offset inside the instruction ----
	struct CapPatch { uint32_t rva; uint8_t off; };
	constexpr CapPatch kCaps[] = {
		{ 0x7EB429, 3 }, // cmp r12,20h  (49 83 FC 20)   collection cap (a SIGNED imm8: at most 127)
	};
	constexpr int kMaxShadowsLimit = 120;   // the relocated collection buffer holds 204 records; the imm8 allows 127

	// verification probes: {rva, expected first bytes} - bail if any mismatch (wrong build)
	struct Probe { uint32_t rva; std::vector<uint8_t> bytes; };

	class Patcher
	{
		uintptr_t base = 0;
		void* mgrBuf = nullptr;
		void* collBuf = nullptr;
		void* rastBuf = nullptr;
		void* cavePage = nullptr;
		bool applied = false;
		uint8_t capByte = 30;   // the collection cap written at apply (Max Shadows)
		// Buffers superseded by a revert. NOT freed at revert time: the engine's shadow/rasterizer
		// functions read the relocated buffer addresses (via the caves) once per frame, so a frame that
		// was in-flight when we restored the lea sites can still touch our buffers this frame. Freeing them
		// immediately = use-after-free (the "crash sometimes on toggle-off"). Instead we queue them and free
		// on the NEXT apply / dtor, by which point many frames have passed and nothing references them.
		// Same crash-safe deferral UncapVisibilityLimits / UncapClusterLimit use.
		struct Pending { void* p; ULONGLONG queuedAt; };
		std::vector<Pending> pendingFree;

		// Every code write of an apply: planned first (threads live - this allocates), then written together inside one
		// QUIET WINDOW (2026-11-11). They used to be written one by one with the game running: a frame could run with some
		// of the ~60 sites patched and the rest not (buffer already relocated, counts / strides / caps still stock), or
		// execute an instruction while it was being overwritten.
		struct Save { uintptr_t addr; std::vector<uint8_t> orig, patch; };
		std::vector<Save> saves;

		static void* allocNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO si; GetSystemInfo(&si);
			uintptr_t gran = si.dwAllocationGranularity;
			uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
			uintptr_t hi = target + 0x70000000ULL;
			uintptr_t startp = target & ~(gran - 1);
			for (uintptr_t p = startp; p > lo; p -= gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
			for (uintptr_t p = startp + gran; p < hi; p += gran)
				if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
			return nullptr;
		}

		// plan `len` bytes at absolute addr (nothing is written yet), recording the originals for revert
		void writeSaved(uintptr_t addr, const uint8_t* data, size_t len)
		{
			Save s; s.addr = addr;
			s.orig.assign((const uint8_t*)addr, (const uint8_t*)addr + len);
			s.patch.assign(data, data + len);
			saves.push_back(std::move(s));
		}
		// Usable INSIDE a window (memory + bare system calls, no container growth): is our halo2.dll mapped, with every
		// site holding our bytes (patched) / the bytes it had when it was planned (!patched)?
		bool sitesHold(bool patched) const
		{
			const Save* const v = saves.data(); const size_t n = saves.size();
			if (!h2safe::moduleMappedAt(base)) return false;
			for (size_t k = 0; k < n; ++k)
			{
				const std::vector<uint8_t>& want = patched ? v[k].patch : v[k].orig;
				if (!h2safe::equals(v[k].addr, want.data(), want.size())) return false;
			}
			return true;
		}
		// INSIDE a window. restore = put the originals back, last first. Every write is guarded (H2SafeWrite.h, 2026-11-18).
		// 0 = written; 1 = the image is gone or its sites are not in the state this starts from (nothing written);
		// 2 = a write failed and every site was put back as it was; 3 = ...and that failed too (an unknown mix).
		int writeAll(bool restore)
		{
			if (!sitesHold(restore)) return 1;
			if (restore) zeroInlineMgrCountsSEH(base);   // BEFORE the lea sites go back (see zeroInlineMgrCountsSEH)
			const Save* const v = saves.data(); const size_t n = saves.size();
			size_t k = 0;
			for (; k < n; ++k)
			{
				const Save& sv = v[restore ? n - 1 - k : k];
				const std::vector<uint8_t>& src = restore ? sv.orig : sv.patch;
				if (!h2safe::patch(sv.addr, src.data(), src.size())) break;
			}
			if (k == n) return 0;
			bool back = true;
			for (size_t i = 0; i < n; ++i)
			{
				const std::vector<uint8_t>& was = restore ? v[i].patch : v[i].orig;
				back = h2safe::patch(v[i].addr, was.data(), was.size()) && back;
			}
			return back ? 2 : 3;
		}

		uintptr_t bufAddr(BufSel s) const
		{
			switch (s)
			{
			case SEL_MGR:   return (uintptr_t)mgrBuf;
			case SEL_COLL:  return (uintptr_t)collBuf;
			case SEL_COLL8: return (uintptr_t)collBuf + 8;
			case SEL_RAST:  return (uintptr_t)rastBuf;
			}
			return 0;
		}

		bool verify()
		{
			const Probe probes[] = {
				{ 0x7EB429, { 0x49,0x83,0xFC,0x20 } },             // cmp r12,20h
				{ 0x7EB36E, { 0x48,0x69,0xD8,0x84,0x04,0x00,0x00 } }, // imul rbx,rax,484h
				{ 0x7EB375, { 0x48,0x8D,0x05 } },                  // lea rax, unk_181660C70
				{ 0x7F7112, { 0x83,0xF9,0x20 } },                  // cmp ecx,20h
				{ 0x7F7C88, { 0x8B,0x47,0xF4,0x41,0x89,0x47,0xF8 } }, // mov eax,[rdi-0Ch]; mov [r15-8],eax (fade writeback)
				{ 0x973E1F, { 0x81,0xF9,0x00,0x00,0x04,0x00 } },   // cmp ecx,40000h
				{ 0x973E47, { 0x48,0x8D,0x0D } },                  // lea rcx, unk_1819CBF54
				// 2026-11-18: every remaining site that is written without being searched for (read from the shipped dll)
				{ 0x7EB2DD, { 0x4C,0x8D,0x7C,0x24,0x38 } },        // lea r15,[rsp+38h]
				{ 0x7EB37F, { 0x48,0x8D,0x54,0x24,0x30 } },        // lea rdx,[rsp+30h]
				{ 0x7EB394, { 0x48,0x8D,0x54,0x24,0x30 } },        // lea rdx,[rsp+30h]
				{ 0x7EB3B6, { 0x48,0x8D,0x5C,0x24,0x38 } },        // lea rbx,[rsp+38h]
				{ 0x973E86, { 0x48,0x8D,0x0D } },                  // lea rcx, unk_1819CBF54 (reset / memset)
				{ 0x973E76, { 0x41,0xB8,0x00,0x00,0x04,0x00 } },   // mov r8d,40000h
				{ 0x7F7196, { 0x83,0xF8,0x20 } },                  // cmp eax,20h
				{ 0x7F7219, { 0x41,0x83,0xFB,0x20 } },             // cmp r11d,20h
				{ 0x7F729A, { 0x83,0xF9,0x20 } },                  // cmp ecx,20h
				{ 0x7F735C, { 0x83,0xF8,0x20 } },                  // cmp eax,20h
			};
			for (auto& p : probes)
				if (!h2safe::equals(base + p.rva, p.bytes.data(), p.bytes.size()))
					return false;
			return true;
		}

		void freeAll()
		{
			if (mgrBuf)  { VirtualFree(mgrBuf, 0, MEM_RELEASE);  mgrBuf = nullptr; }
			if (collBuf) { VirtualFree(collBuf, 0, MEM_RELEASE); collBuf = nullptr; }
			if (rastBuf) { VirtualFree(rastBuf, 0, MEM_RELEASE); rastBuf = nullptr; }
			if (cavePage){ VirtualFree(cavePage, 0, MEM_RELEASE);cavePage = nullptr; }
		}

		// move the current buffers into the deferred-free queue (called by revert instead of freeAll)
		void queueBuffersForDeferredFree()
		{
			const ULONGLONG now = GetTickCount64();
			for (void* p : { mgrBuf, collBuf, rastBuf, cavePage })
				if (p) pendingFree.push_back(Pending{ p, now });
			mgrBuf = collBuf = rastBuf = cavePage = nullptr;
		}

		// Free what earlier reverts queued - only what was queued at least minAgeMs ago, so "many frames later" is a
		// fact and not a hope (the rasterizer scratch pool hands out pointers into rastBuf for the length of a frame).
		// The rest stays queued for a later drain.
		void drainPending(ULONGLONG minAgeMs)
		{
			const ULONGLONG now = GetTickCount64();
			std::vector<Pending> keep;
			for (const Pending& q : pendingFree)
			{
				if (now - q.queuedAt >= minAgeMs) VirtualFree(q.p, 0, MEM_RELEASE);
				else keep.push_back(q);
			}
			pendingFree.swap(keep);
		}

	public:
		bool isApplied() const { return applied; }

		// Max Shadows. Before apply: what apply writes. While applied: the cap byte is rewritten in place - ONE byte (a
		// single store, nothing to suspend), and the planned bytes are updated so the revert / module checks still match.
		// False = applied but the byte could not be written (nothing changed).
		bool setCap(int value)
		{
			const uint8_t v = (uint8_t)(value < 4 ? 4 : value > kMaxShadowsLimit ? kMaxShadowsLimit : value);
			if (!applied) { capByte = v; return true; }
			const uintptr_t at = base + kCaps[0].rva + kCaps[0].off;
			for (Save& sv : saves)
				if (sv.addr == at && sv.patch.size() == 1)
				{
					if (!h2safe::equals(at, sv.patch.data(), 1) || !h2safe::patch(at, &v, 1)) return false;
					sv.patch[0] = v; capByte = v;
					return true;
				}
			return false;
		}

		// returns false (no throw) if halo2.dll isn't loaded yet; throws on a real failure
		bool apply()
		{
			syncModule();   // the dll we patched may be gone (a game ended): then this is a fresh, unpatched one
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false; // deferred until Halo 2 loads

			drainPending(1000); // free buffers a previous revert queued a while ago

			if (!verify())
				throw HCMRuntimeException("UncapDropShadows: halo2.dll bytes don't match build 1.3528; refusing to patch.");

			collBuf = VirtualAlloc(0, kCollSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			rastBuf = VirtualAlloc(0, kRastSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			cavePage = allocNear(base, 0x1000);
			if (!collBuf || !rastBuf || !cavePage)
			{
				freeAll();
				throw HCMRuntimeException("UncapDropShadows: buffer allocation failed (incl. near cave page).");
			}
			memset(rastBuf, 0, kRastSize);

			// ---- caves + lea-site jmps ----
			uint8_t* cave = (uint8_t*)cavePage;
			for (const CaveSpec& cs : kCaves)
			{
				uintptr_t caveVA = (uintptr_t)cave;
				uintptr_t imm = bufAddr(cs.sel);
				*cave++ = cs.movOp0; *cave++ = cs.movOp1;
				memcpy(cave, &imm, 8); cave += 8;
				uintptr_t backVA = base + cs.backRVA;
				uintptr_t jmpVA = (uintptr_t)cave;
				*cave++ = 0xE9;
				int32_t rel = (int32_t)(backVA - (jmpVA + 5));
				memcpy(cave, &rel, 4); cave += 4;

				// patch the lea site: E9 rel32 -> cave, NOP-pad to leaLen
				uintptr_t siteVA = base + cs.siteRVA;
				uint8_t patch[7];
				patch[0] = 0xE9;
				int32_t rel2 = (int32_t)(caveVA - (siteVA + 5));
				memcpy(patch + 1, &rel2, 4);
				for (int i = 5; i < cs.leaLen; ++i) patch[i] = 0x90;
				writeSaved(siteVA, patch, cs.leaLen);
			}

			FlushInstructionCache(GetCurrentProcess(), cavePage, (SIZE_T)(cave - (uint8_t*)cavePage));

			// ---- dword replacements: rasterizer cap + memset size ----
			std::vector<DwordPatch> dwords;
			dwords.push_back({ 0x973E1F, 7, 0x00040000, kRastCap });     // cmp ecx,40000h (pool cap) -> 2MB
			dwords.push_back({ 0x973E76, 7, 0x00040000, kRastCap });     // mov r8d,40000h (memset size) -> 2MB
			for (const DwordPatch& dp : dwords)
			{
				uint8_t* p = (uint8_t*)(base + dp.rva);
				int found = -1;
				for (int i = 0; i + 4 <= dp.scanLen; ++i)
				{
					uint32_t v; memcpy(&v, p + i, 4);
					if (v == dp.oldVal) { found = i; break; }
				}
				if (found < 0)
				{
					saves.clear(); freeAll();   // nothing has been written yet (the writes are only planned)
					throw HCMRuntimeException(std::format("UncapDropShadows: expected 0x{:X} not found at rva 0x{:X}.", dp.oldVal, dp.rva));
				}
				uint8_t nb[4]; memcpy(nb, &dp.newVal, 4);
				writeSaved((uintptr_t)(p + found), nb, 4);
			}

			// ---- the collection cap (stock 0x20 = 32 -> Max Shadows) ----
			for (const CapPatch& cp : kCaps)
			{
				uint8_t nb = capByte;
				writeSaved(base + cp.rva + cp.off, &nb, 1);
			}

			// ---- every write above was only PLANNED: make them all in one window where no thread is inside the renderer
			//      (H2QuietWindow.h), so no frame ever runs a mix of patched and stock sites ----
			int tries = 0, rc = -1;
			{
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 800, tries);
				if (window) rc = writeAll(false);
			}   // the window closes here
			if (rc == 3)
			{
				saves.clear(); mgrBuf = collBuf = rastBuf = cavePage = nullptr;   // an unknown mix may reference them: leaked
				throw HCMRuntimeException("Uncap Drop Shadows: a write to halo2.dll failed and could not be undone - restart the game.");
			}
			if (rc != 0)
			{
				saves.clear(); freeAll();   // nothing is written (or it was all put back inside the window)
				if (rc < 0)
				{
					PLOG_ERROR << "UncapDropShadows: no quiet window in " << tries << " tries - nothing patched";
					throw HCMRuntimeException("Uncap Drop Shadows: could not find a safe moment to patch (the game never left its renderer). Nothing was changed - toggle it off and on again.");
				}
				throw HCMRuntimeException(rc == 1 ? "Uncap Drop Shadows: halo2.dll changed or was unloaded while waiting - nothing was changed."
					: "Uncap Drop Shadows: a write to halo2.dll failed and was undone - nothing was changed.");
			}
			PLOG_INFO << "UncapDropShadows: applied (" << saves.size() << " sites, quiet window after " << tries << " tries)";

			applied = true;
			return true;
		}

		// Zero the STALE inline shadow-manager counts. While our enlarged buffers were installed the engine wrote
		// shadow entries into THEM, never the inline manager (unk_181660C70) - so the inline manager still holds a
		// count + entry pointers from before we applied, and those entries reference structures that have since been
		// freed/reused. The instant revert() restores the lea sites, the render thread walks this inline manager
		// again and dereferences a dangling entry (its hash table) -> AV (the confirmed toggle-off crash). Zero the
		// counts so the engine walks NOTHING until it repopulates the manager fresh next frame - exactly the cure
		// that made UncapClusterLimit's toggle-off safe. Layout: unk_181660C70, 4 categories, stock stride 0x484
		// (32*36+4), count dword @ +0x480 (32*36). POD-only + SEH so a wrong base can't fault the process.
		static void zeroInlineMgrCountsSEH(uintptr_t base)
		{
			__try
			{
				for (int i = 0; i < 4; ++i)
					*(volatile uint32_t*)(base + 0x1660C70 + (uint32_t)i * 0x484 + 0x480) = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		// THE DYNAMIC LIGHTS' FADE LISTS (2026-11-19). sub_1807F6FE0 / sub_1807F7400 - where the count offset is patched -
		// also run the lights' lists (qword_18165FA60: 4 views x 0x484, straight before the shadow manager; filled by
		// sub_1807E97C0, which is NOT patched, and never relocated). So while this is on a light list keeps its count at
		// list + 0x10E0 (view 0: inside view 3's unused entries; the other views: inside the shadow manager's inline
		// block, unused while that is relocated), and at +0x480 again once it is off. Whichever offset the code is about
		// to use is emptied here, so it never starts from a stale count over entries rewritten in the meantime; the
		// lists refill on the next frame. newLayout = the patched offset. POD-only + SEH, inside the window.
		static void zeroLightListCountsSEH(uintptr_t base, bool newLayout)
		{
			__try
			{
				for (int i = 0; i < 4; ++i)
					*(volatile uint32_t*)(base + 0x165FA60 + (uint32_t)i * 0x484 + (newLayout ? kNewCount : 0x480u)) = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		// MCC UNLOADS halo2.dll when a game ends and loads it again (often elsewhere) for the next one. If the image we
		// patched is gone - or a fresh one sits at its address - drop the state and touch nothing (2026-11-18; before,
		// `applied` stayed true and the next game's fresh dll was never patched).
		void syncModule()
		{
			if (!applied && saves.empty()) return;
			const uintptr_t now = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (now && now == base && sitesHold(true)) return;
			// gone, or stock again: nothing references the buffers. Anything else (a mix): they are leaked, never freed.
			const bool unreferenced = now != base || !h2safe::moduleMappedAt(base) || sitesHold(false);
			saves.clear();
			if (unreferenced) queueBuffersForDeferredFree(); else mgrBuf = collBuf = rastBuf = cavePage = nullptr;
			applied = false;
		}

		// Our own manager's counts -> 0 (memory of ours, always safe to write). For when the patch has to stay on across a
		// level change: its entries must not outlive the map they point into.
		void zeroOurMgrCounts()
		{
			if (!mgrBuf) return;
			for (int i = 0; i < 4; ++i) *(volatile uint32_t*)((uintptr_t)mgrBuf + (uint32_t)i * kNewStride + kNewCount) = 0;
		}

		// Back to stock. True = nothing of ours is installed any more. False = no safe moment came up (or a restore failed
		// and was undone): the patch is still ON and nothing was changed.
		// 2026-11-18: the revert is ONLY made in a quiet window (no thread in the renderer or our cave, H2QuietWindow.h).
		// It used to go ahead with a plain suspension when none came up - a thread stopped in the middle of the shadow
		// function then ran its second half on the stock stack buffer its first half had not filled - and the destructor
		// then freed the buffers straight away. Heap work (saves.clear, queueing) stays OUTSIDE the window.
		bool revert()
		{
			syncModule();
			if (!applied && saves.empty()) return true;   // nothing of ours is installed (this is called on every menu / load)
			int tries = 0, rc = -1;
			{
				const uintptr_t caveLo = (uintptr_t)cavePage, caveHi = cavePage ? caveLo + 0x1000 : 0;
				std::unique_ptr<ScopedThreadSuspender> window = H2Quiet::open(base, 400, tries, caveLo, caveHi);
				// writeAll zeroes the stale inline-manager counts BEFORE restoring the lea sites, so the first post-revert
				// frame walks an empty (safe) manager instead of dangling entries
				if (window) rc = writeAll(true);
			}   // the window closes here
			if (rc < 0 || rc == 2)
			{
				PLOG_INFO << "UncapDropShadows: NOT reverted (" << (rc < 0 ? "no quiet window, tries " : "a write failed and was undone, tries ") << tries << ") - still on";
				return false;
			}
			if (rc == 3)
			{
				saves.clear(); mgrBuf = collBuf = rastBuf = cavePage = nullptr; applied = false;   // leaked: an unknown mix may reference them
				throw HCMRuntimeException("Uncap Drop Shadows: a write to halo2.dll failed and could not be undone - restart the game.");
			}
			if (rc == 1) { syncModule(); return !applied; }   // the image went away / changed just before the window
			PLOG_INFO << "UncapDropShadows: reverted (quiet window after " << tries << " tries)";
			saves.clear();
			queueBuffersForDeferredFree(); // NOT freeAll(): freed later, by age
			applied = false;
			return true;
		}

		// Feature destroyed (HCM unload / MCC shutdown): revert restores the bytes and queues the buffers;
		// then we free everything. This is a controlled teardown (not a live gameplay toggle), matching how
		// UncapVisibilityLimits / UncapClusterLimit free their deferred buffers in their dtors.
		~Patcher()
		{
			try { revert(); } catch (...) {}
			// Still installed (no safe moment came up): the game's code references the buffers and the cave, so they are
			// LEAKED on purpose - they are VirtualAlloc'd, not part of HCMInternal.dll.
			if (applied) return;
			// Reverted. What was queued a while ago is freed; what was queued just now is only freed if halo2.dll is gone -
			// with the game still running it is leaked (about 2 MB, once) rather than freed under a frame in flight.
			const bool gone = !base || (uintptr_t)GetModuleHandleA("halo2.dll") != base;
			drainPending(gone ? 0 : 1000);
			freeAll();
		}
	};
} // namespace


template <GameState::Value gameT>
class UncapDropShadowsImpl : public IUncapDropShadowsImpl
{
private:
	GameState mGame;
	std::mutex mPatchMutex;   // the toggle arrives on the render thread and on hotkeys, MCC's state event on MCC's thread
	Patcher mPatcher;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(int&)>> mMaxCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	int maxShadows()
	{
		try { lockOrThrow(settingsWeak, settings); return settings->uncapDropShadowsMax->GetValue(); }
		catch (HCMRuntimeException&) { return 30; }
	}
	// HCM's lists for the shadows while the patch is in, the engine's otherwise (2026-11-19)
	void syncLists()
	{
		H2FadeListHooks::configure(H2FadeListHooks::Shadows, mPatcher.isApplied(), maxShadows());
	}

	void onMax(int& newValue)
	{
		std::scoped_lock lock(mPatchMutex);
		if (!mPatcher.setCap(newValue))
		{
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage("Uncap Drop Shadows: could not change the shadow cap - nothing was changed"); } catch (...) {}
			return;
		}
		syncLists();
		if (mPatcher.isApplied())
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(std::format("Drop shadows: up to {} at once", maxShadows())); } catch (...) {}
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "UncapDropShadows onToggle, newValue: " << newValue;
		std::scoped_lock lock(mPatchMutex);
		try
		{
			mPatcher.setCap(maxShadows());
			if (newValue)
			{
				if (!mPatcher.apply())
					PLOG_DEBUG << "UncapDropShadows: deferred (halo2.dll not loaded yet)";
			}
			else if (!mPatcher.revert())
				throw HCMRuntimeException("Uncap Drop Shadows: could not find a safe moment to restore the stock shadows (the game never left its renderer). It is still ON - toggle it off again.");
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			// turning it off did not happen: make the checkbox say so (set without firing the event again)
			if (!newValue && mPatcher.isApplied())
				try { lockOrThrow(settingsWeak, settings); settings->uncapDropShadowsToggle->GetValueDisplay() = true; settings->uncapDropShadowsToggle->GetValue() = true; } catch (...) {}
			syncLists();
			return;
		}
		syncLists();

		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? std::format("Uncapped drop shadows on: up to {} at once (stock 4 shine)", maxShadows()) : std::string("Uncapped drop shadows off")); }
		catch (HCMRuntimeException&) {}
	}

	// Lifecycle: our buffer relocation is live ONLY while fully in a game. On save-and-quit / loading screens we
	// force stock (revert zeros the stale inline-manager counts + restores the lea sites), and re-apply once we're
	// back in-game on the new map. Our relocated buffers are HCM-owned (not game-heap arrays), so revert() here has
	// no game double-free risk - unlike the cluster/visibility uncaps.
	void onMCCStateChanged(const MCCState& newState)
	{
		std::scoped_lock lock(mPatchMutex);
		if (newState.currentGameState != mGame || newState.currentPlayState != PlayState::Ingame)
		{
			// Left Halo 2, or a loading screen / menu inside it: back to stock (revert() first drops the state when the
			// dll is already gone - it unloaded and took our patch with it). If no safe moment came up the patch stays on:
			// then at least empty OUR manager, so no entry of the old map is walked on the next one.
			try { mPatcher.revert(); } catch (HCMRuntimeException&) {}
			syncLists();
			return;
		}
		try
		{
			lockOrThrow(settingsWeak, settings);
			mPatcher.setCap(maxShadows());
			if (settings->uncapDropShadowsToggle->GetValue() && !mPatcher.isApplied())
				mPatcher.apply();
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "UncapDropShadows: load-time apply skipped (" << ex.what() << ")"; }
		syncLists();
	}

public:
	UncapDropShadowsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDropShadowsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMaxCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDropShadowsMax->valueChangedEvent, [this](int& n) { onMax(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		H2FadeListHooks::acquire();
	}

	~UncapDropShadowsImpl()
	{
		std::scoped_lock lock(mPatchMutex);
		H2FadeListHooks::configure(H2FadeListHooks::Shadows, false, 4);   // the engine's lists first, then the patch out
		try { mPatcher.revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
		H2FadeListHooks::release();
	}
};


UncapDropShadows::UncapDropShadows(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<UncapDropShadowsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("UncapDropShadows not impl for this game");
	}
}

UncapDropShadows::~UncapDropShadows()
{
	PLOG_DEBUG << "~" << getName();
}
