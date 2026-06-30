#pragma once
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
}
