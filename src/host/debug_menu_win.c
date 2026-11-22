/*
 * The Windows side of the host's menus (debug_menu.c): the game window's key and mouse messages, and the Direct3D 11 panel
 * and HUD drawn over the frame.
 */
#include "debug_menu.h"
#include "host.h"
#include "text_layer.h"

#include <stdlib.h>
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
    memset(&text, 0, sizeof text);
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

static uint32_t s_rgba565[65536];
static int s_rgba565_ready;
static uint32_t* s_sw_text_pixels;
static size_t s_sw_text_cap;

static int take_software_text_layer(dmp_text_layer_t* out) {
    tl_view_t v;
    size_t tw, th, px;
    if (!tl_current(&v) || v.w == 0 || v.h == 0 || v.dirty.right <= v.dirty.left || v.dirty.bottom <= v.dirty.top)
        return 0;
    if (!s_rgba565_ready) {
        for (uint32_t p = 0; p < 65536u; p++) {
            uint32_t r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
            uint32_t r = (r5 << 3) | (r5 >> 2);
            uint32_t g = (g6 << 2) | (g6 >> 4);
            uint32_t b = (b5 << 3) | (b5 >> 2);
            s_rgba565[p] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
        s_rgba565_ready = 1;
    }
    tw = (size_t)(v.dirty.right - v.dirty.left);
    th = (size_t)(v.dirty.bottom - v.dirty.top);
    px = tw * th;
    if (px > s_sw_text_cap) {
        uint32_t* grown = (uint32_t*)realloc(s_sw_text_pixels, px * sizeof *grown);
        if (!grown) return 0;
        s_sw_text_pixels = grown;
        s_sw_text_cap = px;
    }
    tl_expand(&v.dirty, s_rgba565, s_sw_text_pixels, tw);
    out->texture = s_sw_text_pixels;
    out->legacy_w = (int)v.w;
    out->legacy_h = (int)v.h;
    out->left = (int)v.dirty.left;
    out->top = (int)v.dirty.top;
    out->right = (int)v.dirty.right;
    out->bottom = (int)v.dirty.bottom;
    out->software_rgba = 1;
    return 1;
}

int debug_menu_overlay_rgba(uint8_t* rgba, int width, int height, overlay_tiles_t* tiles) {
    dmp_frame_t f;
    dmp_text_layer_t text;
    char counter[32];
    RECT view;
    if (!rgba || width <= 0 || height <= 0 || !debug_menu_overlay_is_wanted() || !window_host_view_rect(&view)) return 0;
    memset(&f, 0, sizeof f);
    memset(&text, 0, sizeof text);
    f.view_w = (float)(view.right - view.left);
    f.view_h = (float)(view.bottom - view.top);
    f.dpi_scale = dpi_scale();
    scene_view(&f);
    debug_menu_fill_frame(&f);
    if (f.hud && take_software_text_layer(&text)) f.text_layer = &text;
    if (window_host_frame_counter_text(counter, sizeof counter)) f.counter = counter;
    return dmp_draw_rgba(rgba, width, height, &f, tiles ? tiles->mask : NULL, tiles ? tiles->tiles_w : 0,
                         tiles ? tiles->tiles_h : 0);
}

int debug_menu_overlay_gpu(dmp_gpu_draw_t* out, int width, int height) {
    dmp_frame_t f;
    dmp_text_layer_t text;
    char counter[32];
    RECT view;
    if (!out || width <= 0 || height <= 0 || !debug_menu_overlay_is_wanted() || !window_host_view_rect(&view)) return 0;
    memset(&f, 0, sizeof f);
    memset(&text, 0, sizeof text);
    f.view_w = (float)(view.right - view.left);
    f.view_h = (float)(view.bottom - view.top);
    f.dpi_scale = dpi_scale();
    scene_view(&f);
    debug_menu_fill_frame(&f);
    if (f.hud && take_software_text_layer(&text)) f.text_layer = &text;
    if (window_host_frame_counter_text(counter, sizeof counter)) f.counter = counter;
    return dmp_build_gpu(out, width, height, &f);
}

void debug_menu_release(void) {
    dmp_release();
    free(s_sw_text_pixels);
    s_sw_text_pixels = NULL;
    s_sw_text_cap = 0;
}
