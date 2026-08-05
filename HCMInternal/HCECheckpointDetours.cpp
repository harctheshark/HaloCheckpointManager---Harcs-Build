#include "pch.h"
#include "HCECheckpointDetours.h"
#include "HCECheckpointBlob.h"
#include "HCEGetPlayerState.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include <string>

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) checkpoint control.
//
// Ported from the external python tool HCM_Evolved (detours.py). That tool VirtualAllocEx'd a 4 KiB RWX code
// cave near the game code and hand assembled four blobs into it. HCM does not need any of that: safetyhook
// already allocates a near trampoline, relocates the overwritten instructions and suspends threads while it
// patches, and a midhook can redirect execution just by writing ctx.rip. So the cave becomes four midhooks.
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
//       cave : forced      -> clear force_flag, set injection_active, run the call, clear injection_active
//              natural     -> if disable_flag, jmp checkpoint_continue (skip the call), else run the call
//       here : checkpointCallHookFunction - identical, using ctx.rip = callContinue (0x1AE0E7) to skip.
//       A forced checkpoint is checked FIRST and so still happens while natural checkpoints are disabled,
//       exactly like the cave.
//
//   cave code4 @ checkpoint_inject 0x19D51A  (call [r10] ; mov rax,[rsi])  -- THE PROVIDER HAND-OFF.
//       cave : on a forced checkpoint only, rep movsb OUT of rdx into a staging buffer (dump) and/or replace
//              rdx with a staging buffer (inject).
//       here : checkpointHandoffHookFunction - the inject half is identical, minus the flag plumbing, because the
//              arming lives in C++ atomics instead of cave bytes. The dump half is NOT identical and deliberately
//              so - see THE PASSIVE SHADOW below.
//
// ================================================================================================================
// THE PASSIVE SHADOW (why Dump Checkpoint no longer forces a checkpoint)
//
// The cave - and HCM's first version of this - could only dump by FORCING a checkpoint, because the blob exists in
// this process for the duration of one call and there is no passive buffer to read. That meant "dump" silently
// moved the player's checkpoint, which is the opposite of what a dump is for.
//
// But 0x19D51A fires on EVERY checkpoint, natural or forced. So while shadowing is armed the hook copies the blob
// into an HCM-owned buffer unconditionally, and Dump writes THAT. Nothing is forced, nothing about the player's
// game changes, and the checkpoint on disk is the checkpoint they are actually on.
//
// ⚠ COST. This is a ~12 MB (0xC10000) copy on the GAME THREAD, once per checkpoint. Measured with a standalone
// harness on the dev machine, both sides cache-cold: 0.404 ms min / 0.444 ms median / 0.815 ms p95 / 0.982 ms max,
// i.e. 26.5 GB/s, i.e. 2.7% of a 60 Hz frame in the median and 5.9% at the worst sample. It lands at the exact
// instant the engine is already handing 12 MB to a storage provider that will hash and persist it, and checkpoints
// are tens of seconds apart. That is not a material hitch, so shadowing DEFAULTS ON. It is still behind
// settings->hceShadowCheckpoints, and when that is off nothing is installed and nothing is copied: gShadowTarget
// is null, the hook body is one acquire load, and armShadowCapture returns without touching the game at all.
//
// ⚠ ALLOCATION HAPPENS ON THE ARMING THREAD, NEVER IN THE HOOK. The first resize of a 12 MB vector measured
// 1.150 ms of page faults on its own; doing that inside a checkpoint would triple the cost of the first one.
//
// ⚠ THE BUFFER IS RETIRED, NEVER FREED. The hook holds a raw ShadowTarget* and can be inside a copy at any
// instant, so growing the shadow allocates a NEW target and leaves the old one alive for the life of the process.
// In practice the state length is fixed for a build and exactly one is ever allocated.
//
// ⚠ THE SHADOW IS INVALIDATED ON EVERY MCC STATE CHANGE. A checkpoint from the previous level would still copy,
// still sign and still write a perfectly valid file - named after the level the player is on NOW. That trap is
// worth more than the convenience, so a level change wipes it and Dump says "nothing yet" until the next
// checkpoint. Same on toggle-off, for the same reason: checkpoints that happened while it was off were not seen.
//
// ================================================================================================================
// WHY THE HAND-OFF EXISTS AT ALL, AND WHAT RDX IS
//
// save_checkpoint has two completely different storage paths, selected on the engine shell (qword_182C40028) and
// [shell + 0x10]:
//
//   LOCAL    (either null)     0x19D559 onwards. Two in-process 0xC10000 ring buffers, a control block, an
//                              asynchronous SHA-1 worker. This is what HCECheckpointBlob's snapshot/ring code
//                              describes, and it is what HCM used to assume.
//   PROVIDER (both non null)   0x19D4EB .. 0x19D554. The blob is handed to an external storage provider and
//                              there is NO in-process checkpoint buffer to read or write. THE SHIPPED GAME
//                              TAKES THIS PATH - it is why Dump and Inject used to refuse outright.
//
// ⚠⚠ ON THE PROVIDER PATH, RDX AT 0x19D51A IS THE LIVE GAME STATE. It is qword_18142AA10, loaded three
// instructions earlier at 0x19D505. That is not a staging copy of anything:
//
//     0x19D640  (local save)            mov rdx,[0x142AA10] ; ... ; memcpy(slotBuffer, rdx, len)   reads it
//     0x19DB4C  (provider revert)       mov rdx,[0x142AA10] ; provider->read(dest = rdx, len, slot) writes it
//     0x19DBD9  (local revert)          mov rcx,[0x142AA10] ; memcpy(rcx, checkpoint, len)          writes it
//
// i.e. it is the single block the whole simulation lives in and that a revert restores INTO. Writing our blob
// into it before the call would overwrite the running game, not stage a checkpoint. So:
//
//     DUMP   reads THROUGH rdx and copies out. Always safe.
//     INJECT changes the rdx REGISTER ONLY, so the provider's writer reads our staging buffer instead. The
//            game's own block is never touched.
//
// That is exactly what the python cave does, and its code4 carries the same warning in as many words.
//
// ★ The SHA-1 comes free. The 5th argument the engine passes at 0x19D4FE is sub_1803274C0, which is
// mov rdx,rcx / xor ecx,ecx / jmp sub_18019E5E0 - that is sub_18019E5E0(0, blob), and a2 = 0 is the branch that
// regenerates the salt and writes a fresh digest. The provider runs it over the bytes it is about to persist, so
// an injected blob is re-signed by the engine itself. HCM still signs its own copy first, which is belt and
// braces rather than a requirement.
//
// ================================================================================================================
// THE ONE SHOT BRACKET (the INJECTION only - the shadow deliberately ignores it)
//
// The cave set injection_active = 1 immediately before `call save_checkpoint` and 0 immediately after, so the
// swap could only ever fire inside a checkpoint the tool forced. A midhook cannot run code after the call, so the
// equivalent here is: checkpointCallHookFunction ASSIGNS gInsideForcedCheckpoint on every arrival (true when this
// checkpoint is forced, false when it is natural), and checkpointHandoffHookFunction CONSUMES it with
// exchange(false). Between those two points the game thread runs straight line code, so the flag is false
// everywhere except inside one forced save.
//
// The bracket gates the INJECTION and nothing else. The shadow copy sits OUTSIDE it on purpose - a passive dump is
// exactly the case the bracket is designed to exclude - and runs after it, reading ctx.rdx fresh, so that if an
// injection did swap the pointer the shadow holds what the provider actually stored.
//
// ⚠ save_checkpoint has THREE callers - 0x19E482, 0x1AE0E2 and 0x21552B - and only 0x1AE0E2 is hooked. The
// consume-on-use above is what makes that safe: after any forced checkpoint the flag is already back to false, so
// a save arriving from one of the other two call sites finds it clear and is left completely alone. The single
// residual is a forced checkpoint that enters save_checkpoint but never reaches the hand-off, which on a provider
// process cannot happen (the branch at 0x19D4E0/0x19D4E9 is decided by the shell, which is fixed at startup) and
// on a local process is harmless because the hand-off hook never fires there at all.
//
// ================================================================================================================
// SAFETY: HaloSimulation_tag_release.dll has no version resource, so every HCE build reports version 0.0.0.0
// and the pointer data's Version attribute cannot detect a game update. We therefore verify the original bytes
// at all four sites against hceCheckpoint*OriginalBytes and refuse to install on a mismatch. Hooking the middle
// of an instruction would be an instant crash, so do not remove that check.
//
// That verification (and the address resolution it needs) happens in ensureHooksAttached, NOT in the constructor.
// The constructor runs from OptionalCheatConstructor::createCheats on a detached thread very early in injection,
// and HaloSimulation_tag_release.dll is not guaranteed to be loaded yet - GetMCCVersion logs
// "HaloSimulation_tag_release.dll not loaded yet; using synthetic version" for exactly this reason. Reading game
// memory there would lose that race and mark the features permanently failed for the whole session. Everything
// the constructor does now is a pure PointerDataStore (XML) lookup plus four detached ModuleMidHook objects,
// which resolve lazily on attach - the same pattern NaturalCheckpointDisable uses for its ModulePatch.
// ================================================================================================================

namespace
{
	// Written by the UI / hotkey thread, read (and for the force flag, consumed) by the game thread.
	std::atomic<bool> gForceRequested{ false };
	std::atomic<bool> gNaturalDisabled{ false };

	// Absolute address of the instruction FOLLOWING each hooked site, i.e. where the cave's "continue" jumps
	// went. Resolved on the UI thread before the hooks can matter; zero means "not resolved, do nothing".
	// The hand-off hook needs no such address - it skips nothing.
	std::atomic<uintptr_t> gGate1Continue{ 0 };
	std::atomic<uintptr_t> gGate2Continue{ 0 };
	std::atomic<uintptr_t> gCallContinue{ 0 };

	// THE BRACKET. Assigned at the save_checkpoint call site, consumed at the provider hand-off. See the long
	// note above - this is what makes the swap fire on exactly one forced checkpoint and nothing else.
	std::atomic<bool> gInsideForcedCheckpoint{ false };

	// Set the first time the hand-off hook ever runs. Positive proof that this process really is on the provider
	// path, as opposed to us merely having read the shell globals and concluded it.
	std::atomic<bool> gHandoffSeen{ false };

	// One shot arming, cleared by the game thread as it acts.
	std::atomic<bool> gInjectArmed{ false };

	// Status codes the injection reports back through. Only ever written by the game thread while the armed flag
	// is true, only ever read by the arming thread after it observes that flag go false.
	constexpr int kStatusPending = 0;
	constexpr int kStatusDone = 1;
	constexpr int kStatusLengthMismatch = 2;

	// How long a timed-out arm waits, after disarming, for a hook body that was already running to finish. See
	// the SETTLE note in forceAndWait - the hook does a handful of atomic ops plus at most one memcpy, so this is
	// orders of magnitude more than it needs.
	constexpr int kDisarmSettleMilliseconds = 100;
	std::atomic<int> gInjectStatus{ kStatusPending };

	// The length the engine actually passed in R8D, recorded so a mismatch can be reported with real numbers.
	std::atomic<uint32_t> gObservedLength{ 0 };

	// Raw view of the injection buffer, snapshotted at arm time. The hook uses ONLY this, never the vector, so it
	// can never observe a half-resized container.
	std::atomic<uintptr_t> gInjectSource{ 0 };
	std::atomic<uint32_t>  gInjectSize{ 0 };

	// THE INJECTION STAGING BUFFER. Namespace scope and deliberately never shrunk or freed while the process lives.
	//
	// ⚠ It MUST outlive the hook call: the provider is handed a pointer to it and nothing tells us when it has
	// finished reading (0x19D51A is a call into a vtable we do not own, and the engine follows it with a separate
	// "commit" call at 0x19D523). Reusing one long lived allocation, rewritten only under gStagingMutex by the next
	// injection - which cannot happen without the user going through a blocking file dialog first - is the same
	// lifetime the python tool gives its VirtualAllocEx'd buffer.
	std::mutex gStagingMutex;              // serialises arm+wait and every shadow republish/read
	std::vector<byte> gInjectStaging;      // guarded by gStagingMutex

	void clearHandoffArming()
	{
		gInjectArmed.store(false, std::memory_order_release);
		gInjectSource.store(0, std::memory_order_release);
		gInjectSize.store(0, std::memory_order_release);
	}


	// ============================================================================================================
	// THE PASSIVE SHADOW. See the long note at the top of the file for the cost measurement and the three ⚠s.
	// ============================================================================================================

	// One immutable published target: pointer and capacity in a single object, so the hook reads BOTH from one
	// atomic load and can never pair a new pointer with an old capacity.
	struct ShadowTarget
	{
		std::vector<byte> storage;   // ⚠ never resized after construction - the game thread holds storage.data()
		byte* data = nullptr;
		uint32_t capacity = 0;

		explicit ShadowTarget(uint32_t length)
			: storage((size_t)length), capacity(length)
		{
			data = storage.data();
		}
	};

	// NULL = shadowing is off. This load is the entire idle cost of the feature.
	std::atomic<ShadowTarget*> gShadowTarget{ nullptr };

	// ⚠ RETIRED, NEVER ERASED. A hand-off copy can be in flight at any instant holding a raw ShadowTarget*, so a
	// target that has ever been published stays alive for the life of the process. Only ever touched by the arming
	// thread, under gStagingMutex. In practice the state length is fixed for a build, so this holds exactly one.
	std::vector<std::unique_ptr<ShadowTarget>> gShadowTargetPool;

	// SEQLOCK. Odd = the game thread is writing the shadow right now, even = it is stable. This is the same shape
	// as the engine's own publication barrier that the LOCAL path gates on (see HCECheckpointBlob.h): the reader
	// copies between two equal even reads and retries if a checkpoint landed underneath it. A torn blob that
	// reached a file would be a checkpoint that fails its SHA-1, and on HaloCER a rejected revert RESTARTS THE
	// LEVEL - so this is not a nicety.
	std::atomic<uint32_t> gShadowSequence{ 0 };
	std::atomic<uint32_t> gShadowLength{ 0 };
	std::atomic<bool>     gShadowValid{ false };   // false = nothing complete in there. The startup state.
	std::atomic<uint32_t> gShadowCaptureCount{ 0 };

	// Non-zero once the engine has handed over MORE bytes than the published buffer holds. Recorded rather than
	// copied short, and surfaced to the user as a hard refusal - a truncated dump is a lost run.
	std::atomic<uint32_t> gShadowOverlongLength{ 0 };

	// "Forget whatever is in there." Cheap, and always safe: it can only ever move the shadow towards "nothing
	// yet", never towards handing out something stale. Called on level change, on toggle-off and on teardown.
	void invalidateShadow()
	{
		gShadowValid.store(false, std::memory_order_release);
		gShadowLength.store(0, std::memory_order_release);
		gShadowOverlongLength.store(0, std::memory_order_release);
	}

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

	// The save_checkpoint call itself. This is where a force request is consumed, where a natural checkpoint gets
	// suppressed, and where the hand-off bracket is opened.
	void checkpointCallHookFunction(SafetyHookContext& ctx)
	{
		// exchange, not load+store: two rapid presses must not turn into two checkpoints from one arrival here.
		const bool forced = gForceRequested.exchange(false, std::memory_order_acq_rel);

		// ASSIGNED, not just set. A natural checkpoint arriving here has to clear a bracket that a previous
		// forced checkpoint somehow failed to consume, otherwise a stale true could let a swap fire on a
		// checkpoint the user never asked for.
		gInsideForcedCheckpoint.store(forced, std::memory_order_release);

		if (forced)
			return; // forced: fall through into the original call, ignoring gNaturalDisabled

		if (!gNaturalDisabled.load(std::memory_order_acquire)) return; // ordinary natural checkpoint, let it run

		const uintptr_t cont = gCallContinue.load(std::memory_order_relaxed);
		if (cont != 0) ctx.rip = cont; // step over the call -> no checkpoint
	}

	// The shadow copy, factored out only so the hook body reads as the two independent things it is. Runs on the
	// GAME THREAD. `blob` is whatever the provider is about to be given - the live state normally, HCM's staging
	// buffer if the injection above swapped it.
	//
	// ⚠ ALLOCATION FREE, LOCK FREE AND LOG FREE. Everything it needs was published by the arming thread.
	void shadowHandoff(uintptr_t blob, uint32_t length)
	{
		ShadowTarget* const target = gShadowTarget.load(std::memory_order_acquire);
		if (target == nullptr) return;              // shadowing off - THE idle path, and the whole cost of "off"
		if (blob == 0 || length == 0) return;

		if (length > target->capacity)
		{
			// Recorded, never copied short: a truncated blob would sign cleanly as a 12 MB file that is missing
			// its tail, and a checkpoint the engine rejects RESTARTS THE LEVEL. The arming thread re-reads the
			// state length and grows the buffer; until then Dump refuses with this number in the message.
			gShadowOverlongLength.store(length, std::memory_order_release);
			return;
		}

		gShadowSequence.fetch_add(1, std::memory_order_acq_rel);   // -> odd: "being written"

		// SEH backed, so a moved/unmapped state block degrades to "no shadow" instead of killing the game.
		const bool copied = HCEGetPlayerState::tryReadRaw(blob, target->data, (size_t)length);

		// A failed copy leaves torn bytes in the buffer, so it invalidates the shadow rather than keeping the
		// previous one - the previous one is gone either way.
		gShadowLength.store(copied ? length : 0, std::memory_order_release);
		gShadowValid.store(copied, std::memory_order_release);
		if (copied) gShadowCaptureCount.fetch_add(1, std::memory_order_relaxed);

		gShadowSequence.fetch_add(1, std::memory_order_acq_rel);   // -> even: "stable"
	}

	// THE PROVIDER HAND-OFF. Runs on the GAME THREAD, inside save_checkpoint, with:
	//     rdx = the live game state block (qword_18142AA10)   <- read only, NEVER written
	//     r8  = its length in bytes (r8d, zero extended by the mov at 0x19D4F7)
	// Deliberately allocation free, lock free and log free: this is a game thread midhook, and the shadow branch
	// already costs one ~12 MB memcpy (the same rep movsb the python cave does, and the same order of magnitude
	// as the memcpy the engine itself performs two instructions later on the local path).
	void checkpointHandoffHookFunction(SafetyHookContext& ctx)
	{
		gHandoffSeen.store(true, std::memory_order_relaxed);

		const uint32_t length = (uint32_t)ctx.r8;
		gObservedLength.store(length, std::memory_order_relaxed);

		// Consume the bracket. The injection below is therefore reachable only inside one checkpoint HCM forced.
		if (gInsideForcedCheckpoint.exchange(false, std::memory_order_acq_rel)
			&& gInjectArmed.load(std::memory_order_acquire))
		{
			const uintptr_t source = gInjectSource.load(std::memory_order_acquire);
			if (source == 0 || length == 0 || length != gInjectSize.load(std::memory_order_acquire))
			{
				gInjectStatus.store(kStatusLengthMismatch, std::memory_order_release);
			}
			else
			{
				// ⚠⚠ THE ARGUMENT, NOT THE BUFFER. ctx.rdx points at the live simulation; assigning the register
				// hands the provider our bytes, memcpy'ing into it would corrupt the running game.
				ctx.rdx = source;
				gInjectStatus.store(kStatusDone, std::memory_order_release);
			}
			gInjectArmed.store(false, std::memory_order_release);
		}

		// OUTSIDE the bracket, and last. Every checkpoint gets shadowed, natural or forced; and reading ctx.rdx
		// here rather than above means that after an injection the shadow holds what the provider was actually
		// given, not the live state it replaced.
		shadowHandoff((uintptr_t)ctx.rdx, length);
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
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// The state length, and nothing else, is needed to size the shadow buffer. Constructing this is pure
	// PointerDataStore lookups - it reads no game memory - so it is safe to hold here. See HCECheckpointBlob.h.
	HCECheckpointBlob mBlob;

	// data. All of these are pure PointerDataStore (XML) lookups - building them touches no game memory.
	std::shared_ptr<MultilevelPointer> mGate1Function;
	std::shared_ptr<MultilevelPointer> mGate2Function;
	std::shared_ptr<MultilevelPointer> mCallFunction;
	std::shared_ptr<MultilevelPointer> mHandoffFunction;

	std::shared_ptr<MultilevelPointer> mGate1Continue;
	std::shared_ptr<MultilevelPointer> mGate2Continue;
	std::shared_ptr<MultilevelPointer> mCallContinue;

	std::shared_ptr<std::vector<byte>> mGate1ExpectedBytes;
	std::shared_ptr<std::vector<byte>> mGate2ExpectedBytes;
	std::shared_ptr<std::vector<byte>> mCallExpectedBytes;
	std::shared_ptr<std::vector<byte>> mHandoffExpectedBytes;

	std::shared_ptr<ModuleMidHook> mGate1Hook;
	std::shared_ptr<ModuleMidHook> mGate2Hook;
	std::shared_ptr<ModuleMidHook> mCallHook;
	std::shared_ptr<ModuleMidHook> mHandoffHook;

	std::mutex mAttachMutex;
	bool mVerified = false;      // guarded by mAttachMutex
	bool mHooksAttached = false; // guarded by mAttachMutex

	// "the shadow is running": hooks installed AND a target published. Kept as its own atomic rather than reading
	// mHooksAttached, which is guarded by mAttachMutex and would be a data race to peek at.
	std::atomic<bool> mShadowArmed{ false };

	// Set true as the very last statement of the constructor and false as the very first of the destructor.
	// The two ScopedCallback members are declared at the BOTTOM of this class so they subscribe after every
	// member above them exists, but the constructor body still runs afterwards - and createCheats builds cheats
	// on detached threads while the game (and the hotkey thread) is already running. This closes that window
	// instead of letting a keypress observe half-built state.
	std::atomic<bool> mReady{ false };

	// Last level we saw, so the shadow is dropped on a LEVEL change rather than on every MCC state change
	// (which also fires for pause, revert and loading). See onMCCStateChanged.
	LevelID mLastSeenLevelID = LevelID::no_map_loaded;

	// Re-resolve the continue addresses. Cheap, and means a module reload can't leave the hooks jumping to a
	// stale address. Always called on the UI/hotkey thread, never from inside a hook.
	void refreshHookTargets()
	{
		auto resolveOrThrow = [](const std::shared_ptr<MultilevelPointer>& mlp, const char* what) -> uintptr_t
			{
				if (!mlp) throw HCMRuntimeException(std::format("No pointer data for HaloCER {}", what));
				uintptr_t address = 0;
				if (!mlp->resolve(&address) || address == 0)
					throw HCMRuntimeException(std::format("Could not resolve HaloCER {}: {}", what, MultilevelPointer::GetLastError()));
				return address;
			};

		const uintptr_t gate1 = resolveOrThrow(mGate1Continue, nameof(hceCheckpointGate1Continue));
		const uintptr_t gate2 = resolveOrThrow(mGate2Continue, nameof(hceCheckpointGate2Continue));
		const uintptr_t call = resolveOrThrow(mCallContinue, nameof(hceCheckpointCallContinue));

		gGate1Continue.store(gate1, std::memory_order_relaxed);
		gGate2Continue.store(gate2, std::memory_order_relaxed);
		gCallContinue.store(call, std::memory_order_relaxed);
	}

	// The hooks are installed lazily, the first time the user actually asks for one of the features, and then
	// left in place (every hook body is a single atomic load when idle). A user who never touches these features
	// never has a byte of game code modified. Detaching is avoided on purpose - clearing the flags is enough, and
	// it means we never destroy a trampoline the game thread might be standing in.
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
			verifyOriginalBytes(nameof(hceCheckpointHandoffFunction), mHandoffFunction, mHandoffExpectedBytes);
			mVerified = true;
			PLOG_INFO << "HaloCER checkpoint sites matched their expected original bytes";
		}

		mGate1Hook->setWantsToBeAttached(true);
		mGate2Hook->setWantsToBeAttached(true);
		mCallHook->setWantsToBeAttached(true);
		mHandoffHook->setWantsToBeAttached(true);

		// All or nothing. Gates bypassed without the call hook to consume the request would leave the force
		// flag permanently set, which would checkpoint on every single evaluation; and the hand-off hook without
		// the call hook has no bracket to consume, so it could never fire.
		if (!mGate1Hook->isHookInstalled() || !mGate2Hook->isHookInstalled()
			|| !mCallHook->isHookInstalled() || !mHandoffHook->isHookInstalled())
		{
			mGate1Hook->setWantsToBeAttached(false);
			mGate2Hook->setWantsToBeAttached(false);
			mCallHook->setWantsToBeAttached(false);
			mHandoffHook->setWantsToBeAttached(false);
			throw HCMRuntimeException("Failed to install the Halo Campaign Evolved checkpoint hooks");
		}

		mHooksAttached = true;
		PLOG_INFO << "HaloCER checkpoint detours installed";
	}


	// ============================================================================================================
	// SHADOW ARMING. Always on the caller's (UI / hotkey / MCC state) thread, never inside a hook.
	//
	// public only because HCEDumpCheckpoint reaches these through the outer class, which - being the ENCLOSING
	// class of this pimpl rather than the nested one - gets no special access to them. Member FUNCTIONS only, so
	// the declaration-order contract the data members above rely on is untouched.
	// ============================================================================================================
public:

	// The user's setting, read live rather than cached: BinarySettings are restored from disk during startup and
	// nothing guarantees that happens before this cheat is constructed, so a cached copy could be stale for the
	// whole session. This is an in-memory bool read, not game memory.
	bool shadowWanted()
	{
		auto settings = settingsWeak.lock();
		if (!settings) return false;
		return settings->hceShadowCheckpoints->GetValue();
	}

	// Idempotent, and after the first success this is one atomic load plus one bool read. Throws
	// HCMRuntimeException with a user-facing message; does NOTHING (and installs nothing) while the setting is off,
	// which is what makes "off" free.
	void armShadowCapture()
	{
		if (!mReady.load(std::memory_order_acquire))
			throw HCMRuntimeException("HCM's Halo Campaign Evolved checkpoint hooks are not ready yet.");

		if (!shadowWanted())
		{
			// Turning it off has to forget what is in the buffer too - checkpoints that happen while it is off are
			// not seen, so anything left behind is stale by definition.
			if (gShadowTarget.exchange(nullptr, std::memory_order_acq_rel) != nullptr)
			{
				invalidateShadow();
				mShadowArmed.store(false, std::memory_order_release);
				PLOG_INFO << "HaloCER checkpoint shadowing disabled";
			}
			return;
		}

		// Already running - unless the hook has reported a hand-off too big for the buffer, in which case fall
		// through and re-read the length so the growth path below can actually fix it.
		if (mShadowArmed.load(std::memory_order_acquire)
			&& gShadowTarget.load(std::memory_order_acquire) != nullptr
			&& gShadowOverlongLength.load(std::memory_order_acquire) == 0)
			return;

		// Sized from the engine's own state length, not from a constant. Throws the user-facing "load a level
		// first" when there is no session yet, which is exactly the right answer at a menu or mid-load.
		const int32_t length = mBlob.readStateLength();
		if (length <= 0)
			throw HCMRuntimeException("Halo Campaign Evolved reported a zero game state length, so there is nothing to shadow.");

		refreshHookTargets();
		ensureHooksAttached();

		{
			// Serialised against readShadowCheckpoint so a republish cannot swap the target out from under a read.
			std::scoped_lock stagingLock(gStagingMutex);

			ShadowTarget* target = gShadowTarget.load(std::memory_order_acquire);
			if (target == nullptr || target->capacity < (uint32_t)length)
			{
				// Reuse a retired target that is already big enough before allocating another 12 MB.
				ShadowTarget* chosen = nullptr;
				for (auto& retired : gShadowTargetPool)
				{
					if (retired->capacity >= (uint32_t)length) { chosen = retired.get(); break; }
				}
				if (chosen == nullptr)
				{
					// ⚠ THE ALLOCATION HAPPENS HERE, on this thread. Measured at 1.15 ms of page faults for 12 MB -
					// acceptable on a hotkey press or a level load, not inside a checkpoint.
					gShadowTargetPool.push_back(std::make_unique<ShadowTarget>((uint32_t)length));
					chosen = gShadowTargetPool.back().get();
				}

				// A different buffer means whatever the reader would have found in the old one is not ours.
				invalidateShadow();
				gShadowTarget.store(chosen, std::memory_order_release);
			}
			else
			{
				// Same buffer, but it has not been shadowing - anything in it predates the gap.
				invalidateShadow();
			}
		}

		mShadowArmed.store(true, std::memory_order_release);
		PLOG_INFO << std::format("HaloCER checkpoint shadowing armed for 0x{:X} bytes", (uint32_t)length);
	}

	// Best effort version for the paths that are not ABOUT shadowing - a force, a level load. A failure there is
	// almost always "no level loaded yet", which is not worth a message box, and every user-facing entry point
	// (Dump) calls armShadowCapture directly and surfaces the real reason.
	void tryArmShadowCapture(const char* context)
	{
		try
		{
			armShadowCapture();
		}
		catch (HCMRuntimeException ex)
		{
			PLOG_DEBUG << "HaloCER checkpoint shadowing not armed (" << context << "): " << ex.what();
		}
	}

private:

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

			// So that "Force Checkpoint, then Dump" - which is what the dump's own "nothing yet" message tells the
			// user to do - works on the FIRST press, including when HCM was injected mid-level and has therefore
			// never seen a state change. Deliberately before the force, so the checkpoint we are about to cause is
			// the one that gets shadowed.
			tryArmShadowCapture("force checkpoint");

			gForceRequested.store(true, std::memory_order_release);
			messagesGUI->addMessage("Checkpoint forced.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// The shadow toggle. Arms or disarms immediately so the user sees the setting take effect now, not at the next
	// level load.
	void onShadowCheckpointsToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire))
		{
			PLOG_DEBUG << "HCECheckpointDetours not ready yet, ignoring hceShadowCheckpoints toggle";
			return;
		}

		PLOG_DEBUG << "HaloCER hceShadowCheckpoints toggle: newval: " << newValue;

		// armShadowCapture re-reads the setting itself, so nothing here depends on the ordering between this
		// callback and the value actually landing in the BinarySetting.
		if (!newValue)
		{
			tryArmShadowCapture("shadow toggle off"); // takes the "not wanted" branch: disarm + forget
			return;
		}

		try
		{
			armShadowCapture();
		}
		catch (HCMRuntimeException ex)
		{
			// The user just asked for this, so they get the real reason ("load a level first", or an unrecognised
			// build). It will arm by itself on the next level load.
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Fires when a new game/level loads. Two jobs, and the first matters more:
	//   1. FORGET the shadow. A checkpoint from the previous level is still a valid, signable 12 MB blob - it would
	//      dump happily, named after the level the player is on now, and only reveal itself as wrong when injecting
	//      it restarts their run.
	//   2. Arm, if the setting wants it and this is the first level of the session.
	void onMCCStateChanged(const MCCState& newState)
	{
		if (!mReady.load(std::memory_order_acquire)) return;

		// ⚠ INVALIDATE ONLY WHEN THE LEVEL ACTUALLY CHANGES, not on every state change. This event also fires
		// for pausing, reverting and loading hitches - i.e. constantly - and wiping the shadow on those made
		// Dump report "no checkpoint has occurred yet" at apparently random moments, including right after a
		// revert, which is exactly when someone reaches for it.
		//
		// The reason invalidation exists is narrower than "something changed": a checkpoint from the PREVIOUS
		// LEVEL is still a valid, signable 12 MB blob, so it would dump happily under the new level's name and
		// only reveal itself when injecting it restarts the run. Only a level change can do that.
		const auto currentLevel = newState.currentLevelID;
		if (currentLevel != mLastSeenLevelID)
		{
			mLastSeenLevelID = currentLevel;
			invalidateShadow();
		}

		if (static_cast<GameState::Value>(newState.currentGameState) != GameState::Value::HaloCER) return;
		tryArmShadowCapture("MCC state change");
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
			PLOG_DEBUG << "HaloCER naturalCheckpointDisable toggle: newval: " << newValue;

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
	// Throws HCMRuntimeException, not HCMInitException: this runs on first use, from inside the callbacks,
	// which surface it through runtimeExceptions->handleMessage. That is an on-screen message the user actually
	// sees, unlike an init failure (which needs the Control heading's "Show optional cheat service failures").
	static void verifyOriginalBytes(const char* siteName, const std::shared_ptr<MultilevelPointer>& site, const std::shared_ptr<std::vector<byte>>& expectedPtr)
	{
		if (!site) throw HCMRuntimeException(std::format("No pointer data for HaloCER checkpoint site {}", siteName));
		if (!expectedPtr || expectedPtr->empty()) throw HCMRuntimeException(std::format("No expected original bytes for HaloCER checkpoint site {}", siteName));

		const std::vector<byte>& expected = *expectedPtr;
		std::vector<byte> actual(expected.size());
		if (!site->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("Could not read HaloCER checkpoint site {}: {}", siteName, MultilevelPointer::GetLastError()));

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"HaloCER checkpoint site {} did not match its expected original bytes - this build of Halo Campaign Evolved is not supported. Expected [{}], found [{}]",
				siteName, bytesToString(expected), bytesToString(actual)));
	}

	// Declared LAST on purpose - see mReady. A ScopedCallback subscribes in its own constructor, so any member
	// it touches has to already exist; declaring these first (as the other cheats do) meant onForceCheckpoint
	// could run against uninitialised weak_ptrs and null hook pointers.
	// The first two deliberately reuse the existing MCC settings/events - HaloCER and the MCC games can never be
	// in the same process, so there is only ever one listener.
	ScopedCallback<ActionEvent> mForceCheckpointCallbackHandle;
	ScopedCallback<ToggleEvent> mNaturalCheckpointDisableCallbackHandle;
	ScopedCallback<ToggleEvent> mShadowCheckpointsCallbackHandle;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallbackHandle;

public:
	HCECheckpointDetoursImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mBlob(game, dicon),
		// initialiser order must match declaration order - these four are the last members of the class
		mForceCheckpointCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->forceCheckpointEvent, [this]() { onForceCheckpoint(); }),
		mNaturalCheckpointDisableCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->naturalCheckpointDisable->valueChangedEvent, [this](bool& n) { onNaturalCheckpointDisableToggle(n); }),
		mShadowCheckpointsCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->hceShadowCheckpoints->valueChangedEvent, [this](bool& n) { onShadowCheckpointsToggle(n); }),
		mMCCStateChangedCallbackHandle(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a
		// GameState directly against a Value is ambiguous (C2666).
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCECheckpointDetours only supports Halo Campaign Evolved");

		// Everything below is a PointerDataStore (XML) lookup or an unattached ModuleMidHook. Deliberately no
		// game memory is read and no address is resolved here - see the SAFETY note at the top of the file.
		// Construction may ONLY ever throw HCMInitException - OptionalCheatConstructor::createCheats runs each
		// cheat's constructor on its own thread and catches nothing else, so anything else would terminate.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		mGate1Function = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate1Function), mGame);
		mGate2Function = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate2Function), mGame);
		mCallFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointCallFunction), mGame);
		mHandoffFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointHandoffFunction), mGame);

		mGate1Continue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate1Continue), mGame);
		mGate2Continue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointGate2Continue), mGame);
		mCallContinue = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCheckpointCallContinue), mGame);

		mGate1ExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointGate1OriginalBytes), mGame);
		mGate2ExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointGate2OriginalBytes), mGame);
		mCallExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointCallOriginalBytes), mGame);
		mHandoffExpectedBytes = ptr->getVectorData<byte>(nameof(hceCheckpointHandoffOriginalBytes), mGame);

		// startEnabled = false. Nothing is patched until the user asks for it - see ensureHooksAttached.
		// ModuleMidHook::make does not resolve anything while startEnabled is false, so this is safe to do
		// before HaloSimulation_tag_release.dll has loaded; ModuleHookManager attaches on module load instead.
		mGate1Hook = ModuleMidHook::make(mGame.toModuleName(), mGate1Function, &gate1HookFunction, false);
		mGate2Hook = ModuleMidHook::make(mGame.toModuleName(), mGate2Function, &gate2HookFunction, false);
		mCallHook = ModuleMidHook::make(mGame.toModuleName(), mCallFunction, &checkpointCallHookFunction, false);
		mHandoffHook = ModuleMidHook::make(mGame.toModuleName(), mHandoffFunction, &checkpointHandoffHookFunction, false);

		// NOTE: there is deliberately no "the toggle might already be on" handling here. naturalCheckpointDisable
		// defaults to false, is not in SettingsStateAndEvents::allSerialisableOptions (so it is never restored
		// from disk), and PresetManager - the only other thing that can write it - is never constructed in a
		// HaloCER process (presetSaveButton/presetLoadButton are ALL_GAMES_AND_MAINMENU, so the
		// {HaloCER, PresetManager} pair never enters requiredServices). If any of those three facts ever change,
		// the value must be applied through onNaturalCheckpointDisableToggle - NOT by calling
		// ensureHooksAttached from this constructor, which is exactly the module-load race described above.
		//
		// hceShadowCheckpoints is the exact opposite case - it DEFAULTS ON and IS restored from disk - and it is
		// still not applied here, for the same reason: arming reads game memory. It is applied from
		// onMCCStateChanged (every level load), onForceCheckpoint and the dump itself, all of which run at a point
		// where HaloSimulation_tag_release.dll is loaded and a session exists. armShadowCapture re-reads the
		// setting each time, so a value that lands from disk after this constructor still takes effect.

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
		mShadowCheckpointsCallbackHandle.removeCallback();
		mMCCStateChangedCallbackHandle.removeCallback();

		// Stop the hook bodies doing anything before the hooks come down with this object.
		gForceRequested.store(false, std::memory_order_release);
		gNaturalDisabled.store(false, std::memory_order_release);
		gInsideForcedCheckpoint.store(false, std::memory_order_release);
		clearHandoffArming();

		// Stop shadowing. The ShadowTargets themselves are namespace scope and deliberately outlive this object -
		// the game thread can be inside a copy right now, and only the hook coming down (which safetyhook does with
		// threads suspended) guarantees it is not.
		gShadowTarget.store(nullptr, std::memory_order_release);
		mShadowArmed.store(false, std::memory_order_release);
		invalidateShadow();

		gGate1Continue.store(0, std::memory_order_relaxed);
		gGate2Continue.store(0, std::memory_order_relaxed);
		gCallContinue.store(0, std::memory_order_relaxed);
	}


	// ============================================================================================================
	// THE PROVIDER HAND-OFF API. Both run on the caller's (hotkey/UI) thread and block IT, never the game thread.
	// ============================================================================================================
private:
	// Arms whatever the caller staged, forces a checkpoint, and waits for the game thread to clear `armed`.
	// Returns the status the game thread reported. Always leaves everything disarmed.
	int forceAndWait(std::atomic<bool>& armed, std::atomic<int>& status, int timeoutMilliseconds, const char* what)
	{
		status.store(kStatusPending, std::memory_order_release);
		gObservedLength.store(0, std::memory_order_relaxed);

		// ARM FIRST, THEN FORCE. The other order lets a checkpoint that is already in flight reach the hand-off
		// before we are armed, which would silently do nothing and burn the user's forced checkpoint.
		armed.store(true, std::memory_order_release);
		gForceRequested.store(true, std::memory_order_release);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
		while (armed.load(std::memory_order_acquire))
		{
			if (std::chrono::steady_clock::now() > deadline)
			{
				// Disarm BOTH the action and the force request. Leaving the force pending would mean the user's
				// next loading screen quietly spends it on a checkpoint they never asked for.
				armed.store(false, std::memory_order_release);
				gForceRequested.store(false, std::memory_order_release);
				gInsideForcedCheckpoint.store(false, std::memory_order_release);

				// ⚠ SETTLE BEFORE DECLARING FAILURE. The game thread can have read `armed` as true microseconds
				// before the store above and still be inside the hook. Disarming stops any LATER arrival, so
				// after a short settle the status is authoritative - and if it says the action completed, it
				// completed. Reporting "nothing was injected" over a swap that actually happened would be the
				// single worst lie this code could tell.
				Sleep(kDisarmSettleMilliseconds);
				if (status.load(std::memory_order_acquire) == kStatusDone)
				{
					PLOG_WARNING << std::format(
						"HaloCER {} completed on the game thread just as HCM's {} ms deadline expired; treating it as a success",
						what, timeoutMilliseconds);
					return kStatusDone;
				}

				PLOG_ERROR << std::format("HaloCER {} timed out after {} ms (handoff seen this session: {})",
					what, timeoutMilliseconds, gHandoffSeen.load(std::memory_order_relaxed));

				throw HCMRuntimeException(std::format(
					"Halo Campaign Evolved did not reach a checkpoint within {} ms, so nothing was {}.\n"
					"{}",
					timeoutMilliseconds, what,
					gHandoffSeen.load(std::memory_order_relaxed)
					? "Try again once you are actually in gameplay - the game will not checkpoint on a loading screen or in a menu."
					: "HCM has never seen this game hand a checkpoint to its storage provider, so this build may keep its checkpoints somewhere else."));
			}
			Sleep(2);
		}

		return status.load(std::memory_order_acquire);
	}

	// Turns a status code into the user-facing failure. Never returns for anything but kStatusDone.
	void throwForStatus(int status, uint32_t expectedLength, const char* what)
	{
		if (status == kStatusDone) return;

		const uint32_t observed = gObservedLength.load(std::memory_order_relaxed);
		if (status == kStatusLengthMismatch)
			throw HCMRuntimeException(std::format(
				"Halo Campaign Evolved handed its storage provider 0x{:X} bytes, but HCM had prepared 0x{:X}.\n"
				"Nothing was {} - the game state size changed between HCM reading it and the checkpoint happening.",
				observed, expectedLength, what));

		throw HCMRuntimeException(std::format("Halo Campaign Evolved's checkpoint hand-off reported an unexpected status ({}).", status));
	}

public:
	// ============================================================================================================
	// THE SHADOW, READ SIDE.
	// ============================================================================================================
	HCECheckpointDetours::ShadowStatus getShadowStatus()
	{
		HCECheckpointDetours::ShadowStatus status{};
		status.wanted = shadowWanted();
		status.armed = mShadowArmed.load(std::memory_order_acquire) && gShadowTarget.load(std::memory_order_acquire) != nullptr;
		status.haveCheckpoint = status.armed
			&& gShadowValid.load(std::memory_order_acquire)
			&& gShadowLength.load(std::memory_order_acquire) != 0;
		status.handoffSeen = gHandoffSeen.load(std::memory_order_relaxed);
		status.length = gShadowLength.load(std::memory_order_acquire);
		status.captureCount = gShadowCaptureCount.load(std::memory_order_relaxed);
		status.overlongLength = gShadowOverlongLength.load(std::memory_order_acquire);
		return status;
	}

	// How many times readShadowCheckpoint will re-copy when a checkpoint lands mid-read. Each attempt is one ~12 MB
	// memcpy (0.44 ms measured), and a checkpoint has to land inside that window to cost an attempt, so needing
	// even two is already vanishingly unlikely.
	static constexpr int kShadowReadAttempts = 8;

	std::vector<byte> readShadowCheckpoint()
	{
		if (!mReady.load(std::memory_order_acquire))
			throw HCMRuntimeException("HCM's Halo Campaign Evolved checkpoint hooks are not ready yet.");

		// Held for the whole read so an arm on another thread cannot republish the target underneath us. The
		// GAME thread never takes it - it only ever touches the already-published target.
		std::scoped_lock stagingLock(gStagingMutex);

		ShadowTarget* const target = gShadowTarget.load(std::memory_order_acquire);
		if (target == nullptr)
			throw HCMRuntimeException("HCM is not currently keeping a copy of Halo Campaign Evolved's checkpoints.");

		for (int attempt = 0; attempt < kShadowReadAttempts; ++attempt)
		{
			const uint32_t before = gShadowSequence.load(std::memory_order_acquire);
			if (before & 1u) { Sleep(2); continue; }   // odd - the game thread is writing it right now

			if (!gShadowValid.load(std::memory_order_acquire))
				throw HCMRuntimeException("HCM has not seen a Halo Campaign Evolved checkpoint yet.");

			const uint32_t length = gShadowLength.load(std::memory_order_acquire);
			if (length == 0 || length > target->capacity)
				throw HCMRuntimeException("HCM's copy of the last Halo Campaign Evolved checkpoint is not usable.");

			std::vector<byte> out((size_t)length);
			std::memcpy(out.data(), target->data, (size_t)length);

			// Nothing moved while we copied, so `out` is one whole checkpoint rather than two halves of two.
			if (gShadowSequence.load(std::memory_order_acquire) == before) return out;
		}

		throw HCMRuntimeException(
			"Halo Campaign Evolved kept checkpointing while HCM was copying the last one out.\n"
			"Nothing was dumped - try again in a moment.");
	}

	void injectCheckpointAtHandoff(const std::vector<byte>& blob, int timeoutMilliseconds)
	{
		if (!mReady.load(std::memory_order_acquire))
			throw HCMRuntimeException("HCM's Halo Campaign Evolved checkpoint hooks are not ready yet.");
		if (blob.empty())
			throw HCMRuntimeException("There is nothing to inject.");
		if (blob.size() > (size_t)UINT32_MAX)
			throw HCMRuntimeException("That checkpoint is far too large to inject.");

		refreshHookTargets();
		ensureHooksAttached();

		std::scoped_lock stagingLock(gStagingMutex);

		// Grow only, then overwrite. See the ⚠ on gInjectStaging: this is the one place the previous injection's
		// buffer is reused, and it is safe because getting here needs a whole blocking file dialog first.
		if (gInjectStaging.size() < blob.size()) gInjectStaging.resize(blob.size());
		std::memcpy(gInjectStaging.data(), blob.data(), blob.size());

		gInjectSource.store((uintptr_t)gInjectStaging.data(), std::memory_order_release);
		gInjectSize.store((uint32_t)blob.size(), std::memory_order_release);

		const int status = forceAndWait(gInjectArmed, gInjectStatus, timeoutMilliseconds, "injected");
		throwForStatus(status, (uint32_t)blob.size(), "injected");
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

HCECheckpointDetours::ShadowStatus HCECheckpointDetours::getShadowStatus()
{
	return pimpl->getShadowStatus();
}

void HCECheckpointDetours::armShadowCapture()
{
	pimpl->armShadowCapture();
}

std::vector<byte> HCECheckpointDetours::readShadowCheckpoint()
{
	return pimpl->readShadowCheckpoint();
}

void HCECheckpointDetours::injectCheckpointAtHandoff(const std::vector<byte>& blob, int timeoutMilliseconds)
{
	pimpl->injectCheckpointAtHandoff(blob, timeoutMilliseconds);
}
