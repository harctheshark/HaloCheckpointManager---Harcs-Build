#include "pch.h"
#include "HCESkyFix.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "PointerDataStore.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "HCESignatureScan.h"   // AddOccupant is resolved by signature, never from XML - see resolveAddOccupant
#include "HCEGameThreadTick.h"  // engine teardown may only run on the game thread
#include "HCEGetCameraData.h"   // owns the only per-frame game-thread tick on this title
#include <atomic>
#include <thread>

// See HCESkyFix.h for the mechanism and why the Blam sim is not involved.

namespace
{
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

	std::atomic<bool> gSkyFixEnabled{ false };

	// Diagnostics. Latched, because this fires on every area boundary crossing and the log is how we confirm
	// the mechanism - it must be readable, not a flood.
	std::atomic<bool> gLoggedTeardownBlocked{ false };
	std::atomic<bool> gLoggedNonTeardown{ false };
	std::atomic<uint64_t> gBlockedCount{ 0 };

	constexpr uint32_t kOccupantRefcountOffset = 0x2D8;

	// ---------------------------------------------------------------------------------------------------------
	// THE INJECTED REFERENCE, AND WHY IT MUST BE GIVEN BACK.
	//
	// Clamping to 2 leaves the count at 1 after the game's own `sub ...,1`, where an unhooked exit would have left
	// 0. That difference IS the fix - a count of 0 is what triggers teardown - but it means that from the first
	// clamp onward we are holding one reference the game did not ask for, on that area.
	//
	// Switching the fix off used to just stop clamping, which does NOT hand that reference back. The count stays
	// permanently one too high, so every later exit goes 2 -> 1 and never reaches the 0 that tears the area down:
	// the sky never goes black again, the area is pinned for the rest of the session, and closing HCM did not undo
	// it either. Turning a cheat off has to restore the game, so every injected reference is now tracked here and
	// released on disable and on teardown.
	//
	// A fixed array of atomics rather than a set behind a mutex: the recording side runs inside the midhook, on
	// the game thread, where blocking on a lock HCM's own thread might hold is not acceptable. No allocation, no
	// locking, nothing that can throw. One slot per streaming area the player has left with the fix on - 64 is far
	// more than a level has, and running out fails CLOSED (see the midhook) rather than leaking silently.
	constexpr size_t kMaxHeldAreas = 64;
	std::atomic<uintptr_t> gHeldActors[kMaxHeldAreas]{};
	std::atomic<bool> gLoggedHeldFull{ false };

	// Records that we hold an injected reference on this actor. False means no slot was free.
	//
	// outWasNew distinguishes "we just claimed this slot" from "we were already holding one here", because the
	// caller only finds out whether it actually injected anything AFTER the clamp runs - and a slot claimed for a
	// clamp that turned out to be unnecessary has to be given straight back. Recording an actor we never clamped
	// would make the release path decrement a reference we never took, corrupting the game's own count.
	bool tryRecordHeldActor(uintptr_t actor, bool* outWasNew) noexcept
	{
		*outWasNew = false;

		for (auto& slot : gHeldActors)
			if (slot.load(std::memory_order_acquire) == actor) return true;   // already holding one here

		for (auto& slot : gHeldActors)
		{
			uintptr_t empty = 0;
			if (slot.compare_exchange_strong(empty, actor, std::memory_order_acq_rel))
			{
				*outWasNew = true;
				return true;
			}
		}
		return false;
	}

	// Drops a slot claimed by tryRecordHeldActor without touching the actor. For the "reserved it, then did not
	// clamp after all" path only.
	void forgetHeldActor(uintptr_t actor) noexcept
	{
		for (auto& slot : gHeldActors)
		{
			uintptr_t held = actor;
			if (slot.compare_exchange_strong(held, 0, std::memory_order_acq_rel)) return;
		}
	}

	// Hands back exactly one reference. Interlocked because the game thread may be running its own decrement on
	// this field; never takes the count below 0. POD-only + SEH - the actor may have been freed under us.
	bool tryReleaseHeldActor(uintptr_t actor, int32_t* outBefore, int32_t* outAfter)
	{
		__try
		{
			volatile long* refcount = reinterpret_cast<volatile long*>(actor + kOccupantRefcountOffset);
			long current = *refcount;
			*outBefore = (int32_t)current;

			for (;;)
			{
				if (current <= 0) { *outAfter = (int32_t)current; return true; }   // nothing of ours left to give
				const long prev = InterlockedCompareExchange(refcount, current - 1, current);
				if (prev == current) { *outAfter = (int32_t)(current - 1); return true; }
				current = prev;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// GIVING THE REFERENCE BACK IS NOT THE SAME AS UNDOING THE FIX.
	//
	// Handing back the count restores the NUMBER, but the teardown we suppressed is what actually disables the
	// area's streaming source and de-activates its data layers - and that never ran. So an area we blocked stays
	// fully streamed with a refcount of 0: the sky keeps rendering with the fix switched off, until the player
	// crosses a boundary and the game tears it down for real.
	//
	// The correct undo is to let the ENGINE do the teardown: set the count to 1 and call RemoveOccupant, whose
	// own `sub ...,1` then hits 0 and runs the real path. hceSkyFixRemoveOccupantFunction is already that
	// function's ENTRY (its prologue is `mov [rsp+20h],rsi; push rdi; sub rsp,20h; mov rdi,rcx`, which is why the
	// midhook can read the actor straight out of RCX), so no extra address is needed.
	//
	// ⚠ GAME THREAD ONLY - it touches World Partition and data layers. Hence HCEGameThreadTick.
	//
	// ⚠ AND ONLY FOR AREAS THE PLAYER HAS ACTUALLY LEFT. If the count is above our single reference the player is
	// still stood inside, and forcing a teardown there would unload the ground under them. Those get a plain
	// decrement instead - correct, and no teardown is wanted.
	std::atomic<uintptr_t> gRemoveOccupantFunction{ 0 };

	// Set by HCM's thread, consumed by the game thread. Cleared by whichever gets there first, so the fallback
	// and the tick can never both process the same actor.
	std::atomic<bool> gReleaseRequested{ false };
	// tornDown in the low 16 bits, plain decrements in the high 16 - one atomic instead of two to read back.
	std::atomic<uint32_t> gReleaseCounters{ 0 };

	using RemoveOccupantFn = void(__fastcall*)(uintptr_t actor);

	// POD-only + SEH: reads the count, and may call straight into engine code.
	// outAction: 0 = nothing to do, 1 = plain decrement, 2 = forced a real teardown.
	bool tryTeardownOrDecrement(uintptr_t actor, uintptr_t removeOccupant, int32_t* outBefore, int* outAction)
	{
		__try
		{
			volatile long* refcount = reinterpret_cast<volatile long*>(actor + kOccupantRefcountOffset);
			const long before = *refcount;
			*outBefore = (int32_t)before;
			*outAction = 0;

			if (before <= 0) return true;   // nothing of ours left

			if (before > 1)
			{
				// Player still inside. Give our reference back and leave the area alone.
				for (long current = before;;)
				{
					if (current <= 1) break;
					const long prev = InterlockedCompareExchange(refcount, current - 1, current);
					if (prev == current) { *outAction = 1; break; }
					current = prev;
				}
				return true;
			}

			// before == 1, and that one is ours: the player is outside. Let the engine tear it down properly.
			if (!removeOccupant) return false;
			reinterpret_cast<RemoveOccupantFn>(removeOccupant)(actor);
			*outAction = 2;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Gives back every reference we injected. Safe to call when we hold none.
	//
	// ⚠ Call this only AFTER gSkyFixEnabled is false, or the midhook can re-clamp an area we just released.
	void releaseAllHeldReferences(const char* reason)
	{
		// Ask the game thread to do it, then wait briefly. The engine teardown may only run there, and this is
		// HCM's toggle/shutdown thread.
		gReleaseCounters.store(0, std::memory_order_release);
		gReleaseRequested.store(true, std::memory_order_release);

		// ~500ms. Long enough for several frames even at a poor framerate, short enough not to hang a shutdown.
		constexpr int kWaitSlices = 50;
		for (int i = 0; i < kWaitSlices && gReleaseRequested.load(std::memory_order_acquire); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));

		const uint32_t packed = gReleaseCounters.load(std::memory_order_acquire);
		const uint32_t tornDown = packed & 0xFFFF;
		const uint32_t decremented = (packed >> 16) & 0xFFFF;

		// ⚠⚠ THE FALLBACK LEAVES THE REFERENCE IN PLACE, AND USED TO DECREMENT IT. THAT WAS A CRASH.
		//
		// Decrementing looks like the conservative choice - it makes the number right - but the number was never
		// the thing that mattered. Our clamp SUPPRESSED a teardown, so the area is still fully streamed: source
		// enabled, data layers Activated. Writing the count down to 0 without running that teardown leaves the
		// engine holding an area it believes is live with a refcount saying nobody is in it. The next time World
		// Partition acts on that area it operates on state that cannot happen, and the game dies - reported as
		// "closed HCM with Sky Fix on, then the sky tried to change and it crashed", and confirmed in the log:
		//
		//     [shutdown] the game thread did not answer within 500ms, so 2 reference(s) were handed back
		//                without an engine teardown          <- and then the crash, later
		//
		// A LEFT-BEHIND REFERENCE IS HARMLESS BY COMPARISON. It keeps one area loaded - some memory, no hitching,
		// no corruption - and it resolves itself on the next level load. Nothing here is worth trading a running
		// game for a tidy integer, so when we cannot do the real teardown we do NOTHING and say so loudly.
		size_t abandoned = 0;
		if (gReleaseRequested.exchange(false, std::memory_order_acq_rel))
		{
			for (auto& slot : gHeldActors)
				if (slot.exchange(0, std::memory_order_acq_rel)) abandoned++;

			if (abandoned)
				PLOG_ERROR << "HCE Sky Fix: " << reason << " - the game thread never answered, so " << abandoned
					<< " streaming area(s) were LEFT BLOCKED rather than desynchronised. Those areas stay loaded "
					"until the next level load; the sky will not go black out of bounds until then. This is the "
					"safe outcome, but it means the DoUpdateCamera tick was unavailable - see HCEGetCameraData.";
		}

		if (tornDown || decremented)
			PLOG_INFO << "HCE Sky Fix: " << reason << " - " << tornDown
				<< " area(s) torn down by the engine and " << decremented
				<< " reference(s) handed back on areas the player is still inside. The game is back to normal "
				"streaming behaviour.";

		gLoggedHeldFull.store(false, std::memory_order_release);
	}

	// GAME THREAD, once per frame, via HCEGameThreadTick (APlayerCameraManager::DoUpdateCamera).
	//
	// Does nothing at all unless a release has been requested, which is the overwhelmingly common case - this runs
	// on the game's critical path and must cost a single relaxed load per frame.
	// Defined further down, next to the re-arm machinery it belongs with; declared here because the tick runs
	// before it in the file.
	void serviceReArmRequest();

	void skyFixGameThreadTick() noexcept
	{
		// ⚠ RE-ARM IS SERVICED HERE TOO, NOT JUST AT AREA BOUNDARIES.
		//
		// serviceReArmRequest has always been called from removeOccupantMidHook, which only fires when the player
		// crosses a boundary - so a re-arm requested at any other moment sat pending until they happened to walk
		// through one. Running it on the per-frame tick makes it immediate, which is what the teleport path needs:
		// HCESkyFix::requestAreaReArm() is called right after a seated vehicle is moved, and the area has to come
		// back on the next frame, not the next boundary crossing.
		//
		// Cheap when idle - serviceReArmRequest leads with an atomic exchange on gWantReArm and returns.
		serviceReArmRequest();

		if (!gReleaseRequested.load(std::memory_order_acquire)) return;
		if (!gReleaseRequested.exchange(false, std::memory_order_acq_rel)) return;  // fallback beat us to it

		const uintptr_t removeOccupant = gRemoveOccupantFunction.load(std::memory_order_acquire);
		uint32_t tornDown = 0, decremented = 0;

		for (auto& slot : gHeldActors)
		{
			const uintptr_t actor = slot.exchange(0, std::memory_order_acq_rel);
			if (!actor) continue;

			int32_t before = 0;
			int action = 0;
			if (!tryTeardownOrDecrement(actor, removeOccupant, &before, &action)) continue;
			if (action == 2) tornDown++;
			else if (action == 1) decremented++;
		}

		gReleaseCounters.store((decremented << 16) | (tornDown & 0xFFFF), std::memory_order_release);
	}

	// Forgets every tracked actor WITHOUT touching it. For when the level unloads: those actors are gone, so
	// dereferencing them would be a use-after-free, and the references died with the areas anyway.
	void forgetHeldReferences() noexcept
	{
		for (auto& slot : gHeldActors) slot.store(0, std::memory_order_release);
		gLoggedHeldFull.store(false, std::memory_order_release);
	}

	// RCX at RemoveOccupant's entry IS the AHaloWorldStreamingAreaActor* - the game just called a member
	// function on it, so it is live by construction.
	//
	// Runs on the game thread at an area boundary. Must never throw: a midhook that throws is a crash.
	// SEH only, POD locals only. MSVC rejects __try in a function that requires C++ object unwinding (C2712),
	// and the PLOG stream expressions below are exactly such objects - so the raw access lives here and every
	// bit of logging happens in the caller. Same split HCEGetPlayerState.cpp documents around sehCopy.
	//
	// Returns false only if the access faulted. outBefore is the refcount as the game left it.
	bool tryClampOccupantRefcount(uintptr_t actor, int32_t* outBefore, bool* outClamped)
	{
		__try
		{
			int32_t* refcount = reinterpret_cast<int32_t*>(actor + kOccupantRefcountOffset);
			const int32_t before = *refcount;
			*outBefore = before;

			// Only the 1 -> 0 transition tears the area down; the game already skips anything else.
			if (before >= 2) { *outClamped = false; return true; }

			// Clamp rather than increment: the game's own `sub dword ptr [rdi+2D8h], 1` consumes the bump, so
			// this converges on exactly 1 however many exits occur. A blind ++ would drift upward once per
			// enter/exit cycle.
			//
			// ⚠ THAT CONVERGENCE IS NOT UNCONDITIONAL, and the comment here used to claim it was. The
			// decrement at rva 0x95B1376 sits behind `test rax,rax / jz` on the result of sub_1495B3040 (the
			// World Partition subsystem lookup) at 0x95B136D. If that returns null the `sub` never executes,
			// our 2 is never consumed, and the count ratchets up by one per exit - permanently pinning the
			// area even after the fix is switched off. It requires the WP subsystem to be missing while a
			// level is loaded, so it is unlikely rather than impossible; clamping to exactly 2 (never higher)
			// bounds the damage to a single extra reference instead of an unbounded climb.
			if (before < 2) *refcount = 2;
			*outClamped = true;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// ---------------------------------------------------------------------------------------------------------
	// RE-ARM WHILE ALREADY OUT OF BOUNDS.
	//
	// The documented limitation has always been "turn it on while you are still IN bounds - if you enable it
	// after the sky has gone, walk back in once". That is because the fix only acts inside RemoveOccupant, and
	// RemoveOccupant does not fire again while you are already outside every area: the refcount is sitting at 0
	// and nothing is going to decrement it.
	//
	// Fixing it needs the LAST AREA THE PLAYER LEFT, which is exactly what RCX was at the most recent
	// RemoveOccupant - so remember it. On enable, if that actor's refcount is still <= 0, calling the game's own
	// AddOccupant does the 0 -> 1 transition properly: it re-enables the streaming source, re-acquires every
	// AffectedDataLayers entry as Activated and re-registers the camera grids. Writing 1 into the field by hand
	// would NOT do any of that - the count would look right and the sky would stay gone.
	//
	// ⚠ THREADING. AddOccupant touches UWorld and the World Partition subsystem, so it may only be called on
	// the GAME THREAD. The toggle runs on HCM's own thread, so enabling merely ARMS the request; the call is
	// made from the midhook, which is by construction on the game thread. That is also why the request is a
	// plain atomic flag and not a queued callback.
	//
	// ⚠⚠ AND THAT IS THIS DESIGN'S LIMIT, STATED HONESTLY: the only game-thread site we own here is
	// RemoveOccupant itself, which does NOT fire while the player is already outside every area - that is the
	// whole reason the sky is gone. So arming the request does not restore the sky on the spot; it restores it
	// at the NEXT area boundary the player crosses, instead of requiring a full re-enter AND re-exit as
	// before. Half the limitation, not none of it.
	//
	// Removing it entirely needs a per-frame game-thread tick. The only one HCM has on this title is
	// HCEGetCameraData's midhook on the exe's DoUpdateCamera, so the complete fix is to service this request
	// from there. That is a real dependency between two cheats and is deliberately NOT done here without
	// asking - calling engine code from the wrong thread is exactly the class of bug that costs a user their
	// run, and this file's whole job is to not do that.
	std::atomic<uintptr_t> gLastAreaActor{ 0 };
	std::atomic<bool> gWantReArm{ false };
	std::atomic<bool> gLoggedReArm{ false };

	using AddOccupantFn = void(__fastcall*)(uintptr_t actor);
	std::atomic<uintptr_t> gAddOccupantFunction{ 0 };

	// POD-only, SEH-wrapped: reads a refcount the game owns and may call straight into engine code.
	// Returns false if anything faulted. outRefcount is the count as found.
	bool tryReArmOccupant(uintptr_t actor, uintptr_t addOccupant, int32_t* outRefcount)
	{
		__try
		{
			const int32_t current = *reinterpret_cast<int32_t*>(actor + kOccupantRefcountOffset);
			*outRefcount = current;
			if (current > 0) return true;   // still occupied; nothing to do

			reinterpret_cast<AddOccupantFn>(addOccupant)(actor);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Called from the midhook (game thread). Consumes a pending re-arm request, if any.
	void serviceReArmRequest()
	{
		if (!gWantReArm.exchange(false, std::memory_order_acq_rel)) return;

		const uintptr_t actor = gLastAreaActor.load(std::memory_order_acquire);
		const uintptr_t addOccupant = gAddOccupantFunction.load(std::memory_order_acquire);
		if (!actor || !addOccupant) return;

		int32_t refcount = 0;
		if (!tryReArmOccupant(actor, addOccupant, &refcount))
		{
			LOG_ONCE(PLOG_ERROR << "HCE Sky Fix: faulted re-arming the last streaming area - disabling.");
			gSkyFixEnabled.store(false, std::memory_order_release);
			return;
		}

		if (!gLoggedReArm.exchange(true, std::memory_order_acq_rel))
			PLOG_INFO << "HCE Sky Fix: re-armed the last streaming area (refcount was " << refcount
				<< "). If you enabled the fix while already out of bounds, the sky should return without you "
				"having to walk back in.";
	}

	void removeOccupantMidHook(SafetyHookContext& ctx) noexcept
	{
		// Remembered even while the fix is OFF, so enabling it later has something to re-arm.
		if (ctx.rcx != 0) gLastAreaActor.store(ctx.rcx, std::memory_order_release);

		if (!gSkyFixEnabled.load(std::memory_order_acquire)) return;
		if (ctx.rcx == 0) return;

		serviceReArmRequest();

		// Claim a tracking slot BEFORE clamping. If we cannot track the reference we must not create it - an
		// untracked injected reference is exactly the leak this registry exists to prevent, and it would pin the
		// area for the rest of the session with nothing able to undo it. Letting this one teardown through costs
		// the user a black sky for one area; leaking would cost them the fix's reversibility entirely.
		bool slotWasNew = false;
		if (!tryRecordHeldActor(ctx.rcx, &slotWasNew))
		{
			if (!gLoggedHeldFull.exchange(true, std::memory_order_acq_rel))
				PLOG_ERROR << "HCE Sky Fix: already holding references on " << kMaxHeldAreas
					<< " streaming areas, which is more than expected. Letting this teardown proceed rather than "
					"injecting a reference we could not hand back on disable.";
			return;
		}

		int32_t before = 0;
		bool clamped = false;
		if (!tryClampOccupantRefcount(ctx.rcx, &before, &clamped))
		{
			// A bad actor pointer must not take the game down. The byte guard makes this near-impossible, but
			// being wrong here would cost the user their run, so fail closed rather than keep writing.
			if (slotWasNew) forgetHeldActor(ctx.rcx);   // nothing was written, so we hold nothing
			LOG_ONCE(PLOG_ERROR << "HCE Sky Fix: faulted accessing the occupant refcount - disabling.");
			gSkyFixEnabled.store(false, std::memory_order_release);
			return;
		}

		if (!clamped)
		{
			// The count was already >= 2. Either we clamped it on an earlier exit (slot not new - keep it, the
			// reference is still ours) or the game genuinely has two occupants and we injected nothing here.
			if (slotWasNew) forgetHeldActor(ctx.rcx);

			if (!gLoggedNonTeardown.exchange(true, std::memory_order_acq_rel))
				PLOG_DEBUG << "HCE Sky Fix: RemoveOccupant with refcount " << before
					<< " - not the teardown transition, left alone.";
			return;
		}

		const uint64_t blocked = gBlockedCount.fetch_add(1, std::memory_order_relaxed) + 1;
		if (!gLoggedTeardownBlocked.exchange(true, std::memory_order_acq_rel))
			PLOG_INFO << "HCE Sky Fix: blocked a streaming-area teardown (actor "
				<< std::format("0x{:X}", ctx.rcx) << ", refcount " << before
				<< " -> 2). The area's World Partition streaming source stays enabled and its data layers stay "
				"Activated, so the sky and its lighting should keep rendering. Further blocks are counted but "
				"not logged.";
		else if ((blocked % 50) == 0)
			PLOG_DEBUG << "HCE Sky Fix: " << blocked << " teardowns blocked so far.";
	}
}


class HCESkyFix::HCESkyFixImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> mFunction;
	std::shared_ptr<std::vector<byte>> mExpectedBytes;
	std::shared_ptr<ModuleMidHook> mHook;

	std::mutex mAttachMutex;
	bool mVerified = false;   // sticky: after attaching, the site reads back as our jmp

	// ⚠ A STRONG REFERENCE, DELIBERATELY, and it used to be weak.
	//
	// The camera cheat owns the DoUpdateCamera midhook, which IS the game-thread tick this file needs to undo its
	// work. Held weakly, the camera cheat was destroyed first at shutdown - the log showed its hook parked a full
	// 600ms before the sky fix went looking for a tick that no longer existed:
	//
	//     03:06:10.597  parked mid hook on main            <- the tick, gone
	//     03:06:11.205  the game thread did not answer within 500ms
	//
	// A shared_ptr makes the dependency real: the camera cheat cannot be torn down until the sky fix has finished
	// with it. Safe from cycles - HCEGetCameraData knows nothing about this file.
	std::shared_ptr<HCEGetCameraData> mCameraData;
	bool mTickRegistered = false;

	// Borrows / returns the camera cheat's per-frame game-thread tick.
	void setTickWanted(bool wanted)
	{
		if (wanted && !mTickRegistered)
		{
			if (!HCEGameThreadTick::add(&skyFixGameThreadTick))
			{
				PLOG_WARNING << "HCE Sky Fix: the game-thread tick table is full; engine teardown on disable is "
					"unavailable and the fallback will be used.";
				return;
			}
			mTickRegistered = true;
		}
		else if (!wanted && mTickRegistered)
		{
			HCEGameThreadTick::remove(&skyFixGameThreadTick);
			mTickRegistered = false;
		}

		// setHookWanted is refcounted per requester, so this cooperates with the camera features rather than
		// fighting them: the hook stays up while anyone wants it and comes down when the last one lets go.
		if (auto& cameraData = mCameraData)
		{
			try { cameraData->setHookWanted(this, wanted); }
			catch (HCMRuntimeException& ex) { PLOG_WARNING << "HCE Sky Fix: could not " << (wanted ? "borrow" : "release")
				<< " the camera game-thread tick: " << ex.what(); }
		}
	}

	std::atomic<bool> mReady{ false };

	// ⚠ THE ONLY CRASH-CLASS RISK in this feature is +0x2D8 not being the occupant refcount, so refuse to hook
	// a build we do not recognise. The expected bytes deliberately span the decrement itself
	// (83 AF D8 02 00 00 01 = sub dword ptr [rdi+2D8h], 1), so a match proves the hook site, the offset AND
	// the jne-skips-teardown structure all at once.
	//
	// HaloSimulation/HaloCampaignEvolved report version 0.0.0.0 for every build, so Version= in the pointer
	// data cannot detect a game update. This guard is the only protection.
	void verifyOriginalBytes()
	{
		if (!mFunction) throw HCMRuntimeException("No pointer data for the HaloCER sky fix site");
		if (!mExpectedBytes || mExpectedBytes->empty()) throw HCMRuntimeException("No expected original bytes for the HaloCER sky fix site");

		const std::vector<byte>& expected = *mExpectedBytes;
		std::vector<byte> actual(expected.size());
		if (!mFunction->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("Could not read the HaloCER sky fix site: {}", MultilevelPointer::GetLastError()));

		// ⚠⚠ COMPARE rel32 CALL TARGETS AS WILDCARDS, NOT AS BYTES. The 2026-08-17 game update moved this
		// site by 0x6540 and, once re-pointed, the guard STILL refused it - over four bytes, the rel32 of the
		// `call` at +13. Its callee had shifted; the site itself was byte-identical. A relative branch
		// displacement encodes WHERE THE TARGET IS, not what this code does, so a change in it is not a
		// change in the instruction sequence we are about to patch.
		//
		// This is the same rule resolveAddOccupant() already applies to its signature ("wildcarding its two
		// rel32 call targets") - the guard simply was not applying it to itself. Everything the comment above
		// says this check proves - the hook site, the +0x2D8 offset, the jne-skips-teardown structure - is
		// still proven, because those are all opcode and displacement-into-a-STRUCT bytes, none of which are
		// wildcarded here.
		const auto differsIgnoringRel32 = [](const std::vector<byte>& a, const std::vector<byte>& b)
		{
			if (a.size() != b.size()) return true;
			for (size_t i = 0; i < a.size(); )
			{
				if (a[i] != b[i]) return true;
				// E8 = call rel32, E9 = jmp rel32. Skip the 4 displacement bytes that follow.
				if ((a[i] == 0xE8 || a[i] == 0xE9) && i + 5 <= a.size()) { i += 5; continue; }
				++i;
			}
			return false;
		};

		if (differsIgnoringRel32(actual, expected))
			throw HCMRuntimeException(std::format(
				"The HaloCER sky fix site did not match its expected original bytes - this build of Halo "
				"Campaign Evolved is not supported. Expected [{}], found [{}] (rel32 call targets are ignored "
				"in this comparison, so this is a real instruction difference)",
				bytesToString(expected), bytesToString(actual)));
	}

	// AddOccupant, resolved by UNIQUE BYTE SIGNATURE rather than from XML.
	//
	// Same reasoning as everywhere else on this title: HaloCampaignEvolved.exe carries no version resource, so
	// every build reports 0.0.0.0 and a stale address in XML could not be detected - it would just call into
	// whatever now occupies that offset, with a game-object pointer in RCX. Zero or more than one match leaves
	// the re-arm disabled; the clamp itself is unaffected, so the feature degrades rather than breaking.
	//
	// The signature is the function prologue at exe rva 0x95B10E0, wildcarding its two rel32 call targets.
	void resolveAddOccupant()
	{
		if (gAddOccupantFunction.load(std::memory_order_acquire) != 0) return;

		const uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
		if (!exeBase) return;

		int hits = 0;
		const uintptr_t match = HCESignatureScan::resolveUnique(exeBase,
			"48 89 74 24 20 57 48 83 EC 50 48 8B F1 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 0F 84",
			hits);

		if (!match)
		{
			PLOG_WARNING << "HCE Sky Fix: could not locate AddOccupant by signature (" << hits
				<< " matches, exactly 1 required). The refcount clamp still works; only the "
				"'enable while already out of bounds' re-arm is unavailable.";
			return;
		}

		gAddOccupantFunction.store(match, std::memory_order_release);
		PLOG_INFO << "HCE Sky Fix: AddOccupant resolved by signature @ 0x" << std::hex << match << std::dec;
	}

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (newValue)
			{
				std::scoped_lock lock(mAttachMutex);
				if (!mVerified)
				{
					verifyOriginalBytes();
					mVerified = true;
					PLOG_INFO << "HaloCER sky fix site matched its expected original bytes";
				}
				resolveAddOccupant();

				// The hook site IS RemoveOccupant's entry, so this is the function we call to undo a block.
				uintptr_t removeOccupant = 0;
				if (mFunction->resolve(&removeOccupant) && removeOccupant)
					gRemoveOccupantFunction.store(removeOccupant, std::memory_order_release);

				setTickWanted(true);
				mHook->setWantsToBeAttached(true);
				if (!mHook->isHookInstalled())
				{
					mHook->setWantsToBeAttached(false);
					throw HCMRuntimeException("Failed to install the Halo Campaign Evolved sky fix hook");
				}
			}
			else
			{
				mHook->setWantsToBeAttached(false);
			}

			gSkyFixEnabled.store(newValue, std::memory_order_release);

			// Arm the re-arm. Enabling while already outside cannot restore the area from here (this is not
			// the game thread - see serviceReArmRequest), so the request is consumed at the next area
			// boundary instead. Cleared on disable so a stale request cannot fire later.
			gWantReArm.store(newValue, std::memory_order_release);

			// Hand the game back every reference we injected. Strictly AFTER gSkyFixEnabled goes false and the
			// hook is detached, so nothing can re-clamp an area we have just released - and BEFORE the tick is
			// released, because the engine teardown runs on it.
			if (!newValue)
			{
				releaseAllHeldReferences("switched off");
				setTickWanted(false);
			}

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Sky Fix on. If you are ALREADY out of bounds, cross one area boundary to restore it."
					: "Sky Fix off. If you are out of bounds right now, cross one area boundary to return to normal.");
		}
		catch (HCMRuntimeException ex)
		{
			gSkyFixEnabled.store(false, std::memory_order_release);
			releaseAllHeldReferences("failed and shut itself off");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// A level unload destroys every streaming-area actor, taking our injected references with it. Forget them
	// WITHOUT writing - those pointers are freed memory, and the SEH around the release only catches an unmapped
	// page, not memory the allocator has already handed to something else.
	//
	// Keyed on the LEVEL ID changing rather than on "not currently playing". Play state dips out of Ingame for
	// things that do not destroy the areas - a checkpoint load freezes the tick counter, which HCEStateHook reports
	// as Loading - and forgetting there would silently reinstate the leak this whole change exists to remove.
	// Losing the records is not crash-safe-by-default, it is just a quieter bug, so the condition has to be the
	// one that actually invalidates the pointers.
	LevelID mLastSeenLevel = LevelID::no_map_loaded;

	void onGameStateChanged(const MCCState& newState)
	{
		if (newState.currentLevelID == mLastSeenLevel) return;

		mLastSeenLevel = newState.currentLevelID;
		forgetHeldReferences();
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	HCESkyFixImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceSkyFixToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCESkyFix only supports Halo Campaign Evolved");

		// OPTIONAL dependency, resolved in the body so a failure here degrades instead of taking the sky fix down
		// with it. The camera cheat owns the only per-frame game-thread tick on this title, which is the only way
		// to call the engine's RemoveOccupant. Without it the undo still happens, just without a real teardown -
		// see the fallback in releaseAllHeldReferences.
		try
		{
			mCameraData = resolveDependentCheat(HCEGetCameraData);
		}
		catch (HCMInitException& ex)
		{
			PLOG_WARNING << "HCE Sky Fix: could not resolve HCEGetCameraData (" << ex.what()
				<< "). Switching the fix off will still hand back its references, but cannot ask the engine to "
				"tear areas down - the sky will return to normal one boundary crossing later instead.";
		}

		// Pure pointer-data lookups; no game memory is touched here.
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		mFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceSkyFixRemoveOccupantFunction), mGame);
		mExpectedBytes = ptr->getVectorData<byte>(nameof(hceSkyFixRemoveOccupantOriginalBytes), mGame);

		// The hook is on the EXE (L"main"), not the sim dll. startEnabled = false.
		mHook = ModuleMidHook::make(L"main", mFunction, &removeOccupantMidHook, false);

		mReady.store(true, std::memory_order_release);
	}

	~HCESkyFixImpl()
	{
		mReady.store(false, std::memory_order_release);
		gSkyFixEnabled.store(false, std::memory_order_release);
		gWantReArm.store(false, std::memory_order_release);
		if (mHook) mHook->setWantsToBeAttached(false);

		// The game outlives HCM here - closing HCM must leave it exactly as we found it. Detaching the hook alone
		// used to leave our injected references behind, permanently pinning every area we had blocked. Order
		// matters: flag cleared and hook detached first, so no clamp can race this; the tick is released only
		// afterwards, because the engine teardown runs on it.
		releaseAllHeldReferences("shutting down");

		// ⚠ MUST happen before this DLL unmaps - the tick holds a raw pointer to skyFixGameThreadTick.
		setTickWanted(false);

		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCESkyFix::HCESkyFix(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCESkyFixImpl>(game, dicon))
{
}

// See the declaration in HCESkyFix.h. Just raises the flag; skyFixGameThreadTick does the work on the game thread.
void HCESkyFix::requestAreaReArm()
{
	gWantReArm.store(true, std::memory_order_release);
}

HCESkyFix::~HCESkyFix()
{
	PLOG_VERBOSE << "~" << getName();
}
