/*
 * Trespasser recompilation host: a 32-bit process on pcrecomp's native32 runtime.
 * The lifted .text runs as C; imports go to the real Win32 DLLs; SelfMod runs natively.
 * Structure follows sp00nznet/themovies host.c (MIT).
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/d3d_legacy.h"
#include "gamepad.h"
#include "jpt_config.h"
#include "jpt_display.h"
#include "jpt_launcher.h"
#include "jpt_user_data.h"
#include "l10n.h"
#include "loading_hd.h"
#include "stream_audio.h"
#include "recomp_trace.h"

#ifndef TRESPASSER_DEFAULT_GAME_DIR
#define TRESPASSER_DEFAULT_GAME_DIR "game"
#endif
/* The flat user folder of earlier development builds, migrated into the user data root. */
#ifndef TRESPASSER_LEGACY_USER_DIR
#define TRESPASSER_LEGACY_USER_DIR NULL
#endif
#ifndef TRESPASSER_LOG_DIR
#define TRESPASSER_LOG_DIR "build\\logs"
#endif

/* Lifted frames are several times the original's, and they live on the host stack. */
#define GUEST_THREAD_STACK (64u << 20)
#define MAX_OVERRIDES 16
#define RESTARTED_ENV "TRESPASSER_RESTARTED"

typedef struct {
    const char* key;
    const char* value;
} override_t;

typedef struct {
    const char* game_dir;
    const char* user_data_root;
    const char* log_dir;
    const char* log_file;
    char guest_args[MAX_PATH];
    unsigned watchdog_seconds;
    int dry_run;
    int force_launcher, skip_launcher;
    int system_ddraw;
    int dev_filter;
    int lifted_water;
    override_t overrides[MAX_OVERRIDES];
    int override_count;
} options_t;

static native32_shim_t s_shims[] = {
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "MessageBoxA", shim_MessageBoxA },
    { "DialogBoxParamA", shim_DialogBoxParamA },
    { "OutputDebugStringA", shim_OutputDebugStringA },
    { "RegOpenKeyExA", shim_RegOpenKeyExA },
    { "RegCreateKeyA", shim_RegCreateKeyA },
    { "RegQueryValueExA", shim_RegQueryValueExA },
    { "RegSetValueExA", shim_RegSetValueExA },
    { "RegDeleteValueA", shim_RegDeleteValueA },
    { "RegCloseKey", shim_RegCloseKey },
    { "CreateFileA", shim_CreateFileA },
    { "CloseHandle", shim_CloseHandle },
    { "ReadFile", shim_ReadFile },
    { "SetFilePointer", shim_SetFilePointer },
    { "GetFileSize", shim_GetFileSize },
    { "GetFileAttributesA", shim_GetFileAttributesA },
    { "FindFirstFileA", shim_FindFirstFileA },
    { "LZOpenFileA", shim_LZOpenFileA },
    { "LoadLibraryA", shim_LoadLibraryA },
    { "fopen", shim_fopen },
    { "_open", shim_open },
    { "DeleteFileA", shim_DeleteFileA },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "DestroyWindow", shim_DestroyWindow },
    { "PostQuitMessage", shim_PostQuitMessage },
    { "SetWindowLongA", shim_SetWindowLongA },
    { "SetWindowPos", shim_SetWindowPos },
    { "MoveWindow", shim_MoveWindow },
    { "SetWindowTextA", shim_SetWindowTextA },
    { "ShowWindow", shim_ShowWindow },
    { "GetWindowRect", shim_GetWindowRect },
    { "GetClientRect", shim_GetClientRect },
    { "ClientToScreen", shim_ClientToScreen },
    { "GetCursorPos", shim_GetCursorPos },
    { "SetCursorPos", shim_SetCursorPos },
    { "ClipCursor", shim_ClipCursor },
    { "ShowCursor", shim_ShowCursor },
    { "SetCapture", shim_SetCapture },
    { "ReleaseCapture", shim_ReleaseCapture },
    { "EnableMenuItem", shim_EnableMenuItem },
    { "PeekMessageA", shim_PeekMessageA },
    { "IsIconic", shim_IsIconic },
    { "GetForegroundWindow", shim_GetForegroundWindow },
    { "CreateThread", shim_CreateThread },
    { "GetTickCount", shim_GetTickCount },
    { "TextOutA", shim_TextOutA },
    { "LoadStringA", shim_LoadStringA },
    { "CreateFontA", shim_CreateFontA },
    { "CreateFontIndirectA", shim_CreateFontIndirectA },
    { "exit", shim_exit },
    { "_exit", shim__exit },
    { "ExitProcess", shim_ExitProcess },
};


static void usage(void) {
    printf("usage: trespasser [--launcher | --skip-launcher] [--dry-run] [--game DIR] [--user_data_root DIR]\n"
           "                  [--log-file FILE | --log-dir DIR] [--watchdog SECONDS] [--ddraw compat|system]\n"
           "                  [--native-trace] [--callbacks] [--renderer software|d3d11]\n"
           "                  [--render-resolution original|720p|1080p|1440p|4k|native|WxH]\n"
           "                  [--aspect-ratio 4:3|16:9|16:10|21:9|32:9]\n"
           "                  [--texture-filter original|bilinear|anisotropic4|anisotropic8|anisotropic16]\n"
           "                  [--msaa 0|2|4|8] [--vsync on|off] [--display-mode windowed|borderless] [--monitor N]\n"
           "                  [--window-size auto|WxH] [--presentation letterbox|stretch] [--show-fps on|off]\n"
           "                  [--skip-intro on|off] [--ambient-occlusion off|low|medium|high|ultra]\n"
           "                  [--original-engine-limits] [--debug-d3d-output MODE] [--ddraw-cert-dialog]\n"
           "                  [--legacy-safemode-dialog] [--software-water] [--lifted-water] [--perf-log]\n"
           "                  [-- guest arguments]\n"
           "  settings come from <user_data>\\launcher\\trespasser.ini (the launcher writes it); these options override them\n"
           "    for this run only\n"
           "  --launcher shows the launcher even when it is turned off; --skip-launcher skips it once (unless\n"
           "    the game files are not ready); --run is still accepted and changes nothing\n"
           "  --user_data_root keeps saves and settings in DIR instead of user_data next to the\n"
           "    program\n"
           "  --dry-run maps, fingerprints and binds the image, and nothing executes\n"
           "  --log-file appends to FILE; otherwise a new trespasser_NNN.log is created in the log directory\n"
           "  --ddraw system calls the system ddraw.dll instead of the compatibility layer (diagnostics only)\n"
           "  --texture-filter also takes point and trilinear (diagnostics); anisotropic means anisotropic16\n"
           "  --original-engine-limits keeps the 1998 draw distance, texture sizes and texture memory with d3d11\n"
           "    (comparison)\n"
           "  --debug-d3d-output texture|vertex-color|depth: d3d11 draws show only that (diagnostics)\n"
           "  --software-water keeps the game's software water with d3d11 (Hardware Water unset)\n"
           "  --lifted-water runs the lifted water and occlusion functions instead of their host kernels\n"
           "    (comparison)\n"
           "  --legacy-safemode-dialog keeps the game's \"unstable last time, Safe Mode?\" prompt (off by default)\n"
           "  --perf-log adds performance lines every 5 s, renderer summaries every 60 s and a sampled guest CPU\n"
           "    profile to the log (diagnostics)\n");
    recomp_trace_help();
}

static int add_override(options_t* o, const char* key, const char* value) {
    if (o->override_count == MAX_OVERRIDES || !jpt_config_valid(key, value)) return 0;
    o->overrides[o->override_count].key = key;
    o->overrides[o->override_count].value = value;
    o->override_count++;
    return 1;
}

static int texture_filter_option(options_t* o, const char* name) {
    if (!strcmp(name, "point")) return o->dev_filter = D3DR_FILTER_POINT, 1;
    if (!strcmp(name, "trilinear")) return o->dev_filter = D3DR_FILTER_TRILINEAR, 1;
    if (!strcmp(name, "anisotropic")) name = "anisotropic16";
    o->dev_filter = -1;
    return add_override(o, JPT_KEY_TEXTURE_FILTER, name);
}

static int parse_options(int argc, char** argv, options_t* o) {
    static const struct {
        const char* option;
        const char* key;
    } settings[] = {
        { "--renderer", JPT_KEY_RENDERER },         { "--aspect-ratio", JPT_KEY_ASPECT_RATIO },
        { "--msaa", JPT_KEY_MSAA },                 { "--vsync", JPT_KEY_VSYNC },
        { "--display-mode", JPT_KEY_DISPLAY_MODE }, { "--monitor", JPT_KEY_MONITOR },
        { "--window-size", JPT_KEY_WINDOW_SIZE },   { "--presentation", JPT_KEY_PRESENTATION },
        { "--show-fps", JPT_KEY_SHOW_FPS },         { "--skip-intro", JPT_KEY_SKIP_INTRO },
        { "--ambient-occlusion", JPT_KEY_AMBIENT_OCCLUSION },
    };
    o->game_dir = getenv("TRESPASSER_GAME_DIR");
    if (!o->game_dir) o->game_dir = TRESPASSER_DEFAULT_GAME_DIR;
    o->log_dir = TRESPASSER_LOG_DIR;
    o->dev_filter = -1;
    for (int i = 1; i < argc; i++) {
        int taken = recomp_trace_arg(argc, argv, i), matched = 0;
        if (taken) {
            i += taken - 1;
            continue;
        }
        for (size_t k = 0; k < sizeof settings / sizeof settings[0] && !matched; k++) {
            if (strcmp(argv[i], settings[k].option) || i + 1 >= argc) continue;
            if (!add_override(o, settings[k].key, argv[++i])) return 0;
            matched = 1;
        }
        if (matched) continue;
        if (!strcmp(argv[i], "--run")) {
            continue;
        } else if (!strcmp(argv[i], "--dry-run")) {
            o->dry_run = 1;
        } else if (!strcmp(argv[i], "--launcher")) {
            o->force_launcher = 1;
        } else if (!strcmp(argv[i], "--skip-launcher")) {
            o->skip_launcher = 1;
        } else if (!strcmp(argv[i], "--game") && i + 1 < argc) {
            o->game_dir = argv[++i];
        } else if (!strcmp(argv[i], "--user_data_root") && i + 1 < argc) {
            o->user_data_root = argv[++i];
        } else if (!strncmp(argv[i], "--user_data_root=", 17)) {
            o->user_data_root = argv[i] + 17;
        } else if (!strcmp(argv[i], "--log-dir") && i + 1 < argc) {
            o->log_dir = argv[++i];
        } else if (!strcmp(argv[i], "--log-file") && i + 1 < argc) {
            o->log_file = argv[++i];
        } else if (!strcmp(argv[i], "--ddraw") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "system")) o->system_ddraw = 1;
            else if (strcmp(mode, "compat")) return 0;
        } else if (!strcmp(argv[i], "--render-resolution") && i + 1 < argc) {
            const char* value = argv[++i];
            if (!add_override(o, JPT_KEY_RENDER_RESOLUTION, strcmp(value, "auto") ? value : "original")) return 0;
        } else if (!strcmp(argv[i], "--texture-filter") && i + 1 < argc) {
            if (!texture_filter_option(o, argv[++i])) return 0;
        } else if (!strcmp(argv[i], "--ddraw-cert-dialog")) {
            registry_show_ddraw_cert_dialog();
        } else if (!strcmp(argv[i], "--software-water")) {
            registry_software_water();
        } else if (!strcmp(argv[i], "--legacy-safemode-dialog")) {
            registry_legacy_safemode_dialog();
        } else if (!strcmp(argv[i], "--original-engine-limits")) {
            engine_limits_keep_original();
        } else if (!strcmp(argv[i], "--debug-d3d-output") && i + 1 < argc) {
            if (!d3dr_set_debug_output(argv[++i])) return 0;
        } else if (!strcmp(argv[i], "--lifted-water")) {
            o->lifted_water = 1;
        } else if (!strcmp(argv[i], "--perf-log")) {
            host_perf_logging_enable();
        } else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) {
            o->watchdog_seconds = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--native-trace")) {
            native32_trace_native = 1;
        } else if (!strcmp(argv[i], "--callbacks")) {
            native32_trace_callbacks = 1;
        } else if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) {
                if (strlen(o->guest_args) + strlen(argv[i]) + 2 > sizeof o->guest_args) return 0;
                if (o->guest_args[0]) strcat(o->guest_args, " ");
                strcat(o->guest_args, argv[i]);
            }
        } else {
            return 0;
        }
    }
    return 1;
}

static int verify_fingerprint(const char* exe) {
    char actual[65];
    if (!host_sha256_file(exe, actual)) {
        host_log("target", "cannot read %s", exe);
        return 0;
    }
    host_log("target", "%s", exe);
    host_log("target", "sha256 %s", actual);
    if (strcmp(actual, trespasser_expected_sha256)) {
        host_log("target", "MISMATCH: the lifted code was generated from %s", trespasser_expected_sha256);
        return 0;
    }
    host_log("target", "matches the binary the lifted code was generated from");
    return 1;
}

static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    native32_call_guest(trespasser_entry_va, 0, NULL);
    host_log("guest", "entry point returned, eax=%08X", g_eax);
    return (DWORD)host_exit_code_for_guest_return((int)g_eax);
}

static int run_guest(void) {
    DWORD code = 0;
    HANDLE thread = CreateThread(NULL, GUEST_THREAD_STACK, guest_main, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        host_log("guest", "cannot create the guest thread (error %lu)", GetLastError());
        return HOST_EXIT_SETUP;
    }
    guest_profile_start(thread);
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return (int)code;
}

static void log_line(const char* line) { host_log("launcher", "%s", line); }

/* A process that could not reserve the guest's address range before anything loaded (no game installed yet) starts
 * again, once, as a game-only process continuing this log, and ends with its exit code. */
static int restart_for_game(void) {
    static char command[4 * MAX_PATH + 64];
    char me[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = HOST_EXIT_SETUP;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    GetModuleFileNameA(NULL, me, MAX_PATH);
    _snprintf(command, sizeof command - 1, "%s --skip-launcher --log-file \"%s\"", GetCommandLineA(), host_log_path());
    command[sizeof command - 1] = 0;
    host_log("host", "the game files were installed by this process: starting the game as a new process");
    SetEnvironmentVariableA(RESTARTED_ENV, "1");
    if (!CreateProcessA(me, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        host_log("host", "cannot start the game process (error %lu)", GetLastError());
        return HOST_EXIT_SETUP;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void log_settings(const jpt_config_t* c, jpt_size_t box, int monitor_w, int monitor_h) {
    const jpt_size_t hardware = jpt_gameplay_mode(box, c->aspect, 1), software = jpt_gameplay_mode(box, c->aspect, 0);
    const jpt_size_t target = jpt_render_target(box, hardware.width, hardware.height);
    const jpt_size_t menus = jpt_render_target(box, 640, 480);
    const char* preset = c->render_preset >= 0 ? jpt_render_presets[c->render_preset].label : "custom";
    char save[16], load[16];
    host_log("settings", "renderer %s, texture filter %s, MSAA %ux, VSync %s, Ambient Occlusion %s",
             c->hardware ? "d3d11" : "software", jpt_filter_keys[c->texture_filter], c->msaa, c->vsync ? "on" : "off",
             jpt_ao_names[c->ambient_occlusion]);
    if (box.width > 0) {
        host_log("settings", "render resolution %s, aspect ratio %s: gameplay resolution %dx%d (the game's own screen "
                 "with D3D11, %.0f MB of host buffers), 3D render target %dx%d", preset, jpt_aspects[c->aspect].key,
                 hardware.width, hardware.height, jpt_gameplay_host_bytes(hardware) / 1048576.0, target.width,
                 target.height);
        if (hardware.width != box.width || hardware.height != box.height)
            host_log("settings", "the %dx%d box is above the gameplay budget of %d pixels: the game plays at %dx%d and "
                     "D3D11 rasterises at %dx%d", box.width, box.height, JPT_GAMEPLAY_MAX_PIXELS, hardware.width,
                     hardware.height, target.width, target.height);
    } else {
        host_log("settings", "render resolution Original, aspect ratio %s: gameplay resolution %dx%d, 3D render target "
                 "chosen from the window", jpt_aspects[c->aspect].key, hardware.width, hardware.height);
    }
    host_log("settings", "software renderer gameplay resolution %dx%d (its own limit: the CPU rasterises every pixel)",
             software.width, software.height);
    if (box.width > 0)
        host_log("settings", "UI virtual canvas 640x480 for menus and videos (source 640x480), shown 4:3 at %dx%d; "
                 "presentation %dx%d", menus.width, menus.height, jpt_title_size(box, hardware, 1).width,
                 jpt_title_size(box, hardware, 1).height);
    host_log("settings", "display %s on monitor %d (%dx%d), window %s, presentation %s, FPS in title %s",
             c->borderless ? "borderless" : "windowed", c->monitor, monitor_w, monitor_h,
             c->window_w ? "fixed" : "auto", c->stretch ? "stretch" : "letterbox", c->show_fps ? "on" : "off");
    host_log("settings", "Skip Intro %s%s", c->skip_intro ? "on" : "off",
             c->skip_intro ? ": the opening video before the main menu and the one after New Game are not shown" : "");
    host_log("settings", "Trespasser CE: Dual stow %s, Maximum object detail %s; Quick Save %s, Quick Load %s",
             c->dual_stow ? "on" : "off", c->max_object_detail ? "on" : "off",
             jpt_key_label(c->hotkey_vk[JPT_HOTKEY_QUICK_SAVE], save, sizeof save),
             jpt_key_label(c->hotkey_vk[JPT_HOTKEY_QUICK_LOAD], load, sizeof load));
}

static const char* filter_label(const jpt_config_t* c) {
    static const char* const labels[JPT_FILTER_COUNT] = { "original", "bilinear", "anisotropic 4x", "anisotropic 8x",
                                                         "anisotropic 16x" };
    return labels[c->texture_filter];
}

static void renderer_filter(const jpt_config_t* c, int dev_filter, int* filter, unsigned* anisotropy) {
    static const unsigned levels[JPT_FILTER_COUNT] = { 0, 0, 4, 8, 16 };
    *anisotropy = levels[c->texture_filter];
    if (dev_filter >= 0) *filter = dev_filter;
    else if (c->texture_filter == JPT_FILTER_ORIGINAL) *filter = D3DR_FILTER_ORIGINAL;
    else if (c->texture_filter == JPT_FILTER_BILINEAR) *filter = D3DR_FILTER_BILINEAR;
    else *filter = D3DR_FILTER_ANISOTROPIC;
}

int main(int argc, char** argv) {
    options_t opt = { 0 };
    char game_dir[MAX_PATH], log_dir[MAX_PATH], log_file[MAX_PATH];
    const char* settings_file;
    jpt_user_data_t user;
    jpt_config_t cfg;
    jpt_size_t box = { 0, 0 };
    int monitor_w = 0, monitor_h = 0, filter, held;
    unsigned anisotropy;
    uint32_t span;

    if (!parse_options(argc, argv, &opt)) {
        usage();
        return HOST_EXIT_SETUP;
    }
    if (!GetFullPathNameA(opt.game_dir, MAX_PATH, game_dir, NULL) ||
        !GetFullPathNameA(opt.log_dir, MAX_PATH, log_dir, NULL) ||
        (opt.log_file && !GetFullPathNameA(opt.log_file, MAX_PATH, log_file, NULL))) {
        fprintf(stderr, "invalid --game, --log-dir or --log-file path\n");
        return HOST_EXIT_SETUP;
    }
    guest_env_init(game_dir, opt.guest_args);

    /* Restarts this process with the guest's address range reserved before anything else
     * can land there; only the restarted copy continues past this call. */
    held = native32_hold_image(guest_env_exe_path(), trespasser_image_base);

    if (!host_log_open(log_dir, opt.log_file ? log_file : NULL)) {
        fprintf(stderr, "cannot open the log %s (error %lu)\n", opt.log_file ? log_file : log_dir, GetLastError());
        return HOST_EXIT_SETUP;
    }
    host_log("host", "Trespasser recompilation host, %u lifted functions in the dispatch table",
             recomp_dispatch_count);
    host_log("host", "log file %s", host_log_path());
    host_log("host", "game directory %s", game_dir);
    host_log("host", "guest address range %s", held ? "reserved at process start" : "NOT reserved in advance");
    if (!jpt_user_data_select(opt.user_data_root, TRESPASSER_LEGACY_USER_DIR, &user)) {
        host_log("host", "the user data folder's path is too long for the game (%u characters at most)",
                 JPT_USER_PATH_CAP - 1);
        return HOST_EXIT_SETUP;
    }
    host_log("host", "%s%s", user.warning ? "WARNING: " : "", user.message);
    if (!user.writable) {
        fprintf(stderr, "%s\n", user.message);
        MessageBoxA(NULL, user.message, "Jurassic Park: Trespasser Recompiled", MB_OK | MB_ICONERROR);
        return HOST_EXIT_SETUP;
    }
    host_log("host", "user data %s: settings %s, saves %s", user.root, user.settings_file, user.saves_dir);
    settings_file = user.settings_file;

    window_host_init();
    if (!jpt_make_dirs(user.saves_dir)) host_log("host", "cannot create the saves folder %s", user.saves_dir);
    jpt_carry_user_files(game_dir, user.saves_dir, log_line);
    switch (jpt_config_migrate(settings_file)) {
    case JPT_MIGRATED_EXPERIMENTAL:
        host_log("settings", "%s came from the experimental launcher: its display and graphics choices were reset "
                 "once to the current defaults", settings_file);
        break;
    case JPT_MIGRATED_GRAPHICS_DEFAULTS:
        host_log("settings", "%s held the earlier launcher's graphics defaults (texture filter original, MSAA off, "
                 "VSync off): those were reset once to the current defaults, other choices kept", settings_file);
        break;
    default: break;
    }
    if (jpt_config_migrate_language(settings_file))
        host_log("settings", "%s: the earlier game text and subtitle language settings became language", settings_file);

    if (!opt.dry_run) {
        jpt_launcher_config_t launcher;
        int had_game = GetFileAttributesA(guest_env_exe_path()) != INVALID_FILE_ATTRIBUTES;
        char restarted[4];
        int may_restart = !GetEnvironmentVariableA(RESTARTED_ENV, restarted, sizeof restarted);
        SetEnvironmentVariableA(RESTARTED_ENV, NULL);
        memset(&launcher, 0, sizeof launcher);
        launcher.game_dir = game_dir;
        launcher.user_dir = user.root;
        launcher.saves_dir = user.saves_dir;
        launcher.user_data_outcome = user.outcome;
        launcher.logs_dir = log_dir;
        launcher.settings_file = settings_file;
        launcher.force_show = opt.force_launcher && !opt.skip_launcher;
        launcher.skip_once = opt.skip_launcher;
        launcher.log = log_line;
        if (jpt_launcher_run(&launcher) == JPT_LAUNCHER_QUIT) {
            host_log("host", "the launcher was closed without starting the game");
            return 0;
        }
        if (!held && !had_game && may_restart && GetFileAttributesA(guest_env_exe_path()) != INVALID_FILE_ATTRIBUTES)
            return restart_for_game();
    }

    jpt_config_load(settings_file, &cfg);
    for (int i = 0; i < opt.override_count; i++) {
        jpt_config_apply(&cfg, opt.overrides[i].key, opt.overrides[i].value);
        host_log("settings", "%s = %s (command line)", opt.overrides[i].key, opt.overrides[i].value);
    }
    window_host_monitor_size(cfg.monitor, &monitor_w, &monitor_h);
    if (cfg.render_preset < 0) {
        box.width = cfg.render_w;
        box.height = cfg.render_h;
    } else {
        box = jpt_render_box(cfg.render_preset, cfg.aspect, monitor_w, monitor_h);
    }
    log_settings(&cfg, box, monitor_w, monitor_h);
    stream_audio_init();
    l10n_init(user.cache_dir, game_dir, cfg.language);
    gamepad_configure(&cfg.gamepad);
    key_policy_set_quick_keys(cfg.hotkey_vk[JPT_HOTKEY_QUICK_SAVE], cfg.hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
    ce_gameplay_configure(cfg.dual_stow, cfg.max_object_detail);
    active_ai_configure(cfg.active_ai_enhanced);
    if (!verify_fingerprint(guest_env_exe_path()) || !guest_overrides_enable()) return HOST_EXIT_SETUP;

    window_host_configure(cfg.borderless, cfg.monitor, cfg.window_w, cfg.window_h, cfg.stretch, cfg.show_fps,
                          cfg.show_resolution_title,
                          settings_file);
    present_set_vsync(cfg.vsync);
    present_set_graphics_card(cfg.graphics_card);
    native32_init();
    /* Lifted .text always resolves to the same function (an override switches inside its stable entry), so call sites
     * keep it. */
    g_recomp_site_lo = trespasser_text_start;
    g_recomp_site_hi = trespasser_text_end;
    diagnostics_install(opt.dry_run ? 0 : opt.watchdog_seconds);

    if (!SetCurrentDirectoryA(game_dir) || !SetDllDirectoryA(game_dir)) {
        host_log("host", "cannot enter %s (error %lu)", game_dir, GetLastError());
        return HOST_EXIT_SETUP;
    }
    span = native32_map(guest_env_exe_path(), trespasser_image_base);
    if (!span) {
        host_log("loader", "cannot map the image at 0x%08X", trespasser_image_base);
        return HOST_EXIT_SETUP;
    }
    host_log("loader", "image mapped at 0x%08X-0x%08X; no relocations exist, so the base is exact",
             trespasser_image_base, trespasser_image_base + span);
    host_log("loader", ".text 0x%08X-0x%08X is data to the CPU: it runs as lifted C",
             trespasser_text_start, trespasser_text_end);
    if (!selfmod_init()) return HOST_EXIT_SETUP;

    guest_registry_init(game_dir, user.saves_dir);
    io_trace_user_dir(user.saves_dir);
    if (native32_bind(trespasser_image_base, s_shims, (int)(sizeof s_shims / sizeof s_shims[0]))) {
        host_log("imports", "unresolved imports (listed above as [bind] lines); refusing to run");
        return HOST_EXIT_SETUP;
    }
    host_log("imports", "import table bound: see the [bind] line above for native/shimmed counts");
    if (!guest_env_apply_command_line() || !io_trace_init()) {
        host_log("host", "msvcrt.dll lacks _acmdln, fopen or _open; cannot set up the guest CRT");
        return HOST_EXIT_SETUP;
    }
    milestones_init();
    ddraw_trace_init();
    if (!ddraw_compat_init(!opt.system_ddraw)) return HOST_EXIT_SETUP;
    renderer_filter(&cfg, opt.dev_filter, &filter, &anisotropy);
    d3dr_configure(cfg.hardware && !opt.system_ddraw, (unsigned)box.width, (unsigned)box.height, filter, anisotropy,
                   cfg.msaa);
    d3dr_configure_ambient_occlusion(cfg.ambient_occlusion, settings_file);
    d3dr_configure_effects(cfg.water_refraction, cfg.enhanced_sky);
    if (!d3dr_startup()) return HOST_EXIT_SETUP;

    if (opt.dry_run) {
        host_log("host", "dry run complete: nothing was executed; without --dry-run the game enters 0x%08X",
                 trespasser_entry_va);
        return 0;
    }
    if (!opt.lifted_water) guest_kernels_install();
    {
        jpt_size_t hardware = jpt_gameplay_mode(box, cfg.aspect, 1), software = jpt_gameplay_mode(box, cfg.aspect, 0);
        jpt_size_t hardware_title = jpt_title_size(box, hardware, 1);
        if (!widescreen_install(cfg.aspect)) hardware = software = jpt_gameplay_mode(box, 0, 0);
        registry_gameplay_sizes(hardware.width, hardware.height, software.width, software.height);
        dd_set_gameplay_modes((DWORD)hardware.width, (DWORD)hardware.height, (DWORD)software.width,
                              (DWORD)software.height);
        window_host_set_title_sizes(hardware_title.width, hardware_title.height, software.width, software.height);
        terrain_quality_set_render_height(hardware.height);
    }
    game_text_install();
    ui_canvas_install();
    loading_hd_install();
    video_hd_install();
    video_hd_skip_intro(cfg.skip_intro);
    physics_fixes_install();
    ce_gameplay_install();
    active_ai_install();
    host_log("ce", "active AI: %s", active_ai_installed() ? "enhanced (6)" : "original (3)");
    ce_graphics_install(d3dr_hardware());
    engine_limits_install(d3dr_hardware());
    texture_streaming_install();
    engine_heaps_install();
    terrain_quality_install();
    cpu_polygons_install();
    gamepad_install();
    l10n_install();
    if (!guest_overrides_check()) return HOST_EXIT_SETUP;
    engine_limits_log(d3dr_hardware(), filter_label(&cfg), cfg.msaa, cfg.vsync);
    host_log("guest", "entering the CRT entry point 0x%08X", trespasser_entry_va);
    return run_guest();
}
