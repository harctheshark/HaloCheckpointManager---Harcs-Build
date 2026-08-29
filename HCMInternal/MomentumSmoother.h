#pragma once
#include "pch.h"
#include "ISmoother.h"

// ================================================================================================================
// A smoother that CARRIES VELOCITY, for cinematic camera work.
//
// LinearSmoother is a pure exponential approach - `current += (desired - current) * rate` - and it is stateless
// between frames. That is why it cannot drift: the instant input stops, `desired` stops moving, the remaining
// distance collapses over a few frames and the camera parks. Responsive, but stiff, and it always decelerates
// the same way it accelerated.
//
// This one keeps a velocity across frames:
//
//     velocity = velocity * drag + (desired - current) * accel
//     current += velocity
//
// which changes the feel in the two places that matter for a camera move:
//
//   * EASE IN. Velocity starts at zero and builds, so a move begins gently instead of snapping to full speed.
//   * COAST OUT. When input stops, `desired` stops but velocity does not - it decays geometrically by `drag`,
//     so the camera glides to a halt over ~1/(1-drag) frames instead of stopping dead. That overshoot-and-settle
//     is the "less stiff" quality a handheld or dolly shot has.
//
// PARAMETERS
//   accel  - reuses the existing "Snap Factor" setting. How hard the target pulls. Higher = more responsive.
//   drag   - how much velocity survives each frame. 0 makes this behave like LinearSmoother (velocity is
//            rebuilt from scratch every frame); 0.9 is a long, heavy glide. Values at or above 1 would never
//            lose energy and the camera would oscillate forever, so the caller clamps below 1.
//
// ⚠ FRAME-RATE DEPENDENT, DELIBERATELY. ISmoother::smooth takes no delta time and LinearSmoother is
// frame-rate dependent in exactly the same way. Making only this one time-corrected would mean the two modes
// disagreed about what a given Snap Factor means, and every saved config would change behaviour. Matching the
// existing convention is the lesser evil; correcting both would be a separate change with its own migration.
//
// ⚠ SETTLING IS EXPLICIT. A geometric decay never mathematically reaches zero, so without a floor the camera
// would creep by sub-micron amounts forever and never compare equal to its target. Once both the remaining
// distance and the velocity are below the threshold we snap and zero the velocity, which is what lets
// "is the camera still moving" checks elsewhere terminate.
// ================================================================================================================

template<typename valueType>
class MomentumSmoother : public ISmoother<valueType>
{
private:
	valueType mVelocity{};
	float mAccel;
	float mDrag;

	static constexpr float kSettleDistance = 0.0001f;
	static constexpr float kSettleVelocity = 0.0001f;

	static float magnitude(float v) { return std::abs(v); }
	static float magnitude(const DirectX::SimpleMath::Vector3& v) { return v.Length(); }

public:
	MomentumSmoother(float accel, float drag) : mAccel(accel), mDrag(drag) {}

	void setSmoothRate(float accel) { mAccel = accel; }

	// Clamped strictly below 1: at 1.0 the velocity term never decays and the camera oscillates indefinitely
	// around its target rather than arriving.
	void setDrag(float drag)
	{
		if (drag < 0.f) drag = 0.f;
		if (drag > 0.95f) drag = 0.95f;
		mDrag = drag;
	}

	// Drop the carried velocity. Called when the mode is switched or the camera is placed somewhere new, so a
	// glide left over from the previous move cannot leak into the next one.
	void reset() override { mVelocity = valueType{}; }

	virtual void smooth(valueType& currentValue, valueType desiredValue) override
	{
		const valueType distance = desiredValue - currentValue;

		mVelocity = mVelocity * mDrag + distance * mAccel;

		if (magnitude(distance) < kSettleDistance && magnitude(mVelocity) < kSettleVelocity)
		{
			currentValue = desiredValue;
			mVelocity = valueType{};
			return;
		}

		currentValue = currentValue + mVelocity;
	}
};
