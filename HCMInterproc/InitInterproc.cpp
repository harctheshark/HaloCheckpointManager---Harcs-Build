#include "pch.h"
#include "InitInterproc.h"
#include "HookStateMachine.h"
#include "SharedMemoryExports.h"

bool alreadyInitialised = false;
bool resetStateMachineFlag = false;

// ================================================================================================================
// SHUTDOWN. See shutdownInterproc() for why this exists.
//
// ⚠ THIS LOOP USED TO BE `while (true)` WITH NO EXIT AND NO SHUTDOWN HOOK, AND THAT WAS THE BUG. When HCMExternal
// closes, HCMInternal's heartbeat sees the external going away and sets Shutdown. The external's state machine
// thread is still running, reads Shutdown, walks InternalSuccess -> MCCNotFound -> InternalInjecting, and
// INJECTS A FRESH HCMInternal into the game as the external process dies. That orphan cannot find HCMExternal,
// throws during construction, and its error path puts up a blocking MessageBox on the game's thread behind a
// fullscreen game - so it never reaches FreeLibraryAndExitThread and the DLL stays resident. Reopening HCM then
// either refuses ("a previous HCMInternal is still loaded") or LoadLibrary merely bumps the refcount, DllMain
// never runs, and the state machine waits forever for a status flag nobody will ever write.
//
// On HaloCER this fires constantly, because the game destroys and recreates its window mid-session, which trips
// HCMInternal's own WM_DESTROY kill and starts the whole sequence without HCM ever being closed.
// ================================================================================================================
std::atomic_bool gShuttingDown{ false };

void stateMachineLoop()
{
	HookStateMachine stateMachine;
	while (!gShuttingDown.load(std::memory_order_acquire))
	{
		if (resetStateMachineFlag)
		{
			resetStateMachineFlag = false;
			stateMachine.reset();
		}
		stateMachine.update();

		// Sleep in slices so shutdown is observed within ~100ms rather than up to a second - the window in
		// which an injection can still be started is exactly this sleep.
		for (int i = 0; i < 10 && !gShuttingDown.load(std::memory_order_acquire); ++i)
			Sleep(100);
	}
	PLOG_INFO << "interproc state machine loop exited cleanly";
}

uint16_t initialiseInterproc(
	bool CPnullData,
	int CPgame, const char* CPname, const char* CPpath, const char* CPlevelcode, const char* CPgameVersion, int CPdifficulty,
	int SFgame, const char* SFname, const char* SFpath
)
{
	PLOG_DEBUG << "initialising interproc";


	if (alreadyInitialised)
	{
		PLOG_ERROR << "reinitialisation? weird";
		return g_SharedMemoryExternal.operator bool();
	}

	else
		alreadyInitialised = true;


	PLOG_INFO << "Attempting to init shared memory";
	try
	{
		g_SharedMemoryExternal = std::make_unique<SharedMemoryExternal>(CPnullData,
			CPgame, CPname, CPpath, CPlevelcode, CPgameVersion, CPdifficulty,
			SFgame, SFname, SFpath);
		PLOG_INFO << "Success!";
	}
	catch (std::exception ex)
	{
		PLOG_ERROR << "Failure! " << ex.what();
		return false;
	}

	// Begin state machine loop on new thread
	CreateThread(0, 0x1000, (LPTHREAD_START_ROUTINE)stateMachineLoop, NULL, 0, NULL);
	return true;
	

}

void resetStateMachine()
{
	resetStateMachineFlag = true;
}

// Called from HCMExternal's Application_Exit. Must be safe to call more than once, and must never block for
// long - the process is on its way out and Windows will not wait forever.
void shutdownInterproc()
{
	if (gShuttingDown.exchange(true, std::memory_order_acq_rel)) return;
	PLOG_INFO << "shutdownInterproc: state machine will stop; no further injection will be attempted";
}

bool interprocIsShuttingDown()
{
	return gShuttingDown.load(std::memory_order_acquire);
}
