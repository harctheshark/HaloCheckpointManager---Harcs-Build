#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include <string>

// ================================================================================================================
// A LUA CONSOLE FOR HALO 5: FORGE.
//
// Halo 5's campaign scripting is Lua - Havok Script 2013.1.0, a Lua 5.1 derivative - not HaloScript. So this
// shares nothing with the MCC console (GameEngineFunctions::SendCommand) or with HCEConsole (hs_console_execute).
// It compiles the typed text with the engine's own compiler and runs it on the engine's own script state, which
// means the game's 1100-odd script bindings marshal their own arguments and we write no per-function glue.
//
// WHERE IT RUNS: a midhook on sim tick, at exe+0x0088B410
// ------------------------------------------------------
// Halo 5 drives Lua from game_tick (0x01363B10) through 0x0088B740, which resumes two script contexts:
//
//     0088B755  cmp qword [0x05982AC8], 0    ; ctxA->L
//     0088B75F  lea  rcx, [0x05982AC0]       ; ctxA
//     0088B766  call 0x854960                ; resume
//     0088B76B  cmp qword [0x05982BD8], 0    ; ctxB->L
//     0088B775  lea  rcx, [0x05982BD0]       ; ctxB
//     0088B780  jmp  0x854960                ; TAIL CALL
//
// ★ We hook 0x0088B410 instead, which is `ret 0` + 13 int3 with exactly ONE caller - 0x01363CBF, immediately
// after `call 0x88b740` in game_tick. Hooking a no-op displaces zero engine work, and it sits after the whole
// script tick including 0x008514F0's deferred event flush. Measured live: fires at 59.8 Hz on one thread.
//
// ⚠ DO NOT hook the RETURN of 0x0088B740. ctxB is reached by a tail JUMP, so the lone `ret` at 0x0088B789 only
// executes when ctxB->L is null - which it is not, in a loaded level. A return hook there would never fire.
//
// ⚠⚠ g_currentScriptContext (0x059829F0) IS NULL BETWEEN RESUMES, AND THE ENGINE DEREFERENCES IT UNGUARDED:
//         008935E0  mov rcx, [0x059829F0]
//         008935E7  mov edx, [rcx + 0x18]        <-- null-deref
// 0x00854960 only sets it while a context is actually resuming. So any console chunk calling a bound function -
// object_create, log_message, GetCurrentThreadId, the Lua*Provider metatable hooks - would fault. We set and
// restore it ourselves, exactly as the engine's own out-of-band runner does at 0x00853F4C.
//
// WHICH CONTEXT: ctxA (0x05982AC0) is the SERVER
// ----------------------------------------------
// ctx+0xF8 is an IS_CLIENT flag: 0x0085A920 selects "remoteServer" when it is 0 and "remoteClient" when 1
// (siblings pick init/initClient and startup/startupClient). ctxB's static constructor writes 1 at 0x00013B56;
// ctxA's never writes the field. ctxA owns the campaign's game scripts, the live script-thread list, and the
// object-event plumbing, so that is the one a console must target.
//
// TWO EXECUTION MODES, AND WHY BOTH ARE NEEDED
// --------------------------------------------
// A "resume" is not one call - 0x00854960 is a cooperative scheduler that walks up to 256 "lua threads" and
// lua_resumes each ready one. Sleeping scripts stay suspended coroutines.
//   * Immediate  - luaL_loadbuffer then lua_pcall on the main state. Right for expressions and one-shot calls,
//     and the DEFAULT: it is the better-evidenced of the two.
//   * As a thread - leave the compiled chunk on the stack and hand it to the engine's own script-thread
//     runner (0x00853F10), which wraps it in a datum and resumes it now, leaving it for the tick scheduler
//     if it yields.
//     ⚠⚠ THAT RUNNER'S THIRD ARGUMENT IS A STACK COUNT, NOT A REGISTRY REF. Calling it with a luaL_ref
//     value crashed the game - luaL_ref pops the function, so the engine built a thread from an empty stack
//     and then read a non-thread as a lua_State, faulting at 0x00853B28 (`mov rdx,[rbx+0x48]`). The proof it
//     is a count is in the datum creator's failure path at 0x00848A66, which does
//     `L->top -= (arg + 1) * 16`, and in the engine's own caller at 0x00844810, which pushes the function
//     and its arguments then passes the count.
// ⚠ A chunk calling Sleep / SleepUntil / coroutine.yield CANNOT go through lua_pcall - on the main state that
// raises "attempt to yield across metamethod/C-call boundary". Most interesting campaign scripting is the
// sleepable kind, which is why the thread mode exists and is the default.
//
// WHAT IS SAFE, AND WHAT IS NOT
// -----------------------------
//   * The VM is idle 99.6% of wall clock (measured, 805,167 samples). At our hook the main stacks are at
//     top == base, ctx+0x10 == 0 and ctx+0x18 == -1. We assert the latter two anyway and skip if they disagree.
//   * ⚠ Lua errors unwind as MSVC C++ EXCEPTIONS, not longjmp. With L+0x90 (nested C-call depth) at zero an
//     error calls G->panic and NEVER RETURNS. Every call we make is therefore a protected one: luaL_loadbuffer
//     wraps the parser in its own luaD_pcall, and execution goes through lua_pcall / lua_resume. Nothing
//     unprotected is ever called, so no unwind can reach the safetyhook stub below us.
//   * ⚠ Status codes are NEGATIVE: 0 ok, -4 syntax, -5 file, -100 runtime, -200 memory, -300 error-in-error.
//     Testing `status > 0` would read every failure as success. We test != 0.
//   * ⚠ ctx->L is REALLOCATED PER LEVEL and is null outside one. Never cached - re-read every tick.
//   * The engine resets the main stack (L->top = L->base) at the start of each resume, so anything we leave
//     behind is dropped rather than corrupting. We restore top ourselves regardless.
//   * A secure-state gate exists at G+0x1C0 (value 2 refuses source; 0x007BBB65). It is 0 in every state this
//     build creates - single writer, default 0 - but we read it before compiling so a future build that turns
//     it on makes the console refuse cleanly instead of throwing.
//
// NOT IMPLEMENTED: autocomplete. Halo 5's binding names are registered at runtime rather than sitting in a
// table we can enumerate the way HCEScriptRegistry does, so there is no honest corpus to complete against yet.
// ================================================================================================================

// The GUI cannot resolve a cheat - GUIElementConstructor is handed settings and nothing else - so the console
// publishes itself through this bridge. Same shape as HCEConsoleBridge: static storage, armed and disarmed by
// the owning cheat, and safe to call at any time because when disarmed every entry point reports "unavailable"
// rather than touching game memory.
namespace H5LuaConsoleBridge
{
	bool isUsable();

	// Queue a chunk for the next sim tick. false + a reason when it cannot run right now.
	// asThread: run through the engine's script-thread runner so Sleep/SleepUntil/coroutine.yield work.
	bool queue(const std::string& source, bool asThread, std::string& outWhy);

	struct Result
	{
		bool        valid = false;   // false until something has actually run
		bool        ok = false;
		std::string text;            // the Lua error message, or a short confirmation
		uint64_t    seq = 0;         // bumps on every completion, so the GUI can spot a new one
	};
	Result lastResult();

	// ---- autocomplete -------------------------------------------------------------------------------
	// ★ The corpus is gathered BY ASKING LUA, not by scanning the executable. `pairs(_G)` enumerates 3093
	// globals in this build, so a tiny chunk run on the game thread yields the exact set of callable names
	// for whatever build is actually running. Three separate static registrar scans each missed real
	// bindings - 0x008525A0 registers only 30, another shape found 974, and `game_save` is in neither yet
	// works - so scanning was never going to be trustworthy. This cannot be wrong: it is the engine's own
	// globals table.
	void   requestCompletions();                 // queues the enumeration for the next tick; cheap to call
	size_t completionCount();                    // 0 until it has run
	std::vector<std::string> complete(std::string_view prefix, size_t limit);
}

class H5LuaConsole : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5LuaConsole(GameState game, IDIContainer& dicon);
	~H5LuaConsole();
	virtual std::string_view getName() override { return nameof(H5LuaConsole); }

	bool queueCommand(const std::string& source, bool asThread, std::string& outWhy);
	H5LuaConsoleBridge::Result lastResult() const;
	bool isUsable() const;
};
