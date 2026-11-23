/* The headless bootstrap against the installed game: the lifted entry point runs on a guest thread against the virtual window (no
 * window, no audio) until the guest presents its first frame. The outcome must be a defined stop, never a stack, dispatch or lift
 * failure, and it must be the same every time: the guest registers its window class, creates the window, opens DirectDraw, sets
 * the virtual 640x480x16 display and presents a frame. A longer run must put a real picture (the main menu) on that window.
 * The version matrix always checks the compiled lifts, profiles and registries; an installed guest image is booted to its
 * first frame. Needs the game's guest data for that boot; without it the suite says DATA UNAVAILABLE and still reports the
 * static matrix. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "active_profile.h"
#include "guest_thunks.h"
#include "guest_va.h"
#include "jpt_game_version.h"
#include "jpt_hash.h"
#include "linux_boot.h"
#include "linux_hle.h"
#include "linux_lift.h"
#include "linux_log.h"
#include "linux_modules.h"
#include "linux_overrides.h"
#include "linux_spine.h"
#include "linux_window.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

/* The number of distinct colours (5-bit channels) in a binary PPM: a flat frame has one, a menu thousands. */
static unsigned distinct_colours(const char* path) {
    FILE* f = fopen(path, "rb");
    static unsigned char seen[1 << 12];
    int w = 0, h = 0, max = 0;
    unsigned count = 0;
    if (!f) return 0;
    memset(seen, 0, sizeof seen);
    if (fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || fgetc(f) == EOF) {
        fclose(f);
        return 0;
    }
    for (long i = 0; i < (long)w * h; i++) {
        unsigned char rgb[3];
        unsigned key;
        if (fread(rgb, 1, 3, f) != 3) break;
        key = ((unsigned)(rgb[0] >> 3) << 10) | ((unsigned)(rgb[1] >> 3) << 5) | (unsigned)(rgb[2] >> 3);
        if (!(seen[key >> 3] & (1u << (key & 7)))) seen[key >> 3] |= (unsigned char)(1u << (key & 7)), count++;
    }
    fclose(f);
    return count;
}

enum { NEW_GAME_X = 310, NEW_GAME_Y = 105, MENU_FRAMES = 300 };

typedef struct {
    unsigned click_at;
    const char* dump_prefix;
} script_t;

static void queue_mouse(lx_event_kind_t kind, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.x = NEW_GAME_X, ev.y = NEW_GAME_Y, ev.vk = 1, ev.down = down;
    lx_virtual_queue_event(&ev);
}

/* The menu is up by frame 300: point at NEW GAME and click. */
static void new_game_hook(unsigned frame, void* user) {
    const script_t* sc = (const script_t*)user;
    if (frame == sc->click_at) queue_mouse(LX_EV_MOUSE_MOVE, 0);
    if (frame == sc->click_at + 2) queue_mouse(LX_EV_MOUSE_BUTTON, 1);
    if (frame == sc->click_at + 4) queue_mouse(LX_EV_MOUSE_BUTTON, 0);
    if (sc->dump_prefix && frame >= sc->click_at && (frame - sc->click_at) % 100 == 0) {
        char path[256];
        snprintf(path, sizeof path, "%s_%u.ppm", sc->dump_prefix, frame);
        lx_virtual_dump_ppm(path);
    }
}

typedef struct {
    const char* id;
    const char* lift;
    const char* profile;
    unsigned patch_groups;
    const char* sha256;
    unsigned selfmod_lo, selfmod_hi;
} expected_version_t;

typedef struct {
    const char* id;
    const char* lift;
    char build[8], gate[8], runtime[8], data[16];
    char detail[200];
    unsigned selfmod_entries, imports_total, modules;
} version_result_t;

static const expected_version_t k_versions[] = {
    { "en-116", "lift116", "en-116", 15, "d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed", 0x0063B000, 0x00644B75 },
    { "ru-116", "lift116", "en-116", 15, "d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed", 0x0063B000, 0x00644B75 },
    { "es-117", "lift_es117", "es-117", 6, "cfc95e60fcf8ea505ed586e24002858324308774cc5197bac85205633360f753", 0x0063D000, 0x00646B75 },
    { "ptbr-117", "lift_ptbr117", "ptbr-117", 0, "d5de43a68f57caf7644d8045bc8c27936f03357f453e326e846c1f745f1f0040", 0x0063F000, 0x00648B75 },
};

static void set_failure(char* detail, size_t cap, const char* text) {
    if (!detail[0]) snprintf(detail, cap, "%s", text);
}

static void write_version_report(const version_result_t* rows, size_t count) {
    (void)mkdir("build", 0755);
    (void)mkdir("build/logs", 0755);
    FILE* f = fopen("build/logs/version_parity.json", "w");
    CHECK(f != NULL, "the version parity report can be written");
    if (!f) return;
    fprintf(f, "{\"schema\":1,\"versions\":[");
    for (size_t i = 0; i < count; i++) {
        const version_result_t* r = &rows[i];
        fprintf(f, "%s{\"id\":\"%s\",\"lift\":\"%s\",\"build\":\"%s\",\"static\":\"%s\",\"runtime\":\"%s\","
                   "\"data\":\"%s\",\"detail\":\"%s\",\"selfmod_entries\":%u,\"imports_total\":%u,\"modules\":%u}",
                i ? "," : "", r->id, r->lift, r->build, r->gate, r->runtime, r->data, r->detail, r->selfmod_entries,
                r->imports_total, r->modules);
    }
    fprintf(f, "]}\n");
    fclose(f);
}

#define CHECK_VERSION(kind, cond, text, ...)                                                                            \
    do {                                                                                                                \
        int before = g_failures;                                                                                        \
        CHECK(cond, __VA_ARGS__);                                                                                       \
        if (g_failures != before) {                                                                                     \
            kind##_ok = 0;                                                                                              \
            set_failure(detail, sizeof detail, text);                                                                   \
        }                                                                                                               \
    } while (0)

/* The compiled lift, profile and registry of each version, then a headless boot of every installed one. */
static void test_version_matrix(const char* game_dir, const char* user_root) {
    version_result_t results[sizeof k_versions / sizeof k_versions[0]];
    memset(results, 0, sizeof results);
    for (size_t i = 0; i < sizeof k_versions / sizeof k_versions[0]; i++) {
        const expected_version_t* expected = &k_versions[i];
        version_result_t* result = &results[i];
        const jpt_game_version_t* version = jpt_game_version_by_id(expected->id);
        const lx_lift_t* image = version ? lx_lift_for_version(version) : NULL;
        jpt_lift_t lift = JPT_LIFT_NONE;
        int build_ok = 1, gate_ok = 1, data_ok = 1, runtime_ok = 1;
        char detail[200] = "", dir[1024], path[1200], hash[65];
        lx_boot_options_t o;
        lx_boot_result_t r;
        unsigned prev = 0;
        int sorted = 1, in_range = 1;

        result->id = expected->id;
        result->lift = expected->lift;
        snprintf(result->build, sizeof result->build, "FAIL");
        snprintf(result->gate, sizeof result->gate, "FAIL");
        snprintf(result->runtime, sizeof result->runtime, "BLOCKED");
        snprintf(result->data, sizeof result->data, "absent");

        CHECK_VERSION(build, version != NULL, "the version table has no entry", "%s is not in the version table", expected->id);
        CHECK_VERSION(build, image != NULL, "the compiled lift is missing", "%s has no compiled lift", expected->id);
        if (!version || !image) {
            snprintf(result->build, sizeof result->build, "FAIL");
            snprintf(result->gate, sizeof result->gate, "FAIL");
            snprintf(result->detail, sizeof result->detail, "%s", detail);
            continue;
        }

        CHECK_VERSION(build, !strcmp(version->lift, expected->lift), "the version names another lift",
                      "%s names %s, not %s", expected->id, version->lift, expected->lift);
        CHECK_VERSION(build, jpt_lift_find(expected->lift, &lift) && lift == image->lift, "the lift id does not resolve",
                      "%s does not resolve to its compiled lift", expected->lift);
        CHECK_VERSION(build, !strcmp(image->id, expected->lift), "the compiled lift has another id",
                      "%s is the compiled lift, not %s", image->id, expected->lift);
        CHECK_VERSION(build, !strcmp(image->meta->source_profile, expected->profile), "the lift comes from another profile",
                      "%s was generated from %s", expected->lift, image->meta->source_profile);
        CHECK_VERSION(build, !strcmp(image->meta->source_sha256, expected->sha256), "the lift has another fingerprint",
                      "%s has the wrong source SHA-256", expected->lift);
        CHECK_VERSION(build, jpt_lift_meta_check(&jpt_lift_data[image->lift].expected, image->meta) == JPT_LIFT_META_OK,
                      "the lift metadata does not match the build", "%s has stale metadata", expected->lift);
        CHECK_VERSION(build, image->main.count > 0 && image->main.count == image->meta->function_count,
                      "the dispatch table is empty or has another size", "%s has %u dispatched functions", expected->lift,
                      (unsigned)image->main.count);
        CHECK_VERSION(build, image->meta->entry_va == jpt_lift_data[image->lift].expected.entry_va &&
                                 image->meta->image_base == 0x00400000u,
                      "the lift entry or image base is wrong", "%s has the wrong entry point", expected->lift);
        CHECK_VERSION(build, image->selfmod.count == 46, "the SelfMod table has another size", "%s has %u SelfMod entries",
                      expected->lift, (unsigned)image->selfmod.count);
        CHECK_VERSION(build, image->meta->selfmod_start == expected->selfmod_lo && image->meta->selfmod_end == expected->selfmod_hi,
                      "the SelfMod range is wrong", "%s SelfMod is 0x%08X-0x%08X", expected->lift, image->meta->selfmod_start,
                      image->meta->selfmod_end);
        for (size_t k = 0; k < image->selfmod.count; k++) {
            uint32_t va = image->selfmod.entries[k].va;
            if (va < expected->selfmod_lo || va >= expected->selfmod_hi) in_range = 0;
            if (k && va <= prev) sorted = 0;
            prev = va;
        }
        CHECK_VERSION(build, sorted, "the SelfMod table is not sorted", "%s SelfMod entries are not sorted", expected->lift);
        CHECK_VERSION(build, in_range, "a SelfMod entry is outside its range", "%s SelfMod has an entry outside its range",
                      expected->lift);
        prev = 0;
        sorted = 1;
        for (size_t k = 0; k < image->main.count; k++) {
            uint32_t va = image->main.entries[k].va;
            if (k && va <= prev) sorted = 0;
            if (va >= expected->selfmod_lo && va < expected->selfmod_hi) sorted = 0;
            prev = va;
        }
        CHECK_VERSION(build, sorted, "the main dispatch table overlaps SelfMod or is not sorted",
                      "%s main dispatch is not sorted or overlaps SelfMod", expected->lift);
        CHECK_VERSION(build, image->module_count == 1 && image->modules && !strcmp(image->modules[0].meta->name, "msvcirt") &&
                                 image->modules[0].table.count == image->modules[0].meta->function_count,
                      "the MSVCIRT module is missing", "%s has no complete MSVCIRT module", expected->lift);
        snprintf(result->build, sizeof result->build, "%s", build_ok ? "PASS" : "FAIL");
        result->selfmod_entries = (unsigned)image->selfmod.count;

        jpt_profile_reset();
        lx_overrides_reset();
        CHECK_VERSION(gate, jpt_profile_activate(version, dir, sizeof dir), "the active profile cannot be installed",
                      "%s cannot activate its profile", expected->id);
        CHECK_VERSION(gate, jpt_active_profile() == version && jpt_active_lift() == image->lift,
                      "the active profile or lift is wrong", "%s activates another profile", expected->id);
        CHECK_VERSION(gate, jpt_patch_groups == expected->patch_groups && jpt_patch_groups == jpt_lift_data[image->lift].patch_groups,
                      "the Patch 1.1 groups are wrong", "%s applies patch groups 0x%X", expected->id, jpt_patch_groups);
        CHECK_VERSION(gate, jpt_lift_missing_address(&jpt_lift_data[image->lift]) < 0,
                      "the profile lacks a required guest address", "%s lacks a guest address", expected->id);
        {
            static const unsigned used[] = { GVAI_0x005F80B0, GVAI_0x00411EA0, GVAI_0x0040BCC0, GVAI_0x0066E010,
                                             GVAI_0x006603D8 };
            for (size_t k = 0; k < sizeof used / sizeof used[0]; k++)
                CHECK_VERSION(gate, jpt_gva[used[k]] != 0, "a profile guest address is zero",
                              "%s has no address for a host-used guest location", expected->id);
        }
        lx_lift_register_overrides(image->lift);
        CHECK_VERSION(gate, guest_overrides_enable(), "the override registry cannot be enabled",
                      "%s cannot enable its override registry", expected->id);
        {
            override_counts_t counts = lx_overrides_counts();
            CHECK_VERSION(gate, counts.listed == lx_lift_override_count(image->lift),
                          "the override registry has another size", "%s registry has %u entries", expected->id, counts.listed);
            CHECK_VERSION(gate, counts.required_refused == 0, "a required override was refused",
                          "%s has a required override refusal", expected->id);
        }
        lx_overrides_reset();
        jpt_profile_reset();
        snprintf(result->gate, sizeof result->gate, "%s", gate_ok ? "PASS" : "FAIL");
        snprintf(result->detail, sizeof result->detail, "%s", detail);

        if (!jpt_game_version_installed(game_dir, version, dir, sizeof dir)) {
            printf("  %s: DATA UNAVAILABLE (no guest image under %s)\n", expected->id, game_dir);
            snprintf(result->data, sizeof result->data, "absent");
            snprintf(result->runtime, sizeof result->runtime, "BLOCKED");
            set_failure(result->detail, sizeof result->detail, "guest.img is not installed");
            continue;
        }
        snprintf(result->data, sizeof result->data, "present");
        snprintf(path, sizeof path, "%s/guest.img", dir);
        CHECK_VERSION(data, jpt_file_sha256(path, hash) && !strcmp(hash, expected->sha256),
                      "guest.img has another SHA-256", "%s guest.img is not the build the lift was generated from", expected->id);
        snprintf(path, sizeof path, "%s/MSVCIRT.DLL", dir);
        CHECK_VERSION(data, image->module_count == 1 && image->modules &&
                                 jpt_file_sha256(path, hash) && !strcmp(hash, image->modules[0].meta->sha256),
                      "MSVCIRT.DLL has another SHA-256", "%s MSVCIRT.DLL is not the supported file", expected->id);
        if (!data_ok) {
            snprintf(result->data, sizeof result->data, "invalid");
            snprintf(result->runtime, sizeof result->runtime, "FAIL");
            if (!result->detail[0]) snprintf(result->detail, sizeof result->detail, "%s", detail);
            continue;
        }

        jpt_profile_reset();
        lx_overrides_reset();
        lx_boot_options_init(&o, game_dir, expected->id, user_root, NULL, NULL, 0);
        o.use_features = 1;
        o.frame_limit = 1;
        if (lx_boot_headless(&o, &r) != LX_BOOT_FRAME_PRESENTED) {
            runtime_ok = 0;
            set_failure(result->detail, sizeof result->detail, "the guest did not present a frame");
        }
        CHECK_VERSION(runtime, r.status == LX_BOOT_FRAME_PRESENTED, "the guest did not present a frame",
                      "%s reads its installed image and presents a frame: %s (%s)", expected->id,
                      lx_boot_status_name(r.status), r.detail);
        CHECK_VERSION(runtime, r.entry == image->meta->entry_va, "the guest entered another entry point",
                      "%s enters its lift's entry point (0x%08X)", expected->id, r.entry);
        CHECK_VERSION(runtime, r.selfmod_entries == 46, "the guest ran another SelfMod table",
                      "%s runs 46 SelfMod entries (%u)", expected->id, r.selfmod_entries);
        CHECK_VERSION(runtime, r.modules == 1, "the MSVCIRT module did not load", "%s maps MSVCIRT (%u modules)", expected->id, r.modules);
        CHECK_VERSION(runtime, r.imports_total == 297, "the import count is wrong", "%s binds 297 imports (%u)", expected->id,
                      r.imports_total);
        CHECK_VERSION(runtime, r.overrides_installed >= r.overrides_required, "a required override was not installed",
                      "%s installs every required override (%u of %u)", expected->id, r.overrides_installed,
                      r.overrides_required);
        result->imports_total = r.imports_total;
        result->modules = r.modules;
        snprintf(result->runtime, sizeof result->runtime, "%s", runtime_ok ? "PASS" : "FAIL");
        if (!runtime_ok && !result->detail[0]) set_failure(result->detail, sizeof result->detail, "runtime boot failed");
    }
    write_version_report(results, sizeof results / sizeof results[0]);
}

static void test_features_on_after_off(const char* game_dir, const char* user_root) {
    static const char* const ids[] = { "en-116", "ptbr-117" };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        lx_boot_options_t options;
        lx_boot_result_t result;
        lx_boot_options_init(&options, game_dir, ids[i], user_root, NULL, NULL, 0);
        options.use_features = 1;
        options.frame_limit = 1;
        lx_boot_headless(&options, &result);
        CHECK(result.status == LX_BOOT_FRAME_PRESENTED, "%s boots with features on after a run with them off: %s (%s)", ids[i],
              lx_boot_status_name(result.status), result.detail);
    }
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    lx_boot_options_t o;
    lx_boot_result_t first, second, menu;
    const lx_virtual_state_t* v = lx_virtual_state();
    const char* ppm = "build/linux_boot_tests_frame.ppm";
    memset(&o, 0, sizeof o);
    if (getenv("JPT_LINUX_PERMISSIVE")) hle_set_permissive(1);
    o.game_dir = argc > 1 ? argv[1] : env && *env ? env : "game";
    o.version = "en-116";
    o.user_root = "build/linux_boot_tests_user";

    test_version_matrix(o.game_dir, o.user_root);

    lx_boot_headless(&o, &first);
    if (first.status == LX_BOOT_DATA_UNAVAILABLE) {
        printf("linux_boot_tests: skipped - DATA UNAVAILABLE (no guest image under %s)\n", o.game_dir);
        printf("linux_boot_tests: 1 checks, 0 failures\n");
        return 0;
    }
    printf("  result 1: %s: %s (guest EIP 0x%08X, caller 0x%08X), %u of %u imports implemented, %u reached\n",
           lx_boot_status_name(first.status), first.detail, first.guest_eip, first.caller, first.imports_implemented,
           first.imports_total, first.imports_reached);
    CHECK(first.status == LX_BOOT_FRAME_PRESENTED, "the guest presents a frame to the virtual window: %s (%s)", lx_boot_status_name(first.status),
          first.detail);
    CHECK(first.entry == 0x00626BE2u, "the lifted entry point is the executable's (0x%08X)", first.entry);
    CHECK(first.selfmod_entries == 46, "46 SelfMod entries are in the dispatch (%u)", first.selfmod_entries);
    CHECK(first.imports_total == 297, "all 297 imports are bound (%u)", first.imports_total);
    CHECK(first.imports_reached >= 100, "the guest got through start-up, the window, DirectDraw and into its loop (%u imports called)",
          first.imports_reached);
    CHECK(first.detail[0] != 0, "the stop says why");
    CHECK(first.modules == 1 && first.module_functions == 578 && first.module_imports == 48,
          "MSVCIRT is mapped with its 578 lifted functions and its 48 imports bound (%u, %u, %u)", first.modules, first.module_functions,
          first.module_imports);
    CHECK(first.dll_attach_count == 1, "DllMain(DLL_PROCESS_ATTACH) ran once (%u)", first.dll_attach_count);
    CHECK(v->presents == 1 && v->last.width == 640 && v->last.height == 480 && v->last.format == LX_PF_RGB565,
          "one 640x480 RGB565 frame reached the virtual window (%u presents, %dx%d)", v->presents, v->last.width, v->last.height);
    CHECK(!strcmp(v->title, "Trespasser") && v->width == 640 && v->height == 480, "the host window is the guest's: \"%s\" %dx%d", v->title,
          v->width, v->height);

    {
        const uint32_t* vas;
        unsigned n = lx_reached_imports(&vas);
        printf("  imports reached, in order:");
        for (unsigned i = 0; i < n; i++) printf(" %s", ht_name(vas[i]) ? ht_name(vas[i]) : "?");
        printf("\n");
    }
    lx_boot_headless(&o, &second);
    printf("  result 2: %s: %s (guest EIP 0x%08X), %u reached\n", lx_boot_status_name(second.status), second.detail, second.guest_eip,
           second.imports_reached);
    CHECK(second.status == first.status && !strcmp(second.detail, first.detail) && second.guest_eip == first.guest_eip &&
              second.imports_reached + 8 >= first.imports_reached && first.imports_reached + 8 >= second.imports_reached,
          "a second run in the same process stops at the same place (%s)", second.detail);

    o.frame_limit = getenv("JPT_LINUX_FRAMES") ? (unsigned)atoi(getenv("JPT_LINUX_FRAMES")) : 300;
    lx_boot_headless(&o, &menu);
    printf("  result 3 (%u frames): %s: %s (guest EIP 0x%08X), %u reached\n", o.frame_limit, lx_boot_status_name(menu.status), menu.detail,
           menu.guest_eip, menu.imports_reached);
    CHECK(menu.status == LX_BOOT_FRAME_PRESENTED && v->presents == o.frame_limit, "the guest keeps presenting frames (%u of %u)", v->presents,
          o.frame_limit);
    CHECK(lx_virtual_dump_ppm(ppm), "the last frame can be written out");
    if (getenv("JPT_LINUX_DUMP_PPM")) lx_virtual_dump_ppm(getenv("JPT_LINUX_DUMP_PPM"));
    CHECK(distinct_colours(ppm) > 300, "the frame is a picture, not a flat clear (%u colours)", distinct_colours(ppm));
    remove(ppm);

    {
        lx_boot_options_t fresh;
        lx_boot_result_t r;
        memset(&fresh, 0xA5, sizeof fresh);
        lx_boot_options_init(&fresh, o.game_dir, "en-116", o.user_root, NULL, NULL, 0);
        CHECK(!fresh.frame_hook && !fresh.hook_user && !fresh.frame_limit && !fresh.idle_limit && !fresh.unbounded,
              "options built the way the executable builds them leave no hook, limit or user pointer set");
        fresh.frame_limit = 3;
        lx_boot_headless(&fresh, &r);
        CHECK(r.status == LX_BOOT_FRAME_PRESENTED, "a boot from those options presents frames instead of calling a stale hook: %s", lx_boot_status_name(r.status));
    }

    {
        script_t sc = { MENU_FRAMES, getenv("JPT_LINUX_NEWGAME_DUMP") };
        lx_boot_result_t ng;
        const uint32_t* vas;
        unsigned n;
        o.frame_hook = new_game_hook;
        o.hook_user = &sc;
        o.frame_limit = MENU_FRAMES + (getenv("JPT_LINUX_NEWGAME_FRAMES") ? (unsigned)atoi(getenv("JPT_LINUX_NEWGAME_FRAMES")) : 400);
        lx_boot_headless(&o, &ng);
        printf("  New Game (click at frame %u, %u frames): %s: %s (guest EIP 0x%08X, caller 0x%08X), %u of %u imports implemented, %u reached, display %dx%d\n",
               sc.click_at, o.frame_limit, lx_boot_status_name(ng.status), ng.detail, ng.guest_eip, ng.caller, ng.imports_implemented, ng.imports_total,
               ng.imports_reached, v->last.width, v->last.height);
        n = lx_reached_imports(&vas);
        printf("  imports reached by the New Game run, in order:");
        for (unsigned i = 0; i < n; i++) printf(" %s", ht_name(vas[i]) ? ht_name(vas[i]) : "?");
        printf("\n");
        CHECK(ng.status == LX_BOOT_FRAME_PRESENTED, "New Game runs without a missing import: %s (%s)", lx_boot_status_name(ng.status), ng.detail);
    }
    test_features_on_after_off(o.game_dir, o.user_root);
    printf("linux_boot_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
