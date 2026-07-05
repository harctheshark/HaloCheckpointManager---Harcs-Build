#include "pch.h"
#include "SettingsSerialiser.h"
#include "SettingsStateAndEvents.h"
#include "MessagesGUI.h"
#include <pugixml.hpp>


// need to get some xml code up in here

constexpr std::string_view configFileName = "HCMInternalConfig.xml";

void throwOnDuplicateName(pugi::xml_document& doc)
{
	// recursive interface to test if all tag names with the same parent are unique 
	struct simple_walker : pugi::xml_tree_walker
	{
;
		virtual bool for_each(pugi::xml_node& node)
		{
			if (node.name() == "") return true;

			std::set<std::string> checkForDuplicateSymbols;
			for (pugi::xml_node child : node.children())
			{	
				if (child.type() == pugi::node_null) continue;
				if (checkForDuplicateSymbols.emplace(child.name()).second)
				{
					continue;
				}
				else
				{
					throw HCMSerialisationException(std::format("Non-unique setting name \"{}\"! Burnt made an oopsie", child.name()));
				}
			}
		}
	}walker;
	doc.traverse(walker);
}




// Shared writer for both the main config (shared_ptr list) and presets (raw-pointer list). option-> works for
// either, so one template covers both.
namespace
{
	template<typename Container>
	void writeSettingsFile(const std::string& filePath, Container& options, RuntimeExceptionHandler* runtimeExceptions)
	{
		PLOG_DEBUG << "Saving settings to filepath: " << filePath;
		try
		{
			pugi::xml_document doc;
			auto optionArray = doc.append_child(nameof(Setting));

			for (auto& option : options)
			{
				PLOG_DEBUG << "Serialising setting: " << option->getOptionName();
				option->serialise(optionArray);
			}

			throwOnDuplicateName(doc);

			if (!doc.save_file(filePath.c_str()))
				PLOG_ERROR << "Error saving config to " << filePath;
			else
				PLOG_DEBUG << "Successfully saved settings to filepath: " << filePath;
		}
		catch (HCMSerialisationException& ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
}

void SettingsSerialiser::serialise(std::vector<std::shared_ptr<SerialisableSetting>>& allSerialisableOptions)
{
	writeSettingsFile(mDirPath + configFileName.data(), allSerialisableOptions, runtimeExceptions.get());
}

void SettingsSerialiser::serialiseToPath(const std::string& filePath, const std::vector<SerialisableSetting*>& allPresetOptions)
{
	writeSettingsFile(filePath, allPresetOptions, runtimeExceptions.get());
}

void SettingsSerialiser::deserialise(std::vector<std::shared_ptr<SerialisableSetting>>& allSerialisableOptions)
{
	std::string filePath = mDirPath + configFileName.data();
	PLOG_DEBUG << "filePath to find setting serialisation file: " << filePath;
	pugi::xml_document doc;
			
	pugi::xml_parse_result result = doc.load_file(filePath.c_str());
	PLOG_DEBUG << "setting file parse result: " << (result.operator bool() ? "true" : "false");
	if (result)
	{
		try
		{
			auto optionArray = doc.child(nameof(Setting));
			if (optionArray.type() == pugi::node_null) 
				throw HCMSerialisationException("Could not find OptionArray node");
			for (auto& option : allSerialisableOptions)
			{
				PLOG_VERBOSE << "Deserialising setting: " << option->getOptionName();
				auto optionXML = optionArray.child(option->getOptionName().c_str());
				if (optionXML.type() == pugi::node_null)
				{
					messagesGUI->addMessage(std::format("Couldn't find saved value of option: {}, using default value.", option->getOptionName()));
					continue;
				}

				PLOG_VERBOSE << "with value: [" << optionXML.text() << "]";
				option->deserialise(optionXML);
			}
		}
		catch (HCMSerialisationException& ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}
	else
	{
		PLOG_ERROR << "Error parsing settings file";
		std::string resultString = result.description();
		if (resultString == "File was not found")
		{
			messagesGUI->addMessage("Config file not found, loading default settings.");
		}
		else
		{
			std::string err = std::format("Error parsing file at {}\nError description: {}\nError offset: {}", filePath, resultString, result.offset);
			HCMSerialisationException ex(err);
			runtimeExceptions->handleMessage(ex);
		}


	}
}


// Preset load: apply a full settings snapshot from an arbitrary file. Unlike the main-config deserialise, this is
// QUIET on missing options (a setting absent from the preset simply keeps its current value - no per-option spam),
// which lets partial/older presets load cleanly. Each option->deserialise fires that setting's valueChangedEvent,
// so features react live. MUST be called on the render thread (same thread the GUI toggles fire on).
void SettingsSerialiser::deserialiseFromPath(const std::string& filePath, const std::vector<SerialisableSetting*>& allPresetOptions)
{
	PLOG_DEBUG << "Loading preset from filepath: " << filePath;
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filePath.c_str());
	if (!result)
	{
		std::string err = std::format("Couldn't read preset file at {}\nError: {}", filePath, result.description());
		HCMSerialisationException ex(err);
		runtimeExceptions->handleMessage(ex);
		return;
	}

	try
	{
		auto optionArray = doc.child(nameof(Setting));
		if (optionArray.type() == pugi::node_null)
			throw HCMSerialisationException("Preset file has no Setting node (not a valid HCM preset)");

		int applied = 0;
		for (auto& option : allPresetOptions)
		{
			auto optionXML = optionArray.child(option->getOptionName().c_str());
			if (optionXML.type() == pugi::node_null)
				continue; // absent from this preset - leave the setting untouched
			option->deserialise(optionXML);
			++applied;
		}
		PLOG_DEBUG << "Preset applied " << applied << " settings from " << filePath;
	}
	catch (HCMSerialisationException& ex)
	{
		runtimeExceptions->handleMessage(ex);
	}
}




