#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 2 (MCC 1.3528), 2026-11-24. The user's words: "there is a function i can call with the command console called
// 'Ai_Erase_All' ... I want a button called 'Erase All Entities' that ... deletes any AI alive OR DEAD on the map ... If
// the AI is driving a vehicle, delete the vehicle its driving as well. Make sure it doesnt delete the player. Make the AI
// Erase path better than stock ... Make it safe", and "a secondary option ... 'Also Stop New AI From Spawning?' ... a
// checkbox, so if you tick it off, AI can spawn again". See H2EraseEntities.cpp.
class H2EraseEntities : public IOptionalCheat
{
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	H2EraseEntities(GameState gameImpl, IDIContainer& dicon);
	~H2EraseEntities();

	virtual std::string_view getName() override { return nameof(H2EraseEntities); }
};
