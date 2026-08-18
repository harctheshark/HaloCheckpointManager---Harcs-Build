#include "pch.h"
#include "HCEAnchors.h"
#include "HCESignatureScan.h"
#include "CustomExceptions.h"
#include "ModuleCache.h"
#include <array>

// See HCEAnchors.h for why this exists and what the contract is.
//
// EVERY SIGNATURE BELOW WAS VERIFIED AGAINST THE SHIPPED BINARY: scanned over the sim's single executable
// section, confirmed to match EXACTLY ONCE, and its extraction confirmed to reproduce the address that
// InternalPointerData.xml currently carries. The "min-prefix" note on some entries is the shortest leading
// slice that is already unique - the remaining bytes are deliberate redundancy, so a build that changes the
// tail of the function still resolves.
namespace
{
	using HCEAnchors::Anchor;

	// How a match becomes an address.
	enum class Extract
	{
		MatchIsTarget,   // the matched address IS what we want (a function)
		RipRelative,     // read a disp32 out of an instruction inside the match
		TlsDirectory,    // not a scan at all - the PE TLS directory
	};

	struct AnchorDef
	{
		Anchor anchor;
		const char* name;
		const char* signature;
		Extract extract;
		int insnOffset;    // offset within the match of the instruction carrying the disp32
		int dispOffset;    // offset of the disp32 within that instruction
		int insnLength;    // whole instruction length; rip is the address of the NEXT one
		const char* why;   // why this site, and what would break it
	};

	constexpr AnchorDef kAnchors[] =
	{
		{ Anchor::TlsIndex, "TlsIndex", nullptr, Extract::TlsDirectory, 0, 0, 0,
		  "PE TLS directory AddressOfIndex - the loader's own contract, so it cannot be inlined away or moved "
		  "semantically. The global itself has ~6900 referencing instructions, so no single site is "
		  "distinctive; the structural route is strictly better than any signature." },

		{ Anchor::GameThreadEntrypoint, "GameThreadEntrypoint",
		  "40 53 56 57 41 54 41 55 41 56 41 57 48 81 EC 60 03 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 05 00 00 00 3A",
		  Extract::MatchIsTarget, 0, 0, 0,
		  "The checkpoint/game-state initialiser: it allocates the 0xC10000 state block and both checkpoint "
		  "buffers. It cannot disappear while checkpoints exist. Unique at 16 of its 49 bytes." },

		{ Anchor::PhysicsTable, "PhysicsTable",
		  "48 89 2D ?? ?? ?? ?? 48 89 2D ?? ?? ?? ?? C5 F8 77 FF 15 ?? ?? ?? ?? BA 00 00 00 03 B9 00 00 00 01 FF 15 ?? ?? ?? ??",
		  Extract::RipRelative, 7, 3, 7,
		  "Havok world teardown - nulls the table then restores the FP control word (vzeroupper, _clearfp, "
		  "_controlfp). That FPU idiom is unmistakable. Only 2 of 424 references write this global." },

		{ Anchor::CurrentLevel, "CurrentLevel",
		  "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 75 14 66 44 39 35 ?? ?? ?? ?? 75 0A 66 83 3D ?? ?? ?? ?? 03",
		  Extract::RipRelative, 0, 3, 7,
		  "The 'has the level name changed' strncmp inside the session/presence updater. Only 3 references "
		  "exist in the module, all in that one function. ⚠ The length is load-bearing, not decoration: the "
		  "first 14 bytes alone match 17 places." },

		{ Anchor::CurrentBSP, "CurrentBSP",
		  "89 15 ?? ?? ?? ?? 44 89 05 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 48 8B 01 89 3D ?? ?? ?? ?? 44 89 0D ?? ?? ?? ?? FF 50 40",
		  Extract::RipRelative, 22, 2, 6,
		  "The zone/BSP commit writer - the only site that writes a real value; the other ~70 references are "
		  "reads and six '= -1' resets. ⚠ Register allocation here is compiler-chosen, so a recompile could "
		  "shuffle it; fails safe if so." },

		{ Anchor::TickCounterSlot, "TickCounterSlot",
		  "48 8D 81 00 00 C0 00 48 89 05 ?? ?? ?? ?? 48 C7 05 ?? ?? ?? ?? 00 00 01 00",
		  Extract::RipRelative, 7, 3, 7,
		  "The allocator: lea rax,[rcx+0C00000h] (the game-state size) immediately followed by the store. "
		  "⚠ That 0xC00000 immediate is build-specific - if the state size changes this fails safe." },

		{ Anchor::ForceRevertFlag, "ForceRevertFlag",
		  "80 3D ?? ?? ?? ?? 00 74 18 0F B6 05 ?? ?? ?? ?? 0F B6 C8 E8 ?? ?? ?? ?? 66 C7 05 ?? ?? ?? ?? 00 00",
		  Extract::RipRelative, 0, 2, 7,
		  "The engine's own consumer: if the flag byte is set, take the reason byte, call the revert, clear the "
		  "word. This IS the feature ForceRevert drives, so it cannot vanish while the feature exists." },

		{ Anchor::DoubleRevertFlag, "DoubleRevertFlag",
		  "0F B6 05 ?? ?? ?? ?? 88 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? FF C0 89 0D ?? ?? ?? ?? 99 F7 3D ?? ?? ?? ?? B8 60 00 00 00 89 15 ?? ?? ?? ??",
		  Extract::RipRelative, 13, 2, 6,
		  "The checkpoint ring advance, idx = (idx+1) % ringSize, with guard-byte propagation. The "
		  "inc / cdq / idiv-on-a-rip-operand shape is extremely rare." },

		{ Anchor::CheckpointControlBlock, "CheckpointControlBlock",
		  "40 57 48 83 EC 20 33 FF 40 38 3D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00",
		  Extract::RipRelative, 8, 3, 7,
		  "The checkpoint teardown's first act: the engine's own 'is the checkpoint system initialised' guard. "
		  "The MSVC prologue is common; uniqueness comes from the guard plus the TLS load that follows." },

		{ Anchor::CheckpointSlotTable, "CheckpointSlotTable",
		  "48 63 05 ?? ?? ?? ?? 33 D2 83 F8 FF 74 ?? 48 8B D0 48 8D 05 ?? ?? ?? ?? 48 C1 E2 04 48 03 D0 48 8B 12 33 C9 E8 ?? ?? ?? ??",
		  Extract::RipRelative, 17, 3, 7,
		  "The true accessor, slot = table + index*16. ⚠ THE RISKIEST ANCHOR IN THIS FILE: slots 0 and 1 are "
		  "allocated and freed by byte-identical blocks, so a build that reorders them could resolve the WRONG "
		  "SLOT rather than failing. This accessor form was chosen over the alloc/free sites for exactly that "
		  "reason, and validateSlotTable() below adds an alignment invariant on top." },

		{ Anchor::CheckpointStateLength, "CheckpointStateLength",
		  "4C 63 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B 09 E8",
		  Extract::RipRelative, 0, 3, 7,
		  "The local save's memcpy: movsxd r8,[len] / mov rdx,[block] / mov rcx,[rcx] / call memcpy. Every byte "
		  "is opcode+modrm with NO build-specific immediate, which makes it unusually durable." },

		{ Anchor::CheckpointSaveInFlight, "CheckpointSaveInFlight",
		  "44 8B 05 ?? ?? ?? ?? 41 83 E8 02 74 ?? 41 83 F8 01 0F 85 ?? ?? ?? ?? 8B 05 ?? ?? ?? ??",
		  Extract::RipRelative, 0, 3, 7,
		  "The worker's state-machine dispatch - the read side of the publication barrier the checkpoint blob "
		  "code depends on. Taken from its own site rather than sharing the state-block signature, so one "
		  "build change cannot disable three features at once." },

		{ Anchor::CheckpointShell, "CheckpointShell",
		  "B9 00 10 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 63 1D ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 70 10 48 85 F6",
		  Extract::RipRelative, 10, 3, 7,
		  "The provider-vs-local discriminator the checkpoint path already documents. Unique at 6 bytes despite "
		  "the global having 212 references." },

		{ Anchor::CheckpointStateBlock, "CheckpointStateBlock",
		  "4C 63 05 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B 09 E8",
		  Extract::RipRelative, 7, 3, 7,
		  "Same instruction pair as CheckpointStateLength, different operand. Already shipped as "
		  "kSigStateBlockSlot in HCEStateHook.cpp and confirmed to agree with it." },
		// ================================ PATCH SITES ================================
		// These three are why this file now has consumers. Every anchor above resolves a DATA address that HCM
		// reads or writes a flag in; these resolve CODE that HCM overwrites. A stale one does not produce a
		// wrong number, it produces a patch landing mid-instruction - which is exactly what shipped on
		// 2026-08-17, when pauseGameFunction moved 0x10 and pausing became a reliable crash.
		//
		// ⚠ EVERY RELATIVE OPERAND IS WILDCARDED - rel32 branch displacements AND rip-relative memory
		// displacements. That is not tidiness, it is the whole reason these survive an update: a displacement
		// encodes WHERE THE TARGET IS, so it changes whenever the function or its target moves, even though
		// the instruction sequence is identical. The trigger fingerprint in InternalPointerData.xml learned
		// this the hard way - it bakes in three rip displacements and therefore refuses after any relocation.

		{ Anchor::PauseGameFunction, "PauseGameFunction",
		  "84 C0 0F 85 ?? ?? ?? ?? 4E 8B 04 36 C4 C1 78 2F 78 10 0F 83 ?? ?? ?? ?? B8 A8 00 00 00 45 32 FF 48 8B 04 06 48 85 C0 74 ??",
		  Extract::MatchIsTarget, 0, 0, 0,
		  "The `test al,al` HCM overwrites to force the pause predicate, plus the AVX compare and the 0xA8 slot "
		  "load that follow it. Unique at 12 of these 41 bytes; the rest is deliberate redundancy." },

		{ Anchor::FadeFromBlackGuardSite, "FadeFromBlackGuardSite",
		  "41 B9 3C 00 00 00 C5 E8 57 D2 C5 F0 57 C9 C5 F8 57 C0 E8 ?? ?? ?? ?? 4C 8B 6C 24 60 4C 8B 74 24 58 40 84 F6 48 8B 74 24 78",
		  Extract::MatchIsTarget, 0, 0, 0,
		  "`mov r9d, 3Ch` (the fade duration) followed by three vxorps zeroing the colour. HCM patches the "
		  "IMM32 INSIDE this instruction, so the patch target is this address + 2 and the guard site is this "
		  "address - keeping both derived from one anchor is what stops them drifting apart. The imm 3C is "
		  "part of the pattern on purpose: it proves this is the fade call and not another vxorps triple. "
		  "Unique at 10 of these 41 bytes." },

		{ Anchor::TriggerVolumeTestPoint, "TriggerVolumeTestPoint",
		  "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E8 48 81 EC 18 01 00 00 48 63 D9 4D 8B F0 8B 0D ?? ?? ?? ?? C5 F8 29 B4 24 00 01 00 00",
		  Extract::MatchIsTarget, 0, 0, 0,
		  "trigger_volume_test_point's prologue. Its eight HaloScript call sites deliberately have NO anchors: "
		  "HCETriggerActivity already requires each recorded site to be an E8 whose target equals this "
		  "function, which validates all eight against one resolved address and cannot drift out of sync with "
		  "them. Unique at 28 of these 46 bytes." },

	};

	static_assert(std::size(kAnchors) == (size_t)Anchor::Count, "every Anchor needs exactly one definition");

	std::array<uintptr_t, (size_t)Anchor::Count> gResolved{};
	uintptr_t gResolvedAgainstBase = 0;
	std::mutex gResolveMutex;

	// _tls_index via the PE TLS directory. Structural, so it needs no signature and cannot be broken by a
	// recompile - the loader defines where this lives.
	uintptr_t readTlsIndexAddress(uintptr_t base)
	{
		__try
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

			const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
			if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return 0;

			const IMAGE_TLS_DIRECTORY64* tls = (const IMAGE_TLS_DIRECTORY64*)(base + dir.VirtualAddress);
			return (uintptr_t)tls->AddressOfIndex;   // already an absolute VA
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}

	// ⚠ The slot table is the one anchor where a plausible build change resolves the WRONG value instead of
	// failing (slots 0 and 1 are byte-identical code). A cheap invariant catches the obvious version of that:
	// the table is an array of 16-byte entries, so it must be 16-byte aligned.
	bool validateSlotTable(uintptr_t address)
	{
		return address != 0 && (address % 16) == 0;
	}
}

namespace HCEAnchors
{
	void resolveAll(uintptr_t simModuleBase)
	{
		if (!simModuleBase) return;

		std::scoped_lock lock(gResolveMutex);
		if (gResolvedAgainstBase == simModuleBase) return;   // already done for this module load

		gResolvedAgainstBase = simModuleBase;
		gResolved.fill(0);

		for (const AnchorDef& def : kAnchors)
		{
			uintptr_t resolved = 0;

			if (def.extract == Extract::TlsDirectory)
			{
				resolved = readTlsIndexAddress(simModuleBase);
			}
			else
			{
				int hits = 0;
				const uintptr_t match = HCESignatureScan::resolveUnique(simModuleBase, def.signature, hits);
				if (!match)
				{
					PLOG_WARNING << "HCE anchor '" << def.name << "' did NOT resolve (" << hits
						<< " matches, exactly 1 required). The feature that needs it will refuse rather than "
						"use a stale address. This normally means Halo Campaign Evolved updated.";
					continue;
				}

				resolved = (def.extract == Extract::MatchIsTarget)
					? match
					: HCESignatureScan::ripTarget(match + def.insnOffset, def.dispOffset, def.insnLength);
			}

			if (def.anchor == Anchor::CheckpointSlotTable && !validateSlotTable(resolved))
			{
				PLOG_WARNING << "HCE anchor 'CheckpointSlotTable' resolved to a misaligned address ("
					<< std::format("0x{:X}", resolved) << ") - rejecting it. See its note in HCEAnchors.cpp: "
					"this is the one anchor that can resolve the wrong slot rather than failing.";
				resolved = 0;
			}

			gResolved[(size_t)def.anchor] = resolved;
		}

		PLOG_INFO << "HCE anchor resolution complete.\n" << healthReport();
	}

	uintptr_t get(Anchor anchor)
	{
		if ((size_t)anchor >= (size_t)Anchor::Count) return 0;
		std::scoped_lock lock(gResolveMutex);
		return gResolved[(size_t)anchor];
	}

	const char* name(Anchor anchor)
	{
		if ((size_t)anchor >= (size_t)Anchor::Count) return "<invalid>";
		return kAnchors[(size_t)anchor].name;
	}

	size_t resolvedCount()
	{
		std::scoped_lock lock(gResolveMutex);
		size_t n = 0;
		for (uintptr_t a : gResolved) if (a) ++n;
		return n;
	}

	bool allResolved() { return resolvedCount() == (size_t)Anchor::Count; }

	std::string healthReport()
	{
		std::string out;
		size_t ok = 0;
		for (const AnchorDef& def : kAnchors)
		{
			const uintptr_t a = gResolved[(size_t)def.anchor];
			if (a) ++ok;
			out += std::format("  {:<24} {}\n", def.name,
				a ? std::format("0x{:X}", a) : std::string("NOT RESOLVED - feature disabled"));
		}
		out += std::format("  {} of {} anchors resolved.", ok, (size_t)Anchor::Count);
		if (ok != (size_t)Anchor::Count)
			out += "\n  Anchors that did not resolve mean Halo Campaign Evolved changed the code they point at. "
				"The affected features refuse to run rather than read a wrong address; everything else is "
				"unaffected.";
		return out;
	}

	// See the contract in HCEAnchors.h - the three outcomes are deliberately asymmetric.
	void crossCheck(Anchor anchor, uintptr_t addressFromPointerData, const char* featureName)
	{
		// Resolve lazily rather than relying on somebody having called resolveAll() first. This is called
		// from feature constructors, whose order is decided by the DI container, so depending on ordering
		// would make the check silently absent for whichever feature happened to construct first.
		// resolveAll() is idempotent and keyed on the module base, so this is a no-op after the first call.
		if (const auto sim = ModuleCache::getModuleHandle(L"HaloSimulation_tag_release.dll"); sim.has_value())
			resolveAll((uintptr_t)sim.value());

		const uintptr_t fromSignature = get(anchor);

		// UNRESOLVED: say so, once, and continue. A signature that stopped matching means the code AROUND
		// the anchor changed - which is worth knowing, but is not evidence that the address is wrong.
		if (!fromSignature)
		{
			static std::array<std::atomic_bool, (size_t)Anchor::Count> reported{};
			if (!reported[(size_t)anchor].exchange(true))
				PLOG_WARNING << "HCEAnchors: could not cross-check " << name(anchor) << " for " << featureName
					<< " - its byte signature no longer matches this build, so the pointer-data address "
					<< std::format("0x{:X}", addressFromPointerData) << " is being used unverified. "
					"This is a warning, not a failure: the signature proves nothing when it does not match.";
			return;
		}

		if (fromSignature == addressFromPointerData) return;   // the overwhelmingly common case

		// DISAGREEMENT. Two independent derivations contradict each other, so one is wrong and we cannot tell
		// which - the signature could be matching the wrong place just as easily as the XML could be stale.
		// Refusing is the only honest answer, and it is the whole reason this file exists.
		PLOG_ERROR << "HCEAnchors: " << name(anchor) << " MISMATCH - pointer data says "
			<< std::format("0x{:X}", addressFromPointerData) << ", the byte signature finds "
			<< std::format("0x{:X}", fromSignature) << ". " << featureName << " refuses to run.";

		throw HCMRuntimeException(std::format(
			"{} is disabled: HCM's stored address for {} (0x{:X}) disagrees with the one found by scanning "
			"this build of Halo Campaign Evolved (0x{:X}). The game has almost certainly updated. HCM refuses "
			"to use either address rather than risk writing to the wrong place - please report this, quoting "
			"both numbers.",
			featureName, name(anchor), addressFromPointerData, fromSignature));
	}
}
