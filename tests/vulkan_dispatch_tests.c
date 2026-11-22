/*
 * Headless runtime dispatch tests for the Windows renderer backend factory.
 * Proves that renderer=vulkan dispatches to jpt_vulkan_backend_ops (never D3D11),
 * refuses silent fallback on explicit Vulkan failure, guards against accidental D3D11
 * device creation while Vulkan is selected, and preserves D3D11, Software, and Auto.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/renderer_backend.h"
#include "ddraw/vk_win_policy.h"
#include "host.h"
#include "vk_context.h"

static int g_checks = 0;
static int g_failures = 0;
static int g_d3d11_startup_calls = 0;
static int g_sw_startup_calls = 0;
static int g_d3d11_present_calls = 0;

#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        g_checks++;                                                                          \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);                 \
            fprintf(stderr, __VA_ARGS__);                                                    \
            fputc('\n', stderr);                                                             \
            g_failures++;                                                                    \
        }                                                                                    \
    } while (0)

/* Minimal host stubs so renderer_backend.c + vk_win_renderer.c run completely headlessly without opening a window. */
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
dd_surface_t* dd_surface_from(void* iface) { (void)iface; return NULL; }
int dd_format_bytes(const DDPIXELFORMAT* pf) { return pf ? (int)(pf->dwRGBBitCount / 8) : 2; }

/* Instrumented D3D11 / Software backend stubs: detect ANY unintended call into D3D11 when Vulkan is chosen. */
static int stub_d3d11_startup(void) {
    if (!renderer_backend_guard_d3d11("stub_d3d11_startup")) return 0;
    g_d3d11_startup_calls++;
    return 1;
}
static int stub_d3d11_hw(void) { return 1; }
static const char* stub_d3d11_name(void) { return "D3D11"; }
static void stub_d3d11_present_frame(HWND w, const BYTE* b, LONG p, const RECT* s, const DDPIXELFORMAT* f,
                                     const DDGAMMARAMP* g, int gc, int v, unsigned gen) {
    (void)w; (void)b; (void)p; (void)s; (void)f; (void)g; (void)gc; (void)v; (void)gen;
    g_d3d11_present_calls++;
}
static int stub_sw_startup(void) {
    g_sw_startup_calls++;
    return 1;
}
static int stub_sw_hw(void) { return 0; }
static const char* stub_sw_name(void) { return "Software"; }

const jpt_win_backend_ops_t jpt_d3d11_backend_ops = {
    .id = JPT_RENDERER_D3D11,
    .key = "d3d11",
    .short_name = "D3D11",
    .hal_description = "Direct3D 11 hardware (Trespasser compatibility layer)",
    .startup = stub_d3d11_startup,
    .hardware = stub_d3d11_hw,
    .name = stub_d3d11_name,
    .present_frame = stub_d3d11_present_frame,
};

const jpt_win_backend_ops_t jpt_software_backend_ops = {
    .id = JPT_RENDERER_SOFTWARE,
    .key = "software",
    .short_name = "Software",
    .hal_description = "Software renderer",
    .startup = stub_sw_startup,
    .hardware = stub_sw_hw,
    .name = stub_sw_name,
};

static jpt_win_renderer_cfg_t make_cfg(jpt_renderer_t req) {
    jpt_win_renderer_cfg_t c;
    memset(&c, 0, sizeof c);
    c.requested = req;
    c.box_w = 2560;
    c.box_h = 1440;
    c.filter = D3DR_FILTER_ANISOTROPIC;
    c.anisotropy = 16;
    c.msaa = 4;
    c.vsync = 1;
    c.ao_level = JPT_AO_MEDIUM;
    c.water_refraction = 1;
    c.enhanced_sky = 1;
    return c;
}

static void test_factory_resolution(void) {
    const jpt_win_backend_ops_t* vk = renderer_backend_resolve_ops(JPT_RENDERER_VULKAN);
    const jpt_win_backend_ops_t* d3d = renderer_backend_resolve_ops(JPT_RENDERER_D3D11);
    const jpt_win_backend_ops_t* sw = renderer_backend_resolve_ops(JPT_RENDERER_SOFTWARE);

    CHECK(vk == &jpt_vulkan_backend_ops, "factory resolves Vulkan to jpt_vulkan_backend_ops");
    CHECK(d3d == &jpt_d3d11_backend_ops, "factory resolves D3D11 to jpt_d3d11_backend_ops");
    CHECK(sw == &jpt_software_backend_ops, "factory resolves Software to jpt_software_backend_ops");
    CHECK(renderer_backend_resolve_ops(JPT_RENDERER_AUTO) == NULL, "factory does not resolve AUTO directly");

    CHECK(vk != d3d && vk != sw && d3d != sw, "all three backend vtables are distinct");
    CHECK(vk->id == JPT_RENDERER_VULKAN && strcmp(vk->short_name, "Vulkan") == 0, "Vulkan vtable metadata");
    CHECK(vk->startup != NULL && vk->startup != d3d->startup, "Vulkan startup is not D3D11 startup");
    CHECK(vk->draw != NULL && vk->clear != NULL && vk->flip != NULL, "Vulkan D3D6 draw/clear/flip wired");
    CHECK(vk->present_frame != NULL && vk->present_frame != d3d->present_frame, "Vulkan present_frame is not D3D11");
    CHECK(vk->present_adapter != NULL && vk->present_release != NULL, "Vulkan adapter/release wired");
    CHECK(vk->present_video_hd != NULL, "Vulkan HD video path wired");
}

static void test_explicit_vulkan_no_silent_fallback(void) {
    jpt_win_renderer_cfg_t cfg = make_cfg(JPT_RENDERER_VULKAN);
    g_d3d11_startup_calls = 0;
    g_sw_startup_calls = 0;
    g_d3d11_present_calls = 0;

    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 1 /* D3D11 avail */, 0 /* Vulkan unavail */, "simulated ICD missing");

    CHECK(renderer_backend_requested() == JPT_RENDERER_VULKAN, "requested=vulkan");
    CHECK(renderer_backend_chosen() == JPT_RENDERER_VULKAN, "explicit Vulkan refuses silent fallback at configure");
    CHECK(renderer_backend_active() == JPT_RENDERER_AUTO, "active stays unset before startup");
    CHECK(renderer_backend_state()->ops == &jpt_vulkan_backend_ops, "ops points to Vulkan, never D3D11");
    CHECK(renderer_backend_guard_d3d11("explicit_vulkan_guard") == 0, "D3D11 guard blocks D3D11 when Vulkan chosen");

    int started = renderer_backend_startup();
    CHECK(started == 0, "startup fails cleanly when explicit Vulkan probe failed");
    CHECK(renderer_backend_active() != JPT_RENDERER_D3D11, "explicit Vulkan failure NEVER activates D3D11");
    CHECK(g_d3d11_startup_calls == 0 && g_sw_startup_calls == 0 && g_d3d11_present_calls == 0,
          "zero D3D11 or Software startup/present calls");
}

static void test_vulkan_real_dispatch(void) {
    jpt_vk_probe_result_t probe;
    memset(&probe, 0, sizeof probe);
    int vk_avail = jpt_vk_probe_headless("", &probe);

    jpt_win_renderer_cfg_t cfg = make_cfg(JPT_RENDERER_VULKAN);
    g_d3d11_startup_calls = 0;
    g_sw_startup_calls = 0;
    g_d3d11_present_calls = 0;

    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 1, 1, probe.summary);

    CHECK(renderer_backend_requested() == JPT_RENDERER_VULKAN, "requested=vulkan");
    CHECK(renderer_backend_chosen() == JPT_RENDERER_VULKAN, "chosen=vulkan");
    CHECK(renderer_backend_active() == JPT_RENDERER_AUTO, "active unset until startup succeeds");
    CHECK(renderer_backend_state()->ops == &jpt_vulkan_backend_ops, "vtable is jpt_vulkan_backend_ops");
    CHECK(renderer_backend_guard_d3d11("before_vk_startup") == 0, "D3D11 guard active before Vulkan startup");

    if (!vk_avail) {
        printf("vulkan_dispatch_tests: real GPU probe unavailable (%s), skipping live Vulkan startup check\n",
               probe.summary);
        return;
    }

    int ok = renderer_backend_startup();
    CHECK(ok == 1, "real Vulkan startup succeeded on '%s'", probe.gpus[probe.selected_index].label);
    CHECK(renderer_backend_active() == JPT_RENDERER_VULKAN, "active=vulkan after startup");
    CHECK(d3dr_hardware() == 1, "d3dr_hardware() == 1 under Vulkan");
    CHECK(strcmp(d3dr_name(), "Vulkan") == 0, "d3dr_name() == 'Vulkan' (got '%s')", d3dr_name());
    CHECK(g_d3d11_startup_calls == 0 && g_d3d11_present_calls == 0, "D3D11 never touched during Vulkan startup");
    CHECK(renderer_backend_guard_d3d11("after_vk_startup") == 0, "D3D11 guard remains active under Vulkan");

    dd_adapter_t ad;
    CHECK(present_adapter(&ad) == 1 && ad.description[0] != 0, "Vulkan adapter reported: '%s'", ad.description);
    CHECK(ad.vendor == probe.gpus[probe.selected_index].vendor_id, "adapter vendor matches probed Vulkan GPU");

    unsigned bw = 0, bh = 0;
    int fw = 0, fh = 0;
    d3dr_render_box(&bw, &bh);
    CHECK(bw == 2560 && bh == 1440, "render box preserved (got %ux%u)", bw, bh);
    CHECK(d3dr_frame_size(640, 480, &fw, &fh) == 1 && fw == 1920 && fh == 1440,
          "frame size preserves 4:3 inside 2560x1440 (got %dx%d)", fw, fh);
    CHECK(d3dr_set_debug_output("texture") == 1 && d3dr_set_debug_output("invalid") == 0,
          "debug output modes dispatch to Vulkan");
    present_release();
}

static void test_d3d11_software_and_auto(void) {
    jpt_win_renderer_cfg_t cfg;

    /* Explicit D3D11 */
    cfg = make_cfg(JPT_RENDERER_D3D11);
    g_d3d11_startup_calls = 0;
    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 1, 1, NULL);
    CHECK(renderer_backend_chosen() == JPT_RENDERER_D3D11, "D3D11 chosen");
    CHECK(renderer_backend_state()->ops == &jpt_d3d11_backend_ops, "D3D11 ops wired");
    CHECK(renderer_backend_guard_d3d11("d3d11_mode") == 1, "D3D11 guard allows D3D11 when D3D11 chosen");
    CHECK(renderer_backend_startup() == 1 && renderer_backend_active() == JPT_RENDERER_D3D11, "D3D11 active");
    CHECK(g_d3d11_startup_calls == 1 && strcmp(d3dr_name(), "D3D11") == 0, "D3D11 startup invoked");

    /* Explicit Software */
    cfg = make_cfg(JPT_RENDERER_SOFTWARE);
    g_sw_startup_calls = 0;
    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 1, 1, NULL);
    CHECK(renderer_backend_chosen() == JPT_RENDERER_SOFTWARE, "Software chosen");
    CHECK(renderer_backend_state()->ops == &jpt_software_backend_ops, "Software ops wired");
    CHECK(renderer_backend_startup() == 1 && renderer_backend_active() == JPT_RENDERER_SOFTWARE, "Software active");
    CHECK(g_sw_startup_calls == 1 && d3dr_hardware() == 0 && strcmp(d3dr_name(), "Software") == 0,
          "Software startup invoked");

    /* Auto: prefers D3D11 on Windows when available, Vulkan when D3D11 absent, Software when neither */
    cfg = make_cfg(JPT_RENDERER_AUTO);
    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 1, 1, NULL);
    CHECK(renderer_backend_chosen() == JPT_RENDERER_D3D11 && !renderer_backend_state()->fallback,
          "Auto with D3D11 -> D3D11");

    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 0, 1, NULL);
    CHECK(renderer_backend_chosen() == JPT_RENDERER_VULKAN && renderer_backend_state()->fallback,
          "Auto without D3D11 -> Vulkan");
    CHECK(renderer_backend_state()->ops == &jpt_vulkan_backend_ops, "Auto fallback wires Vulkan vtable");

    renderer_backend_reset_for_test(1);
    renderer_backend_configure_with_probe(&cfg, 0, 0, "no GPU");
    CHECK(renderer_backend_chosen() == JPT_RENDERER_SOFTWARE && renderer_backend_state()->fallback,
          "Auto without D3D11/Vulkan -> Software");
}

int main(void) {
    test_factory_resolution();
    test_explicit_vulkan_no_silent_fallback();
    test_vulkan_real_dispatch();
    test_d3d11_software_and_auto();
    printf("vulkan_dispatch_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
