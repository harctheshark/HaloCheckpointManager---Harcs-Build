#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo 5: Forge - PAUSE MENU FIX.
//
// Removes the ~3 second cooldown the engine imposes before the start (pause) menu may be reopened.
//
// WHAT THE COOLDOWN ACTUALLY IS
// The start-menu controller-input handler (gui_screen_start_menu.cpp, exe+0x01D382B0 - it carries its own
// assert string "start_menu: controller not in use.") gates the menu open on:
//
//     allow = (game_time() - lastOpened) > 3.0f  ||  lastOpened > game_time()
//
//     exe+0x01D38361  call  game_time            (exe+0x0131A770; sim tick * dt, so it is 3 seconds of
//     exe+0x01D38366  movss xmm2, [lastOpened]    UNPAUSED gameplay, not wall clock)
//     exe+0x01D38371  subss xmm1, xmm2
//     exe+0x01D38375  comiss xmm1, [3.0f]
//     exe+0x01D3837C  ja    allow
//     exe+0x01D3837E  comiss xmm2, xmm0
//     exe+0x01D38381  jbe   return false
//     exe+0x01D38389  movss [lastOpened], xmm0   <- re-latches on every successful open
//     exe+0x01D38393  call  open_start_menu
//
// So we neutralise it by keeping `lastOpened` far in the past. Writing -1000.0f makes the delta at least
// 1000 at any game time, so the first compare always allows.
//
// ⚠⚠ IT MUST BE WRITTEN EVERY TICK, NOT ONCE. exe+0x01D38389 latches the current game time back into the
// global on every successful open, so a one-shot write buys exactly one un-throttled open.
//
// ⚠⚠⚠ NEVER LET A NaN OR INF REACH THIS GLOBAL. `comiss` against NaN sets ZF=PF=CF=1, so `ja` is NOT taken
// AND the fallback `jbe` IS taken - the menu would be blocked permanently, not unblocked. The value is a
// hard-coded literal for exactly this reason; do not compute it.
//
// ⚠ Why -1000.0f and not 0.0f: with 0.0f the gate still blocks during the first ~3 seconds of game time
// after a level load, and also fails if game_time() returns 0 (which it does on a thread with no sim TLS).
//
// WHY THIS IS SAFE
// The target has EXACTLY TWO references in the whole 122MB image - the read and the write above. That was
// established by the prebuilt xref database and then re-proved independently by an opcode-agnostic sweep of
// every rip-relative displacement in the code section, which also found no absolute pointer to it anywhere.
// It is a 4-byte naturally-aligned float in a READWRITE page, so a single store cannot tear against the
// engine's own movss. Nothing else in the game can observe the value.
//
// ⚠ We do NOT patch the 3.0f constant at exe+0x033200A0: it is the module's shared 3.0f pool constant with
// 268 referencing instructions, and it lives in a PAGE_READONLY region.
// ⚠ We do NOT patch the `ja` to a `jmp` either. That would work (2 bytes, 77 09 -> EB 09) but it needs
// VirtualProtect on an executable page and must be reverted exactly; a data write to a global nothing else
// reads is strictly less dangerous, and in this title every pure data write has worked while every attempt
// to touch code or call engine functions has ended in a crash.
//
// ⚠ NOT YET VERIFIED IN GAME. Every step above is static disassembly plus passive live reads; the causal
// link "write the global -> the menu reopens instantly" has not been tested, because the game was left
// running unattended and a crash was unrecoverable. Treat the first run as the test.
// ================================================================================================================
class H5PauseMenuFix : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5PauseMenuFix(GameState game, IDIContainer& dicon);
	~H5PauseMenuFix();
	std::string_view getName() override { return nameof(H5PauseMenuFix); }
};
