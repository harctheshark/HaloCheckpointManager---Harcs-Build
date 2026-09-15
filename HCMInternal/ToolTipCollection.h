#pragma once
#include "pch.h"
#include "GameState.h"

class ToolTipCollection
{
private:
	std::map<GameState, std::string> toolTipStrings
	{
		{GameState::Value::Halo1,""},
		{GameState::Value::Halo2,""},
		{GameState::Value::Halo2MP,""},
		{GameState::Value::Halo3,""},
		{GameState::Value::Halo4,""},
		{GameState::Value::Halo3ODST,""},
		{GameState::Value::HaloReach,""},
		{GameState::Value::HaloCER,""},   // REQUIRED: ctor does toolTipStrings.at(game) for every AllGameStateValues entry
		{GameState::Value::Halo5Forge,""},// REQUIRED, same reason - see below
		{GameState::Value::NoGame,""},
	};

public:
	ToolTipCollection(std::string sharedTooltip)
	{
		// ⚠⚠ THIS MAP MUST CONTAIN EVERY AllGameStateValues ENTRY. `.at()` throws std::out_of_range for a
		// missing key, and NOTHING here catches it - it propagates out of the gui element's constructor,
		// out of GUIElementConstructor, out of App, and terminates HCMInternal with an unhandled
		// 0xE06D7363 and a minidump that lands in KERNELBASE with no HCM frame to read.
		//
		// That is not hypothetical: adding GameState::Halo5Forge to AllGameStateValues without seeding it
		// here made EVERY gui element fail (every element builds a tooltip), which read as "HCM injects
		// and then dies during GUI construction" rather than as "one map is missing one key".
		//
		// ★ If you add a GameState, add it above. The `at()` is deliberate - a silent operator[] insert
		// would hide the mistake instead of pointing at it.
		for (auto game : AllGameStateValues)
		{
			toolTipStrings.at(game) = sharedTooltip;
		}
	}

	ToolTipCollection(std::map<GameState::Value, std::string> toolTipCollection)
	{
		// deep copy
		for (auto [game, string] : toolTipCollection)
		{
			toolTipStrings.at(game) = string;
		}
	}

	std::string_view getToolTip(GameState game) 
	{ 
		if (toolTipStrings.contains(game))
			return toolTipStrings.at(game);
	}
};