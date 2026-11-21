/*
 * The Windows side of the host's menus (debug_menu.c): the game window's key and mouse messages, and the Direct3D 11 panel
 * and HUD drawn over the frame.
 */
#include "debug_menu.h"
#include "host.h"

#include <string.h>

#include "debug_menu_panel.h"

int debug_menu_key(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return 0;
    return debug_menu_key_event((unsigned)wparam, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, (lparam & 0x40000000) != 0,
                                GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0);
}

int debug_menu_mouse(UINT msg, int x, int y) {
    if (msg == WM_MOUSEMOVE) return debug_menu_pointer(0, x, y);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) return debug_menu_pointer(1, x, y);
    return debug_menu_visible();
}

void debug_menu_windows_init(void) { debug_menu_set_hooks(window_host_input_changed, dmp_hits); }

int debug_menu_overlay_is_wanted(void) { return debug_menu_overlay_wanted(window_host_frame_counter_on()); }

static float dpi_scale(void) {
    static float scale;
    if (!scale) {
        HDC dc = GetDC(NULL);
        scale = dc ? (float)GetDeviceCaps(dc, LOGPIXELSY) / 96.0f : 1.0f;
        if (dc) ReleaseDC(NULL, dc);
        if (scale < 1.0f) scale = 1.0f;
    }
    return scale;
}

/* The compass sits in the 3D picture the game draws, which its view-size setting can leave smaller than the surface. */
static void scene_view(dmp_frame_t* f) {
    float v[4];
    if (!d3dr_scene_view(v) || v[0] < 0.0f || v[1] < 0.0f || v[0] + v[2] > 1.001f || v[1] + v[3] > 1.001f) return;
    f->scene_x = v[0] * f->view_w;
    f->scene_y = v[1] * f->view_h;
    f->scene_w = v[2] * f->view_w;
    f->scene_h = v[3] * f->view_h;
}

void debug_menu_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context,
                        struct ID3D11RenderTargetView* target, int width, int height) {
    dmp_frame_t f;
    dmp_text_layer_t text;
    char counter[32];
    RECT view;
    if (!debug_menu_overlay_is_wanted() || !window_host_view_rect(&view)) return;
    memset(&f, 0, sizeof f);
    f.view_w = (float)(view.right - view.left);
    f.view_h = (float)(view.bottom - view.top);
    f.dpi_scale = dpi_scale();
    scene_view(&f);
    debug_menu_fill_frame(&f);
    if (f.hud && present_take_text_layer(&text.texture, &text.legacy_w, &text.legacy_h, &text.left, &text.top, &text.right,
                                         &text.bottom))
        f.text_layer = &text;
    if (window_host_frame_counter_text(counter, sizeof counter)) f.counter = counter;
    dmp_draw(device, context, target, width, height, &f);
}

void debug_menu_release(void) { dmp_release(); }
