#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved: a live trace of the level script's own function calls, drawn on screen.
//
// WHY THIS EXISTS. The campaign scripts are heavily instrumented by their own authors and the
// instrumentation is SHIPPED ENABLED - d20 (Keyes) declares `(global boolean debug true)` on line 2 and
// then makes 137 `print_if debug` and 175 `print` calls, including, for the squad that gates the Keyes
// control-room door:
//
//     "enc6_1"        "enc6_1 | spawner enabled"      "enc6_1 | cobmat form added"
//                     "enc6_1 | spawner disabled"     "enc6_1 | elite added"
//
// The game is already saying what it did. Nothing was reading it.
//
// HOW IT WORKS. The hs function definition table (sim rva 0x81BA20) is an array of qword pointers to
// definitions whose +0x08 is the function's name and whose +0x20 is its RUNTIME implementation. So the
// functions to watch are found BY NAME - no signature scanning - and one midhook is installed per distinct
// implementation address. At the entry of every implementation CX is the function index and EDX the
// expression-node index of the call site, so a single shared callback serves all of them.
//
// ⚠⚠ NOT def+0x18. That field is the same function for all 1188 definitions and also takes the index in CX,
// which makes it look like the one choke point that sees everything. It is the COMPILE-TIME argument
// type-checker. Scripts are compiled when the scenario tag is built, so in a running game it is called
// exactly zero times - the first version of this hooked it, attached cleanly, and reported 0 script calls
// for a whole test session.
//
// ⚠ THIS DOES NOT PRINT THE SCRIPT'S STRING ARGUMENTS, AND DELIBERATELY DOES NOT PRETEND TO. A string
// argument is a four-byte OFFSET into the scenario's script string data, not a pointer, and the base of
// that blob is not mapped yet. So each call is reported as name plus its expression-node index - the call
// SITE, which distinguishes one `print` from another and can be correlated against the .hsc offline.
// Guessing the base would render convincing nonsense, which is worse than a number. (The engine's own
// console sink, rva 0x2040D0, takes the same id in ECX but has 1417 callers across the whole engine, so
// hooking it would drown the script in engine chatter rather than resolve anything.)
//
// The hooks are on the SIM and fire on every call to the watched functions, so they do the absolute
// minimum on the game thread: one atomic load, one compare against the filter, and a push into a
// lock-free-ish ring buffer. All name resolution, formatting and drawing happens later, on the render
// thread.
class HCEScriptTrace : public IOptionalCheat
{
private:
	class HCEScriptTraceImpl;
	std::unique_ptr<HCEScriptTraceImpl> pimpl;

public:
	HCEScriptTrace(GameState game, IDIContainer& dicon);
	~HCEScriptTrace();

	std::string_view getName() override { return "HCEScriptTrace"; }
};
