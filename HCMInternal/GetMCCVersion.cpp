#include "pch.h"
#include "GetMCCVersion.h"
#include <winver.h> // to get version string of MCC
VersionInfo GetMCCVersion::evalVersion()
{


    VersionInfo outCurrentMCCVersion;

    // Halo Campaign Evolved: version comes from the SIM DLL (HaloSimulation_tag_release.dll), not the host exe -
    // the exe is the UE5 shell and its version tells us nothing about the Halo build our offsets target. The
    // "major must be 1" MCC rule below does not apply, so return early.
    if (evalVersionType() == MCCProcessType::CampaignEvolved)
    {
        wchar_t simPath[MAX_PATH]{};
        HMODULE simModule = GetModuleHandleW(L"HaloSimulation_tag_release.dll");
        if (simModule && GetModuleFileNameW(simModule, simPath, MAX_PATH))
        {
            std::wstring ws(simPath);
            std::string simPathA(ws.begin(), ws.end());
            PLOG_DEBUG << "Getting file version info of HCE sim dll at: " << simPathA;
            try
            {
                outCurrentMCCVersion = getFileVersion(simPathA.c_str());
                PLOG_DEBUG << "HCE sim dll version: " << outCurrentMCCVersion;
                return outCurrentMCCVersion;
            }
            catch (...) { PLOG_DEBUG << "HCE sim dll had no version resource; using synthetic version"; }
        }
        else
        {
            PLOG_DEBUG << "HaloSimulation_tag_release.dll not loaded yet; using synthetic version";
        }
        // Synthetic fallback so a missing/blank version resource can't block init. Must match the version
        // string used by the HaloCER entries in the pointer data XML.
        outCurrentMCCVersion.major = 0; outCurrentMCCVersion.minor = 0;
        outCurrentMCCVersion.build = 0; outCurrentMCCVersion.revision = 0;
        return outCurrentMCCVersion;
    }

    // Halo 5: Forge. The exe DOES carry a version resource, but it is the Store package's version and
    // says nothing about which engine build our RVAs target - and it moves on every Store update,
    // which would silently un-support HCM. Use a synthetic key instead, exactly as HCE does, so the
    // SupportedGameVersions gate is a deliberate decision rather than a Store versioning accident.
    // ⚠ 0.0.0.1, NOT HCE's 0.0.0.0 - two games sharing a version key would share pointer-data entries.
    if (evalVersionType() == MCCProcessType::Halo5Forge)
    {
        outCurrentMCCVersion.major = 0; outCurrentMCCVersion.minor = 0;
        outCurrentMCCVersion.build = 0; outCurrentMCCVersion.revision = 1;
        PLOG_DEBUG << "Halo 5: Forge synthetic version: " << outCurrentMCCVersion;
        return outCurrentMCCVersion;
    }

    if (evalVersionType() == MCCProcessType::WinStore)
    {
        // hard coded
        outCurrentMCCVersion.major = 1;
        outCurrentMCCVersion.minor = 3528;
        outCurrentMCCVersion.build = 0;
        outCurrentMCCVersion.revision = 0;
        return outCurrentMCCVersion;
    }



    HMODULE mccProcess = GetModuleHandle(NULL);
    char mccProcessPath[MAX_PATH];
    GetModuleFileNameA(mccProcess, mccProcessPath, sizeof(mccProcessPath));

    PLOG_DEBUG << "Getting file version info of mcc at: " << mccProcessPath;
    outCurrentMCCVersion = getFileVersion(mccProcessPath);

    PLOG_DEBUG << "mccVersionInfo: " << outCurrentMCCVersion;

    if (outCurrentMCCVersion.major != 1)
    {
        std::stringstream buf;
        buf << outCurrentMCCVersion;
        throw HCMInitException(std::format("mccVersionInfo did not start with \"1.\"! Actual read version: {}", buf.str()).c_str());
    }

    return outCurrentMCCVersion;
}

std::string GetMCCVersion::versionToString(VersionInfo in)
{
    std::stringstream ss;
    ss << in;
    return ss.str();
}

MCCProcessType GetMCCVersion::evalVersionType()
{
    std::string outCurrentMCCType;
    HMODULE mccProcess = GetModuleHandle(NULL);
    char mccProcessPath[MAX_PATH];
    GetModuleFileNameA(mccProcess, mccProcessPath, sizeof(mccProcessPath));

    std::string mccName = mccProcessPath;
    mccName = mccName.substr(mccName.find_last_of("\\") + 1, mccName.size() - mccName.find_last_of("\\") - 1);

    // checks need to ignore letter case
    if (boost::iequals(mccName, "MCCWinStore-Win64-Shipping.exe"))
    {
        PLOG_DEBUG << "setting process type to WinStore";
        return MCCProcessType::WinStore;
    }
    else if (boost::iequals(mccName, "MCC-Win64-Shipping.exe"))
    {
        PLOG_DEBUG << "setting process type to Steam";
        return MCCProcessType::Steam;
    }
    // Halo Campaign Evolved: a separate (non-MCC) UE5 title. Its Halo simulation lives in
    // HaloSimulation_tag_release.dll, which is where all our HaloCER offsets are relative to.
    else if (boost::iequals(mccName, "HaloCampaignEvolved.exe"))
    {
        PLOG_DEBUG << "setting process type to CampaignEvolved";
        return MCCProcessType::CampaignEvolved;
    }
    // Halo 5: Forge. Unlike HaloCER there is no separate simulation DLL - the whole engine is in the
    // exe, so every Halo5Forge offset is an RVA into halo5forge.exe itself.
    else if (boost::iequals(mccName, "halo5forge.exe"))
    {
        PLOG_DEBUG << "setting process type to Halo5Forge";
        return MCCProcessType::Halo5Forge;
    }
    else
    {
        throw HCMInitException(std::format("Host process had an unrecognised name!: {}", mccName));
    }
}

std::string GetMCCVersion::processToString(MCCProcessType in)
{
    switch (in)
    {
    case MCCProcessType::Steam:           return "Steam";
    case MCCProcessType::WinStore:        return "WinStore";
    case MCCProcessType::CampaignEvolved: return "CampaignEvolved";
    case MCCProcessType::Halo5Forge:      return "Halo5Forge";
    default:                              return "Unknown";
    }
}