#pragma once
#include "pch.h"
#include "GameState.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "HotkeyRenderer.h"
#include "ToolTipCollection.h"
class IGUIElement
{
protected:
	std::optional<RebindableHotkeyEnum> mHotkey;
	float currentHeight = 0;
	GameState mImplGame;
	bool requiredServicesReady = true;
	ToolTipCollection toolTipCollection;


public:
	float getCurrentHeight() { return this->currentHeight; }
	virtual void render(HotkeyRenderer&) = 0;

	IGUIElement(GameState implGame, std::optional<RebindableHotkeyEnum> hotkey, ToolTipCollection tooltip)
		: mImplGame(implGame), mHotkey(hotkey), toolTipCollection(tooltip)
	{
	
	}

	virtual std::string_view getName() = 0;

	virtual ~IGUIElement() = default;

	std::optional<RebindableHotkeyEnum> getHotkey() { return mHotkey; }

	// ---- New layout (search / favourites) support --------------------------------------------------------------
	// The new window lists every element by name and needs to (a) show whether it is on and (b) toggle or fire it
	// from the keyboard WITHOUT drawing its widget. The defaults describe "a widget with no one-key action", so a
	// widget that does not override these simply has no quick action - it is still reachable and drawn as itself.
	// Nothing in the classic layout calls any of this.
	enum class QuickKind { None, Toggle, Button };
	virtual QuickKind quickKind() { return QuickKind::None; }
	virtual bool quickIsOn() { return false; }      // Toggle only: the live value
	virtual void quickActivate() {}                 // Toggle: flip it. Button: fire it. Same path as a click.
	std::string_view getTooltipText() { return toolTipCollection.getToolTip(mImplGame); }

	void renderTooltip()
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			// wrapped (2026-11-05): SetTooltip never wraps, so long descriptions ran off the screen; TextUnformatted also means a
			// '%' in a description is printed instead of being read as a format directive
			const std::string_view tip = toolTipCollection.getToolTip(mImplGame);
			if (!tip.empty() && ImGui::BeginTooltip())
			{
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.f);
				ImGui::TextUnformatted(tip.data(), tip.data() + tip.size());
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}
	}
};

template <typename T>
concept GUIElementBaseTemplate = std::is_base_of<IGUIElement, T>::value;


