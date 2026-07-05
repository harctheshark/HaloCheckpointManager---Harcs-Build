#pragma once
#include <pugixml.hpp>
#include <vector>

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
};