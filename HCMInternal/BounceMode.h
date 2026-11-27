#pragma once
#include "pch.h"
#include "SettingsStateAndEvents.h"
#include "SettingsEnums.h"
#include "IMessagesGUI.h"
#include "IMCCStateHook.h"
#include "ScopedCallback.h"

// Bounce Mode / Season 7 Bounce Mode (2026-11-24, Halo 2). The user's words: "Bounce Mode will do quite a few things ...
// Have the options be 'editable' after they're on, but by default, the button should set those to a tee, nothing i edit
// should overwrite the startup behavior", then "Bounce mode (and s7 bounce mode) should be a checkbox".
//
// Two checkboxes, only one on at a time:
//   * ticking one SETS a fixed list of options. The list is the constants and apply() below and nothing else - it is never
//     read back from the options, so editing an option afterwards changes that option only, and the next tick sets the
//     list exactly again;
//   * unticking puts back what each option was before the tick - but only options still at the value Bounce Mode set
//     (one you changed since stays as you left it). The custom emblem IMAGE is not put back (only its on/off);
//   * ticking the other one while one is on switches presets; unticking then restores the values from before the FIRST tick.
//   * ticked outside a level: the tickrate (which needs a level) and the cheats are applied again when the next Halo 2
//     level is in play; likewise a tickrate put back outside a level.
//
// It only drives the existing settings (each cheat reacts as if the user had clicked its own row), plus the two "load
// this file" events H2ArmorColour listens to. Header only; one instance, made in App.h.
class BounceMode
{
private:
	// ---- what a tick sets -----------------------------------------------------------------------------------------
	static constexpr int kTickrate = 30;               // Bounce Mode: 30 tick, Season 7 Physics off (collision scalar 30)
	static constexpr int kSeason7Tickrate = 60;        // Season 7 Bounce Mode: 60 tick, Season 7 Physics on (scalar 60)
	// TAA as the user ran it on 2026-11-24 (TAA Blend 0.20, TAA Motion Weight 0.40, TAA Object Motion on)
	static constexpr float kTAABlend = 0.20f;
	static constexpr float kTAAMotionBlend = 0.40f;
	static constexpr bool kTAAObjectMotion = true;
	// MasterChief Colour preset: in <HCM folder>\armour_colours (its colours only - the emblem is the file below)
	static constexpr const wchar_t* kColourPresetFile = L"harctwo.mccolour";
	// Custom Emblem image (the user's own file, by the path they gave)
	static constexpr const wchar_t* kEmblemPath = L"C:\\Users\\hurri\\Pictures\\emblems\\64 original\\bg\\TransparentEmblem.png";

	// ---- one option Bounce Mode has set: its value before the first tick, and what Bounce Mode set ----
	struct IEntry
	{
		virtual ~IEntry() = default;
		virtual const void* key() const = 0;
		virtual int restore() = 0;     // 1 = put back, 0 = nothing to do, -1 = left alone (changed since)
		virtual void reassert() = 0;   // fire it again with its current value if it is still what Bounce Mode set
	};
	template <typename T>
	struct Entry : IEntry
	{
		std::shared_ptr<BinarySetting<T>> setting; T before; T set;
		Entry(std::shared_ptr<BinarySetting<T>> s) : setting(std::move(s)), before(setting->GetValue()), set(setting->GetValue()) {}
		const void* key() const override { return setting.get(); }
		int restore() override
		{
			if (!(setting->GetValue() == set)) return -1;
			if (before == set) return 0;
			setting->GetValueDisplay() = before;
			setting->UpdateValueWithInput();
			return 1;
		}
		void reassert() override
		{
			if (!(setting->GetValue() == set)) return;
			setting->GetValueDisplay() = set;
			setting->UpdateValueWithInput();
		}
	};

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::wstring mColourPresetPath;
	std::recursive_mutex mMutex;
	std::vector<std::unique_ptr<IEntry>> mEntries;    // in the order they were first set
	std::vector<IEntry*> mReassertAtLevel;            // to fire again once a level is in play (tickrate, cheats)
	bool mPendingLevel = false;
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mBounceCallback;
	ScopedCallback<eventpp::CallbackList<void(bool&)>> mSeason7BounceCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateCallback;

	template <typename T>
	Entry<T>* entryFor(const std::shared_ptr<BinarySetting<T>>& setting)
	{
		for (auto& e : mEntries) if (e->key() == setting.get()) return static_cast<Entry<T>*>(e.get());
		mEntries.push_back(std::make_unique<Entry<T>>(setting));
		return static_cast<Entry<T>*>(mEntries.back().get());
	}

	// Set an option the way its own row does (the cheat behind it hears the change). An option that already has the value
	// is left alone unless `always`: several cheats patch code when switched on, and none of them needs telling twice.
	template <typename T>
	Entry<T>* put(const std::shared_ptr<BinarySetting<T>>& setting, T value, bool always = false)
	{
		if (!setting) return nullptr;
		Entry<T>* e = entryFor(setting);
		const bool same = setting->GetValue() == value;
		setting->GetValueDisplay() = value;
		if (!same || always) setting->UpdateValueWithInput();
		e->set = setting->GetValue();
		return e;
	}

	bool inLevel()
	{
		auto mcc = mccStateHookWeak.lock();
		return mcc && mcc->isGameCurrentlyPlaying(GameState(GameState::Value::Halo2));
	}

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	void apply(const std::shared_ptr<SettingsStateAndEvents>& settings, bool season7)
	{
		const char* name = season7 ? "Season 7 Bounce Mode" : "Bounce Mode";
		const bool level = inLevel();
		mReassertAtLevel.clear();

		// Shadow / Detail: Retail + LOD
		put(settings->h2ShadowResolutionSetting, SettingsEnums::H2ShadowResolution::RetailLOD);

		// Tickrate. The rate first (always sent: the box only mirrors the game's rate until one is picked, so "already 60"
		// does not mean a rate was picked), then Season 7 Physics, which then only moves the collision scalar.
		mReassertAtLevel.push_back(put(settings->customTickrate, season7 ? kSeason7Tickrate : kTickrate, true));
		put(settings->season7PhysicsToggle, season7);

		put(settings->fpScaleFixToggle, true);
		put(settings->animationFixesToggle, true);

		// Infinite Ammo with frags only, Bottomless Clip, Invulnerability
		put(settings->infiniteAmmoGrenades, SettingsEnums::InfiniteAmmoGrenades::Frags);
		mReassertAtLevel.push_back(put(settings->infiniteAmmoToggle, true));
		mReassertAtLevel.push_back(put(settings->bottomlessClipToggle, true));
		mReassertAtLevel.push_back(put(settings->invulnerabilityToggle, true));

		// MP Revert Fix (only if it is not on already)
		put(settings->mpRevertRepairToggle, true);

		// Anti Aliasing: TAA with the values above
		put(settings->h2TAABlend, kTAABlend);
		put(settings->h2TAAMotionBlend, kTAAMotionBlend);
		put(settings->h2TAAObjectMotion, kTAAObjectMotion);
		put(settings->h2TAADebug, false);
		put(settings->postAAMethod, SettingsEnums::PostAAMethod::TAA);
		put(settings->smaaToggle, true);

		put(settings->OBSBypassToggle, true);

		// MasterChief Colour: the preset's two colours (the load is synchronous), then the override on
		auto* primary = entryFor(settings->h2ArmorColourPrimary);
		auto* secondary = entryFor(settings->h2ArmorColourSecondary);
		settings->h2ArmorColourLoadPresetFromPathEvent->operator()(mColourPresetPath);
		primary->set = settings->h2ArmorColourPrimary->GetValue();
		secondary->set = settings->h2ArmorColourSecondary->GetValue();
		put(settings->h2ArmorColourToggle, true);

		// Custom Emblem: that image, then the emblem on
		settings->h2ArmorEmblemLoadFromPathEvent->operator()(std::wstring(kEmblemPath));
		put(settings->h2ArmorEmblemToggle, true);

		std::erase(mReassertAtLevel, nullptr);
		mPendingLevel = !level;
		say(level ? std::format("{} on.", name) : std::format("{} on - the tickrate and cheats are applied when a Halo 2 level loads.", name));
	}

	void restore()
	{
		int restored = 0, kept = 0;
		mReassertAtLevel.clear();
		mPendingLevel = false;
		IEntry* tickrate = nullptr;
		if (auto settings = settingsWeak.lock())
			for (auto& e : mEntries) if (e->key() == settings->customTickrate.get()) tickrate = e.get();
		for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
		{
			const int r = (*it)->restore();
			if (r > 0) ++restored; else if (r < 0) ++kept;
			if (r > 0 && it->get() == tickrate && !inLevel())
			{
				// the rate could not reach the game: send it again once a level is in play
				auto settings = settingsWeak.lock();
				if (settings) mRestoreTickrate = settings->customTickrate->GetValue();
			}
		}
		mEntries.clear();
		say(kept > 0 ? std::format("Bounce Mode off: {} option{} put back, {} you changed left as they are.", restored, restored == 1 ? "" : "s", kept)
			: std::format("Bounce Mode off: {} option{} put back.", restored, restored == 1 ? "" : "s"));
	}
	int mRestoreTickrate = 0;   // > 0: a tickrate put back outside a level, to send when one is in play

	void onToggle(bool season7, bool on)
	{
		std::scoped_lock lk(mMutex);
		auto settings = settingsWeak.lock();
		if (!settings) return;
		auto& other = season7 ? settings->bounceModeToggle : settings->season7BounceModeToggle;
		try
		{
			if (on)
			{
				// only one at a time: switching keeps the values from before the first tick, for the untick
				if (other->GetValue()) { other->GetValue() = false; other->GetValueDisplay() = false; }
				mRestoreTickrate = 0;
				apply(settings, season7);
			}
			else if (!other->GetValue())
				restore();
		}
		catch (HCMRuntimeException& ex) { PLOG_ERROR << "Bounce Mode: " << ex.what(); }
		catch (HCMInitException& ex) { PLOG_ERROR << "Bounce Mode: " << ex.what(); }
	}

	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState.operator GameState::Value() != GameState::Value::Halo2 || s.currentPlayState != PlayState::Ingame) return;
		std::scoped_lock lk(mMutex);
		auto settings = settingsWeak.lock();
		if (!settings) return;
		try
		{
			if (mPendingLevel)
			{
				mPendingLevel = false;
				for (IEntry* e : mReassertAtLevel) e->reassert();
				mReassertAtLevel.clear();
				PLOG_INFO << "Bounce Mode: level in play - tickrate and cheats applied";
			}
			if (mRestoreTickrate > 0)
			{
				if (settings->customTickrate->GetValue() == mRestoreTickrate)
				{
					settings->customTickrate->GetValueDisplay() = mRestoreTickrate;
					settings->customTickrate->UpdateValueWithInput();
				}
				mRestoreTickrate = 0;
			}
		}
		catch (HCMRuntimeException& ex) { PLOG_ERROR << "Bounce Mode: " << ex.what(); }
	}

public:
	BounceMode(std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<IMessagesGUI> messages, std::shared_ptr<IMCCStateHook> mccStateHook, const std::string& hcmDirPath)
		: settingsWeak(settings), messagesWeak(messages), mccStateHookWeak(mccStateHook),
		mColourPresetPath((std::filesystem::path(hcmDirPath) / "armour_colours" / kColourPresetFile).wstring()),
		mBounceCallback(settings->bounceModeToggle->valueChangedEvent, [this](bool& on) { onToggle(false, on); }),
		mSeason7BounceCallback(settings->season7BounceModeToggle->valueChangedEvent, [this](bool& on) { onToggle(true, on); }),
		mMCCStateCallback(mccStateHook->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
	}
};
