#pragma once
#include "HotkeysEnum.h"
#include "imgui.h"
#include <cmath>

// Renders hotkeys and rebinds them (by popup dialog)

class IHotkeyRendererImpl {
public:
	virtual void renderHotkey(std::optional<RebindableHotkeyEnum>, int pixelWidth) = 0;
	virtual std::string getBindingTextShort(RebindableHotkeyEnum) = 0; // "" when unbound or unknown
	virtual bool isPressedNow(RebindableHotkeyEnum) = 0; // one of its bindings went down this frame (render thread)
	virtual ~IHotkeyRendererImpl() = default;
};


class HotkeyRenderer
{
private:
	std::unique_ptr<IHotkeyRendererImpl> pimpl;
	int mProbeCalls = 0;
	float mProbeX = 0.f, mProbeY = 0.f;   // the cursor (screen) at the first renderHotkey call since probeReset()

public:
	HotkeyRenderer(std::unique_ptr<IHotkeyRendererImpl> impl) : pimpl(std::move(impl)) 
	{
		PLOG_DEBUG << "HotkeyRenderer constructor";
	}
	~HotkeyRenderer()
	{
		PLOG_DEBUG << "~HotkeyRenderer()";
	}

	// renders a hotkey if we have it, or blank padding space if we don't. If you don't want either than just don't call renderHotkey.
	void renderHotkey(std::optional<RebindableHotkeyEnum> h, int pixelWidth = 63)
	{
		if (mProbeCalls++ == 0) { const ImVec2 at = ImGui::GetCursorScreenPos(); mProbeX = at.x; mProbeY = at.y; }
		pimpl->renderHotkey(h, pixelWidth);
	}

	// "Did the widget just drawn START with the hotkey column?" (2026-11-18) - for the new layout, which lines every row's
	// control up in one column: a widget that draws no hotkey column gets the same blank space in front. probeReset()
	// before the widget's render(), probeLedAt(where its row started) after it. SCREEN positions: a widget may open its own
	// child window before it draws the column (GUIToggleWithChildren), and a child's cursor is in the child's coordinates.
	void probeReset() { mProbeCalls = 0; }
	bool probeLedAt(const ImVec2& rowStart) const { return mProbeCalls > 0 && std::fabs(mProbeX - rowStart.x) < 3.f && std::fabs(mProbeY - rowStart.y) < 3.f; }

	// just the short binding text (what renderHotkey prints), for the new layout's hotkey chips. Draws nothing.
	std::string getBindingTextShort(RebindableHotkeyEnum h) { return pimpl->getBindingTextShort(h); }
	bool isPressedNow(RebindableHotkeyEnum h) { return pimpl->isPressedNow(h); }

};

