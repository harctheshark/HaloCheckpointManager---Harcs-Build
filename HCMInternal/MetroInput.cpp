#include "pch.h"
#include "MetroInput.h"
#include "safetyhook.hpp"

namespace
{
	// IDirectInputDevice8 vtable slots (A and W share the layout)
	constexpr size_t kAcquire = 7, kUnacquire = 8, kGetDeviceState = 9, kGetDeviceData = 10, kGetDeviceInfo = 15;
	constexpr HRESULT kDI_OK = 0;

	using Acquire_t = HRESULT(__stdcall*)(void* dev);
	using Unacquire_t = HRESULT(__stdcall*)(void* dev);
	using GetDeviceState_t = HRESULT(__stdcall*)(void* dev, DWORD cb, void* data);
	using GetDeviceData_t = HRESULT(__stdcall*)(void* dev, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD flags);
	using SetCursorPos_t = BOOL(__stdcall*)(int x, int y);

	std::atomic<bool> gMouseReleased{ false }, gInputBlocked{ false };
	std::once_flag gInstallOnce;

	// one hooked function (per distinct address - the A and W vtables may share code)
	struct Hooked { void* target = nullptr; safetyhook::InlineHook hook; void* original = nullptr; };
	std::vector<Hooked>* gHooks = new std::vector<Hooked>();   // leaked on purpose: trampolines must outlive everything
	std::mutex gHooksMutex;

	// original by target address (the detour does not know whether it was reached through the A or the W vtable)
	template <typename F> F originalFor(void* fnInVtable)
	{
		for (auto& h : *gHooks) if (h.target == fnInVtable) return (F)h.original;
		return nullptr;
	}

	// what kind of device: DI8DEVTYPE_MOUSE 0x12, DI8DEVTYPE_KEYBOARD 0x13 (low byte of dwDevType); cached per device
	struct DevKind { void* dev; int kind; };
	std::vector<DevKind> gKinds;
	std::mutex gKindsMutex;
	int deviceKind(void* dev)
	{
		{
			std::scoped_lock lk(gKindsMutex);
			for (const auto& k : gKinds) if (k.dev == dev) return k.kind;
		}
		// DIDEVICEINSTANCEW (1100 bytes) / A (580) both start { DWORD dwSize; GUID guidInstance; GUID guidProduct; DWORD
		// dwDevType; ... }: dwDevType at +36. Each interface accepts only its own dwSize, so try the W size, then the A size.
		int kind = 0;
		void** vt = *(void***)dev;
		using GetDeviceInfo_t = HRESULT(__stdcall*)(void*, void*);
		auto gdi = (GetDeviceInfo_t)vt[kGetDeviceInfo];
		alignas(8) unsigned char buf[1200]{};
		for (DWORD size : { (DWORD)(4 + 16 + 16 + 4 + 260 * 2 + 260 * 2 + 16 + 2 + 2), (DWORD)(4 + 16 + 16 + 4 + 260 + 260 + 16 + 2 + 2) })
		{
			memset(buf, 0, sizeof(buf));
			*(DWORD*)buf = size;
			if (SUCCEEDED(gdi(dev, buf))) { kind = (int)(*(DWORD*)(buf + 36) & 0xFF); break; }
		}
		std::scoped_lock lk(gKindsMutex);
		if (gKinds.size() < 64) gKinds.push_back({ dev, kind });
		return kind;
	}
	bool blockedDevice(void* dev)
	{
		const bool mouseFree = gMouseReleased.load(std::memory_order_relaxed), all = gInputBlocked.load(std::memory_order_relaxed);
		if (!mouseFree && !all) return false;
		const int kind = deviceKind(dev);
		if (kind == 0x12) return mouseFree || all;   // mouse
		if (kind == 0x13) return all;                // keyboard
		return false;
	}
	void unacquire(void* dev)
	{
		void** vt = *(void***)dev;
		if (auto un = originalFor<Unacquire_t>(vt[kUnacquire])) un(dev);
		else ((Unacquire_t)vt[kUnacquire])(dev);
	}

	HRESULT __stdcall hkAcquire(void* dev)
	{
		void** vt = *(void***)dev;
		auto orig = originalFor<Acquire_t>(vt[kAcquire]);
		if (blockedDevice(dev)) { unacquire(dev); return kDI_OK; }   // "acquired" - but the OS keeps the mouse
		return orig ? orig(dev) : kDI_OK;
	}

	HRESULT __stdcall hkGetDeviceState(void* dev, DWORD cb, void* data)
	{
		void** vt = *(void***)dev;
		auto orig = originalFor<GetDeviceState_t>(vt[kGetDeviceState]);
		if (blockedDevice(dev))
		{
			unacquire(dev);
			if (data && cb) memset(data, 0, cb);
			return kDI_OK;
		}
		return orig ? orig(dev, cb, data) : kDI_OK;
	}

	HRESULT __stdcall hkGetDeviceData(void* dev, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD flags)
	{
		void** vt = *(void***)dev;
		auto orig = originalFor<GetDeviceData_t>(vt[kGetDeviceData]);
		if (blockedDevice(dev))
		{
			unacquire(dev);
			if (pdwInOut) *pdwInOut = 0;
			return kDI_OK;
		}
		return orig ? orig(dev, cbObjectData, rgdod, pdwInOut, flags) : kDI_OK;
	}

	SetCursorPos_t gOrigSetCursorPos = nullptr;
	BOOL __stdcall hkSetCursorPos(int x, int y)
	{
		if (gMouseReleased.load(std::memory_order_relaxed) || gInputBlocked.load(std::memory_order_relaxed)) return TRUE;
		return gOrigSetCursorPos ? gOrigSetCursorPos(x, y) : TRUE;
	}

	void hookAt(void* target, void* detour, const char* what)
	{
		if (!target) return;
		std::scoped_lock lk(gHooksMutex);
		for (const auto& h : *gHooks) if (h.target == target) return;   // the A and W vtables share it
		auto r = safetyhook::InlineHook::create(target, detour, safetyhook::InlineHook::StartDisabled);
		if (!r) { PLOG_ERROR << "MetroInput: could not hook " << what; return; }
		Hooked h; h.target = target; h.hook = std::move(*r); h.original = h.hook.original<void*>();
		gHooks->push_back(std::move(h));
		(void)gHooks->back().hook.enable();
		PLOG_INFO << "MetroInput: hooked " << what << " @ " << target;
	}

	void install()
	{
		HMODULE di = LoadLibraryW(L"dinput8.dll");
		using DI8Create_t = HRESULT(__stdcall*)(HINSTANCE, DWORD, const GUID&, void**, IUnknown*);
		auto create = di ? (DI8Create_t)GetProcAddress(di, "DirectInput8Create") : nullptr;
		if (!create) { PLOG_ERROR << "MetroInput: no dinput8 DirectInput8Create"; return; }
		// IID_IDirectInput8W {BF798031-483A-4DA2-AA99-5D64ED369700}, IID_IDirectInput8A {BF798030-...}; GUID_SysMouse
		// {6F1D2B60-D5A0-11CF-BFC7-444553540000}
		static const GUID kIIDW = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
		static const GUID kIIDA = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
		static const GUID kSysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
		for (const GUID* iid : { &kIIDW, &kIIDA })
		{
			void* dinput = nullptr;
			if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iid, &dinput, nullptr)) || !dinput) continue;
			// IDirectInput8::CreateDevice is slot 3: (this, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN)
			using CreateDevice_t = HRESULT(__stdcall*)(void*, const GUID&, void**, IUnknown*);
			void* dev = nullptr;
			if (SUCCEEDED(((CreateDevice_t)(*(void***)dinput)[3])(dinput, kSysMouse, &dev, nullptr)) && dev)
			{
				void** vt = *(void***)dev;
				hookAt(vt[kAcquire], (void*)&hkAcquire, "IDirectInputDevice8::Acquire");
				hookAt(vt[kGetDeviceState], (void*)&hkGetDeviceState, "IDirectInputDevice8::GetDeviceState");
				hookAt(vt[kGetDeviceData], (void*)&hkGetDeviceData, "IDirectInputDevice8::GetDeviceData");
				((IUnknown*)dev)->Release();
			}
			((IUnknown*)dinput)->Release();
		}
		// SetCursorPos: published before enabling (see hookAt), kept separately for the detour
		if (void* scp = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetCursorPos"))
		{
			auto r = safetyhook::InlineHook::create(scp, (void*)&hkSetCursorPos, safetyhook::InlineHook::StartDisabled);
			if (r)
			{
				std::scoped_lock lk(gHooksMutex);
				Hooked h; h.target = scp; h.hook = std::move(*r); h.original = h.hook.original<void*>();
				gOrigSetCursorPos = (SetCursorPos_t)h.original;
				gHooks->push_back(std::move(h));
				(void)gHooks->back().hook.enable();
				PLOG_INFO << "MetroInput: hooked SetCursorPos";
			}
		}
	}
}

namespace MetroInput
{
	void setMouseReleased(bool released)
	{
		std::call_once(gInstallOnce, install);
		gMouseReleased = released;
		PLOG_INFO << "MetroInput: mouse " << (released ? "released to HCM" : "back to the game");
	}
	void setInputBlocked(bool blocked)
	{
		std::call_once(gInstallOnce, install);
		gInputBlocked = blocked;
		PLOG_INFO << "MetroInput: game input " << (blocked ? "blocked" : "unblocked");
	}
}
