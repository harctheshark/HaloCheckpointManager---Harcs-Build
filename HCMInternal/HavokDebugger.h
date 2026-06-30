#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// Halo 3 only. Toggles the in-process Havok Visual Debugger server (ported from the
// standalone vdbinject_h3.dll). On enable it serves live collision/broadphase/island
// geometry on port 25001 for the Havok Visual Debugger client to connect to; on disable
// it freezes the gather. See HavokDebuggerImpl.cpp / HavokDebuggerBridge.h.
class HavokDebuggerImplUntemplated { public: virtual ~HavokDebuggerImplUntemplated() = default; };

class HavokDebugger : public IOptionalCheat
{
private:
	std::unique_ptr<HavokDebuggerImplUntemplated> pimpl;

public:
	HavokDebugger(GameState gameImpl, IDIContainer& dicon);
	~HavokDebugger();

	std::string_view getName() override { return nameof(HavokDebugger); }
};
