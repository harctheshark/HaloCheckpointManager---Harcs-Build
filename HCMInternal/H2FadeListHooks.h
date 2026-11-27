#pragma once
#include "pch.h"

// Halo 2 (halo2.dll 1.3528): the hooks that put HCM's own fade lists (H2FadeLists.h) in place of the engine's two list
// functions, for the dynamic lights and / or the drop shadows.  2026-11-19.  Shared by UncapDynamicLights and
// UncapDropShadows - each holds a lease for its lifetime and switches its own kind on and off. See the .cpp.
namespace H2FadeListHooks
{
	enum Kind : int { Lights = 0, Shadows = 1 };
	constexpr int kCapacity = 128;   // entries one of HCM's lists can hold (the engine's: 32)

	void acquire();   // a user exists (creates the hooks on the first one; nothing is attached until a kind is on)
	void release();

	// on: this kind's lists are HCM's, and `limit` of a view's entries may shine at once (the engine's own: 3 lights,
	// 4 shadows). off: the engine's own list code runs for this kind again.
	// Returns 1 = set (attached now, or as soon as halo2.dll is loaded), -1 = this halo2.dll is not build 1.3528 (nothing done).
	int configure(Kind kind, bool on, int limit);

	int listed(Kind kind);    // entries in the first view's list after its last frame (0 while off)
	int shining(Kind kind);   // of those, how many had a faded value above the draw thresholds' neighbourhood (> 0.09)
}
