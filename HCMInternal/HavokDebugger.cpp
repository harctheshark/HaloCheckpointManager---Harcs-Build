#include "pch.h"
#include "HavokDebugger.h"
#include "HavokDebuggerBridge.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"

template<GameState::Value mGame>
class HavokDebuggerImpl : public HavokDebuggerImplUntemplated {
private:
	ScopedCallback<ToggleEvent> mToggleCallbackHandle;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	void onToggleChange(bool& newValue)
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (newValue)
			{
				// Toggled on outside a game (e.g. a persisted/initial "on" state evaluated at the menu)
				// is NOT an error - just don't start yet (avoids noisy error output). Re-enable in-game.
				if (!mccStateHook->isGameCurrentlyPlaying(mGame))
				{
					PLOG_DEBUG << "Havok Debugger enabled with no game playing; deferring start.";
					return;
				}

				// 0 = Halo 3 (live Havok world), 1 = Halo 2 (static world BSP from tag), 2 = Halo 3: ODST (same engine), 3 = Halo Reach (newer Havok)
				constexpr int gameId = (mGame == GameState::Value::Halo2) ? 1 : (mGame == GameState::Value::Halo3ODST) ? 2 : (mGame == GameState::Value::HaloReach) ? 3 : 0;
				if (!HavokDebuggerBridge::start(gameId))
					throw HCMRuntimeException("Couldn't start the Havok Debugger (game module not loaded).");

				messagesGUI->addMessage("Havok Debugger on. Connect the Havok Visual Debugger to 127.0.0.1:25001.");
			}
			else
			{
				HavokDebuggerBridge::stop();
				if (mccStateHook->isGameCurrentlyPlaying(mGame))
					messagesGUI->addMessage("Havok Debugger off.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	HavokDebuggerImpl(IDIContainer& dicon) :
		mToggleCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->havokDebuggerToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
	}
};

HavokDebugger::HavokDebugger(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<HavokDebuggerImpl<GameState::Value::Halo2>>(dicon);
		break;
	case GameState::Value::Halo3:
		pimpl = std::make_unique<HavokDebuggerImpl<GameState::Value::Halo3>>(dicon);
		break;
	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<HavokDebuggerImpl<GameState::Value::Halo3ODST>>(dicon);
		break;
	case GameState::Value::HaloReach:
		pimpl = std::make_unique<HavokDebuggerImpl<GameState::Value::HaloReach>>(dicon);
		break;
	default:
		throw HCMInitException("Havok Debugger supports Halo 2, Halo 3, Halo 3: ODST, and Halo Reach");
	}
}

HavokDebugger::~HavokDebugger()
{
	PLOG_VERBOSE << "~" << getName();
	HavokDebuggerBridge::stop();
}
