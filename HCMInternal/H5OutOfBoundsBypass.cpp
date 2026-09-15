#include "pch.h"
#include "H5OutOfBoundsBypass.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5OutOfBoundsBypass.h - there are TWO killers and this handles both.

namespace
{
	// ---- killer 1: the player "outside the world" tick counter ----------------------------------
	constexpr uintptr_t kPlayerOutsideWorldTicks = 0x89;

	// ---- killer 2: the broadphase-exit object delete --------------------------------------------
	// 8B 4C 24 50  E8 ?? ?? ?? ??  48 83 C7 04  49 3B FF  0F 85
	// The match points at the `mov ecx, [rsp+0x50]`; the call we NOP starts 4 bytes later.
	constexpr uint8_t kSigBytes[] = {
		0x8B, 0x4C, 0x24, 0x50, 0xE8, 0x00, 0x00, 0x00, 0x00,
		0x48, 0x83, 0xC7, 0x04, 0x49, 0x3B, 0xFF, 0x0F, 0x85
	};
	constexpr bool kSigMask[] = {   // false = wildcard
		true, true, true, true, true, false, false, false, false,
		true, true, true, true, true, true, true, true, true
	};
	constexpr size_t kSigLen = sizeof(kSigBytes);
	constexpr size_t kCallOffsetInSig = 4;
	constexpr size_t kCallLength = 5;

	// ⚠ MSVC rejects __try in a function that needs object unwinding (C2712), so every SEH helper here is a
	// plain function with no C++ objects in scope.
	bool sehWrite8(void* dest, uint8_t v)
	{
		__try { *(uint8_t*)dest = v; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehCopy(void* dest, const void* src, size_t n)
	{
		__try { memcpy(dest, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Scans the module's committed pages for the one signature match. Returns 0 on failure.
	uintptr_t sehScanForSignature(uintptr_t base, size_t size)
	{
		__try
		{
			const uint8_t* p = (const uint8_t*)base;
			uintptr_t found = 0;
			for (size_t i = 0; i + kSigLen <= size; ++i)
			{
				size_t j = 0;
				for (; j < kSigLen; ++j)
				{
					if (kSigMask[j] && p[i + j] != kSigBytes[j]) break;
				}
				if (j != kSigLen) continue;
				if (found) return 0;          // not unique any more - refuse rather than guess
				found = base + i;
				i += kSigLen - 1;
			}
			return found;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
}

class H5OutOfBoundsBypass::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	std::unique_ptr<ScopedCallback<RenderEvent>> mTickCallback;
	std::chrono::steady_clock::time_point mLastFailureLog{};

	uintptr_t mDeleteCallSite = 0;        // resolved once, lazily
	bool      mScanFailed = false;
	uint8_t   mOriginalBytes[kCallLength]{};
	bool      mPatchApplied = false;

	// ---- killer 2 -------------------------------------------------------------------------------
	uintptr_t resolveDeleteCallSite()
	{
		if (mDeleteCallSite || mScanFailed) return mDeleteCallSite;

		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t base = playerState->getExeBase();
		if (!base) throw HCMRuntimeException("Could not resolve the Halo 5 module base");

		MODULEINFO mi{};
		if (!GetModuleInformation(GetCurrentProcess(), (HMODULE)base, &mi, sizeof(mi)))
		{
			mScanFailed = true;
			throw HCMRuntimeException("Could not query the Halo 5 module size");
		}

		const uintptr_t match = sehScanForSignature(base, mi.SizeOfImage);
		if (!match)
		{
			mScanFailed = true;
			throw HCMRuntimeException(
				"Could not find the Halo 5 broadphase-delete call site (the game may have updated). "
				"The 'outside the world' half of the bypass still works.");
		}

		const uintptr_t call = match + kCallOffsetInSig;
		uint8_t probe = 0;
		if (!sehCopy(&probe, (const void*)call, 1) || probe != 0xE8)
		{
			mScanFailed = true;
			throw HCMRuntimeException("The Halo 5 broadphase-delete site is not a call - refusing to patch");
		}

		mDeleteCallSite = call;
		return mDeleteCallSite;
	}

	void setDeletePatched(bool patched)
	{
		const uintptr_t call = resolveDeleteCallSite();
		if (!call || patched == mPatchApplied) return;

		DWORD oldProtect = 0;
		if (!VirtualProtect((void*)call, kCallLength, PAGE_EXECUTE_READWRITE, &oldProtect))
			throw HCMRuntimeException("Could not unprotect the Halo 5 broadphase-delete call site");

		bool ok = true;
		if (patched)
		{
			// Save what is ACTUALLY there, so restore can never write a stale hardcoded copy.
			ok = sehCopy(mOriginalBytes, (const void*)call, kCallLength);
			for (size_t i = 0; ok && i < kCallLength; ++i)
				ok = sehWrite8((void*)(call + i), 0x90);   // nop
		}
		else
		{
			ok = sehCopy((void*)call, mOriginalBytes, kCallLength);
		}

		DWORD ignored = 0;
		VirtualProtect((void*)call, kCallLength, oldProtect, &ignored);
		FlushInstructionCache(GetCurrentProcess(), (void*)call, kCallLength);

		if (!ok) throw HCMRuntimeException("Could not write the Halo 5 broadphase-delete patch");
		mPatchApplied = patched;
	}

	// ---- killer 1 -------------------------------------------------------------------------------
	void starveOutsideWorldCounter()
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t player = playerState->getPlayerArray();   // throws at a menu / during a load
		if (!player) return;

		// A plain store of 0 every tick. The engine increments this only while the object's "outside the
		// world" flag is set and resets it itself the moment you are back inside, so writing 0 is exactly
		// what the engine does on the inside-the-world path - never a value it would not write itself.
		if (!sehWrite8((void*)(player + kPlayerOutsideWorldTicks), 0))
			throw HCMRuntimeException("Could not clear the Halo 5 outside-the-world counter");
	}

	void onTick()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			starveOutsideWorldCounter();

			// Applied lazily on the first tick in-game: the module scan needs the game loaded, and doing it
			// on the toggle would fail at a menu.
			if (!mPatchApplied && !mScanFailed) setDeletePatched(true);
		}
		catch (HCMRuntimeException& ex)
		{
			// Normal at a menu, during a load and while dead. Self-healing: never switch the toggle off.
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Broadphase bypass skipped a tick (will retry): " << ex.what();
			}
		}
		catch (...) {}
	}

	void onToggleChanged(bool& newValue)
	{
		try
		{
			if (newValue && !mTickCallback)
			{
				mScanFailed = false;   // give the scan a fresh chance on every enable
				mTickCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onTick(); });
			}
			else if (!newValue && mTickCallback)
			{
				mTickCallback.reset();
				try { setDeletePatched(false); }
				catch (...) {}
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Havok Broadphase Deletion Bypass: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5OutOfBoundsBypassToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5OutOfBoundsBypass only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);

		try
		{
			if (auto settings = settingsWeak.lock(); settings && settings->h5OutOfBoundsBypassToggle->GetValue())
			{
				bool on = true;
				onToggleChanged(on);
			}
		}
		catch (...) {}
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mTickCallback.reset();
		mToggleCallback.removeCallback();
		// ⚠ MUST un-patch. HCM stays resident across sessions; leaving five NOPs in the game's .text after
		// teardown would silently disable broadphase cleanup for the rest of the process's life.
		try { setDeletePatched(false); }
		catch (...) {}
	}
};


H5OutOfBoundsBypass::H5OutOfBoundsBypass(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5OutOfBoundsBypass::~H5OutOfBoundsBypass() { PLOG_VERBOSE << "~" << getName(); }
