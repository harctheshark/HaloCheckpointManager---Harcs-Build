#pragma once
#include "pch.h"
extern "C"
{
	// Needs parameters for default selected checkpoint settings for shared memory. Then begins the HookStateMachine loop.
	__declspec(dllexport) extern uint16_t initialiseInterproc(
		bool CPnullData,
		int CPgame, const char* CPname, const char* CPpath, const char* CPlevelcode, const char* CPgameVersion, int CPdifficulty,
		int SFgame, const char* SFname, const char* SFpath
	);

	_declspec(dllexport) void resetStateMachine();

	// Stops the state machine loop and guarantees no further injection is attempted. HCMExternal MUST call
	// this from Application_Exit: without it, the dying external injects an orphan HCMInternal that pins the
	// DLL in the game and makes the next launch fail. See the comment block in InitInterproc.cpp.
	// Safe to call repeatedly.
	_declspec(dllexport) void shutdownInterproc();

	// True once shutdownInterproc() has been called. The state machine checks this immediately before it
	// injects, because the shutdown can land during the loop's sleep.
	_declspec(dllexport) bool interprocIsShuttingDown();
}

