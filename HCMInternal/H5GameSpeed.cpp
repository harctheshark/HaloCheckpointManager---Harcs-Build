#include "pch.h"
#include "H5GameSpeed.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5GameSpeed.h for the struct layout and the engine addresses it was read from.

namespace
{
	constexpr uintptr_t kTlsGameTimeGlobals = 0x1538;   // tls + 0x1538 -> s_game_time_globals*

	constexpr uintptr_t kSpeed        = 0x10;
	constexpr uintptr_t kRampDuration = 0x24;

	constexpr float kDefaultSpeed = 1.0f;

	bool sehReadPtr(const void* src, uintptr_t& out)
	{
		__try { out = *(const uintptr_t*)src; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehWriteFloat(void* dest, float v)
	{
		__try { *(float*)dest = v; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehReadFloat(const void* src, float& out)
	{
		__try { out = *(const float*)src; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

class H5GameSpeed::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;
	std::shared_ptr<RenderEvent> mRenderEvent;
	std::atomic<bool> mReady{ false };

	std::unique_ptr<ScopedCallback<RenderEvent>> mTickCallback;
	std::chrono::steady_clock::time_point mLastFailureLog{};

	// Resolves the time globals through the simulation thread's TLS block. Throws at a menu / during a load,
	// which is the normal case and must never switch the toggle off.
	uintptr_t getTimeGlobals()
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t tls = playerState->getTlsBase();
		if (!tls) throw HCMRuntimeException("No Halo 5 simulation thread TLS block");

		uintptr_t globals = 0;
		if (!sehReadPtr((const void*)(tls + kTlsGameTimeGlobals), globals) || !globals)
			throw HCMRuntimeException("Could not read the Halo 5 game time globals");
		return globals;
	}

	void applySpeed(float speed)
	{
		const uintptr_t g = getTimeGlobals();

		// ⚠ Zero the scripted ramp FIRST. If a ramp is running it recomputes speed from ramp_from/ramp_to on
		// the next update and would overwrite whatever we just stored.
		float ramp = 0.f;
		if (sehReadFloat((const void*)(g + kRampDuration), ramp) && ramp > 0.f)
		{
			if (!sehWriteFloat((void*)(g + kRampDuration), 0.f))
				throw HCMRuntimeException("Could not clear the Halo 5 game speed ramp");
		}

		float current = 0.f;
		if (sehReadFloat((const void*)(g + kSpeed), current) && current == speed)
			return;   // already there - avoids a pointless cross-thread write every frame

		if (!sehWriteFloat((void*)(g + kSpeed), speed))
			throw HCMRuntimeException("Could not write the Halo 5 game speed");
	}

	float wantedSpeed()
	{
		lockOrThrow(settingsWeak, settings);
		return settings->h5GameSpeedAmount->GetValue();
	}

	void onTick()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			applySpeed(wantedSpeed());
		}
		catch (HCMRuntimeException& ex)
		{
			// Normal at a menu, during a load and between levels. Self-healing: never touch the toggle, just
			// skip the tick and rate-limit the log.
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Game Speed skipped a tick (will retry): " << ex.what();
			}
		}
		catch (...) {}
	}

	void onToggleChanged(bool& newValue)
	{
		try
		{
			if (newValue && !mTickCallback)
			{
				mTickCallback = std::make_unique<ScopedCallback<RenderEvent>>(
					mRenderEvent, [this](SimpleMath::Vector2) { onTick(); });
			}
			else if (!newValue && mTickCallback)
			{
				mTickCallback.reset();
				// Restore the engine default rather than a cached value - see the header.
				try { applySpeed(kDefaultSpeed); }
				catch (...) {}
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Game Speed: ");
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - see the note in HCECheckpointDetours.cpp.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEvent(dicon.Resolve<RenderEvent>().lock()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5GameSpeedToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5GameSpeed only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);

		try
		{
			if (auto settings = settingsWeak.lock(); settings && settings->h5GameSpeedToggle->GetValue())
			{
				bool on = true;
				onToggleChanged(on);
			}
		}
		catch (...) {}
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mTickCallback.reset();
		mToggleCallback.removeCallback();
		// Leave the game running at normal speed if HCM is torn down mid-session.
		try { applySpeed(kDefaultSpeed); }
		catch (...) {}
	}
};


H5GameSpeed::H5GameSpeed(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5GameSpeed::~H5GameSpeed() { PLOG_VERBOSE << "~" << getName(); }
