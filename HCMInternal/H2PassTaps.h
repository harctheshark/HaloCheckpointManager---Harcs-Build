#pragma once
#include <atomic>

// HCM addition (2026-11-23, Halo 2 TAA - H2TAA.cpp). Callbacks from the Collision Viewer's Halo 2 render-pass hooks
// (CollisionViewer.cpp fpDetour / resolveDetour), so a second feature can run at those two points without a second inline
// hook on the same addresses. Both hooks are armed by CollisionViewer once GameDepthTap::wanted() is true (TAA sets it).
// Called on the game's render thread, only while Halo 2 is the game MCC is running, each inside an SEH guard.
// A callback must not throw C++ exceptions (catch them itself).
namespace H2PassTaps
{
	using Fn = void (*)();
	// sub_1807E0C60 (first-person pass) ENTRY: the world depth is complete, the gun and the HUD are not drawn yet;
	// halo2+0x1996B10 / +0x1996B88 hold the main view's world-to-view and projection. Called after the depth tap.
	inline std::atomic<Fn> firstPersonPass{ nullptr };
	// sub_180951EC0 ENTRY (before the original runs): the scene target holds the finished 3D frame (world + gun), before
	// bloom / lens flares / the final pass and the HUD.
	inline std::atomic<Fn> beforeResolve{ nullptr };
}
