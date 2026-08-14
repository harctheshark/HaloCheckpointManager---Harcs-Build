// ================================================================================================================
// HCM Linux launcher - a console replacement for the WPF HCMExternal, for running HCM under Proton/Wine.
//
// WHY THIS EXISTS, AND WHY IT IS STILL A WINDOWS .EXE
// ---------------------------------------------------
// Halo Campaign Evolved is a Windows game. On Linux it runs under Proton (Wine + VKD3D-Proton), so the game
// process is a WINDOWS process. HCMInternal.dll hooks that process's D3D12 swapchain from inside it, with
// safetyhook and an ImGui D3D12 backend. A native ELF .so cannot meaningfully take part in that: it has no
// LoadLibrary semantics, cannot hook Wine's PE modules, and none of the hooking machinery would apply.
//
// So there is no "native Linux build" to make. What there IS: everything must run INSIDE the game's own Wine
// prefix, as Windows binaries. The only piece that genuinely cannot survive that is the GUI - HCMExternal is
// net7.0-windows with UseWPF=true, and WPF has never been ported to Linux and behaves badly under Wine.
//
// That turns out not to matter much, because HCMInternal renders its own ImGui overlay INSIDE the game. The
// overlay is the real UI. HCMExternal is a launcher, an injector, and a checkpoint file browser. This program
// replaces the launcher and injector parts and leaves the rest to the in-game overlay.
//
// WHAT IT DELIBERATELY DOES NOT DO
// --------------------------------
// It reuses HCMInterproc.dll rather than reimplementing anything. That DLL already creates the shared memory
// segment, finds the game, injects HCMInternal, and runs the hook state machine - all of it identical to what
// the Windows build does, so there is no second injection path to keep in sync or to get subtly wrong.
//
// ⚠ THE EXECUTABLE NAME IS LOad-BEARING. HCMInternal's HeartbeatTimer searches running processes for
// "HCMExternal.exe" or "HaloCheckpointManager.exe", and if neither is present within 3 seconds it calls
// GlobalKill::killMe() and unloads itself - by design, so an orphaned DLL cannot pin itself in the game. This
// program must therefore BE one of those two names, and must STAY RUNNING for as long as you want HCM alive.
// Renaming the binary will make HCM inject and then immediately vanish, with the only clue in the log.
//
// ⚠ shutdownInterproc() ON EXIT IS NOT OPTIONAL. See the comment block in InitInterproc.cpp: if the state
// machine is still running while this process dies, it sees HCMInternal report Shutdown, walks its states, and
// injects a FRESH orphan into the game as we exit. That orphan pins HCMInternal.dll and makes the next launch
// fail. Every exit path here - normal, Ctrl+C, console close, logoff - routes through shutdown().
//
// FILE LAYOUT: this .exe must sit in the same directory as HCMInternal.dll, HCMInterproc.dll and
// InternalPointerData.xml. Both HCMInterproc's shared-memory setup and its injector derive their paths from
// GetModuleFileNameA(NULL, ...) - i.e. from THIS executable's directory - so a split layout silently breaks
// pointer data loading rather than failing loudly.
// ================================================================================================================
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>

namespace
{
	typedef uint16_t(__cdecl* FnInitialiseInterproc)(
		bool CPnullData,
		int CPgame, const char* CPname, const char* CPpath, const char* CPlevelcode, const char* CPgameVersion, int CPdifficulty,
		int SFgame, const char* SFname, const char* SFpath);
	typedef void (__cdecl* FnShutdownInterproc)();
	typedef void (__cdecl* FnResetStateMachine)();

	HMODULE gInterproc = nullptr;
	FnShutdownInterproc gShutdown = nullptr;
	volatile LONG gShutdownDone = 0;

	std::string exeDirectory()
	{
		char buffer[MAX_PATH]{};
		GetModuleFileNameA(NULL, buffer, MAX_PATH);
		std::string path(buffer);
		const auto slash = path.find_last_of("\\/");
		return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
	}

	// Idempotent, and safe to call from a console control handler (which runs on its own thread and gives us
	// only a few seconds before Windows kills the process regardless).
	void shutdown()
	{
		if (InterlockedExchange(&gShutdownDone, 1) != 0) return;
		if (gShutdown)
		{
			std::printf("[hcm] stopping the injector state machine...\n");
			gShutdown();
		}
	}

	BOOL WINAPI consoleHandler(DWORD type)
	{
		switch (type)
		{
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			shutdown();
			return TRUE;
		default:
			return FALSE;
		}
	}

	bool fileExists(const std::string& path)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}
}

int main(int argc, char** argv)
{
	std::printf(
		"HCM Linux launcher (console HCMExternal replacement)\n"
		"  Halo Campaign Evolved support. Run this INSIDE the game's Proton/Wine prefix.\n"
		"  All of HCM's UI is the in-game overlay - open it there.\n\n");

	const std::string dir = exeDirectory();
	std::printf("[hcm] directory: %s\n", dir.c_str());

	// Fail LOUDLY and specifically on a bad layout. Both of these are derived from this exe's directory by
	// HCMInterproc, so getting it wrong produces confusing downstream failures rather than an obvious one.
	const char* required[] = { "HCMInternal.dll", "HCMInterproc.dll", "InternalPointerData.xml" };
	bool layoutOk = true;
	for (const char* name : required)
	{
		const std::string path = dir + "\\" + name;
		if (!fileExists(path))
		{
			std::printf("[hcm] MISSING: %s\n", name);
			layoutOk = false;
		}
	}
	if (!layoutOk)
	{
		std::printf(
			"\n[hcm] This launcher must sit in the same folder as HCMInternal.dll, HCMInterproc.dll and\n"
			"      InternalPointerData.xml. Aborting rather than injecting a half-configured HCM.\n");
		return 2;
	}

	// ⚠ The heartbeat inside HCMInternal looks for a process named HCMExternal.exe or
	// HaloCheckpointManager.exe. If this binary has been renamed, say so now rather than let HCM inject and
	// then silently unload itself three seconds later.
	{
		char self[MAX_PATH]{};
		GetModuleFileNameA(NULL, self, MAX_PATH);
		std::string name(self);
		const auto slash = name.find_last_of("\\/");
		if (slash != std::string::npos) name = name.substr(slash + 1);
		if (_stricmp(name.c_str(), "HCMExternal.exe") != 0 && _stricmp(name.c_str(), "HaloCheckpointManager.exe") != 0)
		{
			std::printf(
				"\n[hcm] WARNING: this executable is named '%s'.\n"
				"      HCMInternal's heartbeat only recognises 'HCMExternal.exe' or\n"
				"      'HaloCheckpointManager.exe'. Under any other name HCM will inject and then unload\n"
				"      itself after ~3 seconds. Rename it back.\n\n", name.c_str());
		}
	}

	const std::string interprocPath = dir + "\\HCMInterproc.dll";
	gInterproc = LoadLibraryA(interprocPath.c_str());
	if (!gInterproc)
	{
		std::printf("[hcm] could not load HCMInterproc.dll (error %lu)\n", GetLastError());
		return 3;
	}

	auto initialiseInterproc = (FnInitialiseInterproc)GetProcAddress(gInterproc, "initialiseInterproc");
	gShutdown = (FnShutdownInterproc)GetProcAddress(gInterproc, "shutdownInterproc");
	if (!initialiseInterproc || !gShutdown)
	{
		std::printf("[hcm] HCMInterproc.dll is missing initialiseInterproc/shutdownInterproc - wrong version?\n");
		return 4;
	}

	SetConsoleCtrlHandler(consoleHandler, TRUE);

	// Same call HCMExternal makes when it has no checkpoint selected (InterprocService.cs:99). The checkpoint
	// browser is a GUI feature we are not replacing, so we always start with null checkpoint data; Dump and
	// Inject are driven from the in-game overlay, which does its own file dialog.
	//
	// game index 8 = HaloCER. The save folder name/path are only used by the GUI's browser, so they are empty.
	constexpr int kHaloCERIndex = 8;
	const uint16_t result = initialiseInterproc(
		true,
		kHaloCERIndex, "", "", "", "", 0,
		kHaloCERIndex, "", "");

	if (!result)
	{
		std::printf("[hcm] interproc failed to initialise (shared memory could not be created).\n"
			"      Another HCM may already be running in this prefix.\n");
		shutdown();
		FreeLibrary(gInterproc);
		return 5;
	}

	std::printf(
		"[hcm] running. Start Halo Campaign Evolved (or it can already be running) - HCM will inject\n"
		"      automatically and keep retrying. Open the overlay in-game.\n"
		"[hcm] press Ctrl+C, or close this window, to unload HCM cleanly.\n\n");

	// Park. The state machine runs on its own thread inside HCMInterproc; this thread exists only to keep the
	// process (and therefore HCMInternal's heartbeat target) alive.
	while (!gShutdownDone) Sleep(250);

	std::printf("[hcm] shut down. Safe to close.\n");
	// Deliberately NOT FreeLibrary here: the state machine thread inside HCMInterproc may still be winding
	// down, and unmapping it underneath itself is the same class of bug HCMInternal's ImageResidencyGuard
	// exists to prevent. The process is exiting anyway.
	return 0;
}
