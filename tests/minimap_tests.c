/* The minimap, headless: the per-level map data and the player-centred north-up view, clipping and markers. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "debug_menu_policy.h"
#include "host_policy.h"
#include "jpt_config.h"
#include "minimap.h"

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

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char* const k_levels[] = { "BE", "JR", "IJ", "IT", "LAB", "AS", "AS2", "SUM", "TESTSCENE", "TESTSCNNGHT", "DEMO" };

static int near(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

static void test_bounds(void) {
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_bounds_t* b = minimap_bounds(k_levels[i]);
        CHECK(b, "level %s has bounds", k_levels[i]);
        if (!b) continue;
        CHECK(isfinite(b->min_x) && isfinite(b->min_y) && isfinite(b->max_x) && isfinite(b->max_y),
              "%s bounds are finite", k_levels[i]);
        CHECK(b->max_x > b->min_x && b->max_y > b->min_y, "%s bounds are non-degenerate", k_levels[i]);
    }
    CHECK(minimap_bounds(NULL) == NULL, "no level: no bounds");
    CHECK(minimap_bounds("") == NULL, "empty level: no bounds");
    CHECK(minimap_bounds("XX") == NULL, "unknown level: no bounds");
    CHECK(minimap_bounds("be") != NULL, "the bounds id is case-insensitive");
}

static void test_maps(void) {
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_level_map_t* m = minimap_map(k_levels[i]);
        int land = 0;
        CHECK(m, "level %s has a map", k_levels[i]);
        if (!m) continue;
        CHECK(m->cols > 0 && m->rows > 0, "%s mask has a size", k_levels[i]);
        CHECK(m->count > 0, "%s map is not empty", k_levels[i]);
        CHECK(m->count <= 2000, "%s map stays a reasonable size (%u runs)", k_levels[i], (unsigned)m->count);
        for (int r = 0; r < m->count; r++) {
            const minimap_run_t* run = &m->runs[r];
            CHECK(run->len > 0 && run->row < m->rows && run->start < m->cols && (int)run->start + run->len <= m->cols,
                  "%s run %d lies inside the mask", k_levels[i], r);
            CHECK(run->kind == MM_KIND_LAND || run->kind == MM_KIND_WATER, "%s run %d is land or water", k_levels[i], r);
            if (run->kind == MM_KIND_LAND) land += run->len;
        }
        CHECK(land > 0, "%s has land geometry", k_levels[i]);
    }
    CHECK(minimap_map("BE") && minimap_map("BE")->count >= 100, "BE has meaningful land geometry, not an empty box");
    CHECK(minimap_map("JR") && minimap_map("JR")->count >= 150, "JR has meaningful geometry, not an empty box");
    CHECK(minimap_map(NULL) == NULL, "no level: no map");
    CHECK(minimap_map("") == NULL, "empty level: no map");
    CHECK(minimap_map("XX") == NULL, "unknown level: no map");
    CHECK(minimap_map("be") != NULL, "the map id is case-insensitive");
}

static void test_same_transform(void) {
    /* The map and the bounds are keyed by the same id, so the geometry and the player share one transform. */
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_level_map_t* m = minimap_map(k_levels[i]);
        const minimap_bounds_t* b = minimap_bounds(k_levels[i]);
        CHECK(m && b && !strcmp(m->id, b->id), "%s has both bounds and a map under one id", k_levels[i]);
    }
}

static void test_off_hides(void) {
    dm_state_t s;
    dm_world_t w;
    cp_scene_t scene;
    cp_hud_t hud;
    memset(&s, 0, sizeof s);
    memset(&w, 0, sizeof w);
    memset(&scene, 0, sizeof scene);
    scene.level = scene.player = 1;
    w.place = DM_PLACE_LEVEL;
    w.player = 1;
    w.eye = 1;
    w.at.pos[0] = -1000.0f;
    w.at.pos[1] = -500.0f;
    w.at.rot[0] = 1.0f;
    w.eye_at.rot[0] = 1.0f;
    snprintf(w.level, sizeof w.level, "%s", "BE");

    minimap_configure(1);
    CHECK(minimap_enabled(), "on: enabled");
    dm_navigate(&s, &w, &scene, NULL, 1000u, &hud);
    CHECK(hud.minimap == 1, "on: the HUD asks for the minimap");
    minimap_configure(0);
    CHECK(!minimap_enabled(), "off: disabled");
    dm_navigate(&s, &w, &scene, NULL, 1000u, &hud);
    CHECK(hud.minimap == 0, "off: the HUD does not ask for the minimap");
    minimap_configure(1);
}

#define CX 300.0f
#define CY 250.0f
#define RADIUS 150.0f

static void frame_at(minimap_frame_t* f, float px, float py, float zoom) { minimap_frame(f, CX, CY, RADIUS, px, py, zoom); }

static void test_player_centred(void) {
    const float spots[][2] = { { 0.0f, 0.0f }, { -2700.0f, -1700.0f }, { 900.0f, 340.0f }, { 50000.0f, -80000.0f } };
    for (int i = 0; i < COUNT(spots); i++) {
        minimap_frame_t f;
        float sx, sy;
        frame_at(&f, spots[i][0], spots[i][1], MM_ZOOM);
        minimap_to_screen(&f, spots[i][0], spots[i][1], &sx, &sy);
        CHECK(sx == CX && sy == CY, "the player is exactly at the minimap centre wherever it stands (%g, %g)",
              spots[i][0], spots[i][1]);
    }
}

static void test_map_moves_under_player(void) {
    const float step = 40.0f;
    minimap_frame_t f0, f;
    float x0, y0, x1, y1;
    frame_at(&f0, -500.0f, -200.0f, MM_ZOOM);
    minimap_to_screen(&f0, -450.0f, -150.0f, &x0, &y0);

    frame_at(&f, -500.0f + step, -200.0f, MM_ZOOM);
    minimap_to_screen(&f, -450.0f, -150.0f, &x1, &y1);
    CHECK(x1 < x0 && near(y1, y0, 1e-3f), "walking east moves the map geometry left");
    CHECK(near(x0 - x1, step * f0.scale, 1e-3f), "the geometry shifts by the walked distance times the scale");
    frame_at(&f, -500.0f - step, -200.0f, MM_ZOOM);
    minimap_to_screen(&f, -450.0f, -150.0f, &x1, &y1);
    CHECK(x1 > x0 && near(y1, y0, 1e-3f), "walking west moves the map geometry right");
    frame_at(&f, -500.0f, -200.0f + step, MM_ZOOM);
    minimap_to_screen(&f, -450.0f, -150.0f, &x1, &y1);
    CHECK(y1 > y0 && near(x1, x0, 1e-3f), "walking north moves the map geometry down");
    frame_at(&f, -500.0f, -200.0f - step, MM_ZOOM);
    minimap_to_screen(&f, -450.0f, -150.0f, &x1, &y1);
    CHECK(y1 < y0 && near(x1, x0, 1e-3f), "walking south moves the map geometry up");
}

static void test_heading(void) {
    float dx, dy;
    minimap_point_t t[3];
    minimap_heading_dir(0.0f, &dx, &dy);
    CHECK(near(dx, 0.0f, 1e-5f) && near(dy, -1.0f, 1e-5f), "heading 0 points north (up)");
    minimap_heading_dir(90.0f, &dx, &dy);
    CHECK(near(dx, 1.0f, 1e-5f) && near(dy, 0.0f, 1e-5f), "heading 90 points east (right)");
    minimap_heading_dir(180.0f, &dx, &dy);
    CHECK(near(dx, 0.0f, 1e-5f) && near(dy, 1.0f, 1e-5f), "heading 180 points south (down)");
    minimap_heading_dir(270.0f, &dx, &dy);
    CHECK(near(dx, -1.0f, 1e-5f) && near(dy, 0.0f, 1e-5f), "heading 270 points west (left)");

    for (float heading = 0.0f; heading < 360.0f; heading += 17.0f) {
        const float k = RADIUS * MM_PLAYER_K;
        minimap_heading_dir(heading, &dx, &dy);
        minimap_arrow(CX, CY, dx, dy, k, t);
        CHECK(near((t[0].x + t[1].x + t[2].x) / 3.0f, CX, 1e-3f) && near((t[0].y + t[1].y + t[2].y) / 3.0f, CY, 1e-3f),
              "heading %g rotates the arrow around the centre without translating it", heading);
        CHECK(near(t[0].x, CX + dx * k, 1e-3f) && near(t[0].y, CY + dy * k, 1e-3f),
              "heading %g: the tip faces the heading", heading);
    }
}

/* Player-up: compass heading h faces world (sin h, cos h) (north +y, headings clockwise); that direction is drawn up. */
static void test_player_up(void) {
    static const struct {
        float heading;
        const char* north;
        float n_dx, n_dy; /* where a marker due north lands, as a screen direction from the player */
    } turns[] = { { 0.0f, "up", 0.0f, -1.0f }, { 90.0f, "left", -1.0f, 0.0f }, { 180.0f, "down", 0.0f, 1.0f },
                  { 270.0f, "right", 1.0f, 0.0f } };
    const float px = -120.0f, py = 340.0f, d = 30.0f;
    for (int i = 0; i < COUNT(turns); i++) {
        const float h = turns[i].heading, rad = h * 0.017453292f;
        const float fx = sinf(rad), fy = cosf(rad), rx = cosf(rad), ry = -sinf(rad);
        const struct {
            const char* what;
            float wx, wy, sdx, sdy;
        } marks[] = { { "ahead", px + fx * d, py + fy * d, 0.0f, -1.0f }, { "behind", px - fx * d, py - fy * d, 0.0f, 1.0f },
                      { "right", px + rx * d, py + ry * d, 1.0f, 0.0f }, { "left", px - rx * d, py - ry * d, -1.0f, 0.0f },
                      { "north", px, py + d, turns[i].n_dx, turns[i].n_dy },
                      { "south", px, py - d, -turns[i].n_dx, -turns[i].n_dy },
                      { "east", px + d, py, -turns[i].n_dy, turns[i].n_dx },
                      { "west", px - d, py, turns[i].n_dy, -turns[i].n_dx } };
        minimap_frame_t f;
        float sx, sy;
        frame_at(&f, px, py, MM_ZOOM);
        minimap_face(&f, h);
        minimap_to_screen(&f, px, py, &sx, &sy);
        CHECK(near(sx, CX, 1e-3f) && near(sy, CY, 1e-3f), "heading %g: the player stays at the centre", h);
        for (int m = 0; m < COUNT(marks); m++) {
            minimap_to_screen(&f, marks[m].wx, marks[m].wy, &sx, &sy);
            CHECK(near(sx - CX, marks[m].sdx * d * f.scale, 1e-2f) && near(sy - CY, marks[m].sdy * d * f.scale, 1e-2f),
                  "heading %g: a marker %s of the player is drawn %s", h, marks[m].what,
                  marks[m].sdy < 0.0f ? "above" : marks[m].sdy > 0.0f ? "below" : marks[m].sdx > 0.0f ? "to the right" : "to the left");
        }
        {
            minimap_marker_t w;
            minimap_creature_t c = { px + fx * 10.0f, py + fy * 10.0f, 0 };
            minimap_waypoint(&f, px + fx * 5000.0f, py + fy * 5000.0f, &w);
            CHECK(w.icon == MM_ICON_ARROW && near(w.dir_x, 0.0f, 1e-3f) && near(w.dir_y, -1.0f, 1e-3f) && w.y < CY,
                  "heading %g: a far objective straight ahead is the edge arrow at the top", h);
            minimap_waypoint(&f, px - rx * 5000.0f, py - ry * 5000.0f, &w);
            CHECK(near(w.dir_x, -1.0f, 1e-3f) && near(w.dir_y, 0.0f, 1e-3f), "heading %g: one to the left is on the left edge", h);
            CHECK(minimap_creature_place(&f, &c, &sx, &sy) && near(sx, CX, 1e-2f) && sy < CY,
                  "heading %g: a creature ahead is above the player", h);
        }
        {
            minimap_point_t out[MM_CLIP_MAX];
            int n = minimap_clip_rect(&f, CX - 400.0f, CY - 10.0f, CX + 400.0f, CY + 10.0f, out);
            CHECK(n >= 3, "heading %g: a long shape across the circle is clipped, not dropped", h);
            for (int k = 0; k < n; k++)
                CHECK(hypotf(out[k].x - CX, out[k].y - CY) <= RADIUS + 1e-2f, "heading %g: clipped inside the circle", h);
        }
    }
    {
        minimap_frame_t a, b;
        float ax, ay, bx, by;
        frame_at(&a, 0.0f, 0.0f, 2.0f);
        frame_at(&b, 0.0f, 0.0f, 4.0f);
        minimap_face(&a, 37.0f);
        minimap_face(&b, 37.0f);
        minimap_to_screen(&a, 12.0f, -7.0f, &ax, &ay);
        minimap_to_screen(&b, 12.0f, -7.0f, &bx, &by);
        CHECK(near(bx - CX, 2.0f * (ax - CX), 1e-3f) && near(by - CY, 2.0f * (ay - CY), 1e-3f),
              "player-up: doubling the zoom still doubles every offset");
        frame_at(&a, 0.0f, 0.0f, MM_ZOOM);
        minimap_face(&a, NAN);
        CHECK(a.up_x == 0.0f && a.up_y == 1.0f, "a heading that is not finite leaves the map north-up");
    }
}

static void test_zoom_geometry(void) {
    minimap_frame_t a, b;
    float ax, ay, bx, by, cx, cy;
    frame_at(&a, 100.0f, -300.0f, 2.0f);
    frame_at(&b, 100.0f, -300.0f, 4.0f);
    minimap_to_screen(&a, 100.0f, -300.0f, &cx, &cy);
    CHECK(cx == CX && cy == CY, "the player is at the centre before zooming");
    minimap_to_screen(&b, 100.0f, -300.0f, &cx, &cy);
    CHECK(cx == CX && cy == CY, "zoom does not move the player marker");
    minimap_to_screen(&a, 160.0f, -250.0f, &ax, &ay);
    minimap_to_screen(&b, 160.0f, -250.0f, &bx, &by);
    CHECK(near(bx - CX, 2.0f * (ax - CX), 1e-3f) && near(by - CY, 2.0f * (ay - CY), 1e-3f),
          "doubling the zoom doubles every offset from the player");
    frame_at(&a, 0.0f, 0.0f, 0.25f);
    frame_at(&b, 0.0f, 0.0f, MM_ZOOM_MIN);
    CHECK(a.scale == b.scale, "zoom below the minimum is clamped");
    frame_at(&a, 0.0f, 0.0f, 100.0f);
    frame_at(&b, 0.0f, 0.0f, MM_ZOOM_MAX);
    CHECK(a.scale == b.scale, "zoom above the maximum is clamped");
    CHECK(MM_ZOOM == 2.0f, "the default zoom is 2x");
    frame_at(&a, 0.0f, 0.0f, MM_ZOOM);
    CHECK(near(2.0f * RADIUS / a.scale, MM_VIEW_SPAN_M / MM_ZOOM, 1e-2f),
          "the default view covers a local span, not the whole level");
}

static void test_waypoint(void) {
    minimap_frame_t f;
    minimap_marker_t m;
    const float reach = RADIUS / (2.0f * RADIUS * MM_ZOOM / MM_VIEW_SPAN_M);
    frame_at(&f, 100.0f, -300.0f, MM_ZOOM);

    minimap_waypoint(&f, 100.0f + reach * 0.4f, -300.0f + reach * 0.3f, &m);
    CHECK(m.inside, "an objective within the circle is shown at its own position");
    CHECK(near(m.x, CX + reach * 0.4f * f.scale, 1e-2f) && near(m.y, CY - reach * 0.3f * f.scale, 1e-2f),
          "the inside objective projects relative to the player (north up)");

    {
        const float dirs[4][2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { -1.0f, 0.0f } };
        const char* names[4] = { "north", "east", "south", "west" };
        const float want[4][2] = { { 0.0f, -1.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f } };
        for (int i = 0; i < 4; i++) {
            minimap_waypoint(&f, 100.0f + dirs[i][0] * reach * 5.0f, -300.0f + dirs[i][1] * reach * 5.0f, &m);
            CHECK(!m.inside, "an objective %s beyond the circle is an edge indicator", names[i]);
            CHECK(near(m.dir_x, want[i][0], 1e-4f) && near(m.dir_y, want[i][1], 1e-4f), "the %s indicator points %s",
                  names[i], names[i]);
            CHECK(near(hypotf(m.x - CX, m.y - CY), RADIUS * MM_EDGE_ANCHOR, 1e-2f),
                  "the %s indicator sits on the edge anchor", names[i]);
            CHECK(hypotf(m.x - CX, m.y - CY) + RADIUS * MM_EDGE_ARROW_K < RADIUS,
                  "the %s indicator stays inside the circle", names[i]);
        }
    }

    minimap_waypoint(&f, 100.0f + reach * 3.0f, -300.0f + reach * 4.0f, &m);
    CHECK(!m.inside && near(m.dir_x, 0.6f, 1e-4f) && near(m.dir_y, -0.8f, 1e-4f),
          "a diagonal edge indicator follows the player-to-objective vector");

    minimap_waypoint(&f, 100.0f + 2.0f, -300.0f, &m);
    CHECK(m.inside && hypotf(m.x - CX, m.y - CY) >= RADIUS * MM_WAYPOINT_CLEAR - 1e-3f,
          "a very near objective stays clear of the player marker");
    CHECK(m.dir_x > 0.99f, "a very near objective keeps its true direction");
    minimap_waypoint(&f, 100.0f, -300.0f, &m);
    CHECK(m.inside && isfinite(m.x) && isfinite(m.y) && hypotf(m.x - CX, m.y - CY) >= RADIUS * MM_WAYPOINT_CLEAR - 1e-3f,
          "an objective at the player's feet is finite and does not sit on the player");
    CHECK(MM_WAYPOINT_CLEAR > MM_PLAYER_K + MM_WAYPOINT_K, "the clearance separates the two markers");
    CHECK(MM_EDGE_ANCHOR >= 0.8f && MM_EDGE_ANCHOR <= 0.88f, "the edge arrow sits at 80-88%% of the radius");
}

static void test_clipping(void) {
    {
        minimap_frame_t f;
        minimap_point_t pts[MM_CLIP_MAX];
        int n;
        frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
        CHECK(minimap_clip_rect(&f, CX + RADIUS, CY - 10.0f, CX + RADIUS + 50.0f, CY + 10.0f, pts) == 0,
              "a rect entirely outside is dropped");
        CHECK(minimap_clip_rect(&f, CX - 5.0f, CY - 5.0f, CX + 5.0f, CY + 5.0f, pts) == 4, "a rect inside is untouched");
        CHECK(minimap_clip_rect(&f, CX - 1000.0f, CY - 1000.0f, CX + 1000.0f, CY + 1000.0f, pts) >= MM_CLIP_SIDES,
              "a rect covering the minimap becomes the circle");
        n = minimap_clip_rect(&f, CX + RADIUS * 0.6f, CY - RADIUS * 0.9f, CX + RADIUS * 1.5f, CY - RADIUS * 0.2f, pts);
        CHECK(n >= 3, "a rect straddling the edge is clipped, not dropped");
        for (int k = 0; k < n; k++)
            CHECK(hypotf(pts[k].x - CX, pts[k].y - CY) <= RADIUS + 1e-2f, "a clipped vertex lies inside the circle");
    }
}

static float luminance(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

static void test_colours(void) {
    const float land[3] = { MM_COLOR_LAND }, water[3] = { MM_COLOR_WATER }, building[3] = { MM_COLOR_BUILDING },
                wall[3] = { MM_COLOR_WALL }, road[3] = { MM_COLOR_ROAD }, player[3] = { MM_COLOR_PLAYER },
                objective[3] = { MM_COLOR_OBJECTIVE }, edge[3] = { MM_COLOR_LANDMARK_EDGE }, threat[3] = { MM_COLOR_THREAT },
                passive[3] = { MM_COLOR_PASSIVE };
    CHECK(land[1] > land[0] && land[1] > land[2], "land is dark green");
    CHECK(water[2] > water[0] + 0.4f && water[2] > water[1] + 0.3f, "water is blue");
    CHECK(fabsf(building[0] - building[1]) < 0.06f && fabsf(building[1] - building[2]) < 0.08f, "buildings are grey");
    CHECK(luminance(building) > luminance(land) + 0.25f && luminance(building) > luminance(water) + 0.1f,
          "buildings stand out from land and water");
    CHECK(luminance(wall) > luminance(building) + 0.15f, "walls and fences are lighter than buildings");
    CHECK(road[0] > road[2] && luminance(road) > luminance(land) + 0.1f && luminance(road) < luminance(building),
          "roads are a muted sand between the land and the buildings");
    CHECK(luminance(edge) < luminance(building) * 0.3f, "the landmark outline is clearly darker than its fill");
    CHECK(player[0] == 1.0f && player[1] == 1.0f && player[2] == 1.0f, "the player is white");
    CHECK(objective[0] > 0.9f && objective[1] > 0.75f && objective[2] < 0.4f, "the objective is bright yellow/gold");
    {
        const float* others[6] = { building, wall, road, water, land, passive };
        for (int i = 0; i < 6; i++) {
            const float d = fabsf(objective[0] - others[i][0]) + fabsf(objective[1] - others[i][1]) + fabsf(objective[2] - others[i][2]);
            const float dt = fabsf(threat[0] - others[i][0]) + fabsf(threat[1] - others[i][1]) + fabsf(threat[2] - others[i][2]);
            CHECK(d > 0.45f, "the objective yellow is clearly distinct from map colour %d (%.2f)", i, d);
            if (i != 5) CHECK(dt > 0.5f, "the threat red is clearly distinct from map colour %d (%.2f)", i, dt);
        }
    }
}

static void picture_rect(minimap_rect_t* r, float x, float y, float w, float h) {
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}

static void test_place(void) {
    static const struct {
        float w, h, lo, hi;
    } sizes[] = { { 1920, 1080, 250, 300 }, { 2560, 1440, 330, 400 }, { 3840, 2160, 480, 560 } };
    for (int i = 0; i < COUNT(sizes); i++) {
        minimap_rect_t pic;
        minimap_frame_t f;
        float d, margin;
        picture_rect(&pic, 0, 0, sizes[i].w, sizes[i].h);
        CHECK(minimap_place(&pic, 0.0f, 0.0f, MM_ZOOM, &f), "%gp has a minimap", sizes[i].h);
        d = f.radius * 2.0f;
        margin = f.cx - f.radius;
        CHECK(d >= sizes[i].lo && d <= sizes[i].hi, "%gp diameter %.0f is in %g-%g", sizes[i].h, d, sizes[i].lo,
              sizes[i].hi);
        CHECK(margin >= sizes[i].h * 0.02f && margin <= sizes[i].h * 0.025f,
              "%gp left margin %.1f is 2-2.5%% of the height", sizes[i].h, margin);
        CHECK(near(sizes[i].h - (f.cy + f.radius), margin, 1e-2f), "%gp bottom margin equals the left margin",
              sizes[i].h);
        CHECK(f.cx < sizes[i].w * 0.5f && f.cy > sizes[i].h * 0.5f, "%gp: the minimap is bottom-left", sizes[i].h);
        CHECK(f.cx - f.radius >= 0.0f && f.cy + f.radius <= sizes[i].h, "%gp: the minimap is inside the picture",
              sizes[i].h);
    }
    {
        minimap_rect_t pic;
        minimap_frame_t f;
        picture_rect(&pic, 200.0f, 60.0f, 1440.0f, 810.0f);
        CHECK(minimap_place(&pic, 0.0f, 0.0f, MM_ZOOM, &f), "a letterboxed picture has a minimap");
        CHECK(f.cx - f.radius >= pic.x && f.cy + f.radius <= pic.y + pic.h && f.cx - f.radius < pic.x + pic.h * 0.03f &&
                  f.cy + f.radius > pic.y + pic.h * 0.95f,
              "the minimap hugs the letterboxed picture's bottom-left, not the window's");
        picture_rect(&pic, 0, 0, 100, 60);
        CHECK(!minimap_place(&pic, 0.0f, 0.0f, MM_ZOOM, &f), "a tiny picture shows no minimap");
        picture_rect(&pic, 0, 0, 0, 0);
        CHECK(!minimap_place(&pic, 0.0f, 0.0f, MM_ZOOM, &f), "an empty picture shows no minimap");
        CHECK(!minimap_place(NULL, 0.0f, 0.0f, MM_ZOOM, &f), "no picture: no minimap");
    }
}

/* The runtime data path: world sample -> dm_navigate -> HUD -> minimap_place -> projection, frame after frame. */
static void hud_frame(cp_hud_t* hud, float px, float py, float yaw, int goal_set, float gx, float gy, int game_text) {
    dm_state_t s;
    dm_world_t w;
    cp_scene_t scene;
    cp_goal_t goal;
    memset(&s, 0, sizeof s);
    memset(&w, 0, sizeof w);
    memset(&scene, 0, sizeof scene);
    memset(&goal, 0, sizeof goal);
    scene.level = scene.player = 1;
    scene.game_text = game_text;
    w.place = DM_PLACE_LEVEL;
    w.player = 1;
    w.eye = 1;
    w.at.pos[0] = px;
    w.at.pos[1] = py;
    w.at.rot[0] = 1.0f;
    w.eye_at.pos[0] = px;
    w.eye_at.pos[1] = py;
    w.eye_at.rot[0] = cosf(yaw * 0.5f);
    w.eye_at.rot[3] = sinf(yaw * 0.5f);
    snprintf(w.level, sizeof w.level, "%s", "BE");
    goal.set = goal.target = goal_set;
    goal.pos[0] = gx;
    goal.pos[1] = gy;
    minimap_configure(1);
    dm_navigate(&s, &w, &scene, &goal, 1000u, hud);
}

static int hud_project(const cp_hud_t* hud, float wx, float wy, float* sx, float* sy, minimap_frame_t* fr) {
    minimap_rect_t pic;
    picture_rect(&pic, 0, 0, 1920, 1080);
    if (!minimap_place_facing(&pic, hud->player_pos[0], hud->player_pos[1], hud->heading, MM_ZOOM, fr)) return 0;
    minimap_to_screen(fr, wx, wy, sx, sy);
    return 1;
}

static void test_live_pipeline(void) {
    const float landmark[2] = { -1000.0f, -500.0f };
    const struct {
        const char* name;
        float dx, dy;
        int sign_x, sign_y;
    } walks[] = { { "east", 10.0f, 0.0f, -1, 0 }, { "west", -10.0f, 0.0f, 1, 0 },
                  { "north", 0.0f, 10.0f, 0, 1 }, { "south", 0.0f, -10.0f, 0, -1 } };
    for (int i = 0; i < COUNT(walks); i++) {
        cp_hud_t a, b;
        minimap_frame_t fa, fb;
        float ax, ay, bx, by;
        hud_frame(&a, 100.0f, 100.0f, 0.0f, 0, 0.0f, 0.0f, 0);
        hud_frame(&b, 100.0f + walks[i].dx, 100.0f + walks[i].dy, 0.0f, 0, 0.0f, 0.0f, 0);
        CHECK(a.minimap && b.minimap, "%s: both frames draw the minimap", walks[i].name);
        CHECK(hud_project(&a, landmark[0], landmark[1], &ax, &ay, &fa) &&
                  hud_project(&b, landmark[0], landmark[1], &bx, &by, &fb),
              "%s: both frames project", walks[i].name);
        CHECK(fa.cx == fb.cx && fa.cy == fb.cy, "%s: the player marker centre is identical in both frames",
              walks[i].name);
        CHECK(walks[i].sign_x == 0 ? near(ax, bx, 1e-3f) : (walks[i].sign_x < 0 ? bx < ax : bx > ax),
              "%s: the same world point shifts %s horizontally", walks[i].name, walks[i].sign_x < 0 ? "left" : "right");
        CHECK(walks[i].sign_y == 0 ? near(ay, by, 1e-3f) : (walks[i].sign_y > 0 ? by > ay : by < ay),
              "%s: the same world point shifts %s vertically", walks[i].name, walks[i].sign_y > 0 ? "down" : "up");
        CHECK(near(hypotf(bx - ax, by - ay), 10.0f * fa.scale, 1e-2f),
              "%s: the shift is the walked distance times the scale", walks[i].name);
    }

    {
        cp_hud_t a, b;
        minimap_frame_t fa, fb;
        float ax, ay, bx, by, dxa, dya, dxb, dyb;
        minimap_point_t ta[3], tb[3];
        hud_frame(&a, 100.0f, 100.0f, 0.0f, 0, 0.0f, 0.0f, 0);
        hud_frame(&b, 100.0f, 100.0f, 1.0f, 0, 0.0f, 0.0f, 0);
        hud_project(&a, landmark[0], landmark[1], &ax, &ay, &fa);
        hud_project(&b, landmark[0], landmark[1], &bx, &by, &fb);
        CHECK(fa.cx == fb.cx && fa.cy == fb.cy, "turning keeps the player at the centre");
        CHECK(!near(ax, bx, 0.5f) || !near(ay, by, 0.5f), "turning without walking rotates the map");
        CHECK(near(hypotf(ax - fa.cx, ay - fa.cy), hypotf(bx - fb.cx, by - fb.cy), 1e-2f),
              "the map turns around the player: distances on it stay the same");
        CHECK(fabsf(a.heading - b.heading) > 1.0f, "turning changes the heading");
        minimap_heading_dir(0.0f, &dxa, &dya);
        minimap_heading_dir(0.0f, &dxb, &dyb);
        minimap_arrow(fa.cx, fa.cy, dxa, dya, fa.radius * MM_PLAYER_K, ta);
        minimap_arrow(fb.cx, fb.cy, dxb, dyb, fb.radius * MM_PLAYER_K, tb);
        CHECK(near(ta[0].x, tb[0].x, 1e-3f) && near(ta[0].y, tb[0].y, 1e-3f) && tb[0].y < fb.cy,
              "the player arrow points up whatever the heading");
    }

    {
        cp_hud_t hud;
        minimap_frame_t f;
        minimap_marker_t m;
        float sx, sy, y, last_d = 1e9f;
        int edge_frames = 0, inside_frames = 0, switched = 0, last_inside = -1, closing = 1;
        for (y = -600.0f; y <= -420.0f; y += 6.0f) {
            hud_frame(&hud, -1000.0f, y, 0.0f, 1, -1000.0f, -400.0f, 1);
            CHECK(hud.minimap && hud.waypoint && !hud.compass,
                  "walking to the objective: the minimap and waypoint stay with text up");
            hud_project(&hud, 0.0f, 0.0f, &sx, &sy, &f);
            minimap_waypoint(&f, hud.waypoint_pos[0], hud.waypoint_pos[1], &m);
            if (last_inside == 0 && m.inside) switched++;
            last_inside = m.inside;
            if (m.inside) {
                const float d = hypotf(m.x - f.cx, m.y - f.cy);
                inside_frames++;
                if (d > last_d + 1e-3f) closing = 0;
                last_d = d;
            } else {
                edge_frames++;
                CHECK(near(m.dir_x, 0.0f, 1e-4f) && near(m.dir_y, -1.0f, 1e-4f),
                      "the objective to the north is a north-pointing edge arrow");
            }
        }
        CHECK(edge_frames > 0 && inside_frames > 0 && switched == 1,
              "the edge arrow becomes the inside marker exactly once (%d edge, %d inside, %d switches)", edge_frames,
              inside_frames, switched);
        CHECK(closing, "the inside marker moves toward the player as the objective nears");
    }
}

static void test_landmarks(void) {
    int total = 0;
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_bounds_t* b = minimap_bounds(k_levels[i]);
        const minimap_level_map_t* m = minimap_map(k_levels[i]);
        if (!b || !m) continue;
        total += m->landmark_count;
        for (int l = 0; l < m->landmark_count; l++) {
            const minimap_landmark_t* lm = &m->landmarks[l];
            float min_x = lm->x[0], max_x = lm->x[0], min_y = lm->y[0], max_y = lm->y[0];
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                ok &= isfinite(lm->x[k]) && isfinite(lm->y[k]);
                if (lm->x[k] < min_x) min_x = lm->x[k];
                if (lm->x[k] > max_x) max_x = lm->x[k];
                if (lm->y[k] < min_y) min_y = lm->y[k];
                if (lm->y[k] > max_y) max_y = lm->y[k];
            }
            CHECK(ok, "%s landmark %d is finite", k_levels[i], l);
            CHECK(max_x - min_x >= 1.0f || max_y - min_y >= 1.0f, "%s landmark %d has a footprint", k_levels[i], l);
            CHECK(hypotf(lm->x[1] - lm->x[0], lm->y[1] - lm->y[0]) * hypotf(lm->x[3] - lm->x[0], lm->y[3] - lm->y[0]) > 1.0f,
                  "%s landmark %d is not degenerate", k_levels[i], l);
        }
    }
    CHECK(total > 0, "the maps contain landmarks");
    CHECK(minimap_map("SUM") && minimap_map("SUM")->landmark_count >= 60, "Summit keeps its landmarks (complex, bunker, helipad, fences)");
    CHECK(minimap_map("IT") && minimap_map("IT")->landmark_count >= 300, "the town level keeps its buildings");
    CHECK(minimap_map("XX") == NULL, "an unknown level has no landmarks");

    {
        const minimap_level_map_t* m = minimap_map("SUM");
        minimap_frame_t f;
        int drawn = 0;
        float worst = 0.0f;
        for (int l = 0; m && l < m->landmark_count; l++) {
            const minimap_landmark_t* lm = &m->landmarks[l];
            frame_at(&f, (lm->x[0] + lm->x[2]) * 0.5f, (lm->y[0] + lm->y[2]) * 0.5f, MM_ZOOM);
            minimap_point_t pts[MM_CLIP_MAX];
            const int n = minimap_landmark_polygon(&f, lm, pts);
            drawn += n >= 3;
            for (int k = 0; k < n; k++) worst = fmaxf(worst, hypotf(pts[k].x - CX, pts[k].y - CY));
        }
        CHECK(drawn == (m ? m->landmark_count : 0), "a landmark is drawn when the player stands at it (%d)", drawn);
        CHECK(worst <= RADIUS + 1e-2f, "landmark geometry stays inside the circle");
        {
            minimap_point_t none[MM_CLIP_MAX];
            CHECK(minimap_landmark_polygon(&f, NULL, none) == 0, "no landmark: nothing drawn");
        }
    }
}

static void test_creatures(void) {
    minimap_creature_t in[40], out[MM_CREATURES_MAX], again[MM_CREATURES_MAX];
    minimap_frame_t f;
    float sx, sy;
    int n;
    CHECK(minimap_creature_kind(1, 1) == MM_CREATURE_NONE && minimap_creature_kind(1, 0) == MM_CREATURE_NONE,
          "a dead creature is not shown");
    CHECK(minimap_creature_kind(0, 1) == MM_CREATURE_THREAT, "an engaged creature is a threat");
    CHECK(minimap_creature_kind(0, 0) == MM_CREATURE_PASSIVE, "a creature that is not engaged is passive");

    {
        const float threat[3] = { MM_COLOR_THREAT }, passive[3] = { MM_COLOR_PASSIVE }, objective[3] = { MM_COLOR_OBJECTIVE },
                    player[3] = { MM_COLOR_PLAYER }, land[3] = { MM_COLOR_LAND }, landmark[3] = { MM_COLOR_BUILDING };
        const float* others[4] = { objective, player, land, landmark };
        CHECK(threat[0] > 0.8f && threat[1] < 0.3f && threat[2] < 0.3f, "threats are red");
        CHECK(passive[1] > 0.8f && passive[0] < 0.6f && passive[2] < 0.5f, "passive creatures are green");
        for (int i = 0; i < 4; i++) {
            float dt = fabsf(threat[0] - others[i][0]) + fabsf(threat[1] - others[i][1]) + fabsf(threat[2] - others[i][2]);
            float dp = fabsf(passive[0] - others[i][0]) + fabsf(passive[1] - others[i][1]) + fabsf(passive[2] - others[i][2]);
            CHECK(dt > 0.5f && dp > 0.5f, "creature colours differ from the other map colours (%d)", i);
        }
        CHECK(fabsf(passive[1] - land[1]) > 0.4f, "passive green is clearly brighter than the land green");
    }

    for (int i = 0; i < 40; i++) {
        in[i].x = 3.0f * (float)((i * 7) % 40) - 60.0f;
        in[i].y = 3.0f * (float)((i * 11) % 40) - 60.0f;
        in[i].threat = i & 1;
    }
    in[3].x = NAN;
    in[4].y = INFINITY;
    in[5].x = 5000.0f;
    in[6].x = 400.0f;
    in[6].y = 0.0f;
    n = minimap_creatures_select(0.0f, 0.0f, minimap_creature_range(MM_ZOOM), in, 40, out, MM_CREATURES_MAX);
    CHECK(n == MM_CREATURES_MAX, "a crowded map is capped at %d creatures (%d)", MM_CREATURES_MAX, n);
    for (int i = 0; i < n; i++) {
        CHECK(isfinite(out[i].x) && isfinite(out[i].y), "selected creature %d is finite", i);
        CHECK(hypotf(out[i].x, out[i].y) <= minimap_creature_range(MM_ZOOM), "selected creature %d is in range", i);
        if (i) CHECK(hypotf(out[i].x, out[i].y) >= hypotf(out[i - 1].x, out[i - 1].y) - 1e-3f, "selection is nearest first (%d)", i);
    }
    CHECK(minimap_creatures_select(0.0f, 0.0f, minimap_creature_range(MM_ZOOM), in, 40, again, MM_CREATURES_MAX) == n && !memcmp(out, again, (size_t)n * sizeof out[0]),
          "the selection is deterministic");
    {
        float worst_kept = hypotf(out[n - 1].x, out[n - 1].y);
        int closer = 0;
        for (int i = 0; i < 40; i++) {
            if (!isfinite(in[i].x) || !isfinite(in[i].y)) continue;
            if (hypotf(in[i].x, in[i].y) < worst_kept - 1e-3f) closer++;
        }
        CHECK(closer <= n, "no nearer creature was dropped for a farther one");
    }
    CHECK(minimap_creatures_select(0.0f, 0.0f, 100.0f, NULL, 0, out, MM_CREATURES_MAX) == 0, "no creatures: nothing selected");
    CHECK(minimap_creatures_select(0.0f, 0.0f, 100.0f, in, 40, out, 0) == 0, "no room: nothing selected");
    CHECK(minimap_creatures_select(0.0f, 0.0f, 100.0f, in, 0, out, MM_CREATURES_MAX) == 0, "an empty list selects nothing");
    CHECK(minimap_creatures_select(0.0f, 0.0f, 0.0f, in, 40, out, MM_CREATURES_MAX) == 0, "no range: nothing selected");
    CHECK(minimap_creatures_select(0.0f, 0.0f, NAN, in, 40, out, MM_CREATURES_MAX) == 0, "a non-finite range selects nothing");
    {
        const minimap_creature_t line[3] = { { 30.0f, 0.0f, 0 }, { 80.0f, 0.0f, 1 }, { 150.0f, 0.0f, 0 } };
        CHECK(minimap_creatures_select(0.0f, 0.0f, 50.0f, line, 3, out, MM_CREATURES_MAX) == 1, "a creature past the range is dropped");
        CHECK(minimap_creatures_select(0.0f, 0.0f, 100.0f, line, 3, out, MM_CREATURES_MAX) == 2 && out[0].x == 30.0f,
              "the nearest creature comes first");
    }

    frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
    {
        minimap_creature_t near_c = { 20.0f, 10.0f, 1 }, far_c = { 4000.0f, 0.0f, 0 }, bad = { NAN, 0.0f, 0 };
        CHECK(minimap_creature_place(&f, &near_c, &sx, &sy) && isfinite(sx) && isfinite(sy), "a nearby creature is placed");
        CHECK(near(sx, CX + 20.0f * f.scale, 1e-3f) && near(sy, CY - 10.0f * f.scale, 1e-3f),
              "a creature is placed relative to the player, north up");
        CHECK(!minimap_creature_place(&f, &far_c, &sx, &sy), "a creature outside the circle is not drawn");
        CHECK(!minimap_creature_place(&f, &bad, &sx, &sy), "a non-finite creature is not drawn");
        CHECK(!minimap_creature_place(&f, NULL, &sx, &sy), "no creature: nothing placed");
    }
}

static void test_creature_policy(void) {
    const float zooms[] = { MM_ZOOM_MIN, 2.0f, MM_ZOOM, 4.5f, MM_ZOOM_MAX };
    float last = 1e9f;
    for (int i = 0; i < COUNT(zooms); i++) {
        const float r = minimap_creature_range(zooms[i]);
        const float view_radius = MM_VIEW_SPAN_M / zooms[i] * 0.5f;
        CHECK(r >= MM_CREATURE_RANGE_MIN_M && r <= MM_CREATURE_RANGE_MAX_M, "the creature range %.0f m is within the practical bounds", r);
        CHECK(r <= view_radius + 1e-3f, "zoom %.1f: creatures never appear beyond what the map shows", zooms[i]);
        CHECK(r <= last + 1e-3f, "zooming in never widens the creature range");
        last = r;
    }
    CHECK(near(minimap_creature_range(MM_ZOOM), 90.0f, 1e-3f), "at the default zoom creatures show within 90 m");
    CHECK(minimap_creature_range(MM_ZOOM_MIN) <= MM_CREATURE_RANGE_MAX_M, "zoomed out, the range stays capped");
    CHECK(isfinite(minimap_creature_range(NAN)) && minimap_creature_range(NAN) >= MM_CREATURE_RANGE_MIN_M, "a bad zoom gives a usable range");
    CHECK(MM_CREATURES_MAX <= 6, "at most 6 creature markers");
    CHECK(MM_CREATURE_K < MM_WAYPOINT_K * 0.5f && MM_CREATURE_K < MM_PLAYER_K * 0.5f,
          "creature markers are clearly smaller than the objective and the player");
    CHECK(MM_CREATURE_K <= 0.03f, "creature markers are smaller than the previous 0.05 of the radius");

    CHECK(minimap_creature_engaged(0, 1, 5.0f, 30.0f) == 0, "an animal whose brain is not running is not engaged");
    CHECK(minimap_creature_engaged(1, 1, 20.0f, 30.0f) == 1, "a running animal with the player inside its radius is engaged");
    CHECK(minimap_creature_engaged(1, 1, 45.0f, 30.0f) == 0, "a running animal with the player outside its radius is not engaged");
    CHECK(minimap_creature_engaged(1, 0, 0.0f, 30.0f) == 1, "no player position: running counts as engaged");
    CHECK(minimap_creature_engaged(1, 1, 20.0f, 0.0f) == 1 && minimap_creature_engaged(1, 1, 20.0f, NAN) == 1,
          "an unusable radius falls back to running");
    CHECK(minimap_creature_engaged(1, 1, NAN, 30.0f) == 1, "a non-finite distance falls back to running");
    CHECK(minimap_creature_kind(0, minimap_creature_engaged(1, 1, 5.0f, 30.0f)) == MM_CREATURE_THREAT, "engaged is red");
    CHECK(minimap_creature_kind(0, minimap_creature_engaged(0, 1, 5.0f, 30.0f)) == MM_CREATURE_PASSIVE, "not engaged is green");
    CHECK(minimap_creature_kind(1, minimap_creature_engaged(1, 1, 5.0f, 30.0f)) == MM_CREATURE_NONE, "a dead creature is never shown");
}

static void test_player_shape(void) {
    const float k = RADIUS * MM_PLAYER_K;
    for (float heading = 0.0f; heading < 360.0f; heading += 23.0f) {
        minimap_point_t a[4];
        float dx, dy, area = 0.0f, mx = 0.0f, my = 0.0f;
        minimap_heading_dir(heading, &dx, &dy);
        minimap_player_shape(CX, CY, dx, dy, k, a);
        for (int tri = 0; tri < 2; tri++) {
            const minimap_point_t p0 = a[0], p1 = tri ? a[2] : a[1], p2 = tri ? a[3] : a[2];
            const float cross = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
            area += cross * 0.5f;
            mx += cross * 0.5f * (p0.x + p1.x + p2.x) / 3.0f;
            my += cross * 0.5f * (p0.y + p1.y + p2.y) / 3.0f;
        }
        CHECK(fabsf(area) > 1.0f, "heading %g: the player arrow has an area", heading);
        CHECK(near(mx / area, CX, 1e-2f) && near(my / area, CY, 1e-2f), "heading %g: the arrow's centroid is the player's position", heading);
        for (int i = 0; i < 4; i++)
            CHECK(isfinite(a[i].x) && isfinite(a[i].y) && hypotf(a[i].x - CX, a[i].y - CY) <= k * 1.1f,
                  "heading %g: arrow point %d is finite and compact", heading, i);
        {
            const float notch_back = (a[2].x - CX) * dx + (a[2].y - CY) * dy;
            const float wing_back = (a[1].x - CX) * dx + (a[1].y - CY) * dy;
            CHECK(notch_back > wing_back + 1e-3f, "heading %g: the rear is notched (the arrowhead is not a plain triangle)", heading);
            CHECK(((a[0].x - CX) * dx + (a[0].y - CY) * dy) > 0.0f, "heading %g: the tip faces the heading", heading);
        }
    }
    {
        minimap_point_t a[4], b[4];
        minimap_player_shape(CX, CY, 0.0f, -1.0f, k, a);
        minimap_player_shape(CX, CY, 1.0f, 0.0f, k, b);
        for (int i = 0; i < 4; i++)
            CHECK(near(hypotf(a[i].x - CX, a[i].y - CY), hypotf(b[i].x - CX, b[i].y - CY), 1e-3f),
                  "turning only rotates the arrow about the player (%d)", i);
    }
    CHECK(MM_PLAYER_K >= 0.08f && MM_PLAYER_K <= 0.13f, "the player arrow is not excessively large");
}

static void test_circle_resolution(void) {
    minimap_frame_t f;
    minimap_point_t pts[MM_CLIP_MAX];
    int n;
    frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
    CHECK(MM_CLIP_SIDES >= 128, "the circle is clipped with at least 128 sides");
    CHECK(MM_CLIP_MAX >= MM_CLIP_SIDES + 4, "the clip buffer holds the circle and a quad");
    n = minimap_clip_rect(&f, CX - 1000.0f, CY - 1000.0f, CX + 1000.0f, CY + 1000.0f, pts);
    CHECK(n >= MM_CLIP_SIDES - 2 && n <= MM_CLIP_MAX, "a covering rect becomes a %d-gon (%d)", MM_CLIP_SIDES, n);
    for (int i = 0; i < n; i++) {
        const float d = hypotf(pts[i].x - CX, pts[i].y - CY);
        CHECK(d <= RADIUS + 1e-2f && d >= RADIUS * 0.999f, "clip vertex %d lies on the circle (%.3f)", i, d);
    }
    {
        const float sagitta = RADIUS * (1.0f - cosf(3.14159265f / (float)MM_CLIP_SIDES));
        CHECK(sagitta < 0.1f, "the faceting deviates under 0.1 px at this size (%.3f)", sagitta);
    }
}

static void test_landmark_readability(void) {
    const minimap_level_map_t* m = minimap_map("SUM");
    minimap_frame_t f;
    int walls = 0;
    CHECK(m != NULL, "Summit has a map");
    if (!m) return;
    for (int i = 0; i < m->landmark_count; i++) {
        const minimap_landmark_t* lm = &m->landmarks[i];
        minimap_point_t pts[MM_CLIP_MAX], again[MM_CLIP_MAX];
        float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
        int n, n2;
        frame_at(&f, (lm->x[0] + lm->x[2]) * 0.5f, (lm->y[0] + lm->y[2]) * 0.5f, MM_ZOOM);
        n = minimap_landmark_polygon(&f, lm, pts);
        n2 = minimap_landmark_polygon(&f, lm, again);
        CHECK(n == n2 && !memcmp(pts, again, (size_t)n * sizeof pts[0]), "landmark %d draws deterministically", i);
        for (int k = 0; k < n; k++) {
            min_x = fminf(min_x, pts[k].x);
            max_x = fmaxf(max_x, pts[k].x);
            min_y = fminf(min_y, pts[k].y);
            max_y = fmaxf(max_y, pts[k].y);
            CHECK(isfinite(pts[k].x) && isfinite(pts[k].y), "landmark %d projects to finite points", i);
        }
        walls += lm->kind == MM_LM_WALL;
        CHECK(n >= 3 && fmaxf(max_x - min_x, max_y - min_y) >= MM_LANDMARK_MIN_PX, "landmark %d keeps a readable size", i);
    }
    CHECK(walls >= 40, "Summit shows its fences and walls (%d)", walls);
    {
        const minimap_landmark_t thin_wall = { { 0.0f, 20.0f, 20.0f, 0.0f }, { 0.0f, 0.0f, 0.2f, 0.2f }, MM_LM_WALL };
        const minimap_landmark_t thin_gate = { { 0.0f, 20.0f, 20.0f, 0.0f }, { 0.0f, 0.0f, 0.2f, 0.2f }, MM_LM_GATE };
        minimap_point_t pts[MM_CLIP_MAX];
        float wall_h = 0.0f, gate_h = 0.0f;
        frame_at(&f, 10.0f, 0.1f, MM_ZOOM_MIN);
        {
            float lo = 1e9f, hi = -1e9f;
            const int n = minimap_landmark_polygon(&f, &thin_wall, pts);
            for (int k = 0; k < n; k++) {
                lo = fminf(lo, pts[k].y);
                hi = fmaxf(hi, pts[k].y);
            }
            wall_h = n >= 3 ? hi - lo : 0.0f;
        }
        {
            float lo = 1e9f, hi = -1e9f;
            const int n = minimap_landmark_polygon(&f, &thin_gate, pts);
            for (int k = 0; k < n; k++) {
                lo = fminf(lo, pts[k].y);
                hi = fmaxf(hi, pts[k].y);
            }
            gate_h = n >= 3 ? hi - lo : 0.0f;
        }
        CHECK(near(wall_h, MM_LANDMARK_MIN_PX, 1e-3f), "a thin wall is widened to the minimum thickness (%.2f px)", wall_h);
        CHECK(near(gate_h, MM_GATE_MIN_PX, 1e-3f), "a gate is drawn thicker than a wall (%.2f px)", gate_h);
        CHECK(minimap_landmark_polygon(&f, NULL, pts) == 0, "no landmark: nothing drawn");
    }
}

static void test_icons(void) {
    minimap_frame_t f;
    minimap_marker_t m;
    const float reach = RADIUS / (2.0f * RADIUS * MM_ZOOM / MM_VIEW_SPAN_M);
    frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
    minimap_waypoint(&f, reach * 0.3f, 0.0f, &m);
    CHECK(m.icon == MM_ICON_DIAMOND, "an objective on the map is the world marker's diamond");
    minimap_waypoint(&f, reach * 3.0f, 0.0f, &m);
    CHECK(m.icon == MM_ICON_ARROW, "an objective off the map is the world marker's edge arrow");
    CHECK(MM_ZOOM == 2.0f, "the default zoom is exactly 2x");
}

static void test_world_marker_policy(void) {
    cp_scene_t s;
    cp_hud_t hud;
    memset(&s, 0, sizeof s);
    s.level = s.player = 1;
    CHECK(cp_world_marker_allowed(&s) && cp_hud_allowed(&s), "gameplay: marker and compass allowed");
    s.game_text = 1;
    CHECK(cp_world_marker_allowed(&s) && cp_minimap_allowed(&s) && !cp_hud_allowed(&s),
          "gameplay text hides the compass only: the world marker and minimap stay");
    s.menu = 1;
    CHECK(!cp_world_marker_allowed(&s), "a menu hides the world marker");
    s.menu = 0;
    s.loading = 1;
    CHECK(!cp_world_marker_allowed(&s), "loading hides the world marker");
    s.loading = 0;
    s.card = 1;
    CHECK(!cp_world_marker_allowed(&s), "the controls card hides the world marker");
    s.card = 0;
    s.level = 0;
    CHECK(!cp_world_marker_allowed(&s), "the frontend hides the world marker");

    for (int text = 0; text <= 1; text++) {
        dm_state_t st;
        dm_world_t w;
        cp_scene_t scene;
        cp_goal_t goal;
        memset(&st, 0, sizeof st);
        memset(&w, 0, sizeof w);
        memset(&scene, 0, sizeof scene);
        memset(&goal, 0, sizeof goal);
        scene.level = scene.player = 1;
        scene.game_text = text;
        w.place = DM_PLACE_LEVEL;
        w.player = w.eye = 1;
        w.at.rot[0] = w.eye_at.rot[0] = 1.0f;
        w.tan_half_h = 0.7f;
        w.eye_at.pos[0] = w.at.pos[0] = -1000.0f;
        w.eye_at.pos[1] = w.at.pos[1] = -500.0f;
        snprintf(w.level, sizeof w.level, "%s", "BE");
        goal.set = goal.target = 1;
        goal.pos[0] = -900.0f;
        goal.pos[1] = -400.0f;
        goal.route_count = 0;
        minimap_configure(1);
        dm_navigate(&st, &w, &scene, &goal, 1000u, &hud);
        CHECK(hud.marker == 1 && hud.view == 1 && hud.alpha > 0.0f, "text %d: the world marker is drawn", text);
        CHECK(hud.distance[0] != 0, "text %d: the world marker keeps its distance", text);
        CHECK(near(hud.target[0], goal.pos[0], 1e-3f) && near(hud.target[1], goal.pos[1], 1e-3f),
              "text %d: the world marker targets the objective", text);
        CHECK(hud.compass == !text, "text %d: the compass hides only while text is up", text);
        CHECK(hud.minimap == 1 && hud.waypoint == 1, "text %d: the minimap and its waypoint stay", text);
        scene.menu = 1;
        dm_navigate(&st, &w, &scene, &goal, 1000u, &hud);
        CHECK(hud.marker == 0 && hud.minimap == 0 && hud.compass == 0, "text %d: a menu hides every gameplay HUD", text);
    }
}

typedef struct {
    int polygons, points, bad;
    float worst, area;
} visit_stats_t;

static void count_polygon(void* user, const minimap_point_t* pts, int n) {
    visit_stats_t* v = user;
    float twice = 0.0f;
    v->polygons++;
    v->points += n;
    for (int i = 0; i < n; i++) {
        const minimap_point_t a = pts[i], b = pts[(i + 1) % n];
        const float d = hypotf(a.x - CX, a.y - CY);
        if (!isfinite(a.x) || !isfinite(a.y)) v->bad++;
        if (d > v->worst) v->worst = d;
        twice += a.x * b.y - b.x * a.y;
    }
    v->area += fabsf(twice) * 0.5f;
}

static void visit_layer(const char* level, int kind, float px, float py, float zoom, visit_stats_t* out) {
    minimap_frame_t f;
    memset(out, 0, sizeof *out);
    frame_at(&f, px, py, zoom);
    minimap_layer_visit(&f, minimap_bounds(level), minimap_map(level), kind, count_polygon, out);
}

typedef struct {
    int diagonal;
} shape_t;

static void note_diagonal(void* user, const minimap_point_t* pts, int n) {
    shape_t* s = user;
    for (int i = 0; i < n; i++) {
        const minimap_point_t a = pts[i], b = pts[(i + 1) % n];
        if (fabsf(a.x - b.x) > 0.05f && fabsf(a.y - b.y) > 0.05f && hypotf(a.x - CX, a.y - CY) < RADIUS * 0.95f &&
            hypotf(b.x - CX, b.y - CY) < RADIUS * 0.95f)
            s->diagonal++;
    }
}

static void test_layers(void) {
    const char* const* levels = k_levels;
    const float zooms[] = { MM_ZOOM_MIN, MM_ZOOM, MM_ZOOM_MAX };
    for (int l = 0; l < COUNT(k_levels); l++) {
        const minimap_bounds_t* b = minimap_bounds(levels[l]);
        const minimap_level_map_t* m = minimap_map(levels[l]);
        const minimap_run_t* big = NULL;
        float rx0, rs, rx1, rn;
        int land_polygons = 0;
        for (int i = 0; i < m->count; i++)
            if (m->runs[i].kind == MM_KIND_LAND && (!big || m->runs[i].len > big->len)) big = &m->runs[i];
        minimap_run_world(b, m, big, &rx0, &rs, &rx1, &rn);
        {
            const float px[] = { (rx0 + rx1) * 0.5f, b->min_x - 300.0f, b->max_x + 300.0f, b->min_x };
            const float py[] = { (rs + rn) * 0.5f, b->max_y + 300.0f, b->min_y - 300.0f, b->max_y };
            for (int p = 0; p < COUNT(px); p++) {
                for (int z = 0; z < COUNT(zooms); z++) {
                    for (int kind = MM_KIND_LAND; kind <= MM_KIND_WATER; kind++) {
                        visit_stats_t v;
                        visit_layer(levels[l], kind, px[p], py[p], zooms[z], &v);
                        CHECK(v.worst <= RADIUS + 1e-2f, "%s layer %d stays inside the circle (%.3f of %.0f)", levels[l], kind,
                              v.worst, RADIUS);
                        CHECK(v.bad == 0, "%s layer %d has no NaN or Inf", levels[l], kind);
                        CHECK(v.polygons < 20000, "%s layer %d stays cheap to draw (%d polygons)", levels[l], kind, v.polygons);
                        if (kind == MM_KIND_LAND && p == 0) land_polygons += v.polygons;
                    }
                }
            }
        }
        CHECK(land_polygons > 0, "%s draws land around a land cell", levels[l]);
    }
    {
        visit_stats_t a, b;
        const minimap_level_map_t* jr = minimap_map("JR");
        const minimap_run_t* big = NULL;
        float x0, south, x1, north;
        for (int i = 0; i < jr->count; i++)
            if (jr->runs[i].kind == MM_KIND_LAND && (!big || jr->runs[i].len > big->len)) big = &jr->runs[i];
        minimap_run_world(minimap_bounds("JR"), jr, big, &x0, &south, &x1, &north);
        visit_layer("JR", MM_KIND_LAND, (x0 + x1) * 0.5f, (south + north) * 0.5f, MM_ZOOM, &a);
        visit_layer("JR", MM_KIND_LAND, (x0 + x1) * 0.5f, (south + north) * 0.5f, MM_ZOOM, &b);
        CHECK(a.polygons == b.polygons && a.points == b.points && a.area == b.area, "the layer visit is deterministic");
        CHECK(a.area > 0.0f && a.area <= 3.1416f * RADIUS * RADIUS * 1.001f, "the filled area fits the circle (%.0f)", a.area);
    }
    {
        visit_stats_t v;
        const minimap_bounds_t* sum = minimap_bounds("SUM");
        visit_layer("SUM", MM_KIND_WATER, (sum->min_x + sum->max_x) * 0.5f, (sum->min_y + sum->max_y) * 0.5f, MM_ZOOM_MIN, &v);
        CHECK(v.polygons == 0, "Summit has no water to draw");
    }
    {
        minimap_frame_t f;
        visit_stats_t v;
        const minimap_level_map_t* jr = minimap_map("JR");
        frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
        memset(&v, 0, sizeof v);
        minimap_layer_visit(NULL, minimap_bounds("JR"), jr, MM_KIND_LAND, count_polygon, &v);
        minimap_layer_visit(&f, NULL, jr, MM_KIND_LAND, count_polygon, &v);
        minimap_layer_visit(&f, minimap_bounds("JR"), NULL, MM_KIND_LAND, count_polygon, &v);
        minimap_layer_visit(&f, minimap_bounds("JR"), jr, 0, count_polygon, &v);
        minimap_layer_visit(&f, minimap_bounds("JR"), jr, 9, count_polygon, &v);
        minimap_layer_visit(&f, minimap_bounds("JR"), jr, MM_KIND_LAND, NULL, &v);
        minimap_layer_visit(&f, minimap_bounds("XX"), minimap_map("XX"), MM_KIND_WATER, count_polygon, &v);
        CHECK(v.polygons == 0, "a missing frame, bounds, map, kind or callback draws nothing");
    }
    {
        /* a stair-stepped block: the contour must not be made only of axis-aligned edges */
        static const minimap_run_t runs[] = { { 2, 1, 2, MM_KIND_LAND }, { 2, 2, 3, MM_KIND_LAND }, { 2, 3, 4, MM_KIND_LAND },
                                              { 2, 4, 5, MM_KIND_LAND }, { 2, 5, 6, MM_KIND_LAND }, { 2, 6, 7, MM_KIND_LAND } };
        static const minimap_level_map_t stairs = { "ST", 10, 10, runs, 6, NULL, 0 };
        const minimap_bounds_t bounds = { "ST", 0.0f, 0.0f, 100.0f, 100.0f };
        minimap_frame_t f;
        shape_t shape = { 0 };
        minimap_frame(&f, CX, CY, RADIUS, 50.0f, 50.0f, MM_ZOOM_MIN);
        minimap_layer_visit(&f, &bounds, &stairs, MM_KIND_LAND, note_diagonal, &shape);
        CHECK(shape.diagonal > 0, "the contour is soft: it has slanted edges, not only square steps (%d)", shape.diagonal);
    }
    CHECK(MM_ISO_WATER < MM_ISO_LAND, "water keeps narrower features than land");
}

static void test_water_and_kinds(void) {
    const char* const* levels = k_levels;
    int water_levels = 0;
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_level_map_t* m = minimap_map(levels[i]);
        int water = 0, counts[4] = { 0, 0, 0, 0 };
        for (int r = 0; r < m->count; r++) water += m->runs[r].kind == MM_KIND_WATER;
        for (int l = 0; l < m->landmark_count; l++) {
            CHECK(m->landmarks[l].kind <= MM_LM_GATE, "%s landmark %d has a known kind", levels[i], l);
            if (m->landmarks[l].kind <= MM_LM_GATE) counts[m->landmarks[l].kind]++;
        }
        water_levels += water > 0;
        CHECK(m->landmark_count <= 900, "%s keeps a readable number of landmarks (%u)", levels[i], (unsigned)m->landmark_count);
    }
    CHECK(water_levels >= 8, "water is derived for most levels (%d of %d)", water_levels, COUNT(k_levels));
    {
        const minimap_level_map_t* it = minimap_map("IT");
        const minimap_level_map_t* sum = minimap_map("SUM");
        int it_kind[4] = { 0, 0, 0, 0 }, sum_kind[4] = { 0, 0, 0, 0 };
        for (int l = 0; l < it->landmark_count; l++) it_kind[it->landmarks[l].kind & 3]++;
        for (int l = 0; l < sum->landmark_count; l++) sum_kind[sum->landmarks[l].kind & 3]++;
        CHECK(it_kind[MM_LM_BUILDING] >= 50 && it_kind[MM_LM_WALL] >= 50 && it_kind[MM_LM_ROAD] >= 50,
              "the town keeps buildings, walls and roads (%d, %d, %d)", it_kind[0], it_kind[1], it_kind[2]);
        CHECK(sum_kind[MM_LM_BUILDING] >= 15 && sum_kind[MM_LM_WALL] >= 40, "Summit keeps its structures (%d buildings, %d walls)", sum_kind[0], sum_kind[1]);
        CHECK(minimap_map("LAB")->landmark_count >= 100, "the lab keeps its buildings");
    }
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_level_map_t* m = minimap_map(levels[i]);
        for (int l = 0; l < m->landmark_count; l++) {
            const minimap_landmark_t* lm = &m->landmarks[l];
            const float a = hypotf(lm->x[1] - lm->x[0], lm->y[1] - lm->y[0]), b = hypotf(lm->x[3] - lm->x[0], lm->y[3] - lm->y[0]);
            if (lm->kind == MM_LM_WALL) CHECK(fminf(a, b) <= 6.5f, "%s wall %d is thin", levels[i], l);
            if (lm->kind == MM_LM_BUILDING || lm->kind == MM_LM_GATE)
                CHECK(fminf(a, b) <= 45.0f && a * b <= 4200.0f, "%s landmark %d is not a giant false block (%.0f x %.0f)", levels[i], l, a, b);
            if (lm->kind == MM_LM_BUILDING)
                CHECK(fmaxf(a, b) < 4.0f * fminf(a, b) + 0.5f || fminf(a, b) > 5.5f, "%s building %d is a mass, not a wall", levels[i], l);
        }
    }
}

static int kind_at(const char* level, float x, float y, int kind) {
    const minimap_bounds_t* b = minimap_bounds(level);
    const minimap_level_map_t* m = minimap_map(level);
    int col, row;
    if (!b || !m) return 0;
    col = (int)((x - b->min_x) / ((b->max_x - b->min_x) / (float)m->cols));
    row = (int)((b->max_y - y) / ((b->max_y - b->min_y) / (float)m->rows));
    for (int i = 0; i < m->count; i++) {
        const minimap_run_t* run = &m->runs[i];
        if (run->kind == kind && run->row == row && col >= run->start && col < run->start + run->len) return 1;
    }
    return 0;
}

static void test_water_positions(void) {
    CHECK(kind_at("BE", -1833.4f, -986.3f, MM_KIND_WATER), "BE: the lagoon surface is water");
    CHECK(!kind_at("BE", -1883.9f, -831.6f, MM_KIND_WATER) && kind_at("BE", -1883.9f, -831.6f, MM_KIND_LAND),
          "BE: where Anne starts is land, not water");
    CHECK(kind_at("JR", -2475.6f, 26.8f, MM_KIND_WATER), "JR: the coastal pond is water");
    CHECK(!kind_at("JR", -2053.8f, 450.6f, MM_KIND_WATER), "JR: a dry stream bed is not water");
    CHECK(kind_at("BE", -2300.0f, -1500.0f, MM_KIND_WATER), "BE: the open sea is water");
    CHECK(!kind_at("XX", 0.0f, 0.0f, MM_KIND_WATER), "an unknown level has no water");
}

static void test_extra_maps(void) {
    static const char* const extra[] = { "TESTSCENE", "TESTSCNNGHT", "DEMO" };
    for (int i = 0; i < COUNT(extra); i++) {
        const minimap_bounds_t* b = minimap_bounds(extra[i]);
        const minimap_level_map_t* m = minimap_map(extra[i]);
        int land = 0;
        CHECK(b && m, "%s has a minimap", extra[i]);
        if (!b || !m) continue;
        for (int r = 0; r < m->count; r++) land += m->runs[r].kind == MM_KIND_LAND;
        CHECK(land > 0, "%s has land", extra[i]);
    }
    CHECK(minimap_map("testscene") != NULL, "a test scene's id is case-insensitive");
    CHECK(minimap_map("TESTSCENE2") == NULL, "a near miss is still unknown");
}

static void test_landmarks_in_level(void) {
    for (int i = 0; i < COUNT(k_levels); i++) {
        const minimap_bounds_t* b = minimap_bounds(k_levels[i]);
        const minimap_level_map_t* m = minimap_map(k_levels[i]);
        int bad = 0;
        if (!b || !m) continue;
        for (int l = 0; l < m->landmark_count; l++)
            for (int k = 0; k < 4; k++)
                bad += !(isfinite(m->landmarks[l].x[k]) && isfinite(m->landmarks[l].y[k]) &&
                         m->landmarks[l].x[k] > b->min_x - 250.0f && m->landmarks[l].x[k] < b->max_x + 250.0f &&
                         m->landmarks[l].y[k] > b->min_y - 250.0f && m->landmarks[l].y[k] < b->max_y + 250.0f);
        CHECK(bad == 0, "%s landmarks are finite and inside the level (%d outside)", k_levels[i], bad);
    }
}

static void test_toggle(void) {
    CHECK(minimap_toggle_allowed(1, 0, 0), "in gameplay with nothing open the toggle works");
    CHECK(!minimap_toggle_allowed(0, 0, 0), "outside gameplay (frontend, launcher, loading) it does not");
    CHECK(!minimap_toggle_allowed(1, 1, 0) && !minimap_toggle_allowed(1, 0, 1),
          "it does not work over a host menu or the controls card");
    minimap_configure(1);
    CHECK(minimap_toggle() == 0 && !minimap_enabled(), "toggling turns the minimap off");
    CHECK(minimap_toggle() == 1 && minimap_enabled(), "toggling again turns it on");
    minimap_configure(1);
}

static void test_released_keys(void) {
    static const unsigned freed[] = { 'M', 0xBB, 0xBD, 0x6B, 0x6D };
    for (int i = 0; i < COUNT(freed); i++) {
        CHECK(key_policy(0x100, freed[i], 0, 0, 0) == KEY_TO_GAME && key_policy(0x101, freed[i], 0, 0, 0) == KEY_TO_GAME,
              "key 0x%02X (M, + and -) goes to the game untouched", freed[i]);
        CHECK(key_policy(0x100, freed[i], 0, 1, 0) == KEY_TO_GAME && key_policy(0x100, freed[i], 1, 0, 0) == KEY_TO_GAME,
              "key 0x%02X with Ctrl or held goes to the game too", freed[i]);
    }
    CHECK(key_policy(0x100, 0x74, 0, 0, 0) == KEY_HOST_MINIMAP && key_policy(0x101, 0x74, 0, 0, 0) == KEY_DROP,
          "F5 is the minimap toggle and its key-up never reaches the game");
}

static void test_fixed_zoom(void) {
    minimap_frame_t f;
    CHECK(MM_ZOOM == 2.0f, "the minimap zoom is 2x");
    frame_at(&f, 0.0f, 0.0f, MM_ZOOM);
    CHECK(near(2.0f * RADIUS / f.scale, MM_VIEW_SPAN_M / MM_ZOOM, 1e-2f), "2x shows half of the base span");
    CHECK(near(minimap_creature_range(MM_ZOOM), 90.0f, 1e-3f), "the creature range follows the fixed zoom");
}

static void test_persistence(void) {
    char path[512];
    const char* key = JPT_KEY_MINIMAP;
    const char* off = "0";
    const char* on = "1";
    jpt_config_t c;
    FILE* f;
    snprintf(path, sizeof path, "%s", "minimap_tests_settings.ini");
    f = fopen(path, "w");
    CHECK(f != NULL, "a settings file for the test");
    if (!f) return;
    fclose(f);
    jpt_config_defaults(&c);
    CHECK(c.minimap == 1 && !strcmp(jpt_default_value(JPT_KEY_MINIMAP), "1") && !strcmp(JPT_KEY_MINIMAP, "minimap"),
          "the minimap is enabled by default under the key the launcher writes");
    CHECK(jpt_ini_set(path, &key, &off, 1) && jpt_config_load(path, &c) && c.minimap == 0, "minimap = 0 is read back");
    CHECK(jpt_ini_set(path, &key, &on, 1) && jpt_config_load(path, &c) && c.minimap == 1, "minimap = 1 is read back");
    CHECK(jpt_ini_set(path, &key, &off, 1) && jpt_config_load(path, &c) && c.minimap == 0, "a toggle survives a restart");
    remove(path);
}

static void test_run_world(void) {
    const minimap_bounds_t* b = minimap_bounds("BE");
    const minimap_level_map_t* m = minimap_map("BE");
    float x0, s0, x1, n0;
    minimap_run_t top = { 0, 1, 0, MM_KIND_LAND };
    CHECK(b && m, "BE exists");
    if (!b || !m) return;
    CHECK(minimap_run_world(b, m, &top, &x0, &s0, &x1, &n0), "a run has a world rect");
    CHECK(near(x0, b->min_x, 1e-3f) && near(n0, b->max_y, 1e-3f), "row 0, column 0 starts at the north-west corner");
    CHECK(x1 > x0 && n0 > s0, "the run rect is well formed (north up)");
    CHECK(!minimap_run_world(NULL, m, &top, &x0, &s0, &x1, &n0), "no bounds: no world rect");
}

static void test_heading_survives_text(void) {
    dm_state_t s;
    dm_world_t w;
    cp_scene_t scene;
    cp_hud_t hud;
    memset(&s, 0, sizeof s);
    memset(&w, 0, sizeof w);
    memset(&scene, 0, sizeof scene);
    scene.level = scene.player = 1;
    scene.game_text = 1;
    w.place = DM_PLACE_LEVEL;
    w.player = 1;
    w.eye = 1;
    w.at.rot[0] = 1.0f;
    w.eye_at.rot[0] = 0.70710678f;
    w.eye_at.rot[3] = 0.70710678f;
    snprintf(w.level, sizeof w.level, "%s", "BE");
    minimap_configure(1);
    dm_navigate(&s, &w, &scene, NULL, 1000u, &hud);
    CHECK(hud.minimap == 1 && hud.compass == 0, "gameplay text: the minimap draws with the compass hidden");
    CHECK(fabsf(hud.heading) > 1.0f && near(hud.heading, cp_heading(dm_yaw(w.eye_at.rot)), 1e-3f),
          "the minimap heading is available while the compass is hidden");
}

static void test_visibility(void) {
    cp_scene_t s;
    memset(&s, 0, sizeof s);
    s.level = 1;
    CHECK(cp_minimap_allowed(&s) && cp_hud_allowed(&s), "gameplay, no text: compass and minimap allowed");
    s.game_text = 1;
    CHECK(cp_minimap_allowed(&s) && !cp_hud_allowed(&s), "gameplay text hides the compass but not the minimap");
    s.game_text = 0;
    s.menu = 1;
    CHECK(!cp_minimap_allowed(&s) && !cp_hud_allowed(&s), "a host menu hides both");
    s.menu = 0;
    s.loading = 1;
    CHECK(!cp_minimap_allowed(&s) && !cp_hud_allowed(&s), "loading hides both");
    s.loading = 0;
    s.card = 1;
    CHECK(!cp_minimap_allowed(&s) && !cp_hud_allowed(&s), "the controls card hides both");
    s.card = 0;
    s.level = 0;
    CHECK(!cp_minimap_allowed(&s) && !cp_hud_allowed(&s), "the frontend hides both");
}

static void world_be(dm_state_t* s, dm_world_t* w, cp_scene_t* scene, cp_goal_t* goal) {
    memset(s, 0, sizeof *s);
    memset(w, 0, sizeof *w);
    memset(scene, 0, sizeof *scene);
    memset(goal, 0, sizeof *goal);
    scene->level = scene->player = 1;
    w->place = DM_PLACE_LEVEL;
    w->player = 1;
    w->eye = 1;
    w->at.pos[0] = -1000.0f;
    w->at.pos[1] = -500.0f;
    w->at.rot[0] = 1.0f;
    w->eye_at.rot[0] = 1.0f;
    snprintf(w->level, sizeof w->level, "%s", "BE");
    goal->set = 1;
    goal->target = 1;
    goal->pos[0] = -800.0f;
    goal->pos[1] = -300.0f;
    goal->pos[2] = 0.0f;
}

static void test_independent_visibility(void) {
    dm_state_t s;
    dm_world_t w;
    cp_scene_t scene;
    cp_goal_t goal;
    cp_hud_t hud;
    world_be(&s, &w, &scene, &goal);

    scene.game_text = 1; /* subtitles, narration, tutorial text or a gameplay message */
    minimap_configure(1);
    dm_navigate(&s, &w, &scene, &goal, 1000u, &hud);
    CHECK(hud.minimap == 1, "gameplay text: the minimap stays");
    CHECK(hud.waypoint == 1, "gameplay text: the waypoint stays");
    CHECK(hud.compass == 0, "gameplay text: the compass hides");
    CHECK(near(hud.waypoint_pos[0], goal.pos[0], 1e-3f) && near(hud.waypoint_pos[1], goal.pos[1], 1e-3f),
          "the waypoint carries the objective's world position");

    scene.game_text = 0;
    minimap_configure(0);
    dm_navigate(&s, &w, &scene, &goal, 1000u, &hud);
    CHECK(hud.minimap == 0 && hud.waypoint == 0, "minimap off: the minimap and its waypoint hide");
    CHECK(hud.compass == 1, "minimap off: the compass is unaffected");
    CHECK(hud.marker == 1, "minimap off: the 3D objective marker is unaffected");

    scene.game_text = 1;
    dm_navigate(&s, &w, &scene, &goal, 1000u, &hud);
    CHECK(hud.minimap == 0 && hud.compass == 0, "minimap off with text: the compass still hides by its own policy");
    minimap_configure(1);
}

void minimap_tests(int* checks, int* failures) {
    test_bounds();
    test_maps();
    test_same_transform();
    test_off_hides();
    test_player_centred();
    test_map_moves_under_player();
    test_heading();
    test_player_up();
    test_zoom_geometry();
    test_waypoint();
    test_place();
    test_landmarks();
    test_creatures();
    test_creature_policy();
    test_player_shape();
    test_circle_resolution();
    test_landmark_readability();
    test_icons();
    test_world_marker_policy();
    test_live_pipeline();
    test_clipping();
    test_layers();
    test_water_and_kinds();
    test_toggle();
    test_released_keys();
    test_fixed_zoom();
    test_persistence();
    test_water_positions();
    test_extra_maps();
    test_landmarks_in_level();
    test_colours();
    test_run_world();
    test_heading_survives_text();
    test_visibility();
    test_independent_visibility();
    *checks = g_checks;
    *failures = g_failures;
}
