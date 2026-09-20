#pragma once
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"



class GetCurrentRNG : public IOptionalCheat
{
private:
	class GetCurrentRNGImpl;
	std::unique_ptr<GetCurrentRNGImpl> pimpl;

public:
	GetCurrentRNG(GameState game, IDIContainer& dicon);
	~GetCurrentRNG();

	DWORD getCurrentRNG();

	// The seed as it was at the START of the current level - "what did the scenario begin with".
	//
	// ⚠ MUST BE CALLED EVERY GAME TICK to work, because it latches on the tick it is told about. It is the
	// caller's tick that decides everything: game tick 0 is the level start, so that is when the value is
	// captured, and the tick counter returning to 0 is what arms it again for the next level.
	//
	// ⚠ A CHECKPOINT REVERT DELIBERATELY DOES NOT RE-LATCH. Reverting also moves the tick counter backwards
	// and also restores the seed, so re-latching there would quietly replace the level's starting seed with
	// the checkpoint's. Only a return to tick 0 counts as a new level.
	//
	// Throws while there is no level running. getLevelLoadRNGTick() reports which tick the value was
	// actually captured on - normally 0, but if HCM attached mid-level, or the game state was not readable
	// on tick 0, it will be higher and the display should say so rather than imply it is the true start.
	DWORD getLevelLoadRNG(uint32_t currentGameTick);
	uint32_t getLevelLoadRNGTick();

	virtual std::string_view getName() override { return nameof(GetCurrentRNG); }
};

