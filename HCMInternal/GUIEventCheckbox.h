#pragma once
#include "IGUIElement.h"
#include "SettingsStateAndEvents.h"
#include <functional>

// HCM addition (2026-11-18): a CHECKBOX for an option that is really an action with a state - ticking or unticking it fires an
// event (exactly as GUISimpleButton does), and the tick shows what a caller-supplied function says the state is now. Made for
// "Toggle 30 Tick" (was the button "Toggle Tickrate (60/30)": the only on / off option in the list that was not a tick box).
// The box never keeps a value of its own: if the event changes nothing (no level loaded), the tick simply stays as it was.
template< bool shouldRenderHotkey>
class GUIEventCheckbox : public IGUIElement {

private:
	std::string mText;
	std::weak_ptr<ActionEvent> mEventToFireWeak;
	std::function<bool()> mIsOn;
	std::vector<std::thread> mFireEventThreads;

	void fire(const std::shared_ptr<ActionEvent>& ev)
	{
		auto& newThread = mFireEventThreads.emplace_back(std::thread([mEvent = ev]() { mEvent->operator()(); }));
		newThread.detach();
	}
	bool isOn() { try { return mIsOn && mIsOn(); } catch (...) { return false; } }

public:
	GUIEventCheckbox(GameState implGame, ToolTipCollection tooltip, std::optional<RebindableHotkeyEnum> hotkey, std::string text, std::shared_ptr<ActionEvent> eventToFire, std::function<bool()> isOn)
		: IGUIElement(implGame, hotkey, tooltip), mText(text), mEventToFireWeak(eventToFire), mIsOn(std::move(isOn))
	{
		if (mText.empty()) throw HCMInitException("Cannot have empty checkbox text (needs label for imgui ID system, use ## for invisible labels)");
		PLOG_VERBOSE << "Constructing GUIEventCheckbox, name: " << getName();
		this->currentHeight = GUIFrameHeightWithSpacing;
	}

	void render(HotkeyRenderer& hotkeyRenderer) override
	{
		auto mEventToFire = mEventToFireWeak.lock();
		if (!mEventToFire)
		{
			PLOG_ERROR << "bad mEventToFire weakptr when rendering " << getName();
			return;
		}

		if constexpr (shouldRenderHotkey)
		{
			hotkeyRenderer.renderHotkey(mHotkey);
			ImGui::SameLine();
		}

		bool shown = isOn();   // a copy: the state is the event's to change, not the box's
		if (ImGui::Checkbox(mText.c_str(), &shown))
		{
			PLOG_VERBOSE << "GUIEventCheckbox (" << getName() << ") firing event";
			fire(mEventToFire);
		}
		renderTooltip();
		DEBUG_GUI_HEIGHT;
	}

	~GUIEventCheckbox()
	{
		for (auto& thread : mFireEventThreads)
		{
			if (thread.joinable())
			{
				PLOG_DEBUG << getName() << "joining mFireEventThread";
				thread.join();
				PLOG_DEBUG << getName() << "FireEventThread finished";
			}
		}
	}

	std::string_view getName() override { return mText; }

	// new layout: a toggle in search results / favourite tiles, fired the same way as a click on the box
	QuickKind quickKind() override { return QuickKind::Toggle; }
	bool quickIsOn() override { return isOn(); }
	void quickActivate() override
	{
		auto ev = mEventToFireWeak.lock();
		if (ev) fire(ev);
	}
};
