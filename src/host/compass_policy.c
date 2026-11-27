#include "compass_policy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jpt_tr.h"

#define DEG_PER_RAD 57.29577951308232
#define EDGE_FADE_FROM 0.7f
#define NEAR_M 0.25f

float cp_wrap360(float deg) {
    double r = fmod((double)deg, 360.0);
    if (r < 0.0) r += 360.0;
    return r >= 360.0 ? 0.0f : (float)r;
}

float cp_wrap180(float deg) {
    double r = fmod((double)deg, 360.0);
    if (r <= -180.0) r += 360.0;
    if (r > 180.0) r -= 360.0;
    return (float)r;
}

float cp_heading(float yaw) { return cp_wrap360((float)(-(double)yaw * DEG_PER_RAD)); }

float cp_bearing(const float from[3], const float to[3]) {
    double dx = (double)to[0] - from[0], dy = (double)to[1] - from[1];
    return dx == 0.0 && dy == 0.0 ? 0.0f : cp_wrap360((float)(atan2(dx, dy) * DEG_PER_RAD));
}

float cp_relative(float bearing, float heading) { return cp_wrap180(bearing - heading); }

float cp_distance_m(const float from[3], const float to[3]) {
    double dx = (double)to[0] - from[0], dy = (double)to[1] - from[1];
    return (float)(sqrt(dx * dx + dy * dy) * CP_METRES_PER_UNIT);
}

const char* cp_direction_name(int deg) {
    static const char* const names[8] = { "Compass::N", "Compass::NE", "Compass::E", "Compass::SE",
                                          "Compass::S", "Compass::SW", "Compass::W", "Compass::NW" };
    deg = ((deg % 360) + 360) % 360;
    return deg % 45 ? NULL : jpt_tr(names[deg / 45]);
}

int cp_format_distance(float metres, char* out, size_t cap) {
    int n;
    if (!(metres > 0.0f)) metres = 0.0f;
    n = metres < 999.5f ? snprintf(out, cap, jpt_tr("%d m"), (int)(metres + 0.5f))
                        : snprintf(out, cap, jpt_tr("%.1f km"), metres / 1000.0f);
    return n > 0 && (size_t)n < cap;
}

static int s_compass = 1, s_waypoints = 1;

void cp_configure(int compass, int waypoints) {
    s_compass = compass != 0;
    s_waypoints = waypoints != 0;
}

int cp_compass_enabled(void) { return s_compass; }

int cp_waypoints_enabled(void) { return s_waypoints; }

int cp_hud_allowed(const cp_scene_t* s) { return s->level && !s->menu && !s->card && !s->loading && !s->game_text; }

int cp_minimap_allowed(const cp_scene_t* s) { return s->level && !s->menu && !s->card && !s->loading; }

int cp_world_marker_allowed(const cp_scene_t* s) { return cp_minimap_allowed(s); }

cp_text_kind_t cp_text_kind(uint32_t category, int host_prompt, int first_y, int raster_h) {
    if (category == CP_TEXT_CATEGORY_SUBTITLE) return CP_TEXT_SUBTITLE;
    if (host_prompt) return CP_TEXT_HOST_PROMPT;
    return (float)first_y < CP_TEXT_ZONE * (float)raster_h ? CP_TEXT_GAME_PROMPT : CP_TEXT_MESSAGE;
}

int cp_text_hides_hud(cp_text_kind_t kind) { return kind == CP_TEXT_HOST_PROMPT || kind == CP_TEXT_GAME_PROMPT; }

int cp_reminder_press(cp_reminder_t* r, uint32_t now_ms, const cp_goal_t* goal) {
    if (!goal || !goal->set || !goal->title[0]) {
        memset(r, 0, sizeof *r);
        return 0;
    }
    r->active = 1;
    r->start_ms = now_ms;
    snprintf(r->title, sizeof r->title, "%s", goal->title);
    return 1;
}

void cp_hud_reminder(cp_hud_t* hud, const cp_goal_t* goal, float visibility) {
    if (!(hud->alpha > 0.0f) || !(visibility > 0.0f) || !goal || !goal->set || !goal->title[0]) return;
    snprintf(hud->objective, sizeof hud->objective, "%s", goal->title);
    hud->objective_alpha = visibility > 1.0f ? 1.0f : visibility;
}

static float shown_for(uint32_t age) {
    return age + CP_REMINDER_FADE_MS <= CP_REMINDER_MS ? 1.0f : (float)(CP_REMINDER_MS - age) / (float)CP_REMINDER_FADE_MS;
}

int cp_map_view_press(cp_map_view_t* v, uint32_t now_ms, int has_map) {
    v->active = has_map != 0;
    v->start_ms = now_ms;
    return v->active;
}

float cp_map_view_step(cp_map_view_t* v, uint32_t now_ms, int has_map) {
    const uint32_t age = now_ms - v->start_ms;
    if (!v->active || !has_map || age >= CP_REMINDER_MS) {
        v->active = 0;
        return 0.0f;
    }
    return shown_for(age);
}

static float clamp01(float v, int* outside) {
    if (v < 0.0f || v > 1.0f) *outside = 1;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

int cp_map_cursor(const minimap_picture_t* map, float x, float y, float out[2], int* outside) {
    const float w = map ? map->x1 - map->x0 : 0.0f, h = map ? map->y1 - map->y0 : 0.0f;
    *outside = 0;
    if (!map || !isfinite(x) || !isfinite(y) || !(fabsf(w) > 0.0f) || !(fabsf(h) > 0.0f)) return 0;
    out[0] = clamp01((x - map->x0) / w, outside);
    out[1] = clamp01((y - map->y0) / h, outside);
    return 1;
}

void cp_map_box(const cp_layout_t* l, float view_h, int cols, int rows, float* x, float* y, float* w, float* h) {
    const float side = CP_MAP_SIDE * view_h, aspect = cols > 0 && rows > 0 ? (float)cols / (float)rows : 1.0f;
    *w = aspect >= 1.0f ? side : side * aspect;
    *h = aspect >= 1.0f ? side / aspect : side;
    *x = l->centre - *w * 0.5f;
    *y = l->notice_y + l->small_px * 1.6f;
}

float cp_reminder_step(cp_reminder_t* r, uint32_t now_ms, const cp_goal_t* goal) {
    uint32_t age;
    if (!r->active) return 0.0f;
    if (!goal || !goal->set || !goal->title[0]) {
        memset(r, 0, sizeof *r);
        return 0.0f;
    }
    if (strcmp(goal->title, r->title)) {
        if (!s_waypoints) {
            memset(r, 0, sizeof *r);
            return 0.0f;
        }
        snprintf(r->title, sizeof r->title, "%s", goal->title);
        r->start_ms = now_ms;
    }
    age = now_ms - r->start_ms;
    if (age >= CP_REMINDER_MS) {
        memset(r, 0, sizeof *r);
        return 0.0f;
    }
    return shown_for(age);
}

static float at_least(float v, float floor) { return v < floor ? floor : v; }

void cp_layout(float view_w, float view_h, cp_layout_t* out) {
    float width = fminf(0.40f * view_w, 0.50f * view_h) * CP_SCALE;
    memset(out, 0, sizeof *out);
    out->centre = view_w * 0.5f;
    out->left = out->centre - width * 0.5f;
    out->right = out->centre + width * 0.5f;
    out->top = 0.025f * view_h;
    out->height = 0.036f * view_h * CP_SCALE;
    out->label_px = at_least(0.026f * view_h * CP_SCALE, 11.0f);
    out->small_px = at_least(0.019f * view_h * CP_SCALE, 10.0f);
    out->objective_px = out->small_px * CP_OBJECTIVE_SCALE;
    out->line_px = at_least(view_h / 540.0f, 1.0f);
    out->text_y = out->top + out->height + 0.008f * view_h;
    out->objective_y = out->text_y + out->small_px * 1.6f;
    out->notice_y = out->objective_y + out->objective_px * 1.45f;
}

void cp_layout_in(float x, float y, float w, float h, cp_layout_t* out) {
    float lift;
    cp_layout(w, h, out);
    lift = out->top;
    out->left += x;
    out->right += x;
    out->centre += x;
    out->top = y;
    out->text_y += y - lift;
    out->objective_y += y - lift;
    out->notice_y += y - lift;
}

float cp_outline_px(const cp_layout_t* l) { return at_least(roundf(l->line_px), 1.0f); }

float cp_strip_x(const cp_layout_t* l, float relative, int* side) {
    float half = (l->right - l->left) * 0.5f;
    *side = 0;
    if (fabsf(relative) <= CP_HALF_SPAN_DEG) return l->centre + relative / CP_HALF_SPAN_DEG * half;
    *side = relative > 0.0f ? 1 : -1;
    return *side > 0 ? l->right : l->left;
}

float cp_edge_fade(float relative) {
    float t = fabsf(relative) / CP_HALF_SPAN_DEG;
    if (t <= EDGE_FADE_FROM) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    return 1.0f - (t - EDGE_FADE_FROM) / (1.0f - EDGE_FADE_FROM);
}

int cp_ticks(float heading, cp_tick_t* out, int max) {
    int n = 0, first = (int)ceilf((heading - CP_HALF_SPAN_DEG) / CP_TICK_DEG) * CP_TICK_DEG;
    for (int a = first; (float)a <= heading + CP_HALF_SPAN_DEG && n < max; a += CP_TICK_DEG) {
        int deg = ((a % 360) + 360) % 360;
        float relative = cp_relative((float)a, heading);
        if (fabsf(relative) > CP_HALF_SPAN_DEG) continue;
        out[n].relative = relative;
        out[n].name = cp_direction_name(deg);
        out[n].cardinal = deg % 90 == 0;
        out[n].north = deg % 360 == 0;
        n++;
    }
    return n;
}

/* The game's CRotate3 applied to a vector (q v q*), with the conjugate taking world to camera. */
static void rotate(const float q[4], int inverse, const float v[3], float out[3]) {
    double a = q[0], b = inverse ? -q[1] : q[1], c = inverse ? -q[2] : q[2], d = inverse ? -q[3] : q[3];
    double k = a * a - 0.5;
    out[0] = (float)(2.0 * ((b * b + k) * v[0] + (c * b - d * a) * v[1] + (c * a + d * b) * v[2]));
    out[1] = (float)(2.0 * ((d * a + b * c) * v[0] + (c * c + k) * v[1] + (d * c - b * a) * v[2]));
    out[2] = (float)(2.0 * ((d * b - c * a) * v[0] + (b * a + c * d) * v[1] + (d * d + k) * v[2]));
}

void cp_to_camera(const cp_camera_t* c, const float world[3], float out[3]) {
    float v[3] = { world[0] - c->pos[0], world[1] - c->pos[1], world[2] - c->pos[2] };
    rotate(c->rot, 1, v, out);
}

int cp_project(const cp_camera_t* c, float view_w, float view_h, float margin, const float world[3], cp_screen_t* out) {
    float v[3], focal, cx = view_w * 0.5f, cy = view_h * 0.5f, hw = cx - margin, hh = cy - margin, dx, dy, len, t;
    memset(out, 0, sizeof *out);
    if (!(c->tan_half_h > 0.0f) || view_w <= 2.0f * margin || view_h <= 2.0f * margin) return 0;
    cp_to_camera(c, world, v);
    focal = cy / c->tan_half_h;
    out->depth = v[1];
    if (v[1] > NEAR_M) {
        out->x = cx + focal * v[0] / v[1];
        out->y = cy - focal * v[2] / v[1];
        if (fabsf(out->x - cx) <= hw && fabsf(out->y - cy) <= hh) {
            out->on_screen = 1;
            return 1;
        }
        dx = out->x - cx;
        dy = out->y - cy;
    } else {
        dx = v[0];
        dy = -v[2];
        if (fabsf(dx) < 1e-4f && fabsf(dy) < 1e-4f) dy = 1.0f;
        if (fabsf(dx) < 0.25f * fabsf(dy) && dy < 0.0f) dy = -dy;
    }
    len = sqrtf(dx * dx + dy * dy);
    out->dir_x = dx / len;
    out->dir_y = dy / len;
    t = fminf(out->dir_x != 0.0f ? hw / fabsf(out->dir_x) : 1e9f, out->dir_y != 0.0f ? hh / fabsf(out->dir_y) : 1e9f);
    out->x = cx + out->dir_x * t;
    out->y = cy + out->dir_y * t;
    return 1;
}

int cp_project_in(const cp_camera_t* c, float x, float y, float w, float h, float margin, const float world[3],
                  cp_screen_t* out) {
    int ok = cp_project(c, w, h, margin, world, out);
    out->x += x;
    out->y += y;
    return ok;
}

int cp_route_next(const float route[][3], int count, const float at[3]) {
    int nearest = 0;
    float best = 0.0f;
    if (count <= 0) return 0;
    for (int i = 0; i < count; i++) {
        float d = cp_distance_m(at, route[i]);
        if (!i || d < best) {
            best = d;
            nearest = i;
        }
    }
    if (best <= CP_ROUTE_REACH_M) return nearest + 1;
    if (nearest + 1 < count && cp_distance_m(at, route[nearest + 1]) < cp_distance_m(route[nearest], route[nearest + 1]))
        return nearest + 1;
    return nearest;
}
