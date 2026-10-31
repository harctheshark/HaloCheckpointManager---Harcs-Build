#include "pch.h"
#include "ChaseCamera.h"
#include "SettingsStateAndEvents.h"
#include "IMessagesGUI.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "ModuleHook.h"
#include <span>
#include <intrin.h>

// ============================================================================
//  Chase camera (MCC build 1.3528). RVAs below are module RVAs = IDA EA - 0x180000000.
//
//  HALO 2 (halo2.dll) - RE in the original H2ChaseCamera notes:
//   * The director is a 4-element array of 0x150-byte structs at RVA 0x15EB220. Element +0x10 is the camera's UPDATE
//     FUNCTION POINTER, which IS the camera mode: 0x77DEB0 first person, 0x77C9F0 following (chase), 0x77D570 dead,
//     0x77E990 editor, 0x7186D0 scripted. director_get_perspective (0x6D4EC0) classifies that pointer.
//   * The per-frame chooser 0x6D4B20(user, force) asks the predicate 0x6D4C50(unit_index, short* perspective_code)
//     "should this unit use the following camera?". On foot the answer is always 0 (only 1-sources: a vehicle seat with
//     the third-person flag, or a weapon with bit 31 at +0x12C). If 1 and the camera is first person it installs the
//     following camera (+ blend); if 0 and following it swaps back. It only re-evaluates when the stored perspective
//     code (element +0x70) differs from the new one, or on force. Aim (0x6D4730), level init (0x6D5160) and cutscene
//     exit (0x6D58A0) call the SAME predicate, so one hook keeps all of them consistent.
//   * The following camera needs no vehicle: with no unit camera tracks the evaluators fall back to the default track
//     in game globals, which is what the engine's own debug camera cycle uses on foot.
//   Hook: inline-hook 0x6D4C50; while enabled, when the original answers 0 for a live unit answer 1 and write a
//   sentinel perspective code so 0x6D4B20 sees "code changed" and switches (and swaps back when we go inert).
//
//  HALO 3 / ODST (halo3.dll / halo3odst.dll) - see "Porting ... to Halo 3 / ODST" research report section 4:
//   * Director per user; dir+8 is the camera object (vtable: slot0 = camera/perspective code, slot1 = 1, slot2 = update
//     (this, user, dt, observer_command*), slot3/4 = misc). director_set_perspective(dir, int code, float blend,
//     bool force) = H3 0x131B40 / ODST 0x15298C; codes: 0 FOLLOW (update 0x213CF4 / ODST 0x244118), 1 FIRST PERSON
//     (0x214F20), 2 flying, 3 theater, 4 unit/vehicle, 5-7 scripted. Early-out when code == current && !force.
//   * The director's per-tick update (H3 0x211E80) calls it with code 1 for a living on-foot unit (and when leaving a
//     vehicle). There is no predicate - the code ARGUMENT is rewritten 1 -> 0 while enabled. Turning off just stops the
//     rewrite; the next tick's call (code 1, current 0) switches back with blend 0.
//   * After a successful switch the director writes [camera+0x20] := [director+0x8C] (a first-person value) and
//     [camera+0x2A] := 1 into the new camera object. For the follow camera +0x20 is a distance/offset scale that the
//     engine itself initialises to 1.0f (read by the update as `mulss [rsi+0x20]`), so the follow detour resets it to
//     1.0f before the update runs (only while enabled).
//   * The follow update writes the 0xEC-byte observer command through its 4th argument: +0 flags (it sets 0x800 = FOV
//     valid), +4..+0xC focus point, +0x28 FOV radians. Early-outs untouched when [camera+8] (unit) == -1.
//   * Observer collision stage (H3 0x17E8B4 / ODST 0x1AD468) is skipped when (observer[+0x18] & 0x10); observer[+0x18]
//     is the published command flags. Same bit as Halo 2.
//
//  HALO: REACH / HALO 4 (haloreach.dll / halo4.dll) - research report section 2:
//   * director_set_camera_mode(dir, int mode, float blend, bool force) = Reach 0xC5834 / H4 0x10BD98 (same ABI as
//     H3's set_perspective). Modes: 0 tag-driven unit camera, 1 FOLLOWING (update Reach 0x1DE658 / H4 0x29493C),
//     2 orbit/dead, 3 FIRST PERSON, 4 unit/vehicle, 5 flying, 6 scripted, 7-12 theater/cinematic.
//   * The per-tick chooser (Reach 0x1DAF84 / H4 0x29071C) calls setMode(3, blend, force=0) every tick on foot while the
//     director's desired mode (+0x190 / +0x1B8) is 3; that desired mode is NOT touched by setMode, so rewriting the mode
//     argument 3 -> 1 (not forced) is stable: the chooser keeps asking for 3, we keep answering 1, the early-out
//     fires. Turning off: setMode(3) with current 1 rebuilds first person.
//   * Same observer command layout (flags +0, focus +4..+0xC, FOV radians float[10] = +0x28); the Reach command is
//     validated at the end of every camera update by 0xCB11C (H4 0x129498), which the following update tail-calls and
//     which the observer re-runs on intake (focus clamp +-50000, FOV clamp [0.01745, 2.618] rad), so a FOV of up to
//     150 degrees survives. The collision stage (Reach 0xC8290 / H4 0x1263D0, which also contains the push-out stage)
//     is skipped when (observer[+0x18] & 0x10).
//   Hooking the following camera's own update (rather than the shared validator) means the edits cannot touch any
//   other camera mode, and no "which camera is current" test is needed.
//
//  The select hook is attached once (after a prologue check) and left inert while off - detaching an inline hook
//  while the game thread is inside it races (see ModuleHook.cpp, hook graveyard).
// ============================================================================

namespace
{
	// ---- prologue bytes: first instructions up to (not including) the first relative operand, from the installed
	// 1.3528 DLLs. A different build, or a function someone else already hooked, is refused. All are shorter than
	// 0x4A bytes so they never overlap HCM's own ThirdPersonRendering mid-hook inside the select function. ----

	// halo2.dll sub_1806D4C50 prologue up to its first call. mov [rsp+18h],rbx; push rbp,rsi,rdi,r13,r14; sub rsp,20h;
	// xor ebx,ebx; mov r14d,ecx; mov rdi,rdx; mov ebp,ebx; lea edx,[rbx+3]
	constexpr uint8_t kH2PredicateBytes[] = {
		0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
		0x33, 0xDB, 0x44, 0x8B, 0xF1, 0x48, 0x8B, 0xFA, 0x8B, 0xEB, 0x8D, 0x53, 0x03 };

	// halo2.dll sub_18077C9F0 (following camera update) prologue up to its first call.
	constexpr uint8_t kH2FollowBytes[] = {
		0x48, 0x8B, 0xC4, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF2,
		0x0F, 0x29, 0x70, 0xC8, 0x48, 0x8B, 0xF1, 0x4C, 0x89, 0x78, 0x18, 0x48, 0x8D, 0x50, 0x88, 0x49, 0x8B, 0xF8, 0x41, 0x8B, 0x0E };

	// halo3.dll 0x131B40 and halo3odst.dll 0x15298C director_set_perspective (identical bytes in both):
	// mov rax,rsp; mov [rax+8],rbx; mov [rax+10h],rbp; mov [rax+18h],rsi; push rdi,r12,r13,r14,r15; sub rsp,60h;
	// lea rbx,[rcx+8]; movaps [rax-38h],xmm6; mov rax,[rbx]; mov rsi,rcx
	constexpr uint8_t kH3SelectBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55,
		0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8D, 0x59, 0x08, 0x0F, 0x29, 0x70, 0xC8, 0x48, 0x8B, 0x03, 0x48,
		0x8B, 0xF1 };

	// halo3.dll 0x213CF4 follow-camera update (observer command writer)
	constexpr uint8_t kH3FollowBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55,
		0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xC8, 0x41,
		0x83, 0xCE, 0xFF };

	// halo3odst.dll 0x244118 follow-camera update
	constexpr uint8_t kOdstFollowBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55, 0x41, 0x54, 0x41, 0x55,
		0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xC8, 0x49,
		0x8B, 0xF9 };

	// haloreach.dll 0xC5834 director_set_camera_mode
	constexpr uint8_t kReachSelectBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41,
		0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8D, 0x59, 0x08, 0x0F, 0x29, 0x70, 0xD8, 0x48, 0x8B, 0x03,
		0x48, 0x8B, 0xF1 };

	// haloreach.dll 0x1DE658 following-camera update
	constexpr uint8_t kReachFollowBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41,
		0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x8B, 0x79, 0x08, 0x4D, 0x8B, 0xF1, 0x0F, 0x29,
		0x70, 0xD8, 0x8B, 0xEA };

	// halo4.dll 0x10BD98 director_set_camera_mode
	constexpr uint8_t kH4SelectBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54, 0x41, 0x56,
		0x48, 0x83, 0xEC, 0x60, 0x48, 0x8D, 0x59, 0x08, 0x0F, 0x29, 0x70, 0xD8, 0x48, 0x8B, 0x03, 0x48, 0x8B, 0xF1, 0x48, 0x8B,
		0xCB, 0x45, 0x8A, 0xF1 };

	// halo4.dll 0x29493C following-camera update
	constexpr uint8_t kH4FollowBytes[] = {
		0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x41,
		0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0x59, 0x08, 0x4D, 0x8B, 0xF1, 0x0F, 0x29,
		0x70, 0xD8, 0x8B, 0xEA };

	// Observer command flag bit 4 (Blam observer: no camera collision). The following camera writes the command
	// every frame (flags dword at +0), so setting the bit right after it = a no-clip chase camera. Same bit in all five games.
	constexpr uint32_t kObserverCommandNoCollision = 0x10;
	// Halo 3 / ODST only: "FOV is valid" bit of the command flags (the update sets it unconditionally; checked anyway).
	constexpr uint32_t kH3CommandFovValid = 0x800;

	// Halo 2: real codes the predicate writes are 0..4. Any other value is just "different from the real one" so that
	// 0x6D4B20's `stored != new` check fires on the way in AND on the way out.
	constexpr int16_t kH2ForcedPerspectiveCode = 0x7C;

	bool entryBytesMatch(uintptr_t at, std::span<const uint8_t> expect)
	{
		uint8_t got[64] = {};
		if (expect.size() > sizeof(got)) return false;
		__try { memcpy(got, (const void*)at, expect.size()); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return memcmp(got, expect.data(), expect.size()) == 0;
	}

	// Camera/perspective code of the director's current camera object (dir+8; its vtable slot 0). -1 if unreadable.
	// Plain POD only: __try cannot sit in a function with C++ objects that need unwinding.
	int32_t currentCameraCode(void* dir)
	{
		int32_t id = -1;
		__try
		{
			void* cam = static_cast<uint8_t*>(dir) + 8;
			void** vtable = *reinterpret_cast<void***>(cam);
			id = reinterpret_cast<int32_t(__fastcall*)(void*)>(vtable[0])(cam);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { id = -1; }
		return id;
	}
}

template <GameState::Value gameT>
class ChaseCameraImpl final : public ChaseCameraImplBase
{
private:
	static constexpr bool kIsH2 = (gameT == GameState::Value::Halo2);
	static constexpr bool kIsH3Family = (gameT == GameState::Value::Halo3 || gameT == GameState::Value::Halo3ODST);
	static constexpr bool kIsReachFamily = (gameT == GameState::Value::HaloReach || gameT == GameState::Value::Halo4);
	static_assert(kIsH2 || kIsH3Family || kIsReachFamily, "ChaseCameraImpl: unsupported game");

	// Camera code the engine asks for on foot (first person) and the code of the following camera, for the
	// argument-rewrite backends (H3/ODST: director_set_perspective codes; Reach/H4: director_set_camera_mode modes).
	static constexpr int32_t kFirstPersonCode = kIsH3Family ? 1 : 3;
	static constexpr int32_t kFollowCode = kIsH3Family ? 0 : 1;

	static std::span<const uint8_t> selectEntryBytes()
	{
		if constexpr (gameT == GameState::Value::Halo2) return kH2PredicateBytes;
		else if constexpr (kIsH3Family) return kH3SelectBytes;
		else if constexpr (gameT == GameState::Value::HaloReach) return kReachSelectBytes;
		else return kH4SelectBytes;
	}

	static std::span<const uint8_t> followEntryBytes()
	{
		if constexpr (gameT == GameState::Value::Halo2) return kH2FollowBytes;
		else if constexpr (gameT == GameState::Value::Halo3) return kH3FollowBytes;
		else if constexpr (gameT == GameState::Value::Halo3ODST) return kOdstFollowBytes;
		else if constexpr (gameT == GameState::Value::HaloReach) return kReachFollowBytes;
		else return kH4FollowBytes;
	}

	GameState mGame;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;

	std::shared_ptr<MultilevelPointer> mSelectFunction;      // see InternalPointerData.xml chaseCameraSelectFunction
	std::shared_ptr<MultilevelPointer> mFollowFunction;      // chaseCameraFollowFunction
	std::unique_ptr<ModuleInlineHook> mSelectHook;
	std::unique_ptr<ModuleInlineHook> mFollowHook;
	std::mutex mArmMutex;                                    // toggle thread vs MCC-state thread
	bool mRefused = false;                                   // a prologue didn't match: never hook this build
	bool mFollowUnavailable = false;                         // Halo 2 only: no-clip / FOV / height unavailable (chase itself still works)

	// One instantiation per game, so these are per game. sEnabled is the EFFECTIVE state: toggle on AND this game is
	// the one being played AND its hooks are armed. The hooks are inert (answer honestly) whenever it is false.
	static inline std::atomic<ChaseCameraImpl*> sInstance{ nullptr };
	static inline std::atomic_bool sEnabled{ false };
	static inline std::atomic_bool sNoClip{ false };
	// diagnostics: the first few detour calls after each enable are logged (rate-limited; game thread)
	static inline std::atomic_int sDiagSelect{ 0 }, sDiagFollow{ 0 };
	static constexpr int kDiagMax = 8;
	std::shared_ptr<BinarySetting<float>> mFovSetting, mHeightSetting;   // read by the game thread each chase update

	// Subscribed LAST-declared / reset FIRST in ~ChaseCameraImpl: the settings object and MCC state hook outlive this cheat.
	std::unique_ptr<ScopedCallback<ToggleEvent>> mNoClipCallback;
	std::unique_ptr<ScopedCallback<ToggleEvent>> mToggleCallback;
	std::unique_ptr<ScopedCallback<eventpp::CallbackList<void(const MCCState&)>>> mStateCallback;

	// ------------------------------------------------------------------------------------------------------------
	//  Halo 2 detours
	// ------------------------------------------------------------------------------------------------------------

	// int __fastcall sub_1806D4C50(uint32 unit_index, int16* out_perspective_code)
	// Returns 1 = use the following (chase) camera. Called by the game/director thread every director update.
	static int32_t __fastcall h2PredicateDetour(uint32_t unitIndex, int16_t* outPerspectiveCode)
	{
		ChaseCameraImpl* self = sInstance.load(std::memory_order_acquire);
		if (!self || !self->mSelectHook)
		{
			// teardown race: answer "no unit" the way the original does, never leave the out-param unwritten
			if (outPerspectiveCode) *outPerspectiveCode = 0;
			return 0;
		}

		const int32_t original = self->mSelectHook->getInlineHook().template call<int32_t>(unitIndex, outPerspectiveCode);

		// The aim query (0x6D4730: where the unit aims from and along - used for the red reticle / auto-aim, melee and
		// sword-lunge target acquisition) asks this same predicate and, when told "following camera", aims from the
		// chase camera behind you, so targets come out farther away than in first person. Give it the real answer: the
		// view stays third person, but aiming, reticle colour and lunge range stay exactly first-person.
		{
			static const uintptr_t sBase = (uintptr_t)GetModuleHandleW(L"halo2.dll");
			constexpr uintptr_t kAimQueryReturn = 0x6D4756;   // the instruction after "call 0x6D4C50" in 0x6D4730 (1.3528)
			if (sBase && (uintptr_t)_ReturnAddress() - sBase == kAimQueryReturn) return original;
		}

		// original == 0 for a unit on foot / in a first-person seat. unitIndex == NONE (-1) is "no unit" (dead,
		// not spawned): leave that to the engine so the dead camera and respawn path stay untouched.
		if (original == 0 && sEnabled.load(std::memory_order_relaxed) && unitIndex != 0xFFFFFFFFu && outPerspectiveCode)
		{
			*outPerspectiveCode = kH2ForcedPerspectiveCode;
			return 1;
		}
		return original;
	}

	// __int64 __fastcall sub_18077C9F0(camera_state*, camera_input*, s_observer_command* out)
	static int64_t __fastcall h2FollowDetour(void* state, void* input, uint8_t* outCommand)
	{
		ChaseCameraImpl* self = sInstance.load(std::memory_order_acquire);
		if (!self || !self->mFollowHook) return 0;
		const int64_t r = self->mFollowHook->getInlineHook().template call<int64_t>(state, input, outCommand);
		if (outCommand && sEnabled.load(std::memory_order_relaxed))
		{
			if (sNoClip.load(std::memory_order_relaxed))
				*(uint32_t*)outCommand |= kObserverCommandNoCollision;
			// FOV: command float[10] (+0x28) = field of view (radians); the update mirrors it into float[34] (+0x88) under
			// flag 0x80 and blends on change (decompile of sub_18077C9F0). 0 = leave the game's own value.
			const float fovDeg = self->mFovSetting ? self->mFovSetting->GetValue() : 0.f;
			if (fovDeg > 1.f && fovDeg < 179.f)
			{
				const float rad = fovDeg * 0.017453292f;
				*(float*)(outCommand + 0x28) = rad;
				*(float*)(outCommand + 0x88) = rad;
			}
			// vertical offset: command float[1..3] (+0x04..+0x0C) = the focus point the camera orbits (the unit's camera
			// position from sub_1806C3A90); raising its z lifts both the camera and what it looks at.
			const float dz = self->mHeightSetting ? self->mHeightSetting->GetValue() : 0.f;
			if (dz != 0.f && std::isfinite(dz)) *(float*)(outCommand + 0x0C) += dz;
		}
		return r;
	}

	// ------------------------------------------------------------------------------------------------------------
	//  Halo 3 / ODST / Reach / Halo 4 detours
	// ------------------------------------------------------------------------------------------------------------

	// bool __fastcall director_set_perspective / director_set_camera_mode(void* director, int code, float blend, bool force)
	// Returns "changed". While enabled and the engine asks for first person without force, ask for the following camera.
	static uint8_t __fastcall selectDetour(void* director, int32_t code, float blend, uint8_t force)
	{
		ChaseCameraImpl* self = sInstance.load(std::memory_order_acquire);
		if (!self || !self->mSelectHook) return 0;   // teardown race: "nothing changed"

		// H3 / ODST have TWO first-person requests: code 1 from the gameplay branch of the director update (sub_180211E80,
		// unit found via the player) and code 3 from its other branch (unit from sub_1800EE48C, the path MCC sessions take -
		// live log 2026-10-14: code 3 every tick on foot); that branch's own "use the camera track" case asks for 0 (follow).
		const bool firstPersonRequest = code == kFirstPersonCode || (kIsH3Family && code == 3);
		if (director && !force && firstPersonRequest && sEnabled.load(std::memory_order_relaxed))
		{
			const int32_t asked = code;
			// Already on the following camera: report "no change" WITHOUT calling the original. Normally the original
			// would early-out the same way, but with HCM's ThirdPersonRendering mid-hook active (Free Camera option) that
			// early-out is defeated and the camera object is rebuilt on every call, which would reset the following
			// camera each tick (Reach / Halo 4 lose their yaw/pitch). Skipping is otherwise identical: the original's
			// no-change tail only refreshes a perspective cache that cannot have changed.
			const int32_t cur = currentCameraCode(director);
			if (cur == kFollowCode)
			{
				if (sDiagSelect.fetch_add(1) < kDiagMax) PLOG_INFO << "[ChaseCamera] select: asked " << asked << ", current camera " << cur << " = follow -> 'no change'";
				return 0;
			}
			code = kFollowCode;
			const uint8_t changed = self->mSelectHook->getInlineHook().template call<uint8_t>(director, code, blend, force);
			if (sDiagSelect.fetch_add(1) < kDiagMax)
				PLOG_INFO << "[ChaseCamera] select: asked " << asked << " (current camera " << cur << ") -> rewrote to " << kFollowCode
				<< ", original returned changed=" << (int)changed << ", camera now " << currentCameraCode(director);
			return changed;
		}
		if (sEnabled.load(std::memory_order_relaxed) && sDiagSelect.fetch_add(1) < kDiagMax)
			PLOG_INFO << "[ChaseCamera] select: passed through code " << code << " force " << (int)force << " (not rewritten)";
		return self->mSelectHook->getInlineHook().template call<uint8_t>(director, code, blend, force);
	}

	// void(ish) __fastcall follow_camera_update(void* camera, int user, float dt, s_observer_command* out)
	// camera+8 = unit datum index (-1 = none: the update then returns without writing the command).
	static int64_t __fastcall followDetour(void* camera, int32_t user, float dt, uint8_t* outCommand)
	{
		ChaseCameraImpl* self = sInstance.load(std::memory_order_acquire);
		if (!self || !self->mFollowHook) return 0;

		const bool active = camera && outCommand && sEnabled.load(std::memory_order_relaxed);
		const bool hasUnit = active && *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(camera) + 8) != -1;

		// Halo 3 / ODST: the director has just overwritten the new camera object's +0x20 with a first-person value
		// (see header comment). For the follow camera that is a scale the engine initialises to 1.0f: restore it.
		if constexpr (kIsH3Family)
		{
			if (hasUnit) *reinterpret_cast<float*>(static_cast<uint8_t*>(camera) + 0x20) = 1.0f;
		}

		const int64_t r = self->mFollowHook->getInlineHook().template call<int64_t>(camera, user, dt, outCommand);

		if (active && sDiagFollow.fetch_add(1) < kDiagMax)
		{
			const uint8_t* c = static_cast<const uint8_t*>(camera);
			PLOG_INFO << "[ChaseCamera] follow update: user " << user << " unit 0x" << std::hex << *reinterpret_cast<const uint32_t*>(c + 8) << std::dec
				<< " yaw " << *reinterpret_cast<const float*>(c + 24) << " pitch " << *reinterpret_cast<const float*>(c + 28)
				<< " scale(+0x20) " << *reinterpret_cast<const float*>(c + 32) << " cmd flags 0x" << std::hex << *reinterpret_cast<const uint32_t*>(outCommand) << std::dec
				<< " cmd distance(+0x24) " << *reinterpret_cast<const float*>(outCommand + 0x24) << " cmd offset(+0x10) "
				<< *reinterpret_cast<const float*>(outCommand + 0x10) << "," << *reinterpret_cast<const float*>(outCommand + 0x14) << "," << *reinterpret_cast<const float*>(outCommand + 0x18);
		}

		if (hasUnit)
		{
			if (sNoClip.load(std::memory_order_relaxed))
				*reinterpret_cast<uint32_t*>(outCommand) |= kObserverCommandNoCollision;

			// FOV radians at +0x28 (the observer clamps it to [1, 150] degrees on intake). 0 = leave the game's own.
			const float fovDeg = self->mFovSetting ? self->mFovSetting->GetValue() : 0.f;
			if (fovDeg > 1.f && fovDeg < 179.f)
			{
				bool fovSlotValid = true;
				if constexpr (kIsH3Family) fovSlotValid = (*reinterpret_cast<uint32_t*>(outCommand) & kH3CommandFovValid) != 0;
				if (fovSlotValid) *reinterpret_cast<float*>(outCommand + 0x28) = fovDeg * 0.017453292f;
			}

			// vertical offset: the focus point the camera orbits is command floats +4..+0xC; raising z lifts both
			// the camera and what it looks at.
			const float dz = self->mHeightSetting ? self->mHeightSetting->GetValue() : 0.f;
			if (dz != 0.f && std::isfinite(dz)) *reinterpret_cast<float*>(outCommand + 0x0C) += dz;
		}
		return r;
	}

	// ------------------------------------------------------------------------------------------------------------

	void say(const std::string& msg)
	{
		PLOG_INFO << msg;
		try { if (auto m = messagesWeak.lock()) m->addMessage(msg); }
		catch (...) {}
	}

	// Attach the follow hook. Caller holds mArmMutex. Returns false if it could not be attached (and records why).
	bool armFollowLocked(uintptr_t addr)
	{
		if (mFollowHook->isHookInstalled()) return true;
		if (!entryBytesMatch(addr, followEntryBytes()))
		{
			PLOG_ERROR << "[ChaseCamera] " << mGame.toString() << ": following-camera update prologue differs (another build, or already hooked by another tool)";
			return false;
		}
		mFollowHook->setWantsToBeAttached(true);
		if (!mFollowHook->isHookInstalled())
		{
			PLOG_ERROR << "[ChaseCamera] " << mGame.toString() << ": safetyhook could not install the following-camera hook";
			return false;
		}
		PLOG_INFO << "[ChaseCamera] " << mGame.toString() << ": following-camera hook armed";
		return true;
	}

	enum class ArmResult { Ok, NotLoaded, Refused };

	// Attach the hooks once the game's module is present and both prologues are the ones the RE was done on.
	// A module that isn't loaded yet is NOT a refusal - the next MCC Ingame state retries.
	ArmResult ensureArmed()
	{
		std::scoped_lock lk(mArmMutex);
		if (mRefused) return ArmResult::Refused;

		const bool followDone = !mFollowHook || mFollowUnavailable || mFollowHook->isHookInstalled();
		if (mSelectHook->isHookInstalled() && followDone) return ArmResult::Ok;

		uintptr_t selectAddr = 0;
		if (!mSelectFunction || !mSelectFunction->resolve(&selectAddr)) return ArmResult::NotLoaded;   // module not loaded yet

		// Follow hook first, so by the time the select hook swaps cameras the post-edit is already in place.
		if (!followDone)
		{
			uintptr_t followAddr = 0;
			if (!mFollowFunction || !mFollowFunction->resolve(&followAddr)) return ArmResult::NotLoaded;
			if (!armFollowLocked(followAddr))
			{
				if constexpr (kIsH2)
				{
					mFollowUnavailable = true;   // Halo 2: chase itself still works, no-clip / FOV / height don't
					PLOG_ERROR << "[ChaseCamera] Halo 2 no-clip / FOV / height unavailable";
				}
				else
				{
					mRefused = true;
					return ArmResult::Refused;
				}
			}
		}

		if (!mSelectHook->isHookInstalled())
		{
			if (!entryBytesMatch(selectAddr, selectEntryBytes()))
			{
				mRefused = true;
				PLOG_ERROR << "[ChaseCamera] " << mGame.toString() << ": camera select function prologue differs (another build, or already hooked by another tool) - refusing to hook";
				return ArmResult::Refused;
			}
			mSelectHook->setWantsToBeAttached(true);   // ModuleHookManager attaches it now (module is loaded) and on every re-load
			if (!mSelectHook->isHookInstalled())
			{
				mRefused = true;
				PLOG_ERROR << "[ChaseCamera] " << mGame.toString() << ": safetyhook could not install the camera select hook";
				return ArmResult::Refused;
			}
			PLOG_INFO << "[ChaseCamera] " << mGame.toString() << ": camera select hook armed";
		}
		return ArmResult::Ok;
	}

	void refuseToggle(const char* why)
	{
		sEnabled = false;
		if (auto settings = settingsWeak.lock())
		{
			// untick the checkbox without re-firing the event (same trick as UncapRenderSections / FarClipDistance)
			settings->chaseCameraToggle->GetValueDisplay() = false;
			settings->chaseCameraToggle->GetValue() = false;
		}
		say(why);
	}

	// Single place that decides sEnabled. Called on toggle and on every MCC state change.
	// The settings are shared by all games' objects, so this object only enables itself while ITS game is the one
	// being played; every other game's object just stays (or goes) inert.
	void applyState(bool announce)
	{
		auto settings = settingsWeak.lock();
		auto mcc = mccStateHookWeak.lock();
		if (!settings || !mcc) { sEnabled = false; return; }

		const bool wanted = settings->chaseCameraToggle->GetValue();
		const bool playingThisGame = mcc->isGameCurrentlyPlaying(mGame);

		if (!wanted || !playingThisGame)
		{
			// Hooks stay installed but answer honestly from now on; the director swaps back to first person itself.
			const bool was = sEnabled.exchange(false);
			if (was && announce && playingThisGame) say("Chase Camera: off");
			return;
		}

		switch (ensureArmed())
		{
		case ArmResult::Refused:
			refuseToggle("Chase Camera: unavailable - this game's DLL is not the 1.3528 build this was made for.");
			return;
		case ArmResult::NotLoaded:
			sEnabled = false;
			PLOG_DEBUG << "[ChaseCamera] " << mGame.toString() << " module not loaded yet; will arm when the game is in-game";
			return;
		case ArmResult::Ok:
			break;
		}

		sDiagSelect = 0; sDiagFollow = 0;
		const bool was = sEnabled.exchange(true);
		if (!was && announce) say("Chase Camera: on (takes effect on the next game update; unpause if paused)");
	}

public:
	ChaseCameraImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		// A missing XML entry throws HCMInitException here -> the cheat is unavailable for this game and its GUI rows grey out.
		auto chaseCameraSelectFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chaseCameraSelectFunction), game);
		mSelectFunction = chaseCameraSelectFunction;
		void* selectDetourFn = nullptr;
		void* followDetourFn = nullptr;
		if constexpr (kIsH2) { selectDetourFn = (void*)&h2PredicateDetour; followDetourFn = (void*)&h2FollowDetour; }
		else { selectDetourFn = (void*)&selectDetour; followDetourFn = (void*)&followDetour; }
		mSelectHook = ModuleInlineHook::make(mGame.toModuleName(), mSelectFunction, selectDetourFn);

		try
		{
			auto chaseCameraFollowFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(chaseCameraFollowFunction), game);
			mFollowFunction = chaseCameraFollowFunction;
			mFollowHook = ModuleInlineHook::make(mGame.toModuleName(), mFollowFunction, followDetourFn);
		}
		catch (HCMInitException& ex)
		{
			// Halo 2 works without it (chase camera only, no no-clip / FOV / height); every other game needs it
			// (it also restores the camera-object scale on Halo 3 / ODST), so there the cheat is unavailable.
			if (!kIsH2) throw;
			mFollowUnavailable = true;
			PLOG_ERROR << "[ChaseCamera] no-clip / FOV / height unavailable: " << ex.what();
		}

		auto settings = settingsWeak.lock();
		sNoClip = settings->chaseCameraNoClip->GetValue();
		mFovSetting = settings->chaseCameraFov;
		mHeightSetting = settings->chaseCameraHeight;
		mNoClipCallback = std::make_unique<ScopedCallback<ToggleEvent>>(settings->chaseCameraNoClip->valueChangedEvent,
			[](bool& on) { sNoClip = on; });
		mToggleCallback = std::make_unique<ScopedCallback<ToggleEvent>>(settings->chaseCameraToggle->valueChangedEvent,
			[this](bool&) { applyState(true); });
		mStateCallback = std::make_unique<ScopedCallback<eventpp::CallbackList<void(const MCCState&)>>>(
			dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState&) { applyState(false); });

		sInstance = this;

		// toggle was already on when this cheat was (re)built (e.g. service rebuilt after a game switch)
		if (settings->chaseCameraToggle->GetValue()) applyState(false);
	}

	~ChaseCameraImpl()
	{
		// UNSUBSCRIBE FIRST so no toggle / state event can reach a half-destroyed object.
		if (mToggleCallback) mToggleCallback->removeCallback();
		if (mStateCallback) mStateCallback->removeCallback();
		if (mNoClipCallback) mNoClipCallback->removeCallback();
		mToggleCallback.reset();
		mStateCallback.reset();
		mNoClipCallback.reset();

		// Answer honestly before the hooks go away: the director then swaps itself back to first person on its next
		// update (if the hook is still live that long).
		sEnabled = false;
		if (mSelectHook) mSelectHook->setWantsToBeAttached(false);   // restores the original bytes
		if (mFollowHook) mFollowHook->setWantsToBeAttached(false);
		mSelectHook.reset();
		mFollowHook.reset();
		// only clear if still us (every game has its own instantiation, but a rebuilt service may overlap)
		ChaseCameraImpl* expected = this;
		sInstance.compare_exchange_strong(expected, nullptr);
	}
};

ChaseCamera::ChaseCamera(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<ChaseCameraImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo3:
		pimpl = std::make_unique<ChaseCameraImpl<GameState::Value::Halo3>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<ChaseCameraImpl<GameState::Value::Halo3ODST>>(gameImpl, dicon);
		break;
	case GameState::Value::HaloReach:
		pimpl = std::make_unique<ChaseCameraImpl<GameState::Value::HaloReach>>(gameImpl, dicon);
		break;
	case GameState::Value::Halo4:
		pimpl = std::make_unique<ChaseCameraImpl<GameState::Value::Halo4>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException("ChaseCamera not impl for this game (Halo 2, 3, ODST, Reach, 4 only)");
	}
}

ChaseCamera::~ChaseCamera() { PLOG_VERBOSE << "~" << getName(); }
