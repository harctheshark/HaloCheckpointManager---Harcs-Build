#pragma once
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
	virtual ~IGetMCCVersion() = default;
};