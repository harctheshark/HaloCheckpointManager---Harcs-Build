#include "pch.h"
#include "HCEGameSpeed.h"
#include "HCEGetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "PointerDataStore.h"
#include "IMakeOrGetCheat.h"
#include "MultilevelPointer.h"
#include "GlobalKill.h"

// See HCEGameSpeed.h for the full derivation of the struct and why this is preferred to the Speedhack.

namespace
{
	// s_game_time_globals, reached as *(tlsBase + kTlsSlot).
	constexpr int64_t kTlsSlot = 0x98;
	constexpr int64_t kSpeed = 0x10;
	constexpr int64_t kRampDuration = 0x1C;

	std::atomic<uint32_t>  gSpeedBits{ 0x3F800000 };  // target speed, as float bits (1.0f)
	std::atomic<bool>      gActive{ false };

	// ⚠⚠ WHY THIS NO LONGER RIDES THE GAME-THREAD TICK.
	//
	// The first version resolved the address from the game thread's own TEB inside an HCEGameThreadTick
	// callback. That was correct in principle and did not work in practice: HCEGameThreadTick rides
	// HCEGetCameraData's DoUpdateCamera midhook, its own header warns "THE TICK IS NOT GUARANTEED TO RUN",
	// and if HCEGetCameraData is unavailable this feature silently did nothing while still reporting
	// "Game speed: 2.00x" on screen - which is exactly how it was reported broken.
	//
	// HCEGetPlayerState::getTlsBase() walks to the SIM GAME THREAD's TLS block no matter which thread asks,
	// so the write does not need to be on the game thread to reach the right memory. The RE for this
	// confirmed the walk is safe from a non-game thread, and the write itself is a single naturally-aligned
	// 4-byte store, which is atomic on x86 - the engine cannot observe a torn float.
	//
	// So: one owned worker, no borrowed tick, no dependency that can silently be absent.
	//
	// It re-applies on a short period rather than once on toggle because a scripted time-scale ramp
	// (game_time_globals+0x18..+0x24) recomputes speed while ramp_duration > 0 and would otherwise stomp us.
	// Re-applying also means the value survives a level load, where the struct is re-initialised to 1.0.
	void applyOnce(const uintptr_t timeGlobals) noexcept
	{
		float speed;
		const uint32_t bits = gSpeedBits.load(std::memory_order_relaxed);
		std::memcpy(&speed, &bits, sizeof(speed));

		// Kill any scripted ramp FIRST: if the speed store is the one that gets interrupted we have not left
		// a ramp running that would fight the next pass.
		const float noRamp = 0.0f;
		HCEGetPlayerState::tryWriteRaw(timeGlobals + kRampDuration, &noRamp, sizeof(noRamp));
		HCEGetPlayerState::tryWriteRaw(timeGlobals + kSpeed, &speed, sizeof(speed));
	}
}

class HCEGameSpeed::HCEGameSpeedImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::thread mWorker;
	std::atomic<bool> mWorkerRunning{ false };
	std::atomic<bool> mReady{ false };
	bool mWasCinematic = false;   // worker thread only

	static float clampSpeed(double in)
	{
		// Not the engine's clamp - that one only bounds the catch-up BUDGET. This just keeps the value sane:
		// zero or negative would stop or reverse the accumulator, and the engine stops keeping up past ~5x
		// anyway (see the header).
		if (!(in > 0.0)) return 0.01f;
		if (in > 100.0) return 100.0f;
		return (float)in;
	}

	void publishSpeed(double in)
	{
		const float f = clampSpeed(in);
		uint32_t bits;
		std::memcpy(&bits, &f, sizeof(bits));
		gSpeedBits.store(bits, std::memory_order_relaxed);
	}

	// Resolve game_time_globals through the sim's TLS. Returns 0 when there is no game running, which is
	// the normal case at a menu and is not an error.
	uintptr_t resolveTimeGlobals()
	{
		try
		{
			auto playerState = mPlayerStateWeak.lock();
			if (!playerState) return 0;
			const uintptr_t tlsBase = playerState->getTlsBase();
			uintptr_t timeGlobals = 0;
			if (!HCEGetPlayerState::tryReadRaw(tlsBase + kTlsSlot, &timeGlobals, sizeof(timeGlobals))) return 0;
			return timeGlobals;
		}
		catch (HCMRuntimeException&) { return 0; }   // not in game / TLS walk unavailable
	}

	void setWorkerWanted(bool wanted)
	{
		if (wanted && !mWorkerRunning.exchange(true))
		{
			mWorker = std::thread([this]()
				{
					bool loggedOnce = false;
					while (mWorkerRunning.load(std::memory_order_acquire) && !GlobalKill::isKillSet())
					{
						if (gActive.load(std::memory_order_acquire))
						{
							const uintptr_t tg = resolveTimeGlobals();
							if (tg)
							{
								// ⚠ HANDS OFF DURING A CINEMATIC. Scaling time through a cutscene desynchronises
								// it from its audio and, on the prerendered videos, corrupts playback outright.
								// One edge-triggered write puts the speed back to 1.0 as the cinematic starts,
								// and then we stop writing entirely for its duration - continuing to force 1.0
								// every pass would fight the engine's own scripted time-scale ramps, which are
								// exactly what a cutscene uses.
								auto ps = mPlayerStateWeak.lock();
								const bool cinematic = ps && ps->isCinematicPlaying();
								if (cinematic)
								{
									if (!mWasCinematic)
									{
										const float one = 1.0f;
										HCEGetPlayerState::tryWriteRaw(tg + kSpeed, &one, sizeof(one));
										mWasCinematic = true;
										PLOG_DEBUG << "Game Speed: cinematic started, speed restored to 1.0 "
											"and held until it ends";
									}
									std::this_thread::sleep_for(std::chrono::milliseconds(16));
									continue;
								}
								if (mWasCinematic)
								{
									mWasCinematic = false;
									loggedOnce = false;   // re-log the resume
								}

								applyOnce(tg);
								if (!loggedOnce)
								{
									// One line, once, so a report of "nothing happens" can be told apart from
									// "it applied and the engine ignored it" without another round trip.
									float readBack = 0.f;
									HCEGetPlayerState::tryReadRaw(tg + kSpeed, &readBack, sizeof(readBack));
									PLOG_INFO << "Game Speed applied: game_time_globals=0x" << std::hex << tg
										<< std::dec << " speed reads back as " << readBack;
									loggedOnce = true;
								}
							}
							else
							{
								loggedOnce = false;   // re-log after a level change
							}
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(16));
					}
				});
		}
		else if (!wanted && mWorkerRunning.exchange(false))
		{
			if (mWorker.joinable()) mWorker.join();
		}
	}

	// Put the engine back the way we found it. Done from HERE rather than from the tick, because by the time
	// the tick is unregistered it can no longer run - and leaving the sim at 3x after a toggle-off would look
	// exactly like the game being broken.
	void restoreDefaultSpeed()
	{
		const uintptr_t tg = resolveTimeGlobals();
		if (!tg) return;   // not in game - game_time_initialize re-seeds speed to 1.0 on the next load anyway
		const float one = 1.0f;
		HCEGetPlayerState::tryWriteRaw(tg + kSpeed, &one, sizeof(one));
	}

	std::weak_ptr<HCEGetPlayerState> mPlayerStateWeak;

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (newValue)
			{
				publishSpeed(settings->hceGameSpeedSetting->GetValue());
				gActive.store(true, std::memory_order_release);
				setWorkerWanted(true);

				if (mccStateHook->isGameCurrentlyPlaying(mGame))
					messagesGUI->addMessage(std::format("Game speed: {:.2f}x", settings->hceGameSpeedSetting->GetValue()));
			}
			else
			{
				gActive.store(false, std::memory_order_release);
				setWorkerWanted(false);
				restoreDefaultSpeed();
				if (mccStateHook->isGameCurrentlyPlaying(mGame))
					messagesGUI->addMessage("Game speed: normal.");
			}
		}
		catch (HCMRuntimeException ex)
		{
			gActive.store(false, std::memory_order_release);
			setWorkerWanted(false);
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onSpeedChanged(double& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		publishSpeed(newValue);
		// If it is already running, the next game-thread tick picks the new value up on its own.
	}

	// Declared LAST - ScopedCallbacks subscribe inside their own constructors.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(double&)>> mSpeedCallback;

public:
	HCEGameSpeedImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceGameSpeedToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mSpeedCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceGameSpeedSetting->valueChangedEvent, [this](double& n) { onSpeedChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEGameSpeed only supports Halo Campaign Evolved");

		// HCEGetPlayerState owns the TLS walk. It is the ONLY dependency - deliberately, after the first
		// version silently did nothing because it also needed HCEGetCameraData for a borrowed tick.
		mPlayerStateWeak = resolveDependentCheat(HCEGetPlayerState);

		mReady.store(true, std::memory_order_release);
	}

	~HCEGameSpeedImpl()
	{
		mReady.store(false, std::memory_order_release);

		// ⚠ ORDER. Disarm first so a tick already in flight becomes a no-op, then unregister it (which MUST
		// happen before this object dies, or the camera midhook calls into freed memory), then restore.
		gActive.store(false, std::memory_order_release);
		setWorkerWanted(false);
		restoreDefaultSpeed();

		mToggleCallback.removeCallback();
		mSpeedCallback.removeCallback();
	}
};


HCEGameSpeed::HCEGameSpeed(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEGameSpeedImpl>(game, dicon))
{
}

HCEGameSpeed::~HCEGameSpeed()
{
	PLOG_VERBOSE << "~" << getName();
}
