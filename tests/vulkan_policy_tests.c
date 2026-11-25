#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_present_policy.h"
#include "jpt_prompt_text.h"
#include "jpt_renderer.h"
#include "vk_context.h"
#include "vk_policy.h"
#include "vk_shaders.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond);                     \
            fprintf(stderr, __VA_ARGS__);                                                        \
            fputc('\n', stderr);                                                                 \
            g_failures++;                                                                        \
        }                                                                                        \
    } while (0)

static void test_renderer_policy(void) {
    jpt_renderer_decision_t d;
    jpt_renderer_t parsed = JPT_RENDERER_SOFTWARE;

    CHECK(jpt_renderer_parse("auto", &parsed) && parsed == JPT_RENDERER_AUTO, "parse auto");
    CHECK(jpt_renderer_parse("d3d11", &parsed) && parsed == JPT_RENDERER_D3D11, "parse d3d11");
    CHECK(jpt_renderer_parse("vulkan", &parsed) && parsed == JPT_RENDERER_VULKAN, "parse vulkan");
    CHECK(jpt_renderer_parse("software", &parsed) && parsed == JPT_RENDERER_SOFTWARE, "parse software");
    CHECK(!jpt_renderer_parse("opengl", &parsed), "reject unknown renderer");

    CHECK(jpt_renderer_available_on_platform(JPT_RENDERER_D3D11, 1) == 1, "D3D11 available on Windows");
    CHECK(jpt_renderer_available_on_platform(JPT_RENDERER_D3D11, 0) == 0, "D3D11 unavailable on Linux");
    CHECK(jpt_renderer_available_on_platform(JPT_RENDERER_VULKAN, 1) == 1, "Vulkan available on Windows");
    CHECK(jpt_renderer_available_on_platform(JPT_RENDERER_VULKAN, 0) == 1, "Vulkan available on Linux");

    /* Windows Auto prefers D3D11 compatibly; falls back cleanly when D3D11 absent */
    jpt_renderer_resolve_policy(JPT_RENDERER_AUTO, 1, 1, 1, NULL, &d);
    CHECK(d.chosen == JPT_RENDERER_D3D11 && !d.fallback, "Windows Auto -> D3D11");
    jpt_renderer_resolve_policy(JPT_RENDERER_AUTO, 1, 0, 1, NULL, &d);
    CHECK(d.chosen == JPT_RENDERER_VULKAN && d.fallback, "Windows Auto without D3D11 -> Vulkan");

    /* Linux Auto prefers Vulkan; falls back to Software with explicit reason when unavailable */
    jpt_renderer_resolve_policy(JPT_RENDERER_AUTO, 0, 0, 1, NULL, &d);
    CHECK(d.chosen == JPT_RENDERER_VULKAN && !d.fallback, "Linux Auto -> Vulkan");
    jpt_renderer_resolve_policy(JPT_RENDERER_AUTO, 0, 0, 0, "no ICD", &d);
    CHECK(d.chosen == JPT_RENDERER_SOFTWARE && d.fallback && strstr(d.reason, "no ICD") != NULL,
          "Linux Auto fallback logs reason: %s", d.reason);

    /* Linux D3D11 does not crash; reports unavailable and falls back cleanly */
    jpt_renderer_resolve_policy(JPT_RENDERER_D3D11, 0, 0, 1, NULL, &d);
    CHECK(d.chosen == JPT_RENDERER_VULKAN && d.fallback && strstr(d.reason, "Linux") != NULL,
          "Linux D3D11 -> Vulkan fallback");
    jpt_renderer_resolve_policy(JPT_RENDERER_VULKAN, 0, 0, 0, "loader missing", &d);
    CHECK(d.chosen == JPT_RENDERER_SOFTWARE && d.fallback && strstr(d.reason, "loader missing") != NULL,
          "Explicit Vulkan unavailable -> Software with reason");

    /* Config parsing integration */
    jpt_config_t cfg;
    jpt_config_defaults(&cfg);
    CHECK(cfg.renderer == JPT_RENDERER_AUTO && cfg.hardware == 1, "default config is auto hardware");
    CHECK(jpt_config_apply(&cfg, JPT_KEY_RENDERER, "vulkan") && cfg.renderer == JPT_RENDERER_VULKAN && cfg.hardware == 1,
          "config vulkan");
    CHECK(jpt_config_apply(&cfg, JPT_KEY_RENDERER, "software") && cfg.renderer == JPT_RENDERER_SOFTWARE &&
              cfg.hardware == 0,
          "config software");
}

static void test_renderer_selection_precedence(void) {
    jpt_renderer_selection_t s;
    jpt_renderer_decision_t d;

    /* INI d3d11 without override -> D3D11 */
    jpt_renderer_select_request(1, JPT_RENDERER_D3D11, NULL, NULL, &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_D3D11 && s.source == JPT_RENDERER_SOURCE_INI && !s.env_present && !s.cli_present,
          "INI d3d11 without override");
    CHECK(d.chosen == JPT_RENDERER_D3D11 && !d.fallback, "INI d3d11 resolves to D3D11");

    /* INI vulkan without override -> Vulkan */
    jpt_renderer_select_request(1, JPT_RENDERER_VULKAN, NULL, NULL, &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_VULKAN && s.source == JPT_RENDERER_SOURCE_INI && !s.env_present,
          "INI vulkan without override");
    CHECK(d.chosen == JPT_RENDERER_VULKAN && !d.fallback, "INI vulkan resolves to Vulkan");

    /* INI auto on Windows -> D3D11 */
    jpt_renderer_select_request(1, JPT_RENDERER_AUTO, "", NULL, &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_AUTO && s.source == JPT_RENDERER_SOURCE_INI && !s.env_present,
          "INI auto without env");
    CHECK(d.chosen == JPT_RENDERER_D3D11 && !d.fallback, "INI auto on Windows resolves to D3D11");

    /* Explicit CLI overrides INI and env */
    jpt_renderer_select_request(1, JPT_RENDERER_D3D11, NULL, "vulkan", &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_VULKAN && s.source == JPT_RENDERER_SOURCE_CLI &&
              strcmp(jpt_renderer_source_name(s.source), "cli") == 0,
          "CLI vulkan overrides INI d3d11");
    CHECK(d.chosen == JPT_RENDERER_VULKAN && !d.fallback, "CLI vulkan resolves to Vulkan");

    jpt_renderer_select_request(1, JPT_RENDERER_VULKAN, "vulkan", "d3d11", &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_D3D11 && s.source == JPT_RENDERER_SOURCE_CLI && s.env_present && s.env_ignored,
          "CLI d3d11 overrides INI and env");
    CHECK(d.chosen == JPT_RENDERER_D3D11 && !d.fallback, "CLI d3d11 resolves to D3D11");

    /* Forgotten env var must NOT silently override explicit INI selection */
    jpt_renderer_select_request(1, JPT_RENDERER_D3D11, "vulkan", NULL, &s);
    jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
    CHECK(s.requested == JPT_RENDERER_D3D11 && s.source == JPT_RENDERER_SOURCE_INI && s.env_present && s.env_valid &&
              s.env_ignored,
          "INI d3d11 wins over forgotten TRESPASSER_RENDERER=vulkan");
    CHECK(d.chosen == JPT_RENDERER_D3D11 && !d.fallback, "forgotten env does not switch D3D11 to Vulkan");

    /* Env var applies when INI is auto or unset, and reports source=env (never cli) */
    jpt_renderer_select_request(1, JPT_RENDERER_AUTO, "vulkan", NULL, &s);
    CHECK(s.requested == JPT_RENDERER_VULKAN && s.source == JPT_RENDERER_SOURCE_ENV && !s.env_ignored &&
              strcmp(jpt_renderer_source_name(s.source), "env") == 0,
          "env vulkan applies over INI auto with source=env");

    jpt_renderer_select_request(0, JPT_RENDERER_AUTO, "vulkan", NULL, &s);
    CHECK(s.requested == JPT_RENDERER_VULKAN && s.source == JPT_RENDERER_SOURCE_ENV && !s.ini_present &&
              strcmp(jpt_renderer_source_name(s.source), "env") == 0,
          "env vulkan applies when INI unset with source=env");

    /* Invalid env var is ignored cleanly */
    jpt_renderer_select_request(0, JPT_RENDERER_AUTO, "invalid_backend", NULL, &s);
    CHECK(s.requested == JPT_RENDERER_AUTO && s.source == JPT_RENDERER_SOURCE_DEFAULT && s.env_present && !s.env_valid,
          "invalid env ignored and falls back to default");

    /* Launcher INI persistence round-trip: save choice, reload config, resolve runtime renderer */
    {
        const char* tmp_ini = "tmp_renderer_selection_test.ini";
        const char* base_keys[] = { JPT_KEY_MSAA, JPT_KEY_FOV, JPT_KEY_RENDERER };
        const char* base_vals[] = { "4", "85", "d3d11" };
        const char* modes[] = { "d3d11", "vulkan", "software", "auto" };
        const jpt_renderer_t expected_req[] = { JPT_RENDERER_D3D11, JPT_RENDERER_VULKAN, JPT_RENDERER_SOFTWARE,
                                                JPT_RENDERER_AUTO };
        const jpt_renderer_t expected_win[] = { JPT_RENDERER_D3D11, JPT_RENDERER_VULKAN, JPT_RENDERER_SOFTWARE,
                                                JPT_RENDERER_D3D11 };
        jpt_config_t cfg;

        remove(tmp_ini);
        CHECK(jpt_ini_set(tmp_ini, base_keys, base_vals, 3), "write initial test INI");
        for (int i = 0; i < 4; i++) {
            const char* k = JPT_KEY_RENDERER;
            const char* v = modes[i];
            CHECK(jpt_ini_set(tmp_ini, &k, &v, 1), "persist renderer=%s", v);
            CHECK(jpt_config_load(tmp_ini, &cfg), "reload INI for renderer=%s", v);
            CHECK(cfg.renderer_from_ini == 1 && cfg.renderer == (int)expected_req[i],
                  "loaded INI renderer=%s (got %d, from_ini=%d)", v, cfg.renderer, cfg.renderer_from_ini);
            CHECK(cfg.msaa == 4 && cfg.fov == 85, "other INI preferences preserved after saving renderer=%s", v);

            jpt_renderer_select_request(cfg.renderer_from_ini, (jpt_renderer_t)cfg.renderer, NULL, NULL, &s);
            jpt_renderer_resolve_policy(s.requested, 1, 1, 1, NULL, &d);
            CHECK(s.requested == expected_req[i] && s.source == JPT_RENDERER_SOURCE_INI,
                  "runtime request matches launcher choice %s", v);
            CHECK(d.chosen == expected_win[i] && !d.fallback, "Windows policy resolves %s -> %s", v,
                  jpt_renderer_key(expected_win[i]));
        }
        remove(tmp_ini);
    }
}

static jpt_vk_gpu_info_t make_fake_gpu(const char* name, uint32_t vendor, uint32_t dev, VkPhysicalDeviceType type,
                                       uint64_t vram, int usable_swapchain, uint32_t depth_mask, uint8_t uuid_seed) {
    jpt_vk_gpu_info_t g;
    memset(&g, 0, sizeof g);
    snprintf(g.name, sizeof g.name, "%s", name);
    g.vendor_id = vendor;
    g.device_id = dev;
    g.device_type = type;
    g.api_version = VK_API_VERSION_1_2;
    g.vram_bytes = vram;
    g.graphics_queue_family = 0;
    g.present_queue_family = usable_swapchain ? 0 : -1;
    g.has_swapchain_ext = usable_swapchain;
    g.depth_format_mask = depth_mask;
    g.has_anisotropy = 1;
    g.max_anisotropy = 16.0f;
    g.msaa_color_counts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
    g.msaa_depth_counts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
    for (int i = 0; i < 16; i++) g.uuid[i] = (uint8_t)(uuid_seed + i);
    jpt_vk_gpu_check_requirements(&g, 1);
    return g;
}

static void test_gpu_selection(void) {
    jpt_vk_gpu_info_t gpus[4];
    char reason[192];

    gpus[0] = make_fake_gpu("Intel Iris Xe", 0x8086u, 0x9A49u, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                            2ULL << 30, 1, JPT_VK_DEPTH_BIT_D24S8, 0x10);
    gpus[1] = make_fake_gpu("Broken Discrete GPU", 0x10DEu, 0x2684u, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                            16ULL << 30, 0, JPT_VK_DEPTH_BIT_D24S8, 0x20);
    gpus[2] = make_fake_gpu("AMD Radeon RX 7800 XT", 0x1002u, 0x747Eu, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                            16ULL << 30, 1, JPT_VK_DEPTH_BIT_D32S8, 0x30);
    gpus[3] = make_fake_gpu("llvmpipe", 0x10005u, 0x0000u, VK_PHYSICAL_DEVICE_TYPE_CPU,
                            4ULL << 30, 1, JPT_VK_DEPTH_BIT_D24S8, 0x40);
    jpt_vk_gpu_assign_labels(gpus, 4);

    CHECK(!gpus[1].usable && gpus[1].reject_reason[0] != 0, "unsupported discrete rejected");
    CHECK(gpus[0].usable && gpus[2].usable && gpus[3].usable, "other GPUs usable");

    /* Auto skips unsupported discrete and picks usable discrete over integrated and CPU */
    int idx = jpt_vk_gpu_select(gpus, 4, "", 0, reason, sizeof reason);
    CHECK(idx == 2, "Auto picks usable discrete GPU (got %d: %s)", idx, reason);

    /* Env override respects loader-ordered first usable GPU */
    idx = jpt_vk_gpu_select(gpus, 4, "auto", 1, reason, sizeof reason);
    CHECK(idx == 0, "Env override respects loader order (got %d: %s)", idx, reason);

    /* Explicit selection by stable ID, vendor:device, and label */
    idx = jpt_vk_gpu_select(gpus, 4, gpus[0].stable_id, 0, reason, sizeof reason);
    CHECK(idx == 0, "Stable ID matches integrated GPU (id=%s, got %d)", gpus[0].stable_id, idx);
    idx = jpt_vk_gpu_select(gpus, 4, "8086:9a49", 0, reason, sizeof reason);
    CHECK(idx == 0, "vendor:device matches integrated GPU");
    idx = jpt_vk_gpu_select(gpus, 4, "llvmpipe", 0, reason, sizeof reason);
    CHECK(idx == 3, "Explicit CPU selection works when requested");

    /* Requesting unsupported discrete falls back cleanly to best usable */
    idx = jpt_vk_gpu_select(gpus, 4, "Broken Discrete GPU", 0, reason, sizeof reason);
    CHECK(idx == 2 && strstr(reason, "unusable") != NULL, "Unusable explicit request falls back with reason: %s",
          reason);

    /* Without discrete, integrated beats CPU */
    gpus[2].usable = 0;
    idx = jpt_vk_gpu_select(gpus, 4, "", 0, reason, sizeof reason);
    CHECK(idx == 0, "Integrated beats CPU when no discrete available (got %d)", idx);

    /* Steam Deck AMD RADV Van Gogh APU (integrated, D32S8) beats llvmpipe */
    jpt_vk_gpu_info_t deck[2];
    deck[0] = make_fake_gpu("llvmpipe", 0x10005u, 0x0000u, VK_PHYSICAL_DEVICE_TYPE_CPU, 4ULL << 30, 1,
                            JPT_VK_DEPTH_BIT_D24S8, 0x50);
    deck[1] = make_fake_gpu("AMD Custom GPU 0405 (RADV VANGOGH)", 0x1002u, 0x163Fu,
                            VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 4ULL << 30, 1,
                            JPT_VK_DEPTH_BIT_D32S8 | JPT_VK_DEPTH_BIT_D32, 0x60);
    jpt_vk_gpu_assign_labels(deck, 2);
    CHECK(deck[1].usable && deck[1].preferred_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT,
          "Steam Deck RADV APU meets all Vulkan requirements");
    idx = jpt_vk_gpu_select(deck, 2, "", 0, reason, sizeof reason);
    CHECK(idx == 1, "Steam Deck RADV APU auto-selected over CPU (got %d: %s)", idx, reason);

    /* Duplicate names get disambiguated labels */
    jpt_vk_gpu_info_t dup[2] = { gpus[0], gpus[0] };
    jpt_vk_gpu_assign_labels(dup, 2);
    CHECK(strcmp(dup[0].label, "Intel Iris Xe") == 0 && strcmp(dup[1].label, "Intel Iris Xe (2)") == 0,
          "Duplicate GPU names disambiguated: '%s' / '%s'", dup[0].label, dup[1].label);
}

static void test_device_capabilities(void) {
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int has_stencil = -1;

    CHECK(jpt_vk_pick_depth_format(JPT_VK_DEPTH_BIT_D24S8 | JPT_VK_DEPTH_BIT_D32, &fmt, &has_stencil) &&
              fmt == VK_FORMAT_D24_UNORM_S8_UINT && has_stencil == 1,
          "prefer D24S8");
    CHECK(jpt_vk_pick_depth_format(JPT_VK_DEPTH_BIT_D32S8 | JPT_VK_DEPTH_BIT_D32, &fmt, &has_stencil) &&
              fmt == VK_FORMAT_D32_SFLOAT_S8_UINT && has_stencil == 1,
          "fallback D32S8");
    CHECK(jpt_vk_pick_depth_format(JPT_VK_DEPTH_BIT_D32 | JPT_VK_DEPTH_BIT_D16, &fmt, &has_stencil) &&
              fmt == VK_FORMAT_D32_SFLOAT && has_stencil == 0,
          "fallback D32");
    CHECK(jpt_vk_pick_depth_format(JPT_VK_DEPTH_BIT_D16, &fmt, &has_stencil) && fmt == VK_FORMAT_D16_UNORM &&
              has_stencil == 0,
          "fallback D16");
    CHECK(!jpt_vk_pick_depth_format(0, &fmt, &has_stencil), "reject no depth format");

    CHECK(jpt_vk_clamp_anisotropy(JPT_FILTER_ANISO16, 0, 16.0f) == 1.0f, "no aniso feature -> 1x");
    CHECK(jpt_vk_clamp_anisotropy(JPT_FILTER_ANISO16, 1, 8.0f) == 8.0f, "clamp 16x -> 8x");
    CHECK(jpt_vk_clamp_anisotropy(JPT_FILTER_ANISO4, 1, 16.0f) == 4.0f, "4x stays 4x");
    CHECK(jpt_vk_clamp_anisotropy(JPT_FILTER_BILINEAR, 1, 16.0f) == 1.0f, "bilinear -> 1x");

    CHECK(jpt_vk_clamp_msaa(8, VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT,
                            VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT) == 4,
          "MSAA 8x clamps to 4x");
    CHECK(jpt_vk_clamp_msaa(4, VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT,
                            VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT) == 2,
          "MSAA intersects color & depth counts");
    CHECK(jpt_vk_clamp_msaa(4, VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_1_BIT) == 1, "MSAA fallback 1x");

    CHECK(jpt_vk_resolve_fill_mode(2u, 0) == VK_POLYGON_MODE_FILL, "wireframe falls back to fill when unsupported");
    CHECK(jpt_vk_resolve_fill_mode(2u, 1) == VK_POLYGON_MODE_LINE, "wireframe enabled when supported");
}

static void test_present_mode_and_swapchain_stress(void) {
    VkPresentModeKHR all_modes[4] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                      VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR };
    VkPresentModeKHR no_mailbox[2] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
    VkPresentModeKHR fifo_only[1] = { VK_PRESENT_MODE_FIFO_KHR };

    CHECK(jpt_vk_choose_present_mode(1, all_modes, 4) == VK_PRESENT_MODE_FIFO_KHR, "VSync On always FIFO");
    CHECK(jpt_vk_choose_present_mode(0, all_modes, 4) == VK_PRESENT_MODE_MAILBOX_KHR, "VSync Off prefers MAILBOX");
    CHECK(jpt_vk_choose_present_mode(0, no_mailbox, 2) == VK_PRESENT_MODE_IMMEDIATE_KHR,
          "VSync Off falls back to IMMEDIATE");
    CHECK(jpt_vk_choose_present_mode(0, fifo_only, 1) == VK_PRESENT_MODE_FIFO_KHR, "VSync Off falls back to FIFO");

    CHECK(jpt_vk_choose_image_count(2, 0, VK_PRESENT_MODE_MAILBOX_KHR) == 3, "MAILBOX triple buffering");
    CHECK(jpt_vk_choose_image_count(3, 8, VK_PRESENT_MODE_FIFO_KHR) == 4, "Desktop min=3 -> 4");
    CHECK(jpt_vk_choose_image_count_for(3, 8, VK_PRESENT_MODE_FIFO_KHR, 1) == 3,
          "Gamescope keeps surface minimum 3 to avoid extra frame latency");
    CHECK(jpt_vk_choose_image_count_for(3, 8, VK_PRESENT_MODE_MAILBOX_KHR, 1) == 3, "Gamescope MAILBOX min=3 -> 3");
    CHECK(jpt_vk_choose_image_count_for(1, 0, VK_PRESENT_MODE_FIFO_KHR, 1) == 2, "Gamescope never uses a single image");
    CHECK(jpt_vk_choose_image_count(2, 2, VK_PRESENT_MODE_MAILBOX_KHR) == 2, "Clamped to maxImageCount=2");

    /* Swapchain state machine stress */
    jpt_vk_swapchain_health_t h;
    jpt_vk_sc_init(&h, 1280, 720, 1, 0);
    jpt_vk_sc_on_recreated(&h, 1280, 720, 1, 0);

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t w = 800 + i * 32, ht = 600 + i * 16;
        CHECK(jpt_vk_sc_on_resize(&h, w, ht) == JPT_VK_SC_ACTION_RECREATE, "resize triggers recreate");
        jpt_vk_sc_on_recreated(&h, w, ht, h.vsync, h.fullscreen);
    }

    /* Minimize 0x0 must wait, never recreate */
    CHECK(jpt_vk_sc_on_resize(&h, 0, 0) == JPT_VK_SC_ACTION_WAIT_EXTENT, "0x0 waits");
    CHECK(jpt_vk_sc_on_result(&h, VK_ERROR_OUT_OF_DATE_KHR) == JPT_VK_SC_ACTION_WAIT_EXTENT, "0x0 ignores OUT_OF_DATE");
    CHECK(jpt_vk_sc_on_resize(&h, 1280, 800) == JPT_VK_SC_ACTION_RECREATE, "restore recreates");
    jpt_vk_sc_on_recreated(&h, 1280, 800, 1, 0);

    /* VSync & fullscreen changes trigger recreate */
    CHECK(jpt_vk_sc_on_settings(&h, 0, 0) == JPT_VK_SC_ACTION_RECREATE, "VSync toggle recreates");
    jpt_vk_sc_on_recreated(&h, 1280, 800, 0, 0);
    CHECK(jpt_vk_sc_on_settings(&h, 0, 1) == JPT_VK_SC_ACTION_RECREATE, "Fullscreen toggle recreates");
    jpt_vk_sc_on_recreated(&h, 1280, 800, 0, 1);

    /* Persistent SUBOPTIMAL recreates at most JPT_VK_SUBOPTIMAL_LIMIT times, then accepts */
    for (uint32_t i = 0; i < JPT_VK_SUBOPTIMAL_LIMIT; i++) {
        CHECK(jpt_vk_sc_on_result(&h, VK_SUBOPTIMAL_KHR) == JPT_VK_SC_ACTION_RECREATE,
              "initial suboptimal recreates");
        jpt_vk_sc_on_recreated(&h, 1280, 800, 0, 1);
    }
    CHECK(jpt_vk_sc_on_result(&h, VK_SUBOPTIMAL_KHR) == JPT_VK_SC_ACTION_OK,
          "persistent suboptimal stops recreating in a loop");

    /* OUT_OF_DATE & SURFACE_LOST */
    CHECK(jpt_vk_sc_on_result(&h, VK_ERROR_OUT_OF_DATE_KHR) == JPT_VK_SC_ACTION_RECREATE, "OUT_OF_DATE recreates");
    jpt_vk_sc_on_recreated(&h, 1280, 800, 0, 1);
    CHECK(jpt_vk_sc_on_result(&h, VK_ERROR_SURFACE_LOST_KHR) == JPT_VK_SC_ACTION_RECREATE_SURFACE,
          "SURFACE_LOST recreates surface");
    jpt_vk_sc_on_recreated(&h, 1280, 800, 0, 1);

    /* DEVICE_LOST latches permanently */
    CHECK(jpt_vk_sc_on_result(&h, VK_ERROR_DEVICE_LOST) == JPT_VK_SC_ACTION_DEVICE_LOST, "DEVICE_LOST detected");
    CHECK(jpt_vk_sc_on_resize(&h, 1920, 1080) == JPT_VK_SC_ACTION_DEVICE_LOST, "DEVICE_LOST stays latched");
}

static void test_pipeline_key_and_barriers(void) {
    jpt_render_state_t s1, s2;
    jpt_vk_pipeline_key_t k1, k2;

    memset(&s1, 0, sizeof s1);
    s1.z_enable = 1;
    s1.z_write = 1;
    s1.z_func = 7; /* GREATEREQUAL */
    s1.cull_mode = 3;
    s1.fill_mode = 3;
    s1.alpha_blend = 0;
    s1.src_blend = 5;
    s1.dst_blend = 6;
    s1.fog_color = 0x112233u;
    s1.alpha_ref = 128;
    s1.texture_id = 42;
    s2 = s1;

    /* Same state -> same key & hash */
    jpt_vk_pipeline_key_from_state(&s1, JPT_PRIM_TRIANGLEFAN, 4, JPT_VK_RP_SCENE_MSAA, 1, &k1);
    jpt_vk_pipeline_key_from_state(&s2, JPT_PRIM_TRIANGLELIST, 4, JPT_VK_RP_SCENE_MSAA, 1, &k2);
    CHECK(jpt_vk_pipeline_key_equal(&k1, &k2) && jpt_vk_pipeline_key_hash(&k1) == jpt_vk_pipeline_key_hash(&k2),
          "triangle fan and list share pipeline key");

    /* Irrelevant states must NOT create new pipelines */
    s2.fog_color = 0xAABBCCu;
    s2.alpha_ref = 12;
    s2.texture_id = 999;
    s2.src_blend = 2; /* ignored when alpha_blend == 0 */
    s2.dst_blend = 1;
    jpt_vk_pipeline_key_from_state(&s2, JPT_PRIM_TRIANGLELIST, 4, JPT_VK_RP_SCENE_MSAA, 1, &k2);
    CHECK(jpt_vk_pipeline_key_equal(&k1, &k2), "irrelevant states canonicalized away");

    /* Relevant state change -> different key */
    s2.alpha_blend = 1;
    jpt_vk_pipeline_key_from_state(&s2, JPT_PRIM_TRIANGLELIST, 4, JPT_VK_RP_SCENE_MSAA, 1, &k2);
    CHECK(!jpt_vk_pipeline_key_equal(&k1, &k2), "enabling alpha blend changes pipeline key");

    /* Barrier planner */
    jpt_vk_barrier_plan_t bp;
    CHECK(jpt_vk_plan_image_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_ASPECT_COLOR_BIT, &bp) &&
              bp.dst_stage == VK_PIPELINE_STAGE_TRANSFER_BIT && bp.dst_access == VK_ACCESS_TRANSFER_WRITE_BIT,
          "upload barrier plan");
    CHECK(jpt_vk_plan_image_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_ASPECT_COLOR_BIT, &bp) &&
              bp.dst_stage == VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT && bp.dst_access == VK_ACCESS_SHADER_READ_BIT,
          "sample barrier plan");

    /* Geometry: 16:9 internal inside 1280x800 Steam Deck -> letterbox bars top/bottom */
    jpt_vk_frame_geometry_t g;
    jpt_vk_compute_geometry(640, 480, 1920, 1080, 1280, 800, 0, &g);
    CHECK(g.present_rect.x == 0 && g.present_rect.w == 1280 && g.present_rect.h == 720 && g.present_rect.y == 40,
          "16:9 in 1280x800 letterboxed (%d,%d %ux%u)", g.present_rect.x, g.present_rect.y, g.present_rect.w,
          g.present_rect.h);
    jpt_vk_compute_geometry(640, 480, 1920, 1080, 1280, 800, 1, &g);
    CHECK(g.present_rect.x == 0 && g.present_rect.y == 0 && g.present_rect.w == 1280 && g.present_rect.h == 800,
          "stretch fills window");
    /* 4:3 internal inside 1280x800 Steam Deck -> centred pillarbox, not stretched */
    jpt_vk_compute_geometry(640, 480, 1066, 800, 1280, 800, 0, &g);
    CHECK(g.present_rect.x == 107 && g.present_rect.y == 0 && g.present_rect.w == 1066 && g.present_rect.h == 800,
          "4:3 in 1280x800 pillarboxed (%d,%d %ux%u)", g.present_rect.x, g.present_rect.y, g.present_rect.w,
          g.present_rect.h);
    /* 16:10 internal inside 1280x800 Steam Deck -> exact full-screen fill */
    jpt_vk_compute_geometry(960, 600, 1280, 800, 1280, 800, 0, &g);
    CHECK(g.present_rect.x == 0 && g.present_rect.y == 0 && g.present_rect.w == 1280 && g.present_rect.h == 800,
          "16:10 in 1280x800 fills screen (%d,%d %ux%u)", g.present_rect.x, g.present_rect.y, g.present_rect.w,
          g.present_rect.h);
}

static void test_shaders_and_misc(void) {
    CHECK(strcmp(jpt_vk_result_str(VK_ERROR_DEVICE_LOST), "VK_ERROR_DEVICE_LOST") == 0, "VkResult string");
    CHECK(strcmp(jpt_vk_result_str(VK_SUBOPTIMAL_KHR), "VK_SUBOPTIMAL_KHR") == 0, "VK_SUBOPTIMAL_KHR string");

    char drv[48];
    jpt_vk_format_driver_version(0x10DEu, (555u << 22) | (85u << 14), drv, sizeof drv);
    CHECK(strncmp(drv, "555.85.", 7) == 0, "NVIDIA driver version decode: %s", drv);

    CHECK(jpt_vk_shader_count() == JPT_VK_SH_COUNT, "embedded shader count=%d", jpt_vk_shader_count());
    CHECK(strlen(jpt_vk_shader_bundle_sha256()) == 64, "shader bundle SHA-256 present");
    for (int i = 0; i < jpt_vk_shader_count(); i++) {
        size_t sz = 0;
        const uint32_t* w = jpt_vk_shader_words(i, &sz);
        CHECK(w != NULL && sz >= 20 && (sz % 4) == 0 && w[0] == 0x07230203u, "valid SPIR-V magic for %s",
              jpt_vk_shader_name(i));
    }

    jpt_vk_lifetime_t lt;
    memset(&lt, 0, sizeof lt);
    CHECK(jpt_vk_lifetime_is_clean(&lt), "zero lifetime is clean");
    lt.textures = 1;
    CHECK(!jpt_vk_lifetime_is_clean(&lt) && jpt_vk_lifetime_total_objects(&lt) == 1, "detects leaked texture");
}

static void test_present_policy_parity(void) {
    jpt_present_input_t in;
    jpt_present_plan_t plan;

    /* 1. Frontend HD and 640x480 UI layer are strictly mutually exclusive (D3D11 oracle) */
    memset(&in, 0, sizeof in);
    in.frames_allowed = 1;
    in.base_is_3d = 1;
    in.ao_wanted = 1;
    in.has_frontend_hd = 1;
    in.fe_w = 1920;
    in.fe_h = 1080;
    in.has_pointer = 1;
    in.has_ui_layer = 1;
    in.has_subtitle = 1;
    in.has_host_overlay = 1;
    in.has_hud = 1;
    in.has_text_layer = 1;
    in.out_w = 1920;
    in.out_h = 1080;
    jpt_present_plan_build(&in, &plan);

    CHECK(!plan.held, "menu frame is not held");
    CHECK(plan.draw_frontend_hd == 1, "frontend HD drawn when present");
    CHECK(plan.draw_pointer == 1, "cursor drawn with frontend HD");
    CHECK(plan.draw_ui_layer == 0, "640x480 UI layer suppressed when frontend HD is active");
    CHECK(plan.draw_ao == 0, "AO suppressed during frontend menu");
    CHECK(plan.draw_subtitle == 1 && plan.draw_host_overlay == 1, "subtitles and host overlays active");
    CHECK(plan.draw_text_layer_in_hud == 1, "text layer drawn inside HUD");
    CHECK(plan.end_text_frame == 1, "text frame always ended after present");
    CHECK(plan.stage_count == 5, "exact D3D11 stage count (got %d)", plan.stage_count);
    CHECK(plan.stages[0].stage == JPT_STAGE_BASE && plan.stages[0].apply_gamma == 1 &&
              plan.stages[0].blend == JPT_PRESENT_BLEND_OPAQUE,
          "stage 0 is BASE with gamma");
    CHECK(plan.stages[1].stage == JPT_STAGE_FRONTEND_HD && plan.stages[1].apply_gamma == 1 &&
              plan.stages[1].blend == JPT_PRESENT_BLEND_PREMUL && plan.stages[1].filter == JPT_PRESENT_FILTER_POINT,
          "stage 1 is 1:1 FRONTEND_HD with gamma + premul + point");
    CHECK(plan.stages[2].stage == JPT_STAGE_CURSOR && plan.stages[2].apply_gamma == 1 &&
              plan.stages[2].blend == JPT_PRESENT_BLEND_PREMUL && plan.stages[2].filter == JPT_PRESENT_FILTER_POINT,
          "stage 2 is CURSOR with gamma + premul + point");
    CHECK(plan.stages[3].stage == JPT_STAGE_VIDEO_SUBTITLES && plan.stages[3].apply_gamma == 0 &&
              plan.stages[3].blend == JPT_PRESENT_BLEND_PREMUL,
          "stage 3 is SUBTITLES without gamma + premul");
    CHECK(plan.stages[4].stage == JPT_STAGE_HOST_OVERLAYS && plan.stages[4].apply_gamma == 0 &&
              plan.stages[4].blend == JPT_PRESENT_BLEND_STRAIGHT,
          "stage 4 is HOST_OVERLAYS without gamma + straight alpha");

    /* Scaled frontend HD uses linear filter */
    in.out_w = 2560;
    in.out_h = 1440;
    jpt_present_plan_build(&in, &plan);
    CHECK(plan.stages[1].filter == JPT_PRESENT_FILTER_LINEAR, "scaled FRONTEND_HD uses linear filter");

    /* 2. Fallback to 640x480 UI layer when Frontend HD is absent */
    in.has_frontend_hd = 0;
    jpt_present_plan_build(&in, &plan);
    CHECK(plan.draw_frontend_hd == 0 && plan.draw_pointer == 0 && plan.draw_ui_layer == 1,
          "UI layer drawn as fallback when frontend HD absent");
    CHECK(plan.stages[1].stage == JPT_STAGE_UI_LAYER && plan.stages[1].apply_gamma == 1 &&
              plan.stages[1].blend == JPT_PRESENT_BLEND_PREMUL,
          "UI layer uses gamma + premul");

    /* 3. Gameplay with AO and without HUD suppresses text_layer_in_hud */
    memset(&in, 0, sizeof in);
    in.frames_allowed = 1;
    in.base_is_3d = 1;
    in.ao_wanted = 1;
    in.has_host_overlay = 0;
    in.has_hud = 0;
    in.has_text_layer = 1;
    in.out_w = 1920;
    in.out_h = 1080;
    jpt_present_plan_build(&in, &plan);
    CHECK(plan.draw_ao == 1 && plan.stage_count == 2 && plan.stages[1].stage == JPT_STAGE_AO,
          "gameplay enables AO after base");
    CHECK(plan.draw_text_layer_in_hud == 0 && plan.end_text_frame == 1,
          "text layer not drawn in HUD when HUD off, yet text frame still ends");

    /* 4. Loading HD & UI canvas held-present parity */
    in.ui_canvas_held = 1;
    jpt_present_plan_build(&in, &plan);
    CHECK(plan.held == 1 && plan.stage_count == 0 && plan.end_text_frame == 1, "UI blank transition held cleanly");

    in.ui_canvas_held = 0;
    in.loading_hd_held = 1;
    jpt_present_plan_build(&in, &plan);
    CHECK(plan.held == 1 && plan.stage_count == 0 && plan.end_text_frame == 1, "Loading HD held cleanly");

    /* 5. Render size policy parity */
    unsigned rw = 0, rh = 0;
    jpt_choose_render_size(1280, 720, 2560, 1440, 640, 480, &rw, &rh);
    CHECK(rw == 960 && rh == 720, "fixed box 1280x720 with 4:3 guest -> 960x720 (got %ux%u)", rw, rh);
    jpt_choose_render_size(0, 0, 2560, 1440, 640, 480, &rw, &rh);
    CHECK(rw == 1920 && rh == 1440, "auto 2560x1440 with 4:3 guest -> 1920x1440 (got %ux%u)", rw, rh);
}

static void test_colour_gamma_policy_parity(void) {
    /* Lossless premultiplied BGRA -> RGBA swizzle (OP_DIM scrim + red hover button) */
    uint32_t bgra[3] = {
        0xB3000000u, /* OP_DIM dark scrim: A=179, premul RGB=0 */
        0xFFDC1E19u, /* menu red hover: A=255, R=220, G=30, B=25 */
        0x80601008u, /* translucent premul: A=128, R=96, G=16, B=8 */
    };
    uint8_t rgba[12];
    jpt_bgra_to_rgba_premul(bgra, rgba, 3);
    CHECK(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 179, "OP_DIM scrim lossless");
    CHECK(rgba[4] == 220 && rgba[5] == 30 && rgba[6] == 25 && rgba[7] == 255, "menu red lossless");
    CHECK(rgba[8] == 96 && rgba[9] == 16 && rgba[10] == 8 && rgba[11] == 128,
          "translucent premul preserved without lossy un-premultiply");

    /* Premultiplied-over darkening scrim over mid-grey base */
    uint32_t dst = 0xFF808080u;
    uint32_t scrim = (179u << 24);
    uint32_t blended = jpt_blend_premul_over_rgba(dst, scrim);
    uint8_t br = (uint8_t)(blended & 0xFFu);
    CHECK(br >= 37 && br <= 39, "OP_DIM premul scrim darkens 128 -> ~38 (got %u)", br);

    /* Non-identity gamma LUT reference */
    uint16_t red[256], green[256], blue[256];
    for (int i = 0; i < 256; i++) {
        double x = (double)i / 255.0;
        uint16_t v = (uint16_t)(pow(x, 0.7) * 65535.0 + 0.5);
        red[i] = green[i] = blue[i] = v;
    }
    uint32_t lit = jpt_apply_gamma_rgba(0xFF191EDCu, red, green, blue);
    uint8_t lr = (uint8_t)(lit & 0xFFu), lg = (uint8_t)((lit >> 8) & 0xFFu), lb = (uint8_t)((lit >> 16) & 0xFFu);
    CHECK(lr > 220 && lg > 30 && lb > 25 && (lit >> 24) == 255, "gamma ramp brightens RGB and preserves alpha");
}

static void test_controls_card_and_prompts_policy(void) {
    const jpt_card_t* kb_card = jpt_prompt_card(0);
    const jpt_card_t* pad_card = jpt_prompt_card(1);
    const jpt_card_t* kb_hint = jpt_prompt_hint_card(0);
    const jpt_card_t* pad_hint = jpt_prompt_hint_card(1);

    CHECK(kb_card != NULL && kb_card->count > 0 && kb_card->first_column > 0, "KB controls card defined");
    CHECK(pad_card != NULL && pad_card->count > 0 && pad_card->first_column > 0, "Gamepad controls card defined");
    CHECK(kb_hint != NULL && kb_hint->title && kb_hint->title[0] != 0 && kb_hint->count == 0,
          "KB View Controls hint is title-only panel");
    CHECK(pad_hint != NULL && pad_hint->title && pad_hint->title[0] != 0 && pad_hint->count == 0,
          "Gamepad View Controls hint is title-only panel");

    static const int k_res[][2] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
    for (size_t r = 0; r < sizeof k_res / sizeof k_res[0]; r++) {
        int w = k_res[r][0], h = k_res[r][1];
        jpt_box_t area;
        int max_h = 0;
        jpt_card_area(0, 0, w, h, h * 30 / 1000, 0, &area, &max_h);
        CHECK(area.right > area.left && area.bottom > area.top && max_h > 0, "card area valid at %dx%d", w, h);

        jpt_card_metrics_t m;
        memset(&m, 0, sizeof m);
        m.count = kb_card->count;
        m.first_column = kb_card->first_column;
        m.title_w = max_h * 8;
        for (int i = 0; i < m.count && i < JPT_CARD_MAX_ROWS; i++) {
            m.binding_w[i] = max_h * 3;
            m.action_w[i] = max_h * 6;
        }
        int cols = 2;
        int chosen_h = jpt_card_choose(&m, max_h, max_h, area, &cols);
        jpt_card_layout_t lay;
        CHECK(chosen_h >= JPT_CARD_FLOOR_TEXT && chosen_h <= max_h, "card text height bounded at %dx%d (%d)", w, h,
              chosen_h);
        CHECK(jpt_card_place(&m, chosen_h, cols, area, &lay), "card fits cleanly at %dx%d", w, h);
        CHECK(lay.box.left >= area.left && lay.box.right <= area.right && lay.box.top >= area.top &&
                  lay.box.bottom <= area.bottom,
              "card box stays inside area at %dx%d", w, h);
        for (int i = 1; i < m.first_column; i++)
            CHECK(lay.row_y[i] > lay.row_y[i - 1], "column 1 rows strictly ordered without overlap at %dx%d", w, h);
    }
}

/* The launcher's Vulkan probe over a scripted loader: no Vulkan, device or driver. */
static struct {
    const char* missing;
    uint32_t loader_version;
    uint32_t device_version;
    VkResult instance, count, list;
    uint32_t devices, listed;
    int instances, destroyed, devices_created;
} F;

static VkResult VKAPI_CALL fake_version(uint32_t* version) { return *version = F.loader_version, VK_SUCCESS; }
static VkResult VKAPI_CALL fake_instance_exts(const char* layer, uint32_t* count, VkExtensionProperties* out) {
    (void)layer, (void)out;
    return *count = 0, VK_SUCCESS;
}
static VkResult VKAPI_CALL fake_create_instance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    (void)a;
    CHECK(ci->pApplicationInfo->apiVersion <= F.loader_version, "the instance asks for no more than the loader has");
    *out = (VkInstance)(uintptr_t)(F.instance == VK_SUCCESS ? 0x10 : 0xBAD);
    F.instances += F.instance == VK_SUCCESS;
    return F.instance;
}
static void VKAPI_CALL fake_destroy_instance(VkInstance i, const VkAllocationCallbacks* a) {
    (void)a;
    CHECK(i == (VkInstance)(uintptr_t)0x10, "only a created instance is destroyed");
    F.destroyed++;
}
static VkResult VKAPI_CALL fake_enumerate(VkInstance i, uint32_t* count, VkPhysicalDevice* out) {
    (void)i;
    if (!out) return *count = F.devices, F.count;
    if (F.list != VK_SUCCESS && F.list != VK_INCOMPLETE) return F.list;
    *count = F.listed < *count ? F.listed : *count;
    for (uint32_t n = 0; n < *count; n++) out[n] = (VkPhysicalDevice)(uintptr_t)(0x100 + n);
    return F.list;
}
static void VKAPI_CALL fake_properties(VkPhysicalDevice d, VkPhysicalDeviceProperties* out) {
    CHECK((uintptr_t)d >= 0x100 && (uintptr_t)d < 0x110, "only a listed device is inspected");
    memset(out, 0, sizeof *out);
    out->apiVersion = F.device_version;
    out->vendorID = 0x8086;
    out->deviceID = 0x0102;
    out->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    out->limits.framebufferColorSampleCounts = out->limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_4_BIT;
    strcpy(out->deviceName, "Old GPU");
}
static void VKAPI_CALL fake_properties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* out) { (void)d, (void)out; }
static void VKAPI_CALL fake_features(VkPhysicalDevice d, VkPhysicalDeviceFeatures* out) { (void)d, memset(out, 0, sizeof *out); }
static void VKAPI_CALL fake_memory(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties* out) { (void)d, memset(out, 0, sizeof *out); }
static void VKAPI_CALL fake_queues(VkPhysicalDevice d, uint32_t* count, VkQueueFamilyProperties* out) {
    (void)d;
    *count = 1;
    if (out) memset(out, 0, sizeof *out), out->queueFlags = VK_QUEUE_GRAPHICS_BIT;
}
static void VKAPI_CALL fake_formats(VkPhysicalDevice d, VkFormat f, VkFormatProperties* out) {
    (void)d, (void)f;
    memset(out, 0, sizeof *out);
    out->optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}
static VkResult VKAPI_CALL fake_device_exts(VkPhysicalDevice d, const char* layer, uint32_t* count, VkExtensionProperties* out) {
    (void)d, (void)layer, (void)out;
    return *count = 0, VK_ERROR_OUT_OF_HOST_MEMORY;
}
static VkResult VKAPI_CALL fake_create_device(VkPhysicalDevice d, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    (void)d, (void)ci, (void)a, (void)out;
    F.devices_created++;
    return VK_ERROR_INITIALIZATION_FAILED;
}
static PFN_vkVoidFunction VKAPI_CALL fake_device_proc(VkDevice d, const char* name) { return (void)d, (void)name, NULL; }

static PFN_vkVoidFunction VKAPI_CALL fake_proc(VkInstance instance, const char* name) {
    static const struct {
        const char* name;
        PFN_vkVoidFunction fn;
    } k[] = {
        { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)fake_version },
        { "vkEnumerateInstanceExtensionProperties", (PFN_vkVoidFunction)fake_instance_exts },
        { "vkCreateInstance", (PFN_vkVoidFunction)fake_create_instance },
        { "vkDestroyInstance", (PFN_vkVoidFunction)fake_destroy_instance },
        { "vkEnumeratePhysicalDevices", (PFN_vkVoidFunction)fake_enumerate },
        { "vkGetPhysicalDeviceProperties", (PFN_vkVoidFunction)fake_properties },
        { "vkGetPhysicalDeviceProperties2", (PFN_vkVoidFunction)fake_properties2 },
        { "vkGetPhysicalDeviceFeatures", (PFN_vkVoidFunction)fake_features },
        { "vkGetPhysicalDeviceMemoryProperties", (PFN_vkVoidFunction)fake_memory },
        { "vkGetPhysicalDeviceQueueFamilyProperties", (PFN_vkVoidFunction)fake_queues },
        { "vkGetPhysicalDeviceFormatProperties", (PFN_vkVoidFunction)fake_formats },
        { "vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)fake_device_exts },
        { "vkCreateDevice", (PFN_vkVoidFunction)fake_create_device },
        { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)fake_device_proc },
    };
    (void)instance;
    if (F.missing && !strcmp(name, F.missing)) return NULL;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(name, k[i].name)) return k[i].fn;
    return NULL;
}

static void fake_reset(void) {
    memset(&F, 0, sizeof F);
    F.loader_version = F.device_version = VK_API_VERSION_1_2;
    F.devices = F.listed = 1;
}

static jpt_vk_probe_result_t fake_probe(void) {
    jpt_vk_probe_result_t r;
    jpt_vk_loader_t vk;
    char err[128] = "";
    memset(&r, 0, sizeof r);
    memset(&vk, 0, sizeof vk);
    if (!jpt_vk_loader_bind(&vk, fake_proc, err, sizeof err)) {
        r.status = JPT_VK_PROBE_LOADER;
        CHECK(err[0] && !vk.loaded && !vk.vkGetInstanceProcAddr, "a loader without its entry points is closed with a reason");
        return r;
    }
    jpt_vk_probe_loader(&vk, "", &r);
    CHECK(!vk.loaded, "the probe takes the loader");
    CHECK(F.instances == F.destroyed, "every created instance is destroyed");
    CHECK(F.devices_created == 0, "the probe creates no device");
    CHECK(r.available == (r.status == JPT_VK_PROBE_OK), "available only with status ok");
    CHECK(r.summary[0], "every probe ends with a summary");
    CHECK(jpt_vk_probe_status_name(r.status)[0] && strcmp(jpt_vk_probe_status_name(r.status), "not_run"), "named status");
    return r;
}

static void test_launcher_probe_failures(void) {
    static const char* const k_instance_functions[] = {
        "vkDestroyInstance", "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceFeatures",
        "vkGetPhysicalDeviceMemoryProperties", "vkGetPhysicalDeviceQueueFamilyProperties", "vkGetPhysicalDeviceFormatProperties",
        "vkEnumerateDeviceExtensionProperties", "vkCreateDevice", "vkGetDeviceProcAddr",
    };
    jpt_vk_probe_result_t r;

    fake_reset();
    r = fake_probe();
    CHECK(r.status == JPT_VK_PROBE_OK && r.gpu_count == 1 && r.selected_index == 0 && r.gpus[0].usable, "a capable device is listed");

    fake_reset();
    F.missing = "vkCreateInstance";
    CHECK(fake_probe().status == JPT_VK_PROBE_LOADER, "no vkCreateInstance");

    for (size_t i = 1; i < sizeof k_instance_functions / sizeof k_instance_functions[0]; i++) {
        fake_reset();
        F.missing = k_instance_functions[i];
        r = fake_probe();
        CHECK(r.status == JPT_VK_PROBE_ENTRY_POINTS && r.gpu_count == 0, "without %s nothing is called", F.missing);
    }

    fake_reset();
    F.instance = VK_ERROR_INCOMPATIBLE_DRIVER;
    r = fake_probe();
    CHECK(r.status == JPT_VK_PROBE_INSTANCE && strstr(r.summary, "VK_ERROR_INCOMPATIBLE_DRIVER"), "instance failure: %s", r.summary);

    fake_reset();
    F.devices = 0;
    CHECK(fake_probe().status == JPT_VK_PROBE_NO_DEVICE, "no device");

    fake_reset();
    F.count = VK_ERROR_INITIALIZATION_FAILED;
    CHECK(fake_probe().status == JPT_VK_PROBE_ENUMERATION, "counting devices fails");

    fake_reset();
    F.devices = 4;
    F.list = VK_ERROR_OUT_OF_HOST_MEMORY;
    r = fake_probe();
    CHECK(r.status == JPT_VK_PROBE_ENUMERATION && r.gpu_count == 0, "listing devices fails after counting them");

    fake_reset();
    F.devices = 4;
    F.listed = 0;
    CHECK(fake_probe().status == JPT_VK_PROBE_NO_DEVICE, "the devices are gone when they are listed");

    fake_reset();
    F.devices = F.listed = JPT_VK_MAX_GPUS + 5;
    F.list = VK_INCOMPLETE;
    r = fake_probe();
    CHECK(r.status == JPT_VK_PROBE_OK && r.gpu_count == JPT_VK_MAX_GPUS, "more devices than fit: %d", r.gpu_count);

    /* A loader of Vulkan 1.0 has no vkEnumerateInstanceVersion. */
    fake_reset();
    F.missing = "vkEnumerateInstanceVersion";
    F.loader_version = F.device_version = VK_API_VERSION_1_0;
    r = fake_probe();
    CHECK(r.status == JPT_VK_PROBE_NO_USABLE_DEVICE && r.gpu_count == 1 && !r.gpus[0].usable && strstr(r.gpus[0].reject_reason, "1.0"),
          "Vulkan 1.0 is listed and refused: %s", r.gpus[0].reject_reason);

    for (uint32_t minor = 1; minor <= 3; minor++) {
        fake_reset();
        F.device_version = VK_MAKE_API_VERSION(0, 1, minor, 0);
        F.loader_version = F.device_version;
        CHECK(fake_probe().status == JPT_VK_PROBE_OK, "Vulkan 1.%u keeps the game's own requirement", minor);
    }
    CHECK(!jpt_vk_probe_headless("", NULL), "no result to fill");
}

int main(void) {
    test_launcher_probe_failures();
    test_renderer_policy();
    test_renderer_selection_precedence();
    test_gpu_selection();
    test_device_capabilities();
    test_present_mode_and_swapchain_stress();
    test_pipeline_key_and_barriers();
    test_shaders_and_misc();
    test_present_policy_parity();
    test_colour_gamma_policy_parity();
    test_controls_card_and_prompts_policy();

    if (g_failures) {
        fprintf(stderr, "vulkan_policy_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("vulkan_policy_tests: %d checks, 0 failures\n", g_checks);
    return 0;
}
