#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IFPScaleFixImpl { public: virtual ~IFPScaleFixImpl() = default; };

// Halo 2 only. Ports the offline "Static FP Legs" fix (halo2.dll .fpfix code cave) to a live HCM toggle: the
// first-person BODY/legs render as if at a fixed reference FOV, at any FOV slider value, instead of stretching.
// Installs a runtime code cave (allocated near the module) that hooks the FP model-build count store
// (halo2.dll+0x7E5518) and laterally rescales the body node matrices about the camera, plus the "tuck" constant
// patch at +0x818999 for the body pose. Two knobs are live-adjustable: Leg Size (the cave's TAN_REF reference,
// lower = bigger legs) and Leg Tuck (body push FOV in radians). Fully reverted on disable / unload.
// See memory halo2dll-static-fp-legs (the original binary patch this ports) + build_fpfix.py.
class FPScaleFix : public IOptionalCheat
{
private:
	std::unique_ptr<IFPScaleFixImpl> pimpl;

public:
	FPScaleFix(GameState gameImpl, IDIContainer& dicon);
	~FPScaleFix();

	std::string_view getName() override { return nameof(FPScaleFix); }
};
