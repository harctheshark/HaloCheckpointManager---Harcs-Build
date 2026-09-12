#include "pch.h"
#include "ImbueInternal.h"
#include "Events.h"
#include "WinHandle.h"
#include "SharedMemoryExternal.h"
#include "WindowsUtilities.h"
#include <winternl.h>
#include <Psapi.h>
#include <processthreadsapi.h>
#include <fstream>
#include <vector>



class InjectionException : public std::exception {
private:
	std::string message;
public:
	InjectionException(std::string msg) : message(msg) {
		PLOG_ERROR << msg;
	}
	std::string what() { return message; }
	void append(std::string ap) { message = message + ap; }
};


// Forward declaration


void InjectModule(DWORD pid, std::string dllFilePath);
bool processContainsModule(DWORD pid, std::wstring moduleName);


constexpr auto dllName = "HCMInternal";
constexpr WCHAR wdllChars[] = L"HCMInternal.dll";


// ================================================================================================
// RE-OPENING A RESIDENT HCMInternal
//
// HCMInternal never unmaps itself any more (see dllmain.cpp): unmapping is the one operation that can
// erase another overlay's hook or crash the game, so it stays resident exactly like RTSS, the Steam
// overlay, Discord and OBS all do. Closing HCM ends the SESSION, not the module.
//
// That means LoadLibrary is only correct for the FIRST injection. On every one after it, LoadLibrary
// would just bump the reference count without re-running DllMain, so no session would start and HCM
// would sit on "Internal Initialising" forever. Instead we call the exported entry point directly, the
// same way we call LoadLibraryA: CreateRemoteThread at its address in the target.
// ================================================================================================

// Base address of `moduleName` inside `pid`, or nullptr when it is not loaded.
static HMODULE getRemoteModuleBase(DWORD pid, const std::wstring& moduleName)
{
	HMODULE hMods[1024]; DWORD cbNeeded = 0;
	HandlePtr proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, pid));
	if (!proc) return nullptr;
	if (!EnumProcessModules(proc.get(), hMods, sizeof(hMods), &cbNeeded)) return nullptr;
	// ⚠ cbNeeded is the size REQUIRED, not the size written. This process has Streamline, DLSS-G, RTSS,
	// OBS, Discord and Steam in it, so >1024 modules is not far-fetched, and iterating
	// cbNeeded/sizeof(HMODULE) would then walk off the end of the array.
	const DWORD moduleCount = (cbNeeded < (DWORD)sizeof(hMods) ? cbNeeded : (DWORD)sizeof(hMods)) / sizeof(HMODULE);
	for (unsigned i = 0; i < moduleCount; i++)
	{
		TCHAR name[MAX_PATH];
		if (GetModuleBaseName(proc.get(), hMods[i], name, MAX_PATH) && std::wstring(name) == moduleName)
			return hMods[i];
	}
	return nullptr;
}

// RVA of an exported function, read from the DLL ON DISK.
// ⚠ We cannot GetProcAddress into another process, and LoadLibrary-ing HCMInternal here to ask would run
// its DllMain inside HCMExternal. So parse the export directory out of the file instead.
static DWORD getExportRva(const std::string& dllPath, const char* exportName)
{
	std::ifstream f(dllPath, std::ios::binary);
	if (!f) return 0;
	std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (buf.size() < sizeof(IMAGE_DOS_HEADER)) return 0;
	auto* dos = (IMAGE_DOS_HEADER*)buf.data();
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
	if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > buf.size()) return 0;
	auto* nt = (IMAGE_NT_HEADERS64*)(buf.data() + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

	// The file is not mapped, so every RVA has to be walked back to a raw file offset by section.
	auto rvaToOffset = [&](DWORD rva) -> DWORD
	{
		auto* sec = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
			if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize)
				return rva - sec->VirtualAddress + sec->PointerToRawData;
		return 0;
	};
	const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (!dir.VirtualAddress) return 0;
	DWORD expOff = rvaToOffset(dir.VirtualAddress);
	if (!expOff || expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > buf.size()) return 0;
	auto* exp = (IMAGE_EXPORT_DIRECTORY*)(buf.data() + expOff);
	DWORD namesOff = rvaToOffset(exp->AddressOfNames);
	DWORD ordsOff = rvaToOffset(exp->AddressOfNameOrdinals);
	DWORD funcsOff = rvaToOffset(exp->AddressOfFunctions);
	if (!namesOff || !ordsOff || !funcsOff) return 0;
	auto* names = (DWORD*)(buf.data() + namesOff);
	auto* ords = (WORD*)(buf.data() + ordsOff);
	auto* funcs = (DWORD*)(buf.data() + funcsOff);
	for (DWORD i = 0; i < exp->NumberOfNames; i++)
	{
		DWORD nOff = rvaToOffset(names[i]);
		if (!nOff || nOff >= buf.size()) continue;
		if (strcmp(buf.data() + nOff, exportName) == 0) return funcs[ords[i]];
	}
	return 0;
}

// Start a fresh session inside an HCMInternal that is already loaded. Throws InjectionException.
static void startSessionInResidentInternal(DWORD pid, HMODULE remoteBase, const std::string& dllFilePath)
{
	HandlePtr proc(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
		| PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid));
	if (!proc)
		throw InjectionException(std::format("Could not open the game process to reopen HCM: {}",
			GetErrorMessage(GetLastError())).c_str());

	// ⚠⚠⚠ PROVE THE RESIDENT MODULE IS THE FILE WE ARE ABOUT TO PARSE.
	// The base address comes from a match on module NAME inside the game; the RVA comes from the DLL
	// sitting next to THIS HCMExternal on disk. Nothing otherwise ties the two together - and permanent
	// residency makes "a build from an earlier launch is still mapped" the NORMAL state rather than a
	// corner case. Adding an RVA from the new file to the old module's base lands in the middle of some
	// unrelated function and runs it as a thread entry point.
	wchar_t residentPath[MAX_PATH]{};
	if (!GetModuleFileNameExW(proc.get(), remoteBase, residentPath, MAX_PATH))
		throw InjectionException(std::format(
			"Could not read the path of the HCMInternal already loaded in the game: {}",
			GetErrorMessage(GetLastError())).c_str());
	if (_wcsicmp(residentPath, str_to_wstr(dllFilePath).c_str()) != 0)
		throw InjectionException(std::format(
			"A different copy of HCMInternal.dll is already loaded in this game:{}{}{}This HCM is at:{}{}{}"
			"Close the game once so this build can be loaded.",
			"\n", wstr_to_str(residentPath), "\n", "\n", dllFilePath, "\n").c_str());

	const DWORD rva = getExportRva(dllFilePath, "HCMInternalStartSession");
	if (!rva)
		throw InjectionException(
			"This build of HCMInternal.dll has no session entry point, so a resident copy cannot be reopened. "
			"Close the game once and relaunch.");

	auto entry = (LPTHREAD_START_ROUTINE)((uintptr_t)remoteBase + rva);
	PLOG_INFO << "HCMInternal is already resident at 0x" << std::hex << (uintptr_t)remoteBase
		<< "; starting a new session via HCMInternalStartSession at 0x" << (uintptr_t)entry << std::dec;

	HandlePtr thread(CreateRemoteThread(proc.get(), nullptr, 0, entry, nullptr, 0, nullptr));
	if (!thread)
		throw InjectionException(std::format("Could not start a new HCM session in the game: {}",
			GetErrorMessage(GetLastError())).c_str());

	// ⚠ DO NOT WAIT ON THIS THREAD. HCMInternalStartSession runs the whole session and only returns when
	// the user closes HCM, so waiting would block the state machine for as long as HCM is open. Whether
	// the session actually came up is reported the same way a fresh injection reports it - the shared
	// memory status flag reaching AllGood.
}



// throws InjectionException on failure
void SetupInternal(DWORD mccPID)
{
	PLOG_DEBUG << "SetupInternal running";

		if (!g_SharedMemoryExternal.get())
		{
			throw InjectionException("g_SharedMemoryExternal not initialised!");
		}

		if (!g_SharedMemoryExternal->HCMInternalStatusFlag)
		{
			throw InjectionException("g_SharedMemoryExternal->HCMInternalStatusFlag was null!");
		}

		*g_SharedMemoryExternal->HCMInternalStatusFlag = (int)HCMInternalStatus::Initialising;

		CHAR buffer[MAX_PATH] = { 0 };
		GetModuleFileNameA(NULL, buffer, MAX_PATH);
		std::wstring::size_type pos = std::string(buffer).find_last_of("\\/");
		auto currentDirectory = std::string(buffer).substr(0, pos);
		if (!currentDirectory.ends_with('\\'))
			currentDirectory += '\\';

		PLOG_DEBUG << "HCMInternal path: " << currentDirectory;

		auto dllFilePath = std::string(currentDirectory + dllName + ".dll");

		// test if dll exists and can be read
		std::ifstream inFile(dllFilePath.c_str());
		if (inFile.is_open())
			inFile.close();
		else
			throw InjectionException(std::format("Could not find or read {}.dll! Error: {}", dllName, GetErrorMessage(GetLastError())).c_str());

		PLOG_DEBUG << "Found HCMInternal.dll at " << dllFilePath;

		if (!mccPID) throw InjectionException("Was passed null mcc process ID!");

		PLOG_INFO << "Given MCC process ID: 0x" << std::hex << mccPID;

		// HCMInternal stays resident for the life of the game process (see dllmain.cpp and the block at the
		// top of this file), so "already loaded" is the NORMAL state for every injection after the first -
		// not an error, and nothing to wait for. LoadLibraryA here would only bump the reference count
		// without re-running DllMain, so no session would start and HCM would sit on "Internal Initialising"
		// forever; call the exported session entry point instead.
		// ⚠ getRemoteModuleBase, NOT processContainsModule: that helper `return false`s from INSIDE its
		// enumeration loop whenever GetModuleInformation/GetModuleBaseName fails, so "the query failed" is
		// indistinguishable from "not loaded". That false negative now routes into a LoadLibraryA which
		// merely bumps the refcount and reports success, leaving HCM stuck on "Internal Initialising".
		if (HMODULE resident = getRemoteModuleBase(mccPID, std::wstring(wdllChars)))
		{
			startSessionInResidentInternal(mccPID, resident, dllFilePath);
			return;
		}

		// First injection into this game process.
		InjectModule(mccPID, dllFilePath);
		

		if (!processContainsModule(mccPID, std::wstring(wdllChars)))
		{
			PLOG_ERROR << "Process didn't appear to contain HCMInternal after injecting!";
		}
		else
		{
			PLOG_INFO << "Confirmed that MCC contains HCMInternal!";
		}


}




bool processContainsModule(DWORD pid, std::wstring moduleName)
{
	// Now to actually confirm that the module was injected succesfully by enumerating mcc's modules

// Fill with current values
	HMODULE hMods[1024];
	DWORD cbNeeded;


	HandlePtr mcc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, TRUE, pid));
	if (!mcc)
	{
		PLOG_ERROR << (std::format("InjectCEER: Couldn't open MCC with EnumProcessModules permissions: {}", GetErrorMessage(GetLastError())).c_str());
		return false;
	}


	if (EnumProcessModules(mcc.get(), hMods, sizeof(hMods), &cbNeeded))
	{
		for (int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
		{
			TCHAR szModName[MAX_PATH];

			if (GetModuleBaseName(mcc.get(), hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) 
			{
				MODULEINFO info;
				if (GetModuleInformation(mcc.get(), hMods[i], &info, sizeof(info))) 
				{
					std::wstring name{ szModName };					
					if (name == moduleName) return true;
				}
				else
				{
					PLOG_ERROR << "GetModuleInformation failed with error code: " << GetErrorMessage(GetLastError());
					return false;
				}
			}
			else
			{
				PLOG_ERROR << "GetModuleBaseName failed with error code: " << GetErrorMessage(GetLastError());
				return false;
			}
		}
	}
	else
	{
		PLOG_ERROR << "EnumProcessModules failed with error code: " << GetErrorMessage(GetLastError());
		return false;
	}

	// no match
	PLOG_DEBUG << "No matching module found in target process! moduleName: " << moduleName;
	return false;
}

DWORD checkActualHandlePermissions(HANDLE handle)
{
	// certain driver-level anticheats can hook openProcess and strip certain access privledges.
	// The only way to test for this (besides doing the actions that require those privledges and checking that they failed)
	// is using NTQueryObject.
	// https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryobject

	// NtQueryObject is not defined in any header files so we gotta grab it ourselves from ntdll.dll.
	// ntdll.dll is technically loaded in every process but let's be safe and call loadLibrary anyway
	auto ntHandle = LoadLibraryA("ntdll.dll");
	if (!ntHandle) throw InjectionException(std::format("Could not get handle to ntdll ?!?! {}", GetErrorMessage(GetLastError())));

	typedef NTSTATUS(WINAPI* NTQUERYOBJECT)(
		HANDLE Handle,
		OBJECT_INFORMATION_CLASS ObjectInformationClass,
		PVOID ObjectInformation,
		ULONG ObjectInformationLength,
		PULONG ReturnLength);
	
	NTQUERYOBJECT pNtQueryObject = (NTQUERYOBJECT)GetProcAddress(ntHandle, "NtQueryObject");
	if (!pNtQueryObject) throw InjectionException(std::format("Could not resolve NtQueryObject: {}", GetErrorMessage(GetLastError())));


	PUBLIC_OBJECT_BASIC_INFORMATION obi;
	if (0 <= pNtQueryObject(handle, ObjectBasicInformation, &obi, sizeof(obi), 0))
	{
		// ACCESS_MASK contains the specific rights in the lower half of the DWORD; these are the ones we care about so mask out the rest
		return (obi.GrantedAccess & ~(0xFFFF0000));
	}
	else
	{
		throw InjectionException(std::format("NTQueryObject failed with: {}", GetErrorMessage(GetLastError())));
	}

	
}

// checks if process is elevated.
std::string checkProcessElevation(DWORD pid)
{
	//Only need PROCESS_QUERY_LIMITED_INFORMATION.
	HandlePtr secPrivHandleCheck(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
	if (!secPrivHandleCheck) return std::format("Could not open handle with PROCESS_QUERY_LIMITED_INFORMATION: {}", GetErrorMessage(GetLastError()));

	HANDLE hToken = NULL;
	if (!OpenProcessToken(secPrivHandleCheck.get(), TOKEN_QUERY, &hToken))
		return std::format("OpenProcessToken failed: {}", GetErrorMessage(GetLastError()));

	DWORD dwReturnLength = 0;
	TOKEN_ELEVATION_TYPE tokenElevationType;
	if (!GetTokenInformation(hToken, TokenElevationType, &tokenElevationType, sizeof(tokenElevationType), &dwReturnLength))
		return std::format("GetTokenInformation failed: {}", GetErrorMessage(GetLastError()));

	if (dwReturnLength != sizeof(tokenElevationType))
		return std::format("Bad GetTokenInformation read, expected 0x{:X} bytes but got 0x{:X}", sizeof(tokenElevationType), dwReturnLength);

	switch (tokenElevationType)
	{
	case TOKEN_ELEVATION_TYPE::TokenElevationTypeDefault:
		return "Unlinked";

	case TOKEN_ELEVATION_TYPE::TokenElevationTypeFull:
		return "Admin";

	case TOKEN_ELEVATION_TYPE::TokenElevationTypeLimited:
		return "Normal";

	default:
		return std::format("TOKEN_ELEVATION_TYPE was invalid value: 0x{:X}", (DWORD)tokenElevationType);
	}

}

void InjectModule(DWORD pid, std::string dllFilePath)
{
	try
	{

		DWORD desiredMCCAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
		HandlePtr mcc(OpenProcess(desiredMCCAccess, FALSE, pid));
		if (!mcc) throw InjectionException(std::format("HCM didn't have appropiate permissions to modify MCC. If MCC or steam are running as admin, HCM needs to be run as admin too.\nNerdy details: {}", GetErrorMessage(GetLastError())).c_str());

		try
		{

			// Get the address of our own Kernel32's loadLibrary (it will be the same in the target process because Kernel32 is loaded in the same virtual memory in all processes)
			auto loadLibraryAddr = GetProcAddress(GetModuleHandle(L"kernel32.dll"), "LoadLibraryA");
			if (!loadLibraryAddr) throw InjectionException(std::format("Couldn't find addr of loadLibraryA, error code: {}", GetErrorMessage(GetLastError())).c_str());


			// Allocate some memory on the target process, enough to store the filepath of the DLL
			auto pathAlloc = VirtualAllocEx(mcc.get(), 0, dllFilePath.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (!pathAlloc)
			{
				throw InjectionException(std::format("Failed to allocate memory in MCC for dll path, error code: {}", GetErrorMessage(GetLastError())).c_str());
			}
			// Write the dll filepath string to allocated memory
			DWORD oldProtect;
			DWORD dontcare;
			size_t bytesWritten;
			if (!VirtualProtectEx(mcc.get(), pathAlloc, dllFilePath.size(), PAGE_READWRITE, &oldProtect))
				throw InjectionException(std::format("Failed to unprotect pathAlloc: {}", GetErrorMessage(GetLastError())).c_str());
			if (!WriteProcessMemory(mcc.get(), pathAlloc, dllFilePath.c_str(), dllFilePath.size(), &bytesWritten))
				throw InjectionException(std::format("Failed to write pathAlloc: {}", GetErrorMessage(GetLastError())).c_str());
			if (!VirtualProtectEx(mcc.get(), pathAlloc, dllFilePath.size(), oldProtect, &dontcare))
				throw InjectionException(std::format("Failed to reprotect pathAlloc: {}", GetErrorMessage(GetLastError())).c_str());
			if (bytesWritten != dllFilePath.size())
				throw InjectionException(std::format("Failed to completely write pathAlloc: {}", GetErrorMessage(GetLastError())).c_str());

			PLOG_DEBUG << "Wrote path " << dllFilePath << "to MCC allocated memory at 0x" << std::hex << pathAlloc << "(0x" << bytesWritten << " bytes written)";

			PLOG_DEBUG << "Calling createRemoteThread";
			// Create a thread to call LoadLibraryA with pathAlloc as parameter
			auto tHandle = CreateRemoteThread(mcc.get(), NULL, NULL, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr), pathAlloc, NULL, NULL);
			if (!tHandle) throw InjectionException(std::format("Couldn't create loadLibrary thread in mcc, error code: {}", GetErrorMessage(GetLastError())).c_str());

			// Check if thread completed successfully
			auto waitResult = WaitForSingleObject(tHandle, 3000);

			switch (waitResult)
			{
			case 0x00000080:
				throw InjectionException("Remote thread failed unexpectedly (WAIT_ABANDONED)");
			case 0x00000102:
				throw InjectionException("Remote thread timed out (WAIT_TIMEOUT)");
			default:
				break;
			}

			PLOG_DEBUG << "Remote thread finished execution";

			// Get thread exit code 
			DWORD exitCode;
			GetExitCodeThread(tHandle, &exitCode);
			if (exitCode == 0) throw InjectionException(std::format("LoadLibraryA failed: {}", GetErrorMessage(GetLastError())).c_str());

			PLOG_DEBUG << "Remote thread exit code: 0x" << std::hex << exitCode;

			PLOG_INFO << "Successfully injected module " << dllName;
		}
		catch (InjectionException ex) // Append more information to the exception using the process handle, then rethrow
		{
			// Check that the handle from openProcess was actually granted the permissions we requested.
			// Certain driver-level anti-cheats will hook openProcess to remove certain permissions (without causing the function to fail)
			try
			{
				DWORD grantedMCCAccess = checkActualHandlePermissions(mcc.get());

				// we want to know what bits are set in desired that AREN'T set in granted. Bitwise subtraction.
				DWORD missingRights = desiredMCCAccess & ~grantedMCCAccess;

				if (missingRights != 0)
				{
					ex.append(
						std::format(
							"\n\nGranted handle access was not desired handle access! \nThis probably (? not sure tbh) means an anti-cheat is running, stripping access rights. \nGranted access rights were: 0x{:04X}\nDifference is: 0x{:04X}", 
							grantedMCCAccess, missingRights));
				}
			}
			catch (InjectionException accessCheckEx)
			{
				ex.append(std::format("\n\nCould not double check process handles granted access bits: {}", accessCheckEx.what()));
			}

			throw ex;

		}
	}
	catch (InjectionException ex) // Append security level info then rethrow
	{
		// no mcc handle here but we can still check security levels
		ex.append("\n\nProcess Elevation Info: ");
		ex.append(std::format("\nHCM elevation level: {}", checkProcessElevation(GetCurrentProcessId())));
		ex.append(std::format("\nMCC elevation level: {}", checkProcessElevation(pid)));


		throw ex;
	}
}




std::optional<std::wstring> imbueInternal(HandlePtr& mccHandle)
{
	try
	{
		SetupInternal(GetProcessId(mccHandle.get()));
		return std::nullopt;
	}
	catch (InjectionException ex)
	{
		return str_to_wstr(ex.what());
	}
}
