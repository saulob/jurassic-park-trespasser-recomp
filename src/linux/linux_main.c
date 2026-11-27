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
#include "jpt_app_identity.h"
#include "jpt_build_info.h"
#include "jpt_config.h"
#include "jpt_fs.h"
#include "jpt_game_version.h"
#include "jpt_user_data.h"
#include "jpt_vpath.h"
#ifdef JPT_LINUX_LAUNCHER
#include "jpt_launcher.h"
#include "jpt_renderer.h"
#endif
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
#include "linux_settings.h"
#include "linux_vulkan.h"

#define EXIT_SETUP 1
#define EXIT_UNIMPLEMENTED 3
#define EXIT_NO_RUN 4
#define MAX_DLLS 32
#define MAX_OVERRIDES 32
/* jpt::UserDataOutcome, which the launcher reports on its About page. */
#define USER_DATA_PORTABLE 0
#define USER_DATA_EXPLICIT 6

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
    int build_info, self_check, analyze_json;
    const char* analyze;
    const char *analyze_all, *analyze_report;
    int dry_run_asked, run_direct, force_launcher, skip_launcher, version_given;
    const char* launcher_renderer;
    const char* override_key[MAX_OVERRIDES];
    const char* override_value[MAX_OVERRIDES];
    int override_count;
    jpt_custom_level_request_t custom_level;
} options_t;

/* The command-line spellings of the settings the launcher saves; each one overrides the settings file for this run. */
static const struct {
    const char* option;
    const char* key;
} k_setting_options[] = {
    { "--aspect-ratio", JPT_KEY_ASPECT_RATIO },       { "--msaa", JPT_KEY_MSAA },
    { "--vsync", JPT_KEY_VSYNC },                     { "--display-mode", JPT_KEY_DISPLAY_MODE },
    { "--monitor", JPT_KEY_MONITOR },                 { "--window-size", JPT_KEY_WINDOW_SIZE },
    { "--presentation", JPT_KEY_PRESENTATION },       { "--show-fps", JPT_KEY_SHOW_FPS },
    { "--skip-intro", JPT_KEY_SKIP_INTRO },           { "--video-subtitles", JPT_KEY_VIDEO_SUBTITLES },
    { "--ambient-occlusion", JPT_KEY_AMBIENT_OCCLUSION },
};

static void usage(void) {
    printf("usage: trespasser_recomp [options]\n"
           "  With no mode option the launcher opens, then the game starts in an SDL3 window.\n"
           "  --game DIR             the installed game (default: game/ next to the program, or TRESPASSER_GAME_DIR)\n"
           "  --version ID           the game version folder under DIR/versions (default: the launcher's choice)\n"
           "  --user_data_root DIR   saves, settings and caches (default: user_data/ next to the program,\n"
           "                         or TRESPASSER_USER_DATA_ROOT)\n"
           "  --log-dir DIR          the log folder (default: logs/ next to the program, or TRESPASSER_LOG_DIR)\n"
           "  --log-file FILE        append to FILE instead of the next numbered log\n"
           "  --launcher             show the launcher even when it is turned off\n"
           "  --skip-launcher        skip the launcher once (unless the game files are not ready)\n"
           "  --launcher-renderer NAME  what draws the launcher's own window, for this run only: auto, software (needs no\n"
           "                         graphics driver). The game's renderer does not change\n"
           "  --dry-run              lay out the guest, bind the imports, report, stop\n"
           "  --bootstrap-headless   run the lifted entry point with no window and no audio until the first import with no\n"
           "                         implementation, and report it\n"
           "  --run                  start the game at once, without the launcher\n"
           "  --renderer NAME        select renderer: auto, vulkan, software (default: auto)\n"
           "  --graphics-card ID     select Vulkan GPU by label, stable vendor:device:uuid ID, or auto\n"
           "  --aspect-ratio, --msaa, --vsync, --display-mode, --monitor, --window-size, --presentation, --show-fps,\n"
           "  --skip-intro, --video-subtitles, --ambient-occlusion VALUE   override that setting for this run\n"
           "  --build-info           print what this binary was built from (JSON), nothing else\n"
           "  --self-check           prove which features this binary holds (JSON); no window, no audio, no log, no guest code\n"
           "  --analyze-custom-level PACKAGE [--json]  print what the Custom Levels import makes of a ZIP, 7z, RAR or folder; nothing\n"
           "                         is installed\n"
           "  --analyze-custom-levels FOLDER [--report FOLDER] [--json]  the same for every package below a folder, with totals\n"
           "  --help\n");
}

static const char* argument_of(int* i, int argc, char** argv, const char* name) {
    size_t n = strlen(name);
    if (!strncmp(argv[*i], name, n) && argv[*i][n] == '=') return argv[*i] + n + 1;
    if (!strcmp(argv[*i], name) && *i + 1 < argc) return argv[++*i];
    return NULL;
}

static int add_override(options_t* o, const char* key, const char* value) {
    if (o->override_count == MAX_OVERRIDES) return 0;
    o->override_key[o->override_count] = key;
    o->override_value[o->override_count++] = value;
    return 1;
}

static int parse(int argc, char** argv, options_t* o, int* run) {
    memset(o, 0, sizeof *o);
    o->version = "en-116";
    o->dry_run = 1;
    *run = 0;
    for (int i = 1; i < argc; i++) {
        const char* v;
        size_t k;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return usage(), 2;
        for (k = 0; k < sizeof k_setting_options / sizeof k_setting_options[0]; k++)
            if ((v = argument_of(&i, argc, argv, k_setting_options[k].option)) != NULL) break;
        if (k < sizeof k_setting_options / sizeof k_setting_options[0]) {
            if (!add_override(o, k_setting_options[k].key, v)) return fprintf(stderr, "trespasser_recomp: too many setting options\n"), EXIT_SETUP;
            continue;
        }
        if (!strcmp(argv[i], "--dry-run")) o->dry_run = o->dry_run_asked = 1;
        else if (!strcmp(argv[i], "--run")) *run = o->run_direct = 1;
        else if (!strcmp(argv[i], "--launcher")) o->force_launcher = 1;
        else if (!strcmp(argv[i], "--skip-launcher")) o->skip_launcher = 1;
        else if (!strcmp(argv[i], "--bootstrap-headless")) o->bootstrap = 1;
        else if (!strcmp(argv[i], "--build-info")) o->build_info = 1;
        else if (!strcmp(argv[i], "--self-check")) o->self_check = 1;
        else if (!strcmp(argv[i], "--json")) o->analyze_json = 1;
        else if ((v = argument_of(&i, argc, argv, "--analyze-custom-level")) != NULL) o->analyze = v;
        else if ((v = argument_of(&i, argc, argv, "--analyze-custom-levels")) != NULL) o->analyze_all = v;
        else if ((v = argument_of(&i, argc, argv, "--report")) != NULL) o->analyze_report = v;
        else if ((v = argument_of(&i, argc, argv, "--game")) != NULL) o->game = v;
        else if ((v = argument_of(&i, argc, argv, "--version")) != NULL) o->version = v, o->version_given = 1;
        else if ((v = argument_of(&i, argc, argv, "--user_data_root")) != NULL) o->user_root = v;
        else if ((v = argument_of(&i, argc, argv, "--log-dir")) != NULL) o->log_dir = v;
        else if ((v = argument_of(&i, argc, argv, "--log-file")) != NULL) o->log_file = v;
        else if ((v = argument_of(&i, argc, argv, "--renderer")) != NULL) o->renderer = v;
        else if ((v = argument_of(&i, argc, argv, "--launcher-renderer")) != NULL) {
            if (!jpt_launcher_renderer_valid(v, 0))
                return fprintf(stderr, "trespasser_recomp: --launcher-renderer needs auto or software\n"), EXIT_SETUP;
            o->launcher_renderer = v;
        }
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
    lx_vk_set_env_renderer(getenv("TRESPASSER_RENDERER"));
    snprintf(settings_file, sizeof settings_file, "%s/trespasser.ini", paths->launcher_dir);
    options.settings_file = settings_file;
    options.custom_level = &o->custom_level;
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

#if defined(JPT_LINUX_GUEST) && defined(JPT_LINUX_SDL_VIDEO)
#ifdef JPT_LINUX_LAUNCHER
static void launcher_log(const char* line) { lx_log("launcher", "%s", line); }

static jpt_launcher_result_t run_launcher(options_t* o, const lx_paths_t* paths, const char* settings_file) {
    jpt_launcher_config_t config;
    memset(&config, 0, sizeof config);
    config.game_dir = paths->game_dir;
    config.user_dir = paths->user_root;
    config.saves_dir = paths->saves_dir;
    config.logs_dir = paths->logs_dir;
    config.log_file = lx_log_path();
    config.settings_file = settings_file;
    config.user_data_outcome = o->user_root || getenv("TRESPASSER_USER_DATA_ROOT") ? USER_DATA_EXPLICIT : USER_DATA_PORTABLE;
    config.force_show = o->force_launcher && !o->skip_launcher;
    config.skip_once = o->skip_launcher;
    config.launcher_renderer = o->launcher_renderer;
    config.log = launcher_log;
    config.custom_level = &o->custom_level;
    return jpt_launcher_run(&config);
}
#endif

/* The version to run: --version, else the launcher's choice, among those this executable carries a lift for. With no
 * installed version at all the development layout (a guest image directly in the game folder) stays usable. */
static void choose_version(options_t* o, lx_paths_t* paths, const char* settings_file) {
    jpt_config_t config;
    char dir[JPT_PATH_MAX], saves[LX_PATH_MAX];
    const jpt_game_version_t* version = NULL;
    const char* wanted;
    jpt_config_defaults(&config);
    jpt_config_load(settings_file, &config);
    wanted = o->version_given ? o->version : config.game_version;
    if (!jpt_game_version_resolve(paths->game_dir, wanted, 1, dir, sizeof dir, &version)) {
        lx_log("game-version", "no installed version this build can run in %s: using %s as a development layout", paths->game_dir,
               o->version);
        return;
    }
    if (wanted[0] && strcasecmp(wanted, version->id))
        lx_log("game-version", "the selected version %s is not installed or this build cannot run it: %s is used", wanted, version->id);
    o->version = version->id;
    snprintf(paths->game_dir, sizeof paths->game_dir, "%s", dir);
    if (jpt_game_version_saves_dir(paths->user_root, paths->saves_dir, version, saves, sizeof saves) &&
        strcmp(saves, paths->saves_dir) && jpt_make_dirs(saves))
        strcpy(paths->saves_dir, saves);
    lx_log("game-version", "%s, build %d, %s, from %s; saves in %s", version->label, version->build, version->language, dir,
           paths->saves_dir);
}

static int play(options_t* o, lx_paths_t* paths) {
    char settings_file[LX_PATH_MAX + 32];
    snprintf(settings_file, sizeof settings_file, "%s/trespasser.ini", paths->launcher_dir);
    if (jpt_config_migrate(settings_file)) lx_log("Linux settings", "%s: the earlier launcher's defaults were reset once", settings_file);
    if (jpt_config_migrate_language(settings_file))
        lx_log("Linux settings", "%s: the earlier game text and subtitle language settings became language", settings_file);
#ifdef JPT_LINUX_LAUNCHER
    const jpt_launcher_result_t launched = o->run_direct ? JPT_LAUNCHER_PLAY : run_launcher(o, paths, settings_file);
    if (launched != JPT_LAUNCHER_PLAY) {
        lx_log("boot", launched == JPT_LAUNCHER_FAILED ? "the launcher could not open: the game was not started"
                                                       : "the launcher was closed without starting the game");
        lx_log_close();
        return launched == JPT_LAUNCHER_FAILED ? EXIT_SETUP : 0;
    }
#endif
    choose_version(o, paths, settings_file);
    lx_settings_set_overrides(o->override_key, o->override_value, o->override_count);
    return boot_guest(o, paths, 1);
}
#endif

#ifdef JPT_LINUX_LAUNCHER
static int analyze_custom_levels(const options_t* o, const lx_paths_t* paths) {
    char dir[JPT_PATH_MAX], data[JPT_PATH_MAX + 8];
    const jpt_game_version_t* version = NULL;
    const int installed = jpt_game_version_resolve(paths->game_dir, "", 1, dir, sizeof dir, &version);
    if (installed) snprintf(data, sizeof data, "%s/data", dir);
    if (o->analyze_all) return jpt_analyze_custom_levels(o->analyze_all, installed ? data : NULL, o->analyze_report, o->analyze_json);
    return jpt_analyze_custom_level(o->analyze, installed ? data : NULL, o->analyze_json);
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
#ifndef JPT_LINUX_LAUNCHER
    if (o.analyze || o.analyze_all) return fprintf(stderr, "trespasser_recomp: this build has no launcher to analyze a custom level\n"), EXIT_SETUP;
#endif
    jpt_app_identity_apply(JPT_BUILD_VERSION);
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    if (!lx_paths_resolve(&paths, exe, o.game, o.user_root, o.log_dir, err, sizeof err) || !lx_paths_prepare(&paths, err, sizeof err)) {
        fprintf(stderr, "trespasser_recomp: %s\n", err);
        return EXIT_SETUP;
    }
#ifdef JPT_LINUX_LAUNCHER
    if (o.analyze || o.analyze_all) return analyze_custom_levels(&o, &paths);
#endif
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
    if (!o.dry_run_asked) return play(&o, &paths);
#endif
#else
    if (o.bootstrap) {
        lx_log("boot", "--bootstrap-headless needs the lifted guest: configure with JPT_LINUX_LIFTED=ON");
        return EXIT_SETUP;
    }
#endif
    vp = jpt_vpath_new();
    {
        char version_dir[JPT_PATH_MAX];
        const jpt_game_version_t* v = jpt_game_version_by_id(o.version);
        const char* mount_dir = (v && jpt_game_version_installed(paths.game_dir, v, version_dir, sizeof version_dir))
                                    ? version_dir
                                    : paths.game_dir;
        if (!vp || !jpt_vpath_mount(vp, 'C', mount_dir)) {
            lx_log("boot", "cannot map the game folder");
            return EXIT_SETUP;
        }
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
