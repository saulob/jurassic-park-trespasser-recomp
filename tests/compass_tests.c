/* The compass and the objective markers, headless: headings, bearings, distance, projection, the trail's progress,
 * what the HUD shows, the F5 and F3 panels and where the strip goes on each screen shape. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats_policy.h"
#include "compass_policy.h"
#include "debug_menu_policy.h"
#include "host_policy.h"
#include "jpt_gamepad.h"

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

#define PI_F 3.14159265f
#define VK_ESCAPE_ 0x1Bu

static int near(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

/* A turn about Z, as the game's CRotate3 stores it (scalar first). */
static void yaw_rotation(float yaw, float rot[4]) {
    rot[0] = cosf(yaw * 0.5f);
    rot[1] = rot[2] = 0.0f;
    rot[3] = sinf(yaw * 0.5f);
}

static dm_world_t level_world(const char* scene) {
    dm_world_t w;
    memset(&w, 0, sizeof w);
    w.place = DM_PLACE_LEVEL;
    snprintf(w.scene, sizeof w.scene, "%s", scene);
    dm_level_id(w.scene, w.level, sizeof w.level);
    w.player = 1;
    w.at.rot[0] = 1.0f;
    w.eye = 1;
    w.eye_at.rot[0] = 1.0f;
    w.ai = 0;
    w.camera_head = 1;
    w.speed = w.jump = CH_MULTIPLIER_DEFAULT;
    return w;
}

static cp_scene_t gameplay(void) {
    cp_scene_t s;
    memset(&s, 0, sizeof s);
    s.level = s.player = 1;
    return s;
}

static void press(dm_state_t* s, const dm_world_t* w, unsigned vk) {
    dm_key(s, w, vk, 1, 0, 0, 0, 1000);
    dm_key(s, w, vk, 0, 0, 0, 0, 1000);
}

static void test_headings(void) {
    static const struct {
        float yaw;
        const char* name;
        float heading;
    } dirs[] = {
        { 0.0f, "N", 0.0f },          { -PI_F / 4, "NE", 45.0f }, { -PI_F / 2, "E", 90.0f },
        { -3 * PI_F / 4, "SE", 135.0f }, { PI_F, "S", 180.0f },     { 3 * PI_F / 4, "SW", 225.0f },
        { PI_F / 2, "W", 270.0f },     { PI_F / 4, "NW", 315.0f },
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        float rot[4], heading;
        const char* name;
        yaw_rotation(dirs[i].yaw, rot);
        heading = cp_heading(dm_yaw(rot));
        name = cp_direction_name((int)lroundf(heading) % 360);
        CHECK(near(cp_wrap180(heading - dirs[i].heading), 0.0f, 0.01f) && name && !strcmp(name, dirs[i].name),
              "yaw %.3f faces %s (heading %.2f, %s)", dirs[i].yaw, dirs[i].name, heading, name ? name : "-");
    }
    {
        float rot[4];
        yaw_rotation(-PI_F / 2, rot);
        CHECK(near(cp_heading(dm_yaw(rot)), 90.0f, 0.01f), "facing +X (east) is a quarter turn clockwise from north");
    }
    CHECK(near(cp_wrap360(-1.0f), 359.0f, 1e-4f) && cp_wrap360(360.0f) == 0.0f && near(cp_wrap360(725.0f), 5.0f, 1e-4f),
          "wrap to [0, 360)");
    CHECK(cp_wrap180(180.0f) == 180.0f && cp_wrap180(-180.0f) == 180.0f && near(cp_wrap180(190.0f), -170.0f, 1e-4f) &&
              near(cp_wrap180(-190.0f), 170.0f, 1e-4f),
          "wrap to (-180, 180]");
    CHECK(!cp_direction_name(30) && !strcmp(cp_direction_name(-45), "NW") && !strcmp(cp_direction_name(360), "N"),
          "direction names only on multiples of 45");
    {
        ch_freecam_t cam = { { 0.0f, 0.0f, 0.0f }, -PI_F / 2, 0.4f };
        dm_placement_t at;
        ch_freecam_placement(&cam, &at);
        CHECK(near(cp_heading(dm_yaw(at.rot)), 90.0f, 0.05f), "a pitched Free Camera keeps its heading");
    }
}

static void test_wrap_and_ticks(void) {
    cp_layout_t l;
    cp_tick_t ticks[CP_TICKS_MAX];
    float last_x = -1.0f, worst = 0.0f;
    int found_all = 1;
    cp_layout(1920.0f, 1080.0f, &l);
    for (float h = 350.0f; h <= 370.0f; h += 0.1f) {
        float heading = cp_wrap360(h), x = -1.0f;
        int n = cp_ticks(heading, ticks, CP_TICKS_MAX), side;
        for (int i = 0; i < n; i++)
            if (ticks[i].name && !strcmp(ticks[i].name, "N")) x = cp_strip_x(&l, ticks[i].relative, &side);
        if (x < 0.0f) found_all = 0;
        if (last_x >= 0.0f && x >= 0.0f && fabsf(x - last_x) > worst) worst = fabsf(x - last_x);
        last_x = x;
    }
    CHECK(found_all && worst < 1.0f, "N slides smoothly through 359 -> 0 (largest step %.3f px per 0.1 deg)", worst);
    {
        int n = cp_ticks(0.0f, ticks, CP_TICKS_MAX), names = 0, cardinals = 0, inside = 1;
        for (int i = 0; i < n; i++) {
            names += ticks[i].name != NULL;
            cardinals += ticks[i].cardinal;
            inside &= fabsf(ticks[i].relative) <= CP_HALF_SPAN_DEG;
        }
        CHECK(n == 13 && names == 5 && cardinals == 3 && inside,
              "facing north: ticks every 15 deg over +-100, W NW N NE E named (%d ticks, %d names)", n, names);
    }
    {
        int n = cp_ticks(359.5f, ticks, CP_TICKS_MAX), n_ok = 0;
        for (int i = 0; i < n; i++)
            if (ticks[i].name && !strcmp(ticks[i].name, "N")) n_ok = near(ticks[i].relative, 0.5f, 1e-3f);
        CHECK(n_ok, "at 359.5, N is half a degree to the right");
    }
}

static void test_bearings(void) {
    const float me[3] = { 100.0f, 200.0f, 5.0f };
    cp_layout_t l;
    int side;
    float x;
    cp_layout(1920.0f, 1080.0f, &l);
    {
        const float ahead[3] = { 100.0f, 260.0f, 40.0f }, right[3] = { 160.0f, 200.0f, 0.0f },
                    left[3] = { 40.0f, 200.0f, 0.0f }, behind[3] = { 100.0f, 140.0f, 0.0f };
        CHECK(near(cp_relative(cp_bearing(me, ahead), 0.0f), 0.0f, 1e-3f), "facing north, north is ahead");
        CHECK(near(cp_relative(cp_bearing(me, right), 0.0f), 90.0f, 1e-3f), "east is 90 to the right");
        CHECK(near(cp_relative(cp_bearing(me, left), 0.0f), -90.0f, 1e-3f), "west is 90 to the left");
        CHECK(near(fabsf(cp_relative(cp_bearing(me, behind), 0.0f)), 180.0f, 1e-3f), "south is behind");
        CHECK(near(cp_relative(cp_bearing(me, ahead), 90.0f), -90.0f, 1e-3f), "facing east, north is to the left");
        CHECK(near(cp_relative(cp_bearing(me, behind), 270.0f), -90.0f, 1e-3f), "facing west, south is to the left");
    }
    CHECK(near(cp_relative(10.0f, 350.0f), 20.0f, 1e-3f) && near(cp_relative(350.0f, 10.0f), -20.0f, 1e-3f),
          "relative bearings cross north the short way");
    x = cp_strip_x(&l, 0.0f, &side);
    CHECK(side == 0 && near(x, 960.0f, 1e-3f), "ahead: the centre");
    x = cp_strip_x(&l, 90.0f, &side);
    CHECK(side == 0 && x > l.centre + 0.8f * (l.right - l.centre) && x < l.right, "90 right: inside, near the right end");
    x = cp_strip_x(&l, -90.0f, &side);
    CHECK(side == 0 && x < l.centre - 0.8f * (l.centre - l.left) && x > l.left, "90 left: inside, near the left end");
    x = cp_strip_x(&l, 135.0f, &side);
    CHECK(side == 1 && x == l.right, "behind on the right: pinned at the right end");
    x = cp_strip_x(&l, -170.0f, &side);
    CHECK(side == -1 && x == l.left, "behind on the left: pinned at the left end");
    x = cp_strip_x(&l, 180.0f, &side);
    CHECK(side == 1, "straight behind: the right end, always the same one");
    CHECK(cp_edge_fade(0.0f) == 1.0f && cp_edge_fade(CP_HALF_SPAN_DEG) == 0.0f && cp_edge_fade(85.0f) > 0.0f &&
              cp_edge_fade(85.0f) < 1.0f,
          "ticks fade out at the ends");
}

static void test_distance(void) {
    const float a[3] = { 0.0f, 0.0f, 0.0f }, b[3] = { 3.0f, 4.0f, 100.0f };
    char text[32];
    CHECK(CP_METRES_PER_UNIT == 1.0f, "world units are metres");
    CHECK(near(cp_distance_m(a, b), 5.0f, 1e-5f), "distance is on the ground: 100 m of height do not count");
    CHECK(cp_format_distance(0.4f, text, sizeof text) && !strcmp(text, "0 m"), "%s", text);
    CHECK(cp_format_distance(124.4f, text, sizeof text) && !strcmp(text, "124 m"), "%s", text);
    CHECK(cp_format_distance(999.4f, text, sizeof text) && !strcmp(text, "999 m"), "%s", text);
    CHECK(cp_format_distance(1234.0f, text, sizeof text) && !strcmp(text, "1.2 km"), "%s", text);
    CHECK(!cp_format_distance(1234.0f, text, 4), "a short buffer is refused");
}

static cp_camera_t camera_facing(float yaw) {
    cp_camera_t c;
    memset(&c, 0, sizeof c);
    yaw_rotation(yaw, c.rot);
    c.tan_half_h = 0.5f;
    return c;
}

static void test_projection(void) {
    cp_camera_t c = camera_facing(0.0f);
    cp_screen_t p;
    const float ahead[3] = { 0.0f, 50.0f, 0.0f }, right[3] = { 20.0f, 50.0f, 0.0f }, left[3] = { -20.0f, 50.0f, 0.0f },
                above[3] = { 0.0f, 50.0f, 10.0f }, far_right[3] = { 100.0f, 10.0f, 0.0f },
                behind_left[3] = { -30.0f, -40.0f, 0.0f }, behind[3] = { 0.0f, -40.0f, 0.0f };
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, ahead, &p) && p.on_screen && near(p.x, 960.0f, 1e-2f) &&
              near(p.y, 540.0f, 1e-2f) && near(p.depth, 50.0f, 1e-3f),
          "straight ahead: the picture's centre");
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, right, &p) && p.on_screen && near(p.x, 960.0f + 540.0f / 0.5f * 0.4f, 0.05f),
          "to the right: right of the centre by focal x 20/50 (%.2f)", p.x);
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, left, &p) && p.on_screen && p.x < 960.0f, "to the left");
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, above, &p) && p.on_screen && p.y < 540.0f, "above is up the screen");
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, far_right, &p) && !p.on_screen && near(p.x, 1880.0f, 1e-2f) &&
              p.dir_x > 0.9f,
          "off to the right: pinned at the right edge, pointing right");
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, behind_left, &p) && !p.on_screen && p.depth < 0.0f && p.dir_x < -0.9f &&
              near(p.x, 40.0f, 1e-2f),
          "behind on the left: the left edge, pointing left (turn left)");
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, behind, &p) && !p.on_screen && p.dir_y > 0.9f && near(p.y, 1040.0f, 1e-2f),
          "straight behind: the bottom edge");
    c = camera_facing(-PI_F / 2);
    CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, ahead, &p) && !p.on_screen && p.dir_x < -0.9f,
          "facing east, a point north is to the left");
    {
        const float east[3] = { 50.0f, 0.0f, 0.0f };
        CHECK(cp_project(&c, 1920.0f, 1080.0f, 40.0f, east, &p) && p.on_screen && near(p.x, 960.0f, 1e-2f),
              "facing east, east is the centre");
    }
    c.tan_half_h = 0.0f;
    CHECK(!cp_project(&c, 1920.0f, 1080.0f, 40.0f, ahead, &p), "no view: nothing projected");
    {
        static const float shapes[5][2] = { { 1440, 1080 }, { 1920, 1080 }, { 1728, 1080 }, { 2520, 1080 }, { 3840, 1080 } };
        cp_camera_t d = camera_facing(0.0f);
        int ok = 1;
        for (int i = 0; i < 5; i++) {
            ok &= cp_project(&d, shapes[i][0], shapes[i][1], 40.0f, right, &p) && p.on_screen &&
                  near(p.x - shapes[i][0] * 0.5f, 432.0f, 0.05f) && near(p.y, 540.0f, 1e-2f);
        }
        CHECK(ok, "4:3 to 32:9: the vertical view is fixed, so a point lands as far from the centre on each");
    }
}

static void test_route(void) {
    const float route[4][3] = { { 0, 100, 0 }, { 0, 200, 0 }, { 50, 300, 0 }, { 50, 400, 0 } };
    const float start[3] = { 0, 0, 0 }, near_first[3] = { 3, 97, 0 }, between[3] = { 0, 170, 0 }, beyond[3] = { 50, 395, 0 };
    CHECK(cp_route_next(route, 4, start) == 0, "from the start: the first anchor");
    CHECK(cp_route_next(route, 4, near_first) == 1, "at the first anchor: the second is next");
    CHECK(cp_route_next(route, 4, between) == 1, "between two anchors: the one ahead");
    CHECK(cp_route_next(route, 4, beyond) == 4, "past the last anchor: nothing left before the target");
    CHECK(cp_route_next(route, 0, start) == 0, "no route");
}

static cp_goal_t goal_at(float x, float y) {
    cp_goal_t g;
    memset(&g, 0, sizeof g);
    g.set = g.target = 1;
    g.pos[0] = x;
    g.pos[1] = y;
    snprintf(g.title, sizeof g.title, "Reach the next area");
    return g;
}

static void test_hud(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    cp_scene_t scene = gameplay();
    cp_hud_t hud;
    cp_goal_t goal = goal_at(0.0f, 184.0f);
    w.tan_half_h = 0.5f;
    dm_init(&s);
    dm_navigate(&s, &w, &scene, NULL, 1000, &hud);
    CHECK(hud.compass && !hud.marker && hud.alpha == 1.0f && near(hud.heading, 0.0f, 1e-3f) && !hud.objective[0],
          "no objective: the compass alone");
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.marker && near(hud.relative, 0.0f, 1e-3f) && !strcmp(hud.distance, "184 m") && !hud.objective[0] &&
              hud.view && hud.camera.tan_half_h == 0.5f,
          "an objective ahead: marker in the middle, 184 m, no title unless asked (%s)", hud.distance);
    {
        static const char* const why[] = { "F3/F5/F10", "F2 card", "loading", "no level (F1 Hint, pause, dialogs, videos)" };
        for (int i = 0; i < 4; i++) {
            cp_scene_t no = scene;
            if (i == 0) no.menu = 1;
            if (i == 1) no.card = 1;
            if (i == 2) no.loading = 1;
            if (i == 3) no.level = 0;
            dm_navigate(&s, &w, &no, &goal, 1000, &hud);
            CHECK(!hud.compass && !hud.marker && !hud.objective[0] && !hud.notice[0], "hidden: %s", why[i]);
        }
    }
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.compass && hud.marker, "back when it closes");
    scene.game_text = 1;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(!hud.compass && hud.marker && hud.view, "a gameplay prompt hides the compass, not the world marker");
    scene.game_text = 0;
    yaw_rotation(-PI_F / 2, w.eye_at.rot);
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(near(hud.relative, -90.0f, 1e-3f), "facing east, an objective north: left");
    yaw_rotation(0.0f, w.eye_at.rot);
    goal.pos[1] = -100.0f;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(near(fabsf(hud.relative), 180.0f, 1e-3f) && !strcmp(hud.distance, "100 m"), "behind: 180, pinned at an end");
    {
        dm_world_t free_camera = w;
        free_camera.camera = CH_CAMERA_FREE;
        free_camera.eye_at.pos[1] = -300.0f;
        dm_navigate(&s, &free_camera, &scene, &goal, 1000, &hud);
        CHECK(near(hud.relative, 0.0f, 1e-3f) && !strcmp(hud.distance, "100 m"),
              "Free Camera: direction from the camera, distance still Anne's");
    }
    goal = goal_at(0.0f, 400.0f);
    goal.route_count = 3;
    goal.route[0][1] = 100.0f;
    goal.route[1][1] = 200.0f;
    goal.route[2][1] = 300.0f;
    w.at.pos[1] = 95.0f;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.trail_count == 1 && hud.trail[0][1] == 200.0f,
          "the trail starts past the anchor Anne reached, within %.0f m (%d)", CP_TRAIL_RANGE_M, hud.trail_count);
    w.at.pos[1] = 195.0f;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.trail_count == 1 && hud.trail[0][1] == 300.0f, "and moves on as she does");
    w.at.pos[1] = 0.0f;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.trail_count == 1 && hud.trail[0][1] == 100.0f, "only anchors within %.0f m", CP_TRAIL_RANGE_M);
    goal.route_count = 0;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.trail_count == 0 && hud.marker, "no reliable route: no trail, the markers stay");
    goal.target = 0;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(!hud.marker && !hud.objective[0] && hud.compass, "no target: the compass alone");
    dm_notice(&s, "Objective updated", 2000);
    dm_navigate(&s, &w, &scene, &goal, 2000, &hud);
    CHECK(!strcmp(hud.notice, "Objective updated"), "notice");
    dm_navigate(&s, &w, &scene, &goal, 2000 + DM_FEEDBACK_MS + 1, &hud);
    CHECK(!hud.notice[0], "the notice goes away");
    w.eye = 0;
    dm_navigate(&s, &w, &scene, &goal, 3000, &hud);
    CHECK(!hud.compass, "no camera: no strip");
}

/* Show Objective: the title for CP_REMINDER_MS, restarted by a press or a new objective, ended by none. */
static void test_reminder(void) {
    cp_reminder_t r;
    cp_goal_t goal = goal_at(0.0f, 100.0f), other = goal_at(0.0f, 100.0f);
    cp_hud_t hud;
    snprintf(other.title, sizeof other.title, "Find a way through");
    memset(&r, 0, sizeof r);
    CHECK(CP_REMINDER_MS == 4000u && CP_REMINDER_FADE_MS < CP_REMINDER_MS && CP_NO_OBJECTIVE_MS <= 2000u,
          "4 seconds, a short fade, at most 2 seconds for the no-objective notice");
    CHECK(cp_reminder_step(&r, 1000, &goal) == 0.0f, "nothing shows until it is asked for");
    CHECK(cp_reminder_press(&r, 1000, &goal) == 1 && cp_reminder_step(&r, 1000, &goal) == 1.0f, "a press shows it");
    CHECK(cp_reminder_step(&r, 1000 + CP_REMINDER_MS - CP_REMINDER_FADE_MS, &goal) == 1.0f, "fully visible until the fade");
    {
        float half = cp_reminder_step(&r, 1000 + CP_REMINDER_MS - CP_REMINDER_FADE_MS / 2, &goal);
        CHECK(near(half, 0.5f, 0.01f), "fading over the last %u ms (%.2f)", CP_REMINDER_FADE_MS, half);
    }
    CHECK(cp_reminder_step(&r, 1000 + CP_REMINDER_MS, &goal) == 0.0f && !r.active, "gone after 4 seconds");
    CHECK(cp_reminder_step(&r, 1000 + CP_REMINDER_MS + 50, &goal) == 0.0f, "and stays gone without a press");
    cp_reminder_press(&r, 10000, &goal);
    cp_reminder_press(&r, 13000, &goal);
    CHECK(cp_reminder_step(&r, 16000, &goal) == 1.0f && cp_reminder_step(&r, 17001, &goal) == 0.0f,
          "a press while it shows restarts the 4 seconds");
    cp_reminder_press(&r, 20000, &goal);
    CHECK(cp_reminder_step(&r, 22000, &other) == 1.0f && !strcmp(r.title, "Find a way through"),
          "a new objective shows at once");
    CHECK(cp_reminder_step(&r, 25500, &other) == 1.0f && cp_reminder_step(&r, 26001, &other) == 0.0f,
          "and restarts the timer (%u ms)", CP_REMINDER_MS);
    cp_reminder_press(&r, 30000, &goal);
    CHECK(cp_reminder_step(&r, 31000, NULL) == 0.0f && !r.active, "the objective gone: nothing left to show");
    memset(&goal, 0, sizeof goal);
    CHECK(cp_reminder_press(&r, 40000, &goal) == 0 && cp_reminder_press(&r, 40000, NULL) == 0 && !r.active,
          "no objective: no false text");
    goal = goal_at(0.0f, 100.0f);
    memset(&hud, 0, sizeof hud);
    cp_hud_reminder(&hud, &goal, 1.0f);
    CHECK(!hud.objective[0], "the HUD not allowed: no title");
    hud.alpha = 1.0f;
    cp_hud_reminder(&hud, &goal, 0.0f);
    CHECK(!hud.objective[0], "not asked for: no title");
    cp_hud_reminder(&hud, &goal, 0.4f);
    CHECK(!strcmp(hud.objective, "Reach the next area") && near(hud.objective_alpha, 0.4f, 1e-6f), "asked for: its title");
}

/* Where the game's text sits decides whether it covers the HUD: voice subtitles never do. */
static void test_text_kinds(void) {
    const int raster_h = 480;
    CHECK(cp_text_kind(CP_TEXT_CATEGORY_SUBTITLE, 0, 420, raster_h) == CP_TEXT_SUBTITLE &&
              !cp_text_hides_hud(CP_TEXT_SUBTITLE),
          "a voice subtitle (category 2, at the bottom) does not hide the HUD");
    CHECK(cp_text_kind(CP_TEXT_CATEGORY_SUBTITLE, 0, 8, raster_h) == CP_TEXT_SUBTITLE,
          "category 2 is a subtitle wherever it lands");
    CHECK(cp_text_kind(3, 0, 8, raster_h) == CP_TEXT_GAME_PROMPT && cp_text_hides_hud(CP_TEXT_GAME_PROMPT),
          "a trigger text from the top (the tutorial's prompts) hides it");
    CHECK(cp_text_kind(0, 0, 40, raster_h) == CP_TEXT_GAME_PROMPT, "any other top text hides it");
    CHECK(cp_text_kind(3, 0, 400, raster_h) == CP_TEXT_MESSAGE && !cp_text_hides_hud(CP_TEXT_MESSAGE),
          "a small message at the bottom does not");
    CHECK(cp_text_kind(0x4A51u, 1, 400, raster_h) == CP_TEXT_HOST_PROMPT && cp_text_hides_hud(CP_TEXT_HOST_PROMPT),
          "the port's own prompt hides it");
    CHECK(cp_text_kind(3, 0, (int)(CP_TEXT_ZONE * (float)raster_h) - 1, raster_h) == CP_TEXT_GAME_PROMPT &&
              cp_text_kind(3, 0, (int)(CP_TEXT_ZONE * (float)raster_h) + 1, raster_h) == CP_TEXT_MESSAGE,
          "the HUD's zone is the top %.0f%% of the picture", CP_TEXT_ZONE * 100.0f);
}

/* The scene as the HUD sees it: a subtitle leaves scene.game_text clear, a prompt sets it, and the HUD follows. */
static void test_hud_text(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn");
    cp_scene_t scene = gameplay();
    cp_hud_t hud;
    cp_goal_t goal = goal_at(0.0f, 184.0f);
    w.tan_half_h = 0.5f;
    goal.route_count = 1;
    goal.route[0][1] = 20.0f;
    dm_init(&s);
    scene.game_text = cp_text_hides_hud(cp_text_kind(CP_TEXT_CATEGORY_SUBTITLE, 0, 430, 480));
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.compass && hud.marker && hud.trail_count == 1 && hud.distance[0], "a subtitle: compass, marker, trail stay");
    scene.game_text = cp_text_hides_hud(cp_text_kind(3, 0, 10, 480));
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(!hud.compass && hud.marker && hud.trail_count == 1 && hud.distance[0],
          "a tutorial prompt hides the compass only: marker, trail and distance stay");
    scene.game_text = 0;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(hud.compass && hud.marker && hud.trail_count == 1, "and back when it goes");
    scene.menu = 1;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(!hud.compass && !hud.marker && !hud.trail_count, "a menu hides it");
    cp_hud_reminder(&hud, &goal, 1.0f);
    CHECK(!hud.objective[0], "the title stays out while the HUD is hidden");
    scene.menu = 0;
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    cp_hud_reminder(&hud, &goal, 1.0f);
    CHECK(hud.compass && hud.marker && hud.objective[0], "the title over the compass once it is back");
    dm_notice_for(&s, "No objective", 1000, CP_NO_OBJECTIVE_MS);
    dm_navigate(&s, &w, &scene, &goal, 1000, &hud);
    CHECK(!strcmp(hud.notice, "No objective"), "no objective: a small notice");
    dm_navigate(&s, &w, &scene, &goal, 1000 + CP_NO_OBJECTIVE_MS, &hud);
    CHECK(!hud.notice[0], "gone after at most 2 seconds");
}

static void test_menus(void) {
    dm_state_t s;
    dm_world_t w = level_world("be.scn"), menu;
    dm_row_t rows[DM_ROWS_MAX];
    int n, waypoint = 0, goals = 0, notes = 0, unknown = 0, current = -1;
    dm_init(&s);
    dm_toggle(&s, &w, DM_MENU_CHEATS);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) waypoint |= strstr(rows[i].text, "aypoint") != NULL || strstr(rows[i].text, "Compass") != NULL;
    CHECK(!waypoint, "nothing of the compass or a waypoint in F10 Cheats");
    w.goals.available = 1;
    w.goals.count = 3;
    w.goals.item[0].status = DM_GOAL_DONE;
    snprintf(w.goals.item[0].title, DM_LINE_MAX, "Explore the beach");
    w.goals.item[1].status = DM_GOAL_CURRENT;
    snprintf(w.goals.item[1].title, DM_LINE_MAX, "Head inland");
    snprintf(w.goals.item[1].hint, DM_LINE_MAX, "Nothing to do but explore inward.");
    w.goals.item[2].status = DM_GOAL_UNKNOWN;
    snprintf(w.goals.item[2].title, DM_LINE_MAX, "Spoiler");
    CHECK(dm_toggle(&s, &w, DM_MENU_OBJECTIVES) && s.open && s.menu == DM_MENU_OBJECTIVES && s.cursor == DM_CLOSE,
          "F3 replaces F10");
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    for (int i = 0; i < n; i++) {
        if (rows[i].kind == DM_ROW_GOAL) goals++;
        if (rows[i].kind == DM_ROW_NOTE) notes++;
        if (rows[i].kind == DM_ROW_GOAL && rows[i].item == DM_GOAL_UNKNOWN) unknown = !strcmp(rows[i].text, "???");
        if (rows[i].kind == DM_ROW_GOAL && rows[i].item == DM_GOAL_CURRENT) current = i;
        CHECK(!strstr(rows[i].text, "Spoiler"), "upcoming objectives are not revealed");
    }
    CHECK(!strcmp(rows[0].text, "Objectives") && !strcmp(rows[1].text, "BE - Beach") && goals == 3 && notes == 1 &&
              unknown && current > 0 && rows[current + 1].kind == DM_ROW_NOTE && rows[n - 1].item == DM_CLOSE,
          "Objectives: level, done, current with the game's hint under it, ??? for the rest, Close");
    press(&s, &w, VK_ESCAPE_);
    CHECK(!s.open, "Esc closes Objectives");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_EAST), 1000);
    CHECK(!s.open, "B closes Objectives");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    dm_pad(&s, &w, JPT_PAD_BIT(JPT_PAD_OBJECTIVES_CONTROL), 1000);
    CHECK(!s.open, "its own D-pad shortcut closes it");
    CHECK(dm_toggle(&s, &w, DM_MENU_OBJECTIVES) && dm_toggle(&s, &w, DM_MENU_OBJECTIVES) && !s.open, "F3 toggles");
    w.goals.available = 0;
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    n = dm_rows(&s, &w, rows, DM_ROWS_MAX);
    CHECK(n >= 3 && rows[2].kind == DM_ROW_NOTE && !strcmp(rows[2].text, "Objectives unavailable for this level"),
          "a level without objectives says so");
    dm_close(&s);
    memset(&menu, 0, sizeof menu);
    menu.place = DM_PLACE_MAIN_MENU;
    CHECK(!dm_toggle(&s, &menu, DM_MENU_OBJECTIVES) && !s.open, "no Objectives in the main menu");
    dm_toggle(&s, &w, DM_MENU_OBJECTIVES);
    dm_world_seen(&s, &menu);
    CHECK(!s.open, "it closes when the level is left");
}

static void test_layout(void) {
    static const struct {
        const char* name;
        float w, h;
    } shapes[] = { { "4:3", 1440.0f, 1080.0f },   { "16:9", 1920.0f, 1080.0f }, { "16:10", 1728.0f, 1080.0f },
                   { "21:9", 2520.0f, 1080.0f },  { "32:9", 3840.0f, 1080.0f }, { "4:3 small", 640.0f, 480.0f },
                   { "16:9 4K", 3840.0f, 2160.0f } };
    cp_layout_t ref;
    cp_layout(1920.0f, 1080.0f, &ref);
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        cp_layout_t l;
        float width;
        cp_layout(shapes[i].w, shapes[i].h, &l);
        width = l.right - l.left;
        CHECK(near(l.centre, shapes[i].w * 0.5f, 1e-3f) && near(l.centre - l.left, l.right - l.centre, 1e-3f),
              "%s: centred on the picture", shapes[i].name);
        CHECK(l.left > 0.0f && l.right < shapes[i].w && l.top > 0.0f && l.notice_y + l.small_px < shapes[i].h * 0.2f,
              "%s: inside the top of the picture", shapes[i].name);
        CHECK(width <= 0.44f * shapes[i].w + 1e-3f && width <= 0.55f * shapes[i].h + 1e-3f,
              "%s: short (%.0f px of %.0f)", shapes[i].name, width, shapes[i].w);
        CHECK(l.height > 0.0f && l.label_px >= 11.0f && l.small_px >= 10.0f && l.line_px >= 1.0f &&
                  l.text_y > l.top + l.height && l.notice_y > l.text_y,
              "%s: rows in order", shapes[i].name);
        if (shapes[i].h == 1080.0f && shapes[i].w >= 1728.0f)
            CHECK(near(width, ref.right - ref.left, 1e-3f) && near(l.height, ref.height, 1e-3f),
                  "%s: as wide as 16:9 at the same height", shapes[i].name);
    }
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        cp_layout_t l;
        float title_w;
        cp_layout(shapes[i].w, shapes[i].h, &l);
        title_w = 38.0f * 0.6f * l.objective_px;
        CHECK(l.objective_y >= l.text_y + l.small_px * 1.2f && l.notice_y >= l.objective_y + l.objective_px * 1.2f &&
                  l.objective_px >= l.small_px && l.objective_px <= l.small_px * 1.6f,
              "%s: the title sits below the compass and the distance, the notice below it", shapes[i].name);
        CHECK(title_w < shapes[i].w * 0.9f && cp_outline_px(&l) >= 1.0f && cp_outline_px(&l) <= 0.006f * shapes[i].h + 1.0f,
              "%s: the longest title fits (%.0f px of %.0f), outline %.1f px", shapes[i].name, title_w, shapes[i].w,
              cp_outline_px(&l));
    }
    {
        cp_layout_t l;
        int side;
        cp_layout(1440.0f, 1080.0f, &l);
        CHECK(near(cp_strip_x(&l, 0.0f, &side), 720.0f, 1e-3f), "4:3: ahead is the picture's centre");
        cp_layout(3840.0f, 1080.0f, &l);
        CHECK(near(cp_strip_x(&l, 0.0f, &side), 1920.0f, 1e-3f), "32:9: ahead is the picture's centre");
    }
}

/* The strip is placed in the game's 3D view: the same fractions of the surface give the same place for every version,
 * whatever the resolution, and a smaller view (a view-size setting) moves it into that view. */
/* The 3D image the game draws for a view-size setting: the guest's own arithmetic (0x00412BC0 taking XView and YView from
 * the registry, 0x005F7980 clamping, 0x00504CC0 centring) on a screen of w x h. */
static void game_view(int w, int h, int xview, int yview, float out[4]) {
    const int unit_w = (int)((double)w * 0.66f * 0.1f), unit_h = (int)((double)h * 0.66f * 0.1f);
    int vw = (w - xview * unit_w + 2) & ~3, vh = h - yview * unit_h;
    const int min_h = 240 * h / w;
    vw = vw < 240 ? 240 : vw > w ? w : vw;
    vh = vh < min_h ? min_h : vh > h ? h : vh;
    out[0] = (float)((w - vw) / 2);
    out[1] = (float)((h - vh) / 2);
    out[2] = (float)vw;
    out[3] = (float)vh;
}

/* The view-size settings each version's registry store holds: English 1 and 1, the other three 0 and 0. */
static const struct {
    const char* version;
    int xview, yview;
} k_views[] = { { "en-116", 1, 1 }, { "ru-116", 0, 0 }, { "es-117", 0, 0 }, { "ptbr-117", 0, 0 } };

static void test_scene_layout(void) {
    static const float sizes[][2] = { { 1920.0f, 1080.0f }, { 2560.0f, 1440.0f }, { 3840.0f, 2160.0f } };
    float v[4];
    game_view(3840, 2160, 1, 1, v);
    CHECK(v[0] == 126.0f && v[1] == 71.0f && v[2] == 3588.0f && v[3] == 2018.0f, "XView 1 at 3840x2160: the English log's camera 3588x2018");
    game_view(3840, 2160, 0, 0, v);
    CHECK(v[0] == 0.0f && v[1] == 0.0f && v[2] == 3840.0f && v[3] == 2160.0f, "XView 0: the whole screen, as the Brazilian log's 3840x2160");
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const float w = sizes[s][0], h = sizes[s][1];
        cp_layout_t old_en = { 0 };
        float rel_old_top[4], rel_new[4][4];
        for (size_t p = 0; p < 4; p++) {
            float view[4];
            cp_layout_t old_layout, now;
            game_view((int)w, (int)h, k_views[p].xview, k_views[p].yview, view);
            cp_layout(w, h, &old_layout);
            cp_layout_in(view[0], view[1], view[2], view[3], &now);
            if (!p) old_en = old_layout;
            CHECK(near(old_layout.left, old_en.left, 1e-3f) && near(old_layout.top, old_en.top, 1e-3f) &&
                      near(old_layout.right, old_en.right, 1e-3f) && near(old_layout.height, old_en.height, 1e-3f),
                  "%s %.0fx%.0f: the old layout depends on the surface only, the same for every version", k_views[p].version, w, h);
            rel_old_top[p] = (old_layout.top - view[1]) / view[3];
            rel_new[p][0] = (now.left - view[0]) / view[2];
            rel_new[p][1] = (now.top - view[1]) / view[3];
            rel_new[p][2] = (now.right - now.left) / view[2];
            rel_new[p][3] = now.height / view[3];
            CHECK(now.left >= view[0] && now.right <= view[0] + view[2] && now.top >= view[1] &&
                      now.top + now.height <= view[1] + view[3] && near(now.centre, view[0] + view[2] * 0.5f, 1e-3f),
                  "%s %.0fx%.0f: the strip lies in the 3D image, centred", k_views[p].version, w, h);
            CHECK(near(now.top, view[1], 1e-3f), "%s %.0fx%.0f: glued to the image's top edge", k_views[p].version, w, h);
        }
        /* the cause: the same old strip sits at another place of the image, by the margin the game leaves around it */
        CHECK(rel_old_top[0] < 0.0f && rel_old_top[1] > 0.0f && rel_old_top[2] == rel_old_top[1] && rel_old_top[3] == rel_old_top[1],
              "%.0fx%.0f: old strip: above the English image's top edge, inside the others'", w, h);
        {
            float en[4];
            game_view((int)w, (int)h, 1, 1, en);
            CHECK(near(rel_old_top[1] - rel_old_top[0], en[1] / en[3] - 0.025f * (h / en[3] - 1.0f), 1e-4f) && rel_old_top[1] - rel_old_top[0] > 0.03f &&
                      en[1] / h > 0.03f,
                  "%.0fx%.0f: the others' strip sits %.1f%% of the image lower, from the English margin of %.1f%% of the height", w, h,
                  100.0f * (rel_old_top[1] - rel_old_top[0]), 100.0f * en[1] / h);
        }
        for (size_t p = 1; p < 4; p++)
            CHECK(near(rel_new[p][0], rel_new[0][0], 1e-3f) && near(rel_new[p][1], rel_new[0][1], 1e-4f) &&
                      near(rel_new[p][2], rel_new[0][2], 1e-3f) && near(rel_new[p][3], rel_new[0][3], 1e-3f),
                  "%.0fx%.0f: %s: the same place in the 3D image as in English", w, h, k_views[p].version);
        {
            float en[4];
            cp_layout_t before, now;
            game_view((int)w, (int)h, 1, 1, en);
            cp_layout(w, h, &before);
            cp_layout_in(en[0], en[1], en[2], en[3], &now);
            CHECK(fabsf(now.top - before.top) / h < 0.01f && fabsf(now.centre - before.centre) < 1e-3f &&
                      now.height / before.height > 0.9f && now.height / before.height <= 1.0f,
                  "%.0fx%.0f: English moves by less than 1%% of the height", w, h);
        }
        {
            cp_camera_t cam = camera_facing(0.0f);
            cp_screen_t p;
            float view[4], up[3];
            up[0] = 0.0f;
            up[1] = 10.0f;
            up[2] = 5.0f;
            for (size_t q = 0; q < 4; q++) {
                game_view((int)w, (int)h, k_views[q].xview, k_views[q].yview, view);
                CHECK(cp_project_in(&cam, view[0], view[1], view[2], view[3], 0.0f, up, &p) && near(p.x, view[0] + view[2] * 0.5f, 1e-2f) &&
                          near(p.y, view[1], 1e-2f),
                      "%.0fx%.0f: %s: the top of the camera's view is the top of the 3D image", w, h, k_views[q].version);
            }
        }
    }
}

/* The English store still holds XView 1 and YView 1 from the legacy Video options; the host answers every version with the
 * policy, so the game's 3D image is the launcher's resolution for all four and the compass sits the same in each. */
static void test_view_margin_policy(void) {
    static const int legacy[] = { 1, 0, 0, 0 };
    static const float sizes[][2] = { { 1920.0f, 1080.0f }, { 2560.0f, 1440.0f }, { 3840.0f, 2160.0f } };
    CHECK(registry_view_margin_name("XView") && registry_view_margin_name("YView") && registry_view_margin_name("xview"),
          "both margins are answered by the policy");
    CHECK(!registry_view_margin_name("Width") && !registry_view_margin_name("UseSubTitles") && !registry_view_margin_name(NULL),
          "nothing else is");
    CHECK(REGISTRY_VIEW_MARGIN == 0, "the policy is no margin");
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const int w = (int)sizes[s][0], h = (int)sizes[s][1];
        cp_layout_t first = { 0 };
        for (size_t p = 0; p < 4; p++) {
            float stored[4], used[4];
            cp_layout_t l;
            game_view(w, h, legacy[p], legacy[p], stored);
            game_view(w, h, (int)REGISTRY_VIEW_MARGIN, (int)REGISTRY_VIEW_MARGIN, used);
            CHECK(!k_views[p].xview == !legacy[p] && (legacy[p] ? stored[2] < (float)w : stored[2] == (float)w),
                  "%s: the stored margin alone would shrink the image only in English", k_views[p].version);
            CHECK(used[0] == 0.0f && used[1] == 0.0f && used[2] == (float)w && used[3] == (float)h,
                  "%s %dx%d: the main camera is the whole resolution", k_views[p].version, w, h);
            cp_layout_in(used[0], used[1], used[2], used[3], &l);
            if (!p) first = l;
            CHECK(near(l.left, first.left, 1e-3f) && near(l.top, first.top, 1e-3f) && near(l.right, first.right, 1e-3f) &&
                      near(l.height, first.height, 1e-3f) && near(l.text_y, first.text_y, 1e-3f),
                  "%s %dx%d: the compass sits where it does in English", k_views[p].version, w, h);
        }
    }
    {
        float v[4];
        game_view(3840, 2160, (int)REGISTRY_VIEW_MARGIN, (int)REGISTRY_VIEW_MARGIN, v);
        CHECK(v[2] == 3840.0f && v[3] == 2160.0f, "4K 16:9: main camera 3840x2160, not 3588x2018");
    }
}

void compass_tests(int* checks, int* failures) {
    test_headings();
    test_wrap_and_ticks();
    test_bearings();
    test_distance();
    test_projection();
    test_route();
    test_hud();
    test_reminder();
    test_text_kinds();
    test_hud_text();
    test_menus();
    test_layout();
    test_scene_layout();
    test_view_margin_policy();
    *checks += g_checks;
    *failures += g_failures;
}
