#include "pch.h"
#include "HCEScriptTrace.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "ModuleHook.h"
#include "RenderTextHelper.h"
#include "GlobalKill.h"
#include "ModuleCache.h"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

// See HCEScriptTrace.h for why this exists and what it deliberately does not do.

namespace
{
	// hs function definition layout. Only two fields are read here; the rest are documented in the header
	// and in the InternalPointerData comment on hceScriptFunctionTable.
	constexpr int64_t kDefNameOffset = 0x08;   // char*
	constexpr int64_t kDefArgCountOffset = 0x38;   // word

	// The table is 1188 entries in the shipped build. Read a bounded number rather than trusting a count we
	// have not verified: a bad index would otherwise walk arbitrary memory.
	constexpr int kMaxFunctionIndex = 2048;

	// ⚠ THE HOOK RUNS ON THE GAME THREAD FOR EVERY SCRIPT FUNCTION EVALUATION - hundreds per second. It must
	// not allocate, take a lock, format a string, or log. So it writes a fixed-size record into a ring buffer
	// and nothing else; the render thread resolves names and formats.
	struct TraceRecord
	{
		uint16_t functionIndex;
		uint32_t tick;             // GetTickCount, for ordering and for showing age
	};

	constexpr size_t kRingSize = 512;
	std::array<TraceRecord, kRingSize> gRing{};
	std::atomic<uint64_t> gWriteCursor{ 0 };   // monotonic; index = cursor % kRingSize

	// Set only while the cheat is on. The hook checks this first and returns immediately when clear, so a
	// parked-but-attached hook costs one relaxed atomic load per script call.
	std::atomic<bool> gTracing{ false };

	// Which indices to record. A bitset rather than a list so the hook's test is O(1). Written by the render
	// thread when the filter changes, read by the game thread - hence atomics, and hence a plain array of
	// bytes rather than std::bitset (which is not safe to write concurrently at bit granularity).
	std::array<std::atomic<uint8_t>, kMaxFunctionIndex> gWanted{};

	// The known-interesting indices, resolved by NAME at construction so a different build cannot silently
	// trace the wrong functions. print/print_if are the point; the ai_* ones show what the spawner actually
	// did rather than what it said.
	constexpr const char* kDefaultTraced[] =
	{
		"print", "print_if", "ai_place", "ai_erase", "ai_living_count", "ai_actors",
	};
}


class HCEScriptTrace::HCEScriptTraceImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	std::shared_ptr<MultilevelPointer> mEvaluateFunction;
	std::shared_ptr<MultilevelPointer> mFunctionTable;
	std::shared_ptr<ModuleMidHook> mEvaluateHook;

	std::atomic<bool> mReady{ false };

	// index -> name, filled lazily on the RENDER thread. The names live in the sim's .rdata and never move
	// while the module is loaded, but resolving one costs two reads, so cache it.
	std::mutex mNamesMutex;
	std::unordered_map<uint16_t, std::string> mNames;
	uint64_t mLastRendered = 0;

	// Reads the name for a function index out of the definition table. Render thread only.
	std::string nameForIndex(uint16_t index)
	{
		{
			std::scoped_lock lock(mNamesMutex);
			auto it = mNames.find(index);
			if (it != mNames.end()) return it->second;
		}

		std::string resolved = std::format("fn#{}", index);   // honest fallback, never a guess

		uintptr_t tableAddress = 0;
		if (mFunctionTable && mFunctionTable->resolve(&tableAddress) && index < kMaxFunctionIndex)
		{
			uintptr_t definition = 0;
			if (HCEGetPointer(tableAddress + (uintptr_t)index * 8, definition) && definition > 0x10000)
			{
				uintptr_t namePtr = 0;
				if (HCEGetPointer(definition + kDefNameOffset, namePtr) && namePtr > 0x10000)
				{
					char buffer[64]{};
					if (ReadProcessMemory(GetCurrentProcess(), (void*)namePtr, buffer, sizeof(buffer) - 1, nullptr))
					{
						buffer[sizeof(buffer) - 1] = '\0';
						// Only accept something that actually looks like a script function name.
						bool plausible = buffer[0] != '\0';
						for (const char* p = buffer; *p; ++p)
							if (!(isalnum((unsigned char)*p) || *p == '_')) { plausible = false; break; }
						if (plausible) resolved = buffer;
					}
				}
			}
		}

		std::scoped_lock lock(mNamesMutex);
		mNames[index] = resolved;
		return resolved;
	}

	// Small helper so the reads above stay readable. Guarded, because a wrong index must not fault us.
	static bool HCEGetPointer(uintptr_t address, uintptr_t& out)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery((void*)address, &mbi, sizeof(mbi))) return false;
		if (mbi.State != MEM_COMMIT) return false;
		return ReadProcessMemory(GetCurrentProcess(), (void*)address, &out, sizeof(out), nullptr);
	}

	// ⚠ ONE BYTE AT A TIME, DELIBERATELY. A single ReadProcessMemory of 63 bytes fails ENTIRELY - not
	// partially - if any part of the range is unmapped, so a name sitting near the end of a committed page
	// reads as "no name" and the function silently does not match. That is a plausible cause of finding zero
	// functions in a table that demonstrably contains them.
	static bool readCString(uintptr_t address, std::string& out, size_t maxLength = 63)
	{
		out.clear();
		for (size_t i = 0; i < maxLength; ++i)
		{
			char c = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (void*)(address + i), &c, 1, nullptr))
				return !out.empty();   // truncated is still usable
			if (c == '\0') return true;
			if (!(isalnum((unsigned char)c) || c == '_')) return false;   // not a script function name
			out.push_back(c);
		}
		return !out.empty();
	}

	// Resolves every default-traced name to its index and arms the filter. Returns how many were found, so
	// the caller can say something truthful if the table did not look as expected.
	// ⚠ THIS LOGS ITS WORKING OUT ON FAILURE, ON PURPOSE. The first version logged only successes, so when it
	// found nothing the log said only "could not find any" - which is indistinguishable between a wrong table
	// address, an unreadable table, a wrong name offset, and a name-read that fails. All four need different
	// fixes. Never ship a scan whose failure is a single unexplained sentence.
	int armDefaultFilter()
	{
		uintptr_t tableAddress = 0;
		if (!mFunctionTable || !mFunctionTable->resolve(&tableAddress))
		{
			PLOG_ERROR << "HCEScriptTrace: could not resolve the script function table pointer at all";
			return 0;
		}

		const auto simBase = ModuleCache::getModuleHandle(mGame.toModuleName());
		PLOG_DEBUG << "HCEScriptTrace: sim base 0x" << std::hex
			<< (simBase.has_value() ? (uintptr_t)simBase.value() : 0)
			<< ", function table 0x" << tableAddress << std::dec;

		for (auto& slot : gWanted) slot.store(0, std::memory_order_relaxed);

		int slotsRead = 0, plausibleDefs = 0, namedDefs = 0, armed = 0;
		std::string firstFewNames;

		for (uint16_t i = 0; i < kMaxFunctionIndex; ++i)
		{
			uintptr_t definition = 0;
			if (!HCEGetPointer(tableAddress + (uintptr_t)i * 8, definition)) continue;
			++slotsRead;
			if (definition <= 0x10000) continue;
			++plausibleDefs;

			uintptr_t namePtr = 0;
			if (!HCEGetPointer(definition + kDefNameOffset, namePtr) || namePtr <= 0x10000) continue;

			std::string name;
			if (!readCString(namePtr, name) || name.empty()) continue;
			++namedDefs;
			if (namedDefs <= 6) firstFewNames += (firstFewNames.empty() ? "" : ", ") + std::format("{}={}", i, name);

			for (const char* wanted : kDefaultTraced)
			{
				if (name == wanted)
				{
					gWanted[i].store(1, std::memory_order_release);
					{
						std::scoped_lock lock(mNamesMutex);
						mNames[i] = name;
					}
					PLOG_DEBUG << "HCEScriptTrace: tracing '" << name << "' at function index " << i;
					++armed;
					break;
				}
			}
		}

		// Logged whether it worked or not - the counts are what identify which stage broke.
		PLOG_DEBUG << "HCEScriptTrace: scanned " << kMaxFunctionIndex << " slots: " << slotsRead
			<< " readable, " << plausibleDefs << " plausible definitions, " << namedDefs
			<< " with names, " << armed << " armed. First names: " << (firstFewNames.empty() ? "(none)" : firstFewNames);

		return armed;
	}

	// ⚠ GAME THREAD, EVERY SCRIPT FUNCTION EVALUATION. Do the minimum. See the header.
	static void evaluateHookFunction(SafetyHookContext& ctx)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;

		// CX is the function index - the whole reason one hook is enough. EDX is the expression node index,
		// which is what would be needed to walk to the argument nodes once the string-data base is known.
		const uint16_t index = (uint16_t)(ctx.rcx & 0xFFFF);
		if (index >= kMaxFunctionIndex) return;
		if (!gWanted[index].load(std::memory_order_relaxed)) return;

		const uint64_t slot = gWriteCursor.fetch_add(1, std::memory_order_acq_rel);
		gRing[slot % kRingSize] = TraceRecord{ index, GetTickCount() };
	}

	void onToggleChange(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (!newValue)
			{
				gTracing.store(false, std::memory_order_release);
				if (mEvaluateHook) mEvaluateHook->setWantsToBeAttached(false);
				messagesGUI->addMessage("Script trace off.");
				return;
			}

			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
				throw HCMRuntimeException("Load into a level before starting the script trace.");

			const int armed = armDefaultFilter();
			if (armed == 0)
				throw HCMRuntimeException("Could not find any of the script functions to trace in Halo Campaign "
					"Evolved's function table. The table layout may differ in this build.");

			gWriteCursor.store(0, std::memory_order_release);
			mLastRendered = 0;
			gTracing.store(true, std::memory_order_release);
			if (mEvaluateHook) mEvaluateHook->setWantsToBeAttached(true);

			messagesGUI->addMessage(std::format("Script trace on - watching {} script functions.", armed));
		}
		catch (HCMRuntimeException ex)
		{
			gTracing.store(false, std::memory_order_release);
			if (mEvaluateHook) mEvaluateHook->setWantsToBeAttached(false);
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!gTracing.load(std::memory_order_acquire)) return;

		try
		{
			auto settings = settingsWeak.lock();
			if (!settings) return;

			const int wantLines = std::clamp((int)settings->hceScriptTraceLineCount->GetValue(), 1, 40);

			const uint64_t cursor = gWriteCursor.load(std::memory_order_acquire);
			if (cursor == 0) return;

			// Newest last, so it reads like a log. Only the last wantLines records, and never more than the
			// ring holds - if the game outran us we say so rather than pretending the trace is complete.
			const uint64_t available = std::min<uint64_t>(cursor, kRingSize);
			const uint64_t take = std::min<uint64_t>(available, (uint64_t)wantLines);
			const uint64_t first = cursor - take;

			const uint32_t now = GetTickCount();
			std::string text = "HaloScript trace";
			if (cursor > kRingSize && mLastRendered != 0 && (cursor - mLastRendered) > kRingSize)
				text += std::format(" (dropped ~{} calls since last frame)", (cursor - mLastRendered) - kRingSize);
			text += "\n";

			for (uint64_t i = first; i < cursor; ++i)
			{
				const TraceRecord record = gRing[i % kRingSize];
				const uint32_t ageMs = now - record.tick;   // wrap-safe unsigned
				text += std::format("{:>6}ms  {}\n", ageMs, nameForIndex(record.functionIndex));
			}

			mLastRendered = cursor;

			const auto colour = ImGui::ColorConvertFloat4ToU32(ImVec4(0.6f, 1.0f, 0.7f, 1.0f));
			RenderTextHelper::drawOutlinedText(text, { 12.f, 260.f }, colour, 15.f);
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderEventCallback;

public:
	HCEScriptTraceImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceScriptTraceToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEScriptTrace only supports Halo Campaign Evolved");

		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		mEvaluateFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceScriptEvaluateFunction), game);
		mFunctionTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceScriptFunctionTable), game);

		// startEnabled = false. This hook fires on EVERY script function evaluation, so it stays unpatched
		// until the user asks for the trace.
		mEvaluateHook = ModuleMidHook::make(mGame.toModuleName(), mEvaluateFunction, &evaluateHookFunction, false);

		mReady.store(true, std::memory_order_release);
	}

	~HCEScriptTraceImpl()
	{
		mReady.store(false, std::memory_order_release);
		gTracing.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderEventCallback.removeCallback();
		mEvaluateHook.reset();
	}
};


HCEScriptTrace::HCEScriptTrace(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEScriptTraceImpl>(game, dicon))
{
}

HCEScriptTrace::~HCEScriptTrace()
{
	PLOG_VERBOSE << "~" << getName();
}
