#include "pch.h"
#include "H5Checkpoint.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5Checkpoint.h for the mechanism and the two hard constraints (gate-before-request, campaign only).

namespace
{
	// RVAs into halo5forge.exe, derived live.
	constexpr uintptr_t kRvaRevertGate    = 0x50B0251;   // |= 0x80, FIRST
	constexpr uintptr_t kRvaRevertRequest = 0x50B03AC;   // |= 0x0C, second

	// SEH only - no C++ objects with destructors in here (MSVC C2712).
	bool sehOrByte(uintptr_t addr, uint8_t bits)
	{
		__try
		{
			*(volatile uint8_t*)addr = (uint8_t)(*(volatile uint8_t*)addr | bits);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}


// ------------------------------------------------------------------------------------------------ checkpoint
class H5ForceCheckpoint::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::atomic<bool> mReady{ false };

	void onForceCheckpoint()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			// mode 3 = immediate. 1 = with timeout, 2 = no_timeout (never auto-consumes), 4 = cinematic_skip.
			playerState->requestCheckpoint(3);

			if (!playerState->isLocalSimulation())
				messagesGUI->addMessage("Checkpoint requested, but this is not a local simulation - "
					"the request will be consumed and store nothing.");
			else
				messagesGUI->addMessage("Checkpoint saved");
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error forcing checkpoint: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceCheckpointEvent, [this]() { onForceCheckpoint(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5ForceCheckpoint only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mCallback.removeCallback();
	}
};

H5ForceCheckpoint::H5ForceCheckpoint(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5ForceCheckpoint::~H5ForceCheckpoint() { PLOG_VERBOSE << "~" << getName(); }


// ---------------------------------------------------------------------------------------------------- revert
class H5ForceRevert::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::atomic<bool> mReady{ false };

	void onForceRevert()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			lockOrThrow(playerStateWeak, playerState);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			// ⚠ REFUSE, do not warn. As a distributed client this tears the player down with nothing to
			// restore - a far worse outcome than doing nothing.
			if (!playerState->isLocalSimulation())
			{
				messagesGUI->addMessage("Force Revert is campaign-only in Halo 5 "
					"(this session is not the simulation authority). Refused.");
				return;
			}

			const uintptr_t exeBase = playerState->getExeBase();
			// Gate FIRST - without it the request is never consumed.
			if (!sehOrByte(exeBase + kRvaRevertGate, 0x80))
				throw HCMRuntimeException("Could not write the Halo 5 revert gate");
			if (!sehOrByte(exeBase + kRvaRevertRequest, 0x0C))
				throw HCMRuntimeException("Could not write the Halo 5 revert request");

			messagesGUI->addMessage("Reverted");
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error forcing revert: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	ScopedCallback<ActionEvent> mCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceRevertEvent, [this]() { onForceRevert(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5ForceRevert only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mCallback.removeCallback();
	}
};

H5ForceRevert::H5ForceRevert(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5ForceRevert::~H5ForceRevert() { PLOG_VERBOSE << "~" << getName(); }
