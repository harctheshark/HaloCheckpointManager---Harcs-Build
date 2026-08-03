#pragma once
enum class MCCProcessType
{
	Steam,
	WinStore,
	CampaignEvolved,   // Halo Campaign Evolved (HaloCampaignEvolved.exe) - not MCC at all; UE5 + D3D12
};

class IGetMCCVersion
{
public:
	virtual VersionInfo getMCCVersion() = 0;
	virtual std::string_view getMCCVersionAsString() = 0;
	virtual MCCProcessType getMCCProcessType() = 0;
	virtual std::string_view getMCCProcessTypeAsString() = 0;
	virtual ~IGetMCCVersion() = default;
};