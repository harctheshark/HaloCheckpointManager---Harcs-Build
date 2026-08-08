#include "pch.h"
#include "ModuleCache.h"


// Adapted from scales' MCCTAS https://github.com/Scaless/HaloTAS/blob/master/HaloTAS/libhalotas/dll_cache.cpp
// Intentionally leaked (never deleted) so late-teardown lookups don't hit a freed map - see ModuleCache.h.
std::unordered_map<std::wstring, MODULEINFO>& ModuleCache::mCache = *(new std::unordered_map<std::wstring, MODULEINFO>());

void ModuleCache::initialize()
{

	// Clear the cache
	ModuleCache::mCache.clear();

	// Fill with current values
	HMODULE hMods[1024];
	HANDLE hProcess = GetCurrentProcess();
	DWORD cbNeeded;

	if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded))
	{
		for (int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
		{
			TCHAR szModName[MAX_PATH];

			if (GetModuleBaseName(hProcess, hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) {
				MODULEINFO info;
				if (GetModuleInformation(hProcess, hMods[i], &info, sizeof(info))) {
					std::wstring name{ szModName };
					ModuleCache::mCache.emplace(name, info);
				}
			}
		}
	}
}

bool ModuleCache::addModuleToCache(const std::wstring& moduleName, HMODULE moduleHandle)
{

	// check if it's in the cache already
	if (auto it = ModuleCache::mCache.find(moduleName); it != ModuleCache::mCache.end()) {
		return true;
	}

	// if not, add to cache
	MODULEINFO info;
	if (GetModuleInformation(GetCurrentProcess(), moduleHandle, &info, sizeof(info))) {
		ModuleCache::mCache.emplace(moduleName, info);
		return true;
	}
	else
	{
		// Was silent, and the caller (the LoadLibrary detour) ignores the return - so a failure here used to
		// disappear completely and take every feature that needs this module with it. It is recoverable now,
		// because the getters backfill on miss, but it is still worth saying out loud: it means we are running
		// inside the loader before the module is fully published, and the log is the only place that shows it.
		PLOG_WARNING << "ModuleCache: GetModuleInformation failed for a module that just loaded (error "
			<< GetLastError() << "). It will be resolved lazily on first use instead.";
		return false;
	}


}

bool ModuleCache::removeModuleFromCache(const std::wstring& moduleName)
{
	if (auto it = ModuleCache::mCache.find(moduleName); it != ModuleCache::mCache.end()) {
		ModuleCache::mCache.erase(it);
		return true;
	}

	return false;
}

// ====================================================================================================================
// A CACHE MISS IS NOT PROOF THE MODULE IS ABSENT - SO ASK THE OS BEFORE GIVING UP.
//
// The cache is filled once at startup and then only by the LoadLibrary hooks. Both of those can miss a module that
// is genuinely loaded:
//
//   * addModuleToCache runs INSIDE the LoadLibrary detour, while the loader is still finishing. psapi's
//     GetModuleInformation can fail there because the module is not fully published to the PEB list yet. The
//     result was discarded, so the failure was silent and PERMANENT for the session.
//   * A module loaded by any path that is not our hooked LoadLibrary* (LdrLoadDll, a static import resolved
//     before we injected, a delay-load thunk) never reaches the cache at all.
//
// MEASURED. Starting HCM at the main menu, before HaloSimulation_tag_release.dll had loaded, produced a session
// where EVERY HaloCER feature failed with "modulecache did not contain this MLP's module" - invulnerability,
// checkpoints, the game-thread walk - even though the log shows the hook manager reacting to that exact DLL
// loading nine seconds earlier. One lost race at load time disabled the port for the whole run.
//
// So the getters now fall back to a live GetModuleHandleW / GetModuleInformation and backfill the cache on
// success. That makes every one of those paths self-healing: the miss costs one OS call, once, and then the
// module is cached exactly as if the hook had caught it.
//
// ⚠ Deliberately does NOT cache a failure. A module that is not loaded yet may load later; remembering "absent"
// would recreate the very stickiness this removes.
static bool tryBackfill(const std::wstring& moduleName, MODULEINFO& out)
{
	const HMODULE live = GetModuleHandleW(moduleName.c_str());
	if (!live) return false;
	if (!GetModuleInformation(GetCurrentProcess(), live, &out, sizeof(out))) return false;
	return true;
}

std::optional<HMODULE> ModuleCache::getModuleHandle(const std::wstring& moduleName)
{
	if (auto it = ModuleCache::mCache.find(moduleName); it != ModuleCache::mCache.end()) {
		return (HMODULE)it->second.lpBaseOfDll;
	}

	MODULEINFO info{};
	if (tryBackfill(moduleName, info))
	{
		ModuleCache::mCache.emplace(moduleName, info);
		return (HMODULE)info.lpBaseOfDll;
	}

	return std::nullopt;
}

std::optional<MODULEINFO> ModuleCache::getModuleInfo(const std::wstring& moduleName)
{
	if (auto it = ModuleCache::mCache.find(moduleName); it != ModuleCache::mCache.end()) {
		return it->second;
	}

	MODULEINFO info{};
	if (tryBackfill(moduleName, info))
	{
		ModuleCache::mCache.emplace(moduleName, info);
		return info;
	}

	return std::nullopt;
}

bool ModuleCache::isModuleInCache(const std::wstring& moduleName)
{
	if (ModuleCache::mCache.find(moduleName) != ModuleCache::mCache.end()) return true;

	MODULEINFO info{};
	if (tryBackfill(moduleName, info))
	{
		ModuleCache::mCache.emplace(moduleName, info);
		return true;
	}

	return false;
}