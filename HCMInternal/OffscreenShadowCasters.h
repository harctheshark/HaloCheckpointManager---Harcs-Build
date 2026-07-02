#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IOffscreenShadowCastersImpl { public: virtual ~IOffscreenShadowCastersImpl() = default; };

// Halo 2 only. Keeps instanced geometry AND objects casting projective-light shadows when they leave the
// camera frustum (turn away from a caster near a projective light and its shadow normally vanishes, while
// the BSP's shadow persists). Root cause: the per-instance/per-object bounding-sphere visibility test
// (halo2.dll sub_180709D40, the x64 analog of sapien sub_6752A0) feeds the caster's bounding-sphere radius
// to the region frustum test (sub_18079A380). PVS is position-based, so turning in place keeps the caster
// enumerated, but its sphere leaves the view frustum -> test fails -> caster dropped -> shadow gone. BSP
// casters skip this per-instance test, which is why their shadows survive. We multiply the radius (xmm2)
// by an adjustable factor at the function's entry via a code cave, so the inflated sphere stays in the
// frustum and off-camera instanced geo + objects keep casting. Only the value fed to the visibility test
// is scaled; the stored instance/object data is untouched. Build 1.3528 only; fully reverts on disable.
class OffscreenShadowCasters : public IOptionalCheat
{
private:
	std::unique_ptr<IOffscreenShadowCastersImpl> pimpl;

public:
	OffscreenShadowCasters(GameState gameImpl, IDIContainer& dicon);
	~OffscreenShadowCasters();

	std::string_view getName() override { return nameof(OffscreenShadowCasters); }
};
