/* One host interface at a time, headless: what each request closes and opens, the keys and buttons they keep from the
 * game, Objectives' own capture, and the navigation HUD hidden under every interface and the game's own text. */
#include <stdio.h>
#include <string.h>

#include "compass_policy.h"
#include "debug_menu_policy.h"
#include "jpt_gamepad.h"
#include "overlay_policy.h"

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

#define VK_F1_ 0x70u
#define VK_ESCAPE_ 0x1Bu
#define VK_RETURN_ 0x0Du

static int plan_is(ov_kind_t active, ov_kind_t request, int native, ov_kind_t close, ov_kind_t open) {
    ov_plan_t p = ov_plan(active, request, native);
    return p.close == close && p.open == open;
}

static void test_transitions(void) {
    CHECK(plan_is(OV_CONTROLS, OV_HINT, 0, OV_CONTROLS, OV_HINT), "Controls -> Hint: the card closes first");
    CHECK(plan_is(OV_CONTROLS, OV_OBJECTIVES, 0, OV_CONTROLS, OV_OBJECTIVES), "Controls -> Objectives");
    CHECK(plan_is(OV_OBJECTIVES, OV_HINT, 0, OV_OBJECTIVES, OV_HINT), "Objectives -> Hint");
    CHECK(plan_is(OV_NONE, OV_OBJECTIVES, 0, OV_NONE, OV_OBJECTIVES), "Hint closed -> Objectives");
    CHECK(plan_is(OV_OBJECTIVES, OV_CONTROLS, 0, OV_OBJECTIVES, OV_CONTROLS), "Objectives -> Controls");
    CHECK(plan_is(OV_CONTROLS, OV_DEBUG, 0, OV_CONTROLS, OV_DEBUG), "Controls -> Debug");
    CHECK(plan_is(OV_DEBUG, OV_OBJECTIVES, 0, OV_DEBUG, OV_OBJECTIVES), "Debug -> Objectives");
    CHECK(plan_is(OV_CHEATS, OV_OBJECTIVES, 0, OV_CHEATS, OV_OBJECTIVES), "Cheats -> Objectives");
    CHECK(plan_is(OV_OBJECTIVES, OV_OBJECTIVES, 0, OV_OBJECTIVES, OV_NONE), "F3 again closes Objectives");
    CHECK(plan_is(OV_CONTROLS, OV_CONTROLS, 0, OV_CONTROLS, OV_NONE), "F2 again closes Controls");
    CHECK(plan_is(OV_CONTROLS, OV_NONE, 0, OV_CONTROLS, OV_NONE), "Esc, B or Menu over Controls only closes it");
    CHECK(plan_is(OV_NONE, OV_HINT, 0, OV_NONE, OV_HINT), "D-pad Left with nothing up: the game's Hint at once");
    CHECK(plan_is(OV_NONE, OV_CONTROLS, 1, OV_NONE, OV_NONE), "nothing of the host opens over the Hint or the pause menu");
    CHECK(plan_is(OV_NONE, OV_HINT, 1, OV_NONE, OV_NONE), "no second Hint over a window of the game");
    for (int a = OV_NONE; a <= OV_CHEATS; a++)
        for (int r = OV_NONE; r <= OV_HINT; r++) {
            ov_plan_t p = ov_plan((ov_kind_t)a, (ov_kind_t)r, 0);
            CHECK(p.open == OV_NONE || p.close == a, "%s -> %s: whatever was up closes before another opens",
                  ov_name((ov_kind_t)a), ov_name((ov_kind_t)r));
        }
}

static void test_keys(void) {
    ov_keys_t k;
    memset(&k, 0, sizeof k);
    ov_keys_take(&k, VK_F1_);
    CHECK(ov_keys_filter(&k, VK_F1_, 1) && ov_keys_filter(&k, VK_F1_, 0), "F1 that closed the host's: repeats and release kept");
    CHECK(!ov_keys_filter(&k, VK_F1_, 1), "the next F1 is the game's again");
    ov_keys_take(&k, VK_ESCAPE_);
    CHECK(ov_keys_filter(&k, VK_ESCAPE_, 0), "Esc that closed Controls: its release does not open the pause menu");
    CHECK(!ov_keys_filter(&k, VK_ESCAPE_, 1), "a new Esc opens it");
}

static dm_world_t level_world(void) {
    dm_world_t w;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_LEVEL;
    snprintf(w.scene, sizeof w.scene, "be.scn");
    dm_level_id(w.scene, w.level, sizeof w.level);
    w.player = w.eye = 1;
    w.goals.available = 1;
    return w;
}

static void test_objectives_capture(void) {
    dm_state_t s;
    dm_world_t w = level_world();
    dm_action_t a;
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_SOUTH), 1000);
    CHECK(!s.open && !dm_take(&s, &a), "A on Close closes Objectives and asks for nothing");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_START), 1000);
    CHECK(!s.open, "Menu closes Objectives (the pause menu does not open with it)");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    CHECK(dm_key(&s, &w, VK_ESCAPE_, 1, 0, 0, 0, 1000) && !s.open, "Esc closes Objectives");
    CHECK(dm_key(&s, &w, VK_ESCAPE_, 0, 0, 0, 0, 1000), "its release is kept from the game");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    CHECK(dm_key(&s, &w, 'W', 1, 0, 0, 0, 1000) && dm_key(&s, &w, ' ', 1, 0, 0, 0, 1000) &&
              dm_key(&s, &w, 'Q', 1, 0, 0, 0, 1000),
          "every gameplay key goes to Objectives while it is open");
    dm_key(&s, &w, VK_RETURN_, 1, 0, 0, 0, 1000);
    CHECK(!s.open && dm_key(&s, &w, 'W', 0, 0, 0, 0, 1000) && dm_key(&s, &w, 'Q', 0, 0, 0, 0, 1000),
          "Enter on Close; the keys pressed meanwhile are released to nobody");
}

static void test_hud_hidden(void) {
    static const char* const why[] = { "Hint, pause menu, dialogs, videos, main menu (no level in front)",
                                       "Controls", "Objectives, Debug Menu, Cheats", "loading",
                                       "a tutorial line or any game text" };
    cp_scene_t on;
    memset(&on, 0, sizeof on);
    on.level = on.player = 1;
    CHECK(cp_hud_allowed(&on), "gameplay: the navigation HUD");
    for (int i = 0; i < 5; i++) {
        cp_scene_t s = on;
        if (i == 0) s.level = 0;
        if (i == 1) s.card = 1;
        if (i == 2) s.menu = 1;
        if (i == 3) s.loading = 1;
        if (i == 4) s.game_text = 1;
        CHECK(!cp_hud_allowed(&s), "hidden: %s", why[i]);
    }
}

static void test_spacing(void) {
    static const float shapes[5][2] = { { 1440, 1080 }, { 1920, 1080 }, { 1728, 1080 }, { 2520, 1080 }, { 3840, 1080 } };
    for (int i = 0; i < 5; i++) {
        cp_layout_t l;
        cp_layout(shapes[i][0], shapes[i][1], &l);
        CHECK(l.text_y >= l.top + l.height && l.objective_y >= l.text_y + l.small_px * 1.3f &&
                  l.notice_y >= l.objective_y + l.objective_px * 1.2f,
              "%gx%g: strip, distance, objective and notice never overlap", shapes[i][0], shapes[i][1]);
        CHECK(l.objective_px >= l.small_px * 1.2f, "the objective line is a quarter larger than the distance");
    }
}

void overlay_tests(int* checks, int* failures) {
    test_transitions();
    test_keys();
    test_objectives_capture();
    test_hud_hidden();
    test_spacing();
    *checks += g_checks;
    *failures += g_failures;
}
