#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved (HaloCER) ONLY - the UE5 RENDER camera.
//
// This is the extraction of the camera half of HCETriggerOverlay.cpp. NOTHING about the maths or the offsets has
// been changed; the code was moved so that TWO consumers (the D3D12 3D renderer and the trigger overlay's ImGui
// fallback path) can share ONE midhook. Installing the same midhook twice on APlayerCameraManager::DoUpdateCamera
// is not something to find out about at runtime.
//
// WHERE THE NUMBERS COME FROM (verbatim from the original file header, do not re-derive):
//
//   * APlayerCameraManager::DoUpdateCamera (HaloCampaignEvolved.exe RVA 0x64B3B60) is midhooked ONLY to capture
//     `this` (RCX at entry, before the prologue moves anything). Nothing about the camera update is modified.
//   * From that pointer the POV is a plain read: Location at PCM+0x14B0 (3 x double, UE centimetres), Rotation at
//     PCM+0x14C8 (3 x double, DEGREES: Pitch, Yaw, Roll), FOV at PCM+0x14E0 (float, HORIZONTAL degrees). The FOV
//     offset is the anchor stored in InternalPointerData.xml (region hceCameraManager); the other two are derived
//     from it so all three stay in lockstep if the struct ever moves.
//   * NOT FMinimalViewInfo::operator= (RVA 0x5AA7BB0), which the external CER tool hooks: its first 238 bytes are
//     DUPLICATED at RVA 0x7ECC950, so a byte signature matches twice. DoUpdateCamera has no twin.
//   * The FOV is HORIZONTAL. FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle uses
//     XAxisMultiplier = 1.0 under MaintainXFOV (the live default), so focal = width / (2*tan(FOV/2)) is the
//     engine's own projection, not an approximation. Anything building a projection MATRIX from it must convert:
//     vfov = 2*atan(tan(hfov/2) * height/width).
//
// COORDINATE FRAME - READ BEFORE TOUCHING ueRotationToBlamBasis()
// --------------------------------------------------------------
// Everything this class returns is in the BLAM frame: right-handed, Z up, +X forward at yaw 0, +Y LEFT. UE5 is
// left-handed with +Y RIGHT, and 1 Blam world unit = 304.8 UE centimetres. UE -> Blam is therefore a single Y
// negation, plus a /304.8 for positions. Trigger volume tag data, the player position and the sim's view angle
// are ALL already in the Blam frame, so keeping the camera there means no geometry ever needs converting.
//
// Applying that negation to UE's right vector (-sinYaw, cosYaw, 0), with yaw_ue == -yaw_blam, yields
// (sin(yaw_blam), -cos(yaw_blam), 0) - which is `forward x UnitZ`, and is what HCEGetPlayerState::lookRelativeOffset
// builds for the in-game-verified Teleport/Launch relative modes. Do NOT "simplify" by dropping the negation and
// using the external CER tool's (-sin, cos, 0) literally: that is Blam's LEFT, and it mirrors the whole overlay
// horizontally. That was a shipped bug once already.
// ================================================================================================================
class HCEGetCameraData : public IOptionalCheat
{
private:
	class HCEGetCameraDataImpl;
	std::unique_ptr<HCEGetCameraDataImpl> pimpl;

public:
	// 1 Blam world unit == 304.8 UE centimetres. (A HaloCER world unit is 10 feet.)
	static constexpr float kUeCmPerWorldUnit = 304.8f;

	// The whole render camera, straight out of the one UE POV struct. Position is already converted into the
	// Blam frame; rotation is left in UE degrees because that is what ueRotationToBlamBasis wants.
	struct UeCamera
	{
		SimpleMath::Vector3 positionBlam{};
		float pitchDegrees = 0.f;
		float yawDegrees = 0.f;
		float rollDegrees = 0.f;
		float horizontalFovDegrees = 0.f;
	};

	HCEGetCameraData(GameState game, IDIContainer& dicon);
	~HCEGetCameraData();
	std::string_view getName() override { return nameof(HCEGetCameraData); }

	// Attach/detach the DoUpdateCamera midhook. REFERENCE COUNTED: every setHookWanted(true) must be matched by
	// a setHookWanted(false) from the same caller, so two consumers cannot rip the hook out from under each other.
	// Throws HCMRuntimeException if the game build's bytes at the hook site do not match what the pointer data
	// says they should be (i.e. Halo Campaign Evolved updated) - the caller decides whether that is fatal.
	// Safe to call from any thread; NOT called from the render thread.
	void setHookWanted(const void* requester, bool wanted);

	// The last camera DoUpdateCamera reported, validated for finiteness and plausible range.
	// NEVER THROWS. Returns false when the hook has not fired yet, when a level transition dropped the captured
	// pointer, or when the read looked implausible - callers should fall back to something else, or skip.
	//
	// Failure is reported through a LATCHED, per-REASON diagnostic (see the .cpp). That granularity is the whole
	// point: "the midhook never fires" and "the midhook fires but the POV fails its range check" are completely
	// different faults that a single log-once cannot tell apart - and a log-once spent on frame 6, before the
	// hook has had a chance to fire, tells you nothing at all. That ambiguity cost a whole debugging round.
	bool getUeCamera(UeCamera& out) const;

	// How many times DoUpdateCamera's midhook callback has actually RUN. Zero means the callback is not being
	// reached at all (not installed, detached behind our back, or that address is not executed on this build) -
	// which is a completely different fault from "it runs but the POV read is rejected". Never throws.
	uint64_t getHookFireCount() const;

	// Whether the midhook is currently installed, as opposed to merely wanted.
	bool isHookInstalled() const;

	// Re-verify and re-attach the midhook if it is WANTED but not currently INSTALLED - i.e. self-heal after
	// anything detached it behind our back. No-op when nothing wants it or when it is already live.
	// MUST NOT be called from the render thread: it can install a hook. Never throws; failures are logged.
	void ensureHookLive();

	// The last FOV that read back as plausible, seeded with a sane default so a consumer still has something
	// usable on the very first frames (and while falling back to the sim's player camera, which has no FOV).
	float getLastGoodHorizontalFov() const;

	// ---- the elected camera OBJECT, for consumers that need to WRITE it -------------------------------------
	//
	// Everything above deliberately hands out VALUES captured at assignment time, because reading a latched
	// pointer on a frame of our choosing is what made the overlay flicker (see the .cpp). A writer has no such
	// option: it must address the live object. These two exist for exactly that, and for nothing else.
	//
	// The elected destination IS the FMinimalViewInfo that the engine assigns every frame - which is the POV
	// living INSIDE its owning APlayerCameraManager at PCM + kPovInCameraManagerOffset. So the owner is that
	// pointer minus the offset. (This is also why hceCameraManagerPovFovOffset is 0x30 - it is FMinimalViewInfo's
	// own FOV offset, and PCM + 0x14B0 + 0x30 == PCM + 0x14E0, the POV FOV named in the header above.)
	//
	// ⚠ DO NOT WRITE PCM + 0x14E0 (POV.FOV). The engine rebuilds the POV on the stack and copies over it every
	// frame, so a write there survives less than one frame. That was tested and rejected. The field that STICKS
	// is APlayerCameraManager::LockedFOV - see HCEFieldOfView.
	static constexpr int64_t kPovInCameraManagerOffset = 0x14B0;

	// The FMinimalViewInfo the election currently believes is the render camera's POV. 0 = nothing elected yet
	// (the midhook has not fired, or it was dropped on a level transition). NEVER THROWS.
	uintptr_t getElectedPovAddress() const;

	// The APlayerCameraManager that owns the elected POV, i.e. getElectedPovAddress() - kPovInCameraManagerOffset.
	// 0 when nothing has been elected. NEVER THROWS - and never dereferences anything, so a caller still has to
	// treat the result as untrusted memory. Requires the midhook to be WANTED by somebody.
	uintptr_t getPlayerCameraManager() const;

	// ---- pure maths, no game memory. Static so the two consumers cannot drift apart. ----

	// UE Pitch/Yaw/Roll (DEGREES) -> a Blam-frame orthonormal basis. Roll is applied about the forward axis
	// (the external CER tool reads roll and then silently ignores it; that is a bug there, not here).
	static void ueRotationToBlamBasis(float pitchDegrees, float yawDegrees, float rollDegrees,
		SimpleMath::Vector3& outForward, SimpleMath::Vector3& outRight, SimpleMath::Vector3& outUp);

	// The sim's s_player_control (yaw, pitch) in RADIANS -> the same Blam-frame basis. This is the PLAYER'S AIM,
	// not the render camera: it drifts in a vehicle, a cutscene, the death cam or HCM's own Freecam. It exists
	// only as a fallback for the frames before DoUpdateCamera has run. Roll is always 0 for the player.
	static void simViewAngleToBlamBasis(const SimpleMath::Vector2& viewAngle,
		SimpleMath::Vector3& outForward, SimpleMath::Vector3& outRight, SimpleMath::Vector3& outUp);
};
