#pragma once
#include "pch.h"

// ================================================================================================================
// EVERY HALOSCRIPT NAME THE GAME KNOWS, WITH A FLAG FOR WHETHER IT ACTUALLY DOES ANYTHING.
//
// This is the data behind the console's autocomplete. It is READ-ONLY and touches nothing but .rdata, so it is
// safe to build from any thread and cannot affect the game.
//
// TWO TABLES
// ----------
// FUNCTIONS: simBase + 0x81BA20.
//   ⚠ It is an array of 8-byte POINTERS to variable-sized definitions (0x40/0x50/0x60), NOT an array of
//   fixed-stride structs. A previous attempt marched it at stride 0x40 and got zero entries, which is what
//   made the table look like it had moved. It had not; only the shape was wrong.
//   1695 entries, proven by the game's own console-list builder at 0x8ED0 (`cmp r15d, 0x69f`).
//
//     struct hs_function_definition {
//         int16_t     return_type;   // +0x00   index into the type-name table
//         const char* name;          // +0x08
//         uint64_t    flags;         // +0x10   2 = special form, 1 = cs_* command script, 4 = editor
//         void*       parse_fn;      // +0x18
//         void*       impl;          // +0x20   <- the liveness test
//         const char* description;   // +0x28   NULL throughout this build
//         const char* usage;         // +0x30   printed verbatim instead of the arg list when non-empty
//         int16_t     argc;          // +0x38
//         int16_t     argtypes[];    // +0x3A
//     };
//
//   Every offset above is proven from the game's own usage formatter, sub_1801F8240.
//
// GLOBALS: simBase + 0x7EE540 — also a POINTER array, 1475 entries, each -> {const char* name; uint16 type;
//   void* external;} with stride 0x18.
//   ⚠ Use THIS array and not the descriptor block at 0x9A1738. They are permutations of each other and the
//   engine indexes this one; mixing them up silently selects a different global.
//
// LIVENESS — why "the name exists" is not "the command works"
// ----------------------------------------------------------
// 425 of the 1695 functions share one stub, sub_1801B2430, whose entire body is
// `mov ecx,edx / xor edx,edx / jmp 0x1801FE100` — it returns 0 without evaluating anything. There is even an
// entry literally named `dummy_function` pointing at it. The game's OWN command-list builder filters on
// exactly this (`cmp qword ptr [rbx+0x20], rcx` / `je` -> skip), so matching that test matches the shipped
// console. `game_tick_rate`, `script_recompile`, `drop` and every `havok_debug_*` are among them.
// Two smaller families evaluate their arguments and then do nothing: 0x1B1C10 (38) and 0x1B1BB0 (15).
// And `cond` is the single entry with a NULL impl — classify with `impl != dummy && impl != 0`, or a future
// call path jumps to null.
//
// For GLOBALS the equivalent test is the external pointer: 1278 of 1475 have `external == 0`, meaning no C++
// backing at all. Setting one of those writes a script-visible slot that nothing in the engine reads. That is
// why `cheat_bump_possession` cannot be turned on - the name shipped, the feature did not.
// ================================================================================================================

class HCEScriptRegistry
{
public:
	struct Entry
	{
		std::string name;
		std::string signature;   // rendered the way the game's own formatter renders it
		bool isGlobal = false;
		bool isLive = false;     // function: real impl. global: has C++ backing.
		int16_t argc = 0;
	};

	struct Census
	{
		int functionsTotal = 0, functionsLive = 0, functionsStubbed = 0;
		int globalsTotal = 0, globalsLive = 0;
	};

	// Walks both tables out of the module image. Returns entries sorted by name. Never throws; on a build
	// whose tables do not look right it returns what it could read and reports it in the census.
	static std::vector<Entry> build(uintptr_t simBase, Census& outCensus) noexcept;

	// Case-insensitive prefix match over a built list, best-effort ordered: exact match first, then live
	// entries, then stubbed ones.
	static std::vector<const Entry*> complete(const std::vector<Entry>& all, std::string_view prefix, size_t limit) noexcept;
};
