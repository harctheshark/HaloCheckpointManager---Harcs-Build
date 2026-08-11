#pragma once
#include "IModalDialog.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>

// ================================================================================================================
// Halo Campaign Evolved - pick which trigger volumes the overlay draws, BY NAME, from a checkbox list.
//
// WHY THIS IS NOT MCC'S TriggerFilterStringDialog
// -----------------------------------------------
// MCC's version hands the user a free-text editor and asks them to type a semicolon-separated list of exact
// names. That is fine when you already know what the level's volumes are called; it is close to useless when
// you do not, and a HaloCER level carries upwards of 160 of them. This one shows the actual list, checked or
// unchecked, with a search box - so the names come TO the user.
//
// It reads and writes the SAME triggerOverlayFilterString setting MCC's dialog does (a semicolon-separated
// exact-name list) rather than inventing a second format. HaloCER and MCC can never share a process, so a
// preset saved on one is meaningful on the other, and nothing downstream has to learn a new encoding.
//
// ⚠ THE EMPTY STRING MEANS "EVERYTHING", NOT "NOTHING". That asymmetry is deliberate and is what makes the
// feature inert until it is deliberately used: switching the name filter on with an empty list must not blank
// the overlay and leave the user wondering what broke. "None" therefore writes a sentinel rather than an empty
// string - see kNoneSentinel.
// ================================================================================================================
class HCETriggerNameFilterDialog : public IModalDialogReturner<std::string>
{
public:
	// Mirrors HCETriggerOverlay's own classification, in the SAME priority order it colours by, so a volume is
	// listed under exactly the category whose colour it wears.
	enum class Category { ZoneSet, Kill, SafeZone, Sector, Regular };

	struct Entry
	{
		std::string name;
		Category category = Category::Regular;
		bool speedrun = false;
	};

	// A filter string that means "draw nothing". An empty string already means "draw everything", so None needs
	// a value of its own; this one cannot collide with a real volume name (they are string_ids, no spaces).
	static constexpr const char* kNoneSentinel = "##none##";

private:
	std::string mOriginalValue;
	std::vector<Entry> mEntries;
	std::set<std::string> mChecked;
	char mSearch[128]{};

	static std::vector<std::string> split(const std::string& value)
	{
		std::vector<std::string> out;
		std::string current;
		for (char c : value)
		{
			if (c == ';' || c == '\n' || c == '\r') { if (!current.empty()) out.push_back(current); current.clear(); }
			else if (c != ' ' || !current.empty()) current += c;
		}
		if (!current.empty()) out.push_back(current);
		for (auto& s : out) while (!s.empty() && s.back() == ' ') s.pop_back();
		return out;
	}

	static std::string join(const std::set<std::string>& values)
	{
		std::string out;
		for (const auto& v : values)
		{
			if (!out.empty()) out += ';';
			out += v;
		}
		return out;
	}

	static bool containsInsensitive(const std::string& haystack, const char* needle)
	{
		if (!needle || !*needle) return true;
		const std::string n(needle);
		auto it = std::search(haystack.begin(), haystack.end(), n.begin(), n.end(),
			[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
		return it != haystack.end();
	}

	static const char* categoryName(Category c)
	{
		switch (c)
		{
		case Category::ZoneSet:  return "Zone-set / BSP switch";
		case Category::Kill:     return "Kill volumes";
		case Category::SafeZone: return "Safe zones";
		case Category::Sector:   return "Sector volumes";
		default:                 return "Regular volumes";
		}
	}

	// Everything currently PASSING the search box, so All/None act on what the user can actually see. Acting on
	// the whole level while a search is narrowing the list would be a nasty surprise.
	void setVisible(bool checked, std::optional<Category> onlyCategory = std::nullopt)
	{
		for (const auto& e : mEntries)
		{
			if (!containsInsensitive(e.name, mSearch)) continue;
			if (onlyCategory.has_value() && e.category != onlyCategory.value()) continue;
			if (checked) mChecked.insert(e.name); else mChecked.erase(e.name);
		}
	}

public:
	HCETriggerNameFilterDialog(std::string dialogTitle, std::string defaultValue, std::vector<Entry> entries)
		: IModalDialogReturner(dialogTitle, defaultValue),
		mOriginalValue(defaultValue),
		mEntries(std::move(entries))
	{
		const auto listed = split(defaultValue);
		const bool none = (listed.size() == 1 && listed[0] == kNoneSentinel);

		if (listed.empty())
		{
			// Empty means "everything", so open with everything ticked - what the overlay is doing right now.
			for (const auto& e : mEntries) mChecked.insert(e.name);
		}
		else if (!none)
		{
			for (const auto& n : listed) mChecked.insert(n);
		}

		std::sort(mEntries.begin(), mEntries.end(), [](const Entry& a, const Entry& b)
			{
				if (a.category != b.category) return (int)a.category < (int)b.category;
				return a.name < b.name;
			});
	}

	virtual void renderInternal(SimpleMath::Vector2 screenSize) override
	{
		ImGui::Text("Draw only these trigger volumes. %d of %d selected.",
			(int)mChecked.size(), (int)mEntries.size());

		if (mEntries.empty())
		{
			ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
				"No trigger volumes are loaded. Load a level with the Trigger Overlay on, then reopen this.");
		}

		ImGui::Dummy(ImVec2(4, 4));
		ImGui::SetNextItemWidth(screenSize.x / 4.f);
		ImGui::InputText("Search", mSearch, sizeof(mSearch));
		ImGui::SameLine();
		if (ImGui::Button("Clear##search")) mSearch[0] = '\0';

		const bool searching = mSearch[0] != '\0';
		if (searching) ImGui::TextDisabled("All / None below act on the %d shown, not the whole level.",
			(int)std::count_if(mEntries.begin(), mEntries.end(),
				[this](const Entry& e) { return containsInsensitive(e.name, mSearch); }));

		if (ImGui::Button("All")) setVisible(true);
		ImGui::SameLine();
		if (ImGui::Button("None")) setVisible(false);
		ImGui::SameLine();
		if (ImGui::Button("Invert"))
		{
			for (const auto& e : mEntries)
			{
				if (!containsInsensitive(e.name, mSearch)) continue;
				if (mChecked.count(e.name)) mChecked.erase(e.name); else mChecked.insert(e.name);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Speedrun only"))
		{
			for (const auto& e : mEntries)
			{
				if (!containsInsensitive(e.name, mSearch)) continue;
				if (e.speedrun) mChecked.insert(e.name); else mChecked.erase(e.name);
			}
		}

		ImGui::Dummy(ImVec2(4, 4));
		ImGui::BeginChild("##hceTriggerNameFilterList",
			ImVec2(screenSize.x / 3.f, screenSize.y / 2.5f), true);

		std::optional<Category> lastCategory;
		for (const auto& e : mEntries)
		{
			if (!containsInsensitive(e.name, mSearch)) continue;

			if (!lastCategory.has_value() || lastCategory.value() != e.category)
			{
				lastCategory = e.category;
				ImGui::Dummy(ImVec2(2, 6));
				ImGui::SeparatorText(categoryName(e.category));
				ImGui::PushID((int)e.category);
				if (ImGui::SmallButton("All##cat")) setVisible(true, e.category);
				ImGui::SameLine();
				if (ImGui::SmallButton("None##cat")) setVisible(false, e.category);
				ImGui::PopID();
			}

			bool on = mChecked.count(e.name) != 0;
			if (ImGui::Checkbox(e.name.c_str(), &on))
			{
				if (on) mChecked.insert(e.name); else mChecked.erase(e.name);
			}
			if (e.speedrun)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "(speedrun)");
			}
		}
		ImGui::EndChild();

		ImGui::Dummy(ImVec2(4, 4));
		if (ImGui::Button("Accept"))
		{
			// All ticked is exactly what "no filter" means, so collapse it back to the empty string rather than
			// writing out 160 names - that keeps presets small and keeps the setting readable.
			if (mChecked.size() == mEntries.size() && !mEntries.empty())
				currentReturnValue = "";
			else if (mChecked.empty())
				currentReturnValue = kNoneSentinel;
			else
				currentReturnValue = join(mChecked);
			closeDialog();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			currentReturnValue = mOriginalValue;
			closeDialog();
		}
	}
};
