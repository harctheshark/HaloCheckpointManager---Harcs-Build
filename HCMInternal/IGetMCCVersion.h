#pragma once
#include "GameState.h"
enum class MCCProcessType
{
	Steam,
	WinStore,
	CampaignEvolved,   // Halo Campaign Evolved (HaloCampaignEvolved.exe) - not MCC at all; UE5 + D3D12
	// Halo 5: Forge (halo5forge.exe) - not MCC either. A UWP/Store title, so it runs in an
	// AppContainer: anything HCM shares with it (the DLL on disk, the interproc shared memory) needs
	// an ACL granting ALL APPLICATION PACKAGES (S-1-15-2-1) or the process cannot reach it.
	// Renders with D3D12, same as CampaignEvolved - see D3D12Hook.
	Halo5Forge,
};

// True for every non-MCC host that presents through D3D12 rather than D3D11. Used to pick the
// graphics hook; keep this the single place that knows.
inline bool processUsesD3D12(MCCProcessType t)
{
	return t == MCCProcessType::CampaignEvolved || t == MCCProcessType::Halo5Forge;
}

class IGetMCCVersion
{
public:
	virtual VersionInfo getMCCVersion() = 0;
	virtual std::string_view getMCCVersionAsString() = 0;
	virtual MCCProcessType getMCCProcessType() = 0;
	virtual std::string_view getMCCProcessTypeAsString() = 0;

	// The version whose pointer data applies to `game`. Normally the MCC (exe) version. It differs only for a MIXED
	// downpatch: that game's DLL on disk comes from another MCC build (e.g. a 1.3385 halo3.dll under the 1.3495 or
	// 1.3528 exe) AND the pointer data holds a complete set for the DLL's version. Chosen once, by
	// PointerDataParser::parseVersionedData, before any pointer data is handed out.
	// Not pure, so implementations that never see a mixed install (the test mock) need not care.
	virtual std::string_view getGameDataVersionAsString(GameState game) { return getMCCVersionAsString(); }
	virtual void setGameDataVersion(GameState game, std::string version) {}

	virtual ~IGetMCCVersion() = default;
};