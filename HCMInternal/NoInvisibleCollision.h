#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 2 (MCC 1.3528): switch off the "*" invisible collision surfaces (the player-blocking walls the collision viewer draws
// red) for Havok bodies, keeping every real BSP / instanced-geometry surface solid. 2026-11-15, after the user's modded
// Cartographer xlive "Ghost faces": the same mechanism - Havok's shape-collection filter refuses those keys, so no collision
// agent is ever made for them. See NoInvisibleCollision.cpp.
class NoInvisibleCollision : public IOptionalCheat
{
private:
	class NoInvisibleCollisionImpl;
	std::unique_ptr<NoInvisibleCollisionImpl> pimpl;

public:
	NoInvisibleCollision(GameState gameImpl, IDIContainer& dicon);
	~NoInvisibleCollision();

	virtual std::string_view getName() override { return nameof(NoInvisibleCollision); }

	// true while the game's invisible (*) surfaces really are refused (toggle on, hook made for this build and attached);
	// the bounce viewer leaves them out of its seams / landings / spot scan then (any thread)
	static bool wallsOffInGame();
};
