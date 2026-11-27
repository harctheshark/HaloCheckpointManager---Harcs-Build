#pragma once
#include "pch.h"

// Metro Redux (2026-11-25, user: "i cannot click the menu"): metro.exe reads the mouse and keyboard through DirectInput 8
// (its only input import besides GetAsyncKeyState) and re-centres the OS cursor with SetCursorPos every frame, so while the
// game has the mouse, Windows never moves the cursor and HCM's window gets no mouse at all.
//
// This gate hooks dinput8's device methods (shared by every device - found from a device of HCM's own) and user32's
// SetCursorPos:
//   mouse released (HCM's free cursor is requested): the game's mouse device is unacquired and reads as "no movement, no
//     buttons", its Acquire calls succeed without acquiring, and its SetCursorPos calls do nothing -> the OS cursor is
//     free for HCM's window;
//   input blocked (HCM's Block Input): the same for the keyboard too (reads as "no keys down").
// Nothing is changed while neither is requested. Installed on first use; never removed (HCM stays resident: the hooks stay,
// pass-through, so their trampolines stay valid).
namespace MetroInput
{
	void setMouseReleased(bool released);
	void setInputBlocked(bool blocked);
}
