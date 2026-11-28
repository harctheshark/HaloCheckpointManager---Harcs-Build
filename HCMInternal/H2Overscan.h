#pragma once
#include <atomic>
#include <cstdint>

// Halo 2 TAA OVERSCAN (user 2026-11-27: "apply a TAA overscan, just so the entire screen the player sees is stable").
//
// TAA has no history for what slides in at the screen edges, so the edges show the raw, aliased frame for a few frames. With
// overscan the 3D view is rendered a few percent WIDER (and taller) than the screen into scene targets that much bigger, TAA
// runs on all of it, and the final pass shows only the centre - the part you see. What enters the screen was already in the
// hidden border last frame, so it arrives with history.
//
//  - H2Supersampling builds the targets (the engine's render scale, its window-size getters x the overscan factor) and makes
//    the final pass sample only the centre (the pixel-bounds rectangle halo2+0x1996A58 = the crop in TARGET pixels, and the
//    two getters answer the target size to the quad builder sub_180976010 only) - so the crop is pixel-exact.
//  - H2TAA widens the main view's and the first-person pass's projection window by the same pixels (widen() below), in its
//    hook on the projection-rect function 0x7DFCD0, before its jitter.
//  - Everything after the final pass (the HUD, the fade overlay, HCM's own overlays) stays at the window: unchanged.
namespace h2os
{
	struct Layout
	{
		int windowW = 0, windowH = 0;   // the real window (what the player sees)
		int targetW = 0, targetH = 0;   // the scene targets, overscan included (what the engine renders)
		int innerW = 0, innerH = 0;     // the shown part in target pixels (= window x supersampling factor)
		int offX = 0, offY = 0;         // where it starts in the target
	};

	// Written by H2Supersampling when the engine builds its render rectangle (Halo 2's thread, at a rebuild - the same thread
	// that renders), read while rendering. gActive = the 3D frame is SCALED (supersampling and / or overscan) and gLayout is
	// valid - without overscan the shown part is the whole target; gOverscan = there is a hidden border. Stored last.
	inline Layout gLayout{};
	inline std::atomic<bool> gActive{ false };
	inline std::atomic<bool> gOverscan{ false };

	// H2Supersampling registers this; H2TAA calls it with the wanted overscan in percent per side - left / right, then top /
	// bottom (0 = off). Returns 1 applied (the engine rebuilds its targets in place at the next frame boundary) or already as
	// wanted, 0 set but it applies once Halo 2 is running, -1 not possible (other Halo 2 build).
	using RequestFn = int (*)(float percentLeftRight, float percentTopBottom);
	inline std::atomic<RequestFn> gRequest{ nullptr };

	// Widen a projection window rect {left, right, bottom, top} built for a view of viewW x viewH pixels, so the view's
	// centre (innerW x innerH at offX, offY) is exactly the picture without overscan. Only for a view that fills the whole
	// target (not split screen). True when it widened.
	inline bool widen(float* rect, int viewW, int viewH)
	{
		if (!gOverscan.load(std::memory_order_acquire) || !rect) return false;
		const Layout& L = gLayout;
		if (viewW != L.targetW || viewH != L.targetH || L.innerW <= 0 || L.innerH <= 0) return false;
		const float sx = (rect[1] - rect[0]) / (float)L.innerW;   // window units per target pixel
		const float sy = (rect[3] - rect[2]) / (float)L.innerH;
		rect[0] -= sx * (float)L.offX;
		rect[1] += sx * (float)(L.targetW - L.innerW - L.offX);
		rect[3] += sy * (float)L.offY;                          // top
		rect[2] -= sy * (float)(L.targetH - L.innerH - L.offY);  // bottom
		return true;
	}
}
