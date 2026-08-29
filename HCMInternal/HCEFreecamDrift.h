#pragma once
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// MOMENTUM / DRIFT FOR HALO CAMPAIGN EVOLVED'S FREE CAMERA.
//
// HCM's general free camera (Halo 1-4) is driven by HCM itself, so giving it momentum was a matter of adding a
// smoother to CameraTransformer - see MomentumSmoother.h. HaloCER's free camera is the ENGINE's own: HCM only
// flips it on and sets its movement speed, and the engine integrates the player's input into the observer every
// simulation frame. There is no HCM-side interpolator to hang momentum on.
//
// So this is a LAG FILTER instead. Every simulation frame, after the engine has moved the camera, HCM replaces
// the observer's pose with a smoothed version of it. The camera then eases into motion and coasts to a stop
// instead of starting and stopping dead, which is the "less stiff" quality a dolly or handheld shot has.
//
// ⚠ IT KEEPS NO "TRUE" POSITION OF ITS OWN, BECAUSE IT DOES NOT NEED ONE. An earlier version of this file
// documented a `delta = observerPos - whatWeWroteLastFrame` accumulator, on the premise that the engine
// integrates the next frame's input from whatever we wrote. That premise is FALSE: the engine rebuilds the
// observer's nine floats from scratch every simulation frame, from the camera-mode authority at
// cameraEntry+0x20 (the full chain is in HCEFreecamKeepPosition.h). So the value read at our hook already IS
// the true, unfiltered pose, free of our previous output - and accumulating a delta against it fed our own
// smoothing error back in every frame.
//
// That also makes this purely cosmetic, which is what a cinematic lag filter should be: the authority flies at
// full responsiveness with unmodified input, and only the rendered camera trails.
//
// ⚠⚠ THE FILTER CANNOT OVERSHOOT, AT ANY SETTING. It is two first-order exponential lags in series. One stage
// is a convex combination of the current and target values, so its output can never leave the interval between
// them - no overshoot, no oscillation, no instability, for any coefficient, any dt and any input. Cascading two
// preserves that. This is structural, not a tuning.
//
// It replaces an explicit second-order spring that RANG at 6.01 Hz for 82% of its slider (including its
// default), and that separately had almost no lag at all - a negative one at the default, and a maximum of
// 31 ms anywhere. "Shakes" and "doesn't really drift" were two distinct defects of that one filter; the
// arithmetic for both is recorded at the top of HCEFreecamDrift.cpp.
//
// ⚠ ORIENTATION IS ONE QUATERNION, NOT TWO VECTORS. Smoothing forward and up independently and then
// re-orthonormalising manufactures roll, and lets the interpolated forward shrink toward zero on a fast turn -
// where normalising it either amplifies noise or fails outright, silently leaving a frame unwritten (a visible
// pop). The basis is converted to a quaternion, nlerp'd along the shortest arc - which provably cannot shrink
// below |q| = 0.707 - and converted back, orthonormal by construction. The conversion is convention-free: it
// only ever takes forward/up in and gives forward/up back, so the engine's handedness never enters.
//
// ⚠ Runs on HCEGameThreadPump, i.e. the SIMULATION thread, after the simulation update for the frame. That is
// what makes the write deterministic rather than a race - see the long note in HCEFreecamKeepPosition.cpp about
// what happened when this was attempted from an HCM worker thread.
// ================================================================================================================

class HCEFreecamDrift : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	HCEFreecamDrift(GameState game, IDIContainer& dicon);
	~HCEFreecamDrift();
	virtual std::string_view getName() override { return nameof(HCEFreecamDrift); }
};
