#include "pch.h"
#include "H5SwitchZoneSet.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "IMakeOrGetCheat.h"
#include "GlobalKill.h"
#include <mutex>

// See H5SwitchZoneSet.h for where the names come from and why the switch is a data write, not an engine call.

namespace
{
	// ---- bridge state. Guarded because the GUI reads it on the render thread while the cheat refreshes. ----
	std::mutex g_mutex;
	bool g_armed = false;
	std::weak_ptr<H5GetPlayerState> g_playerState;
	std::vector<std::string> g_names;
	bool g_dirty = true;
	int g_selection = 0;

	// Caller must hold g_mutex.
	void refreshLocked()
	{
		if (!g_dirty && !g_names.empty()) return;

		auto playerState = g_playerState.lock();
		if (!playerState) { g_names.clear(); return; }

		std::vector<std::string> fresh;
		try
		{
			const int32_t count = playerState->getZoneSetCount();
			fresh.reserve((size_t)count);
			for (int32_t i = 0; i < count; ++i)
			{
				// getZoneSetName synthesises "zone set N" when the tag carries no readable name. Unlike the
				// HaloCER version we KEEP those: Halo 5 switches by index, so they are perfectly reachable.
				try { fresh.push_back(playerState->getZoneSetName(i)); }
				catch (HCMRuntimeException&) { fresh.push_back(std::format("zone set {}", i)); }
			}
		}
		catch (HCMRuntimeException&)
		{
			// No scenario loaded yet - normal at a menu or mid-load. Keep the list empty and stay dirty so
			// the next call tries again.
			g_names.clear();
			return;
		}

		g_names = std::move(fresh);
		g_dirty = false;
	}
}

namespace H5ZoneSetBridge
{
	bool isUsable()
	{
		std::scoped_lock lock(g_mutex);
		return g_armed && !g_playerState.expired();
	}

	std::vector<std::string> names()
	{
		std::scoped_lock lock(g_mutex);
		if (!g_armed) return {};
		refreshLocked();
		return g_names;
	}

	int currentIndex()
	{
		std::shared_ptr<H5GetPlayerState> playerState;
		{
			std::scoped_lock lock(g_mutex);
			if (!g_armed) return -1;
			playerState = g_playerState.lock();
		}
		if (!playerState) return -1;
		// ⚠ The COMMITTED index, read from the engine's own global - not the pending one, which is wiped to
		// -1 the moment a switch finishes. See H5GetPlayerState::getCommittedZoneSetName.
		try { return playerState->getCommittedZoneSetIndex(); }
		catch (HCMRuntimeException&) { return -1; }
	}

	void invalidate()
	{
		std::scoped_lock lock(g_mutex);
		g_dirty = true;
	}

	int selection()
	{
		std::scoped_lock lock(g_mutex);
		return g_selection;
	}

	void setSelection(int index)
	{
		std::scoped_lock lock(g_mutex);
		g_selection = index;
	}

	bool switchTo(int index, std::string& outWhy)
	{
		std::shared_ptr<H5GetPlayerState> playerState;
		{
			std::scoped_lock lock(g_mutex);
			if (!g_armed) { outWhy = "zone set service is not running"; return false; }
			refreshLocked();
			if (g_names.empty())
			{
				outWhy = "no zone sets readable - is a level loaded?";
				return false;
			}
			if (index < 0 || (size_t)index >= g_names.size())
			{
				outWhy = std::format("zone set {} is out of range (0..{})", index, g_names.size() - 1);
				return false;
			}
			playerState = g_playerState.lock();
		}
		if (!playerState) { outWhy = "zone set service is not running"; return false; }

		// ⚠ Called OUTSIDE g_mutex on purpose. The write itself can throw, and holding the bridge lock
		// across it would block the render thread's names()/currentIndex() for the duration.
		try
		{
			playerState->requestZoneSetSwitch(index);
			return true;
		}
		catch (HCMRuntimeException& ex)
		{
			outWhy = ex.what();
			return false;
		}
	}
}


class H5SwitchZoneSet::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	void onGameStateChanged(const MCCState&)
	{
		H5ZoneSetBridge::invalidate();   // a level change rebuilds the zone set table; force a re-read
	}

	void onSwitch()
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			const int index = H5ZoneSetBridge::selection();
			std::string why;
			if (H5ZoneSetBridge::switchTo(index, why))
			{
				auto list = H5ZoneSetBridge::names();
				messagesGUI->addMessage(std::format("Switching to zone set: {}",
					(index >= 0 && (size_t)index < list.size()) ? list[(size_t)index] : std::to_string(index)));
			}
			else
			{
				messagesGUI->addMessage(std::format("Switch Zone Set failed: {}", why));
			}
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mSwitchCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mSwitchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5SwitchZoneSetEvent,
			[this]() { onSwitch(); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5SwitchZoneSet only supports Halo 5: Forge");

		auto playerState = resolveDependentCheat(H5GetPlayerState);

		std::scoped_lock lock(g_mutex);
		g_playerState = playerState;
		g_names.clear();
		g_dirty = true;
		g_armed = true;
	}

	~Impl()
	{
		std::scoped_lock lock(g_mutex);
		g_armed = false;
		g_playerState.reset();
		g_names.clear();
	}
};


H5SwitchZoneSet::H5SwitchZoneSet(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

H5SwitchZoneSet::~H5SwitchZoneSet() { PLOG_VERBOSE << "~" << getName(); }
