#pragma once
#include "ModuleHook.h"



// Singleton that works with ModuleCache to manage ModuleHooks,
// attaching and detaching hooks as necessary when modules load/unload)
class ModuleHookManager
{
private:
	friend class ModuleInlineHook;
	friend class ModuleMidHook;
	friend class ModulePatch;


	// Where we keep all the module relative hooks (key = name of module, value = list of all hooks for that module).
	static inline std::unordered_map	<std::wstring, std::vector
		<
		ModuleHookBase*
		>
	> mModuleHooksMap;


	// Hooks on module load/unload functions.
	// These are attached on ModuleHookManager construction
	// These are automagically cleaned up (detached) on ModuleHookManager destruction
	static inline safetyhook::InlineHook mHook_LoadLibraryA;
	static inline safetyhook::InlineHook mHook_LoadLibraryW;
	static inline safetyhook::InlineHook mHook_LoadLibraryExA;
	static inline safetyhook::InlineHook mHook_LoadLibraryExW;
	static inline safetyhook::InlineHook mHook_FreeLibrary;

	// ⚠ The first bytes at each kernel32 site as WE left them, captured right after install. Compared at
	// teardown so we never restore over another injector's hook - the same rule the dxgi sites follow.
	// static inline: these outlive a session, exactly like the hooks they describe.
	static constexpr size_t kLoaderSiteBytes = 16;
	static inline uint8_t mLoaderSiteSnapshot[5][kLoaderSiteBytes]{};
	static inline void* mLoaderSiteAddress[5]{};
	static inline bool mLoaderSitesSnapshotted = false;

	// So we can process the hooks we need to attach/detach of the loading/unloading library
	static HMODULE newLoadLibraryA(LPCSTR lpLibFileName);
	static HMODULE newLoadLibraryW(LPCWSTR lpLibFileName);
	static HMODULE newLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
	static HMODULE newLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
	static BOOL newFreeLibrary(HMODULE hLibModule);


	static void preModuleUnload_UpdateHooks(std::wstring_view libFilename); // called by newFreeLibrary
	static void postModuleLoad_UpdateHooks(std::wstring_view libPath); // called by newLoadLibraries

	static void detachAllHooks();

	static void addHook(const std::wstring& moduleName, ModuleHookBase* newHook)
	{
		// Note: [] operator creates the moduleName key if it didn't already exist in the map
		mModuleHooksMap[moduleName].emplace_back(newHook);
	}


	static void removeHook(const std::wstring& moduleName, ModuleHookBase* hookToRemove)
	{
		if (mModuleHooksMap.contains(moduleName))
		{
			auto position = std::ranges::find(mModuleHooksMap[moduleName], hookToRemove);

			if (position == mModuleHooksMap[moduleName].end())
				return;

			mModuleHooksMap[moduleName].erase(position);
		}
	}

public:

	ModuleHookManager();

	~ModuleHookManager();





};


