#include "pch.h"
#include "H5PauseMenuFix.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include <Psapi.h>

// See H5PauseMenuFix.h for the full derivation of the cooldown and why this write is safe.

namespace
{
	// ⚠ THE VALUE IS A HARD-CODED LITERAL AND MUST STAY ONE. A NaN here permanently BLOCKS the pause menu
	// instead of unblocking it (comiss with NaN fails the `ja` and takes the `jbe` to "return false").
	// Never compute this from game state, never let another float flow into it.
	constexpr float kFarInThePast = -1000.0f;

	// The gate, with every rip-relative displacement wildcarded - the project rule for this title, which
	// ships no version resource and whose exe cannot be read off disk to diff builds.
	//   F3 0F 10 15 ?? ?? ?? ??   movss xmm2, [lastOpened]
	//   0F 28 C8                  movaps xmm1, xmm0
	//   F3 0F 5C CA               subss xmm1, xmm2
	//   0F 2F 0D ?? ?? ?? ??      comiss xmm1, [3.0f]
	//   77 09                     ja allow
	//   0F 2F D0                  comiss xmm2, xmm0
	//   0F 86 ?? ?? ?? ??         jbe return-false
	//   33 D2                     xor edx, edx
	//   F3 0F 11 05               movss [lastOpened], xmm0
	// Verified to match EXACTLY ONCE across the whole image by two independently written scanners.
	constexpr uint8_t kSigBytes[] = {
		0xF3,0x0F,0x10,0x15, 0,0,0,0,
		0x0F,0x28,0xC8,
		0xF3,0x0F,0x5C,0xCA,
		0x0F,0x2F,0x0D, 0,0,0,0,
		0x77,0x09,
		0x0F,0x2F,0xD0,
		0x0F,0x86, 0,0,0,0,
		0x33,0xD2,
		0xF3,0x0F,0x11,0x05,
	};
	constexpr bool kSigMask[] = {
		1,1,1,1, 0,0,0,0,
		1,1,1,
		1,1,1,1,
		1,1,1, 0,0,0,0,
		1,1,
		1,1,1,
		1,1, 0,0,0,0,
		1,1,
		1,1,1,1,
	};
	static_assert(sizeof(kSigBytes) == sizeof(kSigMask) / sizeof(kSigMask[0]), "signature/mask length mismatch");

	// The RVA this resolved to on build 1.194.6192.2. Used ONLY as a soft cross-check that gets logged -
	// never as a hard assert, because the signature derivation is self-validating and a rebased or repacked
	// build would fail an equality test while the derivation was still correct.
	constexpr uintptr_t kExpectedRva = 0x063F63B8;

	bool sehWriteFloat(void* dest, float v)
	{
		__try { *(float*)dest = v; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehReadFloat(const void* src, float& out)
	{
		__try { out = *(const float*)src; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// ⚠ SEH ONLY - NO C++ OBJECTS WITH DESTRUCTORS MAY LIVE IN HERE. MSVC rejects __try in any function
	// that requires object unwinding (C2712), which is why the scan is split out of the caller rather than
	// written inline next to the shared_ptr locks and std::format calls.
	// Returns the number of matches found (stops counting at 2) and writes the first match to outMatch.
	int sehScanForSignature(const uint8_t* start, size_t span, uintptr_t& outMatch)
	{
		int matches = 0;
		outMatch = 0;
		__try
		{
			for (size_t i = 0; i < span; ++i)
			{
				const uint8_t* p = start + i;
				bool ok = true;
				for (size_t k = 0; k < sizeof(kSigBytes); ++k)
				{
					if (!kSigMask[k]) continue;
					if (p[k] != kSigBytes[k]) { ok = false; break; }
				}
				if (ok)
				{
					if (matches == 0) outMatch = (uintptr_t)p;
					if (++matches > 1) break;
				}
			}
			return matches;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}
}

class H5PauseMenuFix::Impl
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

	uintptr_t mCooldownAddress = 0;   // resolved once, on first enable
	bool mResolveTried = false;
	bool mResolveFailed = false;
	std::chrono::steady_clock::time_point mLastFailureLog{};

	// Scan the module's mapped image for the gate and derive the global from its own rip-relative operand.
	// ⚠ Requires EXACTLY ONE match. If the count is anything else we write nothing at all: on a changed
	// build a "best guess" address is a write into unknown memory, which is the one outcome worth avoiding.
	uintptr_t resolveCooldownAddress()
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t base = playerState->getExeBase();

		MODULEINFO mi{};
		HMODULE mod = (HMODULE)base;
		if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)) || mi.SizeOfImage < 0x1000)
			throw HCMRuntimeException("Could not measure the Halo 5 module for the pause menu scan");

		const size_t span = (size_t)mi.SizeOfImage - sizeof(kSigBytes);

		uintptr_t match = 0;
		const int matches = sehScanForSignature((const uint8_t*)base, span, match);
		if (matches < 0)
			throw HCMRuntimeException("Faulted while scanning for the Halo 5 pause menu cooldown");

		if (matches != 1)
			throw HCMRuntimeException(std::format(
				"Pause Menu Fix cannot run on this game build: the pause cooldown signature matched {} times "
				"(expected exactly 1). Halo 5 reports no version number, so HCM cannot detect an update any "
				"other way and would rather do nothing than write to a guessed address.", matches));

		// movss xmm2, [rip + disp32] - the next instruction begins at match+8, so the target is
		// (match + 8) + disp32. This is self-validating: it comes out of the matched instruction itself.
		int32_t disp = 0;
		memcpy(&disp, (const void*)(match + 4), sizeof(disp));
		const uintptr_t target = (match + 8) + (intptr_t)disp;

		if (target < base || target >= base + mi.SizeOfImage)
			throw HCMRuntimeException("The Halo 5 pause cooldown resolved outside the module; refusing to write");

		const uintptr_t rva = target - base;
		if (rva != kExpectedRva)
			PLOG_WARNING << "Pause Menu Fix: cooldown resolved to RVA 0x" << std::hex << rva
				<< " but this build was mapped at 0x" << kExpectedRva
				<< ". Proceeding - the signature derivation is authoritative - but the game may have updated.";
		else
			PLOG_DEBUG << "Pause Menu Fix: cooldown global at exe+0x" << std::hex << rva;

		return target;
	}

	void onTick()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			if (!mCooldownAddress)
			{
				if (mResolveFailed) return;   // already reported; do not retry every frame
				mCooldownAddress = resolveCooldownAddress();
			}

			// A plain 4-byte store. Naturally aligned, so it cannot tear against the engine's own movss,
			// and the engine re-latches it on the next real open if we stop.
			if (!sehWriteFloat((void*)mCooldownAddress, kFarInThePast))
				throw HCMRuntimeException("Could not write the Halo 5 pause menu cooldown");
		}
		catch (HCMRuntimeException& ex)
		{
			// Resolution failures are permanent for this session - stop retrying and tell the user once.
			if (!mCooldownAddress)
			{
				mResolveFailed = true;
				if (auto settings = settingsWeak.lock())
				{
					settings->h5PauseMenuFixToggle->GetValueDisplay() = false;
					settings->h5PauseMenuFixToggle->UpdateValueWithInput();
				}
				mTickCallback.reset();
				ex.prepend("Pause Menu Fix: ");
				runtimeExceptions->handleMessage(ex);
				return;
			}
			// A transient write failure (mid level load) is not worth switching the feature off over.
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Pause Menu Fix skipped a tick: " << ex.what();
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
				mResolveFailed = false;
				mTickCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onTick(); });
			}
			else if (!newValue && mTickCallback)
			{
				mTickCallback.reset();
				// ⚠ Deliberately no restore. The engine writes the correct game time back into the global
				// the next time the menu genuinely opens, so the only consequence of leaving -1000 behind is
				// that the very first open after switching off is also un-throttled. Writing back a cached
				// value would be worse: it could reinstate a stale timestamp from before a level change.
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Pause Menu Fix: ");
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
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5PauseMenuFixToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5PauseMenuFix only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);

		try
		{
			if (auto settings = settingsWeak.lock(); settings && settings->h5PauseMenuFixToggle->GetValue())
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
	}
};


H5PauseMenuFix::H5PauseMenuFix(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5PauseMenuFix::~H5PauseMenuFix() { PLOG_VERBOSE << "~" << getName(); }
