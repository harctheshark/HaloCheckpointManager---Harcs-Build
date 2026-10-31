#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 2 (MCC 1.3528) launch trajectory overlay.
//
// Predicts the path the player (or the vehicle they are seated in) will take from its CURRENT Havok state, by
// stepping the engine's own per-tick airborne model forward, and draws it in 3D through every contact (sliding along
// walls / steep slopes, landing on walkable ground, sliding to a stop) until the body is expected to be at rest.
// Meant for momentum-storage launches: while the stored velocity is held, the line shows where you will go
// the moment you are released.
//
// Engine model (halo2.dll 1.3528, PE timestamp 0x68A0F0F2; see LaunchTrajectory.cpp for the addresses):
//   per tick, for every live Havok rigid body (sub_180712850 -> sub_180711C00 / sub_180713130):
//     v = clamp(|v|, 100 wu/s)            linear velocity cap
//     v.z -= gravity * secondsPerTick     gravity is LIVE: *(*(base + 0x1652740) + 0), default 4.1712594 wu/s^2
//   then Havok integrates  p += v * secondsPerTick.
//
// The airborne part is the engine's model. The contact / ground part is an approximation with tunable constants
// (ground friction, restitution, walkable slope); the recorded actual path, and the ground slow-down measured from it,
// are drawn next to the prediction so they can be calibrated. Stick input is ignored, and the body is three vertical
// rays (centre of mass, capsule top, capsule bottom), so wall contacts can be up to one capsule radius late.
class LaunchTrajectory : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	LaunchTrajectory(GameState gameImpl, IDIContainer& dicon);
	~LaunchTrajectory();

	std::string_view getName() override { return nameof(LaunchTrajectory); }
};
