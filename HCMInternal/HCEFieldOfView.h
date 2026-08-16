#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// ================================================================================================================
// Halo Campaign Evolved ONLY - FIELD OF VIEW, via APlayerCameraManager::LockedFOV.
//
// WHERE THE NUMBER COMES FROM (established, do not re-derive):
//
//   * LockedFOV is a float at PCM + 0x2F4, in HORIZONTAL DEGREES.
//   * GetFOVAngle (HaloCampaignEvolved.exe rva 0x64B5F00) reads [rcx + 0x2F4] and only falls through to the view
//     target's own FOV when that value is <= 0. SetFOV (0x64B5F30) writes it; UnlockFOV (0x64B5F40) zeroes it.
//     So a positive float LOCKS the FOV and 0.0f hands control back - which is the whole feature. No hook, no
//     patch, no per-frame writer.
//   * The APlayerCameraManager comes from HCEGetCameraData::getPlayerCameraManager(), i.e. the elected POV
//     destination minus HCEGetCameraData::kPovInCameraManagerOffset. That class already owns the DoUpdateCamera
//     midhook and the election that decides WHICH camera is the render camera; this one just borrows the answer.
//
// ⚠ DO NOT WRITE PCM + 0x14E0 (POV.FOV). That field is real and is what the overlays READ, but the engine
// rebuilds the POV on the stack and copies over it every frame, so a write there survives less than one frame.
// It was tested and rejected. LockedFOV is upstream of that rebuild, which is why it sticks.
//
// ⚠ MUST BE UNLOCKED ON TEARDOWN. A non-zero LockedFOV outlives HCM - the user would be left with a permanently
// wrong FOV and no way to connect it to a tool that is no longer running. The destructor writes 0.0f, and so does
// turning the toggle off.
//
// WHY THERE IS A THROTTLED PASS AT ALL, given the write is one-shot: the APlayerCameraManager is a per-level
// object. A level transition (or any camera change the election follows) hands us a DIFFERENT PCM whose LockedFOV
// is 0, and the lock would silently stop working with nothing to say so. The pass is a 4 Hz READ that writes only
// when the camera manager has changed under us or the field no longer holds our value. It is not a per-frame
// writer and it is not a re-assert against the engine - nothing in the engine fights this field.
// ================================================================================================================
class HCEFieldOfView : public IOptionalCheat
{
private:
	class HCEFieldOfViewImpl;
	std::unique_ptr<HCEFieldOfViewImpl> pimpl;

public:
	HCEFieldOfView(GameState game, IDIContainer& dicon);
	~HCEFieldOfView();
	std::string_view getName() override { return nameof(HCEFieldOfView); }
};
