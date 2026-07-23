#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IMCCStateHook.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "Datum.h"
#include "GetPlayerDatum.h"
#include "GetObjectAddress.h"
#include "IMakeOrGetCheat.h"


class GetPlayerDatumPresenter : public IOptionalCheat
{
private:
	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback<ActionEvent> getPlayerDatumEventCallback;
	ScopedCallback<ActionEvent> getPlayerAddressEventCallback;



	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;
	std::weak_ptr<GetPlayerDatum> getPlayerDatumWeak;
	std::weak_ptr<GetObjectAddress> getObjectAddressWeak;

	// "Get Player Datum": resolve the local player's object datum, drop it into the copyable box (+ clipboard/message).
	void onGetPlayerDatumEvent()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
				return;

			lockOrThrow(getPlayerDatumWeak, getPlayerDatum)
			auto playerDatum = getPlayerDatum->getPlayerDatum();
			std::string datumStr = playerDatum.toString();

			if (auto settings = mSettingsWeak.lock())
				settings->getPlayerDatumResult->GetValueDisplay() = datumStr; // populate the copyable box

			lockOrThrow(messagesWeak, messages);
			messages->addMessage(std::format("Player Datum: 0x{}", datumStr));
			ImGui::SetClipboardText(datumStr.c_str());
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// "Get Player Address": resolve the local player's object datum -> its object (heap) address, drop it into the
	// copyable box (+ clipboard/message). Saves the user from copying the datum and pasting it into "Get Object Address".
	void onGetPlayerAddressEvent()
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
				return;

			lockOrThrow(getPlayerDatumWeak, getPlayerDatum);
			lockOrThrow(getObjectAddressWeak, getObjectAddress);

			auto playerDatum = getPlayerDatum->getPlayerDatum();
			uintptr_t objectAddress = getObjectAddress->getObjectAddress(playerDatum);

			std::stringstream ss;
			ss << std::hex << std::uppercase << objectAddress;
			std::string addrStr = ss.str();

			if (auto settings = mSettingsWeak.lock())
				settings->getPlayerAddressResult->GetValueDisplay() = addrStr; // populate the copyable box

			lockOrThrow(messagesWeak, messages);
			messages->addMessage(std::format("Player Address: 0x{}", addrStr));
			ImGui::SetClipboardText(addrStr.c_str());
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}




public:

	GetPlayerDatumPresenter(GameState game, IDIContainer& dicon)
		: mGame(game),
		getPlayerDatumEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->getPlayerDatumEvent, [this]() {onGetPlayerDatumEvent(); }),
		getPlayerAddressEventCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->getPlayerAddressEvent, [this]() {onGetPlayerAddressEvent(); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mSettingsWeak(dicon.Resolve<SettingsStateAndEvents>())

	{
		PLOG_VERBOSE << "constructing GetPlayerDatumPresenter OptionalCheat for game: " << mGame.toString();
		getPlayerDatumWeak = resolveDependentCheat(GetPlayerDatum);
		// GetObjectAddress powers "Get Player Address" only; if it can't construct for this game, keep the datum
		// button working and just let the address button fail gracefully (its handler locks this weakptr).
		try { getObjectAddressWeak = resolveDependentCheat(GetObjectAddress); }
		catch (HCMInitException& ex) { PLOG_ERROR << "GetPlayerDatumPresenter: GetObjectAddress unavailable, 'Get Player Address' disabled: " << ex.what(); }
	}

	virtual std::string_view getName() override {
		return nameof(GetPlayerDatumPresenter);
	}

	~GetPlayerDatumPresenter()
	{
		PLOG_VERBOSE << "~" << getName();
	}

};
