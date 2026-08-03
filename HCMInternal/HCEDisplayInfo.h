#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo Campaign Evolved ONLY. The HCE equivalent of DisplayPlayerInfo / "Display 2D Game Info".
//
// Deliberately NOT a HaloCER branch of DisplayPlayerInfo: that class hard-requires GameTickEventHook,
// GetObjectAddress and GetPlayerDatum (none of which have HaloCER pointer data) and composes its text through
// three templated Get*DataAsString<gameT> helpers that would each need a HaloCER specialisation. This shares the
// anchor/font/outline rendering and the "a failed read is transient, keep the last good text" policy, but sources
// its data from HCEGetPlayerState and updates on the render event instead of a game-tick hook.
class HCEDisplayInfo : public IOptionalCheat
{
private:
	class HCEDisplayInfoImpl;
	std::unique_ptr<HCEDisplayInfoImpl> pimpl;

public:
	HCEDisplayInfo(GameState game, IDIContainer& dicon);
	~HCEDisplayInfo();
	std::string_view getName() override { return nameof(HCEDisplayInfo); }
};
