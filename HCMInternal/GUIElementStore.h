#pragma once
#include "IGUIElement.h"
#include "GuiElementEnum.h"
// only TOPLEVEL gui elements go in here. elements nested inside another element do not.
class GUIElementStore {
private:

	// filled up by guiConstructor later



	std::map<GameState, std::vector<std::shared_ptr<IGUIElement>>> allTopLevelGUIElements
	{
	{GameState::Value::Halo1, {}},
	{GameState::Value::Halo2, {}},
	{GameState::Value::Halo2MP, {}},   // was MISSING: entering Halo 2 Anniversary MP threw std::out_of_range and killed MCC
	{GameState::Value::Halo3, {}},
	{GameState::Value::Halo3ODST, {}},
	{GameState::Value::HaloReach, {}},
	{GameState::Value::Halo4, {}},
	{GameState::Value::HaloCER, {}},
	{GameState::Value::Halo5Forge, {}},
	{GameState::Value::NoGame, {}},
	};

	friend class GUIElementConstructor;
	std::map<GameState, std::vector<std::shared_ptr<IGUIElement>>>& getTopLevelGUIElementsMutable() { return allTopLevelGUIElements; };
	
public:
	// ⚠ Called from HCMInternalGUI::onGameStateChange on the RENDER thread, where a throw is unhandled and takes the
	// game down (it did, for every Halo 2 Anniversary MP session, until Halo2MP was added above). A game missing from
	// the map gets NoGame's empty list instead - an empty menu, never a crash.
	const std::vector<std::shared_ptr<IGUIElement>>& getTopLevelGUIElements(GameState game)
	{
		auto found = allTopLevelGUIElements.find(game);
		return found != allTopLevelGUIElements.end() ? found->second : allTopLevelGUIElements.at(GameState::Value::NoGame);
	}

	std::map<GameState, std::set<GUIElementEnum>> mapOfSuccessfullyConstructedGUIElements // used in unit testing
	{
		{GameState::Value::Halo1, std::set<GUIElementEnum>{}},
		{ GameState::Value::Halo2, std::set<GUIElementEnum>{} },
		{ GameState::Value::Halo2MP, std::set<GUIElementEnum>{} },
		{ GameState::Value::Halo3, std::set<GUIElementEnum>{} },
		{ GameState::Value::Halo3ODST, std::set<GUIElementEnum>{} },
		{ GameState::Value::HaloReach, std::set<GUIElementEnum>{} },
		{ GameState::Value::Halo4, std::set<GUIElementEnum>{} },
		{ GameState::Value::HaloCER, std::set<GUIElementEnum>{} },
		{ GameState::Value::Halo5Forge, std::set<GUIElementEnum>{} },
		{ GameState::Value::NoGame, std::set<GUIElementEnum>{} },
	};

};