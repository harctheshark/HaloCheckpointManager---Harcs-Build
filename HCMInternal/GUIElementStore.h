#pragma once
#include "IGUIElement.h"
#include "GuiElementEnum.h"

// One node of the per-game element tree: which GUIElementEnum built this element, the element itself, and the
// elements that were constructed while it was being constructed (its nested children, in construction order).
// Filled by GUIElementConstructor at the single point every element passes through, so it needs no per-widget
// code and no hand-maintained table. Read-only after construction. Used by the new layout (HCMPaletteGUI) for
// search, favourites and browse; the classic layout does not look at it.
struct GUIElementNode
{
	GUIElementEnum id;
	std::shared_ptr<IGUIElement> element;
	std::vector<GUIElementNode> children;
};

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

	// per game: the same top level elements as above, in the same order, but as trees that also name every nested
	// element. operator[] on purpose: a game missing here just gets an empty tree.
	std::map<GameState, std::vector<GUIElementNode>> allElementTrees;
	const std::vector<GUIElementNode> emptyElementTree;

	friend class GUIElementConstructor;
	std::map<GameState, std::vector<std::shared_ptr<IGUIElement>>>& getTopLevelGUIElementsMutable() { return allTopLevelGUIElements; };
	std::map<GameState, std::vector<GUIElementNode>>& getElementTreesMutable() { return allElementTrees; };

public:
	// ⚠ Called from HCMInternalGUI::onGameStateChange on the RENDER thread, where a throw is unhandled and takes the
	// game down (it did, for every Halo 2 Anniversary MP session, until Halo2MP was added above). A game missing from
	// the map gets NoGame's empty list instead - an empty menu, never a crash.
	const std::vector<std::shared_ptr<IGUIElement>>& getTopLevelGUIElements(GameState game)
	{
		auto found = allTopLevelGUIElements.find(game);
		return found != allTopLevelGUIElements.end() ? found->second : allTopLevelGUIElements.at(GameState::Value::NoGame);
	}

	// Never throws: a game with no tree gets an empty one (the new layout then shows an empty menu).
	const std::vector<GUIElementNode>& getElementTree(GameState game)
	{
		auto found = allElementTrees.find(game);
		return found != allElementTrees.end() ? found->second : emptyElementTree;
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
