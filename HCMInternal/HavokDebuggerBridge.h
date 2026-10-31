#pragma once
#include <string>
// Plain interface between the HCM feature (HavokDebugger.cpp, stdcpplatest/permissive-)
// and the ported Havok VDB injector (HavokDebuggerImpl.cpp, compiled C++14 against the
// Havok 2007 SDK). Deliberately free of both HCM and Havok types so it can be included
// from either translation unit without dragging the other's headers in.
namespace HavokDebuggerBridge
{
	// (Re)establishes the debugger for the given game and resumes the gather. The VDB server
	// thread (port 25001) is spun once per session; for Halo 3 the per-tick engine step hook is
	// re-installed each call. game: 0 = Halo 3 (halo3.dll, live Havok world), 1 = Halo 2
	// (halo2.dll, static world BSP collision from the tag). Returns false if the target game
	// module isn't loaded yet.
	bool start(int game);

	// Sets the pause flag: the per-tick gather is skipped (the real engine step still runs)
	// and the VDB server idles. The step hook stays installed for the session.
	void stop();

	bool isRunning();

	// ---- gather-only (no VDB server): Halo Reach world collision for the collision viewer ----
	// Installs the engine step hook (shared with the debugger) and gathers the structure collision on the engine thread
	// whenever the world's content changes, without starting the VDB server. game: 3 = Reach only. False when the module
	// is not loaded or this build has no Havok SDK. Independent of start()/stop(): the hook stays while either wants it.
	bool startGatherOnly(int game);
	void stopGatherOnly();
	// the gathered structure triangles' version (changes on every regather; 0 = none yet)
	unsigned bspVersion();
	// copy the structure triangles (world space, 9 floats each: v0 v1 v2 xyz, winding unknown) into dst (maxTris of
	// room). False while the engine thread is rewriting them (try again next frame). outCount = triangles copied.
	bool copyBspTris(float* dst, int maxTris, int* outCount, unsigned* outVersion);

	// ---- Reach objects (scenery / crates / machines) for the collision viewer, live ----
	// The Havok component datums (an object's +0xCC on Reach) whose rigid bodies to emit, with a caller tag per component
	// (e.g. the object type), replacing the previous set. The engine thread resolves them in the Havok component table. Any thread. The gather-only engine hook emits them every other tick, in world
	// space at the bodies' current transforms (so a pushed crate or a moving lift follows).
	void setReachObjects(const unsigned long long* components, const unsigned char* tags, int count);
	// those triangles (9 floats each) and each triangle's component tag; false while being rewritten (try next frame)
	bool copyObjectTris(float* dst, unsigned char* tags, int maxTris, int* outCount, unsigned* outVersion);
	// the version copyObjectTris would report now - check it before copying (the copy is up to 14 MB)
	unsigned objectTrisVersion();
	// gather-only: the structure pass (a full regather whenever the world's content changes) on/off - off while the
	// collision viewer draws Poorboy's world file, so the game thread only emits the objects
	void setGatherStructure(bool on);
	// Reach: the game thread's TLS block (TLS[TlsIndex]) as last seen by the gather-only engine hook (0 = not yet)
	unsigned long long reachEngineTls();
	// the last object pass on the game thread, in microseconds
	unsigned objectGatherMicros();
	// the last object pass's machines (tag 7): per Havok component its bodies, top shape vtable and triangles emitted
	std::string reachMachineDiag();
	// The collision triangles the gather has collected (world space, 9 floats per triangle: a, b, c). Used by the
	// Launch Trajectory overlay as Reach's collision. which: 0 = structure (world) collision, 1 = instanced geometry,
	// 2 = static object collision (scenery, crates ...), 3 = soft ceilings. Returns the triangle count and points
	// *tris at the gather's own buffer (rewritten on the engine thread when *gen changes - copy it, then check gen).
	int collisionTris(int which, const float** tris, long* gen);
}
