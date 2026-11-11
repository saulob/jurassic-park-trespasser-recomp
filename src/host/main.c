/*
 * Trespasser recompilation host: a 32-bit process on pcrecomp's native32 runtime.
 * The lifted .text runs as C; imports go to the real Win32 DLLs; SelfMod runs natively.
 * Structure follows sp00nznet/themovies host.c (MIT).
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/d3d_translate.h"
#include "recomp_trace.h"

#ifndef TRESPASSER_DEFAULT_GAME_DIR
#define TRESPASSER_DEFAULT_GAME_DIR "game"
#endif
#ifndef TRESPASSER_LOG_DIR
#define TRESPASSER_LOG_DIR "build\\logs"
#endif

/* Lifted frames are several times the original's, and they live on the host stack. */
#define GUEST_THREAD_STACK (64u << 20)

typedef struct {
    const char* game_dir;
    const char* log_dir;
    const char* log_file;
    char guest_args[MAX_PATH];
    unsigned watchdog_seconds;
    int run;
    int system_ddraw;
    int hardware;
    unsigned render_w, render_h;
    int texture_filter;
    unsigned msaa;
} options_t;

static const char* const k_texture_filters[] = { "point", "bilinear", "trilinear", "anisotropic" };

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
    { "GetFileAttributesA", shim_GetFileAttributesA },
    { "FindFirstFileA", shim_FindFirstFileA },
    { "LZOpenFileA", shim_LZOpenFileA },
    { "LoadLibraryA", shim_LoadLibraryA },
    { "fopen", shim_fopen },
    { "_open", shim_open },
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
    { "exit", shim_exit },
    { "_exit", shim__exit },
    { "ExitProcess", shim_ExitProcess },
};

static void usage(void) {
    printf("usage: trespasser [--run] [--game DIR] [--log-file FILE | --log-dir DIR] [--watchdog SECONDS]\n"
           "                  [--ddraw compat|system] [--native-trace] [--callbacks] [--renderer software|d3d11]\n"
           "                  [--render-resolution auto|WxH] [--texture-filter point|bilinear|trilinear|anisotropic]\n"
           "                  [--msaa 0|2|4|8] [--generate-mips] [--debug-d3d-output MODE] [--debug-terrain-*]\n"
           "                  [--ddraw-cert-dialog] [--legacy-safemode-dialog] [--software-water]\n"
           "                  [-- guest arguments]\n"
           "  without --run the image is mapped, fingerprinted and bound, and nothing executes\n"
           "  --log-file appends to FILE; otherwise a new trespasser_NNN.log is created in the log directory\n"
           "  --ddraw system calls the system ddraw.dll instead of the compatibility layer (diagnostics only)\n"
           "  --renderer software (default) rasterises with the game's own SelfMod code; d3d11 translates its\n"
           "    Direct3D 6 path to D3D11; the other three options apply to d3d11 (defaults auto, anisotropic, 4)\n"
           "  --generate-mips gives the game's single-level textures generated mip chains (off: as the original)\n"
           "  --debug-d3d-output texture|vertex-color|depth: d3d11 draws show only that (diagnostics)\n"
           "  --debug-terrain-wireframe|solid|texture|nocull: d3d11 terrain draws only (diagnostics)\n"
           "  --debug-b560-solid|skip|texture|nodepth: d3d11 opaque object batch (sub_0055B560) only\n"
           "  --software-water keeps the game's software water with d3d11 (Hardware Water unset)\n"
           "  --legacy-safemode-dialog keeps the game's \"unstable last time, Safe Mode?\" prompt (off by default)\n");
    recomp_trace_help();
}

static int parse_options(int argc, char** argv, options_t* o) {
    o->game_dir = getenv("TRESPASSER_GAME_DIR");
    if (!o->game_dir) o->game_dir = TRESPASSER_DEFAULT_GAME_DIR;
    o->log_dir = TRESPASSER_LOG_DIR;
    o->texture_filter = 3;
    o->msaa = 4;
    for (int i = 1; i < argc; i++) {
        int taken = recomp_trace_arg(argc, argv, i);
        if (taken) {
            i += taken - 1;
        } else if (!strcmp(argv[i], "--run")) {
            o->run = 1;
        } else if (!strcmp(argv[i], "--game") && i + 1 < argc) {
            o->game_dir = argv[++i];
        } else if (!strcmp(argv[i], "--log-dir") && i + 1 < argc) {
            o->log_dir = argv[++i];
        } else if (!strcmp(argv[i], "--log-file") && i + 1 < argc) {
            o->log_file = argv[++i];
        } else if (!strcmp(argv[i], "--ddraw") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "system")) o->system_ddraw = 1;
            else if (strcmp(mode, "compat")) return 0;
        } else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "d3d11")) o->hardware = 1;
            else if (!strcmp(mode, "software")) o->hardware = 0;
            else return 0;
        } else if (!strcmp(argv[i], "--render-resolution") && i + 1 < argc) {
            if (!d3dt_parse_resolution(argv[++i], &o->render_w, &o->render_h)) return 0;
        } else if (!strcmp(argv[i], "--texture-filter") && i + 1 < argc) {
            const char* name = argv[++i];
            int found = -1;
            for (int k = 0; k < 4; k++)
                if (!strcmp(name, k_texture_filters[k])) found = k;
            if (found < 0) return 0;
            o->texture_filter = found;
        } else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) {
            o->msaa = strtoul(argv[++i], NULL, 10);
            if (o->msaa != 0 && o->msaa != 2 && o->msaa != 4 && o->msaa != 8) return 0;
        } else if (!strcmp(argv[i], "--ddraw-cert-dialog")) {
            registry_show_ddraw_cert_dialog();
        } else if (!strcmp(argv[i], "--software-water")) {
            registry_software_water();
        } else if (!strcmp(argv[i], "--legacy-safemode-dialog")) {
            registry_legacy_safemode_dialog();
        } else if (!strcmp(argv[i], "--generate-mips")) {
            d3dr_set_generate_mips(1);
        } else if (!strcmp(argv[i], "--debug-d3d-output") && i + 1 < argc) {
            if (!d3dr_set_debug_output(argv[++i])) return 0;
        } else if (tdiag_parse_option(argv[i])) {
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
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    return (int)code;
}

int main(int argc, char** argv) {
    options_t opt = { 0 };
    char game_dir[MAX_PATH], log_dir[MAX_PATH], log_file[MAX_PATH];
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
    int held = native32_hold_image(guest_env_exe_path(), trespasser_image_base);

    if (!host_log_open(log_dir, opt.log_file ? log_file : NULL)) {
        fprintf(stderr, "cannot open the log %s (error %lu)\n", opt.log_file ? log_file : log_dir, GetLastError());
        return HOST_EXIT_SETUP;
    }
    host_log("host", "Trespasser recompilation host, %u lifted functions in the dispatch table",
             recomp_dispatch_count);
    host_log("host", "log file %s", host_log_path());
    host_log("host", "game directory %s", game_dir);
    host_log("host", "guest address range %s", held ? "reserved at process start" : "NOT reserved in advance");
    if (!verify_fingerprint(guest_env_exe_path())) return HOST_EXIT_SETUP;

    window_host_init();
    native32_init();
    diagnostics_install(opt.run ? opt.watchdog_seconds : 0);

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

    guest_registry_init(game_dir);
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
    d3dr_configure(opt.hardware && !opt.system_ddraw, opt.render_w, opt.render_h, opt.texture_filter,
                   opt.msaa ? opt.msaa : 1);
    if (!d3dr_startup()) return HOST_EXIT_SETUP;

    if (!opt.run) {
        host_log("host", "dry run complete: nothing was executed; --run enters 0x%08X", trespasser_entry_va);
        return 0;
    }
    terrain_probe_install();
    host_log("guest", "entering the CRT entry point 0x%08X", trespasser_entry_va);
    return run_guest();
}
