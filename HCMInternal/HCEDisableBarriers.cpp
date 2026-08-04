#include "pch.h"
#include "HCEDisableBarriers.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include <atomic>
#include <mutex>
#include <vector>

// ================================================================================================================
// Halo Campaign Evolved soft ceilings ("Disable Barriers").
//
// THE ENGINE OWNS AN ENABLE BIT PER SOFT CEILING AND WE JUST CLEAR THEM ALL.
//
// HaloScript's soft_ceiling_enable(name, bool) is sub_1801B4670. Stripped of the name lookup it is:
//
//   scenario = *(void**)scenarioSlot;
//   for (i = 0; i < *(int32*)(scenario + 0x248); ++i)          // 0x248 = soft ceiling COUNT
//       if (softCeiling[i].name == wantedName)                  // block address at 0x24C, stride 12, name at +4
//       {
//           mask = *(uint32**)(tlsBase + 0x430);
//           if (enable) mask[i >> 5] |=  (1 << i);
//           else        mask[i >> 5] &= ~(1 << i);
//       }
//
// and the consumer, sub_180355540 (4 call sites), looks the soft ceiling up by name and then returns NULL - i.e.
// "this soft ceiling does not exist" - whenever its bit is clear:
//
//   mask = *(uint32*)( *(void**)(tlsBase + 0x430) + 4*(i >> 5) );
//   if (_bittest(&mask, i)) return &softCeiling[i];
//   return 0;
//
// So: BIT SET = ENABLED. Zeroing the mask disables every barrier, using the engine's own supported mechanism.
// No code is patched and no tag data is touched.
//
// THE MASK IS GAME STATE, SO IT IS PART OF EVERY CHECKPOINT.
// Reverting, dying, or loading a level restores the engine's own mask, so a write-once implementation appears to
// work and then silently un-disables - the same trap HCEFreezeAI documents. Hence the throttled re-assert pass on
// BackgroundRenderEvent. That also drives the SAVE side: whenever the pass observes a non-zero mask, that is the
// engine's freshly restored state, so we capture it as the restore value BEFORE zeroing it again. The capture is
// tagged with the scenario pointer it came from, so a mask captured on one level is never written back on
// another.
//
// This being in the checkpoint blob is also why the restore is a plain write of the captured words rather than
// "set all bits": some soft ceilings are legitimately disabled by level scripts, and clobbering those to enabled
// would change scripted behaviour.
//
// ADDRESS RESOLUTION: the scenario data pointer is found by SIGNATURE, not by a hardcoded RVA. HCE has no version
// resource (every build reports 0.0.0.0), so a moved global cannot be detected by version gating; a signature
// that stops matching disables the feature with a message instead of reading a wrong pointer. The pattern is the
// same one HCETriggerOverlay uses and anchors on the engine's own trigger-volume count compare:
//   48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00     mov rax, [rip+scenarioDataPtr] ; cmp ecx, [rax+278h]
//
// NOT DONE HERE: kill volumes. They have a second, independent mask at tlsBase + 0x340 with INVERTED polarity
// (bit set = disabled), indexed by each trigger volume's kill-index field at +0x78 rather than by a flat count
// (see sub_1801B2440 / sub_1801B2540). Invulnerability already stops kill volumes killing the player, which is
// what a user actually wants, so this is deliberately left out rather than shipped half-verified.
// ================================================================================================================

namespace
{
	constexpr int kMaxSigBytes = 64;
	struct HceSigPattern
	{
		unsigned char bytes[kMaxSigBytes];
		bool wild[kMaxSigBytes];
		int length;
	};

	int hexNibble(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	bool parsePattern(const char* text, HceSigPattern& out)
	{
		out.length = 0;
		while (*text && out.length < kMaxSigBytes)
		{
			if (*text == ' ' || *text == '\t') { ++text; continue; }
			if (*text == '?')
			{
				out.bytes[out.length] = 0;
				out.wild[out.length] = true;
				++out.length;
				++text;
				if (*text == '?') ++text;
				continue;
			}
			const int hi = hexNibble(text[0]); if (hi < 0) return false;
			const int lo = hexNibble(text[1]); if (lo < 0) return false;
			out.bytes[out.length] = (unsigned char)((hi << 4) | lo);
			out.wild[out.length] = false;
			++out.length;
			text += 2;
		}
		return out.length > 0;
	}

	// POD only - it holds no C++ object with a destructor, which is what lets it use __try under /EHsc.
	int scanExecutableSections(uintptr_t base, const unsigned char* pattern, const bool* wild, int length,
		uintptr_t* outHits, int maxHits)
	{
		int found = 0;
		__try
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
			for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
			{
				if (!(section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				const DWORD virtualSize = section[s].Misc.VirtualSize ? section[s].Misc.VirtualSize : section[s].SizeOfRawData;
				if (virtualSize <= (DWORD)length) continue;

				const unsigned char* const bytes = (const unsigned char*)(base + section[s].VirtualAddress);
				const size_t span = (size_t)virtualSize - (size_t)length;
				const unsigned char firstByte = pattern[0];
				const bool firstWild = wild[0];

				for (size_t i = 0; i <= span; ++i)
				{
					if (!firstWild && bytes[i] != firstByte) continue;
					int k = 1;
					for (; k < length; ++k)
						if (!wild[k] && bytes[i + k] != pattern[k]) break;
					if (k != length) continue;
					if (found < maxHits) outHits[found] = (uintptr_t)(bytes + i);
					++found;
					if (found > maxHits) return found;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		return found;
	}

	uintptr_t ripTarget(uintptr_t instruction, int dispOffset, int insnLength)
	{
		int32_t displacement = 0;
		if (!HCEGetPlayerState::tryReadRaw(instruction + dispOffset, &displacement, sizeof(displacement))) return 0;
		return instruction + insnLength + (intptr_t)displacement;
	}

	// Exactly one hit or nothing. Zero means the build moved; more than one means the pattern is not specific
	// enough to trust. Neither is allowed to fall back to a remembered address.
	uintptr_t resolveUniqueMatch(uintptr_t base, const char* patternText, int& outHits)
	{
		outHits = 0;
		HceSigPattern pattern{};
		if (!parsePattern(patternText, pattern)) return 0;
		uintptr_t hits[4]{};
		const int count = scanExecutableSections(base, pattern.bytes, pattern.wild, pattern.length, hits, 4);
		outHits = count;
		return (count == 1) ? hits[0] : 0;
	}

	// All guarded (SEH inside tryReadRaw), so a moved offset degrades to a zero rather than a crash.
	int32_t   readI32(uintptr_t address) { int32_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t address) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(address, &v, sizeof(v)); return v; }
	bool plausiblePointer(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }
}


class HCEDisableBarriers::HCEDisableBarriersImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	// MAXIMUM_SCENARIO_SOFT_CEILINGS_PER_SCENARIO. A count outside [0, this] means we are not looking at a
	// scenario, so we refuse rather than writing through a bad pointer.
	static constexpr int32_t kMaxSoftCeilings = 0x80;
	static constexpr int64_t kSoftCeilingCountOffset = 0x248; // scenario + 0x248
	static constexpr int64_t kSoftCeilingMaskTlsOffset = 0x430; // *(tls + 0x430) -> uint32[]

	std::atomic<bool> mReady{ false };
	std::atomic<bool> mWantDisabled{ false };

	// EVERYTHING BELOW IS GUARDED BY mStateMutex. Two threads reach it: the hotkey/GUI thread on toggle and the
	// render thread on the maintain pass. Both go through resolve(), which lazily populates the anchor fields, so
	// the lock has to cover the anchor resolution as well as the saved mask - not just the write.
	std::mutex mStateMutex;

	// ---- signature-resolved, once per session ----
	bool mAnchorsTried = false;
	uintptr_t mScenarioSlot = 0;
	std::string mAnchorFailure;

	// ---- restore state ----
	std::vector<uint32_t> mSavedMask;
	uintptr_t mSavedForScenario = 0;

	std::chrono::steady_clock::time_point mLastMaintain{};

	// Never throws. Returns 0 if unavailable.
	uintptr_t scenarioSlot()
	{
		if (mAnchorsTried) return mScenarioSlot;
		mAnchorsTried = true;

		HMODULE sim = GetModuleHandleW(mGame.toModuleName().c_str());
		if (!sim) { mAnchorFailure = "HaloSimulation_tag_release.dll is not loaded"; mAnchorsTried = false; return 0; }

		int hits = 0;
		const uintptr_t insn = resolveUniqueMatch((uintptr_t)sim, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!insn) { mAnchorFailure = std::format("could not locate the scenario data pointer ({} signature matches)", hits); return 0; }

		mScenarioSlot = ripTarget(insn, 3, 7);
		if (!mScenarioSlot) mAnchorFailure = "could not read the scenario data pointer operand";
		return mScenarioSlot;
	}

	// Resolves scenario + soft ceiling count + mask address. Returns false (silently) whenever the level is not
	// loaded - that is the normal state at a menu and during a load.
	bool resolve(uintptr_t& outScenario, int32_t& outCount, uintptr_t& outMask)
	{
		const uintptr_t slot = scenarioSlot();
		if (!slot) return false;

		outScenario = readPtr(slot);
		if (!plausiblePointer(outScenario)) return false;

		outCount = readI32(outScenario + kSoftCeilingCountOffset);
		if (outCount <= 0 || outCount > kMaxSoftCeilings) return false;

		auto playerState = playerStateWeak.lock();
		if (!playerState) return false;

		// The mask lives in the GAME THREAD's TLS block, so this must go through HCEGetPlayerState's thread walk -
		// reading NtCurrentTeb() from the render thread would give a different, wrong block.
		outMask = readPtr(playerState->getTlsBase() + kSoftCeilingMaskTlsOffset);
		return plausiblePointer(outMask);
	}

	static size_t maskWords(int32_t count) { return (size_t)((count + 31) / 32); }

	// Captures the engine's current mask as the restore value if it is non-zero, then zeroes it.
	// Caller holds mStateMutex.
	void captureAndZero(uintptr_t scenario, int32_t count, uintptr_t mask)
	{
		const size_t words = maskWords(count);
		std::vector<uint32_t> current(words, 0);
		if (!HCEGetPlayerState::tryReadRaw(mask, current.data(), words * sizeof(uint32_t))) return;

		bool anySet = false;
		for (uint32_t w : current) if (w != 0) { anySet = true; break; }

		if (anySet)
		{
			// This is the engine's own state (fresh level, or restored by a revert). Remember it, tagged with the
			// scenario it belongs to so it can never be written back onto a different level.
			mSavedMask = current;
			mSavedForScenario = scenario;

			const std::vector<uint32_t> zeroes(words, 0);
			HCEGetPlayerState::tryWriteRaw(mask, zeroes.data(), words * sizeof(uint32_t));
		}
	}

	void onToggleChange(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			mWantDisabled.store(newValue, std::memory_order_release);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return; // the maintain pass will apply it later

			std::scoped_lock lock(mStateMutex);

			uintptr_t scenario = 0, mask = 0;
			int32_t count = 0;
			if (!resolve(scenario, count, mask))
			{
				if (!mAnchorFailure.empty()) throw HCMRuntimeException(std::format("Halo Campaign Evolved barriers: {}", mAnchorFailure));
				messagesGUI->addMessage("This level has no barriers (soft ceilings).");
				return;
			}

			if (newValue)
			{
				captureAndZero(scenario, count, mask);
				messagesGUI->addMessage(std::format("Barriers disabled ({} soft ceilings).", count));
			}
			else
			{
				if (!mSavedMask.empty() && mSavedForScenario == scenario)
					HCEGetPlayerState::tryWriteRaw(mask, mSavedMask.data(), mSavedMask.size() * sizeof(uint32_t));
				messagesGUI->addMessage("Barriers re-enabled.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Throttled re-assert. Deliberately silent - at a menu, mid load or while dead the chain is legitimately
	// unresolvable and nothing is wrong, so this must never throw or log-spam.
	void onRenderEvent(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mWantDisabled.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		if (mLastMaintain.time_since_epoch().count() != 0 && (now - mLastMaintain) < std::chrono::milliseconds(250)) return;
		mLastMaintain = now;

		try
		{
			auto mccStateHook = mccStateHookWeak.lock();
			if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame)) return;

			std::scoped_lock lock(mStateMutex);

			uintptr_t scenario = 0, mask = 0;
			int32_t count = 0;
			if (!resolve(scenario, count, mask)) return;

			captureAndZero(scenario, count, mask); // no-op when the mask is already all zeroes
		}
		catch (HCMRuntimeException)
		{
			// expected while the chain is down
		}
	}

	// Declared LAST - a ScopedCallback subscribes in its own constructor. See HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallbackHandle;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEDisableBarriersImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mToggleCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->disableBarriersToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		// explicit cast - GameState has operator==(GameState) AND operator Value(), so a direct compare is C2666
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEDisableBarriers only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEDisableBarriersImpl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallbackHandle.removeCallback();
		mRenderEventCallback.removeCallback();

		// Put the engine's barriers back. Best effort: if the level has changed since we captured, the scenario
		// tag will not match and we correctly leave it alone.
		try
		{
			std::scoped_lock lock(mStateMutex);
			if (!mSavedMask.empty())
			{
				uintptr_t scenario = 0, mask = 0;
				int32_t count = 0;
				if (resolve(scenario, count, mask) && scenario == mSavedForScenario)
					HCEGetPlayerState::tryWriteRaw(mask, mSavedMask.data(), mSavedMask.size() * sizeof(uint32_t));
			}
		}
		catch (...) {}
	}
};


HCEDisableBarriers::HCEDisableBarriers(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEDisableBarriersImpl>(game, dicon))
{
}

HCEDisableBarriers::~HCEDisableBarriers()
{
	PLOG_VERBOSE << "~" << getName();
}
