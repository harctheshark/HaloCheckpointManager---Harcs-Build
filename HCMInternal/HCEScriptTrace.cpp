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
#include <algorithm>
#include <string>
#include <unordered_map>

// See HCEScriptTrace.h for why this exists and what it deliberately does not do.

namespace
{
	// hs function definition layout. Only two fields are read here; the rest are documented in the header
	// and in the InternalPointerData comment on hceScriptFunctionTable.
	constexpr int64_t kDefNameOffset = 0x08;   // char*
	constexpr int64_t kDefParseOffset = 0x18;   // COMPILE-TIME parse. Never called at runtime - do not hook.
	constexpr int64_t kDefImplOffset = 0x20;   // the runtime evaluate. THIS is the one to hook.
	constexpr int64_t kDefArgCountOffset = 0x38;   // word

	// The table is 1188 entries in the shipped build. Read a bounded number rather than trusting a count we
	// have not verified: a bad index would otherwise walk arbitrary memory.
	constexpr int kMaxFunctionIndex = 2048;

	// Where each implementation is hooked, and why it is not the first instruction.
	//
	// Every implementation starts with the same prologue and then calls ONE shared argument evaluator
	// (rva 0x1FEC90). On return RAX POINTS AT THE EVALUATED ARGUMENTS, four bytes each - ai_place reads
	// dword[rax] as its squad reference and word[rax+4] as the count. Hooking the first instruction gives
	// the function index in CX but the arguments do not exist yet, which is the difference between "a
	// spawner ran" and "a spawner ran on THIS squad". So the hook goes immediately AFTER that call.
	//
	// The cost of moving past the prologue is that CX and EDX are clobbered by then: the function index is
	// gone, and the node index survives only because every implementation stashes it with `mov ebx, edx`
	// first. The index therefore comes from the hook SLOT rather than from a register - see kThunks.
	//
	// The call is located by the four bytes that precede it in every argument-taking implementation,
	// `movzx edx, word ptr [rdx+0x38]` (0F B7 52 38) followed by E8. Verified: present at +0x21..+0x2A in
	// every implementation, all reaching the same evaluator, with no stray 0xE8 byte earlier in the
	// prologue that a naive scan could trip over. Agreement between implementations on the evaluator's
	// address is checked at runtime, so a build whose prologue differs refuses rather than hooking rubbish.
	constexpr uint8_t kArgEvaluatorAnchor[]{ 0x0F, 0xB7, 0x52, 0x38, 0xE8 };
	constexpr size_t kPrologueSearchLength = 0x40;

	// CAPTURING A RETURN VALUE, NOT JUST ARGUMENTS.
	//
	// Knowing that a spawner called ai_living_count does not say what the count WAS, and the count is the
	// whole question: a top-up loop that keeps placing is telling us its input reads low. The count functions
	// pass the reference to a worker and then store the 16-bit result:
	//
	//     call <worker>                       ; ai_living_count -> 0xFB0C0
	//     mov  word ptr [rsp+0x48], ax        ; 66 89 44 24 48  <- AX IS THE COUNT
	//
	// So the hook goes ON that store, where AX is still live. Verified: ai_living_count 0x1C5B67 and
	// ai_nonswarm_count 0x1C5F2A, each with exactly ONE call between the argument hook and the store, which is
	// what proves AX came from the worker and not from something else.
	//
	// ⚠ ALLOWLISTED BY NAME, DELIBERATELY. `mov word [rsp+x], ax` is an ordinary instruction; finding it in an
	// arbitrary function would NOT mean AX holds a meaningful count. list_count and list_count_not_dead do not
	// have this shape at all. Labelling a random register as "= 7" would be a fabricated measurement.
	constexpr uint8_t kAxStoreAnchor[]{ 0x66, 0x89, 0x44, 0x24 };
	constexpr size_t kResultSearchLength = 0x40;
	constexpr const char* kResultCaptured[]{ "ai_living_count", "ai_nonswarm_count" };

	// WHY ai_place DECLINES - the placement gate.
	//
	// Both ai_place implementations call one worker (rva 0xFD810), and before it looks at the squad reference
	// at all it walks TLS to a state object and tests it:
	//
	//     mov rax, gs:[0x58] ... mov rax, [0x60 + rsi]   ; rax = the state object
	//     cmp byte [rax+0x10], 1   / je proceed          ; 80 78 10 01  <- HOOK HERE
	//     test rax,rax             / je fail
	//     cmp byte [rax], 0        / jne .check
	//     cmp byte [rax+1], 0      / je fail
	//     .check: cmp byte [rax+0x10], 2 / jne fail
	//
	// So it proceeds iff state==1, or state==2 with one of the two leading bytes set. Otherwise it returns
	// having done nothing - no error, no erase, no units, which is exactly the observed failure.
	//
	// Hooking ON the compare rather than at the entry means the GAME has already done the TLS walk and rax
	// holds the object: no need to reproduce a gs:[0x58] chain from the hook, which is the fragile part. RBX
	// already holds the squad reference by then (mov ebx,ecx at +0x13) and RDX still holds the count, so one
	// hook yields the reference, the count AND the verdict.
	constexpr uint8_t kPlacementGateAnchor[]{ 0x80, 0x78, 0x10, 0x01 };
	constexpr size_t kWorkerSearchLength = 0x40;

	// PER-SQUAD PLACEMENT - the layer below the gate.
	//
	// One ai_place on a GROUP (type 2) expands through an iterator into many squads, and both the group branch
	// and the single-squad branch of the worker funnel each one through the same call:
	//
	//     lea rdx, [rsp+0x20] / call 0x48EC0      ; 48 8D 54 24 20 E8, at 0xFDAE4 and 0xFDB54
	//
	// with ECX = the squad index. So a hook on 0x48EC0 counts the squads a placement ACTUALLY performed, which
	// is the difference between "the group expanded to eight squads" and "the group expanded to two". The gate
	// says whether the call was allowed to proceed; this says how far it got.
	//
	// ⚠ 0x48EC0 is generic, so other systems may reach it too. Records are labelled as what they are - a squad
	// placement - rather than being attributed to the script call that happened to be nearby.
	constexpr uint8_t kSquadPlaceAnchor[]{ 0x48, 0x8D, 0x54, 0x24, 0x20, 0xE8 };
	constexpr size_t kWorkerBodyLength = 0x400;

	// THE PLACEMENT WORKER'S EARLY EXIT - the last thing above "units simply do not appear".
	//
	// ⚠⚠ AN EARLIER VERSION HOOKED THE WRONG THING HERE AND LABELLED IT "the per-squad suppression flag".
	// Inside 0x48EC0 there is a block testing a global bit and then `test byte [rax+rcx*4+0x48], 1`, and both
	// tests jump to 0x49564 - which I called a bail. IT IS NOT A BAIL, IT IS A TAIL CALL:
	//     0x49564: mov rdx,r13 / mov ecx,edi / add rsp,0xA8 / pop r15,r13,r12,rdi / jmp 0x49580
	// It restores the frame and jumps to the REAL worker, passing the squad index. So that block is
	// conditional (note the message sink `call 0x2040D0` right before it) and skipping it is the NORMAL path -
	// every placement in a working run takes it. The log said so plainly: 100% "BAILED", including runs that
	// spawned units.
	//
	// ★ THE CHECK THAT WOULD HAVE CAUGHT IT, AND IS APPLIED HERE: follow the jump target and confirm it is an
	// EPILOGUE before calling anything a bail. 0x4AEF5 is `add rsp,0x58D8 / pop r14 / pop rbx / ret` - a real
	// return - whereas 0x49564 ends in `jmp`, which is a continuation.
	//
	// The real worker 0x49580 has a genuine early exit:
	//     mov eax, 0x40 / mov rax, [rax+rbx]    ; rax = [tls slot + 0x40]
	//     cmp byte [rax+1], 0 / je 0x4AEF5      ; zero -> RETURN, having done nothing
	// Hooking the compare (rax live) reads the byte the game is about to test. Anchored on the whole
	// mov/mov/cmp sequence, which is unique within 0x1000 bytes of the per-squad entry.
	constexpr uint8_t kWorkerGateAnchor[]{ 0xB8, 0x40, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x04, 0x18, 0x80, 0x78, 0x01, 0x00 };
	constexpr size_t kWorkerGateAnchorCompareOffset = 9;   // the cmp within the anchor
	constexpr size_t kSquadPlaceBodyLength = 0x1000;

	// What a record describes. Neither the gate nor a squad placement is a script call, so they cannot be
	// labelled from the function table.
	constexpr uint8_t kKindScriptCall = 0;
	constexpr uint8_t kKindPlacementGate = 1;
	constexpr uint8_t kKindSquadPlacement = 2;

	// ⚠ THE HOOK RUNS ON THE GAME THREAD FOR EVERY CALL TO A WATCHED FUNCTION. It must not allocate, take a
	// lock, format a string, or log. So it writes a fixed-size record into a ring buffer and nothing else;
	// the render thread resolves names and formats.
	struct TraceRecord
	{
		uint16_t functionIndex;
		uint32_t node;             // EBX: the expression-node handle of THIS call site
		uint32_t arg0, arg1;       // the first two evaluated arguments, four bytes each
		uint8_t argCount;          // how many the definition declares, so unused slots are not shown
		uint8_t haveArgs;          // 0 = the evaluator returned null, or the buffer would not read
		uint16_t result;           // the 16-bit return value, or the placement gate's state byte
		uint8_t haveResult;        // 0 = no result hook on this function, or it has not run yet
		uint8_t kind;              // kScriptCall / kPlacementGate
		uint8_t gateB0, gateB1;    // the two leading bytes the gate consults when state == 2
		uint32_t tick;             // GetTickCount, for ordering and for showing age
	};

	constexpr size_t kRingSize = 512;
	std::array<TraceRecord, kRingSize> gRing{};
	std::atomic<uint64_t> gWriteCursor{ 0 };   // monotonic; index = cursor % kRingSize

	// ⚠ COUNTS EVERY ENTRY TO THE HOOK, BEFORE THE FILTER. Without this, "nothing on screen" is ambiguous
	// between three completely different faults: the hook never fires, it fires but no traced function is
	// being called, or it records fine and the drawing is broken. That ambiguity cost a whole test run.
	std::atomic<uint64_t> gTotalHookCalls{ 0 };

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
	// ⚠ ai_erase is NOT the only way units leave. d20 marks enc7_1_cov and enc7_1_flood `ai_disposable true`
	// in three later cleanup scripts and the level calls garbage_collect_now/_unsafe seven times, so a squad
	// can be DELETED with no ai_erase call at all - which is exactly the hole in "2018 placements, 0 erases,
	// no enemies". ai_migrate is here for the same reason: enc7_1 ends with
	// `(ai_migrate enc7_1_cov sq_enc7_3_cov_jackals)`, which MOVES the squad rather than removing it, and
	// "they spawn in a different spot" is a migration, not a failed placement.
	constexpr const char* kDefaultTraced[] =
	{
		"print", "print_if",
		"ai_place", "ai_erase", "ai_living_count", "ai_actors",
		"ai_migrate", "ai_disposable", "garbage_collect_now", "garbage_collect_unsafe",
		// ai_nonswarm_count is the OTHER half of enc6_1_manager's logic and was the blind spot in the first
		// trace: the covenant reinforcement wave is gated on it, and so is the limiter increment that is
		// supposed to cap the waves at four. Without it the manager's covenant branch is invisible.
		"ai_nonswarm_count",
	};

	// One hook per distinct implementation, and the hook has to know WHICH function it is on - which it
	// cannot read from a register (CX is gone) and cannot read from ctx.rip either, because safetyhook
	// documents rip as pointing into the trampoline, not at the hooked address. So each slot gets its own
	// callback: a template thunk that closes over its slot number at compile time. Sixteen is well clear of
	// the ten implementations the default filter needs.
	constexpr size_t kMaxTracedImpls = 16;

	struct SlotInfo
	{
		std::atomic<uint16_t> functionIndex{ 0 };
		std::atomic<uint8_t> argCount{ 0 };
		// A zero-argument function never calls the evaluator, so it has no anchor and is hooked at its ENTRY
		// instead. There the node is still in RDX - it has not been stashed into RBX yet - and there are no
		// arguments to read. Both facts are per-slot, so one callback still serves every hook.
		std::atomic<uint8_t> nodeInRdx{ 0 };
		std::atomic<uint8_t> expectsArgs{ 0 };
	};
	std::array<SlotInfo, kMaxTracedImpls> gSlots{};

	// Where each slot's most recent record landed, so the result hook can complete the record the ARGUMENT
	// hook just wrote rather than emitting a second, disconnected line. Safe because the two fire back to back
	// on the same thread with only the worker call between them, and the worker does not run script functions.
	// The result hook still re-checks the record's function index before touching it.
	std::array<std::atomic<uint64_t>, kMaxTracedImpls> gLastRecordIndex{};
	std::atomic<uint64_t> gLastSquadRecord{ ~0ull };   // same idea, for the per-squad flag hook
	constexpr uint64_t kNoRecord = ~0ull;
}


class HCEScriptTrace::HCEScriptTraceImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	std::shared_ptr<MultilevelPointer> mFunctionTable;

	// ⚠⚠ ONE HOOK PER TRACED IMPLEMENTATION, NOT ONE SHARED HOOK.
	//
	// The first version hooked the function at definition +0x18 because it is identical for every script
	// function and takes the function index in CX, which looked like the perfect single choke point. It fires
	// ZERO times in a running game. That field is the COMPILE-TIME PARSE function: it type-checks arguments
	// against def+0x3A and formats compiler error messages. Script compilation happened when the tag was
	// built, so at runtime it is never called - the hook attached successfully and the counter sat at 0.
	//
	// The RUNTIME evaluate is definition +0x20. It calls the argument evaluator that walks the TLS script
	// THREAD structures, which only exist while the game is running. It is per-function (print and print_if
	// share one), so tracing N functions needs a hook per DISTINCT implementation address - resolved out of
	// the table at runtime, because these are not fixed pointer-data entries.
	std::vector<std::shared_ptr<ModuleMidHook>> mImplHooks;

	std::atomic<bool> mReady{ false };

	// index -> name, filled lazily on the RENDER thread. The names live in the sim's .rdata and never move
	// while the module is loaded, but resolving one costs two reads, so cache it.
	std::mutex mNamesMutex;
	std::unordered_map<uint16_t, std::string> mNames;
	uint64_t mLastRendered = 0;
	uint64_t mLoggedUpTo = 0;        // how far the log has consumed the ring
	uint32_t mLastIdleLogTick = 0;   // rate-limits the "hook alive" line
	// One entry per DISTINCT implementation address, in hook-slot order. `label` accumulates every name that
	// shares the implementation, because print and print_if genuinely are the same code and claiming a call
	// was one of them specifically would be a guess. Filled by armDefaultFilter, consumed by installImplHooks.
	struct WantedImpl
	{
		uintptr_t implAddress = 0;
		uint16_t functionIndex = 0;   // the first name found on this implementation
		uint8_t argCount = 0;
		bool hookAtEntry = false;     // true for zero-argument functions - see SlotInfo::nodeInRdx
		bool wantsResult = false;     // allowlisted count function - also hook where it stores AX
		std::string label;
	};
	std::vector<WantedImpl> mWantedImpls;

	// Finds the instruction after the implementation's call to the shared argument evaluator, which is where
	// the hook goes. Also reports the evaluator it found, so the caller can check that every implementation
	// agrees - disagreement means the prologue is not the one this was written against and nothing should be
	// hooked at all.
	static bool findArgsHookPoint(uintptr_t impl, uintptr_t& hookPoint, uintptr_t& evaluator)
	{
		uint8_t window[kPrologueSearchLength]{};
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)impl, window, sizeof(window), nullptr))
			return false;

		for (size_t i = 0; i + sizeof(kArgEvaluatorAnchor) + 4 <= sizeof(window); ++i)
		{
			if (memcmp(window + i, kArgEvaluatorAnchor, sizeof(kArgEvaluatorAnchor)) != 0) continue;

			const uintptr_t callAddress = impl + i + (sizeof(kArgEvaluatorAnchor) - 1);   // at the E8
			int32_t relative = 0;
			memcpy(&relative, window + i + sizeof(kArgEvaluatorAnchor), sizeof(relative));
			hookPoint = callAddress + 5;
			evaluator = hookPoint + (intptr_t)relative;
			return true;
		}
		return false;
	}

	// Finds the `mov word ptr [rsp+x], ax` that stores an allowlisted count function's return value, searching
	// forward from the argument hook. Requires EXACTLY ONE call in between: that is what establishes AX as the
	// worker's return rather than a leftover. Returns false if the shape is not what we verified.
	static bool findResultHookPoint(uintptr_t argsHookPoint, uintptr_t& hookPoint)
	{
		uint8_t window[kResultSearchLength]{};
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)argsHookPoint, window, sizeof(window), nullptr))
			return false;

		for (size_t i = 0; i + sizeof(kAxStoreAnchor) <= sizeof(window); ++i)
		{
			if (memcmp(window + i, kAxStoreAnchor, sizeof(kAxStoreAnchor)) != 0) continue;

			size_t calls = 0;
			for (size_t j = 0; j < i; ++j) if (window[j] == 0xE8) ++calls;
			if (calls != 1) return false;   // not the shape this was written against

			hookPoint = argsHookPoint + i;
			return true;
		}
		return false;
	}

	// From an ai_place argument hook, follows the first call to the placement worker and locates the gate
	// compare inside it. Returns false if either step does not match what was verified, in which case no gate
	// hook is installed and the verdict is simply absent.
	static bool findPlacementGate(uintptr_t argsHookPoint, uintptr_t& gatePoint, uintptr_t& worker)
	{
		uint8_t window[kResultSearchLength]{};
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)argsHookPoint, window, sizeof(window), nullptr))
			return false;

		size_t call = 0;
		for (; call + 5 <= sizeof(window); ++call) if (window[call] == 0xE8) break;
		if (call + 5 > sizeof(window)) return false;

		int32_t relative = 0;
		memcpy(&relative, window + call + 1, sizeof(relative));
		worker = argsHookPoint + call + 5 + (intptr_t)relative;

		uint8_t body[kWorkerSearchLength]{};
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)worker, body, sizeof(body), nullptr)) return false;

		for (size_t i = 0; i + sizeof(kPlacementGateAnchor) <= sizeof(body); ++i)
		{
			if (memcmp(body + i, kPlacementGateAnchor, sizeof(kPlacementGateAnchor)) != 0) continue;
			gatePoint = worker + i;
			return true;
		}
		return false;
	}

	// Finds the per-squad placement call inside the worker. Every occurrence must agree on the target, which is
	// what confirms the two branches really do funnel through one function.
	static bool findSquadPlacement(uintptr_t worker, uintptr_t& squadPlace)
	{
		std::vector<uint8_t> body(kWorkerBodyLength);
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)worker, body.data(), body.size(), nullptr))
			return false;

		bool found = false;
		for (size_t i = 0; i + sizeof(kSquadPlaceAnchor) + 4 <= body.size(); ++i)
		{
			if (memcmp(body.data() + i, kSquadPlaceAnchor, sizeof(kSquadPlaceAnchor)) != 0) continue;

			const uintptr_t callAddress = worker + i + (sizeof(kSquadPlaceAnchor) - 1);
			int32_t relative = 0;
			memcpy(&relative, body.data() + i + sizeof(kSquadPlaceAnchor), sizeof(relative));
			const uintptr_t target = callAddress + 5 + (intptr_t)relative;

			if (!found) { squadPlace = target; found = true; }
			else if (squadPlace != target)
			{
				PLOG_ERROR << "HCEScriptTrace: the worker's per-squad calls disagree (0x" << std::hex
					<< squadPlace << " vs 0x" << target << std::dec << "); not hooking per-squad placement";
				return false;
			}
		}
		return found;
	}

	// Finds the per-squad suppression-flag test inside the squad placement function. Unique in its first 0x100
	// bytes; if it is not there the flag simply is not reported, rather than a nearby instruction being read as
	// if it were the flag.
	static bool findSquadFlagTest(uintptr_t squadPlace, uintptr_t& flagTest)
	{
		std::vector<uint8_t> body(kSquadPlaceBodyLength);
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)squadPlace, body.data(), body.size(), nullptr))
			return false;

		for (size_t i = 0; i + sizeof(kWorkerGateAnchor) <= body.size(); ++i)
		{
			if (memcmp(body.data() + i, kWorkerGateAnchor, sizeof(kWorkerGateAnchor)) != 0) continue;
			flagTest = squadPlace + i + kWorkerGateAnchorCompareOffset;
			return true;
		}
		return false;
	}

	// Installs one midhook per distinct implementation, each on its own thunk so the callback knows which
	// function it is on. Returns how many actually attached, because "armed" and "hooked" are different
	// things and conflating them is what hid the parse/evaluate mistake for a whole test session.
	int installImplHooks()
	{
		mImplHooks.clear();

		// Locate every hook point first and require the implementations to agree on the evaluator. Doing this
		// before installing anything means a mismatched build hooks NOTHING rather than hooking some of it.
		std::vector<uintptr_t> hookPoints(mWantedImpls.size(), 0);
		uintptr_t agreedEvaluator = 0;
		for (size_t i = 0; i < mWantedImpls.size(); ++i)
		{
			uintptr_t hookPoint = 0, evaluator = 0;
			if (!findArgsHookPoint(mWantedImpls[i].implAddress, hookPoint, evaluator))
			{
				// A function that takes no arguments never calls the evaluator, so having no anchor is correct
				// rather than a failure - garbage_collect_now and garbage_collect_unsafe just set a flag word.
				// Hook the entry instead: the node is still in RDX there, and there is nothing to wait for.
				if (mWantedImpls[i].argCount == 0)
				{
					mWantedImpls[i].hookAtEntry = true;
					hookPoints[i] = mWantedImpls[i].implAddress;
					PLOG_DEBUG << "HCEScriptTrace: '" << mWantedImpls[i].label
						<< "' takes no arguments, hooking its entry at 0x" << std::hex
						<< mWantedImpls[i].implAddress << std::dec;
					continue;
				}

				PLOG_ERROR << "HCEScriptTrace: no argument-evaluator call found in the first "
					<< kPrologueSearchLength << " bytes of '" << mWantedImpls[i].label
					<< "' implementation 0x" << std::hex << mWantedImpls[i].implAddress << std::dec
					<< ", and it declares " << (int)mWantedImpls[i].argCount << " arguments so it should have one";
				return 0;
			}
			if (agreedEvaluator == 0) agreedEvaluator = evaluator;
			else if (agreedEvaluator != evaluator)
			{
				PLOG_ERROR << "HCEScriptTrace: implementations disagree on the argument evaluator ('"
					<< mWantedImpls[i].label << "' reaches 0x" << std::hex << evaluator << ", others reach 0x"
					<< agreedEvaluator << std::dec << "). Refusing to hook - this is not the build these "
					"offsets were derived against.";
				return 0;
			}
			hookPoints[i] = hookPoint;
		}

		int attached = 0;
		bool gateHooked = false;
		for (size_t i = 0; i < mWantedImpls.size(); ++i)
		{
			// Publish the slot's identity BEFORE the hook exists, so the callback can never read a stale one.
			gSlots[i].functionIndex.store(mWantedImpls[i].functionIndex, std::memory_order_release);
			gSlots[i].argCount.store(mWantedImpls[i].argCount, std::memory_order_release);
			gSlots[i].nodeInRdx.store(mWantedImpls[i].hookAtEntry ? 1 : 0, std::memory_order_release);
			gSlots[i].expectsArgs.store(mWantedImpls[i].argCount > 0 ? 1 : 0, std::memory_order_release);

			void* address = (void*)hookPoints[i];
			auto pointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>(address);
			auto hook = ModuleMidHook::make(mGame.toModuleName(), pointer, kThunks[i], true);
			if (!hook || !hook->isHookInstalled())
			{
				PLOG_ERROR << "HCEScriptTrace: failed to hook '" << mWantedImpls[i].label << "' at 0x"
					<< std::hex << hookPoints[i] << std::dec;
				continue;
			}
			mImplHooks.push_back(std::move(hook));
			++attached;

			// Both ai_place implementations funnel into the same worker, so the gate is hooked ONCE.
			if (mWantedImpls[i].label == "ai_place" && !gateHooked && !mWantedImpls[i].hookAtEntry)
			{
				uintptr_t gatePoint = 0, worker = 0;
				if (!findPlacementGate(hookPoints[i], gatePoint, worker))
				{
					PLOG_ERROR << "HCEScriptTrace: could not locate the placement gate from ai_place at 0x"
						<< std::hex << hookPoints[i] << std::dec
						<< "; placements will be logged without a verdict";
				}
				else
				{
					void* gateAddress = (void*)gatePoint;
					auto gatePointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>(gateAddress);
					auto gateHook = ModuleMidHook::make(mGame.toModuleName(), gatePointer, &placementGateHook, true);
					if (!gateHook || !gateHook->isHookInstalled())
						PLOG_ERROR << "HCEScriptTrace: failed to hook the placement gate at 0x"
							<< std::hex << gatePoint << std::dec;
					else
					{
						mImplHooks.push_back(std::move(gateHook));
						gateHooked = true;
						PLOG_DEBUG << "HCEScriptTrace: placement worker 0x" << std::hex << worker
							<< ", gate hooked at 0x" << gatePoint << std::dec;

						uintptr_t squadPlace = 0;
						if (!findSquadPlacement(worker, squadPlace))
							PLOG_ERROR << "HCEScriptTrace: per-squad placement call not found in the worker; "
								"placements will show a verdict but not how many squads they placed";
						else
						{
							void* squadAddress = (void*)squadPlace;
							auto squadPointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>(squadAddress);
							auto squadHook = ModuleMidHook::make(mGame.toModuleName(), squadPointer,
								&squadPlacementHook, true);
							if (!squadHook || !squadHook->isHookInstalled())
								PLOG_ERROR << "HCEScriptTrace: failed to hook per-squad placement at 0x"
									<< std::hex << squadPlace << std::dec;
							else
							{
								mImplHooks.push_back(std::move(squadHook));
								PLOG_DEBUG << "HCEScriptTrace: per-squad placement hooked at 0x"
									<< std::hex << squadPlace << std::dec;

								uintptr_t flagTest = 0;
								if (!findSquadFlagTest(squadPlace, flagTest))
									PLOG_ERROR << "HCEScriptTrace: per-squad suppression flag test not found; "
										"squads will show as placed without saying whether the worker ran";
								else
								{
									void* flagAddress = (void*)flagTest;
									auto flagPointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>(flagAddress);
									auto flagHook = ModuleMidHook::make(mGame.toModuleName(), flagPointer,
										&squadFlagHook, true);
									if (!flagHook || !flagHook->isHookInstalled())
										PLOG_ERROR << "HCEScriptTrace: failed to hook the per-squad flag test at 0x"
											<< std::hex << flagTest << std::dec;
									else
									{
										mImplHooks.push_back(std::move(flagHook));
										PLOG_DEBUG << "HCEScriptTrace: placement worker gate hooked at 0x"
											<< std::hex << flagTest << std::dec;
									}
								}
							}
						}
					}
				}
			}

			if (mWantedImpls[i].wantsResult && !mWantedImpls[i].hookAtEntry)
			{
				uintptr_t resultPoint = 0;
				if (!findResultHookPoint(hookPoints[i], resultPoint))
				{
					PLOG_ERROR << "HCEScriptTrace: '" << mWantedImpls[i].label
						<< "' should store its result in AX but the expected instruction was not found; "
						"the count will be missing rather than wrong";
				}
				else
				{
					void* resultAddress = (void*)resultPoint;
					auto resultPointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>(resultAddress);
					auto resultHook = ModuleMidHook::make(mGame.toModuleName(), resultPointer, kResultThunks[i], true);
					if (!resultHook || !resultHook->isHookInstalled())
						PLOG_ERROR << "HCEScriptTrace: failed to hook '" << mWantedImpls[i].label
							<< "' result at 0x" << std::hex << resultPoint << std::dec;
					else
					{
						mImplHooks.push_back(std::move(resultHook));
						PLOG_DEBUG << "HCEScriptTrace: slot " << i << " captures '" << mWantedImpls[i].label
							<< "' RETURN at 0x" << std::hex << resultPoint << std::dec;
					}
				}
			}

			PLOG_DEBUG << "HCEScriptTrace: slot " << i << " = '" << mWantedImpls[i].label << "' argc "
				<< (int)mWantedImpls[i].argCount << ", implementation 0x" << std::hex
				<< mWantedImpls[i].implAddress << " hooked after the evaluator call at 0x" << hookPoints[i]
				<< std::dec;
		}

		PLOG_DEBUG << "HCEScriptTrace: attached " << attached << " of " << mWantedImpls.size()
			<< " script implementation hooks; argument evaluator 0x" << std::hex << agreedEvaluator << std::dec;
		return attached;
	}

	// The label for a hook slot, or an empty string if the index is not one we armed. Render thread only.
	std::string labelForIndex(uint16_t index) const
	{
		for (const auto& wanted : mWantedImpls)
			if (wanted.functionIndex == index) return wanted.label;
		return {};
	}

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
		for (auto& last : gLastRecordIndex) last.store(kNoRecord, std::memory_order_relaxed);
		gLastSquadRecord.store(kNoRecord, std::memory_order_relaxed);
		mWantedImpls.clear();   // must not accumulate across toggles

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

					// def+0x20 is the RUNTIME implementation - the thing to hook. See the note on mImplHooks.
					uintptr_t impl = 0;
					if (!HCEGetPointer(definition + kDefImplOffset, impl) || impl <= 0x10000)
					{
						PLOG_ERROR << "HCEScriptTrace: '" << name << "' index " << i
							<< " has no readable implementation pointer at def+0x20; not traced";
						break;
					}

					uint16_t declaredArgs = 0;
					{
						uintptr_t wide = 0;
						if (HCEGetPointer(definition + kDefArgCountOffset, wide)) declaredArgs = (uint16_t)(wide & 0xFFFF);
					}

					// Two names can share one implementation (print and print_if do), and one hook cannot tell
					// them apart afterwards - CX is gone by then. So they share a slot and the label names both.
					auto existing = std::find_if(mWantedImpls.begin(), mWantedImpls.end(),
						[impl](const WantedImpl& w) { return w.implAddress == impl; });
					if (existing != mWantedImpls.end())
					{
						existing->label += "/" + name;
						// The larger count is the safe one: it only decides how many argument slots to SHOW,
						// and showing an extra dword beats silently dropping a real one.
						existing->argCount = (uint8_t)std::max<uint16_t>(existing->argCount, declaredArgs);
						PLOG_DEBUG << "HCEScriptTrace: '" << name << "' index " << i
							<< " shares implementation 0x" << std::hex << impl << std::dec << " - same slot";
						++armed;
						break;
					}

					if (mWantedImpls.size() >= kMaxTracedImpls)
					{
						PLOG_ERROR << "HCEScriptTrace: '" << name << "' index " << i
							<< " needs a " << (mWantedImpls.size() + 1) << "th hook slot but only "
							<< kMaxTracedImpls << " exist; not traced. Raise kMaxTracedImpls and add a thunk.";
						break;
					}

					bool wantsResult = false;
				for (const char* counted : kResultCaptured) if (name == counted) wantsResult = true;

				mWantedImpls.push_back(WantedImpl{ impl, i, (uint8_t)declaredArgs, false, wantsResult, name });
					PLOG_DEBUG << "HCEScriptTrace: tracing '" << name << "' index " << i << ", argc "
						<< declaredArgs << ", implementation 0x" << std::hex << impl << std::dec;
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

	// ⚠ GAME THREAD, EVERY CALL TO A WATCHED FUNCTION. Do the minimum. See the header.
	static void argsHookCommon(SafetyHookContext& ctx, size_t hookSlot)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;

		gTotalHookCalls.fetch_add(1, std::memory_order_relaxed);

		const uint16_t index = gSlots[hookSlot].functionIndex.load(std::memory_order_relaxed);
		if (index >= kMaxFunctionIndex) return;
		if (!gWanted[index].load(std::memory_order_relaxed)) return;

		TraceRecord record{};
		record.functionIndex = index;
		record.argCount = gSlots[hookSlot].argCount.load(std::memory_order_relaxed);
		record.node = (uint32_t)((gSlots[hookSlot].nodeInRdx.load(std::memory_order_relaxed) ? ctx.rdx : ctx.rbx)
			& 0xFFFFFFFF);
		record.tick = GetTickCount();

		// ⚠ A NULL RAX IS DATA, NOT AN ERROR. The evaluator returns null when it declines to run the call -
		// which is exactly how `print_if` short-circuits on a false condition - so "called but no args" is a
		// meaningful thing to see rather than something to hide.
		//
		// Read with ReadProcessMemory rather than dereferencing: the buffer is a temporary the script thread
		// owns and a stale or half-torn pointer here would fault on the GAME thread, killing the process to
		// print a debug line. The syscall costs a microsecond or two and these functions run at tens of calls
		// per second, not thousands.
		if (gSlots[hookSlot].expectsArgs.load(std::memory_order_relaxed) && ctx.rax >= 0x10000)
		{
			uint32_t value = 0;
			if (ReadProcessMemory(GetCurrentProcess(), (void*)ctx.rax, &value, sizeof(value), nullptr))
			{
				record.arg0 = value;
				record.haveArgs = 1;
				// Separate read, deliberately: one 8-byte read fails ENTIRELY if the second half straddles an
				// unmapped page, which would throw away arg0 as well.
				if (record.argCount >= 2
					&& ReadProcessMemory(GetCurrentProcess(), (void*)(ctx.rax + 4), &value, sizeof(value), nullptr))
					record.arg1 = value;
			}
		}

		const uint64_t slot = gWriteCursor.fetch_add(1, std::memory_order_acq_rel);
		gRing[slot % kRingSize] = record;

		// Publish where it landed so the result hook can complete THIS record instead of emitting a separate,
		// disconnected line. Only meaningful for pass 2 - pass 1 jumps straight to the epilogue and never
		// reaches the worker call, so it can never be completed.
		gLastRecordIndex[hookSlot].store(record.haveArgs ? slot : kNoRecord, std::memory_order_release);
	}

	// ⚠ GAME THREAD. Fires ON the `mov word [rsp+x], ax` that stores an allowlisted count function's return,
	// so AX is still live. Completes the record the argument hook wrote microseconds earlier.
	static void resultHookCommon(SafetyHookContext& ctx, size_t hookSlot)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;

		const uint64_t slot = gLastRecordIndex[hookSlot].load(std::memory_order_acquire);
		if (slot == kNoRecord) return;

		// Re-check ownership: if the ring wrapped or another function's record landed here, leave it alone
		// rather than stamping a count onto an unrelated call.
		TraceRecord& record = gRing[slot % kRingSize];
		if (record.functionIndex != gSlots[hookSlot].functionIndex.load(std::memory_order_relaxed)) return;

		record.result = (uint16_t)(ctx.rax & 0xFFFF);
		record.haveResult = 1;
		gLastRecordIndex[hookSlot].store(kNoRecord, std::memory_order_release);
	}

	// ⚠ GAME THREAD, once per ai_place worker call. Fires ON the gate's `cmp byte [rax+0x10], 1`, so the game
	// has already done the TLS walk for us and RAX is the state object. RBX is the squad reference and RDX the
	// count - the whole verdict in one hook, with no gs:[0x58] chain to reproduce.
	static void placementGateHook(SafetyHookContext& ctx)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;
		gTotalHookCalls.fetch_add(1, std::memory_order_relaxed);

		TraceRecord record{};
		record.kind = kKindPlacementGate;
		record.arg0 = (uint32_t)(ctx.rbx & 0xFFFFFFFF);   // the squad/group reference
		record.arg1 = (uint32_t)(ctx.rdx & 0xFFFF);       // the count argument
		record.argCount = 2;
		record.haveArgs = 1;
		record.tick = GetTickCount();

		// One read covering [0] through [0x10]: the gate consults exactly those three bytes. If it fails the
		// verdict is reported as unknown rather than assumed - a guessed verdict is the whole thing we are
		// trying to avoid here.
		uint8_t state[0x11]{};
		if (ctx.rax >= 0x10000
			&& ReadProcessMemory(GetCurrentProcess(), (void*)ctx.rax, state, sizeof(state), nullptr))
		{
			record.result = state[0x10];
			record.gateB0 = state[0];
			record.gateB1 = state[1];
			record.haveResult = 1;
		}

		const uint64_t slot = gWriteCursor.fetch_add(1, std::memory_order_acq_rel);
		gRing[slot % kRingSize] = record;
	}

	// ⚠ GAME THREAD. Fires ON the real worker's early-exit compare, completing the squad record written
	// microseconds earlier by squadPlacementHook - same thread, one tail call in between.
	static void squadFlagHook(SafetyHookContext& ctx)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;

		const uint64_t slot = gLastSquadRecord.load(std::memory_order_acquire);
		if (slot == kNoRecord) return;

		TraceRecord& record = gRing[slot % kRingSize];
		if (record.kind != kKindSquadPlacement) return;

		// Exactly the byte the game is about to test: [rax+1].
		const uintptr_t flagAddress = ctx.rax + 1;
		uint8_t flags = 0;
		if (flagAddress >= 0x10000
			&& ReadProcessMemory(GetCurrentProcess(), (void*)flagAddress, &flags, sizeof(flags), nullptr))
		{
			record.result = flags;
			record.haveResult = 1;
		}
		gLastSquadRecord.store(kNoRecord, std::memory_order_release);
	}

	// ⚠ GAME THREAD, once per squad a placement actually performs. ECX is the squad index.
	static void squadPlacementHook(SafetyHookContext& ctx)
	{
		if (GlobalKill::isKillSet()) return;
		if (!gTracing.load(std::memory_order_relaxed)) return;
		gTotalHookCalls.fetch_add(1, std::memory_order_relaxed);

		TraceRecord record{};
		record.kind = kKindSquadPlacement;
		record.arg0 = (uint32_t)(ctx.rcx & 0xFFFFFFFF);
		record.argCount = 1;
		record.haveArgs = 1;
		record.tick = GetTickCount();

		const uint64_t slot = gWriteCursor.fetch_add(1, std::memory_order_acq_rel);
		gRing[slot % kRingSize] = record;

		// ⚠⚠ THIS STORE MUST LIVE HERE, IN THE PER-SQUAD HOOK. It was originally applied to the per-CALL gate
		// hook instead, so squadFlagHook always loaded a kKindPlacementGate record, failed its kind check and
		// returned - producing "no gate reading" for every squad across two whole test runs. The symptom
		// (a hook that never fires) looks exactly like "the code path is never executed", which is how it got
		// misdiagnosed as a control-flow discovery rather than a bug in the pairing.
		gLastSquadRecord.store(slot, std::memory_order_release);
	}

	template<size_t N>
	static void argsHookThunk(SafetyHookContext& ctx) { argsHookCommon(ctx, N); }

	template<size_t N>
	static void resultHookThunk(SafetyHookContext& ctx) { resultHookCommon(ctx, N); }

	static constexpr safetyhook::MidHookFn kThunks[kMaxTracedImpls]
	{
		&argsHookThunk<0>,  &argsHookThunk<1>,  &argsHookThunk<2>,  &argsHookThunk<3>,
		&argsHookThunk<4>,  &argsHookThunk<5>,  &argsHookThunk<6>,  &argsHookThunk<7>,
		&argsHookThunk<8>,  &argsHookThunk<9>,  &argsHookThunk<10>, &argsHookThunk<11>,
		&argsHookThunk<12>, &argsHookThunk<13>, &argsHookThunk<14>, &argsHookThunk<15>,
	};

	// Paired with kThunks by index: slot i's result hook completes slot i's argument record.
	static constexpr safetyhook::MidHookFn kResultThunks[kMaxTracedImpls]
	{
		&resultHookThunk<0>,  &resultHookThunk<1>,  &resultHookThunk<2>,  &resultHookThunk<3>,
		&resultHookThunk<4>,  &resultHookThunk<5>,  &resultHookThunk<6>,  &resultHookThunk<7>,
		&resultHookThunk<8>,  &resultHookThunk<9>,  &resultHookThunk<10>, &resultHookThunk<11>,
		&resultHookThunk<12>, &resultHookThunk<13>, &resultHookThunk<14>, &resultHookThunk<15>,
	};

	void onToggleChange(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (!newValue)
			{
				gTracing.store(false, std::memory_order_release);
				mImplHooks.clear();       // detaches and destroys every implementation hook
				mWantedImpls.clear();
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
			gTotalHookCalls.store(0, std::memory_order_release);
			mLastRendered = 0;
			mLoggedUpTo = 0;
			mLastIdleLogTick = 0;
			gTracing.store(true, std::memory_order_release);

			const int hooked = installImplHooks();
			if (hooked == 0)
				throw HCMRuntimeException("Found the script functions to trace but could not hook any of their "
					"implementations. See HCMInternal_logging.txt for the addresses that failed.");

			messagesGUI->addMessage(std::format("Script trace on - {} functions via {} hooks.", armed, hooked));
		}
		catch (HCMRuntimeException ex)
		{
			gTracing.store(false, std::memory_order_release);
			mImplHooks.clear();
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Formats one record. Node is the CALL SITE (two prints from the same script are otherwise identical) and
	// the args are the evaluated arguments - for the ai_* functions arg0 IS the squad/encounter reference,
	// which is the whole point of hooking after the evaluator rather than at the entry.
	std::string describe(const TraceRecord& record)
	{
		if (record.kind == kKindPlacementGate)
		{
			std::string verdict = "verdict UNKNOWN (state object unreadable)";
			if (record.haveResult)
			{
				// Exactly the worker's own rule: state==1, or state==2 with one of the two leading bytes set.
				const bool accepted = record.result == 1
					|| (record.result == 2 && (record.gateB0 != 0 || record.gateB1 != 0));
				verdict = std::format("state {} b0 {} b1 {} -> {}", record.result, record.gateB0, record.gateB1,
					accepted ? "ACCEPTED" : "REJECTED, places nothing");
			}
			return std::format("ai_place GATE  {}  {}", describeAiReference(record), verdict);
		}

		if (record.kind == kKindSquadPlacement)
		{
			if (!record.haveResult)
				return std::format("  -> squad {} (no gate reading - the worker was not reached)",
					(int32_t)record.arg0);
			return std::format("  -> squad {} gate 0x{:02X} -> {}", (int32_t)record.arg0, record.result,
				record.result == 0 ? "RETURNS IMMEDIATELY, places nothing" : "proceeding");
		}

		std::string label = labelForIndex(record.functionIndex);
		if (label.empty()) label = nameForIndex(record.functionIndex);

		std::string args;
		if (record.argCount == 0)
			args = "(takes no arguments)";
		else if (!record.haveArgs)
			args = "pass 1 of 2 - arguments not evaluated yet";
		else if (label.starts_with("ai_"))
			args = describeAiReference(record);
		else if (record.argCount >= 2)
			args = std::format("arg0 0x{:08X}  arg1 0x{:08X}", record.arg0, record.arg1);
		else
			args = std::format("arg0 0x{:08X}", record.arg0);

		std::string result;
		if (record.haveResult) result = std::format("  = {}", record.result);
		return std::format("{} thread {:08X}  {}{}", label, record.node, args, result);
	}

	// Decodes an hs "ai" argument. NOT a guess - this is the dispatch that ai_place's worker (rva 0xFD810)
	// performs on the value it is handed:
	//
	//     cmp  ebx, -1        ; -1 is "none" and does nothing
	//     test dx, dx         ; a zero count also does nothing
	//     shr  ecx, 0x1d      ; the TOP THREE BITS are a type tag
	//     sub ecx,1 / je ...  ; and only types 1, 2, 4, 5 and 7 have a branch. Anything else falls through
	//     ...                 ; to the same failure exit as a -1 reference.
	//
	// The type 1 branch then uses BX as a signed 16-bit index and rejects it unless it is below the count at
	// scenario+0x348, so the low half is an index into a scenario block. The remaining bits are a second
	// field this has not pinned down yet, so they are shown raw rather than named.
	static std::string describeAiReference(const TraceRecord& record)
	{
		const uint32_t reference = record.arg0;
		if (reference == 0xFFFFFFFF) return "ai=NONE (-1)";

		const uint32_t type = reference >> 29;
		const uint16_t index = (uint16_t)(reference & 0xFFFF);
		const uint32_t middle = (reference >> 16) & 0x1FFF;

		std::string decoded = std::format("ai type {} idx {}", type, index);
		if (middle != 0) decoded += std::format(" mid {}", middle);
		if (type != 1 && type != 2 && type != 4 && type != 5 && type != 7)
			decoded += " ⚠ NO BRANCH FOR THIS TYPE - the worker rejects it";
		decoded += std::format(" (raw 0x{:08X})", reference);

		// ai_place's second argument is the count, and the worker bails outright when it is zero.
		if (record.argCount >= 2)
			decoded += std::format("  count {}{}", (uint16_t)(record.arg1 & 0xFFFF),
				(uint16_t)(record.arg1 & 0xFFFF) == 0 ? " ⚠ ZERO - places nothing" : "");

		return decoded;
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
			const uint64_t totalCalls = gTotalHookCalls.load(std::memory_order_relaxed);

			// ⚠ ALWAYS DRAW SOMETHING WHILE THE TRACE IS ON. The first version drew nothing until a traced
			// function fired, so "I never saw anything on screen" could not be told apart from "the hook is
			// dead". A live counter proves the hook is running even when nothing matched yet.
			if (cursor == 0)
			{
				const auto idleColour = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
				RenderTextHelper::drawOutlinedText(
					std::format("HaloScript trace ON - {} script calls seen, none traced yet", totalCalls),
					{ 12.f, 260.f }, idleColour, 15.f);

				// One line a second at most, so a quiet trace does not fill the log.
				const uint32_t now = GetTickCount();
				if (now - mLastIdleLogTick >= 1000)
				{
					mLastIdleLogTick = now;
					PLOG_DEBUG << "HCEScriptTrace: hook alive, " << totalCalls
						<< " script evaluations seen, 0 matched the filter so far";
				}
				return;
			}

			// Log every traced call exactly once, from HERE rather than from the hook - the hook is on a hot
			// path and must not touch PLOG. This is what makes a run reviewable afterwards instead of
			// something you have to catch on screen as it happens.
			if (settings->hceScriptTraceToLog->GetValue())
			{
				const uint64_t logFrom = std::max(mLoggedUpTo, cursor > kRingSize ? cursor - kRingSize : 0ull);
				if (cursor > logFrom && mLoggedUpTo != 0 && logFrom > mLoggedUpTo)
					PLOG_WARNING << "HCEScriptTrace: log fell behind, " << (logFrom - mLoggedUpTo)
						<< " traced calls were overwritten before they could be logged";

				for (uint64_t i = logFrom; i < cursor; ++i)
				{
					const TraceRecord record = gRing[i % kRingSize];
					PLOG_DEBUG << "HCEScriptTrace: " << describe(record)
						<< " (index " << record.functionIndex << ", raw node 0x" << std::hex << record.node << std::dec << ")";
				}
				mLoggedUpTo = cursor;
			}

			// Newest last, so it reads like a log. Only the last wantLines records, and never more than the
			// ring holds - if the game outran us we say so rather than pretending the trace is complete.
			//
			// ⚠ THE WINDOW IS WIDENED AND FIRST-PASS RECORDS ARE SKIPPED. The interpreter invokes each
			// implementation TWICE per call - the first time the argument evaluator returns null because the
			// arguments have not been evaluated yet, the second time it returns them. Verified: the records
			// alternate null/valid with perfect regularity, 9842 runs of length one, every pair sharing a node.
			// Showing both halves every visible call and tells the reader nothing, so the overlay shows only
			// the pass that carries arguments. The LOG keeps both, because an UNPAIRED null is real information:
			// that is what a false `print_if` condition looks like.
			const uint64_t available = std::min<uint64_t>(cursor, kRingSize);
			const uint64_t take = std::min<uint64_t>(available, (uint64_t)wantLines * 2);
			const uint64_t first = cursor - take;

			const uint32_t now = GetTickCount();
			std::string text = "HaloScript trace";
			if (cursor > kRingSize && mLastRendered != 0 && (cursor - mLastRendered) > kRingSize)
				text += std::format(" (dropped ~{} calls since last frame)", (cursor - mLastRendered) - kRingSize);
			text += "\n";

			// Walk NEWEST first so the lines that survive the wantLines limit are the recent ones, then emit in
			// chronological order. Breaking out of an oldest-first loop would keep the oldest and drop the new.
			std::vector<TraceRecord> visible;
			visible.reserve((size_t)wantLines);
			for (uint64_t i = cursor; i > first && (int)visible.size() < wantLines; --i)
			{
				const TraceRecord record = gRing[(i - 1) % kRingSize];
				if (record.kind == kKindScriptCall && record.argCount > 0 && !record.haveArgs)
					continue;   // first pass, carries nothing
				visible.push_back(record);
			}

			for (auto it = visible.rbegin(); it != visible.rend(); ++it)
			{
				const uint32_t ageMs = now - it->tick;   // wrap-safe unsigned
				text += std::format("{:>6}ms  {}\n", ageMs, describe(*it));
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
		mFunctionTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceScriptFunctionTable), game);

		// No hooks are created here. The implementations to hook are read out of the function table by name
		// when the trace is switched on, so nothing is patched until then.

		mReady.store(true, std::memory_order_release);
	}

	~HCEScriptTraceImpl()
	{
		mReady.store(false, std::memory_order_release);
		gTracing.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderEventCallback.removeCallback();
		mImplHooks.clear();
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
