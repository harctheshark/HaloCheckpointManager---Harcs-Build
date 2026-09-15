#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 5: Forge's 2D info overlay - the direct analogue of HCEDisplayInfo.
//
// Reuses the SHARED display2DInfo* settings (toggle, font size/colour, outline, anchor corner, screen offset)
// exactly as the HaloCER overlay does, so the existing UI and hotkey drive it unchanged. The three titles can
// never coexist in one process, so there is only ever one listener on that toggle.
//
// What it shows is limited to what Halo 5 can honestly supply today: position, aim, the player datum, the
// object address, the simulation kind, and whether the resolving thread holds the object-write gate. The last
// two are Halo-5-specific and genuinely useful - "local" is what makes checkpoint/revert meaningful, and the
// gate is what makes engine object calls legal.
//
// ⚠ Position here is obj + 0x224, the PUBLISHED position. It is only rewritten while the player is moving, so
// a stationary player's readout is last-known rather than live. That is the engine's behaviour, not a bug in
// the overlay, and it is why the value can look frozen while you stand still.
class H5DisplayInfo : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H5DisplayInfo(GameState game, IDIContainer& dicon);
	~H5DisplayInfo();
	std::string_view getName() override { return nameof(H5DisplayInfo); }
};
