#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "HCEScriptRegistry.h"

// ================================================================================================================
// A HALOSCRIPT CONSOLE FOR HALO CAMPAIGN EVOLVED.
//
// HCM already has a console for the MCC titles, but it runs through
// GameEngineFunctions::SendCommand -> GameEngineDetail::getCommonHandle()->execute_command("HS: ..."), and that
// handle is MCC's. HaloCER is a separate UE5 title with no such thing, which is why consoleCommandGUI is
// declared ALL_SUPPORTED_GAMES and not ..._AND_HALOCER. This reaches the sim's own script engine instead.
//
// THE ENTRY POINT: hs_console_execute, sim+0x1F8710
// ------------------------------------------------
//     void __fastcall hs_console_execute(void* unused, const char* sourceName,
//                                        const char* command, bool determinismRecord);
// arg1 is dead (rcx is never read). Two shipped callers pass "console_command" and "shell_event" as the source
// name. determinismRecord must be false - true is the path that memcpys into a determinism record.
//
// ★ THE GAME PREPROCESSES BARE INPUT, so HCM does not build s-expressions. The preprocessor at sim+0x1F8470
// trims whitespace, drops everything from the first ';', and then wraps:
//     "cheat_bump_possession 1"    -> (set cheat_bump_possession 1)   [token 0 is a known global, args follow]
//     "cheat_bump_possession"      -> evaluated as a read, prints its value
//     "game_revert"                -> (game_revert)
//     "(a 1) (b 1)"                -> (begin (a 1) (b 1))             [more than one top-level group]
// A single fully-parenthesised group is passed through unchanged, so typing either form works.
// ⚠ The preprocessed text must be under 0x1000 bytes or hs_compile returns -1 immediately.
//
// ⚠⚠ THE SIMULATION THREAD IS MANDATORY.
// Every stage - hs_compile, hs_parse, hs_evaluate, hs_return, hs_global_set - reaches its state through the
// sim TLS block with NO null check, and there is no shipped thread assert to catch a mistake. Calling from an
// HCM thread is a fault, not a wrong answer. So commands are queued from the GUI and drained on
// HCEGameThreadPump, which is a midhook on the simulation thread itself. If the pump is unavailable on a
// build, this feature refuses to run rather than executing anywhere else.
//
// WHAT IS SAFE, AND WHAT IS NOT
// -----------------------------
//   * Wrong arity and wrong argument types CANNOT corrupt the stack - the parser enforces both before
//     anything is evaluated. A bad command is a compile error, not a crash.
//   * Compile errors land in a process-global sink (sim+0x2C2EF18 / +0x2C2EF14), which is unsynchronised;
//     one command at a time is drained per frame partly for that reason.
//   * `shell_frontend_allow_script_commands_without_game_in_progress` is a DEAD STUB - there is no shipped
//     no-game guard behind it. HCM checks the play state itself before draining.
//
// AUTOCOMPLETE comes from HCEScriptRegistry, which enumerates 1695 functions and 1475 globals out of .rdata
// and marks which ones are backed by a real implementation. 425 functions are a shared do-nothing stub, and
// 1278 globals have no C++ backing at all - including every cheat_*, which is why `cheat_bump_possession` can
// be set but will never do anything.
// ================================================================================================================

// The GUI cannot resolve a cheat - GUIElementConstructor is handed settings and nothing else - so the console
// publishes itself through this bridge instead of being injected. Same shape as HCEGameThreadPump: static
// storage, armed and disarmed by the owning cheat, and safe to call at any time because when it is disarmed
// every entry point simply reports "unavailable" rather than touching anything.
namespace HCEConsoleBridge
{
	bool isUsable();
	// false + a reason when the console cannot run the command right now.
	bool queue(const std::string& command, std::string& outWhy);
	// Copies out the suggestions; the corpus is built once and never mutated while armed.
	std::vector<const HCEScriptRegistry::Entry*> complete(std::string_view prefix, size_t limit);
	// For the one-line status the GUI shows before anything has been typed.
	HCEScriptRegistry::Census census();
}

class HCEConsole : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HCEConsole(GameState game, IDIContainer& dicon);
	~HCEConsole();
	virtual std::string_view getName() override { return nameof(HCEConsole); }

	// Queue a command for the next simulation frame. Returns false if the console is not usable right now
	// (no game running, or the pump did not resolve on this build) - the reason goes in outWhy.
	bool queueCommand(const std::string& command, std::string& outWhy);

	// The autocomplete corpus, built once the sim module is up. Empty until then.
	const std::vector<HCEScriptRegistry::Entry>& entries() const;
	HCEScriptRegistry::Census census() const;
	bool isUsable() const;
};
