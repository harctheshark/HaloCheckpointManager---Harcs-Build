#include "pch.h"
#include "UncapDropShadows.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include <Windows.h>
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
//  We raise every 32-cap to 120 and relocate the manager + collection + rasterizer
//  buffers to larger heap allocations. The engine references those buffers through a
//  handful of `lea` instructions; we replace each `lea` with a `jmp` to a code cave
//  that loads the heap address (`mov reg, imm64`) and jumps back. Each `lea` runs
//  exactly once per call, so the cave fires once - no per-iteration reset. Everything
//  else (count displacements +0x480 -> +0x10E0, stride 0x484 -> 0x10E4, the six 0x20
//  caps, the rasterizer cap/memset size) is in-place byte patching.
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
		// manager base   : lea rax, unk_181660C70   -> mgrBuf
		{ 0x7EB375, 7, 0x48, 0xB8, SEL_MGR,   0x7EB37C },
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
		{ 0x7EB429, 3 }, // cmp r12,20h  (49 83 FC 20)   collection cap
		{ 0x7F7112, 2 }, // cmp ecx,20h  (83 F9 20)
		{ 0x7F7196, 2 }, // cmp eax,20h  (83 F8 20)
		{ 0x7F7219, 3 }, // cmp r11d,20h (41 83 FB 20)
		{ 0x7F729A, 2 }, // cmp ecx,20h  (83 F9 20)
		{ 0x7F735C, 2 }, // cmp eax,20h  (83 F8 20)
	};

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

		struct Save { uintptr_t addr; std::vector<uint8_t> orig; };
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

		// write `len` bytes at absolute addr, recording the originals for revert
		void writeSaved(uintptr_t addr, const uint8_t* data, size_t len)
		{
			Save s; s.addr = addr; s.orig.resize(len);
			memcpy(s.orig.data(), (void*)addr, len);
			saves.push_back(std::move(s));
			DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, len);
			VirtualProtect((void*)addr, len, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
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
			};
			for (auto& p : probes)
				if (memcmp((void*)(base + p.rva), p.bytes.data(), p.bytes.size()) != 0)
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

	public:
		bool isApplied() const { return applied; }

		// returns false (no throw) if halo2.dll isn't loaded yet; throws on a real failure
		bool apply()
		{
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false; // deferred until Halo 2 loads

			if (!verify())
				throw HCMRuntimeException("UncapDropShadows: halo2.dll bytes don't match build 1.3528; refusing to patch.");

			mgrBuf  = VirtualAlloc(0, kMgrSize,  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			collBuf = VirtualAlloc(0, kCollSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			rastBuf = VirtualAlloc(0, kRastSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			cavePage = allocNear(base, 0x1000);
			if (!mgrBuf || !collBuf || !rastBuf || !cavePage)
			{
				freeAll();
				throw HCMRuntimeException("UncapDropShadows: buffer allocation failed (incl. near cave page).");
			}
			memset(mgrBuf, 0, kMgrSize);   // counts start empty (static zero-init won't apply to heap)
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

			// ---- fade-force cave: the manager writeback @0x7F7C88 copies manager_entry+0x18 (the per-entry
			//      fade alpha that ramps to 0) into output+0xC, which the draw reads as the blob alpha - so
			//      most shadows fade invisible after frame 1, leaving ~7. Force the written alpha to 1.0f
			//      (exactly sapien's shadowmax fix). The 7-byte pair `mov eax,[rdi-0Ch]; mov [r15-8],eax`
			//      becomes a jmp to a cave that loads 1.0 then runs the (replicated) store. ----
			{
				uintptr_t caveVA = (uintptr_t)cave;
				const uint8_t fc[] = { 0xB8,0x00,0x00,0x80,0x3F,   // mov eax, 0x3F800000 (1.0f)
									   0x41,0x89,0x47,0xF8 };      // mov [r15-8], eax
				memcpy(cave, fc, sizeof(fc)); cave += sizeof(fc);
				uintptr_t backVA = base + 0x7F7C8F;
				uintptr_t jmpVA = (uintptr_t)cave;
				*cave++ = 0xE9;
				int32_t rel = (int32_t)(backVA - (jmpVA + 5));
				memcpy(cave, &rel, 4); cave += 4;

				uintptr_t siteVA = base + 0x7F7C88;
				uint8_t patch[7];
				patch[0] = 0xE9;
				int32_t rel2 = (int32_t)(caveVA - (siteVA + 5));
				memcpy(patch + 1, &rel2, 4);
				patch[5] = 0x90; patch[6] = 0x90;
				writeSaved(siteVA, patch, 7);
			}
			FlushInstructionCache(GetCurrentProcess(), cavePage, (SIZE_T)(cave - (uint8_t*)cavePage));

			// ---- dword replacements: counts (0x480->0x10E0), stride (0x484->0x10E4),
			//      rasterizer cap + memset size (0x40000->0x80000) ----
			std::vector<DwordPatch> dwords;
			for (uint32_t r : kCountSites) dwords.push_back({ r, 7, 0x00000480, kNewCount });
			dwords.push_back({ 0x7EB36E, 7, 0x00000484, kNewStride });   // imul rbx,rax,484h
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
					revert(); // restore whatever we've done so far
					throw HCMRuntimeException(std::format("UncapDropShadows: expected 0x{:X} not found at rva 0x{:X}.", dp.oldVal, dp.rva));
				}
				uint8_t nb[4]; memcpy(nb, &dp.newVal, 4);
				writeSaved((uintptr_t)(p + found), nb, 4);
			}

			// ---- cap bytes (stock 0x20=32 -> kEnforcedCap) ----
			// Held at 30 for now: model_group_count (the shared render-geometry budget) is a packed 5-bit
			// field hard-capped at 32, and pushing shadows TO 32 saturates it and glitches. 30 keeps us
			// safely under that wall. Buffer layout is still 120-wide (headroom) so this can be raised once
			// the model_group bitfield is widened. See memory visibility-projection-cap.
			for (const CapPatch& cp : kCaps)
			{
				uint8_t nb = kEnforcedCap;
				writeSaved(base + cp.rva + cp.off, &nb, 1);
			}

			applied = true;
			return true;
		}

		void revert()
		{
			// restore patched bytes in reverse order
			for (auto it = saves.rbegin(); it != saves.rend(); ++it)
			{
				DWORD o; VirtualProtect((void*)it->addr, it->orig.size(), PAGE_EXECUTE_READWRITE, &o);
				memcpy((void*)it->addr, it->orig.data(), it->orig.size());
				VirtualProtect((void*)it->addr, it->orig.size(), o, &o);
				FlushInstructionCache(GetCurrentProcess(), (void*)it->addr, it->orig.size());
			}
			saves.clear();
			freeAll();
			applied = false;
		}

		~Patcher() { if (applied && GetModuleHandleA("halo2.dll")) revert(); else freeAll(); }
	};
} // namespace


template <GameState::Value gameT>
class UncapDropShadowsImpl : public IUncapDropShadowsImpl
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
		PLOG_DEBUG << "UncapDropShadows onToggle, newValue: " << newValue;
		try
		{
			if (newValue)
			{
				if (!mPatcher.apply())
					PLOG_DEBUG << "UncapDropShadows: deferred (halo2.dll not loaded yet)";
			}
			else
				mPatcher.revert();
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			return;
		}

		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Uncapped drop shadows on" : "Uncapped drop shadows off"); }
		catch (HCMRuntimeException&) {}
	}

	// re-apply on Halo 2 load if the toggle was flipped on while halo2.dll wasn't loaded
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapDropShadowsToggle->GetValue() && !mPatcher.isApplied())
				mPatcher.apply();
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "UncapDropShadows: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	UncapDropShadowsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapDropShadowsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}

	~UncapDropShadowsImpl()
	{
		try { mPatcher.revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
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
