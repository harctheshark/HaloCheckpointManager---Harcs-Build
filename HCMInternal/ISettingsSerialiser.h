#pragma once

#include "BinarySetting.h"
class ISettingsSerialiser
{
public:
	virtual void serialise(std::vector<std::shared_ptr<SerialisableSetting>>& allSerialisableOptions) = 0;
	virtual void deserialise(std::vector<std::shared_ptr<SerialisableSetting>>& allSerialisableOptions) = 0;

	// Preset support: save/load a COMPLETE settings snapshot to/from an arbitrary file path (not the main config).
	// Takes the raw-pointer registry (every setting, including the cheat toggles that allSerialisableOptions omits).
	// deserialiseFromPath leaves any setting absent from the file at its CURRENT value (quiet on missing).
	virtual void serialiseToPath(const std::string& fullFilePath, const std::vector<SerialisableSetting*>& allPresetOptions) = 0;
	virtual void deserialiseFromPath(const std::string& fullFilePath, const std::vector<SerialisableSetting*>& allPresetOptions) = 0;

	~ISettingsSerialiser() = default;
};