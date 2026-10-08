#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// PRIVATE (collision-viewer-private branch - do not push to public).
// Halo 2 (MCC 1.3528) collision wireframe + live Havok TIM overlay. Port of the standalone H2CollisionViewer
// (C:\Users\hurri\source\repos\H2CollisionViewer); the H2CV_* files are generated from it by port_to_hcm.py.
// Lines are drawn inside the game's frame at the first-person pass (sub_1807E0C60) against the game's own world
// depth and projection, or after bloom (sub_180951EC0), or at Present against the viewer's own collision depth.
class CollisionViewer : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	CollisionViewer(GameState gameImpl, IDIContainer& dicon);
	~CollisionViewer();

	std::string_view getName() override { return nameof(CollisionViewer); }
};
