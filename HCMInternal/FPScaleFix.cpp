#include "pch.h"
#include "FPScaleFix.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include <Windows.h>
#include <vector>
#include <cstring>

// ============================================================================
//  Halo 2 "FP Scale Fix" - runtime port of the offline .fpfix code cave (build_fpfix.py).
//
//  The first-person BODY/legs are the unit's real animated model drawn through the WORLD projection, so they
//  stretch as FOV widens (the arms/weapon are camera-relative and fine). The fix makes the body project as if
//  at a fixed reference FOV via a view-space lateral rescale about the camera:
//      s = tan(camFov/2) / tan(REF);   v' = s*v + (1-s)*(f.v)*f   per node matrix vector.
//
//  Original fix = an appended .fpfix PE section (cave code that self-relocates via an r9 rebase slide because
//  it lives inside the ASLR'd module) + a hook + a "tuck" constant patch. We reproduce it at runtime: the cave
//  bytes are copied verbatim into a page allocated NEAR the module, and we relocate it by patching its 9 absolute
//  imm64 references - crucially setting the r9 "slide base" so the slide computes to 0, then baking real runtime
//  addresses (moduleBase+RVA for globals, our page for the data block). The `add rXX, r9` ops then no-op.
//
//  Data-block knobs (cave page +0x00..): +0x00 TAN_REF (Leg Size, live), and a separate .text imm at +0x818999
//  is the body push/tuck FOV (Leg Tuck, live). Both writable while enabled.
// ============================================================================
namespace
{
	// --- the assembled cave code, extracted from the modded halo2.dll .fpfix section (556 bytes) ---
	static constexpr uint8_t kCaveCode[] = {
		0x4C,0x8D,0x0D,0x00,0x00,0x00,0x00,0x48,0xBA,0x47,0x90,0xA3,
		0x82,0x01,0x00,0x00,0x00,0x49,0x29,0xD1,0x49,0xBA,0x04,0xC5,
		0x65,0x81,0x01,0x00,0x00,0x00,0x4D,0x01,0xCA,0x41,0x89,0x02,
		0x85,0xC0,0x0F,0x8E,0xF1,0x01,0x00,0x00,0x48,0x83,0xEC,0x20,
		0x49,0xBB,0x00,0x90,0xA3,0x82,0x01,0x00,0x00,0x00,0x4D,0x01,
		0xCB,0x49,0xBA,0x60,0xC2,0x65,0x81,0x01,0x00,0x00,0x00,0x4D,
		0x01,0xCA,0x48,0xB9,0x84,0x15,0xA8,0x81,0x01,0x00,0x00,0x00,
		0x4C,0x01,0xC9,0xF3,0x0F,0x2A,0x01,0xF3,0x41,0x0F,0x5F,0x43,
		0x1C,0xF3,0x41,0x0F,0x5D,0x43,0x20,0xF3,0x41,0x0F,0x59,0x43,
		0x24,0x48,0xB9,0xF8,0xF4,0xDF,0x80,0x01,0x00,0x00,0x00,0x4C,
		0x01,0xC9,0xF3,0x0F,0x59,0x01,0x48,0xB9,0x74,0x34,0xE1,0x80,
		0x01,0x00,0x00,0x00,0x4C,0x01,0xC9,0xF3,0x0F,0x59,0x01,0xF3,
		0x41,0x0F,0x59,0x43,0x0C,0xF3,0x0F,0x11,0x04,0x24,0xD9,0x04,
		0x24,0xD9,0xF2,0xDD,0xD8,0xD9,0x1C,0x24,0xF3,0x0F,0x10,0x0C,
		0x24,0xF3,0x41,0x0F,0x5E,0x0B,0xF3,0x41,0x0F,0x5F,0x4B,0x10,
		0xF3,0x41,0x0F,0x5D,0x4B,0x14,0x0F,0x28,0xD1,0xF3,0x41,0x0F,
		0x5C,0x53,0x08,0x0F,0x28,0xC2,0xF3,0x0F,0x59,0xC0,0x41,0x0F,
		0x2F,0x43,0x04,0x0F,0x82,0x44,0x01,0x00,0x00,0xF3,0x41,0x0F,
		0x10,0x53,0x08,0xF3,0x0F,0x5C,0xD1,0x0F,0xC6,0xC9,0x00,0x0F,
		0xC6,0xD2,0x00,0xF3,0x41,0x0F,0x10,0x5A,0x0C,0xF3,0x41,0x0F,
		0x10,0x6A,0x10,0x0F,0x14,0xDD,0xF3,0x41,0x0F,0x10,0x6A,0x14,
		0x0F,0x16,0xDD,0xF3,0x41,0x0F,0x10,0x22,0xF3,0x41,0x0F,0x10,
		0x6A,0x04,0x0F,0x14,0xE5,0xF3,0x41,0x0F,0x10,0x6A,0x08,0x0F,
		0x16,0xE5,0x41,0x8A,0x53,0x18,0x48,0xB9,0x08,0xC5,0x65,0x81,
		0x01,0x00,0x00,0x00,0x4C,0x01,0xC9,0x41,0x89,0xC0,0x84,0xD2,
		0x75,0x0A,0x83,0x79,0x08,0x00,0x0F,0x85,0xD1,0x00,0x00,0x00,
		0x4C,0x8D,0x51,0x0C,0x41,0xBB,0x40,0x00,0x00,0x00,0x41,0x0F,
		0x10,0x42,0x04,0x0F,0x28,0xE8,0x0F,0x59,0xEB,0xF2,0x0F,0x7C,
		0xED,0xF2,0x0F,0x7C,0xED,0x0F,0x59,0xEA,0x0F,0x59,0xEB,0x0F,
		0x59,0xC1,0x0F,0x58,0xC5,0x41,0x0F,0x13,0x42,0x04,0x0F,0x12,
		0xE8,0xF3,0x41,0x0F,0x11,0x6A,0x0C,0x41,0x0F,0x10,0x42,0x10,
		0x0F,0x28,0xE8,0x0F,0x59,0xEB,0xF2,0x0F,0x7C,0xED,0xF2,0x0F,
		0x7C,0xED,0x0F,0x59,0xEA,0x0F,0x59,0xEB,0x0F,0x59,0xC1,0x0F,
		0x58,0xC5,0x41,0x0F,0x13,0x42,0x10,0x0F,0x12,0xE8,0xF3,0x41,
		0x0F,0x11,0x6A,0x18,0x41,0x0F,0x10,0x42,0x1C,0x0F,0x28,0xE8,
		0x0F,0x59,0xEB,0xF2,0x0F,0x7C,0xED,0xF2,0x0F,0x7C,0xED,0x0F,
		0x59,0xEA,0x0F,0x59,0xEB,0x0F,0x59,0xC1,0x0F,0x58,0xC5,0x41,
		0x0F,0x13,0x42,0x1C,0x0F,0x12,0xE8,0xF3,0x41,0x0F,0x11,0x6A,
		0x24,0x41,0x0F,0x10,0x42,0x28,0x0F,0x5C,0xC4,0x0F,0x28,0xE8,
		0x0F,0x59,0xEB,0xF2,0x0F,0x7C,0xED,0xF2,0x0F,0x7C,0xED,0x0F,
		0x59,0xEA,0x0F,0x59,0xEB,0x0F,0x59,0xC1,0x0F,0x58,0xC5,0x0F,
		0x58,0xC4,0x41,0x0F,0x13,0x42,0x28,0x0F,0x12,0xE8,0xF3,0x41,
		0x0F,0x11,0x6A,0x30,0x49,0x83,0xC2,0x34,0x41,0xFF,0xCB,0x0F,
		0x85,0x39,0xFF,0xFF,0xFF,0x48,0x81,0xC1,0x0C,0x0D,0x00,0x00,
		0x41,0xFF,0xC8,0x0F,0x85,0x11,0xFF,0xFF,0xFF,0x48,0x83,0xC4,
		0x20,0x48,0xBA,0x1E,0x55,0x7E,0x80,0x01,0x00,0x00,0x00,0x4C,
		0x01,0xCA,0xFF,0xE2,
	};
	static constexpr uint32_t kDataSize = 0x40;        // knobs block precedes the code in the cave page
	static constexpr uint32_t kCaveCodeOff = kDataSize; // code lives at page + 0x40

	// --- halo2.dll v1.3528 RVAs ---
	static constexpr uint32_t kHookRVA = 0x7E5518;  // mov cs:dword_18165C504, eax  (6 bytes) -> detour to cave
	static constexpr uint32_t kTuckRVA = 0x818999;  // body push/tuck FOV imm site  (16-byte region)
	static constexpr uint8_t  kHookOrig[6] = { 0x89, 0x05, 0xE6, 0x6F, 0xE7, 0x00 };
	static constexpr uint8_t  kTuckOrig[16] = { 0xE8,0x92,0x38,0x82,0xFF,0x84,0xC0,0x75,0x07,0x32,0xC9,0xE9,0x82,0x01,0x00,0x00 };

	// imm64 relocations inside kCaveCode: {offset in code, RVA of the global (0 => special-cased)}
	struct Reloc { uint32_t codeOff; uint32_t rva; };
	static constexpr Reloc kRelocs[] = {
		{ 0x16, 0x165C504 }, // COUNT_VA  (mov r10, dword_18165C504)
		{ 0x3F, 0x165C260 }, // CAM_VA
		{ 0x4C, 0x1A81584 }, // PROFILE_FOV_VA
		{ 0x6F, 0xDFF4F8  }, // FOV_SCALE_VA
		{ 0x80, 0xE13474  }, // ASPECT_K_VA (0.785)
		{ 0x11C,0x165C508 }, // BUF_VA (FP model buffer)
		{ 0x21F,0x7E551E  }, // RET_VA (return: hook site + 6)
	};
	static constexpr uint32_t kSlideBaseOff = 0x9;   // -> caveCode + 7  (makes the r9 rebase slide = 0)
	static constexpr uint32_t kDataRefOff   = 0x32;  // -> cave page + 0 (the knobs data block)

	// Repoints the FP-body build store into a runtime cave that rescales the legs. Self-contained raw-Win32,
	// mirrors UncapDropShadows (allocNear + writeSaved + ScopedThreadSuspender + deferred cave free).
	class FpScalePatcher
	{
		uintptr_t base = 0;
		uint8_t* cave = nullptr; // [0x40 data][code]
		bool applied = false;
		std::vector<uint8_t*> pendingFree; // caves superseded by a revert; freed on next apply/dtor (frame-safety)

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

		void buildDataBlock(uint8_t* d, float legSize) const
		{
			memset(d, 0, kDataSize);
			*(float*)(d + 0x00) = legSize;     // TAN_REF (Leg Size)
			*(float*)(d + 0x04) = 4e-6f;       // EPS^2 (skip-if-identity threshold)
			*(float*)(d + 0x08) = 1.0f;        // s identity center
			*(float*)(d + 0x0C) = 0.5f;        // half
			*(float*)(d + 0x10) = 0.25f;       // s min clamp
			*(float*)(d + 0x14) = 4.0f;        // s max clamp
			d[0x18] = 0;                       // ALL_MODELS = body only
			*(float*)(d + 0x1C) = 70.0f;       // configured-fov clamp min (deg)
			*(float*)(d + 0x20) = 120.0f;      // configured-fov clamp max (deg)
			*(float*)(d + 0x24) = 0.017453292519943295f; // deg->rad
		}

		bool verify() const
		{
			auto match = [&](uint32_t rva, const uint8_t* orig, size_t n) {
				const uint8_t* p = (const uint8_t*)(base + rva);
				if (memcmp(p, orig, n) == 0) return true;          // stock
				if (p[0] == 0xE9 || p[0] == 0xC7) return true;     // already our hook / tuck patch (idempotent)
				return false;
			};
			return match(kHookRVA, kHookOrig, sizeof(kHookOrig)) && match(kTuckRVA, kTuckOrig, sizeof(kTuckOrig));
		}

	public:
		bool isApplied() const { return applied; }

		// false (no throw) if halo2.dll isn't loaded; throws on a real failure (bad build / alloc).
		bool apply(float legSize, float legTuckRad)
		{
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;

			// free any cave a previous revert queued (safe now - many frames later)
			for (uint8_t* p : pendingFree) VirtualFree(p, 0, MEM_RELEASE);
			pendingFree.clear();

			if (!verify())
				throw HCMRuntimeException("FP Scale Fix: halo2.dll bytes don't match build 1.3528; refusing to patch.");

			cave = (uint8_t*)allocNear(base, 0x1000);
			if (!cave)
				throw HCMRuntimeException("FP Scale Fix: failed to allocate a near cave page.");

			// data block + code
			buildDataBlock(cave, legSize);
			memcpy(cave + kCaveCodeOff, kCaveCode, sizeof(kCaveCode));

			uintptr_t caveCodeVA = (uintptr_t)cave + kCaveCodeOff;
			uintptr_t caveDataVA = (uintptr_t)cave;
			auto put64 = [&](uint32_t codeOff, uintptr_t val) { memcpy(cave + kCaveCodeOff + codeOff, &val, 8); };
			put64(kSlideBaseOff, caveCodeVA + 7); // r9 = rip(caveCode+7) - this == 0  -> add rXX,r9 become no-ops
			put64(kDataRefOff, caveDataVA);
			for (const Reloc& r : kRelocs) put64(r.codeOff, base + r.rva);
			FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);

			// tuck patch: mov dword[rdx], legTuckRad ; nop*10  (replaces the stock call block)
			uint8_t tuck[16] = { 0xC7, 0x02, 0,0,0,0, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
			memcpy(tuck + 2, &legTuckRad, 4);

			// hook: E9 rel32 -> cave code, + nop  (replaces the 6-byte count-store; the cave replays that store)
			uint8_t hook[6] = { 0xE9, 0,0,0,0, 0x90 };
			int32_t rel = (int32_t)(caveCodeVA - (base + kHookRVA + 5));
			memcpy(hook + 1, &rel, 4);

			// install .text patches with other threads frozen (the FP path runs per-frame; no torn instruction)
			{
				ScopedThreadSuspender suspend;
				writeSaved(base + kTuckRVA, tuck, sizeof(tuck)); // tuck first (cave ready) ...
				writeSaved(base + kHookRVA, hook, sizeof(hook)); // ... then arm the hook last
			}

			applied = true;
			return true;
		}

		void setLegSize(float v) { if (applied && cave) *(float*)(cave + 0x00) = v; } // cave reads it each build
		void setLegTuck(float v) // the tuck imm inside our .text patch (C7 02 [imm] ...)
		{
			if (!applied || !base) return;
			uintptr_t addr = base + kTuckRVA + 2;
			DWORD o; VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, &v, 4);
			VirtualProtect((void*)addr, 4, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, 4);
		}

		void revert()
		{
			if (!applied) return;
			{
				ScopedThreadSuspender suspend; // restore in reverse (hook off first, then tuck)
				for (auto it = saves.rbegin(); it != saves.rend(); ++it)
				{
					DWORD o; VirtualProtect((void*)it->addr, it->orig.size(), PAGE_EXECUTE_READWRITE, &o);
					memcpy((void*)it->addr, it->orig.data(), it->orig.size());
					VirtualProtect((void*)it->addr, it->orig.size(), o, &o);
					FlushInstructionCache(GetCurrentProcess(), (void*)it->addr, it->orig.size());
				}
			}
			saves.clear();
			if (cave) { pendingFree.push_back(cave); cave = nullptr; } // deferred free (an in-flight frame may be in it)
			applied = false;
		}

		~FpScalePatcher()
		{
			if (applied && GetModuleHandleA("halo2.dll")) revert();
			for (uint8_t* p : pendingFree) VirtualFree(p, 0, MEM_RELEASE);
			pendingFree.clear();
			if (cave) VirtualFree(cave, 0, MEM_RELEASE);
		}
	};
} // namespace


template <GameState::Value gameT>
class FPScaleFixImpl : public IFPScaleFixImpl
{
private:
	GameState mGame;
	FpScalePatcher mPatcher;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mLegSizeCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mLegTuckCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	float legSize() { auto s = settingsWeak.lock(); return s ? s->fpScaleFixLegSize->GetValue() : 0.65f; }
	float legTuck() { auto s = settingsWeak.lock(); return s ? s->fpScaleFixLegTuck->GetValue() : 1.5f; }

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "FPScaleFix onToggle: " << newValue;
		try
		{
			if (newValue)
			{
				if (!mPatcher.apply(legSize(), legTuck()))
					PLOG_DEBUG << "FPScaleFix: deferred (halo2.dll not loaded yet)";
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
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "FP Scale Fix on" : "FP Scale Fix off"); }
		catch (HCMRuntimeException&) {}
	}

	void onLegSizeChanged(float& v) { mPatcher.setLegSize(v); }
	void onLegTuckChanged(float& v) { mPatcher.setLegTuck(v); }

	// re-apply on Halo 2 load if the toggle was on while halo2.dll wasn't loaded (the cave hook is live per-frame)
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		try
		{
			auto settings = settingsWeak.lock();
			if (settings && settings->fpScaleFixToggle->GetValue() && !mPatcher.isApplied())
				mPatcher.apply(legSize(), legTuck());
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "FPScaleFix: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	FPScaleFixImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->fpScaleFixToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mLegSizeCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->fpScaleFixLegSize->valueChangedEvent, [this](float& v) { onLegSizeChanged(v); }),
		mLegTuckCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->fpScaleFixLegTuck->valueChangedEvent, [this](float& v) { onLegTuckChanged(v); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}

	~FPScaleFixImpl()
	{
		try { mPatcher.revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


FPScaleFix::FPScaleFix(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<FPScaleFixImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("FPScaleFix not impl for this game");
	}
}

FPScaleFix::~FPScaleFix()
{
	PLOG_DEBUG << "~" << getName();
}
