#pragma once
#include "pch.h"
#include "DIContainer.h"
#include "GameState.h"
#include "IGetMCCVersion.h"

// Which build of a game's DLL is HCM actually driving? Normally the MCC (exe) version, but on a MIXED downpatch
// (e.g. a 1.2094 halo3odst.dll under the 1.3528 exe) PointerDataParser::selectGameDataVersions switches that game to
// its own DLL's version. Code that carries a few hard-coded per-build numbers (TLS slot offsets and the like - things
// that are not in InternalPointerData.xml) asks here instead of assuming the newest build.
// Returns false (= "assume the current build") when the version service is unavailable.
inline bool gameDataVersionIs(IDIContainer& dicon, GameState game, std::string_view version)
{
	try
	{
		if (auto svc = dicon.Resolve<IGetMCCVersion>().lock())
			return svc->getGameDataVersionAsString(game) == version;
	}
	catch (...) {}
	return false;
}

// ODST "Season 5" halo3odst.dll (FileVersion 1.2094.0.0). Its thread-local-storage slot layout differs from 1.3528's
// (time globals 0xD0 not 0xC8, "random math" 0x580 not 0x588); the offsets inside those blocks are unchanged.
inline bool isOdst2094(IDIContainer& dicon, GameState game)
{
	return game == GameState(GameState::Value::Halo3ODST) && gameDataVersionIs(dicon, game, "1.2094.0.0");
}
