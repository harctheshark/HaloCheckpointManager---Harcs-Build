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

			// Clamp rather than increment: the game's own `sub dword ptr [rdi+2D8h], 1` two instructions later
			// consumes the bump, so this converges on exactly 1 however many exits occur. A blind ++ would
			// drift upward once per enter/exit cycle.
			*refcount = 2;
			*outClamped = true;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void removeOccupantMidHook(SafetyHookContext& ctx) noexcept
	{
		if (!gSkyFixEnabled.load(std::memory_order_acquire)) return;
		if (ctx.rcx == 0) return;

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

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Sky Fix on. If you are ALREADY out of bounds, walk back in once."
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
