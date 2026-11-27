#include "pch.h"
#include "MetroMove.h"

// Metro Redux player moves (2026-11-25). Static RE: Documents\Halo Mod And Tools\Metro Redux\binary_survey\agent_move\
// (IDB m.i64; report in the session log). Everything that changes the player runs ON THE GAME'S OWN THREAD, inside two
// inline hooks that stay installed for the life of the process (HCM stays resident; a call may be inside a detour
// whenever we would remove one) and do nothing unless HCM has work queued:
//
//   0x1E1850  player/actor update(this, int dt_ms, matrix*): before the player's update runs, queued teleports and
//             checkpoint restores are applied through the engine's own set_transform (player vtable +0xC18 = 0x2724B0,
//             the same call its own savepos/restore script actions make: it moves the transform AND the physics shell
//             and character controller together - writing the transform alone is undone next frame, the shell keeps its
//             own position).
//   0x79FAB0  shell move(shell, float disp[3] m/s, float, float, ...): the character controller's per-frame move along
//             a displacement in m/s (the engine multiplies by dt). Force Launch adds HCM's own launch velocity here, so
//             the launch goes through the controller's collision (walls stop it, it lands on floors). While a launch is
//             in the air the engine's own fall speed (shell+500, m/s, + = down) is held at 0 and HCM integrates gravity
//             (9.81 m/s^2, the engine's own figure) itself; on landing the engine takes over again.
namespace metro::move
{
namespace
{
	constexpr uintptr_t kUpdateRVA = 0x1E1850;
	constexpr uintptr_t kShellMoveRVA = 0x79FAB0;
	constexpr uintptr_t kSetTransformRVA = 0x2724B0;
	constexpr uintptr_t kSetTransformSlot = 0xC18;
	constexpr uint8_t kUpdatePrologue[] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x08, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF8, 0x01, 0x00, 0x00, 0x44, 0x8B, 0xF2 };
	constexpr uint8_t kShellMovePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xF2 };

	constexpr uintptr_t kPlayerMoverOff = 0x4F8;     // mover*
	constexpr uintptr_t kMoverShellOff = 8;          // physics shell*
	constexpr uintptr_t kShellCtrlOff = 200;         // PhysX character controller*
	constexpr uintptr_t kShellFootOff = 424;         // float3 foot position cache
	constexpr uintptr_t kShellFallSpeedOff = 500;    // float m/s, + = down
	constexpr uintptr_t kShellAirTimeOff = 504;
	constexpr uintptr_t kShellFreeAirVelOff = 448;   // vec3 m/s, the shell's own velocity in its free-air modes 2/3
	constexpr uintptr_t kCtrlPosOff = 232;           // capsule centre (float3 or PhysX big-world double3 - detected)
	constexpr uintptr_t kPlayerAnglesOff = 1616;     // _head/_body/_body_unaffected/_move angles, 4 x vec3
	constexpr uintptr_t kPlayerLookOff = 0x1244;     // unit look vector (probe)
	constexpr uintptr_t kMoverVelocityOff = 0x66C;   // vec3 m/s (probe; derived by the engine, read-only use)
	constexpr float kGravity = 9.81f;
	constexpr float kFreeAirGravity = 19.62f;   // what the shell's free-air move takes off its own vertical velocity per second

	using update_t = uintptr_t(__fastcall*)(uintptr_t self, int dtMs, void* mat, uintptr_t r9);
	using shellmove_t = uintptr_t(__fastcall*)(uintptr_t shell, float* disp, float a3, float a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10);
	using settransform_t = void(__fastcall*)(uintptr_t ent, const float* m, const float* off, const uint8_t* flags, void** out);

	// 2026-11-25 (user: launch "only works going up"; log: asked 0.36 m sideways in a frame, moved 0.02): the ground move
	// (0x79D400) passes the displacement through the player's vtbl +1568 = 0x1EA740(player, float* disp), which scales the
	// HORIZONTAL part down to the run speed. Hooked: HCM's own horizontal launch velocity is taken out before it and put back
	// after, so only the walking input is clamped. Vertical is never clamped.
	constexpr uintptr_t kSpeedClampRVA = 0x1EA740;
	constexpr uint8_t kSpeedClampPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 };
	using speedclamp_t = void(__fastcall*)(uintptr_t player, float* disp);
	std::atomic<speedclamp_t> sOrigSpeedClamp{ nullptr };
	// the horizontal launch velocity added to the displacement of the player move in progress (game thread only)
	thread_local bool tInPlayerMove = false;
	thread_local float tAddX = 0.f, tAddZ = 0.f;

	// 2026-11-25 (user: "sometimes after i fall a bit, acro wont let me ascend anymore"; live probe: shell mode 3, vertical
	// velocity exactly 0, height constant - resting on a floor while still in the free-air mode). The free-air move zeroes an
	// UPWARD velocity whenever 0x793A70 (a PxScene overlap query with the stance capsule) reports contact - and resting on
	// the floor always does. While HCM itself drives the player upward in free air, that probe answers "clear" for the
	// player's shell only; the controller's real move still collides, so ceilings still stop you.
	constexpr uintptr_t kUpProbeRVA = 0x793A70;
	constexpr uint8_t kUpProbePrologue[] = { 0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xE8, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xE8, 0x0F, 0x28, 0xE3 };
	using upprobe_t = uintptr_t(__fastcall*)(uintptr_t shell, uintptr_t stance, float a3, float a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8);
	std::atomic<upprobe_t> sOrigUpProbe{ nullptr };
	thread_local uintptr_t tIgnoreUpProbeShell = 0;

	std::atomic<update_t> sOrigUpdate{ nullptr };
	std::atomic<shellmove_t> sOrigShellMove{ nullptr };
	std::mutex sInitMutex;
	bool sInitDone = false;
	std::string sInitError;

	// queued work (UI thread -> game thread)
	struct Pending
	{
		bool teleport = false; Vec3 to;
		bool restore = false; Snapshot snap;
		std::chrono::steady_clock::time_point when;
	};
	std::mutex sQueueMutex;
	Pending sPending;
	std::atomic<bool> sHasPending{ false };

	// launch state (game thread only, except start under sQueueMutex)
	struct Launch
	{
		bool active = false; bool airborne = false; Vec3 v; float age = 0; int landFrames = 0, ceilFrames = 0, wallFrames = 0;
		bool havePrev = false; Vec3 prevPos, prevD;   // last frame's position and the displacement we asked for (m)
	};
	Launch sLaunch;
	// a launch / Acrophobia flight while the shell is in its free-air mode: HCM's horizontal velocity (game thread only)
	struct AirLaunch { bool active = false; float hx = 0, hz = 0, lastX = 0, lastZ = 0, lastY = 0, age = 0; int wallFrames = 0; };
	AirLaunch sAir;
	bool sLaunchStartPending = false; Vec3 sLaunchAdd; bool sLaunchReplace = false;
	std::atomic<bool> sLaunchWork{ false };

	// Acrophobia (render thread -> game thread)
	std::atomic<bool> sAcroOn{ false }, sAcroJump{ false }, sAcroCrouch{ false };
	// the Halo 1 / CER flight model (H5Acrophobia.h has the derivation), in wu/s; Halo ran it at 30 ticks a second
	constexpr float kAcroScale = 0.0625f, kAcroAccel = 9.f, kAcroAccel2 = 18.f, kAcroDamping = 0.2f, kAcroIdle = 0.2f, kHaloTicks = 30.f;

	bool playerDead(uintptr_t pl) { float h = 0.f; return !read(pl + kPlayerHealthOff, h) || !(h > 0.f); }

	std::atomic<float> sLastDt{ 1.f / 60.f };
	int sCtrlPosKind = 0;   // 0 unknown, 1 float3, 2 double3, -1 neither (use the foot cache)

	uintptr_t playerShell(uintptr_t pl)
	{
		const uintptr_t mover = readPtr(pl + kPlayerMoverOff);
		return mover ? readPtr(mover + kMoverShellOff) : 0;
	}

	bool sehSetTransform(settransform_t fn, uintptr_t ent, const float* m)
	{
		const float off[3] = { 0, 0, 0 };
		const uint8_t flags = 0;
		void* out[2] = { nullptr, nullptr };
		__try { fn(ent, m, off, &flags, out); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// the engine's set_transform for this entity, only if its vtable slot is the function the RE was done on
	settransform_t setTransformFor(uintptr_t pl)
	{
		const uintptr_t vt = readPtr(pl);
		const uintptr_t fn = vt ? readPtr(vt + kSetTransformSlot) : 0;
		return fn == base() + kSetTransformRVA ? (settransform_t)fn : nullptr;
	}

	bool applyTransform(uintptr_t pl, const float m[16])
	{
		auto fn = setTransformFor(pl);
		if (!fn) { PLOG_ERROR << "Metro move: the player's set_transform slot is not the expected function - not moving"; return false; }
		if (!sehSetTransform(fn, pl, m)) { PLOG_ERROR << "Metro move: set_transform faulted"; return false; }
		return true;
	}

	void doPending(uintptr_t pl)
	{
		Pending p;
		{
			std::scoped_lock lk(sQueueMutex);
			p = sPending; sPending = {}; sHasPending = false;
		}
		if (std::chrono::steady_clock::now() - p.when > std::chrono::seconds(3)) return;   // stale (game was paused / loading)
		const uintptr_t shell = playerShell(pl);
		// 2026-11-25 crash: a revert while DEAD gave the body its health back; the weapon tick then took the "owner alive"
		// path (0x2F1280, player vtbl +776 = health > 0) with its alive-only state already gone (+0x890 null) -> AV. Never
		// move or revive a dead player (MetroCheckpoint falls back to the game's own load).
		if (playerDead(pl)) return;
		if (p.restore && p.snap.valid && p.snap.player == pl)
		{
			if (applyTransform(pl, p.snap.transform.data()))
			{
				write(pl + kPlayerAnglesOff, p.snap.angles);    // set_transform re-derives them only for moves > 0.03 m
				write(pl + kPlayerHealthOff, p.snap.health);
				write(pl + kPlayerStaminaOff, p.snap.stamina);
				if (shell) { write(shell + kShellFallSpeedOff, p.snap.fallSpeed); write(shell + kShellAirTimeOff, p.snap.airTime); write(shell + kShellFreeAirVelOff, p.snap.airVel); }
				sLaunch = {}; sAir = {};
				if (p.snap.launchActive) { sLaunch.active = true; sLaunch.airborne = p.snap.launchAirborne; sLaunch.v = p.snap.launchV; }
			}
		}
		if (p.teleport)
		{
			std::array<float, 16> m{};
			std::array<float, 12> angles{};
			const bool haveAngles = read(pl + kPlayerAnglesOff, angles);
			if (read(pl + kPlayerTransformOff, m))
			{
				m[12] = p.to.x; m[13] = p.to.y; m[14] = p.to.z;
				if (applyTransform(pl, m.data()))
				{
					// user: teleport "resets my look direction" - set_transform re-derives the view angles from the matrix
					// (yaw only) for any move over 0.03 m; put the ones from before back
					if (haveAngles) write(pl + kPlayerAnglesOff, angles);
					if (shell) { write(shell + kShellFallSpeedOff, 0.f); write(shell + kShellAirTimeOff, 0.f); write(shell + kShellFreeAirVelOff, Vec3{}); }
					sAir = {};
				}
			}
		}
	}

	uintptr_t __fastcall updateDetour(uintptr_t self, int dtMs, void* mat, uintptr_t r9)
	{
		auto orig = sOrigUpdate.load(std::memory_order_acquire);
		if (dtMs > 0 && (sHasPending.load(std::memory_order_acquire) || sLaunchWork.load(std::memory_order_relaxed)))
		{
			const uintptr_t pl = player();
			if (pl && self == pl)
			{
				sLastDt.store(std::clamp(dtMs * 0.001f, 0.f, 0.2f), std::memory_order_relaxed);
				if (sHasPending.load(std::memory_order_acquire)) doPending(pl);
			}
		}
		else if (dtMs > 0 && self == player())
			sLastDt.store(std::clamp(dtMs * 0.001f, 0.f, 0.2f), std::memory_order_relaxed);
		return orig(self, dtMs, mat, r9);
	}

	// where the controller is: its capsule centre (float3 or double3, whichever agrees with the transform), else the foot cache
	bool ctrlPosition(uintptr_t shell, uintptr_t pl, Vec3& out)
	{
		const uintptr_t ctrl = readPtr(shell + kShellCtrlOff);
		if (sCtrlPosKind == 0 && ctrl)
		{
			const auto pos = playerPosition();
			Vec3 f; double d[3];
			auto isNear = [&](float x, float y, float z) { return pos && std::abs(x - pos->x) < 2.5f && std::abs(y - pos->y) < 2.5f && std::abs(z - pos->z) < 2.5f; };
			if (read(ctrl + kCtrlPosOff, f) && isNear(f.x, f.y, f.z)) sCtrlPosKind = 1;
			else if (read(ctrl + kCtrlPosOff, d) && isNear((float)d[0], (float)d[1], (float)d[2])) sCtrlPosKind = 2;
			else sCtrlPosKind = -1;
			PLOG_INFO << "Metro move: controller position is " << (sCtrlPosKind == 1 ? "float3" : sCtrlPosKind == 2 ? "double3" : "not recognised (using the shell's foot cache)");
		}
		if (sCtrlPosKind == 1 && ctrl) return read(ctrl + kCtrlPosOff, out);
		if (sCtrlPosKind == 2 && ctrl) { double d[3]; if (!read(ctrl + kCtrlPosOff, d)) return false; out = { (float)d[0], (float)d[1], (float)d[2] }; return true; }
		return read(shell + kShellFootOff, out);
	}

	// one Acrophobia step on a velocity (m/s, engine axes): jump = thrust along the look, else hover (toward rest)
	void acroStep(uintptr_t pl, Vec3& v, float dt, bool jump)
	{
		const float ticks = dt * kHaloTicks;   // the model's per-tick factors, made frame-rate independent
		if (!jump) { const float k = std::pow(kAcroIdle, ticks); v.x *= k; v.y *= k; v.z *= k; return; }
		Vec3 b{};
		if (!read(pl + kPlayerLookOff, b)) return;
		const float bl = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
		if (!std::isfinite(bl) || bl < 0.5f) return;
		b = { b.x / bl, b.y / bl, b.z / bl };
		const float tm = std::max(0.f, b.x * v.x + b.y * v.y + b.z * v.z);
		const float s = std::clamp(tm / kMetresPerHaloUnit * kAcroScale, 0.f, 1.f);
		const float a = (kAcroAccel + (s * kAcroAccel - s * s * kAcroAccel2)) * kMetresPerHaloUnit;
		const float damp = 1.f - std::pow(1.f - kAcroDamping, ticks);
		v.x -= (v.x - b.x * tm) * damp; v.y -= (v.y - b.y * tm) * damp; v.z -= (v.z - b.z * tm) * damp;
		v.x += b.x * a * dt; v.y += b.y * a * dt; v.z += b.z * a * dt;
	}

	// 2026-11-25 (user: "theres a seperate airborne flag ... launches and acro do not work on it ... im falling"; live probe:
	// shell+152 mode nibble = 2, shell velocity +448/+452/+456 = (154, -407.7, 77) m/s): in the shell's FREE-AIR modes 2/3
	// the ground move 0x79D400 ignores the displacement's vertical and integrates its OWN velocity at shell+448 (horizontal
	// += disp * k * dt, vertical -= 19.62 * dt) - so launch / Acrophobia act on that velocity directly there.
	constexpr uintptr_t kShellFlagsOff = 152, kShellAirVelOff = 448;
	bool shellFreeAir(uintptr_t shell, uint32_t* modeOut = nullptr)
	{
		uint32_t f = 0;
		if (!read(shell + kShellFlagsOff, f)) return false;
		const uint32_t mode = (f >> 8) & 0xF;
		if (modeOut) *modeOut = mode;
		return mode == 2 || mode == 3;
	}

	uintptr_t __fastcall shellMoveDetour(uintptr_t shell, float* disp, float a3, float a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
	{
		auto orig = sOrigShellMove.load(std::memory_order_acquire);
		const bool acroOn = sAcroOn.load(std::memory_order_relaxed);
		if ((!sLaunchWork.load(std::memory_order_relaxed) && !acroOn) || !disp) return orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10);
		const uintptr_t pl = player();
		if (!pl || shell != playerShell(pl)) return orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10);
		if (playerDead(pl)) { sLaunch = {}; sAir = {}; sLaunchStartPending = false; return orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10); }

		// Acrophobia: hold jump to fly along the look direction, hold crouch to hover; it drives the same launch velocity
		const bool acroJump = acroOn && sAcroJump.load(std::memory_order_relaxed);
		const bool acroFly = acroOn && (acroJump || sAcroCrouch.load(std::memory_order_relaxed));

		uint32_t shellMode = 0;
		if (shellFreeAir(shell, &shellMode))
		{
			// 2026-11-25 (user: "when im 'in air' the horizontal speed fix doesnt work, and its clamped still"): after every
			// physics step 0x79DEB0 rewrites shell+448 from the distance really moved, and the free-air move adds the walking
			// input as an acceleration (no keys held = a brake), so a sideways launch died out in a second or two. While a
			// launch / Acrophobia is going, HCM owns the HORIZONTAL velocity here (sAir.h) and writes it back every frame; the
			// vertical stays the engine's (gravity, ceilings, landing). A wall: really moving < 30% of what we set, 3 frames.
			Vec3 v{};
			if (read(shell + kShellAirVelOff, v) && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))
			{
				const float dt = sLastDt.load(std::memory_order_relaxed);
				if (sLaunch.active)   // a launch from the ground mode carries over: its vertical replaces, its sideways is ours
				{
					if (sLaunch.airborne) v.y = sLaunch.v.y;
					sAir.active = true; sAir.hx = v.x + sLaunch.v.x; sAir.hz = v.z + sLaunch.v.z; sAir.age = 0; sAir.wallFrames = 0;
					sLaunch = {};
				}
				{
					std::scoped_lock lk(sQueueMutex);
					if (sLaunchStartPending)
					{
						sLaunchStartPending = false;
						if (sLaunchReplace) v = sLaunchAdd; else { v.x += sLaunchAdd.x; v.y += sLaunchAdd.y; v.z += sLaunchAdd.z; }
						sAir.active = true; sAir.hx = v.x; sAir.hz = v.z; sAir.age = 0; sAir.wallFrames = 0;
						PLOG_INFO << std::format("Metro launch (free-air mode {}): velocity now {:.2f} {:.2f} {:.2f} m/s", shellMode, v.x, v.y, v.z);
					}
				}
				if (acroFly && !sAir.active) { sAir.active = true; sAir.hx = v.x; sAir.hz = v.z; sAir.age = 0; sAir.wallFrames = 0; }
				if (sAir.active)
				{
					sAir.age += dt;
					// wall check against what we set last frame (v is what the engine measured since)
					const float h2 = sAir.lastX * sAir.lastX + sAir.lastZ * sAir.lastZ;
					if (sAir.age > 0.1f && h2 > 1.f)
					{
						sAir.wallFrames = ((v.x * sAir.lastX + v.z * sAir.lastZ) / h2 < 0.3f) ? sAir.wallFrames + 1 : 0;
						if (sAir.wallFrames >= 3)
						{
							PLOG_INFO << std::format("Metro launch (free-air): wall at {:.2f} s (set {:.2f} {:.2f}, moved {:.2f} {:.2f} m/s)", sAir.age, sAir.lastX, sAir.lastZ, v.x, v.z);
							sAir.hx = v.x; sAir.hz = v.z; sAir.wallFrames = 0;
							if (!acroFly) sAir.active = false;
						}
					}
					if (sAir.active)
					{
						v.x = sAir.hx; v.z = sAir.hz;
						if (acroFly) { acroStep(pl, v, dt, acroJump); sAir.hx = v.x; sAir.hz = v.z; }
						// 2026-11-25 (user: "when i freeze in the air with acro (crouch) i slowly sink down ... make sure it fully
						// stops you"): the hover only damped the velocity while the engine kept adding gravity, settling into a
						// steady sink. Hovering cancels this frame's gravity in advance, so the damping brings you to rest.
						if (acroFly && !acroJump) v.y += kFreeAirGravity * dt;
						write(shell + kShellAirVelOff, v);
						sAir.lastX = v.x; sAir.lastZ = v.z; sAir.lastY = v.y;
					}
					if (sAir.age > 60.f && !acroFly) sAir.active = false;
				}
			}
			if (!acroOn && !sAir.active) sLaunchWork = false;
			// 2026-11-25 (log: set -26 / -40 m/s, moved -1.16 / -1.78 - 4.4% every frame): the free-air move hands the shell
			// velocity to the same run-speed clamp (player vtbl +1568) as the ground move. While HCM owns the horizontal, the
			// whole horizontal is taken out of the clamp and put back.
			if (sAir.active) { tInPlayerMove = true; tAddX = sAir.lastX; tAddZ = sAir.lastZ; }
			if (sAir.active && sAir.lastY > 0.f) tIgnoreUpProbeShell = shell;
			const uintptr_t r = orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10);
			tInPlayerMove = false; tAddX = tAddZ = 0.f; tIgnoreUpProbeShell = 0;
			return r;
		}
		// back in the ground mode with a free-air launch still going: hand its sideways speed to the ground slide
		if (sAir.active)
		{
			sLaunch = {}; sLaunch.active = true; sLaunch.airborne = false; sLaunch.v = { sAir.hx, 0.f, sAir.hz }; sLaunch.age = 1.f;
			sAir = {};
		}
		if (acroFly && !sLaunch.active)
		{
			float fall = 0.f; read(shell + kShellFallSpeedOff, fall);
			sLaunch = {}; sLaunch.active = true; sLaunch.airborne = true; sLaunch.v = { 0.f, std::isfinite(fall) ? -fall : 0.f, 0.f };
		}

		{
			std::scoped_lock lk(sQueueMutex);
			if (sLaunchStartPending)
			{
				sLaunchStartPending = false;
				if (sLaunchReplace || !sLaunch.active)
				{
					// carry the engine's own current fall speed into ours, unless replacing
					float fall = 0.f; read(shell + kShellFallSpeedOff, fall);
					sLaunch.v = sLaunchReplace ? Vec3{} : Vec3{ 0.f, std::isfinite(fall) ? -fall : 0.f, 0.f };
				}
				sLaunch.v.x += sLaunchAdd.x; sLaunch.v.y += sLaunchAdd.y; sLaunch.v.z += sLaunchAdd.z;
				sLaunch.active = true; sLaunch.airborne = true; sLaunch.age = 0;
				sLaunch.landFrames = sLaunch.ceilFrames = sLaunch.wallFrames = 0;
			}
		}
		if (!sLaunch.active) { if (!acroOn) sLaunchWork = false; return orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10); }

		const float dt = sLastDt.load(std::memory_order_relaxed);
		sLaunch.age += dt;
		if (acroFly) { sLaunch.airborne = true; sLaunch.landFrames = 0; if (sLaunch.age > 30.f) sLaunch.age = 1.f; }   // no timeout while flying
		if (sLaunch.airborne)
		{
			write(shell + kShellFallSpeedOff, 0.f);   // ours holds the vertical while airborne
			if (!(acroFly && !acroJump)) sLaunch.v.y -= kGravity * dt;   // no gravity while hovering (Acrophobia crouch): a full stop
		}
		if (acroFly)
		{
			const float ticks = dt * kHaloTicks;   // the model's per-tick factors, made frame-rate independent
			if (acroJump)
			{
				Vec3 b{};
				if (read(pl + kPlayerLookOff, b))
				{
					const float bl = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
					if (std::isfinite(bl) && bl > 0.5f)
					{
						b = { b.x / bl, b.y / bl, b.z / bl };
						Vec3& v = sLaunch.v;
						const float tm = std::max(0.f, b.x * v.x + b.y * v.y + b.z * v.z);          // m/s along the aim
						const float s = std::clamp(tm / kMetresPerHaloUnit * kAcroScale, 0.f, 1.f);
						const float a = (kAcroAccel + (s * kAcroAccel - s * s * kAcroAccel2)) * kMetresPerHaloUnit;   // m/s^2
						const float damp = 1.f - std::pow(1.f - kAcroDamping, ticks);
						v.x -= (v.x - b.x * tm) * damp; v.y -= (v.y - b.y * tm) * damp; v.z -= (v.z - b.z * tm) * damp;
						v.x += b.x * a * dt; v.y += b.y * a * dt; v.z += b.z * a * dt;
					}
				}
			}
			else
			{
				const float k = std::pow(kAcroIdle, ticks);   // hover: velocity toward rest
				sLaunch.v.x *= k; sLaunch.v.y *= k; sLaunch.v.z *= k;
			}
		}
		const float d[3] = { disp[0] + sLaunch.v.x, disp[1] + (sLaunch.airborne ? sLaunch.v.y : 0.f), disp[2] + sLaunch.v.z };
		const float saved[3] = { disp[0], disp[1], disp[2] };
		disp[0] = d[0]; disp[1] = d[1]; disp[2] = d[2];
		// collisions: how far the player REALLY moved since the last frame (transform position, which the update writes from
		// the controller's result) vs how far we asked last frame. 2026-11-25 (user: launch and acro "randomly stop working"):
		// measuring the controller across this call read 0 whenever its move is applied later in the physics step, which
		// looked like a ceiling / wall and cancelled the launch. Frame to frame there is no such race. Debounced (3 frames).
		const auto posNow = playerPosition();
		tInPlayerMove = true; tAddX = sLaunch.v.x; tAddZ = sLaunch.v.z;
		const uintptr_t r = orig(shell, disp, a3, a4, a5, a6, a7, a8, a9, a10);
		tInPlayerMove = false; tAddX = tAddZ = 0.f;
		disp[0] = saved[0]; disp[1] = saved[1]; disp[2] = saved[2];
		if (posNow && sLaunch.havePrev && sLaunch.age > 0.1f)
		{
			const float ex = sLaunch.prevD.x, ey = sLaunch.prevD.y, ez = sLaunch.prevD.z;   // asked last frame (m)
			const float ax = posNow->x - sLaunch.prevPos.x, ay = posNow->y - sLaunch.prevPos.y, az = posNow->z - sLaunch.prevPos.z;
			constexpr int kFrames = 3;
			auto logCancel = [&](const char* what) { PLOG_INFO << std::format("Metro launch: {} at {:.2f} s (asked {:.3f} {:.3f} {:.3f} m, moved {:.3f} {:.3f} {:.3f} m)", what, sLaunch.age, ex, ey, ez, ax, ay, az); };
			if (sLaunch.airborne && !acroFly)
			{
				sLaunch.landFrames = (sLaunch.v.y < 0.f && ey < -0.001f && ay > ey * 0.5f) ? sLaunch.landFrames + 1 : 0;
				if (sLaunch.landFrames >= kFrames) { logCancel("landed"); sLaunch.airborne = false; sLaunch.v.y = 0.f; }
			}
			sLaunch.ceilFrames = (sLaunch.airborne && sLaunch.v.y > 0.5f && ey > 0.005f && ay < ey * 0.25f) ? sLaunch.ceilFrames + 1 : 0;
			if (sLaunch.ceilFrames >= kFrames) { logCancel("ceiling"); sLaunch.v.y = 0.f; sLaunch.ceilFrames = 0; }
			const float eh2 = ex * ex + ez * ez;
			sLaunch.wallFrames = (eh2 > 1e-4f && (ax * ex + az * ez) / eh2 < 0.2f) ? sLaunch.wallFrames + 1 : 0;
			if (sLaunch.wallFrames >= kFrames) { logCancel("wall"); sLaunch.v.x = 0.f; sLaunch.v.z = 0.f; sLaunch.wallFrames = 0; }
		}
		if (posNow) { sLaunch.prevPos = *posNow; sLaunch.prevD = { d[0] * dt, d[1] * dt, d[2] * dt }; sLaunch.havePrev = true; }
		if (!sLaunch.airborne)
		{
			const float k = std::exp(-3.f * dt);   // ground friction once landed: a sideways launch along the floor slides
			sLaunch.v.x *= k; sLaunch.v.z *= k;
			if (sLaunch.v.x * sLaunch.v.x + sLaunch.v.z * sLaunch.v.z < 0.09f) sLaunch = {};
		}
		if (sLaunch.age > 60.f) sLaunch = {};
		if (!sLaunch.active && !acroOn) sLaunchWork = false;
		return r;
	}

	uintptr_t __fastcall upProbeDetour(uintptr_t shell, uintptr_t stance, float a3, float a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8)
	{
		if (tIgnoreUpProbeShell && shell == tIgnoreUpProbeShell) return 0;   // "no contact": keep HCM's upward velocity
		return sOrigUpProbe.load(std::memory_order_acquire)(shell, stance, a3, a4, a5, a6, a7, a8);
	}

	// the run-speed clamp: clamp only the walking part of the displacement, keep HCM's launch on top
	void __fastcall speedClampDetour(uintptr_t self, float* disp)
	{
		auto orig = sOrigSpeedClamp.load(std::memory_order_acquire);
		if (!tInPlayerMove || !disp || (tAddX == 0.f && tAddZ == 0.f)) { orig(self, disp); return; }
		disp[0] -= tAddX; disp[2] -= tAddZ;
		orig(self, disp);
		disp[0] += tAddX; disp[2] += tAddZ;
	}

	bool prologueMatches(uintptr_t at, const uint8_t* bytes, size_t n)
	{
		std::vector<uint8_t> got(n);
		return detail::sehRead(got.data(), (const void*)at, n) && memcmp(got.data(), bytes, n) == 0;
	}

	std::string ensureHooks()
	{
		std::scoped_lock lk(sInitMutex);
		if (sInitDone) return sInitError;
		sInitDone = true;
		const uintptr_t upd = base() + kUpdateRVA, mv = base() + kShellMoveRVA, clampAt = base() + kSpeedClampRVA;
		if (!prologueMatches(upd, kUpdatePrologue, sizeof(kUpdatePrologue)) || !prologueMatches(mv, kShellMovePrologue, sizeof(kShellMovePrologue))
			|| !prologueMatches(clampAt, kSpeedClampPrologue, sizeof(kSpeedClampPrologue))
			|| !prologueMatches(base() + kUpProbeRVA, kUpProbePrologue, sizeof(kUpProbePrologue)))
		{
			sInitError = "this metro.exe build does not match the one HCM was made for";
			PLOG_ERROR << "Metro move: " << sInitError;
			return sInitError;
		}
		// never freed: HCM stays resident and the game thread may be inside a detour at any time
		static auto* hooks = new std::vector<safetyhook::InlineHook>();
		auto h1 = safetyhook::InlineHook::create((void*)upd, (void*)&updateDetour, safetyhook::InlineHook::StartDisabled);
		auto h2 = safetyhook::InlineHook::create((void*)mv, (void*)&shellMoveDetour, safetyhook::InlineHook::StartDisabled);
		auto h3 = safetyhook::InlineHook::create((void*)clampAt, (void*)&speedClampDetour, safetyhook::InlineHook::StartDisabled);
		auto h4 = safetyhook::InlineHook::create((void*)(base() + kUpProbeRVA), (void*)&upProbeDetour, safetyhook::InlineHook::StartDisabled);
		if (!h1 || !h2 || !h3 || !h4) { sInitError = "could not hook the game's player update"; PLOG_ERROR << "Metro move: " << sInitError; return sInitError; }
		sOrigUpdate.store(h1->original<update_t>(), std::memory_order_release);
		sOrigShellMove.store(h2->original<shellmove_t>(), std::memory_order_release);
		sOrigSpeedClamp.store(h3->original<speedclamp_t>(), std::memory_order_release);
		sOrigUpProbe.store(h4->original<upprobe_t>(), std::memory_order_release);
		if (!h4->enable() || !h3->enable() || !h1->enable() || !h2->enable()) { sInitError = "could not enable the player update hooks"; PLOG_ERROR << "Metro move: " << sInitError; return sInitError; }
		hooks->push_back(std::move(*h1));
		hooks->push_back(std::move(*h2));
		hooks->push_back(std::move(*h3));
		hooks->push_back(std::move(*h4));
		PLOG_INFO << "Metro move: player update + shell move + speed clamp + up probe hooked";
		return {};
	}

	void queue(const std::function<void(Pending&)>& f)
	{
		std::scoped_lock lk(sQueueMutex);
		f(sPending);
		sPending.when = std::chrono::steady_clock::now();
		sHasPending.store(true, std::memory_order_release);
	}
}

void init() { (void)ensureHooks(); }

std::optional<Vec3> velocity()
{
	const uintptr_t pl = player();
	const uintptr_t mover = pl ? readPtr(pl + kPlayerMoverOff) : 0;
	Vec3 v;
	if (!mover || !read(mover + kMoverVelocityOff, v) || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return std::nullopt;
	return v;
}

std::optional<Vec3> lookDirectionHalo()
{
	const uintptr_t pl = player();
	Vec3 v;
	if (!pl || !read(pl + kPlayerLookOff, v)) return std::nullopt;
	const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (!std::isfinite(len) || std::abs(len - 1.f) > 0.05f)
	{
		// fall back to the transform's forward row (yaw only)
		std::array<float, 16> m{};
		if (!read(pl + kPlayerTransformOff, m)) return std::nullopt;
		v = { m[8], m[9], m[10] };
	}
	return Vec3{ v.x, v.z, v.y };   // engine (x, y up, z) -> Halo (x, y, z up); unit length, no scaling
}

Vec3 lookRelative(const Vec3& look, const SimpleMath::Vector3& fru, bool ignoreVertical)
{
	Vec3 f = look;
	if (ignoreVertical) f.z = 0.f;
	float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
	if (fl < 1e-4f) { f = { 1, 0, 0 }; fl = 1; }
	f = { f.x / fl, f.y / fl, f.z / fl };
	// right = forward x up(0,0,1); up = right x forward (Halo axes, Z up)
	Vec3 r{ f.y, -f.x, 0.f };
	float rl = std::sqrt(r.x * r.x + r.y * r.y);
	if (rl < 1e-4f) { r = { 0, -1, 0 }; rl = 1; }
	r = { r.x / rl, r.y / rl, 0.f };
	const Vec3 u{ r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
	return { f.x * fru.x + r.x * fru.y + u.x * fru.z, f.y * fru.x + r.y * fru.y + u.y * fru.z, f.z * fru.x + r.z * fru.y + u.z * fru.z };
}

std::string teleportTo(const Vec3& m)
{
	if (auto e = ensureHooks(); !e.empty()) return e;
	if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.z)) return "not a position";
	const uintptr_t pl = player();
	if (!pl) return "no player";
	if (!setTransformFor(pl)) return "the player object does not look as expected";
	queue([&](Pending& p) { p.teleport = true; p.to = m; });
	return {};
}

std::string launch(const Vec3& mps, bool replace)
{
	if (auto e = ensureHooks(); !e.empty()) return e;
	if (!std::isfinite(mps.x) || !std::isfinite(mps.y) || !std::isfinite(mps.z)) return "not a velocity";
	if (!player()) return "no player";
	{
		std::scoped_lock lk(sQueueMutex);
		sLaunchStartPending = true; sLaunchAdd = mps; sLaunchReplace = replace;
	}
	sLaunchWork.store(true, std::memory_order_release);
	return {};
}

std::string take(Snapshot& s)
{
	if (auto e = ensureHooks(); !e.empty()) return e;
	const uintptr_t pl = player();
	if (!pl) return "no player";
	if (playerDead(pl)) return kDeadReason;
	Snapshot n;
	if (!read(pl + kPlayerTransformOff, n.transform) || !read(pl + kPlayerAnglesOff, n.angles)
		|| !read(pl + kPlayerHealthOff, n.health) || !read(pl + kPlayerStaminaOff, n.stamina)) return "could not read the player";
	for (float f : n.transform) if (!std::isfinite(f)) return "the player's transform is not valid right now";
	if (const uintptr_t shell = playerShell(pl)) { read(shell + kShellFallSpeedOff, n.fallSpeed); read(shell + kShellAirTimeOff, n.airTime); read(shell + kShellFreeAirVelOff, n.airVel); }
	{
		std::scoped_lock lk(sQueueMutex);   // the launch state is the game thread's; a torn read here only affects momentum
		n.launchActive = sLaunch.active; n.launchAirborne = sLaunch.airborne; n.launchV = sLaunch.v;
	}
	n.player = pl;
	n.level = level();
	n.valid = true;
	s = n;
	return {};
}

std::string restore(const Snapshot& s)
{
	if (auto e = ensureHooks(); !e.empty()) return e;
	if (!s.valid) return "no checkpoint saved yet";
	const uintptr_t pl = player();
	if (!pl) return "no player";
	if (playerDead(pl)) return kDeadReason;
	if (pl != s.player || level() != s.level) return "the checkpoint is from before the last level load";
	if (!setTransformFor(pl)) return "the player object does not look as expected";
	queue([&](Pending& p) { p.restore = true; p.snap = s; });
	if (s.launchActive) sLaunchWork.store(true, std::memory_order_release);
	return {};
}

bool isPlayerDead() { const uintptr_t pl = player(); return !pl || playerDead(pl); }

void setAcrophobia(bool on)
{
	if (on) (void)ensureHooks();
	sAcroOn.store(on, std::memory_order_release);
	if (on) sLaunchWork.store(true, std::memory_order_release);
	else { sAcroJump = false; sAcroCrouch = false; }
}

void setAcrophobiaInput(bool jump, bool crouch) { sAcroJump.store(jump, std::memory_order_relaxed); sAcroCrouch.store(crouch, std::memory_order_relaxed); }
}
