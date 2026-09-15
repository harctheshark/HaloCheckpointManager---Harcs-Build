#include "pch.h"
#include "H5Invincibility.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"

// See H5Invincibility.h for why BOTH bits are required and why this re-applies every tick.

namespace
{
	constexpr uintptr_t kObjectDamageFlags = 0x138;

	constexpr uint32_t kCannotTakeDamage = 0x00000080;   // bit 7
	constexpr uint32_t kCannotDie        = 0x00100000;   // bit 20
	constexpr uint32_t kInvincibleMask   = kCannotTakeDamage | kCannotDie;

	// ⚠ bit 2 means "already dead". The engine's own object_cannot_die no-ops when it is set
	// (guard at exe+0x0285E2FB), and the health/shield getters return 0. Skip the tick instead of
	// fighting it, and never write it.
	constexpr uint32_t kAlreadyDead = 0x00000004;

	bool sehRead32(const void* src, uint32_t& out)
	{
		__try { out = *(const uint32_t*)src; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool sehWrite32(void* dest, uint32_t v)
	{
		__try { *(uint32_t*)dest = v; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

class H5Invincibility::Impl
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

	// Applies or removes the mask with a fresh read every time. See the header: never cache and restore.
	void applyMask(bool enable)
	{
		lockOrThrow(playerStateWeak, playerState);
		const uintptr_t object = playerState->getPlayerObject();   // throws when dead / no level
		if (!object) return;

		uint32_t flags = 0;
		if (!sehRead32((const void*)(object + kObjectDamageFlags), flags))
			throw HCMRuntimeException("Could not read the Halo 5 object damage flags");

		// Already dead - the engine ignores these bits in that state, so leave it alone this tick.
		if (flags & kAlreadyDead) return;

		const uint32_t wanted = enable ? (flags | kInvincibleMask) : (flags & ~kInvincibleMask);
		if (wanted == flags) return;   // nothing to do; avoids pointless cross-thread writes every frame

		if (!sehWrite32((void*)(object + kObjectDamageFlags), wanted))
			throw HCMRuntimeException("Could not write the Halo 5 object damage flags");
	}

	void onTick()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			applyMask(true);
		}
		catch (HCMRuntimeException& ex)
		{
			// Throwing here is the NORMAL case at a menu, during a load and while dead - see
			// H5GetPlayerState.h. Never switch the toggle off for it; just skip the tick.
			const auto now = std::chrono::steady_clock::now();
			if (mLastFailureLog.time_since_epoch().count() == 0
				|| now - mLastFailureLog > std::chrono::seconds(5))
			{
				mLastFailureLog = now;
				PLOG_DEBUG << "Invincibility skipped a tick (will retry): " << ex.what();
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
				// Clear the bits with a FRESH read, so we cannot clobber anything the engine changed while
				// we were on. Failure here is not worth reporting - the player is dead or at a menu, and in
				// both cases the flags word is about to be rewritten by the engine anyway.
				try { applyMask(false); }
				catch (...) {}
			}
		}
		catch (HCMRuntimeException ex)
		{
			ex.prepend("Error toggling Invincibility: ");
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
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5InvincibilityToggle->valueChangedEvent,
			[this](bool& n) { onToggleChanged(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5Invincibility only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);

		try
		{
			if (auto settings = settingsWeak.lock(); settings && settings->h5InvincibilityToggle->GetValue())
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
	}
};


H5Invincibility::H5Invincibility(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5Invincibility::~H5Invincibility() { PLOG_VERBOSE << "~" << getName(); }
