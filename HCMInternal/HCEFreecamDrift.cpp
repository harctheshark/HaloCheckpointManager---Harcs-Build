#include "pch.h"
#include "HCEFreecamDrift.h"
#include "HCEGameThreadPump.h"
#include "HCEGetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See HCEFreecamDrift.h for why this cannot overshoot and why it reads no clock.

namespace
{
	// Mirrored from HCEGetPlayerState; see HCEFreecamKeepPosition.cpp for the same set.
	constexpr int64_t kObserverGlobalsTls = 0x4E8;
	constexpr int64_t kObserverStride = 0x410;
	constexpr int64_t kCameraEntriesTls = 0x148;
	constexpr int64_t kCameraEntryStride = 0x1AC;
	constexpr int64_t kCameraObserverIndex = 0x180;
	constexpr int64_t kPlayerControlTls = 0xB8;
	constexpr int64_t kFreecamMaster = 0x9C8;
	constexpr int64_t kObserverMagicLow = 0x008;
	constexpr int64_t kObserverMagicHigh = 0x410;
	constexpr uint32_t kObserverMagic = 0x72616421;
	constexpr int64_t kPosition = 0x154;
	constexpr int64_t kForward = 0x17C;
	constexpr int64_t kUp = 0x188;

	struct Vec3 { float x, y, z; };
	struct Quat { float x, y, z, w; };

	inline Vec3 sub3(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline Vec3 add3(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline Vec3 mul3(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline float len3(const Vec3& a) { return std::sqrt(dot3(a, a)); }
	inline Vec3 cross3(const Vec3& a, const Vec3& b)
	{
		return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	}

	inline bool finite3(const Vec3& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)
			&& std::abs(v.x) < 1.0e6f && std::abs(v.y) < 1.0e6f && std::abs(v.z) < 1.0e6f;
	}

	inline bool normalise(Vec3& v)
	{
		const float l = len3(v);
		if (!(l > 1.0e-6f)) return false;
		v = mul3(v, 1.0f / l);
		return true;
	}

	// ============================================================================================================
	// THE FILTER: TWO FIRST-ORDER EXPONENTIAL LAGS IN SERIES. IT CANNOT OVERSHOOT, AT ANY SETTING, EVER.
	//
	// One stage is  x += (target - x) * alpha  with  alpha = 1 - exp(-dt / T)  in (0, 1]. That is a CONVEX
	// COMBINATION of x and target, so the output can never leave [x, target]: no overshoot, no oscillation, no
	// instability, for any alpha, any dt and any target motion. Cascading two stages preserves it - stage 2's
	// target is stage 1's output, which is itself bounded by the true target - so the step response is monotone.
	// This is a STRUCTURAL property, not a tuning. There is no parameter value that makes it ring.
	//
	// ⚠ WHAT IT REPLACES, AND WHY THE OLD ONE SHOOK. The previous filter was an explicit second-order spring,
	// `velocity = velocity*drag + (target-current)*accel; current += velocity`, with accel hard-coded at 0.35 and
	// only `drag` on the slider. Its state matrix on [velocity, error] is [[drag, accel], [-drag, 1-accel]], whose
	// determinant is drag EXACTLY (accel cancels) and whose trace is drag + 1 - accel. The poles are therefore
	// complex whenever (1 + drag - accel)^2 < 4*drag, i.e. for drag > 0.1668 - which is 82% of the old slider,
	// including its 0.75 default. At that default it rang at 6.01 Hz with 1.10 frames-of-travel of overshoot and a
	// 4.82-tick half-life; at the 0.95 maximum the half-life was 27 ticks, so raising "drift" made the WOBBLE
	// longer, not the glide.
	//
	// ⚠⚠ AND A SECOND, INDEPENDENT DEFECT IN THE SAME FILTER: it barely lagged at all. The steady-state trail of
	// that spring is d*(1 - (1-drag)/accel) frames-of-travel, which at the 0.75 default is NEGATIVE (-0.286 ticks)
	// - the rendered camera LED the real one by 4.8 ms. Zero lag sat at drag = 1-accel = 0.65, and the maximum
	// trail available anywhere on the slider was 1.86 ticks (31 ms), at drag = 0 where there was no drift at all.
	// The gain that actually sets lag is `accel`, which was exposed nowhere. So "shakes" and "doesn't really drift"
	// were TWO separate defects, and fixing only the ring would have left the camera with no perceptible coast.
	//
	// Two stages rather than one: a single pole starts with a jerk (velocity steps from 0 to its ramp), two stages
	// start with zero acceleration - the soft push-off of a weighted rig - and settle FASTER for the same lag
	// (2.92*tau to 2%, versus 3.91*tau for one pole).
	//
	// alpha is EXACT, not an approximation: integrating dx/dt = (target - x)/T over dt gives exactly
	// x += (target - x)*(1 - exp(-dt/T)). The same tau produces identical continuous behaviour at any tick rate,
	// so there is no per-frame-rate tuning and no dt-dependent stability limit.
	//
	// TIME BASE: dt is FIXED at one simulation tick, and NO CLOCK IS READ. ⚠ HCM's Game Speed feature hooks
	// QueryPerformanceCounter / GetTickCount / GetTickCount64 / timeGetTime process-wide, so every wall-clock read
	// inside HCM is scaled while it is on; a clock-based dt here would silently change the drift whenever the user
	// touched Game Speed. The pump is 1:1 with the simulation update (sub_1801AF290 has exactly one caller, and it
	// contains both `call sub_1801AE530` and our hook site at 0x1AF2BD), and the sim tick is a hard 60 Hz (literal
	// 0x3C at 0x180208AB9). tau is therefore denominated in SIMULATION seconds, which is also what you want: the
	// drift stretches with slow motion instead of fighting it.
	// ============================================================================================================
	constexpr float kSimTickSeconds = 1.0f / 60.0f;
	constexpr int   kStages = 2;

	// The slider is a COAST TIME in seconds. Settle-to-2% of a two-stage cascade is 5.83 stage time constants =
	// 2.92*tau, so tau = coast/3 makes the slider read, to within ~10%, as "seconds until the camera has visibly
	// stopped". The trail you see while flying is then coast/3 seconds of travel.
	constexpr float kMaxCoastSeconds = 10.0f;
	constexpr float kTauPerCoastSecond = 1.0f / 3.0f;

	// An exponential never exactly arrives. Below this, park on the true pose so a stationary camera is bit-stable
	// frame to frame (it matters for stills) and we never grind toward denormals.
	constexpr float kSnapUnits = 1.0e-4f;

	// ---- discontinuity (teleport / revert / level load / camera cut) detection ------------------------------
	// ⚠ This measures a jump in the ENGINE'S OWN pose between consecutive ticks. It deliberately does NOT measure
	// how far the filter is currently lagging, which is what the previous 250-unit guard did - and that quantity
	// grows with both the smoothing setting and Camera Move Speed, so ordinary flight on a heavy setting walked
	// into the threshold and re-seeded MID-FLIGHT, which is a snap. Steady-state lag alone reaches ~650 ms of
	// travel at the top of the new slider, so the old guard would now be routinely trippable.
	//
	// The allowance auto-scales to the user's actual move speed: kJumpFloor covers starting from a dead stop
	// (where gMaxStep is still 0) and the relative term covers every speed above that. A teleport small enough to
	// slip through is bounded by ~9 ticks of the user's own flight speed - by construction indistinguishable from
	// ordinary motion, never a long slide across the map.
	//
	// ⚠ kJumpFloor is the ONE empirical constant in this file. HCE inherits Halo world units (1 wu = 10 ft), the
	// engine freecam is a few wu/s and hceCameraMoveSpeed clamps at 50x, giving an estimated worst case near
	// 4 wu/tick; 32 is ~8x that and still far below any teleport. TO VERIFY: set Camera Move Speed to 50, log
	// len3(pos - gPrevTruePos) per tick while flying, confirm the maximum sits well under 32. Too low costs one
	// un-smoothed tick when starting from a standstill; too high costs a smoothed rather than instant response to
	// a small teleport. Both are mild.
	constexpr float kJumpFloor = 32.0f;
	constexpr float kJumpFactor = 8.0f;
	// cos(90 deg / 2). A quaternion dot is cos(half-angle), so this fires on 90 degrees of TOTAL rotation - yaw,
	// pitch and roll together - inside one 60 Hz tick, i.e. 5400 deg/s. Not reachable by hand; a cut or a
	// pole-crossing basis flip is. |dot| is used, so the quaternion double cover cannot false-trigger it.
	constexpr float kJumpRotDot = 0.70710678f;

	inline float dotQ(const Quat& a, const Quat& b) noexcept
	{
		return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	}

	inline bool normaliseQ(Quat& q) noexcept
	{
		const float l = std::sqrt(dotQ(q, q));
		if (!(l > 1.0e-6f)) return false;
		const float inv = 1.0f / l;
		q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
		return true;
	}

	// Shortest-arc normalised lerp. After the sign fix dot >= 0, so the interpolant's squared length is
	// (1-a)^2 + a^2 + 2a(1-a)dot >= 0.5, i.e. |q| >= 0.707 BEFORE normalisation: it is arithmetically impossible
	// for this to shrink toward zero or reach a degenerate case - which is exactly the failure the old
	// two-vector-plus-Gram-Schmidt version had on fast turns. It traverses the arc monotonically for a in [0,1],
	// so like the scalar stage it cannot overshoot.
	inline Quat nlerpTo(const Quat& current, const Quat& target, float alpha) noexcept
	{
		Quat t = target;
		if (dotQ(current, t) < 0.0f) { t.x = -t.x; t.y = -t.y; t.z = -t.z; t.w = -t.w; }
		Quat r{
			current.x + (t.x - current.x) * alpha,
			current.y + (t.y - current.y) * alpha,
			current.z + (t.z - current.z) * alpha,
			current.w + (t.w - current.w) * alpha };
		if (!normaliseQ(r)) return current;   // unreachable given the bound above; cheap insurance
		return r;
	}

	// ⚠ CONVENTION-FREE, DELIBERATELY. We build the rotation whose COLUMNS are (forward, right, up) with
	// right = up x forward. cross(forward, right) == up identically, so that ordered triple is right-handed and the
	// matrix is a proper rotation for ANY orthonormal (forward, up) - whatever handedness or axis convention the
	// engine uses. Since forward/up are the only things that go in and the only things that come out, the engine's
	// convention never enters this code. (This is why we do NOT smooth the scalar yaw/pitch at cameraEntry+0x2C:
	// those are only angles under camera modes 2 and 5, they wrap, and the convention is unknown - all for an
	// outcome this is already equivalent to.)
	inline bool basisToQuat(Vec3 fwd, Vec3 up, Quat& out) noexcept
	{
		if (!normalise(fwd)) return false;
		up = sub3(up, mul3(fwd, dot3(up, fwd)));
		if (!normalise(up)) return false;
		const Vec3 right = cross3(up, fwd);

		const float m00 = fwd.x, m01 = right.x, m02 = up.x;
		const float m10 = fwd.y, m11 = right.y, m12 = up.y;
		const float m20 = fwd.z, m21 = right.z, m22 = up.z;

		const float tr = m00 + m11 + m22;
		float s;
		if (tr > 0.0f)
		{
			s = std::sqrt(tr + 1.0f) * 2.0f;
			out.w = 0.25f * s; out.x = (m21 - m12) / s; out.y = (m02 - m20) / s; out.z = (m10 - m01) / s;
		}
		else if (m00 > m11 && m00 > m22)
		{
			s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
			out.w = (m21 - m12) / s; out.x = 0.25f * s; out.y = (m01 + m10) / s; out.z = (m02 + m20) / s;
		}
		else if (m11 > m22)
		{
			s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
			out.w = (m02 - m20) / s; out.x = (m01 + m10) / s; out.y = 0.25f * s; out.z = (m12 + m21) / s;
		}
		else
		{
			s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
			out.w = (m10 - m01) / s; out.x = (m02 + m20) / s; out.y = (m12 + m21) / s; out.z = 0.25f * s;
		}
		return normaliseQ(out);
	}

	// Columns 0 and 2 of the rotation matrix - the exact inverse of basisToQuat. The result is orthonormal by
	// construction, so there is no re-orthonormalisation step and no degenerate "write nothing this frame" path
	// that would show one frame of raw pose.
	inline void quatToBasis(const Quat& q, Vec3& fwd, Vec3& up) noexcept
	{
		const float x = q.x, y = q.y, z = q.z, w = q.w;
		fwd = { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y) };
		up = { 2.0f * (x * z + w * y),        2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y) };
	}

	// One stage's blend factor for a total lag of tauSeconds spread over kStages stages.
	// The !(> ) form also swallows NaN: a garbage setting disables the filter rather than corrupting the camera.
	inline float stageAlpha(float tauSeconds, float dt) noexcept
	{
		if (!(tauSeconds > 1.0e-4f)) return 1.0f;                       // slider at 0 -> pass-through
		const float a = 1.0f - std::exp(-dt / (tauSeconds / (float)kStages));
		if (!(a > 0.0f)) return 0.0f;
		return a > 1.0f ? 1.0f : a;
	}

	inline Vec3 lerpTo3(const Vec3& current, const Vec3& target, float alpha) noexcept
	{
		return add3(current, mul3(sub3(target, current), alpha));
	}

	// ---- settings, published from the GUI thread ----------------------------------------------------
	std::atomic<bool>  gActive{ false };
	std::atomic<float> gTau{ 0.75f * kTauPerCoastSecond };   // seconds of trail; the slider is 3x this

	// ---- filter state. Sim thread only, except gHaveState. ------------------------------------------
	// ⚠ gHaveState is atomic because onToggle clears it from the GUI thread while the pump may be running. The
	// clear was already correctly ORDERED - it happens before the release-store to gActive that the pump
	// acquire-loads - it was only missing atomicity.
	std::atomic<bool> gHaveState{ false };
	Vec3 gPos[kStages]{};          // stage outputs; gPos[kStages-1] is what the viewer sees
	Quat gRot[kStages]{};
	Vec3 gPrevTruePos{};           // last tick's ENGINE pose - the discontinuity test's reference
	Quat gPrevTrueRot{};
	float gMaxStep = 0.0f;         // largest per-tick engine step since the last re-seed

	void setCoastSeconds(float coastSeconds) noexcept
	{
		float v = coastSeconds;
		if (!(v > 0.0f)) v = 0.0f;                       // also catches NaN
		if (v > kMaxCoastSeconds) v = kMaxCoastSeconds;
		gTau.store(v * kTauPerCoastSecond, std::memory_order_relaxed);
	}

	inline bool rdPtr(uintptr_t a, uintptr_t& v) { return HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)) && v != 0; }
	inline bool rdVec(uintptr_t a, Vec3& v) { return HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)) && finite3(v); }
	inline void wrVec(uintptr_t a, const Vec3& v) { HCEGetPlayerState::tryWriteRaw(a, &v, sizeof(v)); }

	uintptr_t resolveObserver(uintptr_t tls) noexcept
	{
		uintptr_t entries = 0;
		if (!rdPtr(tls + kCameraEntriesTls, entries)) return 0;

		uintptr_t cameraEntry = 0;
		for (int slot = 0; slot < 4; ++slot)
		{
			const uintptr_t candidate = entries + (uintptr_t)slot * kCameraEntryStride;
			uintptr_t probe = 0;
			if (rdPtr(candidate, probe)) { cameraEntry = candidate; break; }
		}
		if (!cameraEntry) return 0;

		int32_t index = 0;
		if (!HCEGetPlayerState::tryReadRaw(cameraEntry + kCameraObserverIndex, &index, sizeof(index))) return 0;
		if (index < 0 || index > 3) return 0;

		uintptr_t observers = 0;
		if (!rdPtr(tls + kObserverGlobalsTls, observers)) return 0;
		const uintptr_t observer = observers + (uintptr_t)index * kObserverStride;

		uint32_t lo = 0, hi = 0;
		if (!HCEGetPlayerState::tryReadRaw(observer + kObserverMagicLow, &lo, sizeof(lo))) return 0;
		if (!HCEGetPlayerState::tryReadRaw(observer + kObserverMagicHigh, &hi, sizeof(hi))) return 0;
		if (lo != kObserverMagic || hi != kObserverMagic) return 0;
		return observer;
	}

	// SIM THREAD, once per simulation frame, after the simulation update. noexcept and allocation-free.
	void driftPump(uintptr_t tls) noexcept
	{
		if (!gActive.load(std::memory_order_acquire)) { gHaveState.store(false, std::memory_order_relaxed); return; }

		uintptr_t playerControl = 0;
		uint8_t master = 0;
		if (!rdPtr(tls + kPlayerControlTls, playerControl)) { gHaveState.store(false, std::memory_order_relaxed); return; }
		if (!HCEGetPlayerState::tryReadRaw(playerControl + kFreecamMaster, &master, sizeof(master))) { gHaveState.store(false, std::memory_order_relaxed); return; }
		if (master != 1) { gHaveState.store(false, std::memory_order_relaxed); return; }   // freecam off: re-seed when it returns

		const uintptr_t observer = resolveObserver(tls);
		if (!observer) { gHaveState.store(false, std::memory_order_relaxed); return; }

		Vec3 pos{}, fwd{}, up{};
		if (!rdVec(observer + kPosition, pos) || !rdVec(observer + kForward, fwd) || !rdVec(observer + kUp, up))
		{
			gHaveState.store(false, std::memory_order_relaxed);
			return;
		}

		// ⚠ These nine floats are NOT integrated from what we wrote last frame - the engine rebuilds them from
		// scratch every simulation frame, from the camera-mode authority at cameraEntry+0x20 (see the long
		// comment in HCEFreecamKeepPosition.cpp). So what we just read IS the true, unfiltered camera pose,
		// already free of our previous frame's output. There is no delta to accumulate and nothing to feed back;
		// this filter keeps no "true position" of its own precisely because it does not need one.
		//
		// That also makes this a purely cosmetic filter, which is what a cinematic lag filter should be: the
		// authority keeps flying at full responsiveness with unmodified input, and only the rendered camera
		// trails it. Nothing downstream of the observer can drift out of sync with the real camera.
		Quat trueRot{};
		if (!basisToQuat(fwd, up, trueRot)) { gHaveState.store(false, std::memory_order_relaxed); return; }

		// ---- discontinuity test: has the ENGINE'S pose jumped, independently of how far we are lagging? ------
		const bool hadState = gHaveState.load(std::memory_order_relaxed);
		bool reseed = !hadState;
		float step = 0.0f;
		if (!reseed)
		{
			step = len3(sub3(pos, gPrevTruePos));
			if (step > kJumpFloor + kJumpFactor * gMaxStep) reseed = true;
			else if (std::abs(dotQ(trueRot, gPrevTrueRot)) < kJumpRotDot) reseed = true;
		}

		gPrevTruePos = pos;
		gPrevTrueRot = trueRot;

		if (reseed)
		{
			// First frame, freecam coming back, teleport, revert, level load or a camera cut. Start with
			// everything agreeing so the filter follows the jump immediately and only smooths ordinary motion.
			// Nothing is written: the seeded pose IS the engine's pose, so a write here would be a no-op.
			for (int i = 0; i < kStages; ++i) { gPos[i] = pos; gRot[i] = trueRot; }
			gMaxStep = 0.0f;
			gHaveState.store(true, std::memory_order_relaxed);
			return;
		}

		if (step > gMaxStep) gMaxStep = step;

		const float alpha = stageAlpha(gTau.load(std::memory_order_relaxed), kSimTickSeconds);

		Vec3 p = pos;
		Quat q = trueRot;
		for (int i = 0; i < kStages; ++i)
		{
			gPos[i] = lerpTo3(gPos[i], p, alpha);
			gRot[i] = nlerpTo(gRot[i], q, alpha);
			p = gPos[i];
			q = gRot[i];
		}

		// Once the remaining error is below what a float camera position can express on screen, park exactly on
		// the true pose. During a monotone approach every earlier stage lies between the last stage and the
		// target, so snapping them all is exact.
		if (len3(sub3(pos, gPos[kStages - 1])) < kSnapUnits
			&& std::abs(dotQ(gRot[kStages - 1], trueRot)) > 0.9999999f)
		{
			for (int i = 0; i < kStages; ++i) { gPos[i] = pos; gRot[i] = trueRot; }
		}

		Vec3 outFwd{}, outUp{};
		quatToBasis(gRot[kStages - 1], outFwd, outUp);

		if (!finite3(gPos[kStages - 1]) || !finite3(outFwd) || !finite3(outUp))
		{
			gHaveState.store(false, std::memory_order_relaxed);   // re-seed next tick rather than write garbage
			return;
		}

		wrVec(observer + kPosition, gPos[kStages - 1]);
		wrVec(observer + kForward, outFwd);
		wrVec(observer + kUp, outUp);
	}
}

class HCEFreecamDrift::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::shared_ptr<HCEGetPlayerState> mPlayerState;
	std::shared_ptr<HCEGameThreadPumpHost> mPump;

	bool mRegistered = false;
	std::atomic<bool> mReady{ false };

	void setRegistered(bool wanted)
	{
		if (wanted && !mRegistered)
		{
			if (!HCEGameThreadPump::add(&driftPump))
				throw HCMRuntimeException("Freecam Drift: the simulation-thread pump table is full");
			mRegistered = true;
		}
		else if (!wanted && mRegistered)
		{
			HCEGameThreadPump::remove(&driftPump);
			mRegistered = false;
		}
	}

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(settingsWeak, settings);

			if (newValue)
			{
				setCoastSeconds(settings->hceFreecamDriftAmount->GetValue());
				gHaveState.store(false, std::memory_order_relaxed);   // re-seed on the next frame
				setRegistered(true);
				gActive.store(true, std::memory_order_release);
			}
			else
			{
				gActive.store(false, std::memory_order_release);
				setRegistered(false);
			}

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue ? "Freecam drift on." : "Freecam drift off.");
		}
		catch (HCMRuntimeException ex)
		{
			gActive.store(false, std::memory_order_release);
			try { setRegistered(false); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onAmountChanged(float& newValue) { setCoastSeconds(newValue); }

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	// Declared LAST - ScopedCallbacks subscribe inside their own constructors.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(float&)>> mAmountCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFreecamDriftToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mAmountCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFreecamDriftAmount->valueChangedEvent, [this](float& n) { onAmountChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFreecamDrift only supports Halo Campaign Evolved");

		mPlayerState = resolveDependentCheat(HCEGetPlayerState);
		mPump = resolveDependentCheat(HCEGameThreadPumpHost);
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		gActive.store(false, std::memory_order_release);
		try { setRegistered(false); } catch (...) {}
		mToggleCallback.removeCallback();
		mAmountCallback.removeCallback();
	}
};


HCEFreecamDrift::HCEFreecamDrift(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
HCEFreecamDrift::~HCEFreecamDrift() { PLOG_VERBOSE << "~" << getName(); }
