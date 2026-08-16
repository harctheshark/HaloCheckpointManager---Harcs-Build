#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"

class HideWatermarkCheck : public IOptionalCheat
{
private:



public:
	// Was gated on nameof(dxgiInternalPresentFunction) - a hard-coded OBS RVA that has been deleted
	// (see OBSHookDiscovery.h / OBSBypassCheck.h). The watermark has nothing to do with OBS anyway;
	// keeping the requirement would just hide the toggle. Class kept because GUIRequiredServices names
	// it as the required service for GUIElementEnum::HideWatermarkGUI.
	HideWatermarkCheck(GameState gameImpl, IDIContainer& dicon)
	{
	}


	std::string_view getName() override { return nameof(HideWatermarkCheck); }

};