/*
 * The Linux x86_64 executable's bring-up: reserve the guest's 4 GB window, lay out the installed game's image in it,
 * bind every import of the executable to the Linux HLE, and report what is implemented and what is not.
 *
 * --bootstrap-headless runs the lifted guest against the virtual window (no window, no audio); --run shows it in an SDL3
 * window (docs/LINUX.md). The default dry run only reports.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_fs.h"
#include "jpt_vpath.h"
#include "linux_abi.h"
#ifdef JPT_LINUX_GUEST
#include "linux_boot.h"
#endif
#include "linux_hle.h"
#include "linux_crash.h"
#include "linux_shutdown.h"
#include "linux_log.h"
#include "linux_paths.h"
#include "linux_selfcheck.h"
#include "linux_vulkan.h"

#define EXIT_SETUP 1
#define EXIT_UNIMPLEMENTED 3
#define EXIT_NO_RUN 4
#define MAX_DLLS 32

typedef struct {
    char name[64];
    int imports, implemented, missing;
} dll_stat_t;

typedef struct {
    dll_stat_t dll[MAX_DLLS];
    int ndll;
    int missing_total;
} bind_report_t;

typedef struct {
    const char* game;
    const char* user_root;
    const char* log_dir;
    const char* log_file;
    const char* version;
    const char* renderer;
    const char* graphics_card;
    int dry_run;
    int bootstrap;
    int build_info, self_check;
} options_t;

static void usage(void) {
    printf("usage: trespasser_recomp [options]\n"
           "  --game DIR             the installed game (default: game/ next to the program, or TRESPASSER_GAME_DIR)\n"
           "  --version ID           the game version folder under DIR/versions (default en-116)\n"
           "  --user_data_root DIR   saves, settings and caches (default: user_data/ next to the program,\n"
           "                         or TRESPASSER_USER_DATA_ROOT)\n"
           "  --log-dir DIR          the log folder (default: logs/ next to the program, or TRESPASSER_LOG_DIR)\n"
           "  --log-file FILE        append to FILE instead of the next numbered log\n"
           "  --dry-run              lay out the guest, bind the imports, report, stop (the default)\n"
           "  --bootstrap-headless   run the lifted entry point with no window and no audio until the first import with no\n"
           "                         implementation, and report it\n"
           "  --run                  run the guest in an SDL3 window (needs the lifted guest and SDL3 video)\n"
           "  --renderer NAME        select renderer: auto, vulkan, software (default: auto)\n"
           "  --graphics-card ID     select Vulkan GPU by label, stable vendor:device:uuid ID, or auto\n"
           "  --build-info           print what this binary was built from (JSON), nothing else\n"
           "  --self-check           prove which features this binary holds (JSON); no window, no audio, no log, no guest code\n"
           "  --help\n");
}

static const char* argument_of(int* i, int argc, char** argv, const char* name) {
    size_t n = strlen(name);
    if (!strncmp(argv[*i], name, n) && argv[*i][n] == '=') return argv[*i] + n + 1;
    if (!strcmp(argv[*i], name) && *i + 1 < argc) return argv[++*i];
    return NULL;
}

static int parse(int argc, char** argv, options_t* o, int* run) {
    memset(o, 0, sizeof *o);
    o->version = "en-116";
    o->dry_run = 1;
    *run = 0;
    for (int i = 1; i < argc; i++) {
        const char* v;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return usage(), 2;
        if (!strcmp(argv[i], "--dry-run")) o->dry_run = 1;
        else if (!strcmp(argv[i], "--run")) *run = 1;
        else if (!strcmp(argv[i], "--bootstrap-headless")) o->bootstrap = 1;
        else if (!strcmp(argv[i], "--build-info")) o->build_info = 1;
        else if (!strcmp(argv[i], "--self-check")) o->self_check = 1;
        else if ((v = argument_of(&i, argc, argv, "--game")) != NULL) o->game = v;
        else if ((v = argument_of(&i, argc, argv, "--version")) != NULL) o->version = v;
        else if ((v = argument_of(&i, argc, argv, "--user_data_root")) != NULL) o->user_root = v;
        else if ((v = argument_of(&i, argc, argv, "--log-dir")) != NULL) o->log_dir = v;
        else if ((v = argument_of(&i, argc, argv, "--log-file")) != NULL) o->log_file = v;
        else if ((v = argument_of(&i, argc, argv, "--renderer")) != NULL) o->renderer = v;
        else if ((v = argument_of(&i, argc, argv, "--graphics-card")) != NULL) o->graphics_card = v;
        else {
            fprintf(stderr, "trespasser_recomp: unknown option %s\n", argv[i]);
            return usage(), EXIT_SETUP;
        }
    }
    return 0;
}

static dll_stat_t* stat_for(bind_report_t* r, const char* dll) {
    char lower[64];
    size_t n = 0;
    for (; dll[n] && n < sizeof lower - 1; n++) lower[n] = (char)((dll[n] >= 'A' && dll[n] <= 'Z') ? dll[n] + 32 : dll[n]);
    lower[n] = 0;
    for (int i = 0; i < r->ndll; i++)
        if (!strcmp(r->dll[i].name, lower)) return &r->dll[i];
    if (r->ndll == MAX_DLLS) return NULL;
    strcpy(r->dll[r->ndll].name, lower);
    return &r->dll[r->ndll++];
}

static uint32_t resolve_counting(const char* dll, const char* name, uint16_t ordinal, void* user) {
    bind_report_t* r = (bind_report_t*)user;
    dll_stat_t* s = stat_for(r, dll);
    uint32_t before = hle_missing_count(), va = hle_resolve(dll, name, ordinal, NULL);
    if (s) {
        s->imports++;
        if (hle_missing_count() != before) {
            s->missing++;
            r->missing_total++;
        } else if (name && hle_find(dll, name)) {
            s->implemented++;
        } else {
            s->missing++;
        }
    }
    return va;
}

static int find_guest_image(const lx_paths_t* paths, const char* version, jpt_vpath_t* vp, char* host, size_t cap) {
    char guest[JPT_PATH_MAX];
    snprintf(guest, sizeof guest, "C:\\versions\\%s\\guest.img", version);
    if (jpt_vpath_resolve(vp, guest, 0, host, cap) == JPT_VP_OK) return 1;
    if (jpt_vpath_resolve(vp, "C:\\guest.img", 0, host, cap) == JPT_VP_OK) return 1;
    if (jpt_vpath_resolve(vp, "C:\\trespass.exe", 0, host, cap) == JPT_VP_OK) {
        lx_log("boot", "no guest.img under %s: using trespass.exe as a development input", paths->game_dir);
        return 1;
    }
    return 0;
}

static void report(const bind_report_t* r, uint32_t bound) {
    int implemented = 0, total = 0;
    lx_log("bind", "%u imports bound to thunks, %d without a Linux HLE", bound, r->missing_total);
    for (int i = 0; i < r->ndll; i++) {
        lx_log("bind", "  %-14s %3d imports: %3d implemented, %3d missing", r->dll[i].name, r->dll[i].imports,
               r->dll[i].implemented, r->dll[i].missing);
        implemented += r->dll[i].implemented;
        total += r->dll[i].imports;
    }
    lx_log("bind", "%d of %d imports implemented", implemented, total);
}

#ifdef JPT_LINUX_GUEST
static int boot_guest(const options_t* o, const lx_paths_t* paths, int window) {
    lx_boot_options_t options;
    lx_boot_result_t r;
    lx_boot_status_t status;
    lx_window_backend_t* backend = NULL;
    char settings_file[LX_PATH_MAX + 32];
#ifdef JPT_LINUX_SDL_VIDEO
    if (window) backend = lx_sdl_backend();
#endif
    lx_boot_options_init(&options, paths->game_dir, o->version, paths->user_root, paths->saves_dir, backend, window);
    options.play_videos = 1;
    options.use_gamepad = 1;
    options.use_features = 1;
    lx_vk_set_cli_overrides(o->renderer, o->graphics_card);
    snprintf(settings_file, sizeof settings_file, "%s/trespasser.ini", paths->launcher_dir);
    options.settings_file = settings_file;
    status = lx_boot_headless(&options, &r);
    if (window && options.backend) options.backend->destroy(options.backend);
    printf("%s\n", lx_boot_status_name(status));
    lx_log("boot", "%s: %s (guest EIP 0x%08X, caller 0x%08X); imports %u/%u implemented, %u reached",
           lx_boot_status_name(status), r.detail, r.guest_eip, r.caller, r.imports_implemented, r.imports_total,
           r.imports_reached);
    lx_log_close();
    switch (status) {
    case LX_BOOT_EXITED: return lx_boot_process_exit_code(&r);
    case LX_BOOT_MISSING_IMPORT: return 20;
    case LX_BOOT_REACHED_USER32: return 21;
    case LX_BOOT_FATAL_ABI: return 30;
    case LX_BOOT_BAD_DISPATCH: return 31;
    case LX_BOOT_NOT_LIFTED: return 32;
    case LX_BOOT_FRAME_PRESENTED: return 0;
    case LX_BOOT_IDLE: return 22;
    case LX_BOOT_DATA_UNAVAILABLE: return 40;
    default: return EXIT_SETUP;
    }
}
#endif

int main(int argc, char** argv) {
    options_t o;
    lx_paths_t paths;
    char exe[PATH_MAX], err[256], image_path[JPT_PATH_MAX], map_err[128];
    ssize_t n;
    jpt_vpath_t* vp;
    gm_image_t img;
    bind_report_t rep;
    uint32_t bound = 0;
    int run, parsed, unresolved;

    parsed = parse(argc, argv, &o, &run);
    if (parsed) return parsed == 2 ? 0 : parsed;
    if (o.build_info) return lx_build_info_print();
    if (o.self_check) return lx_selfcheck_run();
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    if (!lx_paths_resolve(&paths, exe, o.game, o.user_root, o.log_dir, err, sizeof err) || !lx_paths_prepare(&paths, err, sizeof err)) {
        fprintf(stderr, "trespasser_recomp: %s\n", err);
        return EXIT_SETUP;
    }
    if (!lx_log_open(paths.logs_dir, o.log_file)) {
        fprintf(stderr, "trespasser_recomp: cannot create a log in %s\n", paths.logs_dir);
        return EXIT_SETUP;
    }
    lx_crash_install();
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    lx_shutdown_install_signals();
    lx_log("boot", "Linux x86_64 host, %d-bit process", (int)(sizeof(void*) * 8));
    lx_log("boot", "game %s, user data %s, log %s", paths.game_dir, paths.user_root, lx_log_path());

#ifdef JPT_LINUX_GUEST
    if (o.bootstrap) return boot_guest(&o, &paths, 0);
#ifdef JPT_LINUX_SDL_VIDEO
    if (run) return boot_guest(&o, &paths, 1);
#endif
#else
    if (o.bootstrap) {
        lx_log("boot", "--bootstrap-headless needs the lifted guest: configure with JPT_LINUX_LIFTED=ON");
        return EXIT_SETUP;
    }
#endif
    vp = jpt_vpath_new();
    if (!vp || !jpt_vpath_mount(vp, 'C', paths.game_dir)) {
        lx_log("boot", "cannot map the game folder");
        return EXIT_SETUP;
    }
    if (!find_guest_image(&paths, o.version, vp, image_path, sizeof image_path)) {
        lx_log("boot", "no guest image for %s under %s (versions/%s/guest.img)", o.version, paths.game_dir, o.version);
        return EXIT_SETUP;
    }
    if (!gm_init(0, 0) || !ht_init()) {
        lx_log("boot", "cannot reserve the guest's 4 GB address space");
        return EXIT_SETUP;
    }
    if (gm_pe_map(image_path, GM_PE_PREFERRED, 0, &img, map_err, sizeof map_err) != 0) {
        lx_log("boot", "cannot map %s: %s", image_path, map_err);
        return EXIT_SETUP;
    }
    lx_log("boot", "guest image 0x%08X-0x%08X, entry 0x%08X, guest window at host %p", img.base, img.base + img.span,
           img.entry, (void*)gm_base());

    hle_init(vp, img.base, "C:\\trespass.exe");
    memset(&rep, 0, sizeof rep);
    unresolved = gm_pe_bind(&img, resolve_counting, &rep, &bound);
    report(&rep, bound);
    if (unresolved) lx_log("bind", "%d imports could not be bound at all", unresolved);

    if (run) {
        lx_log("boot", "--run needs the lifted guest and the SDL3 video subsystem: configure with JPT_LINUX_LIFTED=ON and "
                       "JPT_LINUX_SDL_VIDEO=ON (docs/LINUX.md)");
        return EXIT_NO_RUN;
    }
    lx_log("boot", "dry run complete: %d imports have no Linux HLE yet, so the guest cannot run", rep.missing_total);
    hle_shutdown();
    jpt_vpath_free(vp);
    gm_shutdown();
    lx_log_close();
    return rep.missing_total || unresolved ? EXIT_UNIMPLEMENTED : 0;
}
