/* The host functions the Windows Vulkan backend calls, for the benchmark and nothing else. */
#include <windows.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/vk_win_policy.h"
#include "host.h"
#include "text_layer.h"

void host_log(const char* tag, const char* fmt, ...) {
    (void)tag;
    (void)fmt;
}
HWND window_host_surface(HWND window) { return window; }
int window_host_frames_allowed(void) { return 0; }
void window_host_frame_commit(unsigned generation) { (void)generation; }
void window_host_frame_presented(void) {}
void video_hd_main_presented(void) {}
int video_hd_present(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma) {
    (void)window; (void)bits; (void)pitch; (void)source; (void)rgb565; (void)gamma;
    return 0;
}
const video_subtitle_image_t* video_subtitles_for_screen(int screen_w, int screen_h, int target_w, int target_h) {
    (void)screen_w; (void)screen_h; (void)target_w; (void)target_h;
    return NULL;
}
int debug_menu_overlay_is_wanted(void) { return 0; }
int debug_menu_overlay_gpu(struct dmp_gpu_draw_s* out, int width, int height) {
    (void)out;
    (void)width;
    (void)height;
    return -1;
}

int debug_menu_overlay_rgba(uint8_t* rgba, int width, int height, overlay_tiles_t* tiles) {
    if (tiles && tiles->mask) memset(tiles->mask, 1, (size_t)tiles->tiles_w * (size_t)tiles->tiles_h);
    (void)rgba; (void)width; (void)height;
    return 0;
}
void debug_menu_release(void) {}
void engine_limits_summary(unsigned long long hardware_draw_calls) { (void)hardware_draw_calls; }
void d3d_objects_counts(unsigned long* render_states, unsigned long* stage_states, unsigned long* failed_calls) {
    if (render_states) *render_states = 0;
    if (stage_states) *stage_states = 0;
    if (failed_calls) *failed_calls = 0;
}
float widescreen_tan_half_height(void) { return 0.57735f; }

void vkp_classify_access(cpu_access_t access, uint32_t lock_flags, vkp_access_t* out) {
    (void)access; (void)lock_flags;
    out->caller = 0;
    out->reads = 1;
    out->inert = 0;
    out->text_overlay = 0;
}
uint32_t vkp_lock_caller(void) { return 0; }
int vkp_polygon_caller_ok(uint32_t caller) { (void)caller; return 1; }
int vkp_polygon_store_only(uint32_t caller) { (void)caller; return 0; }
void vkp_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch) { (void)box; (void)origin_bytes; (void)pitch; }
int vkp_text_boxes_known(void) { return 0; }
int vkp_gameplay_now(void) { return 1; }
int vkp_ui_canvas_present_held(void) { return 0; }
int vkp_loading_hd_present_held(void) { return 0; }
int vkp_window_host_view_size(unsigned* w, unsigned* h) { (void)w; (void)h; return 0; }
float vkp_view_tan_half_height(void) { return 0.57735f; }
int vkp_guest_section_count(void) { return 0; }
const char* vkp_guest_section_name(int index) { (void)index; return ""; }
double vkp_guest_section_ms(int index) { (void)index; return 0.0; }
int dd_format_bytes(const DDPIXELFORMAT* pf) { return pf ? (int)(pf->dwRGBBitCount / 8) : 2; }
