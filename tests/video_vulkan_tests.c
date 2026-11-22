/*
 * The Vulkan video passes on the synthetic frames of video_fixtures.h, compared with the Direct3D 11 passes' output
 * that video_tests writes (build/video_ref/<name>.rgba). Offscreen; no window.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "video_fixtures.h"
#include "video_layout.h"
#include "vk_context.h"
#include "vk_renderer.h"

#ifndef JPT_SOURCE_DIR
#define JPT_SOURCE_DIR "."
#endif

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s - ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void log_fn(const char* msg, void* user) {
    (void)user;
    fprintf(stderr, "[vk] %s\n", msg ? msg : "");
}

typedef struct {
    int max_diff;
    double mean_diff;
} diff_t;

static diff_t compare(const uint8_t* a, const uint8_t* b, int pixels) {
    diff_t d = { 0, 0.0 };
    double sum = 0.0;
    for (int i = 0; i < pixels; i++)
        for (int c = 0; c < 3; c++) {
            int v = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            if (v > d.max_diff) d.max_diff = v;
            sum += v;
        }
    d.mean_diff = sum / (3.0 * pixels);
    return d;
}

typedef struct {
    int smooth, deblock, glow, rebuild, stray;
} stages_t;

static int render(const vf_fixture_t* fx, const uint16_t* codes, const vl_palette_map_t* map, const stages_t* st,
                  uint8_t* rgba, size_t rgba_bytes, long long* validation_errors) {
    jpt_vk_renderer_desc_t desc;
    char err[256] = { 0 };
    jpt_vk_renderer_t* r;
    jpt_vk_video_frame_t f;
    vl_policy_t policy = vl_policy_for(fx->rebuild ? VL_SMK_Y_DOUBLED : 0, (uint32_t)fx->shown_rows);
    vl_layout_t layout = vl_layout(fx->out_w, fx->out_h, fx->width, fx->shown_rows);
    uint32_t w = 0, h = 0;
    int ok = 0;
    memset(&desc, 0, sizeof desc);
    desc.log_fn = log_fn;
    desc.enable_validation = 1;
    desc.cfg.guest_w = (uint32_t)fx->out_w;
    desc.cfg.guest_h = (uint32_t)fx->out_h;
    desc.cfg.internal_w = (uint32_t)fx->out_w;
    desc.cfg.internal_h = (uint32_t)fx->out_h;
    desc.cfg.window_w = (uint32_t)fx->out_w;
    desc.cfg.window_h = (uint32_t)fx->out_h;
    desc.cfg.msaa = 1;
    desc.cfg.texture_filter = JPT_FILTER_BILINEAR;
    desc.cfg.vsync = 1;
    desc.cfg.ao_level = JPT_AO_OFF;
    r = jpt_vk_renderer_create(&desc, err, sizeof err);
    CHECK(r != NULL, "renderer for %s: %s", fx->name, err);
    if (!r) return 0;
    memset(&f, 0, sizeof f);
    f.rows = (const uint8_t*)codes;
    f.stride = (long)fx->width * 2;
    f.palette_map = fx->palette ? map->map : NULL;
    f.palette_changed = 1;
    f.smooth = st->smooth;
    f.deblock = st->deblock;
    f.params.width = fx->width;
    f.params.stored_rows = policy.stored_rows;
    f.params.shown_rows = policy.shown_rows;
    f.params.rebuild = st->rebuild;
    f.params.parity = policy.parity;
    f.params.has_palette_map = f.palette_map != NULL;
    f.params.rgb565 = 1;
    f.params.video_x = layout.video.x;
    f.params.video_y = layout.video.y;
    f.params.video_w = layout.video.w;
    f.params.video_h = layout.video.h;
    f.params.out_w = fx->out_w;
    f.params.out_h = fx->out_h;
    f.params.side = layout.side;
    f.params.glow = st->glow;
    f.params.progressive = policy.rows == VL_ROWS_PROGRESSIVE;
    f.params.stray_line = st->stray;
    if (jpt_vk_renderer_begin_frame(r)) {
        jpt_vk_renderer_clear(r, 1, 0xFF000000u, 0, 0.0f, 0, 0);
        if (jpt_vk_renderer_video_draw(r, &f) && jpt_vk_renderer_end_and_present(r) &&
            jpt_vk_renderer_readback_rgba(r, rgba, &w, &h, rgba_bytes))
            ok = (int)w == fx->out_w && (int)h == fx->out_h;
    }
    if (validation_errors) *validation_errors += jpt_vk_renderer_validation_errors(r);
    jpt_vk_renderer_destroy(r);
    return ok;
}

static int read_reference(const char* name, uint8_t* out, size_t bytes) {
    char path[600];
    FILE* file;
    size_t got;
    snprintf(path, sizeof path, "%s/build/video_ref/%s.rgba", JPT_SOURCE_DIR, name);
    file = fopen(path, "rb");
    if (!file) return 0;
    got = fread(out, 1, bytes, file);
    fclose(file);
    return got == bytes;
}

int main(void) {
    static uint16_t codes[256 * 64];
    static vl_palette_map_t map;
    uint8_t pal[768];
    jpt_vk_probe_result_t probe;
    long long validation_errors = 0;
    if (!jpt_vk_probe_headless("", &probe) || !probe.available) {
        printf("video_vulkan_tests: 0 checks, 0 failures (GPU fixtures skipped: %s)\n", probe.summary);
        return 0;
    }
    vf_palette(pal);
    vl_palette_map_update(&map, pal);
    for (int i = 0; i < VF_COUNT; i++) {
        const vf_fixture_t* fx = &k_vf_fixtures[i];
        vl_policy_t policy = vl_policy_for(fx->rebuild ? VL_SMK_Y_DOUBLED : 0, (uint32_t)fx->shown_rows);
        vl_layout_t layout = vl_layout(fx->out_w, fx->out_h, fx->width, fx->shown_rows);
        vl_stages_t stages = vl_video_stages(&policy, &layout, fx->stray);
        size_t bytes = (size_t)fx->out_w * fx->out_h * 4u;
        uint8_t* got = (uint8_t*)malloc(bytes);
        uint8_t* ref = (uint8_t*)malloc(bytes);
        uint8_t* other = (uint8_t*)malloc(bytes);
        stages_t st = { fx->smooth && stages.smooth, fx->deblock && stages.deblock, stages.glow, fx->rebuild, stages.stray };
        diff_t d, match;
        if (!got || !ref || !other) return 1;
        vf_codes(fx, codes);
        CHECK(render(fx, codes, &map, &st, got, bytes, &validation_errors), "%s: the Vulkan passes drew", fx->name);
        if (!read_reference(fx->name, ref, bytes)) {
            CHECK(0, "%s: no Direct3D 11 reference (run video_tests first)", fx->name);
            free(got);
            free(ref);
            free(other);
            continue;
        }
        d = compare(got, ref, fx->out_w * fx->out_h);
        printf("  %-16s Vulkan against Direct3D 11: largest %d levels, mean %.3f\n", fx->name, d.max_diff, d.mean_diff);
        CHECK(d.max_diff <= 2 && d.mean_diff <= 0.2, "%s: Vulkan matches Direct3D 11 (largest %d levels, mean %.3f)", fx->name,
              d.max_diff, d.mean_diff);
        match = d;

        /* A pipeline without the stage under test must not match: the fixture exercises it. */
        {
            stages_t off = st;
            int has_stage = 1;
            if (!strcmp(fx->name, "smooth")) off.smooth = 0;
            else if (!strcmp(fx->name, "deblock")) off.deblock = 0;
            else if (!strcmp(fx->name, "newgame_rebuild")) off.rebuild = 0;
            else if (!strcmp(fx->name, "stray")) off.stray = 0;
            else if (!strcmp(fx->name, "glow")) off.glow = 0;
            else has_stage = 0;
            if (has_stage) {
                int pixels = fx->out_w * fx->out_h;
                vf_fixture_t shown = *fx;
                if (off.rebuild != st.rebuild) {
                    shown.stored_rows = fx->shown_rows;
                    shown.rebuild = 0;
                    vf_codes(&shown, codes);
                    off.rebuild = 0;
                    CHECK(render(&shown, codes, &map, &off, other, bytes, &validation_errors), "%s: stage-less pass drew", fx->name);
                } else {
                    CHECK(render(fx, codes, &map, &off, other, bytes, &validation_errors), "%s: stage-less pass drew", fx->name);
                }
                d = compare(other, ref, pixels);
                printf("  %-16s without its stage: largest %d levels, mean %.3f\n", fx->name, d.max_diff, d.mean_diff);
                CHECK(d.max_diff > 2 * match.max_diff + 1 && d.mean_diff > 4.0 * match.mean_diff,
                      "%s: without its stage the picture differs from the reference (largest %d)", fx->name, d.max_diff);
            }
        }
        free(got);
        free(ref);
        free(other);
    }
    CHECK(validation_errors == 0, "no Vulkan validation errors (%lld)", validation_errors);
    if (g_failures) {
        fprintf(stderr, "video_vulkan_tests: %d failure(s) in %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("video_vulkan_tests: %d checks, 0 failures\n", g_checks);
    return 0;
}
