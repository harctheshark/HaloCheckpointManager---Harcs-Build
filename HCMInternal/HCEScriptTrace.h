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
// HOW IT WORKS. Every one of the 1188 script functions is dispatched through ONE generic
// argument-evaluating wrapper (sim rva 0x350480), and at its entry CX is the FUNCTION INDEX. So a single
// midhook sees every script call, and the name comes from the function definition table (sim rva
// 0x81CA00, an array of qword pointers to definitions whose +0x08 is a char*). That is why this needed no
// signature scanning: a function is found by NAME through the table.
//
// ⚠ THIS DOES NOT PRINT THE SCRIPT'S STRING ARGUMENTS, AND DELIBERATELY DOES NOT PRETEND TO. The
// evaluated arguments are four bytes each in a buffer the script thread allocates, so a string argument
// is an OFFSET into the scenario's script string data, not a pointer. Resolving it needs the string-data
// base, which is not mapped yet. Until it is, `print` is reported as a call plus its raw argument dword -
// which is still enough to tell a spawner that ran from one that never did, and the dwords can be
// correlated against the .hsc source offline. Guessing the base would produce plausible-looking garbage,
// which is worse than a number.
//
// The hook is on the SIM and fires for EVERY script function evaluation - several hundred a second in a
// busy level - so it does the absolute minimum on the game thread: one atomic load, one compare against
// the filter, and a push into a lock-free-ish ring buffer. All name resolution, formatting and drawing
// happens later, on the render thread.
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
