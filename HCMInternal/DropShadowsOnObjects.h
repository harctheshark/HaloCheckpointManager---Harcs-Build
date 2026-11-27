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
// receive bit. We make the filter keep that bit for every object type, and make the shadow passes test an
// object's bounding sphere instead of its (sometimes tiny) dynamic light / shadow sphere, so objects such as
// Ivory Tower's hanging plant pots are found as receivers at all - only for objects whose shadow sphere is
// under about a tenth of their bounding sphere; everything else is as stock. See the .cpp.
// Takes effect live (the code runs per shadow-render frame).
class DropShadowsOnObjects : public IOptionalCheat
{
private:
	std::unique_ptr<IDropShadowsOnObjectsImpl> pimpl;

public:
	DropShadowsOnObjects(GameState gameImpl, IDIContainer& dicon);
	~DropShadowsOnObjects();

	std::string_view getName() override { return nameof(DropShadowsOnObjects); }
};
