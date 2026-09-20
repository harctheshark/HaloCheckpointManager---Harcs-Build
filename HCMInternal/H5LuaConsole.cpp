#include "pch.h"
#include "H5LuaConsole.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "RuntimeExceptionHandler.h"
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <map>
#include <iterator>

// See H5LuaConsole.h for where every address came from and why the hook sits where it does.

namespace
{
	// ---- the sim-tick hook site -----------------------------------------------------------------------
	// `ret 0` + 13 int3, exactly one caller (0x01363CBF, inside game_tick, right after the script tick).
	constexpr uintptr_t kRvaTickHook = 0x0088B410;

	// ---- engine globals -------------------------------------------------------------------------------
	constexpr uintptr_t kRvaCurrentScriptContext = 0x059829F0;   // NULL between resumes - see the header
	constexpr uintptr_t kRvaScriptContextServer  = 0x05982AC0;   // ctxA; ctx+0xF8 == 0 -> "remoteServer"

	// ---- script context layout ------------------------------------------------------------------------
	constexpr uintptr_t kCtxLuaState      = 0x08;   // ⚠ reallocated per level, null outside one
	constexpr uintptr_t kCtxRunningState  = 0x10;   // currently-executing coroutine, 0 == idle
	constexpr uintptr_t kCtxRunningThread = 0x18;   // running thread handle, -1 == idle

	// ---- lua_State layout (NOT stock Lua 5.1 order - top/base/stack_last/stack are contiguous) ---------
	constexpr uintptr_t kLuaGlobalState = 0x10;   // l_G
	constexpr uintptr_t kLuaTop         = 0x48;   // raw StkId; lua_gettop/lua_settop are inlined away
	constexpr uintptr_t kGlobalSecure   = 0x1C0;  // 2 refuses source (0x007BBB65). 0 everywhere in this build.
	constexpr int32_t   kSecureRefuses  = 2;

	// ---- Lua C API ------------------------------------------------------------------------------------
	constexpr uintptr_t kRvaLoadBuffer      = 0x007D7180;   // luaL_loadbuffer(L, buf, sz, name) - 4-arg
	constexpr uintptr_t kRvaPcall           = 0x007D1320;   // lua_pcall(L, nargs, nres, errfunc)
	constexpr uintptr_t kRvaToLString       = 0x007D2510;   // lua_tolstring(L, idx, &len)
	constexpr uintptr_t kRvaLuaType         = 0x007D2930;   // lua_type(L, idx); 9/10 normalise to 6
	constexpr uintptr_t kRvaToBoolean       = 0x007D2330;   // lua_toboolean(L, idx)
	constexpr int32_t   kLuaMultRet         = -1;

	// ⚠ NOT stock Lua 5.1 tags. This build: 0 nil, 1 boolean, 2 lightuserdata, 3 number, 4 string, 5 table,
	// 6 function, 7 userdata, 8 thread, 11 ui64, 12 struct. (Raw tags 9/10 are Lua-closure and C-function,
	// but lua_type normalises both to 6, so only the normalised set matters here.)
	constexpr int32_t   kLuaTypeNil         = 0;
	constexpr int32_t   kLuaTypeBoolean     = 1;
	constexpr int32_t   kLuaTypeFunction    = 6;

	// ⚠⚠ RunScriptThread(ctx, L, nargs) - THE THIRD ARGUMENT IS A STACK COUNT, NOT A REGISTRY REF.
	// This crashed the game once by being called with a luaL_ref value. Proof it is a count, from the
	// failure path of the datum creator it calls (0x008489C0):
	//     00848A66  lea eax, [r14 + 1]        ; r14 = this argument
	//     00848A70  shl rcx, 4                ; * 16 (sizeof TValue)
	//     00848A74  add [rsi + 0x48], rcx     ; L->top -= (arg + 1) * 16   <- pops function + arg slots
	// and from the engine's own caller at 0x00844810, which lua_pushvalues the function, then its arguments
	// in a loop, then passes the COUNT. The function must already be on the stack; do NOT luaL_ref it, that
	// pops the very thing this needs.
	constexpr uintptr_t kRvaRunScriptThread = 0x00853F10;

	using luaL_loadbuffer_t  = int(__fastcall*)(void* L, const char* buf, size_t sz, const char* name);
	using lua_pcall_t        = int(__fastcall*)(void* L, int nargs, int nres, int errfunc);
	using lua_tolstring_t    = const char*(__fastcall*)(void* L, int idx, size_t* len);
	using lua_type_t         = int(__fastcall*)(void* L, int idx);
	using lua_toboolean_t    = int(__fastcall*)(void* L, int idx);
	using runScriptThread_t  = bool(__fastcall*)(void* ctx, void* L, int nargs);

	// ⚠ Chunk name must start with '=' or '@' or the loader prepends '@' and path-normalises it (stripping
	// "./" and truncating at 259). '=' keeps it verbatim, giving clean "=console:1: message near 'x'" errors.
	constexpr const char* kChunkName = "=console";

	// ---- bridge state ---------------------------------------------------------------------------------
	// gArmed gates everything; when the cheat is not constructed, every entry point reports unavailable and
	// touches no game memory.
	std::atomic<bool>     gArmed{ false };
	std::atomic<bool>     gPending{ false };   // checked FIRST in the hook - this runs 60x/sec
	std::atomic<bool>     gWantCorpus{ false };
	std::mutex            gMutex;              // guards gSource / gResult / gCorpus. Never held across a Lua call.
	std::string           gSource;
	bool                  gAsThread = true;
	H5LuaConsoleBridge::Result gResult;
	std::atomic<uint64_t> gSeq{ 0 };
	// Every global name. Callable ones first (alphabetical), then the rest - so autocomplete offers things
	// you can actually call before it offers data.
	std::vector<std::string> gCorpus;
	size_t                   gCallableCount = 0;

	// Asks Lua for its own globals. One line per entry, "name<TAB>typename", newline-separated so a single
	// lua_tolstring gets the lot. table.sort orders by name because the name comes first.
	//
	// ⚠⚠ DO NOT FILTER ON type(v)=='function' HERE. This build's type names are NOT stock: tag 9 is
	// "ifunction" (Lua closure) and tag 10 is "cfunction" - lua_type normalises both to 6 at the C level,
	// but Lua's own type() returns the real name. Filtering on 'function' returned exactly ONE global out of
	// 3093. The type comes back as data and the C++ side decides what is callable, so the set of type names
	// never has to be known in advance.
	constexpr const char* kEnumerateChunk =
		"local t={} for k,v in pairs(_G) do t[#t+1]=tostring(k)..'\\t'..type(v) end "
		"table.sort(t) return table.concat(t,'\\n')";

	uintptr_t exeBase() noexcept
	{
		static uintptr_t cached = 0;
		if (!cached) cached = (uintptr_t)GetModuleHandleW(nullptr);
		return cached;
	}

	void publish(bool ok, std::string text) noexcept
	{
		try
		{
			const uint64_t seq = gSeq.fetch_add(1, std::memory_order_relaxed) + 1;
			std::scoped_lock lock(gMutex);
			gResult.valid = true;
			gResult.ok = ok;
			gResult.text = std::move(text);
			gResult.seq = seq;
		}
		catch (...) {}   // a failed status report must never propagate into the engine
	}

	// Everything below runs ON THE GAME THREAD, inside the tick hook.

	// Pull the error message off the stack top. Only ever called on a non-zero status, where Lua has left
	// exactly one value - the message - at the top.
	std::string errorText(uintptr_t base, void* L) noexcept
	{
		try
		{
			size_t len = 0;
			const char* s = ((lua_tolstring_t)(base + kRvaToLString))(L, -1, &len);
			if (!s) return "(no error message)";
			return std::string(s, len ? len : strlen(s));
		}
		catch (...) { return "(error message unavailable)"; }
	}

	// Format whatever lua_pcall left above the saved stack mark.
	//
	// ⚠ lua_gettop does not exist as a callable function in this build (fully inlined by LTCG), so the count
	// is computed from the raw StkId the same way luaB_print does it at 0x0084FC2F:
	// (L->top - mark) / sizeof(TValue), with sizeof(TValue) == 0x10.
	//
	// ⚠ lua_tolstring returns NULL for anything that is not a string or a number, and it CONVERTS numbers to
	// strings in place - harmless here only because the caller restores top immediately afterwards.
	std::string describeResults(uintptr_t base, void* L, int64_t mark) noexcept
	{
		try
		{
			const int64_t top = *(int64_t*)((uintptr_t)L + kLuaTop);
			const int n = (int)((top - mark) / 0x10);
			if (n <= 0) return "ok";

			const auto luaType = (lua_type_t)(base + kRvaLuaType);
			const auto toStr = (lua_tolstring_t)(base + kRvaToLString);
			const auto toBool = (lua_toboolean_t)(base + kRvaToBoolean);

			std::string out;
			constexpr int kMaxShown = 8;
			for (int i = 0; i < n && i < kMaxShown; ++i)
			{
				if (!out.empty()) out += ",  ";
				const int idx = -(n - i);            // -n .. -1, oldest result first
				const int t = luaType(L, idx);
				if (t == kLuaTypeBoolean) { out += toBool(L, idx) ? "true" : "false"; continue; }
				if (t == kLuaTypeNil) { out += "nil"; continue; }

				size_t len = 0;
				const char* s = toStr(L, idx, &len);
				if (s) out.append(s, len ? len : strlen(s));
				else
				{
					// Tables, functions, userdata, threads: no string form. Name the type rather than
					// printing nothing, so "it returned something" is distinguishable from "it returned".
					switch (t)
					{
					case 5:  out += "<table>"; break;
					case 6:  out += "<function>"; break;
					case 7:  out += "<userdata>"; break;
					case 8:  out += "<thread>"; break;
					case 2:  out += "<lightuserdata>"; break;
					case 12: out += "<struct>"; break;
					default: out += "<type " + std::to_string(t) + ">"; break;
					}
				}
			}
			if (n > kMaxShown) out += ",  ... (" + std::to_string(n - kMaxShown) + " more)";
			return out.empty() ? "ok" : out;
		}
		catch (...) { return "ok (could not format the result)"; }
	}

	// Ask the VM for its own global function names. Runs on the game thread, same window as a command.
	// The caller has already set g_currentScriptContext and saved top.
	void buildCorpus(uintptr_t base, void* L, int64_t savedTop) noexcept
	{
		try
		{
			const size_t len = strlen(kEnumerateChunk);
			int status = ((luaL_loadbuffer_t)(base + kRvaLoadBuffer))(L, kEnumerateChunk, len, "=hcm_enumerate");
			if (status != 0) { PLOG_ERROR << "H5LuaConsole: enumerate chunk did not compile"; return; }
			status = ((lua_pcall_t)(base + kRvaPcall))(L, 0, 1, 0);
			if (status != 0) { PLOG_ERROR << "H5LuaConsole: enumerate chunk failed to run"; return; }

			size_t n = 0;
			const char* s = ((lua_tolstring_t)(base + kRvaToLString))(L, -1, &n);
			if (!s || !n) { PLOG_ERROR << "H5LuaConsole: enumerate returned nothing"; return; }

			// Callable-first ordering: a console is for calling things, so functions lead. "Callable" is
			// decided by the type name CONTAINING "function" - that catches function / cfunction /
			// ifunction without this code having to know the engine's type vocabulary up front.
			std::vector<std::string> callable, other;
			std::map<std::string, int> typeHistogram;
			const char* end = s + n;
			const char* start = s;
			for (const char* p = s; p <= end; ++p)
			{
				if (p != end && *p != '\n') continue;
				if (p > start)
				{
					std::string line(start, (size_t)(p - start));
					const size_t tab = line.find('\t');
					std::string name = (tab == std::string::npos) ? line : line.substr(0, tab);
					std::string type = (tab == std::string::npos) ? "" : line.substr(tab + 1);
					if (!name.empty())
					{
						++typeHistogram[type];
						if (type.find("function") != std::string::npos) callable.push_back(std::move(name));
						else other.push_back(std::move(name));
					}
				}
				start = p + 1;
			}

			std::string hist;
			for (const auto& [t, c] : typeHistogram)
				hist += (hist.empty() ? "" : ", ") + (t.empty() ? std::string("<none>") : t) + "=" + std::to_string(c);
			PLOG_INFO << "H5LuaConsole: enumerated " << (callable.size() + other.size())
				<< " globals from _G (" << callable.size() << " callable). Types: " << hist;

			const size_t nCallable = callable.size();
			callable.insert(callable.end(), std::make_move_iterator(other.begin()),
				std::make_move_iterator(other.end()));
			{
				std::scoped_lock lock(gMutex);
				gCorpus = std::move(callable);
				gCallableCount = nCallable;
			}
		}
		catch (...) { PLOG_ERROR << "H5LuaConsole: exception while enumerating globals"; }
		*(int64_t*)((uintptr_t)L + kLuaTop) = savedTop;
	}

	// Everything needed to touch the VM safely, acquired once. ⚠ Shared by BOTH the command path and the
	// autocomplete enumeration deliberately - these checks are the safety argument for the whole feature,
	// and two copies would eventually disagree.
	struct LuaAccess
	{
		bool        ok = false;
		uintptr_t   ctx = 0;
		void*       L = nullptr;
		void**      currentCtx = nullptr;
		void*       savedCtx = nullptr;
		int64_t     savedTop = 0;
		const char* why = nullptr;
	};

	LuaAccess acquire(uintptr_t base) noexcept
	{
		LuaAccess a;
		a.ctx = base + kRvaScriptContextServer;
		a.L = *(void**)(a.ctx + kCtxLuaState);
		if (!a.L) { a.why = "No script state - load into a level first."; return a; }

		// ⚠ Defence in depth. Our hook site is structurally outside any resume, but if a future build moves
		// the call these two fields still say whether a script is executing, and we refuse rather than push
		// onto a stack somebody else is using.
		if (*(void**)(a.ctx + kCtxRunningState) != nullptr ||
			*(int32_t*)(a.ctx + kCtxRunningThread) != -1)
		{
			a.why = "A script was running at the hook point - refused. This should not happen; "
				"please report it.";
			return a;
		}

		// The secure-state gate. 0 in every state this build creates, but read it so a future build that
		// turns it on refuses cleanly instead of throwing out of the compiler.
		void* const G = *(void**)((uintptr_t)a.L + kLuaGlobalState);
		if (G && *(int32_t*)((uintptr_t)G + kGlobalSecure) == kSecureRefuses)
		{
			a.why = "This Lua state is in secure mode and will not compile source.";
			return a;
		}

		// ⚠⚠ MANDATORY. g_currentScriptContext is null between resumes and the engine dereferences it with
		// no check (0x008935E7), so any bound function the chunk calls would fault. Set it for the duration
		// and put it back, exactly as the engine's own runner does at 0x00853F4C.
		a.currentCtx = (void**)(base + kRvaCurrentScriptContext);
		a.savedCtx = *a.currentCtx;
		*a.currentCtx = (void*)a.ctx;

		// lua_gettop/lua_settop do not exist as callable functions in this build - fully inlined by LTCG -
		// so the stack mark is the raw StkId.
		a.savedTop = *(int64_t*)((uintptr_t)a.L + kLuaTop);
		a.ok = true;
		return a;
	}

	void release(const LuaAccess& a) noexcept
	{
		if (!a.ok) return;
		*(int64_t*)((uintptr_t)a.L + kLuaTop) = a.savedTop;
		*a.currentCtx = a.savedCtx;
	}

	void runCorpus(uintptr_t base) noexcept
	{
		LuaAccess a = acquire(base);
		if (!a.ok) return;          // silent: autocomplete is a convenience, not a command
		buildCorpus(base, a.L, a.savedTop);
		release(a);
	}

	void runPending(uintptr_t base) noexcept
	{
		std::string source;
		bool asThread;
		{
			std::scoped_lock lock(gMutex);
			source = std::move(gSource);
			gSource.clear();
			asThread = gAsThread;
		}
		if (source.empty()) return;

		LuaAccess a = acquire(base);
		if (!a.ok) { publish(false, a.why ? a.why : "The Lua state is not usable right now."); return; }
		void* const L = a.L;
		const uintptr_t ctx = a.ctx;
		const int64_t savedTop = a.savedTop;

		try
		{
			// ★ EXPRESSION FIRST, exactly like the stock Lua REPL (lua.c does the same). A bare expression
			// such as `1+1` or `game_difficulty_get()` is not a statement, so compiling the raw text gives
			// the unhelpful "'=' expected near '<eof>'" - Lua has started parsing it as an assignment. Try
			// `return <input>` first and fall back to the raw text for real statements, so both
			// `log_message("hi")` and `1+1` do what the user meant.
			// ⚠ A failed compile leaves its error message on the stack; drop it before the retry or the
			// fallback's own error would be reported behind a stale one.
			// A single identifier (possibly dotted, e.g. `math.pi`) - the shape a HaloScript user types when
			// they mean "run this". Used below, after we know what it evaluated to.
			const bool bareName =
				!source.empty()
				&& (std::isalpha((unsigned char)source.front()) || source.front() == '_')
				&& std::all_of(source.begin(), source.end(), [](char c)
					{ return std::isalnum((unsigned char)c) || c == '_' || c == '.'; });

			const std::string asExpression = "return " + source;
			int status = ((luaL_loadbuffer_t)(base + kRvaLoadBuffer))(
				L, asExpression.c_str(), asExpression.size(), kChunkName);
			if (status != 0)
			{
				*(int64_t*)((uintptr_t)L + kLuaTop) = savedTop;
				// ⚠ Status is NEGATIVE on failure (-4 syntax, -100 runtime, ...). Test != 0, never > 0.
				status = ((luaL_loadbuffer_t)(base + kRvaLoadBuffer))(
					L, source.c_str(), source.size(), kChunkName);
			}

			if (status != 0)
			{
				publish(false, errorText(base, L));
			}
			// ⚠ GUARD, AND IT IS NOT PARANOIA - its absence is what turned a misread argument into a hard
			// crash. A successful compile must leave exactly one value, a function, on the stack. If it does
			// not, something about the loader is not what we think, and handing the engine a stack we have
			// misjudged is how it ends up reading a non-thread as a lua_State.
			else if (((lua_type_t)(base + kRvaLuaType))(L, -1) != kLuaTypeFunction)
			{
				publish(false, "The compiler did not leave a function on the stack - refusing to run it. "
					"This build's Lua may differ from the one this was written against; please report it.");
			}
			else if (asThread)
			{
				// The compiled chunk IS the function on the stack, with zero arguments after it. Hand it to
				// the engine's own script-thread runner so Sleep / SleepUntil / coroutine.yield work.
				// ⚠ Do not ref it first - the runner counts stack slots, it does not take a reference.
				const bool started = ((runScriptThread_t)(base + kRvaRunScriptThread))((void*)ctx, L, 0);
				publish(started, started ? "ok (script thread)"
					: "The engine refused to start a script thread (the thread pool may be full).");
			}
			else
			{
				// LUA_MULTRET so an expression's value comes back and can be shown - a console that only
				// ever says "ok" cannot answer a question, which is half of what one is for.
				status = ((lua_pcall_t)(base + kRvaPcall))(L, 0, kLuaMultRet, 0);

				// ★ A BARE NAME IS A COMMAND, NOT A LOOKUP. Halo 5's own scripting is Lua, but the people
				// using this console come from HaloScript, where `game_save` on its own RUNS it. In Lua
				// `return game_save` merely evaluates to the function, so the console would print
				// "<function>" and appear to do nothing - which is exactly what it did.
				// So: if the input was a single identifier and it evaluated to exactly one function, call
				// it. Anything else - a value, several results, a real expression - is shown untouched.
				if (status == 0 && bareName
					&& (*(int64_t*)((uintptr_t)L + kLuaTop) - savedTop) == 0x10
					&& ((lua_type_t)(base + kRvaLuaType))(L, -1) == kLuaTypeFunction)
				{
					status = ((lua_pcall_t)(base + kRvaPcall))(L, 0, kLuaMultRet, 0);
				}

				if (status != 0) publish(false, errorText(base, L));
				else             publish(true, describeResults(base, L, savedTop));
			}
		}
		catch (...)
		{
			// Should be unreachable: every call above is a protected one, so a Lua error unwinds into Lua's
			// own __try below us, not through here. This exists so that if that assumption is ever wrong we
			// stop the unwind in a frame that has real .pdata instead of letting it cross the hook stub.
			publish(false, "An exception escaped the Lua call. This should not happen; please report it.");
		}

		release(a);
	}

	// ⚠ RUNS ON THE GAME THREAD, 60x/sec. Do nothing at all in the common case - two relaxed atomic loads.
	void tickHook(SafetyHookContext&) noexcept
	{
		const bool cmd = gPending.load(std::memory_order_acquire);
		const bool corpus = gWantCorpus.load(std::memory_order_acquire);
		if (!cmd && !corpus) return;
		if (!gArmed.load(std::memory_order_relaxed))
		{
			gPending.store(false, std::memory_order_release);
			gWantCorpus.store(false, std::memory_order_release);
			return;
		}
		const uintptr_t base = exeBase();
		// Corpus first: a user who opens the console and immediately types gets suggestions on the same tick.
		if (corpus) { gWantCorpus.store(false, std::memory_order_release); runCorpus(base); }
		if (cmd) { gPending.store(false, std::memory_order_release); runPending(base); }
	}
}


namespace H5LuaConsoleBridge
{
	bool isUsable() { return gArmed.load(std::memory_order_relaxed); }

	bool queue(const std::string& source, bool asThread, std::string& outWhy)
	{
		if (!gArmed.load(std::memory_order_relaxed))
		{
			outWhy = "The Halo 5 Lua console is not running.";
			return false;
		}
		if (source.empty())
		{
			outWhy = "Nothing to run.";
			return false;
		}
		if (gPending.load(std::memory_order_acquire))
		{
			outWhy = "Still running the previous command.";
			return false;
		}
		{
			std::scoped_lock lock(gMutex);
			gSource = source;
			gAsThread = asThread;
		}
		gPending.store(true, std::memory_order_release);
		return true;
	}

	Result lastResult()
	{
		std::scoped_lock lock(gMutex);
		return gResult;
	}

	void requestCompletions()
	{
		if (gArmed.load(std::memory_order_relaxed))
			gWantCorpus.store(true, std::memory_order_release);
	}

	size_t completionCount()
	{
		std::scoped_lock lock(gMutex);
		return gCorpus.size();
	}

	std::vector<std::string> complete(std::string_view prefix, size_t limit)
	{
		std::vector<std::string> hits;
		if (prefix.empty()) return hits;
		std::scoped_lock lock(gMutex);
		// Prefix matches first (what you are most likely typing), then substring matches, because Halo 5's
		// names bury the useful word in the middle - the zone-set switch is prepare_to_switch_to_zone_set,
		// so someone typing "zone" would otherwise be told nothing exists.
		for (const auto& n : gCorpus)
		{
			if (hits.size() >= limit) return hits;
			if (n.size() >= prefix.size() &&
				std::equal(prefix.begin(), prefix.end(), n.begin(),
					[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); }))
				hits.push_back(n);
		}
		std::string lowered;
		lowered.reserve(prefix.size());
		for (char c : prefix) lowered += (char)std::tolower((unsigned char)c);
		for (const auto& n : gCorpus)
		{
			if (hits.size() >= limit) break;
			std::string ln;
			ln.reserve(n.size());
			for (char c : n) ln += (char)std::tolower((unsigned char)c);
			if (ln.find(lowered) != std::string::npos &&
				std::find(hits.begin(), hits.end(), n) == hits.end())
				hits.push_back(n);
		}
		return hits;
	}
}


class H5LuaConsole::Impl
{
private:
	GameState mGame;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::unique_ptr<ModuleMidHook> mHook;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game), runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5LuaConsole only supports Halo 5: Forge");

		auto target = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(
			GameState(mGame).toModuleName(), std::vector<int64_t>{ (int64_t)kRvaTickHook });
		// Attached immediately: the hook is a no-op until something is queued, and the console is only
		// useful if it is listening. The constructor itself still touches no game memory.
		mHook = ModuleMidHook::make(GameState(mGame).toModuleName(), target, tickHook, true);

		{
			std::scoped_lock lock(gMutex);
			gResult = H5LuaConsoleBridge::Result{};
			gCorpus.clear();
		}
		gPending.store(false, std::memory_order_release);
		gArmed.store(true, std::memory_order_release);
		// Ask the VM for its globals on the first tick that has a level loaded. Harmless if there is not one
		// yet - runCorpus just fails to acquire and the GUI re-requests.
		gWantCorpus.store(true, std::memory_order_release);
		PLOG_DEBUG << "H5LuaConsole armed; tick hook at exe+0x" << std::hex << kRvaTickHook;
	}

	~Impl()
	{
		// Disarm BEFORE dropping the hook, so a tick already inside tickHook finds nothing to do and no
		// later tick can enter it expecting state that is going away.
		gArmed.store(false, std::memory_order_release);
		gPending.store(false, std::memory_order_release);
		mHook.reset();
	}

	bool queueCommand(const std::string& source, bool asThread, std::string& outWhy)
	{
		return H5LuaConsoleBridge::queue(source, asThread, outWhy);
	}
	H5LuaConsoleBridge::Result lastResult() const { return H5LuaConsoleBridge::lastResult(); }
	bool isUsable() const { return H5LuaConsoleBridge::isUsable(); }
};


H5LuaConsole::H5LuaConsole(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5LuaConsole::~H5LuaConsole() { PLOG_VERBOSE << "~" << getName(); }

bool H5LuaConsole::queueCommand(const std::string& source, bool asThread, std::string& outWhy)
{
	return pimpl->queueCommand(source, asThread, outWhy);
}
H5LuaConsoleBridge::Result H5LuaConsole::lastResult() const { return pimpl->lastResult(); }
bool H5LuaConsole::isUsable() const { return pimpl->isUsable(); }
