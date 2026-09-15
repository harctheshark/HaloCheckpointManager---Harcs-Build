#include "pch.h"
#include "H5DisplayInfo.h"
#include "H5GetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "RenderTextHelper.h"
#include "GlobalKill.h"

// See H5DisplayInfo.h. Structure mirrors HCEDisplayInfo: refresh the string on a timer, draw it every frame so
// it neither flickers nor blanks while the chain is briefly unresolvable.

class H5DisplayInfo::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<H5GetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };
	bool mIsActive = false;
	std::string mDataString;
	// Resolved once in the ctor; the shared HaloCER transition toggle (see updateData).
	std::shared_ptr<BinarySetting<bool>> mShowZoneSetPrep;
	std::chrono::steady_clock::time_point mLastUpdate{};

	void updateData()
	{
		auto playerState = playerStateWeak.lock();
		if (!playerState) return;

		std::string out;
		// Each row is independently guarded: at a menu or mid-respawn some of these resolve and some throw,
		// and a single failure must not blank the whole overlay.
		try
		{
			const auto p = playerState->getPlayerPosition();
			out += std::format("Position: {:.3f}, {:.3f}, {:.3f}\n", p.x, p.y, p.z);
		}
		catch (HCMRuntimeException&) { out += "Position: -\n"; }

		try
		{
			const auto a = playerState->getPlayerAim();
			out += std::format("Aim:      {:.3f}, {:.3f}, {:.3f}\n", a.x, a.y, a.z);
		}
		catch (HCMRuntimeException&) { out += "Aim:      -\n"; }

		// The character controller's own velocity (proxy+0x40) - the field the engine integrates, NOT the
		// published mirror at obj+0x248, which never changes. Speed is worth showing next to it: the
		// Acrophobia limiter is defined in terms of it (thrust reaches zero at 16 wu/s).
		try
		{
			const auto v = playerState->getPlayerVelocity();
			out += std::format("Velocity: {:.3f}, {:.3f}, {:.3f}\n", v.x, v.y, v.z);
			out += std::format("Speed:    {:.3f} wu/s\n", v.Length());
		}
		catch (HCMRuntimeException&) { out += "Velocity: -\nSpeed:    -\n"; }

		try { out += std::format("Datum:    0x{:08X}\n", playerState->getPlayerDatum()); }
		catch (HCMRuntimeException&) { out += "Datum:    -\n"; }

		try { out += std::format("Object:   0x{:X}\n", playerState->getPlayerObject()); }
		catch (HCMRuntimeException&) { out += "Object:   -\n"; }

		// Halo-5-specific and load-bearing: "local" is what makes checkpoint/revert meaningful, and the gate
		// is what makes engine object calls (teleport) legal on the resolving thread.
		try { out += std::format("Sim:      {}\n", playerState->getSimulationKind()); }
		catch (HCMRuntimeException&) { out += "Sim:      -\n"; }

		try { out += std::format("ObjGate:  {}\n", playerState->hasObjectWriteGate() ? "yes" : "no"); }
		catch (HCMRuntimeException&) { out += "ObjGate:  -\n"; }

		// Same split as the HaloCER overlay: the main row is the COMMITTED zone set, so it does not flip to
		// the incoming name the instant a switch begins. The transition rows are separate.
		// This now reads the engine's own current-zone-set global directly (see getCommittedZoneSetName),
		// which is why it is correct at level start - the old version could only guess until the first
		// switch, and in practice stayed pinned to the first zone set forever.
		try { out += std::format("Zone Set: {}\n", playerState->getCommittedZoneSetName()); }
		catch (HCMRuntimeException&) { out += "Zone Set: None\n"; }

		// ⚠ Deliberately the SAME setting object the HaloCER transition rows use. The two titles can never
		// coexist in one process, and settings serialise BY NAME - giving Halo 5 its own would mean a
		// second name in everyone's config for identical behaviour. Same reasoning as display2DInfoToggle.
		if (mShowZoneSetPrep && mShowZoneSetPrep->GetValue())
		{
			out += std::format("  Preparing Zone Set: {}\n", playerState->isPreparingZoneSet() ? "True" : "False");
			const auto prepared = playerState->getPreparedZoneSetName();
			out += std::format("  Prepared Zone Set:  {}\n", prepared.empty() ? "NULL" : prepared);
		}

		mDataString = out;
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);

			const auto now = std::chrono::steady_clock::now();
			if (mLastUpdate.time_since_epoch().count() == 0 || (now - mLastUpdate) >= std::chrono::milliseconds(33))
			{
				mLastUpdate = now;
				updateData();
			}
			if (mDataString.empty()) return;

			const SimpleMath::Vector2& cornerOffset = settings->display2DInfoScreenOffset->GetValue();
			SimpleMath::Vector2 position;
			switch (settings->display2DInfoAnchorCorner->GetValue())
			{
			case SettingsEnums::ScreenAnchorEnum::TopLeft:     position = cornerOffset; break;
			case SettingsEnums::ScreenAnchorEnum::TopRight:    position = { screenSize.x - cornerOffset.x, cornerOffset.y }; break;
			case SettingsEnums::ScreenAnchorEnum::BottomRight: position = { screenSize.x - cornerOffset.x, screenSize.y - cornerOffset.y }; break;
			case SettingsEnums::ScreenAnchorEnum::BottomLeft:  position = { cornerOffset.x, screenSize.y - cornerOffset.y }; break;
			default:                                           position = cornerOffset; break;
			}

			const auto fontColour = ImGui::ColorConvertFloat4ToU32(settings->display2DInfoFontColour->GetValue());
			const float fontSize = settings->display2DInfoFontSize->GetValue();

			// RenderTextHelper is pure ImGui, so it works unchanged on the D3D12 backend Halo 5 uses.
			if (settings->display2DInfoOutline->GetValue())
				RenderTextHelper::drawOutlinedText(mDataString, position, fontColour, fontSize);
			else
				RenderTextHelper::drawText(mDataString, position, fontColour, fontSize);
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onToggleEvent(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				mIsActive = false;
				return;
			}
			mIsActive = newValue;
			mDataString.clear();
			mLastUpdate = {};
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onGameStateChanged(const MCCState&)
	{
		mDataString.clear();
		mLastUpdate = {};
	}

	// Declared LAST so the callbacks are torn down before the members they capture.
	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(H5GetPlayerState)),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); }),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->display2DInfoToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		mShowZoneSetPrep = dicon.Resolve<SettingsStateAndEvents>().lock()->hceDisplayInfoShowZoneSetPrep;

		if (static_cast<GameState::Value>(game) != GameState::Value::Halo5Forge)
			throw HCMInitException("H5DisplayInfo only supports Halo 5: Forge");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mRenderEventCallback.removeCallback();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


H5DisplayInfo::H5DisplayInfo(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon)) {}
H5DisplayInfo::~H5DisplayInfo() { PLOG_VERBOSE << "~" << getName(); }
