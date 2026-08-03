#include "pch.h"
#include "FindProcess.h"
#include <chrono>
#include <array>
#include <string>
#include <WinUser.h>
#include <TlHelp32.h>

#include <algorithm>>

std::optional<std::chrono::time_point<std::chrono::system_clock>> lastMCCExitTime = std::nullopt;



// Halo Campaign Evolved (a separate UE5 title, not MCC) - found BY PROCESS NAME rather than window title:
// its window caption is not a stable/known constant the way MCC's is. Same handle permissions as the MCC path,
// so the injector downstream is identical (proven on this exe by HceHavokDebugger).
static std::optional<HandlePtr> findValidCampaignEvolvedProcess()
{
	HandlePtr snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return std::nullopt;

	PROCESSENTRY32 process;
	ZeroMemory(&process, sizeof(process));
	process.dwSize = sizeof(process);

	if (Process32First(snapshot.get(), &process))
	{
		do
		{
			if (_wcsicmp(process.szExeFile, L"HaloCampaignEvolved.exe") == 0)
			{
				HandlePtr hceHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, NULL, process.th32ProcessID));
				if (!hceHandle)
				{
					PLOG_ERROR << "Found HaloCampaignEvolved.exe but failed to open it (PROCESS_QUERY_LIMITED_INFORMATION)";
					return std::nullopt;
				}
				PLOG_DEBUG << "Found Halo Campaign Evolved process, pid: " << process.th32ProcessID;
				return hceHandle;
			}
		} while (Process32Next(snapshot.get(), &process));
	}

	return std::nullopt;
}

// Returns a HandlePtr (with QueryLimitedInformation perms) to an active MCC process,
// or nothing if there are no MCC processes or they are all invalid (terminated/terminating)
std::optional<HandlePtr> findValidMCCProcess()
{
	// check if MCC exited too recently
	if (lastMCCExitTime)
	{
		auto now = std::chrono::system_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMCCExitTime.value()).count() < 5000) // 5 seconds
		{
			PLOG_DEBUG << "MCC exited too recently, waiting";
			return std::nullopt;
		}
	}

	// find mcc window
	HWND mccWindowHandle = FindWindowA("UnrealWindow", "Halo: The Master Chief Collection  "); // I tested Japanese windows language and that didn't change the window title so we should be okay.
	if (!mccWindowHandle)
		return findValidCampaignEvolvedProcess(); // no MCC -> is Halo Campaign Evolved running instead?


	DWORD mccPID;
	DWORD threadID = GetWindowThreadProcessId(mccWindowHandle, &mccPID);
	if (!threadID)
	{
		PLOG_ERROR << "Failed to get process ID from mcc window handle; extended error info: " << GetLastError();
		return std::nullopt;
	}

	HandlePtr mccHandle (OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, NULL, mccPID));
	if (!mccHandle)
	{
		PLOG_ERROR << "Failed to open MCC process with permission PROCESS_QUERY_LIMITED_INFORMATION";
		return std::nullopt; // permission issue or zombie process? hmmm
	}

	return mccHandle;

}

// returns true if EasyAnticheat process is detected
bool anticheatIsEnabled()
{
	// just enumurate processes and search for name EasyAntiCheat.exe

	// Create toolhelp snapshot.
	HandlePtr snapshot (CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	PROCESSENTRY32 process;
	ZeroMemory(&process, sizeof(process));
	process.dwSize = sizeof(process);

	// Walkthrough all processes (until finding EAC that is)
	if (Process32First(snapshot.get(), &process))
	{
		do
		{
			if (_wcsicmp(process.szExeFile, L"EasyAntiCheat.exe") == 0)
			{
				return true;
			}
		} while (Process32Next(snapshot.get(), &process));
	}

	return false;

}

bool hasMCCTerminated(HandlePtr& mccHandle)
{
	DWORD exitCode = 0;

	if (GetExitCodeProcess(mccHandle.get(), &exitCode) == FALSE)
	{
		PLOG_ERROR << "GetExitCodeProcess failed, error code: " << GetLastError();
		lastMCCExitTime = std::chrono::system_clock::now();
		return true;
	}

	if (exitCode != STILL_ACTIVE)
	{
		PLOG_INFO << "MCC terminated with code: " << exitCode;
		lastMCCExitTime = std::chrono::system_clock::now();
		return true;
	}

	return false;

}