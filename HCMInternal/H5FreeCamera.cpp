#include "pch.h"
#include "H5FreeCamera.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "GlobalKill.h"
#include "imgui.h"
#include <TlHelp32.h>

// See H5FreeCamera.h for the chain, the write order and why none of this needs an engine call.

namespace
{
	constexpr uintptr_t kRvaTlsIndex      = 0x05F1D56C;
	constexpr uintptr_t kRvaFlyingVtable  = 0x0333CEA0;
	constexpr uintptr_t kRvaFirstPersonVt = 0x0333CD20;
	constexpr uintptr_t kRvaSweepRadius   = 0x0590E6F0;   // default collision sweep radius (0.03334)
	constexpr uintptr_t kRvaFovDegrees    = 0x0590E210;
	constexpr uintptr_t kRvaFlySpeedScale = 0x046EC5E0;   // engine's native flycam speed multiplier

	constexpr uintptr_t kTlsGate      = 0x0024;
	constexpr uintptr_t kTlsPlayerGlo = 0x1560;
	constexpr uintptr_t kTlsObjectGlo = 0x4B68;
	constexpr uintptr_t kTlsDirectors = 0x0198;
	constexpr uintptr_t kTlsObservers = 0x01B0;
	constexpr uintptr_t kTlsNoclipPred = 0x15A0;

	constexpr uintptr_t kDirectorStride = 0x1E8;
	constexpr uintptr_t kDirectorCamera = 0x08;
	constexpr uintptr_t kDirectorWatched = 0x1AC;
	constexpr uintptr_t kDirectorTransA = 0x1A4;
	constexpr uintptr_t kDirectorTransB = 0x1A8;

	constexpr uintptr_t kCamVtable = 0x00, kCamGateA = 0x0C, kCamGateB = 0x10;
	constexpr uintptr_t kCamPos = 0x18, kCamYaw = 0x24, kCamPitch = 0x28;
	constexpr uintptr_t kCamPrevPos = 0x2C, kCamPrevYaw = 0x38, kCamPrevPitch = 0x3C;
	constexpr uintptr_t kCamSweep = 0x40, kCamRampA = 0x44, kCamRampB = 0x48, kCamFlags = 0x4C;

	// ⚠ 0x50 exactly. The flying ctor covers +0x00..+0x4F and the update never touches anything above
	// +0x4F, so this is provably the whole camera. A larger snapshot (an inferred class boundary at
	// 0x19C was suggested) risks restoring stale DIRECTOR-owned fields on top of the director.
	constexpr size_t kCameraSnapshot = 0x50;

	constexpr uintptr_t kObserverStride = 0x4A0;
	constexpr uintptr_t kObsPos = 0x15C, kObsForward = 0x184;

	// The engine's own clamp, which lives inside the look-input block we disable - so we must apply it.
	constexpr float kPitchLimit = 1.49225652f;   // 85.5 degrees

	bool sehCopy(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehWrite(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template<typename T> bool readAt(uintptr_t a, T& out)
	{
		if (!a) return false;
		return sehCopy(&out, (const void*)a, sizeof(T));
	}
	template<typename T> bool writeAt(uintptr_t a, const T& v)
	{
		if (!a) return false;
		return sehWrite((void*)a, &v, sizeof(T));
	}

	typedef LONG(NTAPI* fnNtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	struct ThreadBasicInfo { LONG ExitStatus; PVOID Teb; ULONG_PTR Pid, Tid, Affinity; LONG Pri, BasePri; };

	// ⚠ Re-enumerate every time. A fifth sim-candidate thread appeared mid-session during recon that had
	// not existed minutes earlier, so any cached thread handle or TLS pointer goes stale.
	uintptr_t findSimTls(uintptr_t exeBase)
	{
		uint32_t tlsIndex = 0;
		if (!readAt(exeBase + kRvaTlsIndex, tlsIndex)) return 0;

		static fnNtQueryInformationThread ntq = (fnNtQueryInformationThread)
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
		if (!ntq) return 0;

		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return 0;

		const DWORD pid = GetCurrentProcessId();
		uintptr_t best = 0;
		THREADENTRY32 te{}; te.dwSize = sizeof(te);
		if (Thread32First(snap, &te))
		{
			do
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
				if (!h) continue;
				ThreadBasicInfo tbi{}; ULONG len = 0;
				const LONG st = ntq(h, 0, &tbi, sizeof(tbi), &len);
				CloseHandle(h);
				if (st != 0 || !tbi.Teb) continue;

				uintptr_t tlsArray = 0, block = 0;
				if (!readAt((uintptr_t)tbi.Teb + 0x58, tlsArray) || !tlsArray) continue;
				if (!readAt(tlsArray + 8ull * tlsIndex, block) || !block) continue;

				uintptr_t pg = 0, og = 0;
				if (!readAt(block + kTlsPlayerGlo, pg) || !pg) continue;
				if (!readAt(block + kTlsObjectGlo, og) || !og) continue;

				uint32_t gate = 0;
				readAt(block + kTlsGate, gate);
				if (!best) best = block;
				if (gate & 1) { best = block; break; }
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
		return best;
	}
}

class H5FreeCamera::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	std::unique_ptr<ScopedCallback<RenderEvent>> mTickCallback;

	// Saved camera state, so disabling restores whatever mode was running.
	uint8_t mSnapshot[kCameraSnapshot]{};
	float mSavedTransA = 0.f, mSavedTransB = 0.f;
	bool mHaveSnapshot = false;

	// HCM-owned camera state while free camera is on.
	SimpleMath::Vector3 mPos{};
	float mYaw = 0.f, mPitch = 0.f;
	bool mSeeded = false;

	std::chrono::steady_clock::time_point mLastTick{};
	std::chrono::steady_clock::time_point mLastFailureLog{};

	struct Resolved
	{
		uintptr_t exeBase = 0, simTls = 0, director = 0, camera = 0, observer = 0;
	};

	Resolved resolve()
	{
		lockOrThrow(playerStateWeak, playerState);
		Resolved r;
		r.exeBase = playerState->getExeBase();
		r.simTls = findSimTls(r.exeBase);
		if (!r.simTls) throw HCMRuntimeException("No Halo 5 simulation thread found");

		uintptr_t dirBlock = 0;
		if (!readAt(r.simTls + kTlsDirectors, dirBlock) || !dirBlock)
			throw HCMRuntimeException("The Halo 5 camera director block is not available");
		r.director = dirBlock;                       // user index 0
		r.camera = r.director + kDirectorCamera;

		int32_t watched = 0;
		readAt(r.director + kDirectorWatched, watched);
		if (watched < 0 || watched > 3) watched = 0;

		uintptr_t obsBlock = 0;
		if (readAt(r.simTls + kTlsObservers, obsBlock) && obsBlock)
			r.observer = obsBlock + (uintptr_t)watched * kObserverStride;
		return r;
	}

	bool isFlying(const Resolved& r)
	{
		uintptr_t vt = 0;
		if (!readAt(r.camera + kCamVtable, vt)) return false;
		return vt == r.exeBase + kRvaFlyingVtable;
	}

	// ⚠ Read-only evaluation of the predicate that can FORCE the collision sweep on regardless of the
	// flag bit (exe+0x006EC8EC). We report it rather than promising noclip we cannot deliver.
	bool collisionForcedOn(const Resolved& r)
	{
		uintptr_t p = 0;
		if (!readAt(r.simTls + kTlsNoclipPred, p) || !p) return false;
		uint64_t a = 0, b = 0;
		if (!readAt(p, a) || !readAt(p + 8, b)) return false;
		if (a == 0 && b == 0) return false;
		uint32_t v = 0;
		if (!readAt(p + 0x37240, v)) return false;
		return v == 2;
	}

	void enable()
	{
		const Resolved r = resolve();

		// ⚠⚠ The vtable pointer is only 4-byte aligned (camera & 7 == 4). An 8-byte store is atomic only
		// if it does not straddle a 64-byte cache line - otherwise a reader can observe half a pointer
		// and call through garbage. Refuse rather than gamble.
		if ((r.camera & 63) > 56)
			throw HCMRuntimeException(std::format(
				"Free camera refused: the camera object landed at an address where flipping its vtable "
				"would straddle a cache line (camera & 63 = {}). This is luck of the allocator - reloading "
				"the level will almost certainly move it.", r.camera & 63));

		// Snapshot BEFORE anything is written, so disable restores the real previous mode.
		if (!sehCopy(mSnapshot, (const void*)r.camera, kCameraSnapshot))
			throw HCMRuntimeException("Could not snapshot the Halo 5 camera");
		readAt(r.director + kDirectorTransA, mSavedTransA);
		readAt(r.director + kDirectorTransB, mSavedTransB);
		mHaveSnapshot = true;

		// Seed from the observer - the published render camera - so free camera starts exactly where the
		// view already is.
		SimpleMath::Vector3 pos{}, fwd{ 1.f, 0.f, 0.f };
		bool seeded = false;
		if (r.observer)
		{
			float p[3]{}, f[3]{};
			if (sehCopy(p, (const void*)(r.observer + kObsPos), sizeof(p))
				&& sehCopy(f, (const void*)(r.observer + kObsForward), sizeof(f)))
			{
				pos = { p[0], p[1], p[2] };
				fwd = { f[0], f[1], f[2] };
				seeded = std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z)
					&& fwd.LengthSquared() > 1e-6f;
			}
		}
		if (!seeded)
		{
			lockOrThrow(playerStateWeak, playerState);
			pos = playerState->getCameraPosition();
			fwd = playerState->getPlayerAim();
		}
		fwd.Normalize();

		// Z-up convention, from exe+0x0066E220: yaw = atan2(f.y, f.x), pitch = atan2(f.z, |f.xy|).
		mYaw = std::atan2(fwd.y, fwd.x);
		mPitch = std::atan2(fwd.z, std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y));
		mPitch = std::clamp(mPitch, -kPitchLimit, kPitchLimit);
		mPos = pos;
		mSeeded = true;

		uint32_t flags = 0;
		if (auto settings = settingsWeak.lock())
			flags = settings->h5FreeCameraUseEngineInput->GetValue() ? 0x1Cu : 0x00u;

		float sweep = 0.03334f;
		readAt(r.exeBase + kRvaSweepRadius, sweep);

		// ---- 1. the genuinely inert tail. The first-person class ends at +0x25, so nothing here is
		// observed by the camera that is still live.
		const float zero = 0.f;
		writeAt(r.camera + kCamPitch, mPitch);
		float zeros3[3]{ 0.f, 0.f, 0.f };
		sehWrite((void*)(r.camera + kCamPrevPos), zeros3, sizeof(zeros3));
		writeAt(r.camera + kCamPrevYaw, zero);
		writeAt(r.camera + kCamPrevPitch, zero);
		writeAt(r.camera + kCamSweep, sweep);
		writeAt(r.camera + kCamRampA, zero);
		writeAt(r.camera + kCamRampB, zero);
		writeAt(r.camera + kCamFlags, flags);

		// ---- 2. the 16 bytes that ARE live on the first-person camera (its prevFOV/yaw/pitch/control
		// byte). Written as late as possible: the worst case is a single frame of a snapped view.
		const uint32_t z32 = 0;
		writeAt(r.camera + kCamGateA, z32);
		writeAt(r.camera + kCamGateB, z32);
		float posYaw[4]{ mPos.x, mPos.y, mPos.z, mYaw };
		sehWrite((void*)(r.camera + kCamPos), posYaw, sizeof(posYaw));

		// ---- 3. the class flip, last, as one store.
		const uint64_t vt = (uint64_t)(r.exeBase + kRvaFlyingVtable);
		if (!writeAt(r.camera + kCamVtable, vt))
			throw HCMRuntimeException("Could not switch the Halo 5 camera to flying mode");

		// Kill any in-flight camera transition, exactly as camera_set_flying_cam_at_point does.
		writeAt(r.director + kDirectorTransA, 0.f);

		mLastTick = {};

		if (auto messagesGUI = messagesGUIWeak.lock())
		{
			if (flags == 0 && collisionForcedOn(r))
				messagesGUI->addMessage("Free Camera on - but the engine is forcing collision on right now, "
					"so it will not pass through walls.");
			else
				messagesGUI->addMessage(flags ? "Free Camera on (engine input)" : "Free Camera on");
		}
	}

	void disable()
	{
		if (!mHaveSnapshot) return;
		try
		{
			const Resolved r = resolve();
			// Restore the whole camera verbatim - no ctor replication, so whatever mode was running comes
			// back exactly. ⚠ Both transition fields: camera_set_flying_cam_at_point writes +0x1A4 from
			// +0x1A8 and then sets +0x1A8 = -1, so restoring only +0x1A4 leaves the pair inconsistent.
			sehWrite((void*)r.camera, mSnapshot, kCameraSnapshot);
			writeAt(r.director + kDirectorTransA, mSavedTransA);
			writeAt(r.director + kDirectorTransB, mSavedTransB);
		}
		catch (...) {}
		mHaveSnapshot = false;
		mSeeded = false;
	}

	// Analog where the backend provides it (gamepad sticks), digital otherwise.
	static float axis(ImGuiKey negative, ImGuiKey positive)
	{
		float v = 0.f;
		if (const ImGuiKeyData* n = ImGui::GetKeyData(negative))
			v -= (n->AnalogValue > 0.f ? n->AnalogValue : (n->Down ? 1.f : 0.f));
		if (const ImGuiKeyData* p = ImGui::GetKeyData(positive))
			v += (p->AnalogValue > 0.f ? p->AnalogValue : (p->Down ? 1.f : 0.f));
		return std::clamp(v, -1.f, 1.f);
	}

	static float held(ImGuiKey a, ImGuiKey b)
	{
		return (ImGui::IsKeyDown(a) || ImGui::IsKeyDown(b)) ? 1.f : 0.f;
	}

	void onTick()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (GlobalKill::isKillSet()) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				mLastTick = {};
				return;
			}
			lockOrThrow(settingsWeak, settings);
			const Resolved r = resolve();

			// ⚠ A Lua script can take the camera back at any moment - camera_set_mode is script-facing and
			// camera_set_flying_cam_at_point calls the flying ctor directly. Re-seat rather than silently
			// doing nothing.
			if (!isFlying(r))
			{
				mSeeded = false;
				enable();
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			float dt = 1.f / 60.f;
			if (mLastTick.time_since_epoch().count() != 0)
				dt = std::clamp(std::chrono::duration<float>(now - mLastTick).count(), 1.f / 1000.f, 1.f / 10.f);
			mLastTick = now;

			// With flags = 0x1C the ENGINE drives the camera; leave its fields alone.
			if (settings->h5FreeCameraUseEngineInput->GetValue()) return;
			if (!mSeeded) return;

			const float speed = settings->h5FreeCameraSpeed->GetValue();
			const float sens = settings->h5FreeCameraSensitivity->GetValue();

			// LOOK: right stick, arrow keys, and the mouse when HCM's menu is not eating it.
			float lookX = axis(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight);
			float lookY = axis(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown);
			lookX += held(ImGuiKey_RightArrow, ImGuiKey_RightArrow) - held(ImGuiKey_LeftArrow, ImGuiKey_LeftArrow);
			lookY += held(ImGuiKey_DownArrow, ImGuiKey_DownArrow) - held(ImGuiKey_UpArrow, ImGuiKey_UpArrow);

			const ImGuiIO& io = ImGui::GetIO();
			if (!io.WantCaptureMouse)
			{
				lookX += io.MouseDelta.x * 0.15f;
				lookY += io.MouseDelta.y * 0.15f;
			}

			mYaw -= lookX * sens * dt;
			mPitch -= lookY * sens * dt;
			// The engine's own clamp lives inside the look block we disabled, so we apply it ourselves.
			mPitch = std::clamp(mPitch, -kPitchLimit, kPitchLimit);
			if (mYaw > 3.14159265f) mYaw -= 6.28318531f;
			if (mYaw < -3.14159265f) mYaw += 6.28318531f;

			// MOVE, in the engine's Z-up basis (exe+0x00673510).
			const float cy = std::cos(mYaw), sy = std::sin(mYaw);
			const float cp = std::cos(mPitch), sp = std::sin(mPitch);
			const SimpleMath::Vector3 fwd{ cy * cp, sy * cp, sp };
			const SimpleMath::Vector3 right{ sy, -cy, 0.f };
			const SimpleMath::Vector3 up = right.Cross(fwd);

			float moveF = axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp);
			float moveR = axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight);
			moveF += held(ImGuiKey_W, ImGuiKey_W) - held(ImGuiKey_S, ImGuiKey_S);
			moveR += held(ImGuiKey_D, ImGuiKey_D) - held(ImGuiKey_A, ImGuiKey_A);
			float moveU = held(ImGuiKey_Space, ImGuiKey_GamepadR1) - held(ImGuiKey_LeftCtrl, ImGuiKey_GamepadL1);

			float mult = 1.f;
			if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_GamepadR2)) mult = 4.f;
			if (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_GamepadL2)) mult = 0.25f;

			mPos += (fwd * moveF + right * moveR + up * moveU) * (speed * mult * dt);

			float posYaw[4]{ mPos.x, mPos.y, mPos.z, mYaw };
			sehWrite((void*)(r.camera + kCamPos), posYaw, sizeof(posYaw));
			writeAt(r.camera + kCamPitch, mPitch);
		}
		catch (HCMRuntimeException& ex)
		{
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Free camera skipped a tick (will retry): " << ex.what();
			}
		}
		catch (...) {}
	}

	void onToggleChanged(bool& newValue)
	{
		try
		{
			if (newValue && !mTickCallback)
			{
				enable();
				mTickCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onTick(); });
			}
			else if (!newValue && mTickCallback)
			{
				mTickCallback.reset();
				disable();
			}
		}
		catch (HCMRuntimeException ex)
		{
			mTickCallback.reset();
			if (auto settings = settingsWeak.lock())
			{
				settings->h5FreeCameraToggle->GetValueDisplay() = false;
				settings->h5FreeCameraToggle->UpdateValueWithInput();
			}
			ex.prepend("Free Camera: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// TELEPORT TO CAMERA. Reuses the proven character-controller proxy write rather than inventing a
	// second teleport path.
	// ⚠ teleportPlayerTo takes the OBJECT (feet) position, while the camera is at the EYE. The offset is
	// measured LIVE rather than hard-coded 0.6 - it is exactly 0.599998 with zero variance across 14
	// samples standing, but crouch and vehicles move the eye.
	void onTeleportToCamera()
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				messagesGUI->addMessage("Teleport To Camera: not in game");
				return;
			}
			lockOrThrow(playerStateWeak, playerState);

			SimpleMath::Vector3 camPos;
			const Resolved r = resolve();
			if (mTickCallback && isFlying(r))
			{
				float p[3]{};
				if (!sehCopy(p, (const void*)(r.camera + kCamPos), sizeof(p)))
					throw HCMRuntimeException("Could not read the free camera position");
				camPos = { p[0], p[1], p[2] };
			}
			else if (r.observer)
			{
				float p[3]{};
				if (!sehCopy(p, (const void*)(r.observer + kObsPos), sizeof(p)))
					throw HCMRuntimeException("Could not read the observer position");
				camPos = { p[0], p[1], p[2] };
			}
			else
			{
				camPos = playerState->getCameraPosition();
			}

			const auto eye = playerState->getCameraPosition();
			const auto feet = playerState->getPlayerPosition();
			const SimpleMath::Vector3 eyeOffset = eye - feet;

			playerState->teleportPlayerTo(camPos - eyeOffset);
			messagesGUI->addMessage(std::format("Teleported to camera ({:.2f}, {:.2f}, {:.2f})",
				camPos.x, camPos.y, camPos.z));
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Teleport To Camera: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<ActionEvent> mTeleportCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5FreeCameraToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); }),
		mTeleportCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5TeleportToCameraEvent,
			[this]() { onTeleportToCamera(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5FreeCamera only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mTickCallback.reset();
		// Put the camera back before we go, or the game is left in a mode nothing owns.
		disable();
		mToggleCallback.removeCallback();
		mTeleportCallback.removeCallback();
	}
};


H5FreeCamera::H5FreeCamera(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5FreeCamera::~H5FreeCamera() { PLOG_VERBOSE << "~" << getName(); }
