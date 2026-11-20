/* The headless bootstrap against the installed game: the lifted entry point runs on a guest thread against the virtual window (no
 * window, no audio) until the guest presents its first frame. The outcome must be a defined stop, never a stack, dispatch or lift
 * failure, and it must be the same every time: the guest registers its window class, creates the window, opens DirectDraw, sets
 * the virtual 640x480x16 display and presents a frame. A longer run must put a real picture (the main menu) on that window.
 * Needs the game's guest image; without it the suite says DATA UNAVAILABLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_thunks.h"
#include "linux_boot.h"
#include "linux_hle.h"
#include "linux_log.h"
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
    printf("linux_boot_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
