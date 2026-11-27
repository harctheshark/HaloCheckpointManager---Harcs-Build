#include "pch.h"
#include "HotkeyRendererImpl.h"




void HotkeyRendererImpl::renderHotkey(std::optional<RebindableHotkeyEnum> hotkeyEnum, int pixelWidth)
{


	if (!hotkeyEnum.has_value())
	{
		ImGui::Dummy({ pixelWidth + 30.f,GUIFrameHeight }); // padding
	}
	else
	{
		if (!hotkeyDefinitions->getAllRebindableHotkeys().contains(hotkeyEnum.value()))
		{
			LOG_ONCE(PLOG_ERROR << "Error: bad hotkey passed to beginHotkeyRebindDialog");
			ImGui::Dummy({ pixelWidth + 30.f,GUIFrameHeight }); // padding
			return;
		}

		auto hotkey = hotkeyDefinitions->getAllRebindableHotkeys().at(hotkeyEnum.value());

		ImGui::PushID(hotkey->getName().data());
		ImGui::BeginChild("HotkeyChild", { (float)pixelWidth, GUIFrameHeight });
		ImGui::AlignTextToFramePadding();
		ImGui::Text(hotkey->getBindingTextShort().data());
		addTooltip(std::format("{} hotkeys: {}", hotkey->getName().data(), hotkey->getBindingText().data()).c_str());
		ImGui::EndChild();


		ImGui::SameLine();

		// button for rebinding hotkey
		if (ImGui::Button("..")) 
		{	
			ImGui::PopID(); // pop id before creating dialog
			PLOG_DEBUG << "creating hotkeyRebindDialog";
			hotkeyRebindDialog = std::make_unique<HotkeyRebindDialog>(hotkey, hotkeyDisabler->makeScopedRequest());
		}
		else
		{
			ImGui::PopID(); // pop id 
			// You'd think we could just put the PopID call before the ImGui::Button, but the button needs the id too.
		}
		addTooltip(std::format("Rebind hotkeys for {}", hotkey->getName().data()).c_str());


	}
}

std::string HotkeyRendererImpl::getBindingTextShort(RebindableHotkeyEnum hotkeyEnum)
{
	const auto& all = hotkeyDefinitions->getAllRebindableHotkeys();
	const auto found = all.find(hotkeyEnum);
	if (found == all.end() || !found->second) return {};
	return std::string(found->second->getBindingTextShort());
}

// Same test HotkeyManager uses to fire a hotkey: every key of one binding held, at least one of them newly pressed.
// For text boxes that must not take the character of the key that is about to close the window.
bool HotkeyRendererImpl::isPressedNow(RebindableHotkeyEnum hotkeyEnum)
{
	const auto& all = hotkeyDefinitions->getAllRebindableHotkeys();
	const auto found = all.find(hotkeyEnum);
	if (found == all.end() || !found->second) return false;
	for (const auto& bindingSet : found->second->getBindings())
	{
		if (bindingSet.empty()) continue;
		bool allDown = true, newPress = false;
		for (auto key : bindingSet)
		{
			if (!ImGui::IsKeyDown(key)) { allDown = false; break; }
			if (ImGui::IsKeyPressed(key, false)) newPress = true;
		}
		if (allDown && newPress) return true;
	}
	return false;
}

void HotkeyRendererImpl::renderHotkeyRebindDialog(SimpleMath::Vector2 screenSize)
{
	if (hotkeyRebindDialog)
	{
		// render dialog
		hotkeyRebindDialog->render(screenSize);

		// close if done
		if (hotkeyRebindDialog->isFinished())
		{
			PLOG_DEBUG << "closing Hotkey Rebind Dialog";
			hotkeyRebindDialog.reset();
			hotkeyManager->saveHotkeysToFile();

		}
			


	}
}