#include "pch.h"
#include "ImGuiManager.h"
#include "GlobalKill.h"
#include "SharedMemoryInternal.h"   // forwardedKeyStates - the only keyboard source inside an AppContainer
#include "ImageResidencyGuard.h"
#include "ProggyVectorRegularFont.h"
#include "imgui_internal.h"





ImGuiManager* ImGuiManager::instance = nullptr;

WNDPROC ImGuiManager::mOldWndProc = nullptr;
IMGUI_IMPL_API LRESULT  ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT __stdcall ImGuiManager::mNewWndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// FIRST STATEMENT, before anything else can touch HCM state. Windows can dispatch a message into this
	// procedure at literally any instant, including while HCMInternal.dll is being unloaded - and until now
	// nothing waited for it. See ImageResidencyGuard.h. Re-entrancy on this thread is expected (the present
	// path pumps messages) and is handled by the guard's depth counter.
	ImageResidency::ScopedImageResidency residency;

	// Once shutdown has begun, get completely out of the game's way: forward every message straight to
	// the game's original WndProc and don't touch ImGui. During teardown the "return true" path below
	// can SWALLOW window messages (WM_SIZE / WM_DISPLAYCHANGE / activation etc.) that the game needs to
	// keep its D3D device consistent - a message swallowed there can leave the game's immediate context
	// in a bad state and crash the render thread mid-teardown. This also avoids touching an ImGui
	// context that may be mid-destruction.
	if (GlobalKill::isKillSet())
		return mOldWndProc ? CallWindowProc(mOldWndProc, hWnd, uMsg, wParam, lParam)
		                   : DefWindowProcW(hWnd, uMsg, wParam, lParam);

	// ⚠⚠ MEASURED CRASH, NOT A PRECAUTION. ImGui::GetIO() is `return GImGui->IO;` guarded only by an
	// IM_ASSERT, which COMPILES OUT IN RELEASE. Because it returns a REFERENCE, a null context does not
	// fault here - taking &GImGui->IO on a null GImGui is just offset arithmetic. It faults at the FIRST
	// USE of io, which is `io.WantCaptureMouse` below, reading null+0xB8.
	//
	// That is exactly the crash in the 2026-08-25 dump: HCMInternal.dll!ImGuiManager::mNewWndProc+0x119,
	// ACCESS_VIOLATION reading 0x00000000000000B8, symbolised to this file. It reproduces when RivaTuner
	// is CLOSED while HCM is live - RTSS hooks this same window procedure, and unloading it re-enters the
	// chain at a moment when our ImGui context does not exist. GlobalKill covers our own shutdown; it does
	// NOT cover "a message arrived before the context was created, or after someone else's teardown".
	//
	// Any message that reaches us without a context belongs to the game, so hand it straight on.
	if (ImGui::GetCurrentContext() == nullptr)
		return mOldWndProc ? CallWindowProc(mOldWndProc, hWnd, uMsg, wParam, lParam)
		                   : DefWindowProcW(hWnd, uMsg, wParam, lParam);

	//https://www.unknowncheats.me/forum/2488829-post5.html
	ImGuiIO& io = ImGui::GetIO();
	LRESULT res = ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);


	switch (uMsg)
	{
	//case WM_CLOSE:
	case WM_DESTROY:
	case WM_NCDESTROY:
		GlobalKill::killMe();
		break;
	default:
		break;
	}

	// "GUI showing blocks game input" for games with no engine-level block-input service (Halo Campaign Evolved).
	// MCC gets this from blockGameInputService, which midhooks MCC itself; HCE has no equivalent, so instead we
	// simply don't forward input messages to the game while the HCM window is open. ImGui has already seen the
	// message above, so the overlay still works. Non-input messages (activation, sizing, paint, device changes)
	// MUST still reach the game or its D3D state goes inconsistent - so this only swallows input classes.
	if (sSwallowGameInput.load(std::memory_order_relaxed))
	{
		switch (uMsg)
		{
		case WM_INPUT:              // raw input - how UE5 does mouse-look. Blocking only WM_MOUSEMOVE is not enough.
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
		case WM_KEYDOWN: case WM_KEYUP:
		case WM_CHAR: case WM_DEADCHAR: case WM_UNICHAR:
			return 0;   // consumed: the game never sees it
			// NOTE: WM_SYSKEYDOWN/WM_SYSKEYUP are deliberately NOT swallowed, so Alt+Tab / Alt+F4 keep working.
		default:
			break;
		}
	}

	// ⚠ Read it ONCE and null-check it. ~ImGuiManager nulls mOldWndProc on the shutdown thread while
	// messages are still arriving here, and these two forwards had no guard - unlike the two above.
	// CallWindowProc(nullptr, ...) is an access violation inside user32, attributed to the game.
	const WNDPROC oldProc = ImGuiManager::mOldWndProc;

	if (io.WantCaptureMouse == false)
	{
		// ImGui didn't handle the click so let MCC do it
		return oldProc ? CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam)
		               : DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}
	else
	{
		switch (uMsg)
		{
			// Certain messages we want to let MCC know about too
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
		case WM_NCACTIVATE:
			return oldProc ? CallWindowProc(oldProc, hWnd, uMsg, wParam, lParam)
			               : DefWindowProcW(hWnd, uMsg, wParam, lParam);
			break;
		default:
			return true; // otherwise we just tell MCC to not worry about it
		}
	}





}

// Virtual-key -> ImGuiKey. Covers what HCM's GUI actually needs: navigation, editing and text entry.
static ImGuiKey vkToImGuiKey(int vk)
{
	if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (ImGuiKey)(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));

	switch (vk)
	{
	case VK_BACK:     return ImGuiKey_Backspace;
	case VK_DELETE:   return ImGuiKey_Delete;
	case VK_RETURN:   return ImGuiKey_Enter;
	case VK_ESCAPE:   return ImGuiKey_Escape;
	case VK_TAB:      return ImGuiKey_Tab;
	case VK_SPACE:    return ImGuiKey_Space;
	case VK_LEFT:     return ImGuiKey_LeftArrow;
	case VK_RIGHT:    return ImGuiKey_RightArrow;
	case VK_UP:       return ImGuiKey_UpArrow;
	case VK_DOWN:     return ImGuiKey_DownArrow;
	case VK_HOME:     return ImGuiKey_Home;
	case VK_END:      return ImGuiKey_End;
	case VK_PRIOR:    return ImGuiKey_PageUp;
	case VK_NEXT:     return ImGuiKey_PageDown;
	case VK_INSERT:   return ImGuiKey_Insert;
	case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
	case VK_SHIFT:   case VK_LSHIFT:   return ImGuiKey_LeftShift;
	case VK_MENU:    case VK_LMENU:    return ImGuiKey_LeftAlt;
	case VK_RCONTROL: return ImGuiKey_RightCtrl;
	case VK_RSHIFT:   return ImGuiKey_RightShift;
	case VK_RMENU:    return ImGuiKey_RightAlt;
	case VK_LWIN:     return ImGuiKey_LeftSuper;
	case VK_RWIN:     return ImGuiKey_RightSuper;
	case VK_APPS:     return ImGuiKey_Menu;

	// ⚠ THE OEM KEYS MATTER - DO NOT LEAVE THEM OUT AGAIN.
	// Omitting them is not a cosmetic gap: VK_OEM_3 is the `~` key, which is HCM's menu hotkey, so with it
	// unmapped vkToImGuiKey returned ImGuiKey_None, no event was ever emitted, and the main menu simply
	// could not be opened on Halo 5 - while Enter worked fine, which makes it look like a keyboard problem
	// rather than a missing table entry.
	case VK_OEM_3:      return ImGuiKey_GraveAccent;    // ` ~
	case VK_OEM_MINUS:  return ImGuiKey_Minus;
	case VK_OEM_PLUS:   return ImGuiKey_Equal;
	case VK_OEM_4:      return ImGuiKey_LeftBracket;    // [ {
	case VK_OEM_6:      return ImGuiKey_RightBracket;   // ] }
	case VK_OEM_5:      return ImGuiKey_Backslash;      // \ |
	case VK_OEM_1:      return ImGuiKey_Semicolon;      // ; :
	case VK_OEM_7:      return ImGuiKey_Apostrophe;     // ' "
	case VK_OEM_COMMA:  return ImGuiKey_Comma;
	case VK_OEM_PERIOD: return ImGuiKey_Period;
	case VK_OEM_2:      return ImGuiKey_Slash;          // / ?

	case VK_CAPITAL:  return ImGuiKey_CapsLock;
	case VK_NUMLOCK:  return ImGuiKey_NumLock;
	case VK_SCROLL:   return ImGuiKey_ScrollLock;
	case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
	case VK_PAUSE:    return ImGuiKey_Pause;

	case VK_ADD:      return ImGuiKey_KeypadAdd;
	case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
	case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
	case VK_DIVIDE:   return ImGuiKey_KeypadDivide;
	case VK_DECIMAL:  return ImGuiKey_KeypadDecimal;

	default:          return ImGuiKey_None;
	}
}

// Raw Input plumbing for windowless targets. See the note on startWindowlessRawInput in the header.
LRESULT __stdcall ImGuiManager::rawInputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_INPUT)
	{
		UINT size = 0;
		if (::GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == 0 && size > 0 && size <= 1024)
		{
			alignas(8) BYTE buffer[1024];
			if (::GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size)
			{
				const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buffer);
				sRawEventCount.fetch_add(1, std::memory_order_relaxed);

				std::scoped_lock queueLock(sRawQueueMutex);
				// Hard cap: if the render thread ever stops draining (overlay hidden, game paused) this
				// must not grow without bound inside the game's process.
				if (sRawQueue.size() > 512) sRawQueue.clear();

				if (ri->header.dwType == RIM_TYPEMOUSE)
				{
					// Relative motion, accumulated for the clipped-cursor case. Absolute-mode devices
					// (tablets, some RDP setups) report a screen position instead and are no use here.
					if ((ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
					{
						if (ri->data.mouse.lLastX) sRawMouseDeltaX.fetch_add(ri->data.mouse.lLastX, std::memory_order_relaxed);
						if (ri->data.mouse.lLastY) sRawMouseDeltaY.fetch_add(ri->data.mouse.lLastY, std::memory_order_relaxed);
					}

					const USHORT f = ri->data.mouse.usButtonFlags;
					auto button = [](int i, bool down)
						{
							sRawMouseDown[i].store(down ? 1 : 0, std::memory_order_relaxed);
							sRawQueue.push_back({ WindowlessInputEvent::Kind::MouseButton, i, down, 0.0f });
							sRawButtonEvents.fetch_add(1, std::memory_order_relaxed);
						};
					if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   button(0, true);
					if (f & RI_MOUSE_LEFT_BUTTON_UP)     button(0, false);
					if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  button(1, true);
					if (f & RI_MOUSE_RIGHT_BUTTON_UP)    button(1, false);
					if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) button(2, true);
					if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   button(2, false);
					if (f & RI_MOUSE_WHEEL)
						sRawQueue.push_back({ WindowlessInputEvent::Kind::Wheel, 0, false,
							(float)(short)ri->data.mouse.usButtonData / (float)WHEEL_DELTA });
				}
				else if (ri->header.dwType == RIM_TYPEKEYBOARD)
				{
					USHORT vk = ri->data.keyboard.VKey;
					if (vk < 256)
					{
						const bool up = (ri->data.keyboard.Flags & RI_KEY_BREAK) != 0;
						sRawKeyDown[vk].store(up ? 0 : 1, std::memory_order_relaxed);
						sRawQueue.push_back({ WindowlessInputEvent::Kind::Key, (int)vk, !up, 0.0f });
						sRawKeyEvents.fetch_add(1, std::memory_order_relaxed);

						// Text entry. Build the keyboard state by hand - GetKeyboardState reads the
						// calling THREAD's input state, and this thread has none of its own.
						if (!up)
						{
							BYTE kbState[256] = {};
							if (sRawKeyDown[VK_SHIFT].load(std::memory_order_relaxed)) kbState[VK_SHIFT] = 0x80;
							if (sRawKeyDown[VK_CAPITAL].load(std::memory_order_relaxed)) kbState[VK_CAPITAL] = 0x01;
							WCHAR chars[4] = {};
							const int produced = ::ToUnicode(vk, ri->data.keyboard.MakeCode, kbState, chars, 4, 0);
							for (int i = 0; i < produced; ++i)
								if (chars[i] >= 0x20 && chars[i] != 0x7F)
									sRawQueue.push_back({ WindowlessInputEvent::Kind::Char, (int)chars[i], false, 0.0f });
						}
					}
				}
			}
		}
		return 0;
	}
	return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ImGuiManager::startWindowlessRawInput()
{
	if (sRawInputThread.joinable()) return;

	sRawInputThread = std::thread([]()
		{
			sRawInputThreadId.store(::GetCurrentThreadId(), std::memory_order_release);

			HMODULE self = nullptr;
			::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)&ImGuiManager::rawInputWndProc, &self);

			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = &ImGuiManager::rawInputWndProc;
			wc.hInstance = self;
			wc.lpszClassName = L"HCMWindowlessInput";
			::RegisterClassExW(&wc);   // failure here is fine if the class already exists from a prior session

			sRawInputWindow = ::CreateWindowExW(0, L"HCMWindowlessInput", L"", 0, 0, 0, 0, 0,
				HWND_MESSAGE, nullptr, self, nullptr);
			if (!sRawInputWindow)
			{
				PLOG_ERROR << "Windowless raw input: CreateWindowEx failed (" << ::GetLastError()
					<< "). Falling back to GetAsyncKeyState, which may not work in this sandbox.";
				return;
			}

			// RIDEV_INPUTSINK is the whole point: it delivers input while we are NOT the foreground window,
			// which we never are - we have no visible window at all.
			RAWINPUTDEVICE rid[2]{};
			rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;  // mouse
			rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = sRawInputWindow;
			rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;  // keyboard
			rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = sRawInputWindow;

			if (!::RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
			{
				PLOG_ERROR << "Windowless raw input: RegisterRawInputDevices failed (" << ::GetLastError()
					<< "). Falling back to GetAsyncKeyState.";
				::DestroyWindow(sRawInputWindow);
				sRawInputWindow = nullptr;
				return;
			}

			sRawInputActive.store(true, std::memory_order_release);
			PLOG_INFO << "Windowless raw input is live (message-only window 0x" << std::hex
				<< (uintptr_t)sRawInputWindow << std::dec << "); mouse and keyboard will come from WM_INPUT.";

			MSG msg;
			while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
			{
				::TranslateMessage(&msg);
				::DispatchMessageW(&msg);
			}

			sRawInputActive.store(false, std::memory_order_release);
			if (sRawInputWindow) { ::DestroyWindow(sRawInputWindow); sRawInputWindow = nullptr; }
		});
}

void ImGuiManager::stopWindowlessRawInput()
{
	// ⚠ The thread owns a window and a message pump inside the GAME's process. It MUST be gone before the
	// DLL unmaps, or the next message dispatched to our (freed) WndProc kills the game.
	if (const DWORD tid = sRawInputThreadId.load(std::memory_order_acquire))
		::PostThreadMessageW(tid, WM_QUIT, 0, 0);
	if (sRawInputThread.joinable())
		sRawInputThread.join();
	sRawInputActive.store(false, std::memory_order_release);
	sRawInputThreadId.store(0, std::memory_order_release);
}

// Is any top-level window of ours on screen? Called right after Render(), so WasActive is this frame's
// answer. Child windows are skipped - they cannot exist without a visible parent, and counting them would
// keep the cursor freed on frames where nothing is really shown.
void ImGuiManager::refreshOverlayVisibility()
{
	// ⚠⚠⚠ "ANYTHING IS DRAWN" IS THE WRONG QUESTION - IT LEAVES THE GAME WITH NO MOUSE, EVER.
	// HCM always has something on screen: the collapsed "Halo Checkpoint Manager" title bar, the current
	// game/level readout, the message log. Treating those as "the overlay wants the cursor" meant we called
	// ClipCursor(nullptr) on every single frame for the whole session, so the game could never re-capture
	// the mouse and the player could not look around or close the menu.
	//
	// What actually warrants taking the cursor is an OPEN, INTERACTIVE window: not collapsed, and not one
	// of the read-only HUD overlays (which set NoInputs / NoMouseInputs).
	// HCM's menu being open, OR a modal dialog being up (the service-failures box, which the user must be
	// able to dismiss even with the menu closed). Nothing is inferred from what happens to be drawn.
	const bool modalOpen = (ImGui::GetCurrentContext() != nullptr) && (ImGui::GetTopMostPopupModal() != nullptr);
	const bool wantsCursor = sMenuWantsCursor.load(std::memory_order_relaxed) || modalOpen;
	sOverlayVisible.store(wantsCursor, std::memory_order_relaxed);
}

// Feed ImGui's IO by polling, for targets with no window and therefore no messages.
//
// This is the route proven working against Halo 5: Forge - an ImGui window drawing over the campaign at
// 60fps with live mouse tracking. imgui_impl_win32 is not involved at all here, so everything it would
// normally supply (display size, delta time, mouse, keyboard) has to be supplied by hand.
//
// ⚠ Fullscreen composition means SCREEN COORDS MAP 1:1 TO THE BACK BUFFER. Do not try to convert through
// a client rect - there is no window to have one, and ScreenToClient would fail and leave the cursor
// pinned at the origin.
void ImGuiManager::synthesiseWindowlessInput(UINT backBufferWidth, UINT backBufferHeight)
{
	ImGuiIO& io = ImGui::GetIO();

	io.DisplaySize = ImVec2((float)backBufferWidth, (float)backBufferHeight);

	// No platform backend is ticking the clock for us.
	static std::chrono::steady_clock::time_point lastFrame{};
	const auto now = std::chrono::steady_clock::now();
	if (lastFrame.time_since_epoch().count() != 0)
	{
		const float delta = std::chrono::duration<float>(now - lastFrame).count();
		// Clamp: a stall (loading screen, alt-tab) otherwise hands imgui a multi-second delta, which makes
		// every key repeat and every animation jump.
		io.DeltaTime = std::clamp(delta, 1.0f / 1000.0f, 1.0f / 10.0f);
	}
	lastFrame = now;

	// ⚠⚠⚠ THE GAME CLIPS THE CURSOR TO A 1x1 RECTANGLE. That is the standard FPS mouse-capture trick, and
	// it is fatal to the naive approach: GetCursorPos then returns the SAME point forever, so the overlay's
	// cursor cannot be moved onto anything and nothing can be clicked. Measured live on Halo 5:
	//     clip: [603,516,604,517]   cursorInfo flags=0   (hidden)
	// HCM's normal answer is freeMCCCursorService, which does not exist for this title - there is no
	// pointer data for shouldCursorBeFreeFunction - so nothing else is going to release it.
	//
	// So take it back ourselves, but ONLY while something of ours is actually on screen; otherwise we
	// would be stealing the mouse from someone trying to play the game. We run at Present, i.e. after the
	// game's own per-frame ClipCursor, so ours is the one that sticks.
	const bool overlayVisible = sOverlayVisible.load(std::memory_order_relaxed);
	if (overlayVisible)
		::ClipCursor(nullptr);

	// ⚠⚠ SCREEN COORDINATES ARE NOT BACK-BUFFER COORDINATES, but they ARE a fixed OFFSET from them.
	// My earlier note claimed composition fullscreen maps 1:1; that holds only for a single monitor at the
	// origin. Measured live: the virtual desktop spans [-2560..6000] while the back buffer is 2560x1440,
	// and cursor X reached 4864.
	//
	// ⚠ I first "solved" this by accumulating deltas from a centred virtual cursor. Do not go back to
	// that - it produces TWO VISIBLY DESYNCED CURSORS (the real one the OS draws, and ours somewhere
	// else), because a delta-integrated position drifts away from the OS cursor the moment a single
	// movement is dropped or clamped at an edge. Absolute positioning keeps exactly one cursor.
	//
	// The offset is the origin of the monitor the game is presenting on, found by matching a monitor's
	// size to the back buffer. Falls back to (0,0), i.e. the old behaviour, if nothing matches.
	static POINT sMonitorOrigin{ 0, 0 };
	static UINT sOriginForWidth = 0, sOriginForHeight = 0;
	if (sOriginForWidth != backBufferWidth || sOriginForHeight != backBufferHeight)
	{
		sOriginForWidth = backBufferWidth;
		sOriginForHeight = backBufferHeight;
		sMonitorOrigin = { 0, 0 };

		struct MonCtx { UINT w, h; POINT origin; bool found; } ctx{ backBufferWidth, backBufferHeight, {0,0}, false };
		::EnumDisplayMonitors(nullptr, nullptr,
			[](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<MonCtx*>(lp);
				MONITORINFO mi{}; mi.cbSize = sizeof(mi);
				if (::GetMonitorInfoW(hm, &mi))
				{
					const UINT mw = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left);
					const UINT mh = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
					if (mw == c->w && mh == c->h)
					{
						c->origin = { mi.rcMonitor.left, mi.rcMonitor.top };
						c->found = true;
						return FALSE;   // stop at the first match
					}
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&ctx));

		if (ctx.found) sMonitorOrigin = ctx.origin;
		PLOG_INFO << "Windowless input: back buffer " << backBufferWidth << "x" << backBufferHeight
			<< " maps to monitor origin (" << sMonitorOrigin.x << "," << sMonitorOrigin.y << ")"
			<< (ctx.found ? "" : " - NO monitor of that size found, assuming the desktop origin");
	}

	POINT cursor{};
	const BOOL gotCursor = ::GetCursorPos(&cursor);

	// ⚠⚠⚠ THE GAME RE-CAPTURES THE MOUSE ON THE FRAME YOU CLICK, AND THAT IS WHAT BROKE CLICKING.
	// Hovering worked perfectly - items highlighted - but a press did nothing. The reason is in the log at
	// the exact sample where rawLMB first read 1:
	//     clip: [1950,575,1951,576]   cursorInfo flags=0
	// Pressing LMB makes the game clip the cursor to 1x1 and hide it, so GetCursorPos JUMPS to that pin
	// point on the very frame of the click. ImGui applies the position change before the button event
	// (event trickling), so the press was delivered at wherever the cursor got yanked to instead of at the
	// button the user was pointing at.
	//
	// ⚠⚠ AND THE CLIP IS OFTEN PERMANENT, NOT MOMENTARY. My first attempt at this simply held the last
	// unpinned position - which broke the mouse completely, because the game already had the cursor pinned
	// before the overlay appeared, so there never WAS an unpinned sample: ImGui's position stayed at its
	// uninitialised -FLT_MAX sentinel and nothing could be pointed at. We release the clip every frame and
	// the game re-applies it just as fast, so "wait for it to be free" is not a strategy.
	//
	// Raw input still reports RELATIVE motion while the cursor is clipped, so:
	//   * cursor free  -> absolute position, which keeps our pointer exactly under the visible OS cursor;
	//   * cursor pinned -> integrate raw deltas (the OS cursor is hidden then, so there is no second
	//     pointer to desync from, and this is the only source of movement that still works).
	static ImVec2 sPointer{ 0.0f, 0.0f };
	static bool sPointerSeeded = false;

	RECT clipNow{};
	const bool cursorIsPinned = ::GetClipCursor(&clipNow)
		&& (clipNow.right - clipNow.left) <= 2 && (clipNow.bottom - clipNow.top) <= 2;

	// Motion forwarded by HCMExternal. Cumulative, so diff against the last reading - a missed frame costs
	// nothing. This is the ONLY source that still moves while the game has the cursor clipped to 1x1.
	float dx = 0.0f, dy = 0.0f;
	{
		static int sLastX = 0, sLastY = 0, sLastWheel = 0;
		static bool sHaveLast = false;
		int x = 0, y = 0, wheel = 0;
		if (SharedMemoryInternal::forwardedMouseMotion(x, y, wheel))
		{
			if (sHaveLast)
			{
				dx = (float)(x - sLastX);
				dy = (float)(y - sLastY);
				if (const int dw = wheel - sLastWheel; dw != 0)
					io.AddMouseWheelEvent(0.0f, (float)dw / (float)WHEEL_DELTA);
			}
			sLastX = x; sLastY = y; sLastWheel = wheel;
			sHaveLast = true;
		}
	}

	// ⚠ WHILE THE MENU IS OPEN, MOVEMENT IS THE FORWARDED DELTA - FULL STOP.
	// Not "absolute when the cursor happens to be free": the game re-clips the cursor constantly, so an
	// absolute/relative choice made per frame flaps between two sources and the pointer stutters or sticks.
	// The OS cursor is hidden and pinned during play anyway, so there is no second pointer to stay in sync
	// with; ours is the only one the user sees.
	const bool menuHasCursor = sMenuWantsCursor.load(std::memory_order_relaxed);

	if (!sPointerSeeded)
	{
		sPointer = gotCursor && !cursorIsPinned
			? ImVec2((float)(cursor.x - sMonitorOrigin.x), (float)(cursor.y - sMonitorOrigin.y))
			: ImVec2(backBufferWidth * 0.5f, backBufferHeight * 0.5f);
		sPointerSeeded = true;
	}
	else if (menuHasCursor)
	{
		sPointer.x += dx;
		sPointer.y += dy;
	}
	else if (gotCursor && !cursorIsPinned)
	{
		// Menu closed and the cursor is genuinely free (desktop, a game menu): keep ours under the real one
		// so that opening the menu starts the pointer where the user is already looking.
		sPointer = ImVec2((float)(cursor.x - sMonitorOrigin.x), (float)(cursor.y - sMonitorOrigin.y));
	}

	// Only draw our cursor when it is ours to drive. Otherwise it sits frozen on screen during play, which
	// is what "the cursor still shows the whole time but is stuck" was.
	io.MouseDrawCursor = menuHasCursor;

	sPointer.x = std::clamp(sPointer.x, 0.0f, (float)backBufferWidth - 1.0f);
	sPointer.y = std::clamp(sPointer.y, 0.0f, (float)backBufferHeight - 1.0f);
	io.AddMousePosEvent(sPointer.x, sPointer.y);

	// ───────────────────────────── TEMPORARY DIAGNOSTIC ─────────────────────────────
	// Is the cursor actually MOVING? Halo 5 captures the mouse during gameplay and HCM's
	// freeMCCCursorService does not exist for this title (no pointer data), so the game may be pinning
	// or clipping it - in which case the overlay's cursor cannot reach anything. Tracks the range seen
	// so a pinned cursor is unmistakable. Remove once input works.
	{
		static std::atomic_uint32_t sInputFrames{ 0 };
		static long sMinX = LONG_MAX, sMaxX = LONG_MIN, sMinY = LONG_MAX, sMaxY = LONG_MIN;
		if (gotCursor)
		{
			sMinX = (std::min)(sMinX, cursor.x); sMaxX = (std::max)(sMaxX, cursor.x);
			sMinY = (std::min)(sMinY, cursor.y); sMaxY = (std::max)(sMaxY, cursor.y);
		}
		const uint32_t n = sInputFrames.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n <= 3 || (n % 120) == 0)
		{
			RECT clip{};
			const BOOL gotClip = ::GetClipCursor(&clip);
			CURSORINFO ci{}; ci.cbSize = sizeof(ci);
			const BOOL gotInfo = ::GetCursorInfo(&ci);
			PLOG_INFO << "[input-diag] #" << n
				<< " | GetCursorPos: " << (gotCursor ? "ok" : "FAILED") << " (" << cursor.x << "," << cursor.y << ")"
				<< " | range seen x[" << sMinX << ".." << sMaxX << "] y[" << sMinY << ".." << sMaxY << "]"
				<< " | overlayVisible: " << overlayVisible
				<< " | rawInput: " << (sRawInputActive.load(std::memory_order_relaxed) ? "LIVE" : "off")
				<< " rawEvents: " << sRawEventCount.load(std::memory_order_relaxed)
				<< " btnEvents: " << sRawButtonEvents.load(std::memory_order_relaxed)
				<< " keyEvents: " << sRawKeyEvents.load(std::memory_order_relaxed)
				<< " | fwdLMB: " << (SharedMemoryInternal::forwardedKeyStates()
						? (int)SharedMemoryInternal::forwardedKeyStates()[VK_LBUTTON] : -1)
				<< " | fwdKeys: " << (SharedMemoryInternal::forwardedKeyStates() ? "yes" : "NO")
				<< " fwdEnter: " << (SharedMemoryInternal::forwardedKeyStates()
						? (int)SharedMemoryInternal::forwardedKeyStates()[VK_RETURN] : -1)
				<< " | cursorPinned: " << cursorIsPinned
				<< " | DisplaySize: " << io.DisplaySize.x << "x" << io.DisplaySize.y
				<< " | imgui MousePos: (" << io.MousePos.x << "," << io.MousePos.y << ")"
				<< " | LMB: " << ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
				<< " RMB: " << ((::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)
				<< " | WantCaptureMouse: " << io.WantCaptureMouse
				<< " | clip: " << (gotClip ? "ok" : "FAILED")
				<< " [" << clip.left << "," << clip.top << "," << clip.right << "," << clip.bottom << "]"
				<< " | cursorInfo: " << (gotInfo ? "ok" : "FAILED") << " flags=" << ci.flags;
		}
	}

	// ⚠ GetAsyncKeyState's HIGH bit is "down now"; the LOW bit is "pressed since last call" and is a trap -
	// it is consumed by whoever reads it first, so two readers race and lose presses.
	// ⚠⚠ And in this sandbox it appears to report nothing at all, which is why raw input exists - see
	// startWindowlessRawInput. OR the two so whichever works wins.
	auto isDown = [](int vk) -> bool
		{
			if (vk >= 0 && vk < 256 && sRawInputActive.load(std::memory_order_relaxed))
			{
				int rawIndex = -1;
				if (vk == VK_LBUTTON) rawIndex = 0;
				else if (vk == VK_RBUTTON) rawIndex = 1;
				else if (vk == VK_MBUTTON) rawIndex = 2;

				const bool raw = (rawIndex >= 0)
					? sRawMouseDown[rawIndex].load(std::memory_order_relaxed) != 0
					: sRawKeyDown[vk].load(std::memory_order_relaxed) != 0;
				if (raw) return true;
			}
			return (::GetAsyncKeyState(vk) & 0x8000) != 0;
		};

	// Modifiers are STATE, not transitions, so they are read directly.
	io.AddKeyEvent(ImGuiMod_Ctrl,  isDown(VK_CONTROL));
	io.AddKeyEvent(ImGuiMod_Shift, isDown(VK_SHIFT));
	io.AddKeyEvent(ImGuiMod_Alt,   isDown(VK_MENU));

	// ⚠⚠⚠ KEYBOARD COMES FROM OUTSIDE THE SANDBOX, because nothing inside it can read the keyboard:
	// GetAsyncKeyState returns 0x0000 for every key, and Raw Input registers for mouse AND keyboard but
	// then delivers MOUSE ONLY (measured: btnEvents climbing, keyEvents stuck at 0 forever). HCMExternal
	// polls the real keyboard and publishes it through shared memory; see publishKeyboardState.
	// Emitted AFTER the modifier lines above so these win when both are present.
	if (const unsigned char* keys = SharedMemoryInternal::forwardedKeyStates())
	{
		static bool sPrevKeyDown[256] = {};
		const bool shiftHeld = keys[VK_SHIFT] != 0;

		io.AddKeyEvent(ImGuiMod_Ctrl,  keys[VK_CONTROL] != 0);
		io.AddKeyEvent(ImGuiMod_Shift, shiftHeld);
		io.AddKeyEvent(ImGuiMod_Alt,   keys[VK_MENU] != 0);

		for (int vk = 0; vk < 256; ++vk)
		{
			const bool down = keys[vk] != 0;

			if (const ImGuiKey key = vkToImGuiKey(vk); key != ImGuiKey_None)
				io.AddKeyEvent(key, down);

			// Text on the press edge only, so a held key does not spray characters.
			if (down && !sPrevKeyDown[vk])
			{
				BYTE kbState[256] = {};
				if (shiftHeld) kbState[VK_SHIFT] = 0x80;
				if (keys[VK_CAPITAL]) kbState[VK_CAPITAL] = 0x01;
				WCHAR chars[4] = {};
				const int produced = ::ToUnicode(vk, ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), kbState, chars, 4, 0);
				for (int i = 0; i < produced; ++i)
					if (chars[i] >= 0x20 && chars[i] != 0x7F)
						io.AddInputCharacter((unsigned int)chars[i]);
			}
			sPrevKeyDown[vk] = down;
		}
	}

	// ⚠ CONTROLLER. Gamepad state normally reaches ImGui from imgui_impl_win32, which polls XInput - and we
	// skip that backend entirely in windowless mode, so HCM's controller hotkey binds had no source at all
	// and silently never fired.
	//
	// XInput is loaded dynamically rather than linked: it keeps the DLL free of a hard dependency, lets a
	// machine without the runtime carry on with keyboard and mouse, and avoids a load-time failure inside
	// the game's process.
	{
		typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
		static XInputGetStateFn xinputGetState = nullptr;
		static bool xinputTried = false;
		if (!xinputTried)
		{
			xinputTried = true;
			for (const wchar_t* dll : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
			{
				if (HMODULE m = ::LoadLibraryW(dll))
				{
					xinputGetState = (XInputGetStateFn)::GetProcAddress(m, "XInputGetState");
					if (xinputGetState)
					{
						PLOG_INFO << "Windowless input: controller support via " << wstr_to_str(dll);
						break;
					}
				}
			}
			if (!xinputGetState)
				PLOG_WARNING << "Windowless input: no XInput runtime found; controller hotkeys will not work.";
		}

		if (xinputGetState)
		{
			// XINPUT_STATE laid out by hand so no XInput header/lib is needed.
			struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } state{};

			// ⚠ SWEEP ALL FOUR SLOTS. Polling only index 0 is a real bug, not a simplification: a pad that
			// has been reconnected, or that shares the machine with another XInput device, routinely lands
			// on slot 1-3 and index 0 then returns ERROR_DEVICE_NOT_CONNECTED forever.
			static int sPadIndex = -1;
			bool haveState = false;
			if (sPadIndex >= 0)
				haveState = (xinputGetState(sPadIndex, &state) == ERROR_SUCCESS);
			if (!haveState)
			{
				for (int i = 0; i < 4; ++i)
				{
					if (xinputGetState(i, &state) == ERROR_SUCCESS)
					{
						if (sPadIndex != i)
						{
							sPadIndex = i;
							PLOG_INFO << "Windowless input: controller found on XInput slot " << i;
						}
						haveState = true;
						break;
					}
				}
			}

			static uint32_t sPadDiag = 0;
			if (haveState && (sPadDiag++ % 240) == 0 && state.buttons != 0)
				PLOG_INFO << "[pad-diag] slot " << sPadIndex << " buttons 0x" << std::hex << state.buttons
					<< std::dec << " lt " << (int)state.lt << " rt " << (int)state.rt;

			if (haveState)
			{
				auto btn = [&](WORD mask) { return (state.buttons & mask) != 0; };
				io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    btn(0x0001));
				io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  btn(0x0002));
				io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  btn(0x0004));
				io.AddKeyEvent(ImGuiKey_GamepadDpadRight, btn(0x0008));
				io.AddKeyEvent(ImGuiKey_GamepadStart,     btn(0x0010));
				io.AddKeyEvent(ImGuiKey_GamepadBack,      btn(0x0020));
				io.AddKeyEvent(ImGuiKey_GamepadL3,        btn(0x0040));
				io.AddKeyEvent(ImGuiKey_GamepadR3,        btn(0x0080));
				io.AddKeyEvent(ImGuiKey_GamepadL1,        btn(0x0100));
				io.AddKeyEvent(ImGuiKey_GamepadR1,        btn(0x0200));
				io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  btn(0x1000));   // A
				io.AddKeyEvent(ImGuiKey_GamepadFaceRight, btn(0x2000));   // B
				io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  btn(0x4000));   // X
				io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    btn(0x8000));   // Y

				// Triggers and sticks are analog; ImGui wants a 0..1 value with a deadzone applied.
				auto analog = [&](ImGuiKey key, float v) { io.AddKeyAnalogEvent(key, v > 0.10f, v); };
				analog(ImGuiKey_GamepadL2, state.lt / 255.0f);
				analog(ImGuiKey_GamepadR2, state.rt / 255.0f);

				auto stick = [&](ImGuiKey neg, ImGuiKey pos, SHORT raw)
					{
						const float v = (float)raw / 32767.0f;
						analog(neg, v < -0.25f ? -v : 0.0f);
						analog(pos, v >  0.25f ?  v : 0.0f);
					};
				stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, state.lx);
				stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp,    state.ly);
				stick(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, state.rx);
				stick(ImGuiKey_GamepadRStickDown, ImGuiKey_GamepadRStickUp,    state.ry);
			}
		}
	}

	// Everything else is replayed from the raw input queue, in the order it happened.
	std::vector<WindowlessInputEvent> events;
	{
		std::scoped_lock queueLock(sRawQueueMutex);
		events.swap(sRawQueue);
	}

	for (const auto& e : events)
	{
		switch (e.kind)
		{
		case WindowlessInputEvent::Kind::MouseButton:
			io.AddMouseButtonEvent(e.code, e.down);
			break;
		case WindowlessInputEvent::Kind::Wheel:
			io.AddMouseWheelEvent(0.0f, e.wheel);
			break;
		case WindowlessInputEvent::Kind::Char:
			io.AddInputCharacter((unsigned int)e.code);
			break;
		case WindowlessInputEvent::Kind::Key:
			if (const ImGuiKey key = vkToImGuiKey(e.code); key != ImGuiKey_None)
				io.AddKeyEvent(key, e.down);
			break;
		}
	}

	// Mouse buttons ride in on the forwarded key state - they are just virtual keys 1/2/4, and HCMExternal
	// polls all 256. See the note in initializeImGuiContextAndPlatform for why raw input is not used.
	if (const unsigned char* keys = SharedMemoryInternal::forwardedKeyStates())
	{
		io.AddMouseButtonEvent(0, keys[VK_LBUTTON] != 0);
		io.AddMouseButtonEvent(1, keys[VK_RBUTTON] != 0);
		io.AddMouseButtonEvent(2, keys[VK_MBUTTON] != 0);
	}
}

// The backend-independent half of ImGui bring-up: WndProc chain, context, io flags and the win32
// platform backend. Identical for D3D11 and D3D12 - only the renderer backend differs.
void ImGuiManager::initializeImGuiContextAndPlatform(HWND windowHandle)
{
	m_windowHandle = windowHandle;
	mWindowless = (windowHandle == nullptr);

	if (mWindowless)
	{
		// No window means no message queue to hook. Skip the subclass and the win32 platform backend
		// entirely - ImGui_ImplWin32_Init would fail on a null HWND and take initialisation down with it.
		// See the note on mWindowless in the header for why this target genuinely has no window.
		PLOG_INFO << "ImGuiManager: this target has no window (composition swapchain). Skipping the WndProc "
			"subclass and imgui_impl_win32; input will be synthesised each frame instead.";

		ImGui::SetCurrentContext(ImGui::CreateContext());
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;

		// No OS cursor is composited over the game, so ImGui has to draw its own or the user is aiming blind.
		io.MouseDrawCursor = true;
		// ⚠ Never write files from inside the AppContainer sandbox - imgui.ini would land in the package's
		// private AC folder at best and throw at worst.
		io.IniFilename = nullptr;
		// Nothing feeds the platform backend's timing, so ImGui would divide by a zero delta.
		io.DeltaTime = 1.0f / 60.0f;

		// ⚠⚠⚠ DO NOT START RAW INPUT HERE. IT STEALS THE GAME'S OWN INPUT.
		// RegisterRawInputDevices is scoped PER PROCESS, not per window: "only one window per raw input
		// device class can be registered to receive raw input within a process". Registering mouse and
		// keyboard from inside the game therefore REPLACED THE GAME'S OWN REGISTRATION and redirected every
		// WM_INPUT to our message-only window. The player could no longer aim or move - we broke the game's
		// input in order to read input.
		//
		// It is also unnecessary. HCMExternal already forwards the whole 256-byte key state, and
		// GetAsyncKeyState covers VK_LBUTTON/VK_RBUTTON/VK_MBUTTON (virtual keys 1/2/4), so mouse BUTTONS
		// arrive on that path for free. Position comes from GetCursorPos, which works because we release the
		// game's cursor clip while the menu is open.
		//
		// The one thing lost is the mouse wheel, which has no GetAsyncKeyState equivalent. That is a fair
		// trade for not disabling the player's controls.
		//
		// ImGui WIPES every gamepad key in NewFrame unless the backend advertises a pad, so say so here or
		// controller binds silently never fire (UpdateKeyboardInputs: "Clear gamepad data if disabled").
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		return;
	}

	// ⚠⚠⚠ ADOPT, DO NOT RE-SUBCLASS. If a previous session left our proc installed (see the teardown
	// below: something subclassed above us, so removing ours would have deleted them from the chain),
	// then our proc is STILL LIVE in this window's chain. Subclassing again would set mOldWndProc - a
	// static, shared across sessions - to a proc that eventually calls back into ours, and every single
	// window message would recurse until the stack died.
	//
	// The flag rather than "is our proc on top?" is deliberate: when we were left installed it is
	// precisely because we are NOT on top, so comparing against the current proc would miss it and
	// subclass anyway. Our old proc is still in the chain, still forwarding through the mOldWndProc it
	// was installed with, and the new session simply reuses it.
	if (mWndProcLeftInstalled.load(std::memory_order_acquire)
		&& mWndProcInstalledOn.load(std::memory_order_acquire) == m_windowHandle)
	{
		PLOG_INFO << "ImGuiManager: adopting the window procedure a previous session left installed on this "
			"same window (mOldWndProc kept at 0x" << std::hex << (uintptr_t)mOldWndProc << std::dec << ")";
	}
	else
	{
		if (mWndProcLeftInstalled.load(std::memory_order_acquire))
			PLOG_WARNING << "ImGuiManager: a previous session left our proc on HWND 0x" << std::hex
				<< (uintptr_t)mWndProcInstalledOn.load(std::memory_order_acquire) << ", but this session's window "
				"is 0x" << (uintptr_t)m_windowHandle << std::dec << "; subclassing the new one";
		// Setup the imgui WndProc callback
		mOldWndProc = (WNDPROC)SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)&mNewWndProc);
		mWndProcInstalledOn.store(m_windowHandle, std::memory_order_release);
		mWndProcLeftInstalled.store(false, std::memory_order_release);
	}


	// Setup ImGui stuff
	PLOG_DEBUG << "Initializing ImGui";
	// ⚠ SET IT CURRENT. CreateContext RESTORES the previously-current context, so with the return value
	// discarded a second session kept running against session 1's destroyed context - and that defeats
	// mNewWndProc's ImGui::GetCurrentContext() guard, which is the check that stops the documented
	// crash-when-RivaTuner-is-closed.
	ImGui::SetCurrentContext(ImGui::CreateContext());
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;// | ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

	if (!ImGui_ImplWin32_Init(m_windowHandle))
	{
		throw HCMInitException(std::format("ImGui_ImplWin32_Init failed w/ {} ", (uint64_t)m_windowHandle).c_str());
	};
}

void ImGuiManager::initializeImGuiResources(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	PLOG_VERBOSE << "initializeImGuiResources";
	// Use swap chain description to get MCC window handle
	DXGI_SWAP_CHAIN_DESC swapChainDesc;
	pSwapChain->GetDesc(&swapChainDesc);
	if (!&swapChainDesc) throw HCMInitException("Failed to get swap chain description");

	initializeImGuiContextAndPlatform(swapChainDesc.OutputWindow);

	if (!ImGui_ImplDX11_Init(pDevice, pDeviceContext))
	{
		throw HCMInitException(std::format("ImGui_ImplDX11_Init failed w/ {}, {} ", (uint64_t)pDevice, (uint64_t)pDeviceContext).c_str());
	};

	PLOG_DEBUG << "ImGui Initialized";

	applyImGuiStyleAndFonts();
}


// Halo Campaign Evolved / D3D12. Everything the dx12 renderer backend needs (device, command queue,
// SRV heap, frames-in-flight, RTV format, and the SRV sub-allocator callbacks) lives on D3D12Hook,
// so we pull a ready-made ImGui_ImplDX12_InitInfo off it rather than widening the present event
// with init-only data.
void ImGuiManager::initializeImGuiResourcesD3D12(IDXGISwapChain3* pSwapChain)
{
	PLOG_VERBOSE << "initializeImGuiResourcesD3D12";

	auto d3d12 = m_d3d12.lock();
	if (!d3d12) throw HCMInitException("D3D12Hook was already destroyed when ImGui tried to initialize");

	// Prefer the hook's window handle: it is the one it validated with isOwnedByThisProcess, so we
	// can never end up hooking the WndProc of some other process's window. Fall back to the
	// swapchain description (which is what the D3D11 path does) if the hook somehow has none.
	HWND windowHandle = d3d12->getWindowHandle();
	if (!windowHandle && pSwapChain)
	{
		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc)))
			windowHandle = swapChainDesc.OutputWindow;
	}
	// ⚠ NO WINDOW IS A LEGITIMATE OUTCOME - DO NOT THROW HERE. A composition swapchain has no HWND, and on
	// Halo 5: Forge the process owns no window at all, so this used to abort initialisation for the one
	// target that most needs the overlay. initializeImGuiContextAndPlatform handles null by switching to
	// synthesised input; see mWindowless.
	if (!windowHandle)
		PLOG_INFO << "No window handle available for the D3D12 swapchain - entering windowless mode.";

	ImGui_ImplDX12_InitInfo initInfo{};
	if (!d3d12->getImGuiInitInfo(initInfo))
		throw HCMInitException("D3D12Hook could not supply an ImGui_ImplDX12_InitInfo");

	initializeImGuiContextAndPlatform(windowHandle);

	if (!ImGui_ImplDX12_Init(&initInfo))
	{
		throw HCMInitException(std::format("ImGui_ImplDX12_Init failed w/ device {}, queue {}, srvHeap {}, framesInFlight {} ",
			(uint64_t)initInfo.Device, (uint64_t)initInfo.CommandQueue, (uint64_t)initInfo.SrvDescriptorHeap, initInfo.NumFramesInFlight).c_str());
	};

	PLOG_DEBUG << "ImGui Initialized (D3D12)";

	applyImGuiStyleAndFonts();

	// ⚠ Build the root signature, PSO and FONT TEXTURE here, with timing in the log, instead of letting the
	// first ImGui_ImplDX12_NewFrame() do it silently from inside the game's Present.
	//   1. The font texture does a BLOCKING GPU round trip. When this hung, the log's last line was
	//      "ImGui Initialized (D3D12)" and there was nothing to say which of ~6 candidate calls was stuck -
	//      it cost a 242-session census to localise. Now it is one line either way.
	//   2. NewFrame's lazy path is `if (!bd->pPipelineState) CreateDeviceObjects()`, so doing it once here
	//      means the per-frame path never carries this cost or this risk again.
	// applyImGuiStyleAndFonts must stay BEFORE this: it queues the fonts that CreateFontsTexture rasterises.
	// Healthy baseline for the window this replaces: 7-25 ms, median 9 (measured over 235 sessions).
	PLOG_DEBUG << "Creating the D3D12 ImGui device objects (root signature, PSO, font texture)";
	const auto deviceObjectsStart = std::chrono::steady_clock::now();
	if (!ImGui_ImplDX12_CreateDeviceObjects())
		throw HCMInitException("ImGui_ImplDX12_CreateDeviceObjects failed");
	PLOG_INFO << "D3D12 ImGui device objects created in "
		<< std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now() - deviceObjectsStart).count() << " ms";
}


// Style + fonts. Shared verbatim by both backends so HCM's GUI is pixel-identical on MCC and HCE.
void ImGuiManager::applyImGuiStyleAndFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	ImGuiStyle* style = &ImGui::GetStyle();
	style->FrameRounding = 0;
	style->WindowBorderSize = 0;
	style->WindowRounding = 0;
	style->FrameBorderSize = 1;

	// Colors
	style->Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.04f, 0.08f, 1.00f);
	style->Colors[ImGuiCol_ChildBg] = ImVec4(0.18f, 0.10f, 0.18f, 0.50f);
	style->Colors[ImGuiCol_Text] = ImVec4(0.80f, 0.90f, 0.90f, 1.00f);
	style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.29f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_Border] = ImVec4(0.40f, 0.50f, 0.50f, 0.38f);
	style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);

	style->Colors[ImGuiCol_FrameBg] = ImVec4(0.35f, 0.09f, 0.12f, 1.00f);
	//style->Colors[ImGuiCol_FrameBg] = ImVec4(0.95f, 0.99f, 0.12f, 1.00f);
	style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.55f, 0.23f, 0.29f, 1.00f);
	style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.81f, 0.23f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.9f, 0.6f, 0.7f);
	style->Colors[ImGuiCol_Button] = ImVec4(0.30f, 0.09f, 0.12f, 1.00f);
	style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.23f, 0.29f, 1.00f);
	style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.23f, 0.29f, 1.00f);

	style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.75f, 0.43f);

	style->Colors[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.70f, 0.73f, 0.31f);
	style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.26f, 0.05f, 0.07f, 1.00f);

	style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.f, 0.f, 0.f, 0.f);
	style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.35f, 0.09f, 0.12f, 0.7f);

	style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.1f, 0.00f, 0.1f, 0.3f);

	style->IndentSpacing = 10.f;

	// setup font
	io.Fonts->AddFontDefault();
	mRescalableMonospacedFont = io.Fonts->AddFontFromMemoryCompressedTTF(ProggyVectorRegularFont_compressed_data, ProggyVectorRegularFont_compressed_size, 15.f * 2);

	//int my_image_width = 0;
	//int my_image_height = 0;

	//bool ret = LoadTextureFromMemory(103, pDevice, &my_texture, &my_image_width, &my_image_height);
	//// todo; overlay bypass stuff based on return value
	//IM_ASSERT(ret);


}


ImGuiManager::~ImGuiManager()
{
	PLOG_DEBUG << "~ImGuiManager()";
	// Set BEFORE mDestructionGuard is taken, so a present handler that is already inside its body and
	// merely waiting on the guard sees it and bails instead of resuming into a shut-down backend.
	mShuttingDown.store(true, std::memory_order_release);
	//presentEventCallback.~ScopedCallback();
	if (presentEventCallback) presentEventCallback->removeCallback(); // no new callback invokes
	if (presentEventCallbackD3D12) presentEventCallbackD3D12->removeCallback();
	// ⚠ A DESTROYED WINDOW IS NOT A THIRD-PARTY SUBCLASS. GetWindowLongPtrW on a dead HWND returns 0,
	// which compares unequal to our proc and would latch mWndProcLeftInstalled with nobody else involved.
	// HaloCER recreates its window mid-session, so this is the common case there, not a corner.
	if (mOldWndProc && (!m_windowHandle || !IsWindow(m_windowHandle)))
	{
		PLOG_INFO << "ImGuiManager: our window is gone; there is no chain to restore and none to stay in.";
		mOldWndProc = nullptr;
		mWndProcInstalledOn.store(nullptr, std::memory_order_release);
		mWndProcLeftInstalled.store(false, std::memory_order_release);
	}
	else if (mOldWndProc) 		// restore the original wndProc
	{
		// ⚠ ONLY RESTORE IF WE ARE STILL THE TOP OF THE CHAIN. WndProc subclassing is a linked list, and this
		// used to write mOldWndProc back unconditionally. If anything subclassed AFTER us - RTSS, the Steam
		// overlay, Streamline, another injector - then the window's current proc is THEIRS, and overwriting it
		// with our saved pointer silently deletes them from the chain and hands the game a proc that skips
		// their state. That corrupts a process we do not own, and it is exactly the class of "HCM closed and
		// the game died later" bug this whole investigation is about.
		//
		// If we are not on top, the safe move is to leave the chain alone: our proc stays live, and the
		// residency drain below (plus the GlobalKill early-out at the top of mNewWndProc) keeps it harmless.
		const LONG_PTR current = GetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC);
		if (current == (LONG_PTR)&ImGuiManager::mNewWndProc)
		{
			SetWindowLongPtrW(m_windowHandle, GWLP_WNDPROC, (LONG_PTR)mOldWndProc);
			mOldWndProc = nullptr;
			// We really did come off, so the next session must subclass again rather than adopt.
			mWndProcLeftInstalled.store(false, std::memory_order_release);
		}
		else
		{
			PLOG_ERROR << "ImGuiManager: NOT restoring the window procedure - something else subclassed after "
				"us (current proc is 0x" << std::hex << current << std::dec << ", ours is not on top). "
				"Restoring would delete that subclass from the chain. Our proc stays installed and inert; "
				"HCMInternal.dll must therefore not be unloaded from under it.";
			mWndProcInstalledOn.store(m_windowHandle, std::memory_order_release);
			mWndProcLeftInstalled.store(true, std::memory_order_release);
		}
	}

	std::unique_lock<std::mutex> lock(mDestructionGuard); // block until callbacks finish executing



	// Cleanup
	if (m_isImguiInitialized)
	{
		if (mBackend == RenderBackend::D3D12)
		{
			// MANDATORY GPU FLUSH BEFORE ImGui_ImplDX12_Shutdown().
			// ImGui_ImplDX12_Shutdown -> InvalidateDeviceObjects releases imgui's PSO, root
			// signature, font texture and the whole per-frame vertex/index buffer ring, with no GPU
			// wait of its own (the dx12 backend never fences on shutdown). D3D12 does NOT defer
			// destruction until the GPU is done with a resource, and the very last thing D3D12Hook
			// submitted was an overlay draw referencing exactly those objects. Freeing them while
			// that command list is still executing is a GPU-side use-after-free -> device removed ->
			// the GAME dies, which is precisely the failure mode we must never cause.
			//
			// App.h declares the graphics hook BEFORE `imm`, so the hook is guaranteed to still be
			// alive here (destruction is reverse declaration order). If the flush cannot be PROVEN
			// (Signal failed, or the wait timed out - both mean device removal or a hung GPU) we
			// deliberately LEAK imgui's dx12 backend rather than free memory the GPU may be reading.
			// That is the same "abandon rather than free" policy D3D12Hook uses internally.
			bool gpuIsIdle = false;
			if (auto d3d12 = m_d3d12.lock())
			{
				gpuIsIdle = d3d12->waitForGpuIdle();
			}
			else
			{
				PLOG_ERROR << "D3D12Hook was already destroyed at ~ImGuiManager; cannot prove the GPU is idle";
			}

			if (gpuIsIdle)
			{
				ImGui_ImplDX12_Shutdown();
				// ⚠ Only shut down the win32 backend if we ever started it - see mWindowless.
				if (!mWindowless) ImGui_ImplWin32_Shutdown();
				ImGui::DestroyContext();
			}
			else
			{
				// Leak the renderer backend AND the context: DestroyContext with a live renderer
				// backend still attached trips imgui's "forgot to shutdown backend" assert and frees
				// the font atlas the backend's texture still refers to. A leak in a process that is
				// about to lose HCM entirely is free; a GPU fault is not.
				PLOG_FATAL << "Could not prove the GPU was idle; abandoning (leaking) the ImGui D3D12 backend rather than freeing resources the GPU may still be reading";
				if (!mWindowless) ImGui_ImplWin32_Shutdown();
			}

			// ⚠ UNCONDITIONAL, and BEFORE the DLL can unmap: that thread owns a window whose WndProc lives
			// in this image, so a message dispatched to it after we unload kills the game. It must run on
			// BOTH the idle and the not-idle path - which is why it sits after the if/else and not inside
			// one branch. (Written as a bare `if (mWindowless) ...;` immediately before an `else`, it binds
			// to that else and silently rewires the whole block.)
			if (mWindowless) stopWindowlessRawInput();
		}
		else
		{
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			// Nothing may run against a destroyed context; leaving it current is what let a straggling
			// WndProc message walk into freed memory.
			ImGui::SetCurrentContext(nullptr);
		}

		m_isImguiInitialized = false;
	}

	instance = nullptr;
}



//void ImGuiManager::lapuaTest(SimpleMath::Vector2 ss)
//{
//	constexpr auto strength = 0x01FFFFFF;
//	constexpr auto zero = ImVec2(0, 0);
//	constexpr auto one = ImVec2(1, 1);
//	ImGui::GetBackgroundDrawList()->AddImage(my_texture, zero, ss, zero, one, strength);
//
//}

void ImGuiManager::onPresentHookEvent(ID3D11Device* pDevice, ID3D11DeviceContext* pDeviceContext, IDXGISwapChain* pSwapChain, ID3D11RenderTargetView* pMainRenderTargetView)
{
	LOG_ONCE(PLOG_DEBUG << "ImGuiManager::onPresentHookEvent running");
	// ⚠ CHECK EITHER SIDE OF THE LOCK. ~ImGuiManager sets instance = nullptr INSIDE mDestructionGuard, so
	// a handler that was already waiting on it is guaranteed to resume with instance null and then
	// dereference it. The D3D12 twin below already does this; this one did not.
	if (mShuttingDown.load(std::memory_order_acquire)) return;
	std::unique_lock<std::mutex> lock(mDestructionGuard);
	if (mShuttingDown.load(std::memory_order_acquire)) return;
#pragma region init


	if (!instance->m_isImguiInitialized)
	{
		PLOG_INFO << "Initializing ImGuiManager";
		try
		{
			instance->initializeImGuiResources(pDevice, pDeviceContext, pSwapChain, pMainRenderTargetView);
			instance->m_isImguiInitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize ImGui, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}
#pragma endregion init

	//TODO: check if this is necessary
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);

	}


	// Start ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	GImGui->NavWindowingTarget = nullptr; // prevent navinputs altogether

	// I don't 100% understand what this does, but it must be done before we try to render
	pDeviceContext->OMSetRenderTargets(1, &pMainRenderTargetView, NULL);
	auto screenSize = getScreenSize();


	// invoke callback of anything that wants to render with ImGui
	BackgroundRenderEvent->operator()(screenSize); // for overlays.
	MidgroundRenderEvent->operator()(screenSize);
	ForegroundRenderEvent->operator()(screenSize);
	ForegroundDirectXRenderEvent->operator()(pDevice, pDeviceContext, screenSize, pMainRenderTargetView);

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();

	// Render it !
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}


// Halo Campaign Evolved / D3D12 equivalent of onPresentHookEvent above.
//
// Contract with D3D12Hook (see D3D12Hook.h): pCommandList is already Reset() and recording, the
// back buffer is already in RENDER_TARGET state, OMSetRenderTargets(backBufferRTV) and
// SetDescriptorHeaps(srvHeap) have already been issued on it, and we must NOT Close(), Execute() or
// Signal() - the hook owns all of that. Which is exactly why there is no OMSetRenderTargets here:
// the D3D11 path's `pDeviceContext->OMSetRenderTargets(...)` has already been done for us, by the
// only code that knows which back buffer this frame is.
void ImGuiManager::onPresentHookEventD3D12(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, IDXGISwapChain3* pSwapChain, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV)
{
	LOG_ONCE(PLOG_DEBUG << "ImGuiManager::onPresentHookEventD3D12 running");
	if (mShuttingDown.load(std::memory_order_acquire)) return; // don't even queue up behind the destructor
	std::unique_lock<std::mutex> lock(mDestructionGuard);
	// Authoritative check: the destructor sets the flag BEFORE it takes the guard, so if it started
	// while we were blocked above, we see it here and get out without touching the imgui backend.
	if (mShuttingDown.load(std::memory_order_acquire)) return;
#pragma region init


	// `this`, not `instance` (which the D3D11 handler uses): they are always the same object, but the
	// subscription is published a hair before `instance` is, so `this` is the one that is never null.
	if (!m_isImguiInitialized)
	{
		PLOG_INFO << "Initializing ImGuiManager (D3D12)";
		try
		{
			initializeImGuiResourcesD3D12(pSwapChain);
			m_isImguiInitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize ImGui, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}
#pragma endregion init

	//TODO: check if this is necessary
	// NOTE: this dispatches window messages, so the game's WndProc - and therefore ResizeBuffers -
	// can run RE-ENTRANTLY from right here. D3D12Hook handles that: it samples mResourceGeneration
	// around this event and abandons the frame (closing the command list) rather than submitting one
	// that references a back buffer released underneath us. It also deliberately does NOT hold its
	// resource mutex across this event, so the re-entry cannot deadlock.
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
	{
		::TranslateMessage(&msg);
		::DispatchMessage(&msg);

	}


	// Start ImGui frame
	ImGui_ImplDX12_NewFrame();
	if (mWindowless)
	{
		// No WM_ messages exist to consume, so drive IO directly. Size comes from the swapchain because
		// there is no client rect to measure.
		DXGI_SWAP_CHAIN_DESC windowlessDesc{};
		if (pSwapChain && SUCCEEDED(pSwapChain->GetDesc(&windowlessDesc)))
			synthesiseWindowlessInput(windowlessDesc.BufferDesc.Width, windowlessDesc.BufferDesc.Height);
	}
	else
	{
		ImGui_ImplWin32_NewFrame();
	}
	ImGui::NewFrame();

	GImGui->NavWindowingTarget = nullptr; // prevent navinputs altogether

	// (no OMSetRenderTargets - D3D12Hook already bound backBufferRTV on pCommandList)
	auto screenSize = getScreenSize();


	// invoke callback of anything that wants to render with ImGui.
	// The three RenderEvents below are backend-agnostic (they only carry the screen size) and carry
	// HCM's entire GUI, so everything that renders on MCC renders here too.
	BackgroundRenderEvent->operator()(screenSize); // for overlays.
	MidgroundRenderEvent->operator()(screenSize);
	ForegroundRenderEvent->operator()(screenSize);
	// ForegroundDirectXRenderEvent is deliberately NOT fired: its payload is live ID3D11Device /
	// ID3D11DeviceContext / ID3D11RenderTargetView pointers which do not exist under D3D12, and every
	// one of its subscribers dereferences them immediately. Its D3D12 counterpart is fired instead.
	// The back buffer format comes off the hook: a D3D12 subscriber that records its own draws needs
	// it to build a PSO, and it cannot be recovered from the RTV descriptor handle. DXGI_FORMAT_UNKNOWN
	// if the hook has gone away, which subscribers must treat as "do not render this frame".
	DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
	if (auto d3d12 = m_d3d12.lock()) backBufferFormat = d3d12->getRenderTargetFormat();
	ForegroundD3D12RenderEvent->operator()(pDevice, pCommandList, screenSize, backBufferRTV, backBufferFormat);

	// Finish ImGui frame
	ImGui::EndFrame();
	ImGui::Render();

	// Must be after Render(): the windowless input path reads this next frame to decide whether to take
	// the cursor back off the game.
	if (mWindowless) refreshOverlayVisibility();

	// Render it !
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pCommandList);
}


