#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"

class OBSBypassCheck : public IOptionalCheat
{
private:



public:
	// This used to resolve nameof(dxgiInternalPresentFunction) out of PointerDataStore purely as an
	// "is this build supported" gate. That datum no longer exists: OBS's Present hook is now located
	// at runtime by shape (see OBSHookDiscovery.h) rather than by a hard-coded RVA that rots on every
	// OBS release, so there is nothing version-specific left to check here. Requiring the old entry
	// would now HIDE the toggle on every game. The class is kept because GUIRequiredServices names it
	// as the required service for GUIElementEnum::OBSBypassToggleGUI.
	OBSBypassCheck(GameState gameImpl, IDIContainer& dicon)
	{
	}
	

	std::string_view getName() override { return nameof(OBSBypassCheck); }

};