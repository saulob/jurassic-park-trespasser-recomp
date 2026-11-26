#pragma once

#include <algorithm>

namespace jpt {

// Where the loading view puts its text and spinner: centered in the space left, in proportion to the UI scale.
struct LoadingLayout {
    float text_x = 0, text_y = 0;
    float spinner_x = 0, spinner_y = 0;
    float radius = 0, thickness = 0;
    float bottom = 0; // of the spinner
};

inline LoadingLayout LayOutLoading(float scale, float min_x, float min_y, float avail_x, float avail_y, float text_w, float text_h) {
    LoadingLayout l;
    l.radius = 10.0f * scale;
    l.thickness = 3.0f * scale;
    const float gap = 14.0f * scale;
    const float block = text_h + gap + l.radius * 2.0f;
    const float center_x = min_x + avail_x * 0.5f, top = min_y + std::max(0.0f, (avail_y - block) * 0.5f);
    l.text_x = center_x - text_w * 0.5f;
    l.text_y = top;
    l.spinner_x = center_x;
    l.spinner_y = top + text_h + gap + l.radius;
    l.bottom = l.spinner_y + l.radius;
    return l;
}

} // namespace jpt
