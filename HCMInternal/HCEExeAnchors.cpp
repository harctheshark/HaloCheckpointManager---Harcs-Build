#include "pch.h"
#include "HCEExeAnchors.h"
#include "HCESignatureScan.h"
#include "HCEGetPlayerState.h"   // tryReadRaw, for the guarded call-opcode check
#include <array>

// See HCEExeAnchors.h for why this file is separate from HCEAnchors.cpp and what the contract is.
//
// EVERY SIGNATURE BELOW WAS VERIFIED OFFLINE AGAINST BOTH SHIPPING EXES: exactly one match on each, and the
// Steam match equal to the RVA InternalPointerData.xml already ships. That second half is the whole safety
// argument for letting a signature override pointer data - reproducing the address Steam users already run
// cannot change their behaviour.
namespace
{
	using HCEExeAnchors::Anchor;

	// How a match becomes an address.
	enum class Extract
	{
		MatchIsTarget,   // the matched address IS what we want
		CallTarget,      // the match is a CALLER; follow the E8 rel32 at +insnOffset to get the target
	};

	struct AnchorDef
	{
		Anchor anchor;
		const char* name;
		const char* signature;
		Extract extract;
		int insnOffset;               // CallTarget only: offset of the E8 within the match
		uintptr_t expectedSteamRva;   // documentation + the log line; NOT used to resolve anything
		const char* why;
	};

	constexpr AnchorDef kAnchors[] =
	{
		{ Anchor::SkyFixRemoveOccupant, "SkyFixRemoveOccupant",
		  "48 89 74 24 20 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
		  "48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 83 AF D8 02 00 00 01",
		  Extract::MatchIsTarget, 0,
		  0x95B7890,
		  "AHaloWorldStreamingAreaActor::RemoveOccupant. The pattern deliberately runs THROUGH the refcount "
		  "decrement itself (83 AF D8 02 00 00 01 = sub dword ptr [rdi+2D8h],1), so one match proves the "
		  "function, the +0x2D8 field offset AND the structure together rather than just a prologue shape. "
		  "Both call rel32s and the jcc are wildcarded - the only three bytes that differ between the two "
		  "builds across this entire function are one of those displacements. "
		  "Corroborated independently: RemoveOccupant has exactly 2 call sites and its +0x86 callee exactly 1, "
		  "on BOTH builds, which is what HCESkyFix.h already documents for the Steam build." },

		{ Anchor::BlockGameInputWndProc, "BlockGameInputWndProc",
		  "4C 89 4C 24 20 4C 89 44 24 18 89 54 24 10 48 89 4C 24 08 55 56 57 41 56 48 83 EC 78",
		  Extract::MatchIsTarget, 0,
		  0x39FCCA0,
		  "The raw-input WndProc's home-register saves plus prologue. This is byte-for-byte "
		  "blockGameInputHCEWndProcOriginalBytes from the XML, and it needs NO wildcards: all 28 bytes are "
		  "position independent, which is exactly why the same run of bytes serves as both the locator here "
		  "and the pre-patch guard there. Unique in the ~177 MB .text of both builds." },

		{ Anchor::CameraManagerUpdate, "CameraManagerUpdate",
		  "C5 F8 29 B4 24 C0 00 00 00 C4 C1 7A 10 75 00 C5 F8 57 C0 C5 F8 2F F0 0F 86 ?? ?? ?? ?? "
		  "48 83 3E 00 75 ?? 48 8D 97 70 03 00 00 48 8B CE E8 ?? ?? ?? ?? 48 8B 07 48 8B CF "
		  "FF 90 70 07 00 00 48 8B D0 48 8D 8F 80 03 00 00 E8 ?? ?? ?? ??",
		  Extract::CallTarget, 0x48,
		  0x5AA79E0,
		  "⚠ THE MATCH IS NOT THE TARGET. This 77-byte pattern is a cold (chained) chunk of "
		  "APlayerCameraManager::DoUpdateCamera; the E8 at +0x48 is its call to FMinimalViewInfo::operator=, "
		  "which is what we actually want.\n"
		  "WHY IT HAS TO BE DONE THIS WAY: operator= has a byte-identical COMDAT twin on both builds (Steam "
		  "0x7ED2E10, Store 0x90B8920). A full-body normalised sweep of every comparably sized .pdata function "
		  "found exactly two copies per build at 100% similarity and NO near misses, so no in-function "
		  "signature can ever resolve uniquely - HCM's old 16-byte prologue 'signature' matches 538 places on "
		  "Steam and was only ever a plausibility check.\n"
		  "WHY THIS IS THE RIGHT COPY: caller pairing by normalised body hash gives 9 caller functions shared "
		  "between Steam 0x5AA79E0 and Store 0x5644390, 2 between the two twins, and ZERO cross-pairing in "
		  "either direction. The twin's only callers on both builds are UE5 script-VM exec thunks - it is the "
		  "Blueprint lineage, not the render path. Corroborated by the callee at +0xED being the same "
		  "FPostProcessSettings::operator= at a constant +0x1B0 on both builds, where each twin calls a "
		  "different, larger one.\n"
		  "⚠ Being semantically anchored to the camera path is deliberate: if a future build stops splitting "
		  "DoUpdateCamera this pattern stops matching and the overlay refuses, rather than landing on a "
		  "plausible wrong function. Note the XML's claim that the twin is at 0x7ECC950 is STALE - that "
		  "address does not even carry the prologue." },
	};

	static_assert(std::size(kAnchors) == (size_t)Anchor::Count, "every Anchor needs exactly one definition");

	std::array<uintptr_t, (size_t)Anchor::Count> gResolved{};
	uintptr_t gResolvedAgainstBase = 0;
	std::mutex gResolveMutex;

	uintptr_t exeBase()
	{
		return (uintptr_t)GetModuleHandleW(nullptr);
	}
}

namespace HCEExeAnchors
{
	void resolveAll(uintptr_t exeModuleBase)
	{
		if (!exeModuleBase) return;

		std::scoped_lock lock(gResolveMutex);
		if (gResolvedAgainstBase == exeModuleBase) return;   // already done for this module load

		gResolvedAgainstBase = exeModuleBase;
		gResolved.fill(0);

		for (const AnchorDef& def : kAnchors)
		{
			int hits = 0;
			const uintptr_t match = HCESignatureScan::resolveUnique(exeModuleBase, def.signature, hits);
			if (!match)
			{
				// hits == 0 can also mean the pattern was REJECTED as unparseable - parsePattern fails rather
				// than truncating when a pattern exceeds kMaxSigBytes. If you just added an anchor and see 0
				// matches on both builds, check its length before you go hunting in the binary.
				PLOG_WARNING << "HCE exe anchor '" << def.name << "' did NOT resolve (" << hits
					<< " matches, exactly 1 required). Falling back to the stored pointer-data address, which "
					"is correct on the Steam build only. If this is the Microsoft Store build, the feature "
					"that needs it will disable itself through its own original-bytes guard.";
				continue;
			}

			uintptr_t resolved = match;

			if (def.extract == Extract::CallTarget)
			{
				// Require the byte we are about to follow to actually BE a call. If the pattern ever drifts
				// so that +insnOffset lands elsewhere, reading a displacement out of it would produce a
				// confident, arbitrary address - the exact failure this whole file exists to prevent.
				uint8_t opcode = 0;
				if (!HCEGetPlayerState::tryReadRaw(match + def.insnOffset, &opcode, sizeof(opcode)) || opcode != 0xE8)
				{
					PLOG_WARNING << "HCE exe anchor '" << def.name << "' matched at "
						<< std::format("0x{:X}", match) << " but the byte at +"
						<< std::format("0x{:X}", def.insnOffset) << " is "
						<< std::format("0x{:02X}", opcode) << ", not a call (E8). Refusing to follow it.";
					continue;
				}

				resolved = HCESignatureScan::ripTarget(match + def.insnOffset, 1, 5);
				if (!resolved)
				{
					PLOG_WARNING << "HCE exe anchor '" << def.name << "' could not read the call displacement "
						"at " << std::format("0x{:X}", match + def.insnOffset) << ".";
					continue;
				}
			}

			gResolved[(size_t)def.anchor] = resolved;

			// Worth one line each: the rva tells us at a glance WHICH build this is, without needing a
			// version resource the title does not have.
			const uintptr_t rva = resolved - exeModuleBase;
			PLOG_INFO << "HCE exe anchor '" << def.name << "' resolved to rva " << std::format("0x{:X}", rva)
				<< (rva == def.expectedSteamRva
					? " (matches the shipped Steam address)"
					: std::format(" (Steam ships 0x{:X} - so this is a DIFFERENT exe build, most likely the "
						"Microsoft Store one; the scan is what makes it work)", def.expectedSteamRva));
		}
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

	std::string healthReport()
	{
		std::scoped_lock lock(gResolveMutex);
		std::string out;
		size_t ok = 0;
		for (const AnchorDef& def : kAnchors)
		{
			const uintptr_t a = gResolved[(size_t)def.anchor];
			if (a) ++ok;
			out += std::format("  {:<26} {}\n", def.name,
				a ? std::format("0x{:X}  (rva 0x{:X})", a, gResolvedAgainstBase ? a - gResolvedAgainstBase : 0)
				  : std::string("NOT RESOLVED - using the stored Steam address"));
		}
		out += std::format("  {} of {} exe anchors resolved.", ok, (size_t)Anchor::Count);
		return out;
	}

	std::shared_ptr<MultilevelPointer> preferAnchor(Anchor anchor,
		std::shared_ptr<MultilevelPointer> fromPointerData, const char* featureName)
	{
		// Resolve lazily rather than depending on somebody having called resolveAll() first: this runs from
		// feature constructors, whose order the DI container decides. resolveAll() is idempotent and keyed on
		// the module base, so this is a no-op after the first call.
		resolveAll(exeBase());

		const uintptr_t resolved = get(anchor);
		if (!resolved)
		{
			// Not a failure. The stored address is still right on Steam, and the feature's own guard is what
			// stops a wrong one from being patched. Say it once per anchor so a Store user reporting "this
			// feature does nothing" has the reason in their log.
			static std::array<std::atomic_bool, (size_t)Anchor::Count> reported{};
			if (!reported[(size_t)anchor].exchange(true))
				PLOG_WARNING << "HCEExeAnchors: " << name(anchor) << " did not resolve by signature, so "
					<< featureName << " is using the stored pointer-data address unverified. That address is "
					"correct on the Steam build of Halo Campaign Evolved; on any other build this feature will "
					"refuse to attach rather than patch the wrong code.";
			return fromPointerData;
		}

		PLOG_DEBUG << "HCEExeAnchors: " << featureName << " is using the signature-resolved address for "
			<< name(anchor) << " (" << std::format("0x{:X}", resolved) << ")";

		return std::make_shared<MultilevelPointerSpecialisation::Resolved>((void*)resolved);
	}
}
