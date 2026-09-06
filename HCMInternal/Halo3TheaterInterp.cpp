#include "pch.h"
#include "Halo3TheaterInterp.h"
#include "Halo3TheaterInterpCave.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ScopedThreadSuspender.h"
#include "GlobalKill.h"
#include <Windows.h>
#include <Psapi.h>

// ================================================================================================
// HALO 3 THEATER INTERPOLATION
//
// Halo 3 MCC renders Theater straight off the tick stream. The engine DOES interpolate object
// POSITION (sub_180184438), which is why walking and animations look smooth - but camera
// ORIENTATION is rebuilt from raw per-tick unit aiming, so the moment the watched player turns, the
// whole world steps at the tick rate. In gameplay the camera forward comes from live input at frame
// rate, so it never shows; this is Theater-only.
//
// Three sites fix the three visible symptoms. They were found over a long RE session (see
// Fixes/Halo3_Theater_Interp_FIXES.md) and are shipped here as the cave that session produced.
//
// THE CAVE PREFERS moduleBase + kCaveRva (there it is byte-identical to the recovered blob), but it
// is no longer pinned to it: kCaveRva is one page past halo3.dll's image end, i.e. unowned address
// space that another allocation can and did take. relocateCave() lifts the pin - see the header for
// why only 13 fixups are needed and why placement is bounded to +/-2GB.
//
// ⚠⚠ WHY THE ORIGINAL BYTES ARE VERIFIED. These are jmp-over-instruction patches. If an MCC update
// moves the code, a stale RVA lands the jmp in the MIDDLE of an instruction and the game executes
// nonsense - the same failure mode that shipped a reliable crash-on-pause on HaloCER. Every site
// carries the bytes it expects and the patch refuses on a mismatch.
//
// KNOWN LIMITATION, deliberately shipped: in first person the LEGS still step when you turn. That is
// a separate, unsolved problem - the render alpha is pinned to ~1 at every biped-pose site (they are
// per-tick), so no in-chain lerp can smooth them. Notes in Fixes/Halo3_FP_Legs_TurnJitter_TODO.md.
// ================================================================================================
class Halo3TheaterInterp::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	uintptr_t mBase = 0;
	uint8_t*  mCave = nullptr;
	bool      mApplied = false;

	struct Save { uintptr_t addr; std::vector<uint8_t> orig; };
	std::vector<Save> mSaves;
	// A revert cannot free the cave immediately - a render frame may still be executing inside it.
	// Same reasoning as FPScaleFix's deferred free.
	std::vector<uint8_t*> mPendingFree;

	static uintptr_t moduleBase()
	{
		return (uintptr_t)GetModuleHandleW(L"halo3.dll");
	}

	void writeSaved(uintptr_t addr, const uint8_t* data, size_t len)
	{
		Save s; s.addr = addr; s.orig.resize(len);
		memcpy(s.orig.data(), (void*)addr, len);
		mSaves.push_back(std::move(s));
		DWORD o; VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &o);
		memcpy((void*)addr, data, len);
		VirtualProtect((void*)addr, len, o, &o);
		FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
	}

	// ⚠ Reads through a __try: an RVA that a game update invalidated can land outside the image.
	static bool readBytes(uintptr_t addr, uint8_t* out, size_t len)
	{
		__try { memcpy(out, (const void*)addr, len); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool verifySites(std::string& why) const
	{
		for (size_t i = 0; i < Halo3TheaterInterp_Detail::kHookCount; ++i)
		{
			const auto& h = Halo3TheaterInterp_Detail::kHooks[i];
			uint8_t actual[16]{};
			if (!readBytes(mBase + h.rva, actual, h.stolen))
			{
				why = std::format("could not read the {} site at halo3.dll+0x{:X}", h.name, h.rva);
				return false;
			}
			if (memcmp(actual, h.original, h.stolen) != 0)
			{
				why = std::format("the {} site at halo3.dll+0x{:X} does not hold the bytes this build "
					"expects - Halo 3 has been updated, or another mod already patched it. Patching anyway "
					"would land a jump inside an instruction.", h.name, h.rva);
				return false;
			}
		}
		return true;
	}

	// A free page within rel32 reach of the module, searched outward from the module base. The cave's
	// four branches back into halo3.dll are rel32, so 2GB is a hard wall - we stay well inside it.
	// (Same shape as MasterTickrate's allocNear; kept local so this TU stays self-contained.)
	static void* allocNearModule(uintptr_t moduleBase, size_t size)
	{
		SYSTEM_INFO si; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		const uintptr_t reach = 0x60000000ULL;                       // 1.5GB, comfortably inside rel32
		const uintptr_t lo = moduleBase > reach ? moduleBase - reach : 0x10000ULL;
		const uintptr_t hi = moduleBase + reach;
		const uintptr_t start = moduleBase & ~(gran - 1);
		for (uintptr_t p = start; p > lo; p -= gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		for (uintptr_t p = start + gran; p < hi; p += gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		return nullptr;
	}

	bool apply()
	{
		if (mApplied) return true;

		mBase = moduleBase();
		if (!mBase)
			throw HCMRuntimeException("halo3.dll is not loaded yet - load a level first");

		std::string why;
		if (!verifySites(why))
			throw HCMRuntimeException("Theater Interpolation Fix: " + why);

		// Free anything a previous revert deferred, BEFORE the suspender exists (see ScopedThreadSuspender:
		// no virtual-memory calls while threads are frozen).
		for (uint8_t* p : mPendingFree) VirtualFree(p, 0, MEM_RELEASE);
		mPendingFree.clear();

		// PREFER moduleBase + kCaveRva - there the cave is byte-identical to the recovered blob and no
		// fixups apply at all. But that page is one past halo3.dll's image end (SizeOfImage is exactly
		// 0x4768000), so it is unowned space anything can take, and something did. Fall back to any slot
		// within +/-2GB and relocate; only refuse if nothing in rel32 range is free.
		const uintptr_t want = mBase + Halo3TheaterInterp_Detail::kCaveRva;
		mCave = (uint8_t*)VirtualAlloc((void*)want, Halo3TheaterInterp_Detail::kCaveSize,
			MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

		bool relocated = false;
		if (!mCave)
		{
			mCave = (uint8_t*)allocNearModule(mBase, Halo3TheaterInterp_Detail::kCaveSize);
			if (!mCave)
				throw HCMRuntimeException(std::format(
					"Theater Interpolation Fix: could not reserve the 4KB cave anywhere within 2GB of "
					"halo3.dll (preferred halo3.dll+0x{:X} is taken). Restarting the game should clear it.",
					Halo3TheaterInterp_Detail::kCaveRva));
			relocated = true;
		}

		memcpy(mCave, Halo3TheaterInterp_Detail::kCave, Halo3TheaterInterp_Detail::kCaveSize);

		if (relocated)
		{
			// A wrong fixup would send the cave's `jmp rax` into arbitrary memory, so prove it rather
			// than assume it: relocate, then re-derive every game address the way the cave will and
			// require it to match. Anything short of an exact match frees the page and refuses.
			if (!Halo3TheaterInterp_Detail::relocateCave(mCave, (uintptr_t)mCave, mBase)
				|| !Halo3TheaterInterp_Detail::verifyRelocation(mCave, (uintptr_t)mCave, mBase))
			{
				VirtualFree(mCave, 0, MEM_RELEASE); mCave = nullptr;
				throw HCMRuntimeException(
					"Theater Interpolation Fix: the cave was placed away from its preferred address and "
					"the relocation did not verify. Refusing to arm.");
			}
			PLOG_INFO << "Halo3TheaterInterp: cave relocated to " << (void*)mCave
				<< " (preferred halo3.dll+0x" << std::hex << Halo3TheaterInterp_Detail::kCaveRva
				<< " was taken); " << std::dec << Halo3TheaterInterp_Detail::kAbsFixupCount
				<< " absolute + " << Halo3TheaterInterp_Detail::kRelFixupCount << " rel32 fixups verified";
		}

		// Knob values. ⚠ Write ONLY what we mean to change - every knob already holds the value the
		// recovered (working) blob shipped, so a stray write is a behaviour change, not a no-op. HCM
		// used to write 3 into DESTGATE and 1 into OBSPOS through a mislabelled table; that second one
		// is what made the body detach. OBSPOS is written explicitly as 0 so the intent is on the record
		// rather than relying on the blob's contents.
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobFpCode - Halo3TheaterInterp_Detail::kCaveRva)) = 3;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobEnable - Halo3TheaterInterp_Detail::kCaveRva)) = 1;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobObsPos - Halo3TheaterInterp_Detail::kCaveRva)) = 0;
		// Our own knob (not from the blob): 1 = interpolate the FP leg anchor. Poke it to 0 live to A/B
		// the leg fix without disturbing the camera fix; kLegFixHits counts hooked frames, so a stuck-at-0
		// counter means the hook never fired rather than that the interpolation did nothing.
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobLegFix - Halo3TheaterInterp_Detail::kCaveRva)) = 1;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kLegFixHits - Halo3TheaterInterp_Detail::kCaveRva)) = 0;
		setCrouchKnob(currentCrouchSetting());

		FlushInstructionCache(GetCurrentProcess(), mCave, Halo3TheaterInterp_Detail::kCaveSize);

		// Arm the three hooks with every other thread frozen: the render path runs per-frame and a torn
		// instruction here is an immediate crash.
		{
			ScopedThreadSuspender suspend;
			for (size_t i = 0; i < Halo3TheaterInterp_Detail::kHookCount; ++i)
			{
				const auto& h = Halo3TheaterInterp_Detail::kHooks[i];
				uint8_t patch[16];
				memset(patch, 0x90, sizeof(patch));            // NOP the remainder of the stolen bytes
				patch[0] = 0xE9;
				// ⚠ Relative to the cave's ACTUAL base, not mBase + caveRva - the cave may have been
				// relocated above, and this jmp is what enters it.
				const uintptr_t entry = (uintptr_t)mCave + (h.caveRva - Halo3TheaterInterp_Detail::kCaveRva);
				const int32_t rel = (int32_t)((intptr_t)entry - (intptr_t)(mBase + h.rva + 5));
				memcpy(patch + 1, &rel, 4);
				writeSaved(mBase + h.rva, patch, h.stolen);
			}
		}

		mApplied = true;
		return true;
	}

	void revert()
	{
		if (!mApplied) return;
		{
			ScopedThreadSuspender suspend;
			// Reverse order: the hooks come off first so nothing can enter the cave again.
			for (auto it = mSaves.rbegin(); it != mSaves.rend(); ++it)
			{
				DWORD o; VirtualProtect((void*)it->addr, it->orig.size(), PAGE_EXECUTE_READWRITE, &o);
				memcpy((void*)it->addr, it->orig.data(), it->orig.size());
				VirtualProtect((void*)it->addr, it->orig.size(), o, &o);
				FlushInstructionCache(GetCurrentProcess(), (void*)it->addr, it->orig.size());
			}
		}
		mSaves.clear();
		if (mCave) { mPendingFree.push_back(mCave); mCave = nullptr; }   // deferred - a frame may be inside it
		mApplied = false;
	}

	bool currentCrouchSetting() const
	{
		if (auto s = settingsWeak.lock()) return s->halo3TheaterInterpCrouch->GetValue();
		return true;
	}

	void setCrouchKnob(bool on)
	{
		if (!mCave) return;
		*(uint32_t*)(mCave + (Halo3TheaterInterp_Detail::kKnobCrouch - Halo3TheaterInterp_Detail::kCaveRva))
			= on ? 1u : 0u;
	}

	void onToggle(bool& newValue)
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (newValue) apply(); else revert();

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Theater Interpolation Fix on."
					: "Theater Interpolation Fix off.");
		}
		catch (HCMRuntimeException ex)
		{
			try { revert(); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onCrouchToggle(bool& newValue)
	{
		// A live knob - no re-patching, the cave reads it every frame.
		setCrouchKnob(newValue);
	}

	// Declared LAST - ScopedCallbacks subscribe inside their own constructors.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ToggleEvent> mCrouchCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->halo3TheaterInterpToggle->valueChangedEvent,
			[this](bool& n) { onToggle(n); }),
		mCrouchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->halo3TheaterInterpCrouch->valueChangedEvent,
			[this](bool& n) { onCrouchToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo3)
			throw HCMInitException("Theater Interpolation Fix is Halo 3 only");
	}

	~Impl()
	{
		mToggleCallback.removeCallback();
		mCrouchCallback.removeCallback();
		try { revert(); } catch (...) {}
		for (uint8_t* p : mPendingFree) VirtualFree(p, 0, MEM_RELEASE);
		if (mCave) VirtualFree(mCave, 0, MEM_RELEASE);
	}
};


Halo3TheaterInterp::Halo3TheaterInterp(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

Halo3TheaterInterp::~Halo3TheaterInterp()
{
	PLOG_VERBOSE << "~" << getName();
}
