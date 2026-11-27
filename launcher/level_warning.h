// Compatibility Warning, apart from Dear ImGui: what a key or button chooses, and how the dialog fits the window at any scale.
#pragma once

#include <algorithm>

namespace jpt {

enum class WarningChoice { kNone, kCancel, kPlayAnyway };

// Cancel is the safe default: Escape and a controller's B cancel too, and nothing else starts the level.
inline WarningChoice ChooseWarning(bool cancel_pressed, bool play_pressed, bool escape, bool gamepad_back) {
    if (cancel_pressed || escape || gamepad_back) return WarningChoice::kCancel;
    return play_pressed ? WarningChoice::kPlayAnyway : WarningChoice::kNone;
}

// The dialog fits the window, its reasons scroll, and its buttons stack when they do not fit side by side.
struct WarningLayout {
    float width = 0, list_height = 0, button = 0;
    bool row = true;
};

inline WarningLayout LayOutWarning(float scale, float view_w, float view_h, float label_w, float window_padding, float spacing) {
    WarningLayout l;
    l.width = std::max(0.0f, std::min(560.0f * scale, view_w - 32.0f * scale));
    l.list_height = std::min(220.0f * scale, std::max(80.0f * scale, view_h * 0.35f));
    const float inner = std::max(0.0f, l.width - 2.0f * window_padding);
    l.button = std::max(label_w, 150.0f * scale);
    l.row = l.button * 2.0f + spacing <= inner;
    if (!l.row) l.button = inner;
    return l;
}

} // namespace jpt
