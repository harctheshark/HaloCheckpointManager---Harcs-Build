#include "pch.h"
#include "HCEScriptRegistry.h"
#include "HCEGetPlayerState.h"

// See HCEScriptRegistry.h for the table layouts and where each offset was proven.

namespace
{
	// --- table locations, all RVAs off the sim module base -------------------------------------------
	constexpr int64_t kFunctionTable = 0x81BA20;   // pointer array, stride 8
	constexpr int     kFunctionCount = 1695;       // game's own loop bound: cmp r15d, 0x69f
	constexpr int64_t kGlobalTable = 0x7EE540;     // pointer array, stride 8
	constexpr int     kGlobalCount = 1475;
	constexpr int64_t kTypeNameTable = 0x9AA1C0;   // const char*[], indexed by type id
	constexpr int     kTypeCount = 69;             // bounds-check every type id against this

	// --- impl addresses that mean "this does nothing" ------------------------------------------------
	constexpr int64_t kDummyImpl = 0x1B2430;   // 425 entries; the game filters on exactly this
	constexpr int64_t kArgNoopA = 0x1B1C10;    // 38 entries: evaluates its args, then nothing
	constexpr int64_t kArgNoopB = 0x1B1BB0;    // 15 entries: same

	// --- function definition field offsets (sub_1801F8240) -------------------------------------------
	constexpr int64_t kFnReturnType = 0x00;   // int16
	constexpr int64_t kFnName = 0x08;
	constexpr int64_t kFnFlags = 0x10;
	constexpr int64_t kFnImpl = 0x20;
	constexpr int64_t kFnUsage = 0x30;
	constexpr int64_t kFnArgc = 0x38;         // int16
	constexpr int64_t kFnArgTypes = 0x3A;     // int16[]

	// --- global descriptor field offsets -------------------------------------------------------------
	constexpr int64_t kGlName = 0x00;
	constexpr int64_t kGlType = 0x08;         // uint16
	constexpr int64_t kGlExternal = 0x10;

	// Reads a NUL-terminated ASCII string out of the game. Bounded, and rejects anything that is not plainly
	// printable - a bad pointer that happens to be readable must not become a console entry.
	bool readName(uintptr_t address, std::string& out, size_t maxLen = 96) noexcept
	{
		out.clear();
		if (!address) return false;
		char buf[97]{};
		for (size_t i = 0; i < maxLen; ++i)
		{
			char c = 0;
			if (!HCEGetPlayerState::tryReadRaw(address + i, &c, 1)) return false;
			if (c == '\0') { buf[i] = '\0'; break; }
			if ((unsigned char)c < 32 || (unsigned char)c > 126) return false;
			buf[i] = c;
		}
		out = buf;
		return !out.empty();
	}

	std::string typeName(uintptr_t simBase, int16_t typeId) noexcept
	{
		if (typeId < 0 || typeId >= kTypeCount) return "?";
		uintptr_t p = 0;
		if (!HCEGetPlayerState::tryReadRaw(simBase + kTypeNameTable + (int64_t)typeId * 8, &p, sizeof(p))) return "?";
		std::string s;
		return readName(p, s) ? s : "?";
	}
}

std::vector<HCEScriptRegistry::Entry> HCEScriptRegistry::build(uintptr_t simBase, Census& census) noexcept
{
	census = Census{};
	std::vector<Entry> out;
	if (!simBase) return out;
	out.reserve(kFunctionCount + kGlobalCount);

	const uintptr_t dummy = simBase + kDummyImpl;
	const uintptr_t noopA = simBase + kArgNoopA;
	const uintptr_t noopB = simBase + kArgNoopB;

	// ---------------- functions ----------------
	for (int i = 0; i < kFunctionCount; ++i)
	{
		uintptr_t def = 0;
		if (!HCEGetPlayerState::tryReadRaw(simBase + kFunctionTable + (int64_t)i * 8, &def, sizeof(def)) || !def)
			continue;

		uintptr_t namePtr = 0;
		if (!HCEGetPlayerState::tryReadRaw(def + kFnName, &namePtr, sizeof(namePtr))) continue;

		Entry e;
		if (!readName(namePtr, e.name)) continue;

		uintptr_t impl = 0;
		HCEGetPlayerState::tryReadRaw(def + kFnImpl, &impl, sizeof(impl));

		// ⚠ THE LIVENESS RULE DELIBERATELY MATCHES THE GAME'S OWN, WHICH IS `impl != dummy` AND NOTHING MORE.
		// The shipped command-list builder at 0x8ED0 filters on exactly that, so anything stricter would hide
		// commands the game itself lists.
		//
		// `impl != 0` is the one addition, and it is required: `cond` is the single entry of 1695 with a null
		// impl and would otherwise classify as live.
		//
		// The two arg-evaluating no-op families are NOT excluded, only annotated below. They are the debug
		// print group (print, print_if, log_print, breakpoint, cinematic_print, ...) and calling one is
		// harmless; whether it produces visible output is a question about this build's debug output, not
		// about whether the entry is real. Hiding them outright would be a stronger claim than the evidence
		// supports - a verified census called them "real", a second pass called them no-ops, and both can be
		// true depending on what you count as doing something.
		const bool argEvalNoop = (impl == noopA) || (impl == noopB);
		e.isLive = (impl != 0) && (impl != dummy);
		e.isGlobal = false;

		int16_t argc = 0, retType = 0;
		HCEGetPlayerState::tryReadRaw(def + kFnArgc, &argc, sizeof(argc));
		HCEGetPlayerState::tryReadRaw(def + kFnReturnType, &retType, sizeof(retType));
		e.argc = argc;

		// Render the signature the way the game's own formatter does: usage string verbatim when present,
		// otherwise the argument type list.
		std::string sig = "(<" + typeName(simBase, retType) + "> " + e.name;
		uintptr_t usagePtr = 0;
		std::string usage;
		if (HCEGetPlayerState::tryReadRaw(def + kFnUsage, &usagePtr, sizeof(usagePtr))
			&& usagePtr && readName(usagePtr, usage) && !usage.empty())
		{
			sig += " " + usage;
		}
		else if (argc > 0 && argc < 64)
		{
			for (int a = 0; a < argc; ++a)
			{
				int16_t at = 0;
				HCEGetPlayerState::tryReadRaw(def + kFnArgTypes + (int64_t)a * 2, &at, sizeof(at));
				sig += " <" + typeName(simBase, at) + ">";
			}
		}
		sig += ")";
		if (argEvalNoop) sig += "   (evaluates its arguments, then does nothing in this build)";
		e.signature = sig;

		++census.functionsTotal;
		if (e.isLive) ++census.functionsLive; else ++census.functionsStubbed;
		out.push_back(std::move(e));
	}

	// ---------------- globals ----------------
	for (int i = 0; i < kGlobalCount; ++i)
	{
		uintptr_t desc = 0;
		if (!HCEGetPlayerState::tryReadRaw(simBase + kGlobalTable + (int64_t)i * 8, &desc, sizeof(desc)) || !desc)
			continue;

		uintptr_t namePtr = 0;
		if (!HCEGetPlayerState::tryReadRaw(desc + kGlName, &namePtr, sizeof(namePtr))) continue;

		Entry e;
		if (!readName(namePtr, e.name)) continue;

		uint16_t type = 0;
		uintptr_t external = 0;
		HCEGetPlayerState::tryReadRaw(desc + kGlType, &type, sizeof(type));
		HCEGetPlayerState::tryReadRaw(desc + kGlExternal, &external, sizeof(external));

		e.isGlobal = true;
		// A global with no external pointer has no C++ backing - setting it writes a script slot that nothing
		// in the engine reads. 1278 of 1475 are in that state.
		e.isLive = (external != 0);
		e.argc = 0;
		e.signature = e.name + "  <" + typeName(simBase, (int16_t)type) + " global>"
			+ (e.isLive ? "" : "  (no engine backing - setting it has no effect)");

		++census.globalsTotal;
		if (e.isLive) ++census.globalsLive;
		out.push_back(std::move(e));
	}

	std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
	return out;
}

std::vector<const HCEScriptRegistry::Entry*> HCEScriptRegistry::complete(
	const std::vector<Entry>& all, std::string_view prefix, size_t limit) noexcept
{
	std::vector<const Entry*> hits;
	if (prefix.empty()) return hits;

	std::string needle(prefix);
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });

	for (const Entry& e : all)
	{
		if (e.name.size() < needle.size()) continue;
		bool match = true;
		for (size_t i = 0; i < needle.size(); ++i)
			if (std::tolower((unsigned char)e.name[i]) != (unsigned char)needle[i]) { match = false; break; }
		if (match) hits.push_back(&e);
	}

	// Exact match first, then anything that actually works, then the rest. Someone typing a command wants the
	// one that does something, not the alphabetically-first stub that shares its prefix.
	std::stable_sort(hits.begin(), hits.end(), [&needle](const Entry* a, const Entry* b)
		{
			const bool ea = (a->name.size() == needle.size()), eb = (b->name.size() == needle.size());
			if (ea != eb) return ea;
			if (a->isLive != b->isLive) return a->isLive;
			return false;
		});

	if (hits.size() > limit) hits.resize(limit);
	return hits;
}
