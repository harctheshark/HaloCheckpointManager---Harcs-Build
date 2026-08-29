#include "pch.h"
#include "HCEConsole.h"
#include "HCEGameThreadPump.h"
#include "HCEGetPlayerState.h"
#include "HCESignatureScan.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include <shared_mutex>

// See HCEConsole.h for the entry point, the preprocessor's wrapping rules and the thread requirement.

namespace
{
	// hs_console_execute. 28 bytes, no wildcards, exactly one match in the module - verified against the
	// shipped image rather than taken on trust, because HaloCER reports version 0.0.0.0 for every build it has
	// ever shipped and a fixed RVA would silently become a call into the middle of something else.
	constexpr const char* kConsoleExecuteSignature =
		"48 89 5C 24 10 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 F9 FF FF";

	using t_hs_console_execute = void(__fastcall*)(void*, const char*, const char*, bool);

	std::atomic<uintptr_t> gExecute{ 0 };
	std::atomic<bool> gArmed{ false };

	// One slot, not a deque. The console is driven by a human at typing speed, the drain runs every simulation
	// frame, and the error sink the engine writes into is process-global and unsynchronised - so there is
	// nothing to gain from having more than one command in flight and something to lose.
	std::mutex gQueueMutex;
	std::string gPending;
	bool gHavePending = false;

	// The autocomplete corpus. Built once while arming and not touched again until disarm, so readers need
	// only be kept out during those two moments.
	std::shared_mutex gCorpusMutex;
	std::vector<HCEScriptRegistry::Entry> gCorpus;
	HCEScriptRegistry::Census gCensus{};
	std::function<bool(std::string&)> gPlayGate;   // set while armed; asks the owner whether a game is up

	// The SEH bracket lives in its own function on purpose: __try cannot appear in a function that has any
	// object requiring unwinding in scope (C2712), and the caller below holds a std::string.
	void invokeGuarded(uintptr_t fn, const char* command) noexcept
	{
		__try
		{
			((t_hs_console_execute)fn)(nullptr, "HCM", command, false);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// The parser validates arity and types before evaluating, so a bad command is a compile error
			// rather than a fault - but this is the game's own thread and a surprise here would take the
			// process down. Swallow it; the user sees the command simply not work.
		}
	}

	// SIMULATION THREAD. noexcept and cheap: sub_1801AF290 carries an unwind-only handler and the trampoline
	// has no .pdata entry, so an exception escaping here is an unwalkable stack rather than a catchable error.
	void consolePump(uintptr_t /*simTlsBase*/) noexcept
	{
		if (!gArmed.load(std::memory_order_acquire)) return;

		const uintptr_t fn = gExecute.load(std::memory_order_acquire);
		if (!fn) return;

		std::string command;
		{
			// try_lock, never lock: this is the game's critical path, and a command that misses this frame is
			// simply executed on the next one.
			std::unique_lock<std::mutex> lock(gQueueMutex, std::try_to_lock);
			if (!lock.owns_lock() || !gHavePending) return;
			command = std::move(gPending);
			gPending.clear();
			gHavePending = false;
		}

		if (command.empty()) return;
		invokeGuarded(fn, command.c_str());
	}
}

class HCEConsole::Impl
{
public:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::shared_ptr<HCEGetPlayerState> mPlayerState;
	std::shared_ptr<HCEGameThreadPumpHost> mPump;

	std::vector<HCEScriptRegistry::Entry> mEntries;
	HCEScriptRegistry::Census mCensus{};
	std::atomic<bool> mUsable{ false };

	Impl(GameState game, IDIContainer& dicon)
		: mGame(game), mccStateHookWeak(dicon.Resolve<IMCCStateHook>())
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEConsole only supports Halo Campaign Evolved");

		mPlayerState = resolveDependentCheat(HCEGetPlayerState);

		// A shared_ptr, not weak: the pump owns the midhook our callback is registered in, and it must outlive
		// us. Same reasoning as HCESkyFix's hold on HCEGetCameraData.
		mPump = resolveDependentCheat(HCEGameThreadPumpHost);

		uintptr_t simBase = 0;
		try { simBase = mPlayerState->getSimModuleBase(); }
		catch (HCMRuntimeException& ex)
		{
			throw HCMInitException(std::format("Console: the simulation module is not loaded ({})", ex.what()));
		}

		int hits = 0;
		const uintptr_t execute = HCESignatureScan::resolveUnique(simBase, kConsoleExecuteSignature, hits);
		if (!execute)
			throw HCMInitException(std::format(
				"The console is unavailable on this build of Halo Campaign Evolved: the script entry point's "
				"signature matched {} times, expected exactly 1.", hits));

		gExecute.store(execute, std::memory_order_release);
		PLOG_INFO << "HCEConsole: hs_console_execute at 0x" << std::hex << execute << std::dec
			<< " (sim+0x" << std::hex << (execute - simBase) << std::dec << ")";

		mEntries = HCEScriptRegistry::build(simBase, mCensus);
		{
			std::unique_lock lock(gCorpusMutex);
			gCorpus = mEntries;
			gCensus = mCensus;
		}
		{
			auto weakHook = mccStateHookWeak;
			GameState g = mGame;
			gPlayGate = [weakHook, g](std::string& why) -> bool
				{
					auto hook = weakHook.lock();
					if (!hook || !hook->isGameCurrentlyPlaying(g))
					{
						why = "Load a level first - script commands need a running game.";
						return false;
					}
					return true;
				};
		}
		PLOG_INFO << "HCEConsole: " << mCensus.functionsLive << " live functions of " << mCensus.functionsTotal
			<< ", " << mCensus.globalsLive << " backed globals of " << mCensus.globalsTotal
			<< " (" << mEntries.size() << " names for autocomplete)";

		if (!HCEGameThreadPump::add(&consolePump))
			throw HCMInitException("Console: the simulation-thread pump table is full");

		gArmed.store(true, std::memory_order_release);
		mUsable.store(true, std::memory_order_release);
	}

	~Impl()
	{
		// Disarm, then unregister, then let the pump host go. Order matters: the callback must be out of the
		// table before this object dies, or the midhook calls into freed memory.
		gArmed.store(false, std::memory_order_release);
		HCEGameThreadPump::remove(&consolePump);
		mUsable.store(false, std::memory_order_release);
		gExecute.store(0, std::memory_order_release);
		{
			std::unique_lock lock(gCorpusMutex);
			gCorpus.clear();
			gCensus = HCEScriptRegistry::Census{};
			gPlayGate = nullptr;
		}
	}

	bool queue(const std::string& command, std::string& why)
	{
		if (!mUsable.load(std::memory_order_acquire)) { why = "The console is not available on this build."; return false; }
		if (command.empty()) { why = "Nothing to run."; return false; }

		// The preprocessor's buffer is 0x1000 and hs_compile rejects anything at or over it outright. Refuse
		// here with an explanation rather than letting it fail silently in the engine.
		if (command.size() >= 0x0F00) { why = "That command is too long for the game's script buffer."; return false; }

		auto mccStateHook = mccStateHookWeak.lock();
		if (!mccStateHook || !mccStateHook->isGameCurrentlyPlaying(mGame))
		{
			// There is no shipped no-game guard - the string that looks like one is a dead stub - so this
			// check is ours and it is the only thing standing between a menu and hs_evaluate starting and
			// stopping the script runtime under itself.
			why = "Load a level first - script commands need a running game.";
			return false;
		}

		std::scoped_lock lock(gQueueMutex);
		gPending = command;
		gHavePending = true;
		return true;
	}
};


namespace HCEConsoleBridge
{
	bool isUsable() { return gArmed.load(std::memory_order_acquire) && gExecute.load(std::memory_order_acquire) != 0; }

	bool queue(const std::string& command, std::string& why)
	{
		if (!isUsable()) { why = "The console is not available on this build."; return false; }
		if (command.empty()) { why = "Nothing to run."; return false; }
		// The preprocessor's buffer is 0x1000 and hs_compile rejects at or over it outright - refuse here
		// with an explanation rather than letting it fail silently inside the engine.
		if (command.size() >= 0x0F00) { why = "That command is too long for the game's script buffer."; return false; }

		// There is no shipped no-game guard (the string that looks like one is a dead stub), so this check is
		// ours and it is all that stands between a menu and hs_evaluate starting and stopping the script
		// runtime under itself.
		if (gPlayGate && !gPlayGate(why)) return false;

		std::scoped_lock lock(gQueueMutex);
		gPending = command;
		gHavePending = true;
		return true;
	}

	std::vector<const HCEScriptRegistry::Entry*> complete(std::string_view prefix, size_t limit)
	{
		std::shared_lock lock(gCorpusMutex);
		return HCEScriptRegistry::complete(gCorpus, prefix, limit);
	}

	HCEScriptRegistry::Census census()
	{
		std::shared_lock lock(gCorpusMutex);
		return gCensus;
	}
}

HCEConsole::HCEConsole(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
HCEConsole::~HCEConsole() { PLOG_VERBOSE << "~" << getName(); }

bool HCEConsole::queueCommand(const std::string& command, std::string& outWhy) { return pimpl->queue(command, outWhy); }
const std::vector<HCEScriptRegistry::Entry>& HCEConsole::entries() const { return pimpl->mEntries; }
HCEScriptRegistry::Census HCEConsole::census() const { return pimpl->mCensus; }
bool HCEConsole::isUsable() const { return pimpl->mUsable.load(std::memory_order_acquire); }
