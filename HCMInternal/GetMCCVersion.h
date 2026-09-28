#pragma once
#include "IGetMCCVersion.h"





class GetMCCVersion : public IGetMCCVersion
{
private:
	const VersionInfo mccVersion;
	const std::string mccVersionString;
	const MCCProcessType mccVersionType;
	const std::string mccVersionTypeString;

	// called by constructor
	VersionInfo evalVersion();
	std::string versionToString(VersionInfo in);
	MCCProcessType evalVersionType();
	std::string processToString(MCCProcessType in);

	// Per-game overrides for a mixed downpatch - see IGetMCCVersion::getGameDataVersionAsString. Written only while
	// the pointer data is parsed (App init, single-threaded, before anything reads it), so no lock.
	std::map<GameState::Value, std::string> gameDataVersions;
public:
	GetMCCVersion() : mccVersion(evalVersion()), mccVersionString(versionToString(mccVersion)), mccVersionType(evalVersionType()), mccVersionTypeString(processToString(mccVersionType)) {};
	virtual VersionInfo getMCCVersion() override { return mccVersion; };
	virtual std::string_view getMCCVersionAsString() override { return mccVersionString; };
	virtual MCCProcessType getMCCProcessType() override { return mccVersionType; };
	virtual std::string_view getMCCProcessTypeAsString() override { return mccVersionTypeString; };

	virtual std::string_view getGameDataVersionAsString(GameState game) override
	{
		auto it = gameDataVersions.find(game);
		return it == gameDataVersions.end() ? std::string_view(mccVersionString) : std::string_view(it->second);
	}
	virtual void setGameDataVersion(GameState game, std::string version) override { gameDataVersions[game] = std::move(version); }
};

