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
		// ⚠⚠ "CANNOT SEE HCMExternal" IS NOT THE SAME AS "HCMExternal IS GONE".
		// From an APPCONTAINER host it is simply impossible to tell this way: a sandboxed process can
		// neither enumerate desktop processes nor OpenProcess one, so findProcess returns 0 and OpenProcess
		// fails with ERROR_INVALID_PARAMETER. Halo 5: Forge is such a host, and treating that as orphanhood
		// made every session initialise fully and then kill itself ~3s later.
		//
		// So before standing down, ask shared memory - which we are already connected to, and which the
		// external bumps once per state machine tick. If that counter exists, the external is alive and we
		// run in HEARTBEAT MODE (below) instead of handle mode.
		bool shmAlive = false;
		if (auto shm = sharedMemWeak.lock())
			shmAlive = shm->getExternalHeartbeat().has_value();

		if (!shmAlive)
		{
			PLOG_WARNING << "HeartbeatTimer: no HCMExternal process after 3s (last error " << GetLastError()
				<< ") and no heartbeat in shared memory. This instance is an orphan - unloading quietly "
				"rather than failing loudly, so the DLL does not stay pinned in the game and block the "
				"next launch.";
			GlobalKill::killMe();
			return;   // no handle, no heartbeat thread; the kill flag unwinds App and the DLL unloads
		}

		PLOG_INFO << "HeartbeatTimer: cannot open the HCMExternal process (expected in a sandboxed host such "
			"as Halo 5: Forge - an AppContainer cannot open desktop processes). Shared memory carries a live "
			"heartbeat, so this session is NOT an orphan; watching the heartbeat instead.";
		// fall through with a null handle - the thread below switches to heartbeat mode
	}

	HCMExternalHandle = std::move(h);   // may legitimately be null in heartbeat mode

	_thd = std::thread([this]()
		{
			static int waitCount = 0;

			// Heartbeat mode state (only meaningful when HCMExternalHandle is null - see the constructor).
			// We cannot detect a hard kill instantly the way GetExitCodeProcess does, so instead we require
			// the counter to keep moving. The external bumps it once per state machine tick (~1s); allow a
			// generous stall before concluding it is gone, because the external's tick can be delayed by a
			// slow injection attempt or by the user dragging its window.
			const bool heartbeatMode = (HCMExternalHandle.get() == nullptr);
			int lastHeartbeat = -1;
			auto lastHeartbeatChange = std::chrono::steady_clock::now();
			constexpr auto heartbeatStallLimit = std::chrono::seconds(15);

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



					if (heartbeatMode)
					{
						// ⚠ NO PROCESS HANDLE HERE - DO NOT "FIX" THIS BY CALLING GetExitCodeProcess ANYWAY.
						// On a null handle it fails, the old code read that as "external died" and killed the
						// session immediately, which is the exact orphan-suicide this mode exists to avoid.
						std::optional<int> beat;
						try
						{
							lockOrThrow(sharedMemWeak, sharedMem);
							beat = sharedMem->getExternalHeartbeat();
						}
						catch (HCMRuntimeException)
						{
							// shared memory gone entirely - that IS the external going away
							PLOG_INFO << "HCMExternal's shared memory is gone; shutting down.";
							GlobalKill::killMe();
							return;
						}

						auto now = std::chrono::steady_clock::now();
						if (beat.has_value() && beat.value() != lastHeartbeat)
						{
							lastHeartbeat = beat.value();
							lastHeartbeatChange = now;
						}
						else if (now - lastHeartbeatChange > heartbeatStallLimit)
						{
							PLOG_INFO << "HCMExternal's heartbeat stalled at " << lastHeartbeat << " for over "
								<< std::chrono::duration_cast<std::chrono::seconds>(heartbeatStallLimit).count()
								<< "s; assuming it is gone and shutting down.";
							GlobalKill::killMe();
							return;
						}
					}
					else
					{
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