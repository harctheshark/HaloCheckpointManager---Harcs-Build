#include "pch.h"
#include "HeartbeatTimer.h"
#include "GlobalKill.h"
#include <winternl.h>
#include <TlHelp32.h>

DWORD findProcess(std::wstring targetProcessName)
{


	// Get a snapshot of running processes
	HandlePtr hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL));

	// Success check on the snapshot tool.
	if (hSnap.get() == INVALID_HANDLE_VALUE) {
		throw HCMInitException(std::format("Failed to get snapshot of running processes: {}", GetLastError()).c_str());
	}

	// PROCESSENTRY32 is used to open and get information about a running process..
	PROCESSENTRY32 entry;
	entry.dwSize = sizeof(PROCESSENTRY32);

	// If a first process exist (there are running processes), iterate through
	// all running processes.
	if (Process32First(hSnap.get(), &entry)) {
		do
		{

			// If the current process entry is the target process, return its ID (ignore case)
			if (_wcsicmp(entry.szExeFile, targetProcessName.c_str()) == 0)
			{
				return entry.th32ProcessID;
			}

		} while (Process32Next(hSnap.get(), &entry));        // Move on to the next running process.
	}


	return 0;
}


HeartbeatTimer::HeartbeatTimer(std::weak_ptr<SharedMemoryInternal> shm, std::weak_ptr<SettingsStateAndEvents> set)
	: sharedMemWeak(shm), settingsWeak(set)
{
	// PROCESS_QUERY_LIMITED_INFORMATION allows non-admin HCMInternal(mcc) to use GetExitCodeProcess on admin HCMExternal.
	// PROCESS_QUERY_INFORMATION is a denied permission in that scenario.
	//
	// ⚠ FAILING TO FIND HCMExternal MUST NOT THROW. An instance that starts with no external alive is an
	// ORPHAN - it was injected by an HCMExternal that has since exited - and the only correct thing for it to
	// do is quietly unload. Throwing HCMInitException instead routes it into App's catch block, which used to
	// put up a BLOCKING MessageBox on the game's thread behind a fullscreen game; until somebody found and
	// dismissed that box, FreeLibraryAndExitThread never ran and HCMInternal.dll stayed pinned in the game,
	// which is what made the NEXT launch of HCM fail. Retry briefly (the external may still be starting), then
	// stand down cleanly.
	HandlePtr h(nullptr);
	for (int attempt = 0; attempt < 6 && !h; ++attempt)
	{
		if (attempt) Sleep(500);

		DWORD pid = findProcess(L"HCMExternal.exe");
		h = HandlePtr(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, TRUE, pid));
		if (h) break;

		pid = findProcess(L"HaloCheckpointManager.exe");
		h = HandlePtr(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, TRUE, pid));
	}

	if (!h)
	{
		PLOG_WARNING << "HeartbeatTimer: no HCMExternal process after 3s (last error " << GetLastError()
			<< "). This instance is an orphan - unloading quietly rather than failing loudly, so the DLL "
			"does not stay pinned in the game and block the next launch.";
		GlobalKill::killMe();
		return;   // no handle, no heartbeat thread; the kill flag unwinds App and the DLL unloads
	}


	HCMExternalHandle = std::move(h);

	_thd = std::thread([this]()
		{
			static int waitCount = 0;
			while (!GlobalKill::isKillSet())
			{
				waitCount = (waitCount + 1) % 100;
				if (waitCount != 0)
				{
					// process inject command queue
					try
					{
						lockOrThrow(settingsWeak, settings);
						lockOrThrow(sharedMemWeak, sharedMem);

						if (sharedMem->getAndClearInjectQueue())
						{
							PLOG_DEBUG << "Inject command recieved, firing event!";
							settings->injectCheckpointEvent->operator()();
						}
					}
					catch (HCMRuntimeException ex)
					{
						PLOG_ERROR << "Heartbeat timer unable to check inject command queue";
					}



					DWORD exitCode = 0;
					if (GetExitCodeProcess(HCMExternalHandle.get(), &exitCode) == FALSE)
					{
						PLOG_ERROR << "GetExitCodeProcess failed, error code: " << GetLastError();
						GlobalKill::killMe();
						return;
					}
					else
					{
						// https://youtu.be/APc8QCGOdUE
						if (exitCode != STILL_ACTIVE)
						{
							PLOG_INFO << "HCMExternal terminated with code: " << exitCode;
							GlobalKill::killMe();
							return;
						}
					}
				}




				auto nextWakeup = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
				std::this_thread::sleep_until(nextWakeup);
			}
		});
}

HeartbeatTimer::~HeartbeatTimer()
{
	PLOG_VERBOSE << "~HeartbeatTimer";

	// ⚠ joinable() IS LOAD-BEARING, NOT DEFENSIVE PADDING. The constructor has an early-return path (see the
	// orphan handling above) that never starts the thread, leaving _thd default-constructed. join() on a
	// non-joinable thread throws std::system_error - and a destructor is implicitly noexcept, so that throw
	// goes straight to std::terminate and KILLS THE GAME. It would fire on precisely the scenario the
	// constructor's early return exists to make safe.
	if (_thd.joinable())
	{
		_thd.join();
		PLOG_VERBOSE << "aye the thread died as it should";
	}
	else
	{
		PLOG_VERBOSE << "heartbeat thread was never started (orphan instance); nothing to join";
	}
}