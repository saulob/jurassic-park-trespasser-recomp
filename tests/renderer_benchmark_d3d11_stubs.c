/* The host functions the Direct3D 11 backend calls, for the benchmark and nothing else. */
#include <windows.h>

#include <d3d11.h>

#include "active_profile.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "loading_hd.h"
#include "perf_sections.h"
#include "ddraw/d3d_legacy.h"
#include "ddraw/renderer_backend.h"
#include "host.h"
#include "host_policy.h"

uint32_t g_cur_func;
uint32_t g_icall_trace[128];
uint32_t g_icall_from[128];
uint32_t g_icall_trace_idx;
uint32_t jpt_gva[JPT_GVA_COUNT];

void host_log(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; }
void host_log_perf(const char* tag, const char* fmt, ...) { (void)tag; (void)fmt; }
int host_perf_logging(void) { return 0; }
jpt_lift_t jpt_active_lift(void) { return (jpt_lift_t)0; }

HWND window_host_surface(HWND window) { return window; }
int window_host_frames_allowed(void) { return 0; }
void window_host_frame_commit(unsigned generation) { (void)generation; }
void window_host_frame_presented(void) {}
void window_host_view_size(int* width, int* height) { if (width) *width = 0; if (height) *height = 0; }
float widescreen_view_tan_half_height(void) { return 0.57735f; }

void video_hd_main_presented(void) {}
void video_hd_release(void) {}
int video_hd_present(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma) {
    (void)window; (void)bits; (void)pitch; (void)source; (void)rgb565; (void)gamma;
    return 0;
}
const video_subtitle_image_t* video_subtitles_for_screen(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return NULL; }
int video_test_visible(void) { return 0; }
void video_test_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context, struct ID3D11RenderTargetView* target, int width, int height) {
    (void)device; (void)context; (void)target; (void)width; (void)height;
}
void video_test_release(void) {}
int debug_menu_overlay_is_wanted(void) { return 0; }
void debug_menu_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context, struct ID3D11RenderTargetView* target, int width, int height) {
    (void)device; (void)context; (void)target; (void)width; (void)height;
}
void debug_menu_release(void) {}
int loading_hd_present_held(void) { return 0; }
int ui_canvas_present_held(void) { return 0; }
int ui_screen_blt_writes_only(void) { return 0; }
int guest_ui_probe(guest_ui_view_t* out) { (void)out; return 0; }
int game_text_drawing(void) { return 0; }
int game_text_boxes_known(void) { return 0; }

int64_t perf_section_begin(void) { return 0; }
void perf_section_end(perf_section_t section, int64_t begin) { (void)section; (void)begin; }
unsigned long long perf_section_calls(perf_section_t section) { (void)section; return 0; }
const char* perf_section_name(perf_section_t section) { (void)section; return ""; }
double perf_section_total_ms(perf_section_t section) { (void)section; return 0.0; }
int guest_profile_running(void) { return 0; }
double guest_profile_sample_ms(void) { return 0.0; }
const char* guest_profile_slot_name(unsigned slot) { (void)slot; return ""; }
int guest_profile_slot_physics(unsigned slot) { (void)slot; return 0; }
unsigned guest_profile_slots(void) { return 0; }
void guest_profile_snapshot(unsigned* out) { (void)out; }
void guest_profile_window(void) {}
void physics_counts_take(physics_counts_t* out) { memset(out, 0, sizeof *out); }
unsigned long selfmod_entries(void) { return 0; }
void selfmod_report(double frames, double frame_ms) { (void)frames; (void)frame_ms; }
double selfmod_total_ms(void) { return 0.0; }
void cpu_polygons_frame_end(int gameplay, int slow) { (void)gameplay; (void)slow; }
void engine_limits_summary(unsigned long long hardware_draw_calls) { (void)hardware_draw_calls; }
void d3d_objects_counts(unsigned long* a, unsigned long* b, unsigned long* c) { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
void d3d_objects_load_counts(unsigned long long* loads, unsigned long long* unchanged, unsigned long long* texels,
                             unsigned long long* changed_texels) {
    (void)loads; (void)unchanged; (void)texels; (void)changed_texels;
}

int renderer_backend_guard_d3d11(const char* caller) { (void)caller; return 1; }
int d3dr_hardware(void) { return 1; }
const char* d3dr_name(void) { return "Direct3D 11"; }
void d3dr_device_lost(void) {}
void d3dr_frame_presented(double present_ms) { (void)present_ms; }
void d3dr_frontend_release(void) {}
HRESULT d3dr_clear(d3d_device_t* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z) {
    (void)dev; (void)count; (void)rects; (void)flags; (void)color; (void)z;
    return 0;
}
const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap) { (void)hr; (void)buf; (void)cap; return ""; }
