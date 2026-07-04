#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IH2ArmorColourImpl { public: virtual ~IH2ArmorColourImpl() = default; };

// Halo 2 only. Live-recolours the local player's biped (MasterChief) via the engine's own change-color
// setter object_set_change_color (halo2.dll sub_1808D9BA0 @0x8D9BA0) - the exact call the game uses when it
// applies a player's colours (players.cpp: sub_18069DEC0 calls it 4x, indices 0..3). We drive slot 0
// (primary armour) and slot 1 (secondary/trim) from two RGB pickers, every simulation tick on the GAME
// thread (via GameTickEventHook) so the colour holds across respawns / the game's own re-apply. The setter
// writes the change-color block AND invalidates the render-colour cache, so the change is live and correct
// (a raw memory poke can't refresh that cache off-thread - see memory h2-object-change-color).
class H2ArmorColour : public IOptionalCheat
{
private:
	std::unique_ptr<IH2ArmorColourImpl> pimpl;

public:
	H2ArmorColour(GameState gameImpl, IDIContainer& dicon);
	~H2ArmorColour();

	std::string_view getName() override { return nameof(H2ArmorColour); }
};
