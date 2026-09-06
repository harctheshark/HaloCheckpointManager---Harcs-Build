#include "pch.h"
#include "HavokDebugger.h"
#include "HavokDebuggerBridge.h"
#include "HCEHavokDebuggerBridge.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"

template<GameState::Value mGame>
class HavokDebuggerImpl : public HavokDebuggerImplUntemplated {
private:
	ScopedCallback<ToggleEvent> mToggleCallbackHandle;
	ScopedCallback<ToggleEvent> mWorldCacheCallbackHandle;
	ScopedCallback<ActionEvent> mClearCacheCallbackHandle;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

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

				// Halo Campaign Evolved is a completely different engine (UE5 shell + HaloSimulation_tag_release.dll)
				// and has its own ported payload behind its own bridge. See HCEHavokDebuggerBridge.h for why it is
				// not just another game id.
				if constexpr (mGame == GameState::Value::HaloCER)
				{
					if (!HceHavokDebuggerBridge::start())
						throw HCMRuntimeException("Couldn't start the Havok Debugger. Either the Halo simulation dll isn't loaded yet (load a level first), or this game build moved the code the debugger locates by byte signature - check HCM_HCEHavokDebugger.log next to HCMInternal.dll.");

					const int unresolved = HceHavokDebuggerBridge::unresolvedAnchorCount();
					if (unresolved > 0)
						messagesGUI->addMessage(std::format("Havok Debugger on, but {} engine signature(s) did not resolve - some viewers will be empty. See HCM_HCEHavokDebugger.log.", unresolved));
					else
						messagesGUI->addMessage("Havok Debugger on. Connect the Havok Visual Debugger to 127.0.0.1:25001. Viewers: Object Collision, World Collision, Center of Mass, Havok Islands, Trigger Volumes, Soft Ceilings.");

					messagesGUI->addMessage("Note: the first world walk runs on the engine thread and will hitch the game for a moment.");

					// Push the current cache setting down whenever the debugger starts - the toggle can
					// have been changed (or restored from a preset) while the debugger was off.
					HceHavokDebuggerBridge::setWorldCacheEnabled(
						settingsWeak.lock()->havokWorldCacheToggle->GetValue());
				}
				else
				{
					// 0 = Halo 3 (live Havok world), 1 = Halo 2 (static world BSP from tag), 2 = Halo 3: ODST (same engine), 3 = Halo Reach (newer Havok)
					constexpr int gameId = (mGame == GameState::Value::Halo2) ? 1 : (mGame == GameState::Value::Halo3ODST) ? 2 : (mGame == GameState::Value::HaloReach) ? 3 : 0;
					if (!HavokDebuggerBridge::start(gameId))
						throw HCMRuntimeException("Couldn't start the Havok Debugger (game module not loaded).");

					messagesGUI->addMessage("Havok Debugger on. Connect the Havok Visual Debugger to 127.0.0.1:25001.");
				}
			}
			else
			{
				if constexpr (mGame == GameState::Value::HaloCER)
					HceHavokDebuggerBridge::stop();
				else
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

	// The world collision cache. Trades disk for the ~1.5-2 s engine-thread stall a world walk costs.
	void onWorldCacheChange(bool& newValue)
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			HceHavokDebuggerBridge::setWorldCacheEnabled(newValue);

			if (newValue)
			{
				// ⚠ Say the cost UP FRONT. Every BSP set the user visits gets written, so this grows
				// quietly in the background until it is large. Better a plain warning than a surprise.
				messagesGUI->addMessage("World collision cache ON. Each BSP set is saved to "
					"HCM_HavokWorldCache next to HCMInternal.dll the first time it is walked, and loaded "
					"instantly after that.");
				messagesGUI->addMessage("Disk use: roughly 1 GB per BSP set. With every level cached this "
					"can reach about 15 GB. Turn it off and use Clear World Cache to reclaim the space.");
			}
			else
			{
				int hits = 0, writes = 0;
				HceHavokDebuggerBridge::worldCacheStats(hits, writes);
				messagesGUI->addMessage(std::format(
					"World collision cache off. This session it skipped {} world walk(s) and wrote {} file(s). "
					"Cached files are kept - clear them explicitly if you want the space back.", hits, writes));
			}
		}
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	void onClearWorldCache()
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			const unsigned long long freed = HceHavokDebuggerBridge::clearWorldCache();
			messagesGUI->addMessage(freed
				? std::format("World collision cache cleared - {:.1f} MB freed.", (double)freed / 1048576.0)
				: std::string("World collision cache was already empty."));
		}
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

public:
	HavokDebuggerImpl(IDIContainer& dicon) :
		mToggleCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->havokDebuggerToggle->valueChangedEvent, [this](bool& n) { onToggleChange(n); }),
		mWorldCacheCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->havokWorldCacheToggle->valueChangedEvent, [this](bool& n) { onWorldCacheChange(n); }),
		mClearCacheCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->havokWorldCacheClearEvent, [this]() { onClearWorldCache(); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>())
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
	case GameState::Value::HaloCER:
		pimpl = std::make_unique<HavokDebuggerImpl<GameState::Value::HaloCER>>(dicon);
		break;
	default:
		throw HCMInitException("Havok Debugger supports Halo 2, Halo 3, Halo 3: ODST, Halo Reach, and Halo Campaign Evolved");
	}
}

HavokDebugger::~HavokDebugger()
{
	PLOG_VERBOSE << "~" << getName();
	// Both are safe to call unconditionally - each is a no-op when its payload was never started, and
	// the two games can never share a process anyway.
	HavokDebuggerBridge::stop();
	HceHavokDebuggerBridge::stop();
}
