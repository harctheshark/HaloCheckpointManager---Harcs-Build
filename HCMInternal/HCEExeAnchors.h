#pragma once
#include "pch.h"
#include "MultilevelPointer.h"

// ================================================================================================================
// HCE **EXE-SIDE** ANCHOR REGISTRY - the Microsoft Store port lives here.
//
// WHY THIS FILE EXISTS, AND WHY IT IS SEPARATE FROM HCEAnchors
// -----------------------------------------------------------
// Halo Campaign Evolved ships from two stores. Comparing the two installs byte for byte (2026-09-20):
//
//   HaloSimulation_tag_release.dll  ->  .text, .data, .pdata, .reloc, _RDATA and .rsrc are ALL BYTE-IDENTICAL.
//                                       Only .rdata differs, in 21 bytes: the build-time string ("00:59:13" vs
//                                       "00:59:32", NINETEEN SECONDS apart), three bytes of mirrored
//                                       TimeDateStamp, and the 16-byte RSDS PDB GUID. The entire 15,632-byte
//                                       file size difference is the Authenticode signature, which the Store
//                                       copy does not carry because Store packages are signed as packages.
//                                       => ONE COMPILATION, PACKAGED TWICE. Every sim-DLL RVA HCM ships is
//                                          already correct on the Store build, by construction.
//
//   HaloCampaignEvolved.exe         ->  a genuinely different build. Every section differs in size, and the
//                                       Store exe carries an extra `.xbld` section (the Microsoft GDK build
//                                       marker) that the Steam exe does not.
//
// That asymmetry IS the bug report: on the Store build, force checkpoint / revert / switch zone set work
// (wholly sim-side) while every overlay is dead (they need the exe-side camera).
//
// ⚠ DO NOT MERGE THIS INTO HCEAnchors.cpp. HCEAnchorUpdater parses that file by regex and scans every anchor
// it finds against the SIM DLL. An exe anchor added there would be reported as permanently BROKE, which would
// train the next person to ignore the tool's output. Keeping the two registries apart keeps that tool honest.
//
// THE CONTRACT - deliberately NOT the same as HCEAnchors::crossCheck
// ------------------------------------------------------------------
// HCEAnchors treats pointer data as authoritative and uses the signature only to CONTRADICT it. That is right
// for the sim, where the stored address is correct on every build seen so far.
//
// Here the stored address is correct on exactly ONE of the two shipping builds, so the roles invert:
//
//   signature resolves uniquely -> USE IT. It is the only derivation that is right on both stores.
//   signature does not resolve  -> fall back to the pointer-data address and warn once. That is precisely
//                                  today's behaviour, so a dead signature can never be worse than not having
//                                  had one.
//
// ⚠ THE SAFETY PROPERTY THAT MAKES "SIGNATURE WINS" SAFE. Every signature below was verified OFFLINE to match
// EXACTLY ONCE on the Steam exe AND to resolve to EXACTLY THE RVA InternalPointerData.xml already ships. A
// signature that reproduces the address Steam users are already running cannot change Steam behaviour - it can
// only also be right on the Store build. If you add an anchor here, re-run that check
// (scratchpad `verify_exe_anchors.py`, or reproduce it: 1 hit on Steam == the XML offset, 1 hit on Store).
// An anchor that cannot meet that bar does not belong in this file.
//
// COST. The exe's single executable section is ~177 MB, so each scan is real work - but resolution happens
// once per module load, from feature constructors, never on the render or game thread. HCESkyFix has scanned
// this same image for AddOccupant since it shipped.
//
// ⚠ preferAnchor returns an ABSOLUTE address baked into a MultilevelPointer::Resolved, captured once when the
// feature is constructed. That is safe here and ONLY here: the game exe is the process image, so it is mapped
// before HCM exists and never unloads or rebases while we are running. Do NOT copy this pattern for a DLL
// that can be unloaded and reloaded at a different base - use ModuleOffset for those, as the sim-side
// entries already do.
// ================================================================================================================
namespace HCEExeAnchors
{
	enum class Anchor
	{
		// AHaloWorldStreamingAreaActor::RemoveOccupant. HCM MIDHOOKS this and writes the occupant refcount at
		// [rcx+0x2D8], so a wrong address is crash-class. Steam 0x95B7890 / Store 0x8E0F160 (a constant
		// -0x7A8730; the function was relocated, not recompiled - of its 198 bytes only three differ, and all
		// three are the rel32 of one call).
		SkyFixRemoveOccupant,

		// The raw-input WndProc that BlockGameInput INLINE HOOKS with a 5-byte jmp. A wrong address here is an
		// instant crash on the message thread. Steam 0x39FCCA0 / Store 0x3618BB0.
		BlockGameInputWndProc,

		// FMinimalViewInfo::operator=, which HCM midhooks to capture the render camera. THE one address that
		// gates every HaloCER overlay: without it HCEGetCameraData throws and the trigger, BSP, soft ceiling
		// and AI squad overlays all refuse, while every non-drawing feature carries on working.
		// Steam 0x5AA79E0 / Store 0x5644390.
		//
		// ⚠ THIS ONE CANNOT BE MATCHED ON ITS OWN BYTES, EVER. The function has a byte-identical COMDAT twin
		// on both builds (Steam 0x7ED2E10, Store 0x90B8920) - identical across all 430 bytes bar a single
		// rel32 - so no signature length and no wildcarding scheme can separate them. The anchor matches the
		// CALLER instead and follows its call. See the definition in the .cpp for how that was established.
		CameraManagerUpdate,

		Count
	};

	// Resolves every anchor against the game exe. Idempotent, keyed on the module base, and never throws.
	// Callers do not normally need this - preferAnchor() resolves lazily.
	void resolveAll(uintptr_t exeModuleBase);

	// The resolved ABSOLUTE address, or 0 if this anchor did not resolve uniquely on this build.
	// ⚠ CALLERS MUST CHECK FOR 0.
	uintptr_t get(Anchor anchor);

	const char* name(Anchor anchor);
	std::string healthReport();

	// ================================================================================================
	// THE ONE CONSUMERS CALL.
	//
	// Returns a MultilevelPointer for `anchor`: the signature-resolved address when the scan found exactly
	// one match, otherwise `fromPointerData` unchanged.
	//
	// Deliberately takes the already-fetched pointer rather than a PointerDataStore, so a call site keeps its
	// existing nameof() lookup verbatim and this header stays independent of the pointer-data machinery:
	//
	//     mFunction = HCEExeAnchors::preferAnchor(HCEExeAnchors::Anchor::SkyFixRemoveOccupant,
	//         ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceSkyFixRemoveOccupantFunction), mGame),
	//         "HCE Sky Fix");
	//
	// `featureName` appears in the log line. Never throws: a feature that cannot resolve its address at all
	// still fails the way it always did, through its own original-bytes guard.
	// ================================================================================================
	std::shared_ptr<MultilevelPointer> preferAnchor(Anchor anchor,
		std::shared_ptr<MultilevelPointer> fromPointerData, const char* featureName);
}
