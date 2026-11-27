/* The host features that change the lifted game, on the real guest: the program boots the installed game against the virtual
 * window with its features on (the overrides and lift hooks of src/host over lift116's stable entries), plays New Game into the
 * first level, and reads what the features did to the game's own memory: the overrides installed, the camera's field of view,
 * the official Patch 1.1 level fixes served through the LZ calls, the physics hooks that ran, Maximum object detail, the saved
 * cheats applied to the player and the Skip Intro answer to the opening. Needs the installed game; otherwise DATA UNAVAILABLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_menu.h"
#include "debug_menu_panel.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "guest_memory.h"
#include "linux_boot.h"
#include "linux_hle.h"
#include "linux_log.h"
#include "linux_overrides.h"
#include "linux_panel.h"
#include "linux_window.h"
#include "perf_sections.h"
#include "recomp_types.h"

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

#define LOG_FILE "build/linux_features_tests.log"
#define SETTINGS_FILE "build/linux_features_tests.ini"
#define MENU_FRAMES 300
#define LEVEL_FRAMES 700
#define NEW_GAME_X 310
#define NEW_GAME_Y 105

static const uint32_t k_shared[] = {
    0x0040DA00, 0x00410540, 0x004610D0, 0x00419330, 0x004195A0, 0x00419800, 0x004199D0, 0x00419C60, 0x00419CE0, 0x00419DB0, 0x00419E30,
    0x0041A200, 0x0041ADF0, 0x0041AE50, 0x0041AF00, 0x004213F0, 0x004218C0, 0x00475F60, 0x0049E530, 0x004EDFA0, 0x004F0960, 0x004F3FB0, 0x0051FD90,
    0x0060FC10, 0x005F80B0, 0x005F8A20, 0x005F8B10, 0x00614F50,
};

static void queue_mouse(lx_event_kind_t kind, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.x = NEW_GAME_X, ev.y = NEW_GAME_Y, ev.vk = 1, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static char* read_log(void) {
    FILE* f = fopen(LOG_FILE, "rb");
    char* text;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)n + 1, 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) text[0] = 0;
    fclose(f);
    return text;
}

static uint32_t read32(uint32_t va) {
    const uint32_t* p = (const uint32_t*)gm_ptr_checked(va, 4, 0);
    return p ? *p : 0;
}

typedef struct {
    unsigned frames;
    unsigned shared_installed, shared_total;
    uint32_t world, player, player_flags, detail;
    int cheats_open, cheats_hits, cheats_rows, cheats_has_invulnerable, objectives_open, objectives_goals;
    unsigned pairs;
    unsigned long long terrain_calls, human_idof;
    int evaluated;
} state_t;

#define WORLD_VA 0x0073F444u
#define PLAYER_VA 0x0066EF64u
#define PLAYER_FLAGS 0xDCu
#define RENDER_SETTINGS_VA 0x006D2224u
#define DETAIL_REDUCE 0x33u

static void queue_key(uint32_t vk) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.down = 1;
    lx_virtual_queue_event(&ev);
    ev.down = 0;
    lx_virtual_queue_event(&ev);
}

static void look_at_menu(state_t* s, int cheats) {
    dmp_frame_t f;
    memset(&f, 0, sizeof f);
    debug_menu_fill_frame(&f);
    if (cheats) {
        s->cheats_open = debug_menu_open_menu();
        s->cheats_hits = lx_panel_hits() && lx_panel_hits()->valid;
        s->cheats_rows = f.row_count;
        for (int i = 0; i < f.row_count; i++) s->cheats_has_invulnerable |= strstr(f.rows[i].text, "Invulnerab") != NULL;
    } else {
        s->objectives_open = debug_menu_open_menu();
        for (int i = 0; i < f.row_count; i++) s->objectives_goals += f.rows[i].kind == DM_ROW_GOAL;
    }
}

static void hook(unsigned frame, void* user) {
    state_t* s = (state_t*)user;
    if (frame == s->frames - 60) queue_key(0x79);
    if (frame == s->frames - 30) {
        look_at_menu(s, 1);
        queue_key(0x1B);
        queue_key(0x72);
    }
    if (frame == s->frames) look_at_menu(s, 0);
    if (frame == MENU_FRAMES) queue_mouse(LX_EV_MOUSE_MOVE, 0);
    if (frame == MENU_FRAMES + 2) queue_mouse(LX_EV_MOUSE_BUTTON, 1);
    if (frame == MENU_FRAMES + 4) queue_mouse(LX_EV_MOUSE_BUTTON, 0);
    if (frame != s->frames) return;
    s->evaluated = 1;
    for (unsigned i = 0; i < sizeof k_shared / sizeof k_shared[0]; i++) {
        s->shared_total++;
        s->shared_installed += lx_overrides_state(k_shared[i]) == OVERRIDE_INSTALLED;
    }
    {
        physics_counts_t pc;
        physics_counts_take(&pc);
        s->pairs = pc.pairs;
    }
    s->terrain_calls = perf_section_calls(PS_TERRAIN_UPDATE);
    s->human_idof = perf_section_calls(PS_PHYSICS_HUMAN_IDOF);
    active_ai_summary();
    s->world = read32(WORLD_VA);
    s->player = read32(PLAYER_VA);
    s->player_flags = s->player ? read32(s->player + PLAYER_FLAGS) : 0;
    {
        uint32_t renderer = read32(RENDER_SETTINGS_VA), settings = renderer ? read32(renderer) : 0;
        const uint8_t* b = settings ? (const uint8_t*)gm_ptr_checked(settings + DETAIL_REDUCE, 1, 0) : NULL;
        s->detail = b ? *b : 0xFFFFu;
    }
}

static void write_settings(const char* extra) {
    FILE* f = fopen(SETTINGS_FILE, "wb");
    if (!f) return;
    fprintf(f, "fov = 90\nactive_ai = enhanced\ndual_stow = on\nmax_object_detail = on\nofficial_patch_117 = on\n"
               "cheat_invulnerability = on\ncheat_infinite_ammo = on\n%s", extra ? extra : "");
    fclose(f);
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    lx_boot_options_t o;
    lx_boot_result_t r;
    state_t s;
    char* log;
    size_t log_end = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    memset(&o, 0, sizeof o);
    memset(&s, 0, sizeof s);
    o.game_dir = argc > 1 ? argv[1] : env && *env ? env : "game";
    o.version = "en-116";
    o.user_root = "build/linux_features_tests_user";
    o.settings_file = SETTINGS_FILE;
    o.use_features = 1;
    o.frame_hook = hook;
    o.hook_user = &s;
    s.frames = MENU_FRAMES + LEVEL_FRAMES;
    o.frame_limit = s.frames;
    remove(LOG_FILE);
    lx_log_open(NULL, LOG_FILE);
    write_settings("");

    lx_boot_headless(&o, &r);
    if (r.status == LX_BOOT_DATA_UNAVAILABLE) {
        printf("linux_features_tests: skipped - DATA UNAVAILABLE (no guest image under %s)\n", o.game_dir);
        printf("linux_features_tests: 1 checks, 0 failures\n");
        return 0;
    }
    printf("  New Game with the features on: %s: %s, %u of %u overrides installed, %u lift hooks\n", lx_boot_status_name(r.status), r.detail,
           r.overrides_installed, r.overrides_listed, r.lift_hooks);
    CHECK(r.status == LX_BOOT_FRAME_PRESENTED && s.evaluated, "the guest plays New Game into the first level with the features on: %s (%s)",
          lx_boot_status_name(r.status), r.detail);
    CHECK(r.overrides_listed == 81 && r.lift_hooks == 29, "lift116 carries 81 override sites and 29 lift hook sites (%u, %u)", r.overrides_listed, r.lift_hooks);
    CHECK(s.shared_installed == s.shared_total && s.shared_total == sizeof k_shared / sizeof k_shared[0],
          "every shared override is installed on the lifted game (%u of %u)", s.shared_installed, s.shared_total);
    CHECK(r.overrides_installed >= s.shared_installed, "the boot reports the installed count (%u)", r.overrides_installed);
    CHECK(s.world != 0 && s.player != 0, "a level is running: the world and Anne exist (%08X, %08X)", s.world, s.player);
    CHECK((s.player_flags & 0x100u) != 0, "the saved Invulnerability cheat reached the game's own CPlayer flags (%08X)", s.player_flags);
    CHECK(s.detail == 0, "Maximum object detail cleared the renderer's bDetailReduce (%u)", s.detail);
    CHECK(s.terrain_calls > 0, "CQuadRootTIN::Update ran through the terrain override (%llu calls)", s.terrain_calls);
    CHECK(s.human_idof > 0, "Anne's skeleton steps ran through the Pelvic_IDOF override: the crouch clamps and the hand carrier (%llu steps)", s.human_idof);
    CHECK(s.cheats_open == 1 && s.cheats_hits && s.cheats_rows > 3 && s.cheats_has_invulnerable,
          "F10 opened the Cheats panel over the running level and the frame was drawn with its rows (open %d, hit table %d, %d rows, Invulnerability %d)",
          s.cheats_open, s.cheats_hits, s.cheats_rows, s.cheats_has_invulnerable);
    CHECK(s.objectives_open == 2 && s.objectives_goals >= 2,
          "F3 opened Objectives with the level's own objectives, Escape having closed the Cheats panel first (%d, %d goals)", s.objectives_open, s.objectives_goals);
    CHECK(s.pairs > 0, "the broad-phase lift hook of the official Patch 1.1 ran inside the lifted physics (%u pairs)", s.pairs);
    log = read_log();
    CHECK(log != NULL, "the host log was written");
    if (log) {
        CHECK(strstr(log, "[patch117] applied") != NULL, "the official Patch 1.1 level fixes were applied to the level the LZ calls served");
        CHECK(strstr(log, "main camera") != NULL && strstr(log, "field of view 90") != NULL, "the main camera took the field of view of the settings");
        CHECK(strstr(log, "active AI: ") != NULL && strstr(log, "(limit 6)") != NULL,
              "Active AI ran on the game's own AI system with the limit of 6 animals");
        CHECK(strstr(log, "[cheats] active in") != NULL, "the cheats report what the session plays with");
        CHECK(strstr(log, "legacy 100 MB disk-space startup check bypassed") != NULL, "the legacy disk-space check was bypassed");
        log_end = strlen(log);
        free(log);
    }

    /* Skip Intro: the opening is answered through the game's own no-video path. */
    memset(&s, 0, sizeof s);
    s.frames = 20;
    o.frame_limit = s.frames;
    write_settings("skip_intro = on\n");
    lx_boot_headless(&o, &r);
    CHECK(r.status == LX_BOOT_FRAME_PRESENTED, "the guest still starts with Skip Intro on: %s (%s)", lx_boot_status_name(r.status), r.detail);
    log = read_log();
    CHECK(log && strlen(log) >= log_end && strstr(log + log_end, "tpassintro.smk skipped (Skip Intro)") != NULL, "Skip Intro answered the opening through the game's no-video branch");
    free(log);

    /* A language: the menu labels of the game's DDF files come from a translated copy in the cache, the game's text by string id. */
    memset(&s, 0, sizeof s);
    s.frames = MENU_FRAMES;
    o.frame_limit = s.frames;
    {
        FILE* f = fopen(SETTINGS_FILE, "wb");
        if (f) {
            fputs("language = fr\n", f);
            fclose(f);
        }
    }
    lx_boot_headless(&o, &r);
    CHECK(r.status == LX_BOOT_FRAME_PRESENTED, "the guest starts in French: %s (%s)", lx_boot_status_name(r.status), r.detail);
    log = read_log();
    CHECK(log && strlen(log) >= log_end && strstr(log + log_end, "[l10n] language fr: game text") != NULL,
          "the language's game text, subtitles and interface are loaded");
    CHECK(log && strstr(log + log_end, "mainscreen.ddf") == NULL, "the game opened its menu DDF through the translation layer without a failure");
    free(log);

    printf("linux_features_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
