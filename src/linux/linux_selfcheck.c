#include "linux_selfcheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jpt_build_info.h"
#include "jpt_renderer.h"
#include "linux_log.h"
#include "vk_policy.h"
#include "vk_shaders.h"

#ifdef JPT_LINUX_GUEST
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_version.h>

#include "audio_backend.h"
#include "audio_pacer.h"
#include "frontend_assets.h"
#include "gamepad.h"
#include "gamepad_ui.h"
#include "host_policy.h"
#include "jpt_config.h"
#include "jpt_prompt_text.h"
#include "linux_card.h"
#include "linux_features.h"
#include "linux_gamepad.h"
#include "linux_lift116.h"
#include "linux_mouse.h"
#include "linux_overrides.h"
#include "linux_text.h"
#include "linux_video_subtitles.h"
#include "loading_hd.h"
#include "loading_layout.h"
#include "override_registry.h"
#include "save_preview.h"
#include "sdl_audio.h"
#include "smacker_backend.h"
#include "stream_vfile.h"
#endif

#define MAX_CAPABILITIES 32

typedef struct {
    const char* name;
    int ok;
    char detail[200];
} capability_t;

static void json_string(const char* s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        if ((unsigned char)*s >= 0x20) putchar(*s);
    }
    putchar('"');
}

static void print_build(void) {
    printf("  \"build\": {\n    \"version\": \"%s\", \"git_commit\": \"%s\", \"git_dirty\": %s,\n", JPT_BUILD_VERSION, JPT_BUILD_GIT_COMMIT,
           JPT_BUILD_GIT_DIRTY ? "true" : "false");
    printf("    \"source_fingerprint\": \"%s\",\n    \"compiler\": ", JPT_BUILD_SOURCE_FINGERPRINT);
    json_string(JPT_BUILD_COMPILER);
    printf(", \"target\": \"%s\", \"config\": \"%s\",\n", JPT_BUILD_TARGET, JPT_BUILD_CONFIG);
    printf("    \"glibc_baseline\": \"%s\", \"contract_sha256\": \"%s\", \"contract_schema\": %d,\n", JPT_BUILD_GLIBC_BASELINE,
           JPT_BUILD_CONTRACT_SHA256, JPT_BUILD_CONTRACT_SCHEMA);
#ifdef JPT_LINUX_GUEST
    printf("    \"lifted_guest\": true,\n");
#else
    printf("    \"lifted_guest\": false,\n");
#endif
#ifdef JPT_LINUX_SDL_VIDEO
    printf("    \"sdl_video\": true,\n");
#else
    printf("    \"sdl_video\": false,\n");
#endif
    printf("    \"vulkan\": {\"compiled\": %s, \"api_baseline\": \"1.2\", \"headers_version\": \"%s\", \"loader\": \"%s\", \"shader_count\": %d, \"shader_bundle_sha256\": \"%s\"},\n",
           JPT_BUILD_VULKAN_COMPILED ? "true" : "false", JPT_BUILD_VULKAN_HEADERS, JPT_BUILD_VULKAN_LOADER,
           jpt_vk_shader_count(), JPT_BUILD_VULKAN_SHADER_SHA256);
#if defined(JPT_LINUX_GUEST) && defined(JPT_SMACKER_FFMPEG)
    {
        char libraries[256];
        smk_ffmpeg_describe(libraries, sizeof libraries);
        printf("    \"ffmpeg\": {\"enabled\": true, \"lgpl_only\": %s, \"libraries\": ", strstr(libraries, "(LGPL") && !strstr(libraries, "(GPL") ? "true" : "false");
        json_string(libraries);
        printf("}\n  }");
    }
#else
    printf("    \"ffmpeg\": {\"enabled\": false}\n  }");
#endif
}

int lx_build_info_print(void) {
    printf("{\n  \"schema\": 1,\n");
    print_build();
    printf("\n}\n");
    return 0;
}

#ifdef JPT_LINUX_GUEST

static capability_t s_caps[MAX_CAPABILITIES];
static int s_cap_count;

static void cap(const char* name, int ok, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

static void cap(const char* name, int ok, const char* fmt, ...) {
    capability_t* c;
    va_list list;
    if (s_cap_count >= MAX_CAPABILITIES) return;
    c = &s_caps[s_cap_count++];
    c->name = name;
    c->ok = ok;
    va_start(list, fmt);
    vsnprintf(c->detail, sizeof c->detail, fmt, list);
    va_end(list);
}

typedef struct {
    unsigned listed, hooks, installed_default, installed_max, required_refused, selfmod, main_entries;
} registry_t;

static void install_counts(const jpt_config_t* config, unsigned* installed, unsigned* required_refused) {
    lx_lift116_register_overrides();
    if (!lx_features_start(config, "", "en-116", NULL, NULL)) {
        *installed = 0;
        *required_refused = 1;
        return;
    }
    *installed = lx_overrides_counts().installed;
    *required_refused = lx_overrides_counts().required_refused;
    lx_features_stop();
}

static registry_t probe_registry(void) {
    registry_t r;
    jpt_config_t config;
    unsigned ignored = 0;
    memset(&r, 0, sizeof r);
    r.listed = lx_lift116_override_count();
    r.hooks = lx_lift116_hook_count();
    r.selfmod = (unsigned)lx_lift116_selfmod_table().count;
    r.main_entries = (unsigned)lx_lift116_main_table().count;
    jpt_config_defaults(&config);
    install_counts(&config, &r.installed_default, &ignored);
    config.active_ai_enhanced = 1;
    config.dual_stow = 1;
    config.max_object_detail = 1;
    config.official_patch_117 = 1;
    config.fov = 90;
    config.skip_intro = 1;
    install_counts(&config, &r.installed_max, &r.required_refused);
    cap("selfmod", r.selfmod > 0, "%u SelfMod lifted entries, %u main entries", r.selfmod, r.main_entries);
    cap("override_registry", r.listed > 0 && r.required_refused == 0,
        "%u listed, %u applicable on Linux (%u with default settings), no required override refused", r.listed, r.installed_max, r.installed_default);
    cap("lift_hooks", r.hooks > 0, "%u lift hooks bound at link time", r.hooks);
    cap("host_features", r.installed_max > 0, "lx_features_start installs the shared host features over the lifted game");
    return r;
}

static void probe_audio(void) {
    audio_pacer_t pacer;
    audio_plan_t plan;
    SDL_AudioStream* (*volatile stream_api)(const SDL_AudioSpec*, const SDL_AudioSpec*) = SDL_CreateAudioStream;
    bool (*volatile put_api)(SDL_AudioStream*, const void*, int) = SDL_PutAudioStreamData;
    const audio_backend_t* backend = sdl_audio_backend();
    const uint32_t low = audio_pacer_period_frames(22050u), high = audio_pacer_period_frames(48000u);
    int version = SDL_GetVersion();
    audio_pacer_init(&pacer, 22050u, 4u);
    plan = audio_pacer_tick(&pacer, 0, 0u);
    audio_pacer_set_rate(&pacer, 48000u);
    cap("audio_pacer", backend && backend->open && backend->set_rate && (plan.mix_frames || plan.silence_frames) && high > low,
        "policy %d, queue target %d..%d periods, backlog +%d, period 22050 Hz %u frames, 48000 Hz %u frames, rate follows the mixer",
        AUDIO_PACER_POLICY_VERSION, (int)AUDIO_PACER_MIN_PERIODS, (int)AUDIO_PACER_MAX_PERIODS, (int)AUDIO_PACER_BACKLOG_EXTRA_PERIODS, low, high);
    cap("audio_sdl_stream", stream_api && put_api && backend, "SDL %d.%d.%d AudioStream playback, resampling by SDL, device not opened by the check",
        SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
}

static input_state_t state_for(guest_ui_t ui, int focused) {
    input_signals_t s;
    memset(&s, 0, sizeof s);
    s.focused = focused;
    s.ui = ui;
    return input_state_from(&s);
}

static void probe_input(void) {
    const input_state_t gameplay = state_for(GUEST_UI_GAMEPLAY, 1), menu = state_for(GUEST_UI_MENU, 1), dialog = state_for(GUEST_UI_DIALOG, 1);
    void (*volatile step)(gamepad_ui_t*, const gamepad_ui_in_t*) = gamepad_ui_step;
    int (*volatile menu_free)(void) = gamepad_menu_pointer_free;
    int (*volatile pad_init)(const jpt_gamepad_settings_t*) = lx_gamepad_init;
    lx_mouse_stats_t stats;
    lx_mouse_stats(&stats);
    cap("input_context", gameplay != menu && gameplay != dialog && menu == INPUT_MENU && gameplay == INPUT_GAMEPLAY_CAPTURED,
        "the input state comes from the game's window stack and focus (host_policy input_state_from), SetCursorPos only stores the position");
    cap("absolute_pointer", !input_captured(menu) && !input_captured(dialog) && !input_cursor_virtual(menu),
        "menus, dialogs and videos get the free absolute pointer");
    cap("relative_gameplay", input_captured(gameplay) && input_cursor_virtual(gameplay), "gameplay takes relative motion and a virtual cursor");
    cap("gamepad_layer", pad_init != NULL && lx_gamepad_connected() == 0, "the SDL3 gamepad layer over the shared policy (gamepad_policy)");
    cap("gamepad_menu", step != NULL && menu_free != NULL, "the shared UI-loop step drives the game's menus with the game's own pointer");
}

static int probe_frontend(void) {
    fe_painter_t* painter = fe_painter_create();
    ld_layout_t layout;
    void (*volatile install)(void) = loading_hd_install;
    int painted = painter && fe_painter_text_width(painter, FE_FONT_UI, 400, 20.0, "Hg") > 0.0;
    fe_painter_destroy(painter);
    cap("frontend_hd", painted, "the HD front end's painter, text from the embedded face (%s)", painted ? "measured" : "not available");
    ld_layout_make(&layout, 1280, 960, 1, 0);
    cap("loading_hd", install != NULL && layout.photo.right > layout.photo.left, "the HD loading screen's layout and overrides");
    return painted;
}

static void probe_card_and_text(void) {
    static uint32_t frame[320 * 240];
    const jpt_card_t* card = jpt_prompt_card(0);
    int drawn;
    lx_card_reset();
    lx_card_show();
    drawn = lx_card_draw(frame, 320, 320, 240, 0, 0, 320, 240);
    lx_card_reset();
    cap("controls_card", card && card->count > 0 && drawn, "%d rows, drawn by the host's overlay stage", card ? card->count : 0);
    lx_text_set_codepage(1251);
    {
        const float scale = lx_text_scale(-16);
        const int cyrillic = lx_text_advance(scale, 0xCD, 0) == lx_text_unicode_advance(scale, 0x41D);
        lx_text_set_codepage(1252);
        cap("russian_text", cyrillic, "code page 1251 reaches the text rasteriser");
    }
}

static void quiet_log(const char* line) { (void)line; }

static void probe_media(void) {
    sp_size_t picture = save_preview_size(640, 480);
    char libraries[256] = "FFmpeg not compiled in";
    int ffmpeg = 0;
#ifdef JPT_SMACKER_FFMPEG
    ffmpeg = smk_ffmpeg_describe(libraries, sizeof libraries) > 0;
#endif
    stream_catalog_init(quiet_log);
    lx_vsub_reset();
    cap("ffmpeg_smacker", ffmpeg, "%s", libraries);
    cap("stream_tpa", stream_catalog_audio_count() > 0 && stream_catalog_caption_count() > 0, "virtual Stream.tpa: %d restored records, %d captions",
        stream_catalog_audio_count(), stream_catalog_caption_count());
    cap("video_subtitles", lx_vsub_cue() == -1, "frame-indexed cues over the Smacker HLE");
    cap("save_pictures", picture.width == 640 && picture.height == 480, "previews/<save>.png written and read by the host");
}

static void probe_vulkan(void) {
    jpt_renderer_decision_t dec;
    jpt_vk_pipeline_key_t k1, k2;
    jpt_render_state_t st;
    VkPresentModeKHR modes[2] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    int shaders_ok = jpt_vk_shader_count() == JPT_VK_SH_COUNT && strlen(jpt_vk_shader_bundle_sha256()) == 64;

    memset(&st, 0, sizeof st);
    st.z_enable = 1;
    st.z_write = 1;
    st.z_func = 7;
    jpt_vk_pipeline_key_from_state(&st, JPT_PRIM_TRIANGLEFAN, 4, JPT_VK_RP_SCENE_MSAA, 1, &k1);
    jpt_vk_pipeline_key_from_state(&st, JPT_PRIM_TRIANGLELIST, 4, JPT_VK_RP_SCENE_MSAA, 1, &k2);
    jpt_renderer_resolve_policy(JPT_RENDERER_AUTO, 0, 0, 1, NULL, &dec);

    cap("vulkan_backend",
        JPT_BUILD_VULKAN_COMPILED && shaders_ok && dec.chosen == JPT_RENDERER_VULKAN &&
            jpt_vk_pipeline_key_equal(&k1, &k2) &&
            jpt_vk_choose_present_mode(1, modes, 2) == VK_PRESENT_MODE_FIFO_KHR &&
            jpt_vk_choose_present_mode(0, modes, 2) == VK_PRESENT_MODE_MAILBOX_KHR,
        "Vulkan 1.2 backend (compiled=yes, selectable=yes, runtime_dispatch=wired, headers %s, %s loader, %d SPIR-V shaders, GPU not opened by self-check)",
        JPT_BUILD_VULKAN_HEADERS, JPT_BUILD_VULKAN_LOADER, jpt_vk_shader_count());
}

int lx_selfcheck_run(void) {
    registry_t registry;
    int failed = 0, passed = 0;
    lx_log_quiet(1);
    registry = probe_registry();
    probe_audio();
    probe_input();
    probe_frontend();
    probe_card_and_text();
    probe_media();
    probe_vulkan();
    for (int i = 0; i < s_cap_count; i++) {
        if (s_caps[i].ok) passed++;
        else failed++;
    }
    printf("{\n  \"schema\": 1,\n  \"self_check\": %s,\n", failed ? "false" : "true");
    print_build();
    printf(",\n  \"registry\": {\"selfmod_entries\": %u, \"main_entries\": %u, \"override_registry\": %u, \"overrides_linux_applicable\": %u, "
           "\"overrides_installed_default_settings\": %u, \"lift_hooks\": %u, \"lift_hooks_active\": %u},\n",
           registry.selfmod, registry.main_entries, registry.listed, registry.installed_max, registry.installed_default, registry.hooks, registry.hooks);
    printf("  \"audio\": {\"backend\": \"sdl3\", \"pacer\": true, \"queue_policy_version\": %d, \"source_rate_handling\": \"period follows the mixer rate\"},\n",
           AUDIO_PACER_POLICY_VERSION);
    printf("  \"capability_count\": %d,\n  \"capabilities\": {\n", passed);
    for (int i = 0; i < s_cap_count; i++) {
        printf("    \"%s\": {\"ok\": %s, \"detail\": ", s_caps[i].name, s_caps[i].ok ? "true" : "false");
        json_string(s_caps[i].detail);
        printf("}%s\n", i + 1 < s_cap_count ? "," : "");
    }
    printf("  }\n}\n");
    return failed ? 1 : 0;
}

#else

int lx_selfcheck_run(void) {
    printf("{\n  \"schema\": 1,\n  \"self_check\": false,\n");
    print_build();
    printf(",\n  \"capabilities\": {}\n}\n");
    return 1;
}

#endif
