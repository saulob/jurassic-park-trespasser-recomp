/*
 * Host overhead of one renderer backend on the shared command stream of renderer_workload.h: the backend's own code
 * from the game's calls to the recorded commands, with no window, no swapchain and no presentation. Built twice:
 * renderer_benchmark_d3d11 and renderer_benchmark_vulkan. tools/renderer_benchmark.py compares their output.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderer_workload.h"

#ifdef BENCH_VULKAN
#include "ddraw/vk_win_test.h"
#include "vk_context.h"
#define BENCH_NAME "vulkan"
#else
#define BENCH_NAME "d3d11"
#endif

#define WARMUP_FRAMES 40
#define MEASURED_FRAMES 240

static dd_surface_t* g_polygon_surface;
dd_surface_t* dd_surface_from(void* iface) { return iface ? g_polygon_surface : NULL; }

static int compare_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static double percentile(double* sorted, int n, double p) {
    int i = (int)(p * (double)(n - 1) + 0.5);
    return sorted[i < 0 ? 0 : i >= n ? n - 1 : i];
}

int main(int argc, char** argv) {
    static wl_rig_t rig;
    static double samples[WL_CAT_COUNT + 2][MEASURED_FRAMES];
    const jpt_win_backend_ops_t* ops;
    double before[WL_CAT_COUNT], startup_ms, scene_total, flip_median = 0.0, flip_p95 = 0.0, flip_p99 = 0.0;
    int n = 0;
    const char* out_path = argc > 1 ? argv[1] : NULL;
    int stats_on = 0;
    FILE* out = stdout;
    double t0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--stats")) stats_on = 1;
        if (!strcmp(argv[i], "--mutate")) g_wl_mutation = 0x9e3779b9u;
    }
#ifdef BENCH_VULKAN
    jpt_vk_probe_result_t probe;
    if (!jpt_vk_probe_headless("", &probe) || !probe.available) {
        printf("renderer_benchmark_vulkan: 0 checks, 0 failures (no Vulkan device: %s)\n", probe.summary);
        return 0;
    }
    vk_win_test_enable_headless(WL_W, WL_H);
    vk_win_test_set_stats(stats_on);
    ops = &jpt_vulkan_backend_ops;
#else
    jpt_win_renderer_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.requested = JPT_RENDERER_D3D11;
    cfg.filter = D3DR_FILTER_BILINEAR;
    cfg.msaa = 1;
    ops = &jpt_d3d11_backend_ops;
    ops->configure(&cfg);
    if (!ops->startup()) {
        printf("renderer_benchmark_d3d11: 0 checks, 0 failures (no Direct3D 11 device)\n");
        return 0;
    }
#endif
    t0 = wl_now_ms();
    if (!wl_open(&rig, ops)) {
        printf("renderer_benchmark_%s: FAIL the backend did not attach\n", BENCH_NAME);
        return 1;
    }
    g_polygon_surface = &rig.target;
    for (int f = 0; f < WARMUP_FRAMES; f++) {
        wl_frame(&rig, (unsigned)f);
#ifdef BENCH_VULKAN
        ops->flip(&rig.target, 0);
#endif
    }
    startup_ms = wl_now_ms() - t0;
    memset(rig.ms, 0, sizeof rig.ms);
    rig.draws = rig.state_changes = rig.texture_updates = rig.polygons = 0;
    rig.stream_hash = 14695981039346656037ull;
    for (int f = 0; f < MEASURED_FRAMES; f++) {
        double frame_start;
        memcpy(before, rig.ms, sizeof before);
        frame_start = wl_now_ms();
        wl_frame(&rig, (unsigned)(WARMUP_FRAMES + f));
        scene_total = wl_now_ms() - frame_start;
        for (int c = 0; c < WL_CAT_COUNT; c++) samples[c][n] = rig.ms[c] - before[c];
        samples[WL_CAT_COUNT][n] = scene_total;
#ifdef BENCH_VULKAN
        {
            double flip_start = wl_now_ms();
            ops->flip(&rig.target, 0);
            samples[WL_CAT_COUNT + 1][n] = wl_now_ms() - flip_start;
        }
#endif
        n++;
    }
#ifdef BENCH_VULKAN
    {
        int c = WL_CAT_COUNT + 1;
        qsort(samples[c], (size_t)n, sizeof samples[c][0], compare_double);
        flip_median = percentile(samples[c], n, 0.5);
        flip_p95 = percentile(samples[c], n, 0.95);
        flip_p99 = percentile(samples[c], n, 0.99);
    }
#endif
    if (out_path) out = fopen(out_path, "w");
    if (!out) out = stdout;
    fprintf(out, "backend %s\n", BENCH_NAME);
    fprintf(out, "stream %016llx draws %llu state_changes %llu texture_updates %llu polygons %llu frames %d warmup %d startup_ms %.1f\n",
            rig.stream_hash, rig.draws, rig.state_changes, rig.texture_updates, rig.polygons, n, WARMUP_FRAMES, startup_ms);
    for (int c = 0; c <= WL_CAT_COUNT; c++) {
        qsort(samples[c], (size_t)n, sizeof samples[c][0], compare_double);
        fprintf(out, "metric %s median %.4f p95 %.4f p99 %.4f\n", c == WL_CAT_COUNT ? "total backend time per frame" : k_wl_category_names[c],
                percentile(samples[c], n, 0.5), percentile(samples[c], n, 0.95), percentile(samples[c], n, 0.99));
    }
#ifdef BENCH_VULKAN
    fprintf(out, "extra submit and flip (Vulkan only, includes waiting for the GPU) median %.4f p95 %.4f p99 %.4f\n", flip_median, flip_p95, flip_p99);
    {
        jpt_vk_renderer_stats_t st;
        vk_win_test_stats(&st);
        fprintf(out, "vulkan draws %llu recorded %llu merged %llu descriptor_updates %llu descriptor_cache_hits %llu pipeline_hits %llu\n", (unsigned long long)st.draws,
                (unsigned long long)st.draw_calls_recorded, (unsigned long long)st.draw_calls_merged,
                (unsigned long long)st.descriptor_updates, (unsigned long long)st.descriptor_cache_hits,
                (unsigned long long)st.pipeline_cache_hits);
        if (stats_on) fprintf(out, "vulkan cpu_ms draw %.1f descriptor %.1f pipeline %.1f upload %.1f submit %.1f\n", st.cpu_draw_ms, st.cpu_descriptor_ms, st.cpu_pipeline_ms, st.cpu_upload_ms, st.cpu_submit_ms);
    }
#endif
    if (out != stdout) fclose(out);
    wl_close(&rig);
#ifdef BENCH_VULKAN
    vk_win_test_shutdown();
#endif
    printf("renderer_benchmark_%s: %d checks, 0 failures\n", BENCH_NAME, n);
    return 0;
}
