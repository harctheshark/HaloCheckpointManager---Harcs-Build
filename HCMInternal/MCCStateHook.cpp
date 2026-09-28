#include "pch.h"
#include "MCCStateHook.h"


void MCCStateHook::updateMCCState()
{
	PLOG_DEBUG << "updating MCC State";
	try
	{
		byte currentGameEngineUnparsed = 255;
		if (!instance->gameEngineIndicator->readData(&currentGameEngineUnparsed))
		{
			throw  HCMRuntimeException(std::format("Failed to read gameEngineIndicator: {}", MultilevelPointer::GetLastError()));
		}
		PLOG_DEBUG << "read current game engine: " << (int)currentGameEngineUnparsed;
		uintptr_t pGameEngineIndicator;
		gameEngineIndicator->resolve(&pGameEngineIndicator);
		PLOG_VERBOSE << "gameEngineIndicator read @0x" << std::hex << (int64_t)pGameEngineIndicator;

		GameState currentGameEngine{ magic_enum::enum_cast<GameState::Value>(currentGameEngineUnparsed).value_or(GameState::Value::NoGame) };

		PLOG_DEBUG << "read current game engine (parsed): " << currentGameEngine.toString();

		bool loadingFlag;
		if (!instance->loadIndicator->readData(&loadingFlag))
		{
			throw  HCMRuntimeException(std::format("Failed to read loadIndicator: {}", MultilevelPointer::GetLastError()));
		}
		PLOG_DEBUG << "read loadingFlag: " << loadingFlag;

		bool menuFlag;
		if (!instance->menuIndicator->readData(&menuFlag))
		{
			throw  HCMRuntimeException(std::format("Failed to read menuIndicator: {}", MultilevelPointer::GetLastError()));
		}
		PLOG_DEBUG << "read menuFlag: " << menuFlag;
		menuFlag = !menuFlag; // need to flip it to be the right way around, the read value is actually equal to false when in the menu.

		// load flag takes priority over mainmenu flag
		PlayState currentPlayState = loadingFlag ? PlayState::Loading : (menuFlag ? PlayState::MainMenu : PlayState::Ingame);
		PLOG_DEBUG << "read playstate: " << magic_enum::enum_name(currentPlayState);
		
		byte currentLevelIDUnparsed;
		if (!instance->levelIndicator->readData(&currentLevelIDUnparsed))
		{
			throw  HCMRuntimeException(std::format("Failed to read levelIndicator: {}", MultilevelPointer::GetLastError()));
		}
		PLOG_DEBUG << "read level ID: " << (int)currentLevelIDUnparsed;

		LevelID currentLevelID{ magic_enum::enum_cast<LevelID>(currentLevelIDUnparsed).value_or(LevelID::no_map_loaded) };
		PLOG_DEBUG << "read level ID (parsed): " << magic_enum::enum_name(currentLevelID);

		// update current state
		currentMCCState = { currentGameEngine, currentPlayState, currentLevelID };
		PLOG_DEBUG << "state updated";

		// fire event
		MCCStateChangedEvent->operator()(currentMCCState);
		PLOG_DEBUG << "event fired";
	}
	catch(HCMRuntimeException ex)
	{
		ex.prepend("An error occured while evaluating new MCC state: \n");
		runtimeExceptions->handleMessage(ex);
	}
	// This runs inside a safetyhook midhook on MCC's own thread, and ~60 subscribers run inside the
	// MCCStateChangedEvent call above (eventpp does not catch). Any std:: exception from any of them used to
	// escape into MCC and kill it - that is exactly how the Halo 2 Anniversary MP (groundhog) crash happened
	// (std::out_of_range from a map .at(game)). Contain everything here.
	catch (const std::exception& e)
	{
		HCMRuntimeException converted(std::format("MCC state change subscriber threw {}: {}", typeid(e).name(), e.what()));
		runtimeExceptions->handleMessage(converted);
	}
	catch (...)
	{
		PLOG_ERROR << "Unknown (non std::exception) exception while evaluating new MCC state - swallowed so it cannot take MCC down";
	}
}


