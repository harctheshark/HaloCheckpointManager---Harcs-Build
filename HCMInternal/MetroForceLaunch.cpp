#include "pch.h"
#include "MetroForceLaunch.h"
#include "MetroEngine.h"
#include "MetroMove.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"

class MetroForceLaunch::Impl
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

	void onLaunch()
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			if (!inLevel()) { say("Force Launch: load a level first"); return; }
			lockOrThrow(settingsWeak, settings);
			if ((settings->forceLaunchManual->GetValue() ^ settings->forceLaunchForward->GetValue()) == false)
			{
				settings->forceLaunchManual->GetValueDisplay() = false;
				settings->forceLaunchManual->UpdateValueWithInput();
				settings->forceLaunchForward->GetValueDisplay() = true;
				settings->forceLaunchForward->UpdateValueWithInput();
				say("Force Launch radio button was in invalid state. \nHCM has set it to a valid state (relative)");
			}

			// Metro's walking motion is recomputed from input every frame, so a launch is HCM's own velocity on top of it:
			// relative ADDS to it (and to the current fall), absolute REPLACES it (and the fall)
			const bool replace = settings->forceLaunchManual->GetValue();
			metro::Vec3 velHalo;   // wu/s
			if (replace)
			{
				const auto v = settings->forceLaunchAbsoluteVec3->GetValue();
				velHalo = { v.x, v.y, v.z };
			}
			else
			{
				const auto look = metro::move::lookDirectionHalo();
				if (!look) { say("Force Launch: could not read the look direction"); return; }
				velHalo = metro::move::lookRelative(*look, settings->forceLaunchRelativeVec3->GetValue(), settings->forceLaunchForwardIgnoreZ->GetValue());
			}
			const std::string why = metro::move::launch(metro::fromHalo(velHalo), replace);
			if (why.empty()) say(std::format("Launched: {} {:.3f}, {:.3f}, {:.3f} wu/s", replace ? "set" : "added", velHalo.x, velHalo.y, velHalo.z));
			else say("Force Launch failed: " + why);
		}
		catch (HCMRuntimeException ex) { ex.prepend("Error launching player: "); runtimeExceptions->handleMessage(ex); }
	}

	ScopedCallback<ActionEvent> mLaunchCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mLaunchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->forceLaunchEvent, [this]() { onLaunch(); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroForceLaunch is Metro Redux only");
		metro::move::init();
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mLaunchCallback.removeCallback();
	}
};

MetroForceLaunch::MetroForceLaunch(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroForceLaunch::~MetroForceLaunch() { PLOG_VERBOSE << "~" << getName(); }
