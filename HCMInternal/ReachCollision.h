#pragma once
// Halo: Reach world collision for the Launch Trajectory overlay, read straight from the live Havok world in
// haloreach.dll (MCC 1.3528). No Havok SDK or extra files needed: the layout and shape vtables are the ones HCM's
// Havok Debugger reverse engineered (HavokDebuggerImpl.cpp, resolveAddrs / gatherBSP), re-implemented here without
// any Havok types.
#include <vector>
#include <string>
#include <cstdint>

namespace ReachCollision
{
	// Call on the GAME thread (e.g. from the game tick hook): the structure's collision mesh is decoded through the
	// engine's own getChildShape, which needs the game thread's Havok TLS. Cheap when nothing changed; when the level
	// / loaded BSPs change it walks the world once (can take a moment on big levels).
	void tick(uintptr_t haloreachBase, bool includeObjects);

	// Any thread: copies the triangles (9 floats each, world space) if they changed since *gen. Returns true if copied.
	bool snapshot(std::vector<float>& out, long& gen);

	// Short human-readable state, for the overlay label and the launch logs.
	std::string status();
}
