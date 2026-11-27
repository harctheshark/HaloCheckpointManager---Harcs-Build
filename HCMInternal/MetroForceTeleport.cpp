#include "pch.h"
#include "MetroForceTeleport.h"
#include "MetroEngine.h"
#include "MetroMove.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"

class MetroForceTeleport::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::atomic<bool> mReady{ false };

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }
	bool inLevel() { auto m = mccWeak.lock(); return m && m->isGameCurrentlyPlaying(mGame); }

	void onTeleport()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			if (!inLevel()) { say("Force Teleport: load a level first"); return; }
			lockOrThrow(settingsWeak, settings);
			// shared radio pair: repair an invalid state the way MCC's ForceTeleport does
			if ((settings->forceTeleportManual->GetValue() ^ settings->forceTeleportForward->GetValue()) == false)
			{
				settings->forceTeleportManual->GetValueDisplay() = false;
				settings->forceTeleportManual->UpdateValueWithInput();
				settings->forceTeleportForward->GetValueDisplay() = true;
				settings->forceTeleportForward->UpdateValueWithInput();
				say("Force Teleport radio button was in invalid state. \nHCM has set it to a valid state (relative)");
			}

			const auto pos = metro::playerPosition();
			if (!pos) { say("Force Teleport: no player"); return; }
			metro::Vec3 destHalo;
			if (settings->forceTeleportManual->GetValue())
			{
				const auto v = settings->forceTeleportAbsoluteVec3->GetValue();
				destHalo = { v.x, v.y, v.z };
			}
			else
			{
				const auto look = metro::move::lookDirectionHalo();
				if (!look) { say("Force Teleport: could not read the look direction"); return; }
				const auto off = metro::move::lookRelative(*look, settings->forceTeleportRelativeVec3->GetValue(), settings->forceTeleportForwardIgnoreZ->GetValue());
				const metro::Vec3 here = metro::toHalo(*pos);
				destHalo = { here.x + off.x, here.y + off.y, here.z + off.z };
			}
			const std::string why = metro::move::teleportTo(metro::fromHalo(destHalo));
			if (why.empty()) say(std::format("Teleported to {:.3f}, {:.3f}, {:.3f}", destHalo.x, destHalo.y, destHalo.z));
			else say("Force Teleport failed: " + why);
		}
		catch (HCMRuntimeException ex) { ex.prepend("Error teleporting player: "); runtimeExceptions->handleMessage(ex); }
	}

	void onFillCurrent()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			if (!inLevel()) return;
			lockOrThrow(settingsWeak, settings);
			const auto pos = metro::playerPosition();
			if (!pos) return;
			const metro::Vec3 h = metro::toHalo(*pos);
			settings->forceTeleportAbsoluteVec3->GetValueDisplay() = SimpleMath::Vector3(h.x, h.y, h.z);
			settings->forceTeleportAbsoluteVec3->UpdateValueWithInput();
		}
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	void onCopy()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try { lockOrThrow(settingsWeak, settings); settings->forceTeleportAbsoluteVec3->serialiseToClipboard(); }
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	void onPaste()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try { lockOrThrow(settingsWeak, settings); settings->forceTeleportAbsoluteVec3->deserialiseFromClipboard(); }
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	ScopedCallback<ActionEvent> mTeleportCallback;
	ScopedCallback<ActionEvent> mFillCallback;
	ScopedCallback<ActionEvent> mCopyCallback;
	ScopedCallback<ActionEvent> mPasteCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mTeleportCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportEvent, [this]() { onTeleport(); }),
		mFillCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsoluteFillCurrent, [this]() { onFillCurrent(); }),
		mCopyCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsoluteCopy, [this]() { onCopy(); }),
		mPasteCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceTeleportAbsolutePaste, [this]() { onPaste(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroForceTeleport is Metro Redux only");
		metro::move::init();
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mTeleportCallback.removeCallback();
		mFillCallback.removeCallback();
		mCopyCallback.removeCallback();
		mPasteCallback.removeCallback();
	}
};

MetroForceTeleport::MetroForceTeleport(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroForceTeleport::~MetroForceTeleport() { PLOG_VERBOSE << "~" << getName(); }
