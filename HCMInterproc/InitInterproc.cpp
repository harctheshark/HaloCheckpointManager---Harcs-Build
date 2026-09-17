#include "pch.h"
#include "InitInterproc.h"
#include "HookStateMachine.h"
#include "SharedMemoryExports.h"
#include "SharedMemoryExternal.h"
#include <fstream>

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

		// Prove we are alive to a sandboxed HCMInternal, which cannot open our process to check.
		// See SharedMemoryExternal::externalHeartbeat.
		if (g_SharedMemoryExternal.get()) g_SharedMemoryExternal->bumpHeartbeat();

		// Write a settings file on behalf of a sandboxed HCMInternal - see pendingConfigXml in the header.
		// ⚠ Only ever non-empty for a title that could not write its own config (Halo 5: Forge). MCC and
		// HaloCER write directly and never reach this path.
		if (g_SharedMemoryExternal.get())
		{
			std::string xml;
			if (g_SharedMemoryExternal->takePendingConfigSave(xml))
			{
				const std::string dir = getOwnProcessDirectory();
				const std::string finalPath = dir + "HCMInternalConfig.xml";
				const std::string tempPath = finalPath + ".tmp";
				// Same temp-then-rename the in-process saver uses, for the same reason: a crash mid-write
				// must not leave a truncated config, because a truncated config still PARSES.
				bool ok = false;
				{
					std::ofstream f(tempPath, std::ios::binary | std::ios::trunc);
					if (f) { f.write(xml.data(), (std::streamsize)xml.size()); ok = f.good(); }
				}
				if (ok && MoveFileExA(tempPath.c_str(), finalPath.c_str(),
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					PLOG_DEBUG << "Wrote forwarded settings (" << xml.size() << " bytes) to " << finalPath;
				else
					PLOG_ERROR << "Could not write forwarded settings to " << finalPath
						<< " (error " << GetLastError() << ")";
			}
		}

		// Sleep in slices so shutdown is observed within ~100ms rather than up to a second - the window in
		// which an injection can still be started is exactly this sleep.
		for (int i = 0; i < 10 && !gShuttingDown.load(std::memory_order_acquire); ++i)
			Sleep(100);
	}
	PLOG_INFO << "interproc state machine loop exited cleanly";
}

// Is the game the window the user is actually typing into?
//
// ⚠ Halo 5: Forge has NO WINDOW OF ITS OWN (60 threads, zero HWNDs). What the user sees and focuses is an
// ApplicationFrameWindow owned by ApplicationFrameHost.exe, so the obvious test - foreground window's PID
// equals the game's PID - is ALWAYS FALSE for this title and would disable every hotkey.
// So accept either: the foreground window belongs to the game process (normal Win32 titles), or it is an
// ApplicationFrameWindow whose title names the game (UWP titles).
static bool gameIsForegroundWindow()
{
	const HWND fg = GetForegroundWindow();
	if (!fg) return false;

	// UWP case FIRST, because for Halo 5 the PID test below can never succeed.
	wchar_t cls[64] = {};
	if (GetClassNameW(fg, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"ApplicationFrameWindow") == 0)
	{
		wchar_t title[256] = {};
		GetWindowTextW(fg, title, ARRAYSIZE(title));
		if (wcsstr(title, L"Halo") != nullptr) return true;
	}

	// Normal Win32 titles: ask the foreground process what it is.
	DWORD fgPid = 0;
	GetWindowThreadProcessId(fg, &fgPid);
	if (!fgPid) return false;

	bool isGame = false;
	if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid))
	{
		wchar_t path[MAX_PATH] = {};
		DWORD len = ARRAYSIZE(path);
		if (QueryFullProcessImageNameW(h, 0, path, &len))
		{
			const wchar_t* exe = wcsrchr(path, L'\\');
			exe = exe ? exe + 1 : path;
			for (const auto* name : { L"halo5forge.exe", L"HaloCampaignEvolved.exe",
									  L"MCC-Win64-Shipping.exe", L"MCCWinStore-Win64-Shipping.exe" })
			{
				if (_wcsicmp(exe, name) == 0) { isGame = true; break; }
			}
		}
		CloseHandle(h);
	}
	return isGame;
}

// Raw mouse motion, collected HERE rather than inside the game. See publishMouseMotion for why that
// distinction is the whole point.
static std::atomic<int> gRawDX{ 0 }, gRawDY{ 0 }, gRawWheel{ 0 };

static LRESULT __stdcall inputForwardingWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT)
	{
		UINT size = 0;
		if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == 0
			&& size > 0 && size <= 1024)
		{
			alignas(8) BYTE buffer[1024];
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size)
			{
				const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buffer);
				if (ri->header.dwType == RIM_TYPEMOUSE)
				{
					if ((ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
					{
						gRawDX.fetch_add(ri->data.mouse.lLastX, std::memory_order_relaxed);
						gRawDY.fetch_add(ri->data.mouse.lLastY, std::memory_order_relaxed);
					}
					if (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
						gRawWheel.fetch_add((short)ri->data.mouse.usButtonData, std::memory_order_relaxed);
				}
			}
		}
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void keyboardForwardingLoop()
{
	// A message-only window of our own, in THIS process, to receive raw mouse motion. Safe here precisely
	// because it is not the game's process - the same registration inside the game would displace the
	// game's and break aiming.
	HWND inputWindow = nullptr;
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &inputForwardingWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"HCMInputForwarder";
		RegisterClassExW(&wc);

		inputWindow = CreateWindowExW(0, L"HCMInputForwarder", L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
		if (inputWindow)
		{
			RAWINPUTDEVICE rid{};
			rid.usUsagePage = 0x01;
			rid.usUsage = 0x02;                  // mouse only; the keyboard is polled, not hooked
			rid.dwFlags = RIDEV_INPUTSINK;       // delivered even though we are never the foreground window
			rid.hwndTarget = inputWindow;
			if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
				PLOG_INFO << "Mouse motion forwarding is live (raw input in HCMExternal, not in the game).";
			else
				PLOG_ERROR << "Could not register raw mouse input in HCMExternal (error " << GetLastError()
					<< "); the overlay pointer will not move while the game has the cursor clipped.";
		}
		else
		{
			PLOG_ERROR << "Could not create the input forwarding window (error " << GetLastError() << ").";
		}
	}

	while (!gShuttingDown.load(std::memory_order_acquire))
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		if (g_SharedMemoryExternal.get())
		{
			const bool foreground = gameIsForegroundWindow();
			g_SharedMemoryExternal->publishKeyboardState(foreground);

			// ⚠ Drain the accumulators even when the game is not foreground, and discard the result then -
			// otherwise alt-tabbing away and moving the mouse banks a huge delta that teleports the overlay
			// pointer the instant the user comes back.
			const int dx = gRawDX.exchange(0, std::memory_order_relaxed);
			const int dy = gRawDY.exchange(0, std::memory_order_relaxed);
			const int wheel = gRawWheel.exchange(0, std::memory_order_relaxed);
			if (foreground)
				g_SharedMemoryExternal->publishMouseMotion(dx, dy, wheel);
		}

		Sleep(4);   // ~250Hz. Hotkeys have to feel instant and pointer motion has to feel smooth.
	}

	if (inputWindow) DestroyWindow(inputWindow);
	PLOG_INFO << "keyboard forwarding loop exited cleanly";
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

	// And the keyboard forwarder - see SharedMemoryExternal::publishKeyboardState. The state machine tick
	// is far too slow for this (one second; hotkeys need to feel instant), so it gets its own thread.
	CreateThread(0, 0x1000, (LPTHREAD_START_ROUTINE)keyboardForwardingLoop, NULL, 0, NULL);
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
