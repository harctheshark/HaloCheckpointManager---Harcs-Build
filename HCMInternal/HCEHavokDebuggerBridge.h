#pragma once
// Plain interface between the HCM feature (HavokDebugger.cpp, stdcpplatest/permissive-) and the
// ported Halo Campaign Evolved Havok VDB payload (HCEHavokDebuggerImpl.cpp, compiled C++14 /EHa
// against the Havok 2007 SDK). Deliberately free of both HCM and Havok types so it can be included
// from either translation unit without dragging the other's headers in.
//
// Deliberately a SEPARATE namespace from HavokDebuggerBridge rather than another `game` id: the MCC
// implementation is a different engine with its own ~400 MB of BSS, and mixing HaloCER into it buys
// nothing while putting the shipped MCC path at risk. The two never coexist in one process anyway.
namespace HceHavokDebuggerBridge
{
	// (Re)establishes the debugger and resumes the gather. Idempotent: every call re-resolves the sim
	// module and its byte-signature anchors, reinstalls the per-tick engine hook, and resets the
	// gather state so a new level is rebuilt from scratch. The VDB server thread (port 25001) is spun
	// exactly once per session - re-serving would collide on the port.
	//
	// Returns false when HaloSimulation_tag_release.dll is not loaded yet, OR when the signatures that
	// locate the havok component array / the engine tick vtable slot did not resolve uniquely. HCE
	// ships no version resource, so a signature miss is the ONLY way to detect a game update, and
	// serving a scene built from stale offsets is worse than refusing.
	bool start();

	// Sets the pause flag (the gather stops, the real engine step still runs) and restores the engine
	// tick vtable slot. The VDB server thread itself never exits.
	void stop();

	bool isRunning();

	// How many of the byte-signature anchors failed to resolve on the last start(). 0 = everything
	// found. Non-zero after a successful start() means some viewers will be empty - surfaced to the
	// user rather than left as "the overlay is just broken".
	int unresolvedAnchorCount();
}
