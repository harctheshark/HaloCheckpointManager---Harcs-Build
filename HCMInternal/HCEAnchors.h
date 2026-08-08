#pragma once
#include "pch.h"

// ================================================================================================================
// HCE ANCHOR REGISTRY - every Halo Campaign Evolved address, found by BYTE SIGNATURE instead of trusted from XML.
//
// WHY THIS EXISTS
// ---------------
// HaloCampaignEvolved ships NO VERSION RESOURCE. Every build of HaloSimulation_tag_release.dll reports 0.0.0.0,
// so InternalPointerData.xml's Version attribute cannot tell two builds apart. That makes a hard-coded address
// worse than useless after a game update: it does not fail, it silently resolves to whatever now occupies that
// offset. On one observed update the tag address table moved from 0x2C2DCC0 to 0x2C2CCC0 - the stale address
// landed inside a string table, and everything downstream produced confident nonsense.
//
// A signature cannot make an update harmless, and this header does not claim to. What it changes is the FAILURE
// MODE: a signature that no longer matches uniquely resolves NOTHING, so the feature that needed it refuses and
// says so, instead of reading garbage. Small updates keep working with no user action; large ones report
// exactly which anchors died.
//
// THE CONTRACT
// ------------
//   * Exactly one match is a resolution. Zero matches, or more than one, resolves to 0.
//   * An anchor NEVER falls back to a remembered address. That is the whole point.
//   * Resolution is per-anchor, so one dead anchor disables one feature rather than the port.
//   * healthReport() names every anchor and whether it resolved, so an update produces a readable answer.
//
// SCANNING IS SAFE AND CHEAP HERE. The sim has exactly one executable section and ZERO base relocations inside
// it, so the bytes on disk are the bytes in memory regardless of ASLR, and a scan is deterministic. Resolution
// happens once per module load, never on the render thread.
//
// ⚠ WHAT IS *NOT* HERE. Structural constants - tag block offsets, record strides, field offsets - stay in
// InternalPointerData.xml. Those are tag-FORMAT facts rather than code addresses: they move only if the tag
// format itself changes, they cannot be signature-scanned (there is no code to match), and a wrong one is
// caught by the struct-walk closure checks documented in the XML.
// ================================================================================================================
namespace HCEAnchors
{
	enum class Anchor
	{
		TlsIndex,                 // _tls_index. Structural: read from the PE TLS directory, not a signature.
		GameThreadEntrypoint,
		PhysicsTable,
		CurrentLevel,
		CurrentBSP,
		TickCounterSlot,
		ForceRevertFlag,
		DoubleRevertFlag,
		CheckpointControlBlock,
		CheckpointSlotTable,
		CheckpointStateLength,
		CheckpointSaveInFlight,
		CheckpointShell,
		CheckpointStateBlock,
		Count
	};

	// Resolves every anchor against the sim module. Idempotent and cheap to call again; a different module base
	// (a reload) re-arms it. Never throws.
	void resolveAll(uintptr_t simModuleBase);

	// The resolved ABSOLUTE address, or 0 if this anchor could not be resolved on this build.
	// ⚠ CALLERS MUST CHECK FOR 0. That is the update-detection mechanism; treating 0 as an address defeats
	// the entire purpose of this file.
	uintptr_t get(Anchor anchor);

	// Human-readable name, for logs and user-facing errors.
	const char* name(Anchor anchor);

	size_t resolvedCount();
	bool allResolved();

	// One line per anchor, resolved or not. Written to the log after resolution, and worth quoting to a user
	// who reports "feature X stopped working after the game updated".
	std::string healthReport();
}
