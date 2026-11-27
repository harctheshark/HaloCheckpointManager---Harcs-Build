#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 2 (MCC 1.3528): temporal anti-aliasing (2026-11-23, user: "a friend got it working by injecting their own motion
// predictor into the engine"). A sub-pixel jitter in the engine's projection (hook on halo2+0x7DFCD0) plus HCM's own
// resolve inside the frame (via the Collision Viewer's pass hooks, H2PassTaps.h): the current frame blended with a
// camera-reprojected history. See H2TAA.cpp.
class H2TAA : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H2TAA(GameState gameImpl, IDIContainer& dicon);
	~H2TAA();

	virtual std::string_view getName() override { return nameof(H2TAA); }
};
