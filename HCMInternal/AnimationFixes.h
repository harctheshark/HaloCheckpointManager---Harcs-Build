#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IAnimationFixesImpl { public: virtual ~IAnimationFixesImpl() = default; };

// Halo 2 only. One toggle ("Animation Fixes") driving three runtime fixes:
//   1. Rocket launcher firing interpolation (barrel 60-tick pop): mid-hook the anim codec dispatcher and
//      integer-step (frac=0) the rocket fire anim's tail frames so the f25 antipode half-frame can't pop.
//   2. Cyclotron elevator interpolation: inline-hook the interp gate and return "can't interpolate" for the
//      elevator (object 0 on Cyclotron) on its loop-reset frames, killing the one-tick backward blend.
//   3. Rocket launcher animation pop: the existing heap tag-data patcher (re-applied on a background timer).
// Replaces the old standalone RocketLauncherAnimationFix toggle.
class AnimationFixes : public IOptionalCheat
{
private:
	std::unique_ptr<IAnimationFixesImpl> pimpl;

public:
	AnimationFixes(GameState gameImpl, IDIContainer& dicon);
	~AnimationFixes();

	std::string_view getName() override { return nameof(AnimationFixes); }
};
