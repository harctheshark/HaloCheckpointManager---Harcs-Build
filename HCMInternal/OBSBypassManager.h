#pragma once
#include "D3D11Hook.h"
#include "D3D12Hook.h"
#include "Lapua.h"
#include "RuntimeExceptionHandler.h"
#include "BinarySetting.h"
#include "IMessagesGUI.h"
#include "OBSHookDiscovery.h"


class OBSBypassManager
{
private:

	// injected services
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	// Needed for the one outcome that is NOT an error and must NOT reset the toggle: "OBS isn't
	// running yet". runtimeExceptions can only say "An error occured", which that is not.
	std::shared_ptr<IMessagesGUI> messagesGUI;
	// EXACTLY ONE of these is ever non-empty - whichever graphics hook App.h built for this process.
	// mIsD3D12 (not weak_ptr::expired()) is what selects the branch: expired() would also be true for
	// a D3D12 hook that has already been destroyed, which would silently fall through to the D3D11
	// branch and report the wrong error.
	const bool mIsD3D12;
	std::weak_ptr<D3D11Hook> d3d11HookWeak;
	std::weak_ptr<D3D12Hook> d3d12HookWeak;

	ScopedCallback<ToggleEvent> OBSBypassToggleEventCallback;
	std::shared_ptr<BinarySetting<bool>> OBSBypassToggle;

	void onOBSBypassToggleEvent(bool& newValue)
	{
		PLOGV << "onOBSBypassToggleEvent firing, newValue: " << newValue;
		try
		{
			OBSBypassResult result = OBSBypassResult::Removed;

			// Halo Campaign Evolved / D3D12 takes the SAME path as MCC now. OBS has no separate D3D12
			// hook to defeat - it picks d3d11/d3d10/d3d12_capture behind one shared Present hook - so
			// D3D12Hook::setOBSBypass does the same job against Present AND Present1.
			//
			// ⚠ The Lapua checks stay D3D11-ONLY. They gate Renderer2D (the watermark), which is
			// DirectXTK SpriteBatch, i.e. hard D3D11, and is never rendered on the D3D12 path. Running
			// them here would report a "Lapua service failure" for a service that is not involved.
			if (mIsD3D12)
			{
				lockOrThrow(d3d12HookWeak, d3d12Hook);
				result = d3d12Hook->setOBSBypass(newValue);
			}
			else
			{
				lockOrThrow(d3d11HookWeak, d3d11Hook);

				if (newValue)
				{
					// todo fix this up
					if (Lapua::lapuaGood == false) throw HCMRuntimeException("Lapua service one failure");
					if (Lapua::lapuaGood2 == false) throw HCMRuntimeException("Lapua service two failure");
					result = d3d11Hook->setOBSBypass(true);
				}
				else
				{
					result = d3d11Hook->setOBSBypass(false);
				}
			}

			// NOT an error, and the toggle deliberately stays ON: the hook is registered with
			// ModuleHookManager and attaches by itself the moment OBS's Game Capture injects.
			if (result == OBSBypassResult::DeferredModuleNotLoaded && messagesGUI)
			{
				messagesGUI->addMessage("OBS Game Capture not detected - the bypass will engage automatically when you start a Game Capture source.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);

			// ⚠ DO NOT call UpdateValueWithInput() here. This handler is ALREADY running inside it: it takes
			// std::lock_guard(updateValueWithInputMutex) at BinarySetting.h:35 and fires valueChangedEvent at :41
			// while still holding it. Re-entering on the same thread re-locks a non-recursive std::mutex, which
			// MSVC turns into a thrown std::system_error. Nothing on this path catches that, and we are on a
			// detached thread (GUISimpleToggle.h spawns one per click), so it escapes to std::terminate - HCM
			// installs no set_terminate. Writing both fields directly is equivalent here and cannot re-enter.
			//
			// Making updateValueWithInputMutex recursive is NOT the fix: every setting in the program shares
			// that class, so it would change locking semantics for all of MCC.
			OBSBypassToggle->GetValueDisplay() = false;
			OBSBypassToggle->GetValue() = false;
		}

	}


public:
	OBSBypassManager(std::weak_ptr<D3D11Hook> d3d11Hook, std::shared_ptr<BinarySetting<bool>> toggle, std::shared_ptr<RuntimeExceptionHandler> exp, std::shared_ptr<IMessagesGUI> mes)
		:
		mIsD3D12(false),
		d3d11HookWeak(d3d11Hook),
		OBSBypassToggle(toggle),
		runtimeExceptions(exp),
		messagesGUI(mes),
		OBSBypassToggleEventCallback(toggle->valueChangedEvent, [this](bool& n) { onOBSBypassToggleEvent(n); })
	{
		// The deferred re-attach runs on the LoadLibrary detour's thread, inside
		// ModuleInlineHook::attach(), with no service graph in reach and no way to throw. This is how
		// it reaches the user. See OBSHookDiscovery.h.
		setOBSDiscoveryMessageSink(mes);
	}

	// Halo Campaign Evolved / D3D12 overload. Exists so App.h can build the service graph identically
	// on both games. It now does the same real work as the D3D11 one - see D3D12Hook::setOBSBypass.
	OBSBypassManager(std::weak_ptr<D3D12Hook> d3d12Hook, std::shared_ptr<BinarySetting<bool>> toggle, std::shared_ptr<RuntimeExceptionHandler> exp, std::shared_ptr<IMessagesGUI> mes)
		:
		mIsD3D12(true),
		d3d12HookWeak(d3d12Hook),
		OBSBypassToggle(toggle),
		runtimeExceptions(exp),
		messagesGUI(mes),
		OBSBypassToggleEventCallback(toggle->valueChangedEvent, [this](bool& n) { onOBSBypassToggleEvent(n); })
	{
		setOBSDiscoveryMessageSink(mes);
	}

};
