/*
 * Trespasser recompilation host: a 32-bit process on pcrecomp's native32 runtime.
 * The lifted .text runs as C; imports go to the real Win32 DLLs; SelfMod runs natively.
 * Structure follows sp00nznet/themovies host.c (MIT).
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_host.h"
#include "audio/platform/windows_dsound_bridge.h"
#include "audio/platform/windows_dsound_imports.h"
#include "audio/sdl_audio.h"
#include "cheats.h"
#include "compass_policy.h"
#include "ddraw/d3d_legacy.h"
#include "ddraw/renderer_backend.h"
#include "gamepad.h"
#include "jpt_config.h"
#include "jpt_custom_level.h"
#include "debug_menu.h"
#include "jpt_display.h"
#include "vk_context.h"
#include "vk_shaders.h"
#include "jpt_launcher.h"
#include "jpt_level_scripts.h"
#include "jpt_renderer.h"
#include "jpt_user_data.h"
#include "l10n.h"
#include "launch_handoff.h"
#include "level_session.h"
#include "loading_hd.h"
#include "platform.h"
#include "stream_audio.h"
#include "recomp_trace.h"

/* The flat user folder of earlier development builds, migrated into the user data root. */
#ifndef TRESPASSER_LEGACY_USER_DIR
#define TRESPASSER_LEGACY_USER_DIR NULL
#endif

/* Lifted frames are several times the original's, and they live on the host stack. */
#define GUEST_THREAD_STACK (64u << 20)
#define MAX_OVERRIDES 16

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
    int self_check;
    const char* analyze;
    const char *analyze_all, *analyze_report;
    int analyze_json;
    int force_launcher, skip_launcher;
    const char* launcher_renderer;
    int system_ddraw;
    int dev_filter;
    int lifted_water;
    const char* debug_d3d_output;
    const char* env_renderer;
    const char* cli_renderer;
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
    { "GetDiskFreeSpaceA", shim_GetDiskFreeSpaceA },
    { "FindFirstFileA", shim_FindFirstFileA },
    { "FindNextFileA", shim_FindNextFileA },
    { "LZOpenFileA", shim_LZOpenFileA },
    { "LZRead", shim_LZRead },
    { "LZSeek", shim_LZSeek },
    { "LZClose", shim_LZClose },
    { "LoadLibraryA", shim_LoadLibraryA },
    { "GetProcAddress", shim_GetProcAddress },
    { "FreeLibrary", shim_FreeLibrary },
    { "fopen", shim_fopen },
    { "_open", shim_open },
    { "_close", shim_close },
    { "DeleteFileA", shim_DeleteFileA },
    { "_mkdir", shim_mkdir },
    { "remove", shim_remove },
    { "rename", shim_rename },
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
    { "VirtualAlloc", shim_VirtualAlloc },
    { "??2@YAPAXI@Z", shim_operator_new },
    { "malloc", shim_malloc },
    { "?set_new_handler@@YAP6AXXZP6AXXZ@Z", shim_set_new_handler },
    { "?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", shim__set_new_handler },
    { "?_set_new_mode@@YAHH@Z", shim__set_new_mode },
    { "GetTickCount", shim_GetTickCount },
    { "TextOutA", shim_TextOutA },
    { "LoadStringA", shim_LoadStringA },
    { "CreateFontA", shim_CreateFontA },
    { "CreateFontIndirectA", shim_CreateFontIndirectA },
    { "exit", shim_exit },
    { "_exit", shim__exit },
    { "ExitProcess", shim_ExitProcess },
};


/* A folder next to the program, whatever the working directory; the bare name when the program's path is unusable. */
static const char* beside_program(const char* name, char* out, size_t cap) {
    char* slash;
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (!n || n >= cap || !(slash = strrchr(out, '\\')) || (size_t)(slash + 1 - out) + strlen(name) >= cap) return name;
    strcpy(slash + 1, name);
    return out;
}

static const char* default_game_dir(void) {
#ifdef TRESPASSER_DEFAULT_GAME_DIR
    return TRESPASSER_DEFAULT_GAME_DIR;
#else
    static char dir[MAX_PATH];
    return beside_program("game", dir, sizeof dir);
#endif
}

static const char* default_log_dir(void) {
#ifdef TRESPASSER_LOG_DIR
    return TRESPASSER_LOG_DIR;
#else
    static char dir[MAX_PATH];
    return beside_program("logs", dir, sizeof dir);
#endif
}

static void usage(void) {
    printf("usage: trespasser_recomp [--launcher | --skip-launcher] [--launcher-renderer auto|software|direct3d11]\n"
           "                  [--dry-run] [--game DIR] [--user_data_root DIR]\n"
           "                  [--log-file FILE | --log-dir DIR] [--watchdog SECONDS] [--ddraw compat|system]\n"
           "                  [--native-trace] [--callbacks] [--renderer auto|d3d11|vulkan|software]\n"
           "                  [--render-resolution original|720p|1080p|1440p|4k|native|WxH]\n"
           "                  [--aspect-ratio 4:3|16:9|16:10|21:9|32:9]\n"
           "                  [--texture-filter original|bilinear|anisotropic4|anisotropic8|anisotropic16]\n"
           "                  [--msaa 0|2|4|8] [--vsync on|off] [--display-mode windowed|borderless] [--monitor N]\n"
           "                  [--window-size auto|WxH] [--presentation letterbox|stretch] [--show-fps on|off]\n"
           "                  [--skip-intro on|off] [--ambient-occlusion off|low|medium|high|ultra]\n"
           "                  [--original-engine-limits] [--debug-d3d-output MODE] [--ddraw-cert-dialog]\n"
           "                  [--legacy-safemode-dialog] [--software-water] [--lifted-water] [--perf-log]\n"
           "                  [--analyze-custom-level PACKAGE [--json]]\n"
           "                  [--analyze-custom-levels FOLDER [--report FOLDER] [--json]] [-- guest arguments]\n"
           "  settings come from <user_data>\\launcher\\trespasser.ini (the launcher writes it); these options override them\n"
           "    for this run only\n"
           "  --launcher shows the launcher even when it is turned off; --skip-launcher skips it once (unless\n"
           "    the game files are not ready); --run is still accepted and changes nothing\n"
           "  --launcher-renderer chooses what draws the launcher's own window, for this run only; software needs no\n"
           "    graphics driver: --launcher --launcher-renderer software. The game's renderer does not change\n"
           "  --user_data_root keeps saves and settings in DIR instead of user_data next to the\n"
           "    program\n"
           "  --dry-run maps, fingerprints and binds the image, and nothing executes\n"
           "  --log-file appends to FILE; otherwise a new trespasser_recomp_NNN.log is created in the log directory\n"
           "  --ddraw system calls the system ddraw.dll instead of the compatibility layer (diagnostics only)\n"
           "  --texture-filter also takes point and trilinear (diagnostics); anisotropic means anisotropic16\n"
           "  --original-engine-limits keeps the 1998 draw distance, texture sizes and texture memory with d3d11\n"
           "    (comparison)\n"
           "  --debug-d3d-output texture|vertex-color|depth: d3d11 draws show only that (diagnostics)\n"
           "  --software-water keeps the game's software water with d3d11 (Hardware Water unset)\n"
           "  --lifted-water runs the lifted water and occlusion functions instead of their host kernels\n"
           "    (comparison)\n"
           "  --legacy-safemode-dialog keeps the game's \"unstable last time, Safe Mode?\" prompt (off by default)\n"
           "  --analyze-custom-level prints what the Custom Levels import makes of a ZIP, 7z, RAR or folder; nothing is installed\n"
           "  --analyze-custom-levels does so for every package below FOLDER, with totals and the most frequent problems;\n"
           "    --report writes it to a folder as well, as text and JSON\n"
           "  --perf-log adds performance lines every 5 s, renderer summaries every 60 s, the exit and slow-frame\n"
           "    breakdowns and a sampled guest CPU profile to the log (diagnostics)\n");
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
        { "--video-subtitles", JPT_KEY_VIDEO_SUBTITLES },
        { "--ambient-occlusion", JPT_KEY_AMBIENT_OCCLUSION },
    };
    o->game_dir = getenv("TRESPASSER_GAME_DIR");
    if (!o->game_dir) o->game_dir = default_game_dir();
    o->log_dir = default_log_dir();
    o->dev_filter = -1;
    {
        const char* env_r = getenv("TRESPASSER_RENDERER");
        if (env_r && env_r[0]) o->env_renderer = env_r;
    }
    for (int i = 1; i < argc; i++) {
        int taken = recomp_trace_arg(argc, argv, i), matched = 0;
        if (taken) {
            i += taken - 1;
            continue;
        }
        for (size_t k = 0; k < sizeof settings / sizeof settings[0] && !matched; k++) {
            if (strcmp(argv[i], settings[k].option) || i + 1 >= argc) continue;
            if (!strcmp(settings[k].key, JPT_KEY_RENDERER)) {
                const char* val = argv[++i];
                if (!jpt_config_valid(JPT_KEY_RENDERER, val)) return 0;
                o->cli_renderer = val;
            } else if (!add_override(o, settings[k].key, argv[++i])) {
                return 0;
            }
            matched = 1;
        }
        if (matched) continue;
        if (!strcmp(argv[i], "--run")) {
            continue;
        } else if (!strcmp(argv[i], "--dry-run")) {
            o->dry_run = 1;
        } else if (!strcmp(argv[i], "--self-check")) {
            o->self_check = 1;
        } else if (!strcmp(argv[i], "--analyze-custom-level") && i + 1 < argc) {
            o->analyze = argv[++i];
        } else if (!strcmp(argv[i], "--analyze-custom-levels") && i + 1 < argc) {
            o->analyze_all = argv[++i];
        } else if (!strcmp(argv[i], "--report") && i + 1 < argc) {
            o->analyze_report = argv[++i];
        } else if (!strcmp(argv[i], "--json")) {
            o->analyze_json = 1;
        } else if (!strcmp(argv[i], "--launcher")) {
            o->force_launcher = 1;
        } else if (!strcmp(argv[i], "--skip-launcher")) {
            o->skip_launcher = 1;
        } else if (!strcmp(argv[i], "--launcher-renderer") && i + 1 < argc) {
            o->launcher_renderer = argv[++i];
            if (!jpt_launcher_renderer_valid(o->launcher_renderer, 1)) return 0;
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
            o->debug_d3d_output = argv[++i];
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

/* SDL starts here: the game creates and pumps its window on this thread, which makes it SDL's video thread. */
static DWORD WINAPI guest_main(LPVOID unused) {
    (void)unused;
    if (!platform_start()) {
        MessageBoxA(NULL, "SDL could not start its video subsystem; the log names the reason.",
                    "Jurassic Park: Trespasser Recompiled", MB_OK | MB_ICONERROR);
        return HOST_EXIT_SETUP;
    }
    native32_call_guest(lift_entry_va, 0, NULL);
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
    if (host_perf_logging()) guest_profile_start(thread);
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return (int)code;
}

static void log_line(const char* line) { host_log("launcher", "%s", line); }

/* The version's lifted build becomes the one this process runs, before any guest code does; a lift that is not the one
 * this program was built to carry is refused here. */
static int activate_profile(const jpt_game_version_t* version) {
    char error[512];
    if (!lift_activate(version, error, sizeof error)) {
        host_log("game-version", "%s", error);
        fprintf(stderr, "%s\n", error);
        return 0;
    }
    host_log("game-version", "%s runs the lifted build %s: %u functions, generated from %s", version->id, version->lift,
             (unsigned)lift_active_image()->meta->function_count, lift_active_image()->meta->source_profile);
    return 1;
}

/* The version the guest runs: the setting's one when the port can run it, else the first installed one it can.
 * A version other than English 1.0.116 keeps its own Installed Directory. */
static const jpt_game_version_t* select_game_version(const char* game_root, const char* wanted, char* game_dir,
                                                     jpt_user_data_t* user) {
    const jpt_game_version_t* version = NULL;
    char saves[JPT_USER_PATH_CAP];
    if (!jpt_game_version_resolve(game_root, wanted, 1, game_dir, MAX_PATH, &version)) {
        const jpt_game_version_t* chosen = jpt_game_version_by_id(wanted);
        char dir[MAX_PATH];
        if (chosen && jpt_game_version_installed(game_root, chosen, dir, sizeof dir))
            host_log("game-version", "%s is installed but cannot run: %s", chosen->label, chosen->unplayable);
        else
            host_log("game-version", "no game version the port can run is installed in %s", game_root);
        return NULL;
    }
    host_log("game-version", "%s, build %d, %s, profile %s, from %s", version->label, version->build, version->language,
             version->id, game_dir);
    if (wanted[0] && _stricmp(wanted, version->id))
        host_log("game-version", "the selected version %s is not installed or cannot run: %s is used", wanted, version->id);
    if (!jpt_game_version_saves_dir(user->root, user->saves_dir, version, saves, sizeof saves)) {
        host_log("game-version", "the saves folder of %s does not fit in a path", version->id);
        return NULL;
    }
    if (strcmp(saves, user->saves_dir)) {
        strcpy(user->saves_dir, saves);
        if (!jpt_make_dirs(user->saves_dir)) host_log("host", "cannot create the saves folder %s", user->saves_dir);
    }
    host_log("game-version", "saves and the game's registry store in %s", user->saves_dir);
    return version;
}

/* The chosen package answers for the data folder and plays with its own Installed Directory: its saves name its files,
 * and level swap files are told apart only by name. The registry store stays the version's. */
static int start_custom_level(const jpt_custom_level_request_t* request, jpt_user_data_t* user, jpt_config_t* cfg) {
    char saves[JPT_USER_PATH_CAP], error[512];
    const char *key, *value;
    if (!request->scene[0]) return 1;
    if (!jpt_custom_level_activate(user->root, request, error, sizeof error)) {
        host_log("custom-level", "%s", error);
        MessageBoxA(NULL, error, "Jurassic Park: Trespasser Recompiled", MB_OK | MB_ICONERROR);
        return 0;
    }
    host_log("custom-level", "compatibility=%s", jpt_custom_level_compat_name());
    if (!jpt_custom_level_admit_start(error, sizeof error)) {
        host_log("custom-level", "%s", error);
        MessageBoxA(NULL, error, "Jurassic Park: Trespasser Recompiled", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (error[0]) host_log("custom-level", "%s", error);
    if (!jpt_custom_level_saves_dir(user->saves_dir, request->package, saves, sizeof saves) || !jpt_make_dirs(saves)) {
        host_log("custom-level", "cannot create the saves folder of %s", request->package);
        return 0;
    }
    strcpy(user->saves_dir, saves);
    cfg->skip_intro = 1;
    for (int i = 0; jpt_custom_level_setting(i, &key, &value); i++) {
        jpt_config_apply(cfg, key, value);
        host_log("custom-level", "%s = %s for this session (the saved setting stays)", key, value);
    }
    host_log("custom-level", "%s, scene %s loaded as %s; saves in %s", request->package, request->scene,
             jpt_custom_level_scene(), user->saves_dir);
    for (int cap = 0; cap < JPT_LEVEL_CAPS; cap++)
        if (jpt_custom_level_requires((jpt_level_cap_t)cap)) host_log("custom-level", "the package needs %s: on for this session only", jpt_custom_level_cap_name((jpt_level_cap_t)cap));
    return 1;
}

static void log_settings(const jpt_config_t* c, jpt_size_t box, int monitor_w, int monitor_h) {
    const jpt_size_t hardware = jpt_gameplay_mode(box, c->aspect, 1), software = jpt_gameplay_mode(box, c->aspect, 0);
    const jpt_size_t target = jpt_render_target(box, hardware.width, hardware.height);
    const jpt_size_t menus = jpt_render_target(box, 640, 480);
    const char* preset = c->render_preset >= 0 ? jpt_render_presets[c->render_preset].label : "custom";
    char save[16], load[16];
    host_log("settings", "renderer %s, texture filter %s, MSAA %ux, VSync %s, Ambient Occlusion %s",
             jpt_renderer_key((jpt_renderer_t)c->renderer), jpt_filter_keys[c->texture_filter], c->msaa,
             c->vsync ? "on" : "off", jpt_ao_names[c->ambient_occlusion]);
    if (box.width > 0) {
        const char* hw_name = (c->renderer == JPT_RENDERER_VULKAN)  ? "Vulkan"
                              : (c->renderer == JPT_RENDERER_D3D11) ? "D3D11"
                                                                    : "hardware renderer";
        host_log("settings", "render resolution %s, aspect ratio %s: gameplay resolution %dx%d (the game's own screen "
                 "with %s, %.0f MB of host buffers), 3D render target %dx%d", preset, jpt_aspects[c->aspect].key,
                 hardware.width, hardware.height, hw_name, jpt_gameplay_host_bytes(hardware) / 1048576.0, target.width,
                 target.height);
        if (hardware.width != box.width || hardware.height != box.height)
            host_log("settings", "the %dx%d box is above the gameplay budget of %d pixels: the game plays at %dx%d and "
                     "%s rasterises at %dx%d", box.width, box.height, JPT_GAMEPLAY_MAX_PIXELS, hardware.width,
                     hardware.height, hw_name, target.width, target.height);
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

static int analyze_custom_level(const options_t* o) {
    char dir[MAX_PATH], data[MAX_PATH + 8];
    const jpt_game_version_t* version = NULL;
    int installed = jpt_game_version_resolve(o->game_dir, "", 1, dir, sizeof dir, &version);
    if (installed) snprintf(data, sizeof data, "%s\\data", dir);
    if (o->analyze_all) return jpt_analyze_custom_levels(o->analyze_all, installed ? data : NULL, o->analyze_report, o->analyze_json);
    return jpt_analyze_custom_level(o->analyze, installed ? data : NULL, o->analyze_json);
}

static int host_main(int argc, char** argv) {
    options_t opt = { 0 };
    char game_root[MAX_PATH], game_dir[MAX_PATH], log_dir[MAX_PATH], log_file[MAX_PATH];
    const jpt_game_version_t* version;
    const char* settings_file;
    char store_dir[JPT_USER_PATH_CAP];
    jpt_custom_level_request_t custom_level = { "", "" };
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
    if (opt.analyze || opt.analyze_all) return analyze_custom_level(&opt);
    if (opt.self_check) {
        const jpt_win_backend_ops_t* vk_ops = renderer_backend_resolve_ops(JPT_RENDERER_VULKAN);
        const jpt_win_backend_ops_t* d3d_ops = renderer_backend_resolve_ops(JPT_RENDERER_D3D11);
        const jpt_win_backend_ops_t* sw_ops = renderer_backend_resolve_ops(JPT_RENDERER_SOFTWARE);
        int vk_dispatch = (vk_ops == &jpt_vulkan_backend_ops && vk_ops != d3d_ops && vk_ops->id == JPT_RENDERER_VULKAN);
        int ok = vk_dispatch && d3d_ops == &jpt_d3d11_backend_ops && sw_ops == &jpt_software_backend_ops &&
                 jpt_vk_shader_count() == JPT_VK_SH_COUNT;
        printf("{\n"
               "  \"platform\": \"windows\",\n"
               "  \"self_check\": %s,\n"
               "  \"vulkan\": {\n"
               "    \"compiled\": true,\n"
               "    \"selectable\": %s,\n"
               "    \"runtime_dispatch_wired\": %s,\n"
               "    \"spirv_shaders\": %d\n"
               "  },\n"
               "  \"backends\": {\n"
               "    \"d3d11\": %s,\n"
               "    \"vulkan\": %s,\n"
               "    \"software\": %s\n"
               "  }\n"
               "}\n",
               ok ? "true" : "false", jpt_renderer_available_on_platform(JPT_RENDERER_VULKAN, 1) ? "true" : "false",
               vk_dispatch ? "true" : "false", jpt_vk_shader_count(),
               d3d_ops == &jpt_d3d11_backend_ops ? "true" : "false", vk_dispatch ? "true" : "false",
               sw_ops == &jpt_software_backend_ops ? "true" : "false");
        return ok ? 0 : 1;
    }
    if (!GetFullPathNameA(opt.game_dir, MAX_PATH, game_root, NULL) ||
        !GetFullPathNameA(opt.log_dir, MAX_PATH, log_dir, NULL) ||
        (opt.log_file && !GetFullPathNameA(opt.log_file, MAX_PATH, log_file, NULL))) {
        fprintf(stderr, "invalid --game, --log-dir or --log-file path\n");
        return HOST_EXIT_SETUP;
    }
    /* Restarts this process with the guest's address range reserved before anything else can land there; only the
     * restarted copy continues past this call. The range fits the largest build the program carries, whatever is
     * installed: a version imported by the launcher in this run maps into it. */
    held = native32_hold_range(JPT_GUEST_IMAGE_BASE, jpt_guest_reservation_size());

    if (!host_log_open(log_dir, opt.log_file ? log_file : NULL)) {
        fprintf(stderr, "cannot open the log %s (error %lu)\n", opt.log_file ? log_file : log_dir, GetLastError());
        return HOST_EXIT_SETUP;
    }
    host_log("host", "Trespasser recompilation host, one program with %d lifted builds", JPT_LIFT_COUNT);
    host_log("host", "log file %s", host_log_path());
    host_log("host", "game folder %s", game_root);
    if (held)
        host_log("host", "guest address range reserved 0x%08X-0x%08X for all supported builds", JPT_GUEST_IMAGE_BASE,
                 JPT_GUEST_IMAGE_BASE + jpt_guest_reservation_size());
    else
        host_log("host", "guest address range NOT reserved in advance");
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
    jpt_carry_user_files(game_root, user.saves_dir, log_line);
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
        memset(&launcher, 0, sizeof launcher);
        launcher.game_dir = game_root;
        launcher.user_dir = user.root;
        launcher.saves_dir = user.saves_dir;
        launcher.user_data_outcome = user.outcome;
        launcher.logs_dir = log_dir;
        launcher.log_file = host_log_path();
        launcher.settings_file = settings_file;
        launcher.force_show = opt.force_launcher && !opt.skip_launcher;
        launcher.skip_once = opt.skip_launcher;
        launcher.launcher_renderer = opt.launcher_renderer;
        launcher.log = log_line;
        launcher.custom_level = &custom_level;
        launcher.handoff = launch_handoff_begin;
        switch (jpt_launcher_run(&launcher)) {
        case JPT_LAUNCHER_PLAY: break;
        case JPT_LAUNCHER_FAILED:
            host_log("host", "the launcher could not open: the game was not started");
            return HOST_EXIT_SETUP;
        default:
            host_log("host", "the launcher was closed without starting the game");
            return 0;
        }
        launch_handoff_launcher_closed();
    }

    jpt_config_load(settings_file, &cfg);
    {
        jpt_renderer_selection_t rsel;
        jpt_renderer_select_request(cfg.renderer_from_ini, (jpt_renderer_t)cfg.renderer, opt.env_renderer,
                                    opt.cli_renderer, &rsel);
        jpt_config_apply(&cfg, JPT_KEY_RENDERER, jpt_renderer_key(rsel.requested));
        host_log("settings",
                 "renderer selection: ini=%s, env(TRESPASSER_RENDERER)=%s%s, cli=%s -> requested=%s (source=%s)",
                 rsel.ini_present ? jpt_renderer_key(rsel.ini_value) : "unset",
                 rsel.env_present ? opt.env_renderer : "unset",
                 rsel.env_present ? (!rsel.env_valid ? " [invalid, ignored]"
                                                     : (rsel.env_ignored ? " [ignored]" : ""))
                                  : "",
                 rsel.cli_present ? opt.cli_renderer : "unset", jpt_renderer_key(rsel.requested),
                 jpt_renderer_source_name(rsel.source));
    }
    for (int i = 0; i < opt.override_count; i++) {
        jpt_config_apply(&cfg, opt.overrides[i].key, opt.overrides[i].value);
        host_log("settings", "%s = %s (command line)", opt.overrides[i].key, opt.overrides[i].value);
    }
    version = select_game_version(game_root, cfg.game_version, game_dir, &user);
    if (!version) return HOST_EXIT_SETUP;
    if (!activate_profile(version)) return HOST_EXIT_SETUP;
    jpt_config_select_language(&cfg, version);
    strcpy(store_dir, user.saves_dir);
    if (!start_custom_level(&custom_level, &user, &cfg)) return HOST_EXIT_SETUP;
    guest_env_init(game_dir, opt.guest_args);
    window_host_monitor_size(cfg.monitor, &monitor_w, &monitor_h);
    if (cfg.render_preset < 0) {
        box.width = cfg.render_w;
        box.height = cfg.render_h;
    } else {
        box = jpt_render_box(cfg.render_preset, cfg.aspect, monitor_w, monitor_h);
    }
    log_settings(&cfg, box, monitor_w, monitor_h);
    stream_audio_init();
    l10n_init(user.cache_dir, game_dir, game_dir, cfg.language, version);
    gamepad_configure(&cfg.gamepad);
    key_policy_set_quick_keys(cfg.hotkey_vk[JPT_HOTKEY_QUICK_SAVE], cfg.hotkey_vk[JPT_HOTKEY_QUICK_LOAD]);
    guest_features_configure(&cfg);
    cheats_start(cfg.cheat);
    if (!guest_overrides_enable()) return HOST_EXIT_SETUP;

    debug_menu_windows_init();
    window_host_set_frame_counter(cfg.show_frame_counter);
    window_host_configure(cfg.borderless, cfg.monitor, cfg.window_w, cfg.window_h, cfg.stretch, cfg.show_fps,
                          cfg.show_resolution_title,
                          settings_file);
    present_set_vsync(cfg.vsync);
    present_set_graphics_card(cfg.graphics_card);
    native32_init();
    /* Lifted .text always resolves to the same function (an override switches inside its stable entry), so call sites
     * keep it. */
    g_recomp_site_lo = lift_text_start;
    g_recomp_site_hi = lift_text_end;
    diagnostics_install(opt.dry_run ? 0 : opt.watchdog_seconds);

    if (!SetCurrentDirectoryA(game_dir) || !SetDllDirectoryA(game_dir)) {
        host_log("host", "cannot enter %s (error %lu)", game_dir, GetLastError());
        return HOST_EXIT_SETUP;
    }
    span = native32_map(guest_env_exe_path(), JPT_GUEST_IMAGE_BASE);
    if (!span) {
        host_log("loader", "cannot map the image at 0x%08X", JPT_GUEST_IMAGE_BASE);
        return HOST_EXIT_SETUP;
    }
    if (held && span > jpt_guest_reservation_size()) {
        host_log("loader", "the guest image spans 0x%X bytes, more than the 0x%X reserved at start", (unsigned)span,
                 (unsigned)jpt_guest_reservation_size());
        return HOST_EXIT_SETUP;
    }
    host_log("loader", "image mapped at 0x%08X-0x%08X; no relocations exist, so the base is exact",
             JPT_GUEST_IMAGE_BASE, JPT_GUEST_IMAGE_BASE + span);
    host_log("loader", ".text 0x%08X-0x%08X is data to the CPU: it runs as lifted C",
             lift_text_start, lift_text_end);
    if (!selfmod_init()) return HOST_EXIT_SETUP;

    guest_registry_init(game_dir, store_dir, user.saves_dir);
    io_trace_user_dir(user.saves_dir);
    l10n_set_user_folder(user.saves_dir);
    if (native32_bind(JPT_GUEST_IMAGE_BASE, s_shims, (int)(sizeof s_shims / sizeof s_shims[0]))) {
        host_log("imports", "unresolved imports (listed above as [bind] lines); refusing to run");
        return HOST_EXIT_SETUP;
    }
    host_log("imports", "import table bound: see the [bind] line above for native/shimmed counts");
    if (!jpt_dsound_bridge_install(sdl_audio_backend())) host_log("audio", "the DirectSound compatibility layer could not be installed");
    if (dsound_patch_imports(GetModuleHandleA("smackw32.dll")) != 3)
        host_log("audio", "SMACKW32.DLL does not resolve DirectSound through the host: its audio may use the system");
    if (!guest_env_apply_command_line() || !io_trace_init()) {
        host_log("host", "msvcrt.dll lacks _acmdln, fopen or _open; cannot set up the guest CRT");
        return HOST_EXIT_SETUP;
    }
    if (jpt_custom_level_active() && !custom_level_files_start(game_dir)) {
        host_log("custom-level", "the game's level files cannot be redirected");
        return HOST_EXIT_SETUP;
    }
    milestones_init();
    ddraw_trace_init();
    if (!ddraw_compat_init(!opt.system_ddraw)) return HOST_EXIT_SETUP;
    renderer_filter(&cfg, opt.dev_filter, &filter, &anisotropy);
    {
        jpt_win_renderer_cfg_t rcfg;
        char vk_cache[MAX_PATH];
        memset(&rcfg, 0, sizeof rcfg);
        _snprintf(vk_cache, sizeof vk_cache - 1, "%s\\cache\\vulkan_pipeline.cache", user.root);
        vk_cache[sizeof vk_cache - 1] = 0;
        rcfg.requested = (jpt_renderer_t)cfg.renderer;
        rcfg.system_ddraw = opt.system_ddraw;
        rcfg.gpu_request = cfg.graphics_card;
        rcfg.cache_path = vk_cache;
        rcfg.box_w = (unsigned)box.width;
        rcfg.box_h = (unsigned)box.height;
        rcfg.filter = filter;
        rcfg.anisotropy = anisotropy;
        rcfg.msaa = cfg.msaa;
        rcfg.vsync = cfg.vsync;
        rcfg.stretch = cfg.stretch;
        rcfg.aspect = cfg.aspect;
        rcfg.ao_level = cfg.ambient_occlusion;
        rcfg.water_refraction = cfg.water_refraction;
        rcfg.enhanced_sky = cfg.enhanced_sky;
        renderer_backend_configure(&rcfg);
    }
    if (opt.debug_d3d_output && !d3dr_set_debug_output(opt.debug_d3d_output)) return HOST_EXIT_SETUP;
    window_host_set_mouse_sensitivity(cfg.mouse_sensitivity);
    host_log("input", "mouse sensitivity %.2f (both axes)", cfg.mouse_sensitivity);
    launch_handoff_phase(JPT_HANDOFF_PHASE_RENDERER);
    if (!renderer_backend_startup()) return HOST_EXIT_SETUP;

    if (opt.dry_run) {
        host_log("host", "dry run complete: nothing was executed; without --dry-run the game enters 0x%08X",
                 lift_entry_va);
        return 0;
    }
    if (!opt.lifted_water) guest_kernels_install();
    {
        jpt_size_t hardware = jpt_gameplay_mode(box, cfg.aspect, 1), software = jpt_gameplay_mode(box, cfg.aspect, 0);
        jpt_size_t hardware_title = jpt_title_size(box, hardware, 1);
        if (!widescreen_install_modes() || !widescreen_install_camera(cfg.aspect)) hardware = software = jpt_gameplay_mode(box, 0, 0);
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
    video_subtitles_configure(cfg.video_subtitles, game_dir);
    minimap_configure(cfg.minimap);
    cp_configure(cfg.compass, cfg.waypoints);
    guest_features_install();
    host_log("ce", "active AI: %s", active_ai_installed() ? "enhanced (6)" : "original (3)");
    ce_graphics_install(d3dr_hardware());
    engine_limits_install(d3dr_hardware());
    texture_streaming_install();
    engine_heaps_install();
    terrain_quality_install();
    cpu_polygons_install();
    gamepad_install();
    l10n_install();
    menu_audio_install();
    if (!guest_overrides_check()) return HOST_EXIT_SETUP;
    debug_menu_start_custom_level();
    engine_limits_log(d3dr_hardware(), filter_label(&cfg), cfg.msaa, cfg.vsync);
    level_session_start(version->id, renderer_backend_key(), cfg.language);
    launch_handoff_phase(JPT_HANDOFF_PHASE_WINDOW);
    host_log("guest", "entering the CRT entry point 0x%08X", lift_entry_va);
    return run_guest();
}

int main(int argc, char** argv) {
    const int code = host_main(argc, argv);
    jpt_level_scripts_end();
    jpt_session_end();
    launch_handoff_finish(code);
    return code;
}
