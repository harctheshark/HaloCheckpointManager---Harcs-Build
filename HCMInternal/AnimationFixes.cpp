#include "pch.h"
#include "AnimationFixes.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMessagesGUI.h"
#include <array>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>

namespace
{
	// ============================================================================================
	// Fix 3: Rocket launcher animation pop — heap tag-data patcher (the "grenade throw pop" content fix).
	// Scans decompressed tag data (writable, non-executable, private heap) for the animation-tag header
	// signature and patches a table of offsets. Re-applied on a background timer (tag data is re-created
	// on map/BSP loads). Idempotent: re-writing identical bytes is harmless if the tag is already fixed.
	// ============================================================================================
	constexpr std::array<uint8_t, 40> kSignature{
		0x03,0x15,0x04,0x00,0x28,0x96,0xCF,0x38,0x33,0x36,0x10,0x3F,0x00,0x1D,0x00,0x00,
		0x40,0x25,0x00,0x00,0x60,0x01,0x00,0x00,0x10,0x02,0x00,0x00,0xB0,0x00,0x00,0x00,
		0x73,0xBD,0x5E,0xFD,0xAC,0x03,0xC3,0x92 };

	struct PatchEntry
	{
		uint32_t rel;                 // offset from the matched header
		std::array<uint8_t, 8> fix;   // bytes when enabled
		std::array<uint8_t, 8> orig;  // bytes when disabled (restore)
	};

	const std::array<PatchEntry, 51> kPatches{ {
		{ 0x180, {0x15,0xA3,0xA9,0xF5,0x4E,0xBA,0x3B,0xCB}, {0x25,0x17,0xE2,0xE5,0xA5,0x12,0x46,0x86} },
		{ 0x440, {0x00,0x00,0xF9,0x5D,0x00,0x00,0x18,0xA9}, {0x00,0x00,0xF8,0x5D,0x00,0x00,0x19,0xA9} },
		{ 0x700, {0xEF,0x73,0x09,0xDD,0xFB,0xEF,0xBF,0xD9}, {0x39,0xE4,0x32,0xB3,0x76,0xFF,0x74,0x9D} },
		{ 0x188, {0xE3,0xA3,0xEF,0xF4,0x71,0xBA,0xCE,0xC9}, {0x95,0x18,0xAA,0xE5,0x7D,0x13,0xBD,0x86} },
		{ 0x448, {0x00,0x00,0x67,0x5E,0x00,0x00,0x91,0xA9}, {0x00,0x00,0x67,0x5E,0x00,0x00,0x91,0xA9} },
		{ 0x708, {0x42,0x73,0x0C,0xDC,0xDA,0xF0,0x4B,0xD8}, {0x6A,0xE2,0x74,0xB2,0x36,0x00,0x91,0x9E} },
		{ 0x190, {0x9A,0xA6,0xBB,0xF2,0x2C,0xBB,0x07,0xC5}, {0x4A,0x1D,0x37,0xE5,0x77,0x16,0x68,0x88} },
		{ 0x450, {0x00,0x00,0x91,0x5F,0x00,0x00,0xDB,0xAA}, {0x00,0x00,0x91,0x5F,0x00,0x00,0xDB,0xAA} },
		{ 0x710, {0xBC,0x70,0xA1,0xD8,0x25,0xF3,0xBF,0xD3}, {0x75,0xDC,0xE3,0xB0,0x3F,0x02,0xE6,0xA1} },
		{ 0x198, {0x89,0xAD,0x59,0xEE,0x6B,0xBE,0x80,0xB9}, {0x46,0x28,0x5D,0xE5,0x45,0x1E,0x65,0x8D} },
		{ 0x458, {0x00,0x00,0x43,0x61,0x00,0x00,0xCC,0xAC}, {0x00,0x00,0x43,0x61,0x00,0x00,0xCC,0xAC} },
		{ 0x718, {0x45,0x69,0x0E,0xD0,0xAE,0xF6,0x00,0xCA}, {0x6E,0xCE,0xA7,0xB0,0x63,0x05,0xD3,0xA8} },
		{ 0x1A0, {0x09,0xC7,0x3F,0xE5,0x8C,0xCF,0xA1,0x9B}, {0xEB,0x44,0xBE,0xEA,0x24,0x35,0x96,0xA4} },
		{ 0x460, {0x00,0x00,0x49,0x63,0x00,0x00,0x39,0xAF}, {0x00,0x00,0x49,0x63,0x00,0x00,0x39,0xAF} },
		{ 0x720, {0x7C,0x4A,0x03,0xB9,0xF7,0xFC,0xEF,0xB3}, {0x47,0xA9,0xFE,0xBC,0xA0,0x09,0x97,0xBE} },
		{ 0x1A8, {0xDD,0xFA,0xA8,0xDE,0xA8,0xF6,0xE2,0x84}, {0x7B,0x5F,0x98,0xFA,0xA5,0x4D,0x3E,0xDD} },
		{ 0x468, {0x00,0x00,0x72,0x65,0x00,0x00,0xF3,0xB1}, {0x00,0x00,0x71,0x65,0x00,0x00,0xF3,0xB1} },
		{ 0x728, {0x4E,0x09,0x83,0xA4,0xA6,0x05,0x26,0xA7}, {0x33,0x85,0x56,0xE3,0xA8,0x0A,0xD4,0xEC} },
		{ 0x1B0, {0xB9,0x10,0xCE,0xDC,0x2A,0x08,0x5A,0x86}, {0xA4,0x5D,0x14,0x03,0xC0,0x56,0x24,0xF7} },
		{ 0x470, {0x00,0x00,0x26,0x6B,0x00,0x00,0xFC,0xB9}, {0x00,0x00,0x26,0x6B,0x00,0x00,0xFC,0xB9} },
		{ 0x730, {0xA2,0xED,0xE8,0x9E,0x22,0x0A,0x48,0xAF}, {0xE6,0x80,0x95,0xF5,0x95,0x0A,0x9A,0x02} },
		{ 0x1B8, {0x47,0x17,0xBC,0xDB,0xAE,0x0F,0x80,0x88}, {0x00,0x54,0x98,0x07,0x45,0x60,0x8C,0xFF} },
		{ 0x478, {0x00,0x00,0x24,0x72,0x00,0x00,0x15,0xC6}, {0x00,0x00,0x24,0x72,0x00,0x00,0x15,0xC6} },
		{ 0x738, {0xC9,0xE4,0xA8,0x98,0x29,0x0D,0xCC,0xBA}, {0xEF,0x80,0x15,0xFC,0x29,0x0A,0xE2,0x0A} },
		{ 0x1C0, {0xD9,0x19,0x3E,0xDC,0x23,0x13,0x62,0x89}, {0xDE,0x4C,0x92,0x0A,0xC3,0x65,0xA3,0x02} },
		{ 0x480, {0x00,0x00,0x7E,0x75,0x00,0x00,0x39,0xCD}, {0x00,0x00,0x7E,0x75,0x00,0x00,0x39,0xCD} },
		{ 0x740, {0x67,0xE1,0x36,0x95,0x45,0x0F,0x47,0xC2}, {0x2E,0x81,0xA4,0xFE,0xC4,0x08,0xD6,0x0E} },
		{ 0x1C8, {0x59,0x18,0xE6,0xE0,0x76,0x15,0x28,0x88}, {0xD3,0x3D,0x27,0x0C,0x63,0x6F,0x4A,0x02} },
		{ 0x488, {0x00,0x00,0x5C,0x79,0x00,0x00,0x52,0xD7}, {0x00,0x00,0x5B,0x79,0x00,0x00,0x52,0xD7} },
		{ 0x748, {0xC4,0xE1,0x6C,0x8E,0x82,0x0F,0xBE,0xCF}, {0x10,0x81,0x96,0xFE,0xDB,0x01,0x3D,0x10} },
		{ 0x1D0, {0xEB,0x18,0xDF,0xDF,0x58,0x15,0x86,0x88}, {0x46,0x40,0x2A,0x0C,0xFC,0x6D,0xA2,0x02} },
		{ 0x490, {0x00,0x00,0xEC,0x78,0x00,0x00,0x0A,0xD6}, {0x00,0x00,0xEB,0x78,0x00,0x00,0x0A,0xD6} },
		{ 0x750, {0x98,0xE1,0xA3,0x8F,0xD4,0x0F,0x30,0xCD}, {0x1B,0x81,0xA8,0xFE,0xC6,0x03,0x3F,0x10} },
		{ 0x1D8, {0xE8,0x19,0x01,0xDE,0xBF,0x14,0x26,0x89}, {0x48,0x45,0x12,0x0C,0xE6,0x6A,0x05,0x03} },
		{ 0x498, {0x00,0x00,0xDC,0x77,0x00,0x00,0x18,0xD3}, {0x00,0x00,0xDC,0x77,0x00,0x00,0x18,0xD3} },
		{ 0x758, {0x65,0xE1,0x23,0x92,0x47,0x10,0x37,0xC8}, {0x3B,0x81,0xA9,0xFE,0x1E,0x07,0x1F,0x10} },
		{ 0x1E0, {0x99,0x1A,0x99,0xDC,0xD7,0x13,0x8F,0x89}, {0xD1,0x49,0xDD,0x0B,0xD8,0x67,0x0A,0x03} },
		{ 0x4A0, {0x00,0x00,0xAA,0x76,0x00,0x00,0x06,0xD0}, {0x00,0x00,0xA9,0x76,0x00,0x00,0x06,0xD0} },
		{ 0x760, {0x94,0xE1,0x9C,0x94,0x92,0x10,0x95,0xC3}, {0x62,0x81,0x51,0xFE,0xE9,0x09,0xC8,0x0F} },
		{ 0x1E8, {0x88,0x1A,0x01,0xDC,0xE8,0x12,0x91,0x89}, {0xC3,0x4B,0x99,0x0B,0x7B,0x66,0x6B,0x02} },
		{ 0x4A8, {0x00,0x00,0x17,0x76,0x00,0x00,0xA1,0xCE}, {0x00,0x00,0x17,0x76,0x00,0x00,0xA1,0xCE} },
		{ 0x768, {0x62,0xE2,0xBB,0x95,0xA5,0x10,0x45,0xC1}, {0x71,0x81,0x8A,0xFD,0x50,0x0B,0x38,0x0F} },
		{ 0x1F0, {0x40,0x1A,0x31,0xDB,0x53,0x11,0x84,0x89}, {0x50,0x4F,0xF5,0x0A,0xD9,0x63,0x64,0x01} },
		{ 0x4B0, {0x00,0x00,0xD9,0x74,0x00,0x00,0xC1,0xCB}, {0x00,0x00,0xD8,0x74,0x00,0x00,0xC1,0xCB} },
		{ 0x770, {0x8E,0xE3,0xA7,0x97,0x89,0x10,0x93,0xBD}, {0x83,0x81,0x67,0xFC,0xF9,0x0C,0x38,0x0E} },
		{ 0x1F8, {0x72,0x19,0x77,0xDA,0x5E,0x0E,0x2C,0x89}, {0x4B,0x56,0x8F,0x09,0x0B,0x5E,0x89,0xFF} },
		{ 0x4B8, {0x00,0x00,0x98,0x71,0x00,0x00,0x03,0xC5}, {0x00,0x00,0x97,0x71,0x00,0x00,0x03,0xC5} },
		{ 0x778, {0x8D,0xE5,0xCC,0x9B,0xEA,0x0F,0x96,0xB6}, {0x92,0x81,0x7B,0xFA,0xD8,0x0E,0x1E,0x0C} },
		{ 0x200, {0xF4,0x16,0xC2,0xDA,0x6A,0x0A,0x2A,0x88}, {0x74,0x5C,0x94,0x07,0x18,0x58,0xF1,0xFB} },
		{ 0x4C0, {0x00,0x00,0x86,0x6D,0x00,0x00,0xC2,0xBD}, {0x00,0x00,0x85,0x6D,0x00,0x00,0xC3,0xBD} },
		{ 0x780, {0x48,0xE9,0xEF,0x9F,0xB5,0x0E,0xDD,0xAF}, {0x85,0x81,0x64,0xF7,0x6D,0x0F,0x88,0x08} },
	} };

	bool headerValid(uintptr_t h)
	{
		if (IsBadReadPtr((void*)h, kSignature.size())) return false;
		return memcmp((void*)h, kSignature.data(), kSignature.size()) == 0;
	}

	// Scan committed, writable, non-executable, PRIVATE heap for the signature. (MEM_PRIVATE skips images +
	// mapped files, where tag data never lives — a big speedup over scanning the whole address space.)
	std::vector<uintptr_t> scanForHeaders()
	{
		std::vector<uintptr_t> found;
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uint8_t* addr = (uint8_t*)si.lpMinimumApplicationAddress;
		uint8_t* maxAddr = (uint8_t*)si.lpMaximumApplicationAddress;
		MEMORY_BASIC_INFORMATION mbi;

		while (addr < maxAddr)
		{
			if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
			uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;

			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE)
			{
				DWORD p = mbi.Protect & 0xFF;
				bool writable = (p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
				bool executable = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
				bool inaccessible = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

				if (writable && !executable && !inaccessible && mbi.RegionSize >= kSignature.size())
				{
					uint8_t* base = (uint8_t*)mbi.BaseAddress;
					const uint8_t first = kSignature[0];
					const size_t limit = mbi.RegionSize - kSignature.size();
					for (size_t i = 0; i <= limit; ++i)
					{
						if (base[i] == first && memcmp(base + i, kSignature.data(), kSignature.size()) == 0)
							found.push_back((uintptr_t)(base + i));
					}
				}
			}

			if (next <= addr) break;
			addr = next;
		}
		return found;
	}

	void writeHeader(uintptr_t header, bool applyFix)
	{
		for (const auto& e : kPatches)
		{
			uintptr_t dst = header + e.rel;
			if (IsBadReadPtr((void*)dst, 8)) continue;
			memcpy((void*)dst, applyFix ? e.fix.data() : e.orig.data(), 8);
		}
	}


	// ============================================================================================
	// Halo2 1.3528 hook sites + clean original bytes (for the "already statically patched?" guard).
	// ============================================================================================
	std::atomic_uintptr_t gHalo2Base = 0;

	// Fix 1 (.snap) codec dispatcher
	constexpr uintptr_t kRvaSnap = 0x7DA419;
	constexpr uint8_t   kCleanSnap[] = { 0x49,0x03,0xD0,0x8B,0x02 };
	// Fix 1b (.rckt) FP-weapon render-interp guard
	constexpr uintptr_t kRvaRckt = 0x7228EF;
	constexpr uint8_t   kCleanRckt[] = { 0x48,0x8B,0x0D,0x02,0x8A,0xF2,0x00 };
	constexpr uintptr_t kRvaRcktSnapPath = 0x722963; // bail/snap target
	constexpr uintptr_t kRenderRegionAdd = 0x1A0600C; // region base -> node array
	constexpr ptrdiff_t kRenderNodeStride = 0x34;     // 52 bytes (13 floats) per node
	constexpr float     kRcktRotThreshold = 1.5f;     // 1+2cos(theta); below => rotated > ~75 deg
	constexpr float     kRcktPosThreshold = 0.04f;    // position delta-squared cutoff
	// Fix 2 cyclotron: HCM hooks the gate, but the static ".elev" cave hooks the populate fn — detect there.
	constexpr uintptr_t kRvaCycloPopulate = 0x723380;
	constexpr uint8_t   kCleanCycloPopulate[] = { 0x48,0x89,0x7C,0x24,0x20 };
	// cyclotron detour data
	constexpr uintptr_t kRvaObjHeaderTableGlobal = 0x18B7398;
	constexpr uintptr_t kRvaCurrentLevelCode     = 0xE70E68;
	constexpr uintptr_t kRvaObjDatumResolver     = 0x8D7000;
	constexpr ptrdiff_t kObjDatumPhaseOffset     = 0x144;
	constexpr float     kCyclotronSeamThreshold  = 0.05f;
	using ObjDatumResolverFn = uintptr_t(__fastcall*)(uintptr_t);

	// true if the bytes at base+rva differ from the clean originals (i.e. a static dll cave is already there)
	bool siteIsStaticallyPatched(uintptr_t base, uintptr_t rva, const uint8_t* clean, size_t n)
	{
		if (!base) return false;
		void* p = (void*)(base + rva);
		if (IsBadReadPtr(p, n)) return false;
		return memcmp(p, clean, n) != 0;
	}
}


template <GameState::Value gameT>
class AnimationFixesImpl : public IAnimationFixesImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// Fix 1 + 1b + 2: runtime hooks
	std::shared_ptr<ModuleMidHook> mBarrelSnapHook;       // .snap  codec frame snap
	std::shared_ptr<ModuleMidHook> mRcktHook;             // .rckt  render-interp seam guard
	static inline std::shared_ptr<ModuleInlineHook> sCyclotronGateHook; // cyclotron elevator interp

	// Fix 3: rocket-anim heap patcher worker
	std::atomic_bool mRunning = false;
	std::thread mWorker;
	std::mutex mHeadersMutex;
	std::vector<uintptr_t> mCachedHeaders;
	uint64_t mLastScanTick = 0;


	// ---- Fix 1: rocket firing interp (barrel 60-tick codec pop) ----
	static void barrelSnapMidHook(safetyhook::Context& ctx)
	{
		uintptr_t r = ctx.rdx + ctx.r8;                  // TLS + 0xD0
		if (*(uint32_t*)(r + 0x0C) != 43) return;        // nodeCount == 43 (fire_1:var1)
		uintptr_t anim = *(uintptr_t*)(r + 0x28);        // anim ptr
		if (!anim) return;
		if (*(uint16_t*)(anim + 0x14) != 31) return;     // frame_count == 31
		if (*(int32_t*)(r + 0x14) < 25) return;          // frame0 >= 25 (only the spin's tail)
		*(uint32_t*)(r + 0x18) = *(uint32_t*)(r + 0x14); // frame1 = frame0  } SNAP
		*(float*)(r + 0x1C) = 0.0f;                      // frac = 0.0f      }
	}


	// ---- Fix 1b: rocket barrel render-interp seam guard (port of the static .rckt cave) ----
	// At the hook the engine is about to interpolate the FP weapon's render pose. Re-check each node's
	// rotation/position delta between the two render regions (rdx / r10); if any moved too much, redirect
	// to the engine's SNAP path (0x722963) so it renders the tick pose instead of an interpolation that
	// would cross the barrel's 180-degree antipode. Else fall through (the trampoline runs the stolen
	// 'mov rcx,[out region]' and continues to the interpolate path).
	static void rcktMidHook(safetyhook::Context& ctx)
	{
		uintptr_t base = gHalo2Base.load();
		if (!base) { base = (uintptr_t)GetModuleHandleW(L"halo2.dll"); gHalo2Base.store(base); }
		if (!base) return;

		const float* a = (const float*)(ctx.rdx + kRenderRegionAdd + ctx.rdi); // region A node
		const float* c = (const float*)(ctx.r10 + kRenderRegionAdd + ctx.rdi); // region B node
		int count = (int)(int32_t)ctx.rsi;

		for (int i = 0; i < count; ++i)
		{
			if (IsBadReadPtr((void*)a, kRenderNodeStride) || IsBadReadPtr((void*)c, kRenderNodeStride)) return;
			// 3x3 rotation matrix dot (floats [1..9]) = 1 + 2cos(theta)
			float dot = a[1]*c[1] + a[2]*c[2] + a[3]*c[3] + a[4]*c[4] + a[5]*c[5]
				+ a[6]*c[6] + a[7]*c[7] + a[8]*c[8] + a[9]*c[9];
			if (dot < kRcktRotThreshold) { ctx.rip = base + kRvaRcktSnapPath; return; }
			// position delta squared (floats [10..12])
			float dx = c[10]-a[10], dy = c[11]-a[11], dz = c[12]-a[12];
			if ((dx*dx + dy*dy + dz*dz) > kRcktPosThreshold) { ctx.rip = base + kRvaRcktSnapPath; return; }
			a += 13; // 0x34 bytes
			c += 13;
		}
		// all nodes similar -> let the original interpolate path run (do not touch ctx.rip)
	}


	// ---- Fix 2: cyclotron elevator interp (inline-hook the interp gate) ----
	static __int64 __fastcall cyclotronGateDetour(uint32_t a1, uint32_t* a2)
	{
		__int64 result = sCyclotronGateHook->getInlineHook().call<__int64, uint32_t, uint32_t*>(a1, a2);
		if (result == 0 || (uint16_t)a1 != 0) return result; // only the elevator (object index 0)

		uintptr_t base = gHalo2Base.load();
		if (!base) { base = (uintptr_t)GetModuleHandleW(L"halo2.dll"); gHalo2Base.store(base); }
		if (!base) return result;

		const char* lvl = (const char*)(base + kRvaCurrentLevelCode);
		if (IsBadReadPtr((void*)lvl, 1)) return result;
		bool isCyclotron = false;
		for (int i = 0; i < 28 && lvl[i]; ++i)
		{
			if (lvl[i] == 'c' && !IsBadReadPtr((void*)(lvl + i), 10) && memcmp(lvl + i, "cyclotron", 9) == 0) { isCyclotron = true; break; }
		}
		if (!isCyclotron) return result;

		uintptr_t g = *(uintptr_t*)(base + kRvaObjHeaderTableGlobal);
		if (!g) return result;
		uintptr_t v2 = *(uintptr_t*)(g + 72);
		if (!v2) return result;
		uintptr_t entry = v2 + g;
		if (IsBadReadPtr((void*)(entry + 8), 4)) return result;
		auto resolver = (ObjDatumResolverFn)(base + kRvaObjDatumResolver);
		uintptr_t datum = resolver(entry);
		if (!datum || IsBadReadPtr((void*)(datum + kObjDatumPhaseOffset), 4)) return result;

		if (*(float*)(datum + kObjDatumPhaseOffset) < kCyclotronSeamThreshold)
		{
			if (a2) *a2 = 0xFFFFFFFF;
			return 0;
		}
		return result;
	}


	// ---- Fix 3: rocket-anim heap patcher worker (fast cadence) ----
	void applyAll(bool applyFix)
	{
		std::vector<uintptr_t> headers;
		{
			std::lock_guard<std::mutex> lock(mHeadersMutex);

			bool cacheOk = !mCachedHeaders.empty();
			for (uintptr_t h : mCachedHeaders) { if (!headerValid(h)) { cacheOk = false; break; } }

			if (!cacheOk)
			{
				// poll quickly until the tag is loaded (was 4000ms -> fires within ~1s of a map/weapon load)
				uint64_t now = GetTickCount64();
				if (mCachedHeaders.empty() || (now - mLastScanTick) >= 400)
				{
					mLastScanTick = now;
					mCachedHeaders = scanForHeaders();
				}
			}
			headers = mCachedHeaders;
		}

		for (uintptr_t h : headers) writeHeader(h, applyFix);
	}

	void workerLoop()
	{
		while (mRunning)
		{
			try { applyAll(true); }
			catch (...) { /* keep the worker alive across transient failures */ }
			// poll ~400ms (was 2000ms) so the heap fix fires fast after the tag loads
			for (int i = 0; i < 4 && mRunning; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	void startWorker()
	{
		if (mRunning) return;
		mRunning = true;
		applyAll(true);
		mWorker = std::thread([this]() { workerLoop(); });
	}

	void stopWorker()
	{
		mRunning = false;
		if (mWorker.joinable()) mWorker.join();
	}


	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "AnimationFixes onToggle, newValue: " << newValue;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (newValue)
			{
				uintptr_t base = gHalo2Base.load();
				if (!base) { base = (uintptr_t)GetModuleHandleW(L"halo2.dll"); gHalo2Base.store(base); }

				messagesGUI->addMessage("Animation Fixes ON:");

				// Each hook is skipped if your loaded dll already carries that fix as a baked-in cave
				// (so HCM never double-hooks the same site).
				if (siteIsStaticallyPatched(base, kRvaSnap, kCleanSnap, sizeof(kCleanSnap)))
					messagesGUI->addMessage("  rocket barrel codec snap: SKIPPED (already patched in your dll)");
				else
				{
					mBarrelSnapHook->setWantsToBeAttached(true);
					messagesGUI->addMessage("  rocket barrel codec snap: APPLIED");
				}

				if (siteIsStaticallyPatched(base, kRvaRckt, kCleanRckt, sizeof(kCleanRckt)))
					messagesGUI->addMessage("  rocket barrel render guard (.rckt): SKIPPED (already patched in your dll)");
				else
				{
					mRcktHook->setWantsToBeAttached(true);
					messagesGUI->addMessage("  rocket barrel render guard (.rckt): APPLIED");
				}

				if (siteIsStaticallyPatched(base, kRvaCycloPopulate, kCleanCycloPopulate, sizeof(kCleanCycloPopulate)))
					messagesGUI->addMessage("  cyclotron elevator: SKIPPED (already patched in your dll)");
				else
				{
					sCyclotronGateHook->setWantsToBeAttached(true);
					messagesGUI->addMessage("  cyclotron elevator: APPLIED");
				}

				startWorker();
				messagesGUI->addMessage("  rocket launcher animation (heap): APPLIED");
			}
			else
			{
				// detach is harmless for hooks we never attached
				mBarrelSnapHook->setWantsToBeAttached(false);
				mRcktHook->setWantsToBeAttached(false);
				sCyclotronGateHook->setWantsToBeAttached(false);

				stopWorker();
				applyAll(false); // restore the rocket-anim tag bytes
				{
					std::lock_guard<std::mutex> lock(mHeadersMutex);
					mCachedHeaders.clear();
				}
				messagesGUI->addMessage("Animation Fixes disabled.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}


public:
	AnimationFixesImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->animationFixesToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		gHalo2Base.store((uintptr_t)GetModuleHandleW(L"halo2.dll"));

		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		auto barrelSnapFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(animFixBarrelSnapFunction), gameImpl);
		mBarrelSnapHook = ModuleMidHook::make(gameImpl.toModuleName(), barrelSnapFunction, &barrelSnapMidHook);

		auto rcktFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(animFixRcktFunction), gameImpl);
		mRcktHook = ModuleMidHook::make(gameImpl.toModuleName(), rcktFunction, &rcktMidHook);

		auto cyclotronGateFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(animFixCyclotronGateFunction), gameImpl);
		sCyclotronGateHook = ModuleInlineHook::make(gameImpl.toModuleName(), cyclotronGateFunction, &cyclotronGateDetour);
	}

	~AnimationFixesImpl()
	{
		stopWorker();
	}
};


AnimationFixes::AnimationFixes(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<AnimationFixesImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("AnimationFixes not impl for this game");
	}
}

AnimationFixes::~AnimationFixes()
{
	PLOG_DEBUG << "~" << getName();
}
