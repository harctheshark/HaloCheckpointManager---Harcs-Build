#include "pch.h"
#include "MetroDisplayInfo.h"
#include "MetroEngine.h"
#include "IMCCStateHook.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "RenderTextHelper.h"
#include "GlobalKill.h"
#include "MetroStateHook.h"

class MetroDisplayInfo::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::atomic<bool> mReady{ false };
	std::string mDataString;
	std::chrono::steady_clock::time_point mLastUpdate{};

	// speed from position over real time, over a ~0.1 s window so it reads steadily at any frame rate
	struct Sample { std::chrono::steady_clock::time_point t; metro::Vec3 p; };
	std::deque<Sample> mSamples;
	float mSpeedMs = 0.f, mSpeedHMs = 0.f;   // metres a second: total, horizontal

	// 2026-11-25 (user: "what is the furthest i can fall/highest speed i can survive? i want that speed readout in display
	// info"). Fall damage (static RE, binary_survey\agent_fall): the character shell (*(*(player+1272)+8)) latches the PEAK
	// downward speed of the fall at shell+0x1F0 (m/s of game time, from the real motion; the current one is +0x1F4). On
	// landing 0x79D250: strength = min((peak - crash_vel_min) / (crash_vel_max - crash_vel_min), 10), ignored below 0.1; the
	// hit goes through the damage pipeline (0x21EEC0 multiplies it by per-character / difficulty factors) and is taken off
	// health (1.0 = full). crash_vel_min/max are game data at *(shell+0xA8) +0x48/+0x4C.
	// The damage factor is not read statically, so HCM learns it from your first damaging landing (health lost / strength);
	// until then the lethal speed assumes a factor of 1 and is shown with "~".
	static constexpr uintptr_t kShellOwnerOff = 1272, kShellOff = 8, kShellCfgOff = 0xA8, kCrashMinOff = 0x48, kCrashMaxOff = 0x4C;
	static constexpr uintptr_t kPeakOff = 0x1F0, kFallSpeedOff = 0x1F4;
	static constexpr float kGravity = 9.81f;
	float mLastPeak = 0.f, mLastHealth = -1.f, mLearnedFactor = 0.f;
	bool mLoggedCfg = false;

	void fallLine(std::string& out, uintptr_t pl, float health)
	{
		const uintptr_t owner = metro::readPtr(pl + kShellOwnerOff);
		const uintptr_t shell = owner ? metro::readPtr(owner + kShellOff) : 0;
		const uintptr_t cfg = shell ? metro::readPtr(shell + kShellCfgOff) : 0;
		float lo = 0.f, hi = 0.f, peak = 0.f, cur = 0.f;
		if (!cfg || !metro::read(cfg + kCrashMinOff, lo) || !metro::read(cfg + kCrashMaxOff, hi) || !metro::read(shell + kPeakOff, peak)
			|| !metro::read(shell + kFallSpeedOff, cur) || !(lo >= 0.f && hi > lo && hi < 10000.f) || !std::isfinite(peak))
		{
			mLastHealth = health;
			return;
		}
		if (!mLoggedCfg) { mLoggedCfg = true; PLOG_INFO << std::format("Metro fall damage: crash_vel_min {:.3f} m/s, crash_vel_max {:.3f} m/s", lo, hi); }

		// learn the damage factor from a landing: the peak goes back to 0 in the tick that applies the hit
		if (peak == 0.f && mLastPeak > 0.f && mLastHealth > 0.f && health > 0.f && health < mLastHealth)
		{
			const float s = std::min((mLastPeak - lo) / (hi - lo), 10.f);
			if (s >= 0.1f)
			{
				mLearnedFactor = (mLastHealth - health) / s;
				PLOG_INFO << std::format("Metro fall damage: landed at {:.2f} m/s, strength {:.3f}, health {:.3f} -> {:.3f}, factor {:.3f}",
					mLastPeak, s, mLastHealth, health, mLearnedFactor);
			}
		}
		mLastPeak = peak;
		mLastHealth = health;

		const float factor = mLearnedFactor > 0.f ? mLearnedFactor : 1.f;
		const float hurts = lo + 0.1f * (hi - lo);
		const float lethal = std::max(hurts, lo + (hi - lo) * std::min(health / factor, 10.f));
		const float drop = lethal * lethal / (2.f * kGravity);   // free fall from rest, metres
		const float wu = metro::kMetresPerHaloUnit;
		const char* est = mLearnedFactor > 0.f ? "" : "~";
		out += std::format("Fall:     {:.2f} wu/s (peak {:.2f}) | Lethal Speed: {}{:.2f} wu/s\n", cur / wu, peak / wu, est, lethal / wu);
		out += std::format("          hurts from {:.2f} wu/s | lethal drop {}{:.1f} wu ({:.1f} m)\n", hurts / wu, est, drop / wu, drop);
	}

	bool active()
	{
		auto settings = settingsWeak.lock();
		auto mcc = mccStateHookWeak.lock();
		return settings && mcc && settings->display2DInfoToggle->GetValue() && mcc->isGameCurrentlyPlaying(mGame);
	}

	void updateData()
	{
		std::string out;
		const auto pos = metro::playerPosition();
		const auto now = std::chrono::steady_clock::now();
		if (pos)
		{
			mSamples.push_back({ now, *pos });
			while (mSamples.size() > 2 && now - mSamples.front().t > std::chrono::milliseconds(100)) mSamples.pop_front();
			const auto& a = mSamples.front();
			const float dt = std::chrono::duration<float>(now - a.t).count();
			if (dt > 0.02f)
			{
				const float dx = pos->x - a.p.x, dy = pos->y - a.p.y, dz = pos->z - a.p.z;
				mSpeedMs = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
				mSpeedHMs = std::sqrt(dx * dx + dz * dz) / dt;
			}
			const metro::Vec3 h = metro::toHalo(*pos);
			out += std::format("Position: {:.3f}, {:.3f}, {:.3f} wu\n", h.x, h.y, h.z);
			out += std::format("Speed:    {:.3f} wu/s ({:.3f} horizontal)\n", mSpeedMs / metro::kMetresPerHaloUnit, mSpeedHMs / metro::kMetresPerHaloUnit);
			// 2026-11-25 (user: the metres line "doesnt look right"): same axes as the Halo line (Z up), just not divided by 3.048
			out += std::format("Metres:   {:.3f}, {:.3f}, {:.3f}, {:.2f} m/s\n", pos->x, pos->z, pos->y, mSpeedMs);
		}
		else
		{
			mSamples.clear();
			out += "Position: -\nSpeed:    -\n";
		}
		const uintptr_t pl = metro::player();
		float health = 0.f, stamina = 0.f;
		if (pl && metro::read(pl + metro::kPlayerHealthOff, health) && metro::read(pl + metro::kPlayerStaminaOff, stamina))
		{
			out += std::format("Health:   {:.2f}   Stamina: {:.2f}\n", health, stamina);
			fallLine(out, pl, health);
		}
		if (auto mcc = std::dynamic_pointer_cast<MetroStateHook>(mccStateHookWeak.lock()))
		{
			const auto name = mcc->getLevelName();
			if (!name.empty()) out += std::format("Level:    {}\n", name);
		}
		mDataString = out;
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire) || GlobalKill::isKillSet()) return;
		try
		{
			if (!active()) { mSamples.clear(); return; }
			lockOrThrow(settingsWeak, settings);
			const auto now = std::chrono::steady_clock::now();
			if (mLastUpdate.time_since_epoch().count() == 0 || (now - mLastUpdate) >= std::chrono::milliseconds(16))
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
			if (settings->display2DInfoOutline->GetValue()) RenderTextHelper::drawOutlinedText(mDataString, position, fontColour, fontSize);
			else RenderTextHelper::drawText(mDataString, position, fontColour, fontSize);
		}
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	void onGameStateChanged(const MCCState&) { mDataString.clear(); mLastUpdate = {}; mSamples.clear(); }

	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroDisplayInfo is Metro Redux only");
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mRenderEventCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};

MetroDisplayInfo::MetroDisplayInfo(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroDisplayInfo::~MetroDisplayInfo() { PLOG_VERBOSE << "~" << getName(); }
