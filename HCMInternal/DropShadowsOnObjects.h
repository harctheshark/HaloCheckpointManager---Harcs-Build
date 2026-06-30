#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IDropShadowsOnObjectsImpl { public: virtual ~IDropShadowsOnObjectsImpl() = default; };

// Halo 2 only. Lets objects (bipeds, vehicles, scenery, crates...) RECEIVE the lightmap drop-shadow
// "blobs" cast by other objects - e.g. the player's shadow falling onto a nearby crate/couch. The engine
// fully implements this but ships it disabled: the per-caster drop-shadow pass calls the receiver-filter
// with the "object-receive shadow_mode mask" hardcoded to 0, so every non-caster object is stripped of its
// receive bit. We flip that one argument from 0 to "all shadow modes" by rewriting the 3-byte
// `xor r9d, r9d` that sets it. Takes effect live (read per shadow-render frame).
class DropShadowsOnObjects : public IOptionalCheat
{
private:
	std::unique_ptr<IDropShadowsOnObjectsImpl> pimpl;

public:
	DropShadowsOnObjects(GameState gameImpl, IDIContainer& dicon);
	~DropShadowsOnObjects();

	std::string_view getName() override { return nameof(DropShadowsOnObjects); }
};
