#pragma once

#include <imgui.h>

#include "loading_layout.h"
#include "localization/ui_text.h"
#include "trg/launcher.h"

namespace jpt {

// The centered text and spinner a page shows while its content is being prepared.
inline void DrawLoadingView(trg::Ui& ui, const char* english_text) {
    const char* text = Tr(english_text);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const LoadingLayout l = LayOutLoading(ui.scale(), min.x, min.y, avail.x, avail.y, text_size.x, text_size.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(l.text_x, l.text_y), ImGui::GetColorU32(ui.theme().text), text);
    const float angle = float(ImGui::GetTime()) * 5.0f;
    draw->PathArcTo(ImVec2(l.spinner_x, l.spinner_y), l.radius, angle, angle + 4.2f, 24);
    draw->PathStroke(ImGui::GetColorU32(ui.theme().accent), 0, l.thickness);
}

} // namespace jpt
