#include "vk_win_policy.h"

#include "active_profile.h"
#include "guest_va.h"
#include "host.h"
#include "host_policy.h"
#include "guest_abi.h"
#include "loading_hd.h"
#include "perf_sections.h"

#define RASTER_LOCK GVA(0x00503400)
#define RASTER_WIN_LOCK GVA(0x00504D20)
#define RASTER_WIN_LOCK_THUNK GVA(0x00504D40)

uint32_t vkp_lock_caller(void) {
    const uint32_t wrappers[] = { RASTER_LOCK, RASTER_WIN_LOCK, RASTER_WIN_LOCK_THUNK };
    uint32_t caller = g_cur_func, found;
    if (caller != RASTER_LOCK && caller != RASTER_WIN_LOCK && caller != RASTER_WIN_LOCK_THUNK) return caller;
    found = icall_ring_caller(g_icall_trace, g_icall_from, g_icall_trace_idx, ICALL_TRACE_SIZE, wrappers, 3);
    return found ? found : caller;
}

void vkp_classify_access(cpu_access_t access, uint32_t lock_flags, vkp_access_t* out) {
    out->text_overlay = access == CPU_ACCESS_GETDC && game_text_drawing();
    out->caller = out->text_overlay ? GETDC_CALLER_TEXT_OVERLAY : vkp_lock_caller();
    out->reads = target_access_reads(access, lock_flags, out->caller, ui_screen_blt_writes_only());
    out->inert = access == CPU_ACCESS_LOCK && out->caller == LOCK_CALLER_FLUSH_BATCH;
}

int vkp_polygon_caller_ok(uint32_t caller) { return target_access_reads(CPU_ACCESS_LOCK, 0, caller, 1); }

int vkp_polygon_store_only(uint32_t caller) {
    return cpu_polygon_store_only_caller(caller) >= 0 && jpt_active_lift() == JPT_LIFT_116;
}

void vkp_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch) { cpu_polygon_to_surface(box, origin_bytes, pitch); }

int vkp_text_boxes_known(void) { return game_text_boxes_known(); }

int vkp_gameplay_now(void) {
    guest_ui_view_t v;
    guest_ui_probe(&v);
    return v.ui == GUEST_UI_GAMEPLAY;
}

int vkp_ui_canvas_present_held(void) { return ui_canvas_present_held(); }
int vkp_loading_hd_present_held(void) { return loading_hd_present_held(); }
int vkp_window_host_view_size(unsigned* w, unsigned* h) {
    int iw = 0, ih = 0;
    window_host_view_size(&iw, &ih);
    if (iw <= 0 || ih <= 0) return 0;
    if (w) *w = (unsigned)iw;
    if (h) *h = (unsigned)ih;
    return 1;
}
float vkp_view_tan_half_height(void) { return widescreen_view_tan_half_height(); }
int vkp_guest_section_count(void) { return PS_COUNT; }
const char* vkp_guest_section_name(int index) { return perf_section_name((perf_section_t)index); }
double vkp_guest_section_ms(int index) { return perf_section_total_ms((perf_section_t)index); }
