#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"


class ForceRevert : public IOptionalCheat
{
private:
	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback <ActionEvent>mForceRevertCallbackHandle;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	//data
	std::shared_ptr<MultilevelPointer> forceRevertFlag;
	std::shared_ptr<MultilevelPointer> revertQueuedFlag; // only used/non-null for Halo1

	// primary event callback
	void onForceRevert()
	{

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;
			PLOG_DEBUG << "Force Revert called";

			if (mGame.operator GameState::Value() == GameState::Value::HaloCER)
			{
				// Halo Campaign Evolved writes a 4 byte int, NOT a byte, deliberately.
				// The reference tool (HCM_Evolved checkpoints.py force_revert -> pymem write_int) writes
				// int 1 at HaloSimulation_tag_release.dll+0x135706A, which spans 0x135706A..0x135706D. The
				// last of those bytes is the global the GAME reads in its checkpoint "gate2" predicate
				// (cmp byte [0x135706D],0 at 0x1AE0A2), so the wide write additionally sets that permission
				// byte as a side effect. Note HCECheckpointDetours never reads or writes 0x135706D - it steps
				// over that cmp instead - so the two features do not interact through this address.
				// The 4 byte width is part of the known-good observed behaviour of the tool this was ported
				// from, so it is reproduced exactly rather than narrowed to a single byte.
				//
				// RESIDUAL RISK: unlike HCECheckpointDetours, this write is not byte-verified against a known
				// build (requiredServicesPerGUIElement is not game-keyed, so there is no clean place to hang a
				// HaloCER-only check). HaloSimulation_tag_release.dll has no version resource, so if the game
				// updates, this silently writes 4 bytes to whatever now lives at +0x135706A. Re-derive the
				// address from HCM_Evolved's addresses.json when bumping HCE support.
				int32_t enableFlag = 1;
				if (!forceRevertFlag->writeData(&enableFlag)) throw HCMRuntimeException(std::format("Failed to write Revert flag {}", MultilevelPointer::GetLastError()));
				messagesGUI->addMessage("Revert forced.");
				return;
			}

			byte enableFlag = 1;
			if (!forceRevertFlag->writeData(&enableFlag)) throw HCMRuntimeException(std::format("Failed to write Revert flag {}", MultilevelPointer::GetLastError()));

			if (mGame.operator GameState::Value() == GameState::Value::Halo1)
			{
				byte clearQueueFlag = 0;
				if (!revertQueuedFlag->writeData(&clearQueueFlag)) PLOG_ERROR << "Failed to clear revertQueuedFlag, error: " << MultilevelPointer::GetLastError();
			}

			messagesGUI->addMessage("Revert forced.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}

	}



public:
	ForceRevert(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl), 
		mForceRevertCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->forceRevertEvent, [this]() { onForceRevert(); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()), 
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		forceRevertFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(forceRevertFlag), mGame);

		if (gameImpl.operator GameState::Value() == GameState::Value::Halo1)
		{
			revertQueuedFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(revertQueuedFlag), mGame);
		}
	}

	~ForceRevert()
	{
		PLOG_VERBOSE << "~" << getName();
	}

	std::string_view getName() override { return "Force Revert"; }

};