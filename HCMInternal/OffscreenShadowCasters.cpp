#include "pch.h"
#include "OffscreenShadowCasters.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include <array>

// halo2.dll sub_180709D40 = sapien sub_6752A0 (per-instance/object bounding-sphere visibility test).
// The radius arrives in xmm2 (3rd fastcall arg, a float); it is saved (movaps xmm6,xmm2) and passed to the
// frustum test sub_18079A380 (movaps xmm3,xmm2). We hook the function entry: replace the first 5 bytes
// (mov [rsp+8],rbx) with jmp->cave; the cave does `mulss xmm2,[factor]`, runs the displaced instruction,
// then jmps back. The factor float lives inside the cave and is live-editable from the multiplier setting.
template <GameState::Value gameT>
class OffscreenShadowCastersImpl : public IOffscreenShadowCastersImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mMultiplierCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	// sub_180709D40 entry: mov [rsp+8], rbx  (the 5 bytes we detour through the cave)
	static constexpr uint32_t kTargetRVA = 0x709D40;
	static constexpr std::array<uint8_t, 5> kEntryBytes{ 0x48, 0x89, 0x5C, 0x24, 0x08 };

	// cave layout (offsets): 0=mulss xmm2,[rip+disp32] (8B), 8=displaced entry (5B), 13=jmp back (5B), 18=factor(4B)
	static constexpr int kCaveMulss = 0;
	static constexpr int kCaveDisplaced = 8;
	static constexpr int kCaveJmp = 13;
	static constexpr int kCaveFactor = 18;
	static constexpr int kCaveSize = 22;

	uintptr_t base = 0;
	void* cavePage = nullptr;
	uintptr_t factorAddr = 0;
	bool applied = false;
	std::array<uint8_t, 5> savedEntry{};

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

	static void writeRaw(uintptr_t addr, const void* data, size_t len)
	{
		DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
		memcpy((void*)addr, data, len);
		VirtualProtect((void*)addr, len, o, &o);
		FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
	}

	float currentMultiplier()
	{
		if (auto settings = settingsWeak.lock())
			return settings->offscreenShadowCastersMultiplier->GetValue();
		return 25.f;
	}

	// build the cave + write the entry detour. returns false (no throw) if halo2.dll isn't loaded yet.
	bool apply()
	{
		if (applied) return true;
		base = (uintptr_t)GetModuleHandleA("halo2.dll");
		if (!base) return false; // deferred until Halo 2 loads

		uintptr_t targetVA = base + kTargetRVA;
		if (memcmp((void*)targetVA, kEntryBytes.data(), kEntryBytes.size()) != 0)
			throw HCMRuntimeException("OffscreenShadowCasters: halo2.dll bytes don't match build 1.3528; refusing to patch.");

		cavePage = allocNear(base, 0x1000);
		if (!cavePage)
			throw HCMRuntimeException("OffscreenShadowCasters: near cave allocation failed.");

		uint8_t* c = (uint8_t*)cavePage;
		uintptr_t caveVA = (uintptr_t)cavePage;
		factorAddr = caveVA + kCaveFactor;

		// mulss xmm2,[rip+disp32]  -> factor
		c[0] = 0xF3; c[1] = 0x0F; c[2] = 0x59; c[3] = 0x15;
		int32_t mulDisp = (int32_t)(factorAddr - (caveVA + kCaveMulss + 8));
		memcpy(c + 4, &mulDisp, 4);
		// displaced entry (mov [rsp+8],rbx)
		memcpy(c + kCaveDisplaced, kEntryBytes.data(), 5);
		// jmp back to targetVA+5
		c[kCaveJmp] = 0xE9;
		int32_t backRel = (int32_t)((targetVA + 5) - (caveVA + kCaveJmp + 5));
		memcpy(c + kCaveJmp + 1, &backRel, 4);
		// factor float
		float f = currentMultiplier();
		memcpy(c + kCaveFactor, &f, 4);
		FlushInstructionCache(GetCurrentProcess(), cavePage, kCaveSize);

		// entry detour: E9 rel32 -> cave
		uint8_t det[5];
		det[0] = 0xE9;
		int32_t rel = (int32_t)(caveVA - (targetVA + 5));
		memcpy(det + 1, &rel, 4);
		memcpy(savedEntry.data(), (void*)targetVA, 5);
		writeRaw(targetVA, det, 5);

		applied = true;
		PLOG_DEBUG << "OffscreenShadowCasters applied: cave=" << std::hex << caveVA << " factor=" << f;
		return true;
	}

	void revert()
	{
		if (!applied) return;
		// only restore the entry if the SAME halo2.dll we patched is still loaded (MCC unloads it on
		// exit-to-menu; writing to the old base would corrupt whatever now occupies that address).
		uintptr_t curBase = (uintptr_t)GetModuleHandleA("halo2.dll");
		if (curBase && curBase == base)
		{
			// Restore the detour entry with other threads suspended so the render thread can't
			// execute a half-restored instruction mid-revert (races and can crash the game). The
			// cave VirtualFree stays OUTSIDE the suspend window (a suspended thread could hold the
			// VM lock). (See ScopedThreadSuspender.)
			ScopedThreadSuspender suspend;
			writeRaw(base + kTargetRVA, savedEntry.data(), 5);
		}
		if (cavePage) { VirtualFree(cavePage, 0, MEM_RELEASE); cavePage = nullptr; }
		factorAddr = 0;
		applied = false;
		PLOG_DEBUG << "OffscreenShadowCasters reverted";
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "OffscreenShadowCasters onToggle: " << newValue;
		try
		{
			if (newValue)
			{
				if (!apply())
					PLOG_DEBUG << "OffscreenShadowCasters: deferred (halo2.dll not loaded)";
			}
			else revert();
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			return;
		}

		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(newValue ? "Off-screen shadow casters on" : "Off-screen shadow casters off"); }
		catch (HCMRuntimeException&) {}
	}

	void onMultiplierChanged(float& newValue)
	{
		if (applied && factorAddr)
		{
			float f = newValue > 1.f ? newValue : 1.f;
			memcpy((void*)factorAddr, &f, 4); // plain RW page, no protect needed
			PLOG_DEBUG << "OffscreenShadowCasters multiplier -> " << f;
		}
	}

	// re-apply on each Halo 2 load if the toggle is on. MCC unloads halo2.dll on exit-to-menu, so on
	// re-entry the module base changes and our cave (in the old address space) is stale; detect that and
	// rebuild against the new module.
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			if (!settings->offscreenShadowCastersToggle->GetValue()) return;

			uintptr_t curBase = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!curBase) return;
			if (applied && curBase == base) return; // already patched into the current module

			// module was reloaded (or first apply): drop the stale cave WITHOUT touching the old base
			if (cavePage) { VirtualFree(cavePage, 0, MEM_RELEASE); cavePage = nullptr; }
			factorAddr = 0; base = 0; applied = false;
			apply();
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "OffscreenShadowCasters load-apply skipped: " << ex.what(); }
	}

public:
	OffscreenShadowCastersImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->offscreenShadowCastersToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMultiplierCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->offscreenShadowCastersMultiplier->valueChangedEvent, [this](float& n) { onMultiplierChanged(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}

	~OffscreenShadowCastersImpl()
	{
		try { revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


OffscreenShadowCasters::OffscreenShadowCasters(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<OffscreenShadowCastersImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("OffscreenShadowCasters not impl for this game");
	}
}

OffscreenShadowCasters::~OffscreenShadowCasters()
{
	PLOG_DEBUG << "~" << getName();
}
