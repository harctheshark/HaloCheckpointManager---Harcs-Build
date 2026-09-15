#pragma once
#include <pugixml.hpp>
#include <vector>
#include <atomic>
#include <chrono>

class SerialisableSetting
{
private:
	bool mIsIncludedInClipboard;
public:
	virtual void serialise(pugi::xml_node parent) = 0;
	virtual void deserialise(pugi::xml_node input) = 0;
	virtual std::string getOptionName() = 0;
	explicit SerialisableSetting() {}

	// Preset support: allSerialisableOptions is only a curated persist-subset (cheat toggles are deliberately left
	// out so they don't carry between game launches). Presets need EVERY setting, so while a SettingsStateAndEvents
	// is constructing it points this collector at its own vector and each BinarySetting self-registers (see
	// BinarySetting's constructor). Null except during that construction, so there's no cross-instance or dangling
	// state - safe across HCM re-attach.
	static inline std::vector<SerialisableSetting*>* s_presetCollector = nullptr;

	// ---- crash-durable autosave -----------------------------------------------------------------------------
	// ⚠⚠ THE CONFIG USED TO BE WRITTEN **ONLY** IN ~SettingsStateAndEvents. A game that crashes never runs that
	// destructor, so every setting changed that session was silently lost - which is exactly what a user hitting
	// frequent crashes sees. Every value change now stamps this clock instead, and an autosave thread flushes
	// once the user has stopped fiddling. The destructor still saves, so nothing about clean shutdown changed.
	//
	// This lives on the BASE so a single touch in BinarySetting::UpdateValueWithInput covers every setting of
	// every game - there is no per-title wiring to keep in sync and no game-specific behaviour.
	//
	// ⚠ SUPPRESSED DURING LOAD. deserialise() pushes values in through the same mutation path, which would
	// otherwise mark the config dirty before the user has touched anything and trigger a pointless write.
	static inline std::atomic<bool> s_suppressDirty{ false };
	static inline std::atomic<bool> s_dirty{ false };
	static inline std::atomic<std::chrono::steady_clock::time_point> s_lastChange{};

	static void markDirty() noexcept
	{
		if (s_suppressDirty.load(std::memory_order_acquire)) return;
		s_lastChange.store(std::chrono::steady_clock::now(), std::memory_order_release);
		s_dirty.store(true, std::memory_order_release);
	}

	struct DirtySuppressor
	{
		DirtySuppressor() { s_suppressDirty.store(true, std::memory_order_release); }
		~DirtySuppressor() { s_suppressDirty.store(false, std::memory_order_release); }
	};
};