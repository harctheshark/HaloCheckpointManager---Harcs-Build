#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IH2ShadowResolutionImpl { public: virtual ~IH2ShadowResolutionImpl() = default; };

// Halo 2 only. Ports the Cartographer/xlive "shadow resolution" setting to MCC halo2.dll.
//
// In the DX11 renderer the whole dynamic (projected-light) shadow pipeline is sized off ONE global
// (dword_180E13490): target_shadow_buffer = res x res, target_shadow / scratch = res>>2, alias = res>>1,
// and the shadow-buffer texel math samples 1/res - so bumping that single value is fully self-consistent.
// The value is written by the detail-preset function sub_1807E1430 (three branches: 256/512/1024). We patch
// those three imm32 writes to the chosen square resolution (or restore stock for "Retail"). The engine
// re-derives the value + rebuilds the shadow render-targets on the next resolution/display-mode change or
// game restart, so that's when the change becomes visible. Fully reverted on Retail / disable / unload.
// See memory h2-shadow-buffer-resolution (sapien original F69654 / sub_800E40).
class H2ShadowResolution : public IOptionalCheat
{
private:
	std::unique_ptr<IH2ShadowResolutionImpl> pimpl;

public:
	H2ShadowResolution(GameState gameImpl, IDIContainer& dicon);
	~H2ShadowResolution();

	std::string_view getName() override { return nameof(H2ShadowResolution); }
};
