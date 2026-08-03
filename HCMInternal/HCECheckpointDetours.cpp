#include "pch.h"
#include "HCECheckpointDetours.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

// ================================================================================================================
// Halo Campaign Evolved (HaloCE) checkpoint control.
//
// Ported from the external python tool HCM_Evolved (detours.py). That tool VirtualAllocEx'd a 4 KiB RWX code
// cave near the game code and hand assembled four blobs into it. HCM does not need any of that: safetyhook
// already allocates a near trampoline, relocates the overwritten instructions and suspends threads while it
// patches, and a midhook can redirect execution just by writing ctx.rip. So the cave becomes three midhooks.
//
// Equivalence with the python cave (all RVAs into HaloSimulation_tag_release.dll):
//
//   cave code1 @ gate1 0x1ADA2A  (test al,al ; je 0x1AE475)
//       cave : if (force_flag) jmp gate1_continue; else run the original test/je
//       here : gate1HookFunction - if forcing, ctx.rip = gate1Continue (0x1ADA32), skipping both instructions.
//
//   cave code2 @ gate2 0x1AE0A2  (cmp byte [0x135706D],0 ; je 0x1AE178)
//       cave : if (force_flag) jmp gate2_continue; else run the original cmp/je
//       here : gate2HookFunction - if forcing, ctx.rip = gate2Continue (0x1AE0AF).
//       Note this deliberately does NOT write to the game's 0x135706D global; it steps over the test instead,
//       so no game state is modified.
//
//   cave code3 @ checkpoint_call 0x1AE0E2  (call save_checkpoint 0x19D400)
//       cave : forced      -> clear force_flag, run the call
//              natural     -> if disable_flag, jmp checkpoint_continue (skip the call), else run the call
//       here : checkpointCallHookFunction - identical, using ctx.rip = callContinue (0x1AE0E7) to skip.
//       A forced checkpoint is checked FIRST and so still happens while natural checkpoints are disabled,
//       exactly like the cave.
//
//   cave code4 @ checkpoint_inject 0x19D51A - checkpoint dump/injection. NOT ported (see the header of this
//       file's feature list in the repo notes); nothing here touches that site.
//
// The force request is consumed by the game thread at the call site, so "the flag went 1 -> 0" means the
// checkpoint actually happened. If the game never reaches the call the request simply stays pending, which is
// the same fire-and-forget behaviour the python tool has.
//
// SAFETY: HaloSimulation_tag_release.dll has no version resource, so every HCE build reports version 0.0.0.0
// and the pointer data's Version attribute cannot detect a game update. We therefore verify the original bytes
// at all three sites against hceCheckpoint*OriginalBytes and refuse to install on a mismatch. Hooking the middle
// of an instruction would be an instant crash, so do not remove that check.
//
// That verification (and the address resolution it needs) happens in ensureHooksAttached, NOT in the constructor.
// The constructor runs from OptionalCheatConstructor::createCheats on a detached thread very early in injection,
// and HaloSimulation_tag_release.dll is not guaranteed to be loaded yet - GetMCCVersion logs
// "HaloSimulation_tag_release.dll not loaded yet; using synthetic version" for exactly this reason. Reading game
// memory there would lose that race and mark BOTH features permanently failed for the whole session. Everything
// the constructor does now is a pure PointerDataStore (XML) lookup plus three detached ModuleMidHook objects,
// which resolve lazily on attach - the same pattern NaturalCheckpointDisable uses for its ModulePatch.
// ================================================================================================================

namespace
{
	// Written by the UI / hotkey thread, read (and for the force flag, consumed) by the game thread.
	std::atomic<bool> gForceRequested{ false };
	std::atomic<bool> gNaturalDisabled{ false };

	// Absolute address of the instruction FOLLOWING each hooked site, i.e. where the cave's "continue" jumps
	// went. Resolved on the UI thread before the hooks can matter; zero means "not resolved, do nothing".
	std::atomic<uintptr_t> gGate1Continue{ 0 };
	std::atomic<uintptr_t> gGate2Continue{ 0 };
	std::atomic<uintptr_t> gCallContinue{ 0 };

	// Predicate #1 - "may a checkpoint happen right now?". Skip it while a force is pending.
	void gate1HookFunction(SafetyHookContext& ctx)
	{
		if (!gForceRequested.load(std::memory_order_acquire)) return;
		const uintptr_t cont = gGate1Continue.load(std::memory_order_relaxed);
		if (cont != 0) ctx.rip = cont;
	}

	// Predicate #2 - a global byte flag. Skip it while a force is pending.
	void gate2HookFunction(SafetyHookContext& ctx)
	{
		if (!gForceRequested.load(std::memory_order_acquire)) return;
		const uintptr_t cont = gGate2Continue.load(std::memory_order_relaxed);
		if (cont != 0) ctx.rip = cont;
	}

	// The save_checkpoint call itself. This is where a force request is consumed, and where a natural
	// checkpoint gets suppressed.
	void checkpointCallHookFunction(SafetyHookContext& ctx)
	{
		// exchange, not load+store: two rapid presses must not turn into two checkpoints from one arrival here.
		if (gForceRequested.exchange(false, std::memory_order_acq_rel))
			return; // forced: fall through into the original call, ignoring gNaturalDisabled

		if (!gNaturalDisabled.load(std::memory_order_acquire)) return; // ordinary natural checkpoint, let it run

		const uintptr_t cont = gCallContinue.load(std::memory_order_relaxed);
		if (cont != 0) ctx.rip = cont; // step over the call -> no checkpoint
	}

	std::string bytesToString(const std::vector<byte>& bytes)
	{
		std::string out;
		for (auto b : bytes)
		{
			if (!out.empty()) out += ' ';
			out += std::format("{:02X}", (uint32_t)(unsigned char)b);
		}
		return out;
	}
}


class HCECheckpointDetours::HCECheckpointDetoursImpl
{
private:
	GameState mGame;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// data. All of these are pure PointerDataStore (XML) lookups - building them touches no game memory.
	std::shared_ptr<MultilevelPointer> mGate1Function;
	std::shared_ptr<MultilevelPointer> mGate2Function;
	std::shared_ptr<MultilevelPointer> mCallFunction;

	std::shared_ptr<MultilevelPointer> mGate1Continue;
	std::shared_ptr<MultilevelPointer> mGate2Continue;
	std::shared_ptr<MultilevelPointer> mCallContinue;

	std::shared_ptr<std::vector<byte>> mGate1ExpectedBytes;
	std::shared_ptr<std::vector<byte>> mGate2ExpectedBytes;
	std::shared_ptr<std::vector<byte>> mCallExpectedBytes;

	std::shared_ptr<ModuleMidHook> mGate1Hook;
	std::shared_ptr<ModuleMidHook> mGate2Hook;
	std::shared_ptr<ModuleMidHook> mCallHook;

	std::mutex mAttachMutex;
	bool mVerified = false;      // guarded by mAttachMutex
	bool mHooksAttached = false; // guarded by mAttachMutex

	// Set true as the very last statement of the constructor and false as the very first of the destructor.
	// The two ScopedCallback members are declared at the BOTTOM of this class so they subscribe after every
	// member above them exists, but the constructor body still runs afterwards - and createCheats builds cheats
	// on detached threads while the game (and the hotkey thread) is already running. This closes that window
	// instead of letting a keypress observe half-built state.
	std::atomic<bool> mReady{ false };

	// Re-resolve the continue addresses. Cheap, and means a module reload can't leave the hooks jumping to a
	// stale address. Always called on the UI/hotkey thread, never from inside a hook.
	void refreshHookTargets()
	{
		auto resolveOrThrow = [](const std::shared_ptr<MultilevelPointer>& mlp, const char* what) -> uintptr_t
			{
				if (!mlp) throw HCMRuntimeException(std::format("No pointer data for HaloCE {}", what));
				uintptr_t address = 0;
				if (!mlp->resolve(&address) || address == 0)
					throw HCMRuntimeException(std::format("Could not resolve HaloCE {}: {}", what, MultilevelPointer::GetLastError()));
				return address;
			};

		const uintptr_t gate1 = resolveOrThrow(mGate1Continue, nameof(hceCheckpointGate1Continue));
		const uintptr_t gate2 = resolveOrThrow(mGate2Continue, nameof(hceCheckpointGate2Continue));
		const uintptr_t call = resolveOrThrow(mCallContinue, nameof(hceCheckpointCallContinue));

		gGate1Continue.store(gate1, std::memory_order_relaxed);
		gGate2Continue.store(gate2, std::memory_order_relaxed);
		gCallContinue.store(call, std::memory_order_relaxed);
	}

	// The hooks are installed lazily, the first time the user actually asks for one of the two features, and
	// then left in place (both hook bodies are a single atomic load when idle). A user who never touches these
	// features never has a byte of game code modified. Detaching is avoided on purpose - flipping the flag off
	// is enough, and it means we never destroy a trampoline the game thread might be standing in.
	//
	// This is also where the build check lives (see the SAFETY note at the top of the file). It has to run
	// BEFORE anything is patched, since after attaching, the sites read back as our jmp rather than the
	// original instructions - hence the sticky mVerified flag rather than a per-call check.
	void ensureHooksAttached()
	{
		std::scoped_lock lock(mAttachMutex);
		if (mHooksAttached) return;

		if (!mVerified)
		{
			verifyOriginalBytes(nameof(hceCheckpointGate1Function), mGate1Function, mGate1ExpectedBytes);
			verifyOriginalBytes(nameof(hceCheckpointGate2Function), mGate2Function, mGate2ExpectedBytes);
			verifyOriginalBytes(nameof(hceCheckpointCallFunction), mCallFunction, mCallExpectedBytes);
			mVerified = true;
			PLOG_INFO << "HaloCE checkpoint sites matched their expected original bytes";
		}

		mGate1Hook->setWantsToBeAttached(true);
		mGate2Hook->setWantsToBeAttached(true);
		mCallHook->setWantsToBeAttached(true);

		// All or nothing. Gates bypassed without the call hook to consume the request would leave the force
		// flag permanently set, which would checkpoint on every single evaluation.
		if (!mGate1Hook->isHookInstalled() || !mGate2Hook->isHookInstalled() || !mCallHook->isHookInstalled())
		{
			mGate1Hook->setWantsToBeAttached(false);
			mGate2Hook->setWantsToBeAttached(false);
			mCallHook->setWantsToBeAttached(false);
			throw HCMRuntimeException("Failed to install the Halo Campaign Evolved checkpoint hooks");
		}

		mHooksAttached = true;
		PLOG_INFO << "HaloCE checkpoint detours installed";
	}

	// primary event callback: Force Checkpoint
	void onForceCheckpoint()
	{
		if (!mReady.load(std::memory_order_acquire))
		{
			PLOG_DEBUG << "HCECheckpointDetours not ready yet, ignoring force checkpoint";
			return;
		}

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;
			PLOG_DEBUG << "Force checkpoint called for game: " << mGame.toString();

			refreshHookTargets();
			ensureHooksAttached();

			gForceRequested.store(true, std::memory_order_release);
			messagesGUI->addMessage("Checkpoint forced.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// primary event callback: Disable Natural Checkpoints
	void onNaturalCheckpointDisableToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire))
		{
			PLOG_DEBUG << "HCECheckpointDetours not ready yet, ignoring naturalCheckpointDisable toggle";
			return;
		}

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			PLOG_DEBUG << "HaloCE naturalCheckpointDisable toggle: newval: " << newValue;

			if (newValue)
			{
				refreshHookTargets();
				ensureHooksAttached();
			}

			gNaturalDisabled.store(newValue, std::memory_order_release);

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				messagesGUI->addMessage(newValue ? "Natural Checkpoints disabled." : "Natural Checkpoints re-enabled.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Refuse to hook a build we don't recognise. See the SAFETY note at the top of this file.
	// Throws HCMRuntimeException, not HCMInitException: this runs on first use, from inside the two callbacks,
	// which surface it through runtimeExceptions->handleMessage. That is an on-screen message the user actually
	// sees, unlike an init failure (which needs the Control heading's "Show optional cheat service failures").
	static void verifyOriginalBytes(const char* siteName, const std::shared_ptr<MultilevelPointer>& site, const std::shared_ptr<std::vector<byte>>& expectedPtr)
	{
		if (!site) throw HCMRuntimeException(std::format("No pointer data for HaloCE checkpoint site {}", siteName));
		if (!expectedPtr || expectedPtr->empty()) throw HCMRuntimeException(std::format("No expected original bytes for HaloCE checkpoint site {}", siteName));

		const std::vector<byte>& expected = *expectedPtr;
		std::vector<byte> actual(expected.size());
		if (!site->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("Could not read HaloCE checkpoint site {}: {}", siteName, MultilevelPointer::GetLastError()));

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"HaloCE checkpoint site {} did not match its expected original bytes - this build of Halo Campaign Evolved is not supported. Expected [{}], found [{}]",
				siteName, bytesToString(expected), bytesToString(actual)));
	}

	// Declared LAST on purpose - see mReady. A ScopedCallback subscribes in its own constructor, so any member
	// it touches has to already exist; declaring these first (as the other cheats do) meant onForceCheckpoint
	// could run against uninitialised weak_ptrs and null hook pointers.
	// Both features deliberately reuse the existing MCC settings/events - HaloCE and the MCC games can never be
	// in the same process, so there is only ever one listener.
	ScopedCallback<ActionEvent> mForceCheckpointCallbackHandle;
	ScopedCallback<ToggleEvent> mNaturalCheckpointDisableCallbackHandle;

public:
	HCECheckpointDetoursImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		// initialiser order must match declaration order - these two are the last members of the class
		mForceCheckpointCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->forceCheckpointEvent, [this]() { onForceCheckpoint(); }),
		mNaturalCheckpointDisableCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->naturalCheckpointDisable->valueChangedEvent, [this](bool& n) { onNaturalCheckpointDisableToggle(n); })
	{
		// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a
		// GameState directly against a Value is ambiguous (C2666).
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCE)
			throw HCMInitException("HCECheckpointDetours only supports Halo Campaign Evolved");

		// Everything below is a PointerDataStore (XML) lookup or an unattached ModuleMidHook. Deliberately no
		// game memory is read and no address is resolved here - see the SAFETY note at the top of the file.
		// Construction may ONLY ever throw HCMInitException - OptionalCheatConstructor::createCheats runs each
		// cheat's constructor on its own thread and catches nothing else, so anything else would terminate.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		mGate1Function = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate1Function), mGame);
		mGate2Function = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate2Function), mGame);
		mCallFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointCallFunction), mGame);

		mGate1Continue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate1Continue), mGame);
		mGate2Continue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate2Continue), mGame);
		mCallContinue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointCallContinue), mGame);

		mGate1ExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointGate1OriginalBytes), mGame);
		mGate2ExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointGate2OriginalBytes), mGame);
		mCallExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointCallOriginalBytes), mGame);

		// startEnabled = false. Nothing is patched until the user asks for it - see ensureHooksAttached.
		// ModuleMidHook::make does not resolve anything while startEnabled is false, so this is safe to do
		// before HaloSimulation_tag_release.dll has loaded; ModuleHookManager attaches on module load instead.
		mGate1Hook = ModuleMidHook::make(mGame.toModuleName(), mGate1Function, &gate1HookFunction, false);
		mGate2Hook = ModuleMidHook::make(mGame.toModuleName(), mGate2Function, &gate2HookFunction, false);
		mCallHook = ModuleMidHook::make(mGame.toModuleName(), mCallFunction, &checkpointCallHookFunction, false);

		// NOTE: there is deliberately no "the toggle might already be on" handling here. naturalCheckpointDisable
		// defaults to false, is not in SettingsStateAndEvents::allSerialisableOptions (so it is never restored
		// from disk), and PresetManager - the only other thing that can write it - is never constructed in a
		// HaloCE process (presetSaveButton/presetLoadButton are ALL_GAMES_AND_MAINMENU, so the
		// {HaloCE, PresetManager} pair never enters requiredServices). If any of those three facts ever change,
		// the value must be applied through onNaturalCheckpointDisableToggle - NOT by calling
		// ensureHooksAttached from this constructor, which is exactly the module-load race described above.

		// Last statement: the callbacks above are already subscribed, this is what lets them actually run.
		mReady.store(true, std::memory_order_release);
	}

	~HCECheckpointDetoursImpl()
	{
		// Stop the callbacks doing anything, then unsubscribe them. The ScopedCallback members are declared
		// last so they are destroyed first, but the destructor body runs before ANY member destructor, so
		// removing them by hand here is still what actually closes the re-entrancy window.
		mReady.store(false, std::memory_order_release);
		mForceCheckpointCallbackHandle.removeCallback();
		mNaturalCheckpointDisableCallbackHandle.removeCallback();

		// Stop the hook bodies doing anything before the hooks come down with this object.
		gForceRequested.store(false, std::memory_order_release);
		gNaturalDisabled.store(false, std::memory_order_release);
		gGate1Continue.store(0, std::memory_order_relaxed);
		gGate2Continue.store(0, std::memory_order_relaxed);
		gCallContinue.store(0, std::memory_order_relaxed);
	}
};


HCECheckpointDetours::HCECheckpointDetours(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<HCECheckpointDetoursImpl>(gameImpl, dicon))
{
}

HCECheckpointDetours::~HCECheckpointDetours()
{
	PLOG_VERBOSE << "~" << getName();
}
