#pragma once
#include "imgui.h"
#include <cstring>

// HCM addition (2026-11-18, user: the skull group's little tree arrow "gets lost" among the other options - "I'm used to seeing
// the symbol next to it"). An expandable row drawn LIKE the tick-box rows around it: a framed square with a tick box's size and
// colours, holding an arrow (pointing right = closed, down = open), then the name. Click the box or the name to open / close.
// Returns `open`. Text after "##" is not shown (ImGui's label rule); the whole label is the ID. The row is one ImGui item, so
// IsItemHovered() right after it (the tooltip) works as after a tick box.
inline bool GUIExpanderBox(const char* label, bool& open)
{
	const ImGuiStyle& st = ImGui::GetStyle();
	const float sz = ImGui::GetFrameHeight();
	const char* shownEnd = std::strstr(label, "##");
	if (!shownEnd) shownEnd = label + std::strlen(label);
	const ImVec2 textSize = ImGui::CalcTextSize(label, shownEnd);
	const ImVec2 p = ImGui::GetCursorScreenPos();

	ImGui::PushID(label);
	if (ImGui::InvisibleButton("##expander", ImVec2(sz + st.ItemInnerSpacing.x + textSize.x, sz))) open = !open;
	const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	ImGui::PopID();

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), ImGui::GetColorU32(held ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), st.FrameRounding);
	const ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.5f);
	const float r = sz * 0.30f;
	const ImU32 col = ImGui::GetColorU32(ImGuiCol_CheckMark);
	if (open) dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.8f), col);
	else dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x + r * 0.8f, c.y), ImVec2(c.x - r * 0.6f, c.y + r), col);
	dl->AddText(ImVec2(p.x + sz + st.ItemInnerSpacing.x, p.y + st.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label, shownEnd);
	return open;
}
