#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IUncapDropShadowsImpl { public: virtual ~IUncapDropShadowsImpl() = default; };

// Halo 2 only. Raises the baked-in caps that limit how many lightmap drop-shadows render at once.
// Stock halo2.dll caps both the per-frame caster COLLECTION and the persistent fade-MANAGER at 32 entries
// (4 categories x 1156-byte managers @ unk_181660C70, count @+0x480), and the rasterizer scratch pool at
// 256KB (which is what de-renders the sky/water when too many shadows are queued). This feature relocates
// the manager + collection buffers to larger heap allocations (via code caves that repoint the engine's
// buffer `lea`s) and raises every 32-cap to 120, plus repoints the rasterizer pool to 8MB. Fully reverted
// on disable / unload. See memory hcm-h2-shadow-uncap-port + visibility-projection-cap (sapien original).
class UncapDropShadows : public IOptionalCheat
{
private:
	std::unique_ptr<IUncapDropShadowsImpl> pimpl;

public:
	UncapDropShadows(GameState gameImpl, IDIContainer& dicon);
	~UncapDropShadows();

	std::string_view getName() override { return nameof(UncapDropShadows); }
};
