#pragma once
#include "pch.h"
#include "GameState.h"
#include "GUIElementStore.h"
#include "HotkeyRenderer.h"
#include "SettingsStateAndEvents.h"
#include "ControlServiceContainer.h"

// The NEW HCM window layout ("command palette + compact dashboard"):
//
//   status line            current game / level, [Classic layout] button, [N active] button
//   search bar             type to filter every option of the current game (label, path, tooltip, aliases)
//   Home | Favourites | Browse all
//   body                   tiles / full controls / the category's existing subtree / ranked search results
//   key-hint footer
//
// It draws INSIDE the HCM window that HCMInternalGUI has already begun, and it draws every option with that
// option's EXISTING widget (IGUIElement::render) - it never re-implements what an option does. What is new is
// only how you get to an option. The classic layout is untouched and is what HCMInternalGUI falls back to if
// anything in here throws.
//
// Everything is driven by the per-game element tree in GUIElementStore, so it covers every game HCM supports
// with no per-option table. The two small tables that do exist (search aliases, the "Theater Replay" category)
// are keyed by GUIElementEnum and are optional extras: an option missing from them still works.
class HCMPaletteGUI
{
public:
	HCMPaletteGUI(std::shared_ptr<GUIElementStore> store, std::shared_ptr<HotkeyRenderer> hotkeyRenderer,
		std::shared_ptr<SettingsStateAndEvents> settings, std::shared_ptr<ControlServiceContainer> control);
	~HCMPaletteGUI();

	struct FrameResult
	{
		float desiredWindowHeight = 300.f; // what the window wants to be next frame (caller clamps to its cap)
		bool switchToClassic = false;      // the user pressed "Classic layout"
	};

	// Call between ImGui::Begin and ImGui::End of the HCM window, on the render thread.
	//   renderStatusText  draws the existing "Current Game: ..." header text (one Text item)
	//   maxWindowHeight   the cap the whole window must stay under (classic's 2/3 of the screen)
	//   cursorShowing     false while the game owns the mouse: the search box then refuses / drops the keyboard
	FrameResult render(GameState game, const std::function<void()>& renderStatusText, float maxWindowHeight, bool cursorShowing);

	// Call every frame the window is collapsed or not drawn: hands the keyboard back (releases the hotkey-disable
	// and block-game-input requests) and forgets a pending "focus search" hotkey press.
	void onWindowHidden();

private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
};
