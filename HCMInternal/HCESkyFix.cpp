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
#include <atomic>

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

		int32_t before = 0;
		bool clamped = false;
		if (!tryClampOccupantRefcount(ctx.rcx, &before, &clamped))
		{
			// A bad actor pointer must not take the game down. The byte guard makes this near-impossible, but
			// being wrong here would cost the user their run, so fail closed rather than keep writing.
			LOG_ONCE(PLOG_ERROR << "HCE Sky Fix: faulted accessing the occupant refcount - disabling.");
			gSkyFixEnabled.store(false, std::memory_order_release);
			return;
		}

		if (!clamped)
		{
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

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"The HaloCER sky fix site did not match its expected original bytes - this build of Halo "
				"Campaign Evolved is not supported. Expected [{}], found [{}]",
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

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Sky Fix on. If you are ALREADY out of bounds, cross one area boundary to restore it."
					: "Sky Fix off.");
		}
		catch (HCMRuntimeException ex)
		{
			gSkyFixEnabled.store(false, std::memory_order_release);
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	HCESkyFixImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceSkyFixToggle->valueChangedEvent, [this](bool& n) { onToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCESkyFix only supports Halo Campaign Evolved");

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
		if (mHook) mHook->setWantsToBeAttached(false);
		mToggleCallback.removeCallback();
	}
};


HCESkyFix::HCESkyFix(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCESkyFixImpl>(game, dicon))
{
}

HCESkyFix::~HCESkyFix()
{
	PLOG_VERBOSE << "~" << getName();
}
