#include "minimap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const minimap_bounds_t k_levels[MM_LEVEL_COUNT] = {
#include "minimap_bounds.inc"
};

#include "minimap_map.inc"

static int s_enabled = 1;
static minimap_source_fn s_source;

void minimap_set_source(minimap_source_fn source) { s_source = source; }

const minimap_bounds_t* minimap_retail_bounds(const char* level_id) {
    if (!level_id) return NULL;
    for (int i = 0; i < MM_LEVEL_COUNT; i++)
        if (k_levels[i].id && !_stricmp(k_levels[i].id, level_id)) return &k_levels[i];
    return NULL;
}

const minimap_level_map_t* minimap_retail_map(const char* level_id) {
    if (!level_id) return NULL;
    for (int i = 0; i < MM_LEVEL_COUNT; i++)
        if (k_maps[i].id && !_stricmp(k_maps[i].id, level_id)) return &k_maps[i];
    return NULL;
}

const minimap_bounds_t* minimap_bounds(const char* level_id) {
    const minimap_bounds_t* bounds;
    const minimap_level_map_t* map;
    if (s_source && s_source(level_id, &bounds, &map)) return bounds;
    return minimap_retail_bounds(level_id);
}

const minimap_level_map_t* minimap_map(const char* level_id) {
    const minimap_bounds_t* bounds;
    const minimap_level_map_t* map;
    if (s_source && s_source(level_id, &bounds, &map)) return map;
    return minimap_retail_map(level_id);
}

static float clamp_zoom(float zoom) {
    if (!(zoom >= MM_ZOOM_MIN)) return MM_ZOOM_MIN;
    return zoom > MM_ZOOM_MAX ? MM_ZOOM_MAX : zoom;
}

void minimap_frame(minimap_frame_t* f, float cx, float cy, float radius, float player_x, float player_y, float zoom) {
    f->cx = cx;
    f->cy = cy;
    f->radius = radius;
    f->scale = 2.0f * radius * clamp_zoom(zoom) / MM_VIEW_SPAN_M;
    f->player_x = player_x;
    f->player_y = player_y;
    f->up_x = 0.0f;
    f->up_y = 1.0f;
}

void minimap_face(minimap_frame_t* f, float heading_deg) {
    const double rad = (double)heading_deg * 0.017453292519943295;
    if (!isfinite(heading_deg)) return;
    f->up_x = (float)sin(rad);
    f->up_y = (float)cos(rad);
}

int minimap_place_facing(const minimap_rect_t* picture, float player_x, float player_y, float heading_deg, float zoom,
                         minimap_frame_t* out) {
    if (!minimap_place(picture, player_x, player_y, zoom, out)) return 0;
    minimap_face(out, heading_deg);
    return 1;
}

int minimap_place(const minimap_rect_t* picture, float player_x, float player_y, float zoom, minimap_frame_t* out) {
    float diameter, margin, limit;
    if (!picture || !out || !(picture->w > 0.0f) || !(picture->h > 0.0f)) return 0;
    limit = (picture->w < picture->h ? picture->w : picture->h) * 0.45f;
    diameter = picture->h * MM_HEIGHT_FRACTION;
    if (diameter > limit) diameter = limit;
    if (diameter < MM_DIAMETER_MIN) return 0;
    margin = picture->h * MM_MARGIN_FRACTION;
    minimap_frame(out, picture->x + margin + diameter * 0.5f, picture->y + picture->h - margin - diameter * 0.5f,
                  diameter * 0.5f, player_x, player_y, zoom);
    return 1;
}

void minimap_to_screen(const minimap_frame_t* f, float x, float y, float* sx, float* sy) {
    const float dx = x - f->player_x, dy = y - f->player_y;
    const float right = dx * f->up_y - dy * f->up_x, ahead = dx * f->up_x + dy * f->up_y;
    if (sx) *sx = f->cx + right * f->scale;
    if (sy) *sy = f->cy - ahead * f->scale;
}

int minimap_run_world(const minimap_bounds_t* b, const minimap_level_map_t* m, const minimap_run_t* run, float* min_x,
                      float* south_y, float* max_x, float* north_y) {
    float w, h;
    if (!b || !m || !run || !m->cols || !m->rows) return 0;
    w = b->max_x - b->min_x;
    h = b->max_y - b->min_y;
    if (!(w > 0.0f) || !(h > 0.0f)) return 0;
    *min_x = b->min_x + w * (float)run->start / (float)m->cols;
    *max_x = b->min_x + w * (float)(run->start + run->len) / (float)m->cols;
    *north_y = b->max_y - h * (float)run->row / (float)m->rows;
    *south_y = b->max_y - h * (float)(run->row + 1) / (float)m->rows;
    return 1;
}

typedef struct {
    float nx, ny;
} clip_normal_t;

static clip_normal_t s_normals[MM_CLIP_SIDES];
static int s_normals_ready;

static void prepare_normals(void) {
    if (s_normals_ready) return;
    for (int i = 0; i < MM_CLIP_SIDES; i++) {
        const double a = 6.283185307179586 * ((double)i + 0.5) / (double)MM_CLIP_SIDES;
        s_normals[i].nx = (float)cos(a);
        s_normals[i].ny = (float)sin(a);
    }
    s_normals_ready = 1;
}

/* The circle is clipped as an inscribed polygon: its vertices lie on the circle, so nothing is emitted outside it. */
int minimap_clip_polygon(const minimap_frame_t* f, const minimap_point_t* in, int count, minimap_point_t out[MM_CLIP_MAX]) {
    minimap_point_t a[MM_CLIP_MAX], b[MM_CLIP_MAX];
    minimap_point_t *src = a, *dst = b;
    const float apothem = f->radius * (float)cos(3.141592653589793 / (double)MM_CLIP_SIDES);
    float min_x, max_x, min_y, max_y;
    int n = count, inside = 1;
    if (!(f->radius > 0.0f) || count < 3 || count > MM_CLIP_MAX - MM_CLIP_SIDES) return 0;
    min_x = max_x = in[0].x;
    min_y = max_y = in[0].y;
    for (int i = 0; i < count; i++) {
        const float dx = in[i].x - f->cx, dy = in[i].y - f->cy;
        if (!isfinite(in[i].x) || !isfinite(in[i].y)) return 0;
        if (in[i].x < min_x) min_x = in[i].x;
        if (in[i].x > max_x) max_x = in[i].x;
        if (in[i].y < min_y) min_y = in[i].y;
        if (in[i].y > max_y) max_y = in[i].y;
        if (dx * dx + dy * dy > apothem * apothem) inside = 0;
        a[i] = in[i];
    }
    if (max_x <= f->cx - f->radius || min_x >= f->cx + f->radius || max_y <= f->cy - f->radius ||
        min_y >= f->cy + f->radius)
        return 0;
    if (inside) {
        memcpy(out, a, (size_t)count * sizeof *out);
        return count;
    }
    prepare_normals();
    for (int e = 0; e < MM_CLIP_SIDES && n > 0; e++) {
        const float nx = s_normals[e].nx, ny = s_normals[e].ny;
        int kept = 0;
        for (int i = 0; i < n; i++) {
            const minimap_point_t p = src[i], q = src[(i + 1) % n];
            const float dp = nx * (p.x - f->cx) + ny * (p.y - f->cy) - apothem;
            const float dq = nx * (q.x - f->cx) + ny * (q.y - f->cy) - apothem;
            if (dp <= 0.0f && kept < MM_CLIP_MAX) dst[kept++] = p;
            if ((dp <= 0.0f) != (dq <= 0.0f) && kept < MM_CLIP_MAX) {
                const float t = dp / (dp - dq);
                dst[kept].x = p.x + (q.x - p.x) * t;
                dst[kept].y = p.y + (q.y - p.y) * t;
                kept++;
            }
        }
        { minimap_point_t* t = src; src = dst; dst = t; }
        n = kept;
    }
    if (n < 3) return 0;
    memcpy(out, src, (size_t)n * sizeof *out);
    return n;
}

int minimap_clip_rect(const minimap_frame_t* f, float x0, float y0, float x1, float y1, minimap_point_t out[MM_CLIP_MAX]) {
    const minimap_point_t corners[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    return minimap_clip_polygon(f, corners, 4, out);
}

int minimap_landmark_polygon(const minimap_frame_t* f, const minimap_landmark_t* landmark, minimap_point_t out[MM_CLIP_MAX]) {
    minimap_point_t c[4];
    float ux, uy, vx, vy, lu, lv, mx, my, min_px;
    if (!landmark) return 0;
    min_px = landmark->kind == MM_LM_GATE ? MM_GATE_MIN_PX : MM_LANDMARK_MIN_PX;
    for (int i = 0; i < 4; i++) minimap_to_screen(f, landmark->x[i], landmark->y[i], &c[i].x, &c[i].y);
    ux = c[1].x - c[0].x;
    uy = c[1].y - c[0].y;
    vx = c[3].x - c[0].x;
    vy = c[3].y - c[0].y;
    lu = hypotf(ux, uy);
    lv = hypotf(vx, vy);
    if (!(lu > 1e-6f) || !(lv > 1e-6f)) return 0;
    if (lu < min_px || lv < min_px) {
        const float wu = fmaxf(lu, min_px), wv = fmaxf(lv, min_px);
        mx = (c[0].x + c[2].x) * 0.5f;
        my = (c[0].y + c[2].y) * 0.5f;
        ux = ux / lu * wu * 0.5f;
        uy = uy / lu * wu * 0.5f;
        vx = vx / lv * wv * 0.5f;
        vy = vy / lv * wv * 0.5f;
        c[0] = (minimap_point_t){ mx - ux - vx, my - uy - vy };
        c[1] = (minimap_point_t){ mx + ux - vx, my + uy - vy };
        c[2] = (minimap_point_t){ mx + ux + vx, my + uy + vy };
        c[3] = (minimap_point_t){ mx - ux + vx, my - uy + vy };
    }
    return minimap_clip_polygon(f, c, 4, out);
}

typedef struct {
    const minimap_level_map_t* map;
    float* field;
} layer_cache_t;

static layer_cache_t s_layers[2];

static void blur_pass(const float* in, float* out, int cols, int rows, int horizontal) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const int i = r * cols + c;
            const float before = horizontal ? (c > 0 ? in[i - 1] : 0.0f) : (r > 0 ? in[i - cols] : 0.0f);
            const float after = horizontal ? (c < cols - 1 ? in[i + 1] : 0.0f) : (r < rows - 1 ? in[i + cols] : 0.0f);
            out[i] = 0.25f * before + 0.5f * in[i] + 0.25f * after;
        }
    }
}

/* The layer's runs as a 0/1 grid, blurred once in each direction so a contour through it is soft. */
static float* build_field(const minimap_level_map_t* m, int kind) {
    const size_t count = (size_t)m->cols * m->rows;
    float* a = calloc(count, sizeof *a);
    float* b = calloc(count, sizeof *b);
    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    for (int i = 0; i < m->count; i++) {
        const minimap_run_t* run = &m->runs[i];
        if (run->kind != kind || run->row >= m->rows) continue;
        for (int c = run->start; c < run->start + run->len && c < m->cols; c++) a[(size_t)run->row * m->cols + c] = 1.0f;
    }
    blur_pass(a, b, m->cols, m->rows, 1);
    blur_pass(b, a, m->cols, m->rows, 0);
    free(b);
    return a;
}

static const float* layer_field(const minimap_level_map_t* m, int kind) {
    layer_cache_t* cache = &s_layers[kind - 1];
    if (cache->map != m || !cache->field) {
        free(cache->field);
        cache->field = build_field(m, kind);
        cache->map = m;
    }
    return cache->field;
}

typedef struct {
    const minimap_frame_t* frame;
    const minimap_bounds_t* bounds;
    const float* field;
    int cols, rows;
    float cw, ch, iso;
    minimap_poly_fn fn;
    void* user;
} layer_t;

static float field_at(const layer_t* l, int c, int r) {
    return c < 0 || c >= l->cols || r < 0 || r >= l->rows ? 0.0f : l->field[r * l->cols + c];
}

static float sample_x(const layer_t* l, int c) { return l->bounds->min_x + ((float)c + 0.5f) * l->cw; }
static float sample_y(const layer_t* l, int r) { return l->bounds->max_y - ((float)r + 0.5f) * l->ch; }

static void emit_world(const layer_t* l, const float* wx, const float* wy, int count) {
    minimap_point_t screen[8], out[MM_CLIP_MAX];
    int n;
    for (int i = 0; i < count; i++) minimap_to_screen(l->frame, wx[i], wy[i], &screen[i].x, &screen[i].y);
    n = minimap_clip_polygon(l->frame, screen, count, out);
    if (n >= 3) l->fn(l->user, out, n);
}

/* A contour crossing is always computed from the same end of an edge, so neighbouring cells share exact vertices. */
static void emit_cell(const layer_t* l, int i, int j) {
    const float x0 = sample_x(l, i), x1 = sample_x(l, i + 1), y0 = sample_y(l, j), y1 = sample_y(l, j + 1);
    const float cx[4] = { x0, x1, x1, x0 }, cy[4] = { y0, y0, y1, y1 };
    const float v[4] = { field_at(l, i, j), field_at(l, i + 1, j), field_at(l, i + 1, j + 1), field_at(l, i, j + 1) };
    const int from[4] = { 0, 1, 3, 0 }, to[4] = { 1, 2, 2, 3 };
    float wx[8], wy[8];
    int n = 0;
    for (int k = 0; k < 4; k++) {
        const int next = (k + 1) & 3;
        const int in = v[k] >= l->iso, in_next = v[next] >= l->iso;
        if (in) {
            wx[n] = cx[k];
            wy[n] = cy[k];
            n++;
        }
        if (in != in_next) {
            const int a = from[k], b = to[k];
            const float t = (l->iso - v[a]) / (v[b] - v[a]);
            wx[n] = cx[a] + (cx[b] - cx[a]) * t;
            wy[n] = cy[a] + (cy[b] - cy[a]) * t;
            n++;
        }
    }
    if (n >= 3) emit_world(l, wx, wy, n);
}

static void emit_strip(const layer_t* l, int from_col, int to_col, int j) {
    const float wx[4] = { sample_x(l, from_col), sample_x(l, to_col), sample_x(l, to_col), sample_x(l, from_col) };
    const float wy[4] = { sample_y(l, j), sample_y(l, j), sample_y(l, j + 1), sample_y(l, j + 1) };
    emit_world(l, wx, wy, 4);
}

void minimap_layer_visit(const minimap_frame_t* f, const minimap_bounds_t* b, const minimap_level_map_t* m, int kind,
                         minimap_poly_fn fn, void* user) {
    layer_t l;
    float span_x, span_y, half;
    int c0, c1, r0, r1;
    if (!f || !b || !m || !fn || (kind != MM_KIND_LAND && kind != MM_KIND_WATER)) return;
    if (!(f->radius > 0.0f) || !(f->scale > 0.0f) || !m->cols || !m->rows) return;
    span_x = b->max_x - b->min_x;
    span_y = b->max_y - b->min_y;
    if (!(span_x > 0.0f) || !(span_y > 0.0f)) return;
    l.field = layer_field(m, kind);
    if (!l.field) return;
    l.frame = f;
    l.bounds = b;
    l.cols = m->cols;
    l.rows = m->rows;
    l.cw = span_x / (float)l.cols;
    l.ch = span_y / (float)l.rows;
    l.iso = kind == MM_KIND_WATER ? MM_ISO_WATER : MM_ISO_LAND;
    l.fn = fn;
    l.user = user;
    half = f->radius / f->scale;
    c0 = (int)floorf((f->player_x - half - b->min_x) / l.cw - 0.5f);
    c1 = (int)floorf((f->player_x + half - b->min_x) / l.cw - 0.5f);
    r0 = (int)floorf((b->max_y - (f->player_y + half)) / l.ch - 0.5f);
    r1 = (int)floorf((b->max_y - (f->player_y - half)) / l.ch - 0.5f);
    if (c0 < -1) c0 = -1;
    if (r0 < -1) r0 = -1;
    if (c1 > l.cols - 1) c1 = l.cols - 1;
    if (r1 > l.rows - 1) r1 = l.rows - 1;
    for (int j = r0; j <= r1; j++) {
        int run = -1;
        for (int i = c0; i <= c1 + 1; i++) {
            const int inside = i <= c1;
            const float a = inside ? field_at(&l, i, j) : 0.0f, bq = inside ? field_at(&l, i + 1, j) : 0.0f;
            const float c = inside ? field_at(&l, i + 1, j + 1) : 0.0f, d = inside ? field_at(&l, i, j + 1) : 0.0f;
            const int full = inside && a >= l.iso && bq >= l.iso && c >= l.iso && d >= l.iso;
            if (full) {
                if (run < 0) run = i;
                continue;
            }
            if (run >= 0) {
                emit_strip(&l, run, i, j);
                run = -1;
            }
            if (inside && (a >= l.iso || bq >= l.iso || c >= l.iso || d >= l.iso)) emit_cell(&l, i, j);
        }
    }
}

void minimap_waypoint(const minimap_frame_t* f, float x, float y, minimap_marker_t* out) {
    float sx, sy, dx, dy, d, anchor = f->radius * MM_EDGE_ANCHOR, clear = f->radius * MM_WAYPOINT_CLEAR, at;
    minimap_to_screen(f, x, y, &sx, &sy);
    dx = sx - f->cx;
    dy = sy - f->cy;
    d = sqrtf(dx * dx + dy * dy);
    if (d > 1e-3f) {
        dx /= d;
        dy /= d;
    } else {
        dx = 0.0f;
        dy = -1.0f;
        d = 0.0f;
    }
    out->inside = d <= anchor;
    out->icon = out->inside ? MM_ICON_DIAMOND : MM_ICON_ARROW;
    at = d > anchor ? anchor : d < clear ? clear : d;
    out->x = f->cx + dx * at;
    out->y = f->cy + dy * at;
    out->dir_x = dx;
    out->dir_y = dy;
}

minimap_creature_kind_t minimap_creature_kind(int dead, int engaged) {
    if (dead) return MM_CREATURE_NONE;
    return engaged ? MM_CREATURE_THREAT : MM_CREATURE_PASSIVE;
}

float minimap_creature_range(float zoom) {
    float range = MM_VIEW_SPAN_M / clamp_zoom(zoom) * 0.5f * MM_CREATURE_RANGE_VIEW;
    if (range < MM_CREATURE_RANGE_MIN_M) range = MM_CREATURE_RANGE_MIN_M;
    return range > MM_CREATURE_RANGE_MAX_M ? MM_CREATURE_RANGE_MAX_M : range;
}

int minimap_creature_engaged(int active, int have_distance, float distance, float keep_radius) {
    if (!active) return 0;
    if (!have_distance || !isfinite(distance) || !(keep_radius > 0.0f) || !(keep_radius < 1.0e6f)) return 1;
    return distance <= keep_radius;
}

int minimap_creatures_select(float player_x, float player_y, float range, const minimap_creature_t* in, int count,
                             minimap_creature_t* out, int max) {
    float dist[MM_CREATURES_MAX];
    int n = 0;
    if (!in || !out || max <= 0 || !(range > 0.0f)) return 0;
    if (max > MM_CREATURES_MAX) max = MM_CREATURES_MAX;
    for (int i = 0; i < count; i++) {
        const float dx = in[i].x - player_x, dy = in[i].y - player_y, d = sqrtf(dx * dx + dy * dy);
        int at;
        if (!isfinite(in[i].x) || !isfinite(in[i].y) || !(d <= range)) continue;
        if (n == max && d >= dist[n - 1]) continue;
        for (at = n < max ? n : max - 1; at > 0 && dist[at - 1] > d; at--) {
            dist[at] = dist[at - 1];
            out[at] = out[at - 1];
        }
        dist[at] = d;
        out[at] = in[i];
        if (n < max) n++;
    }
    return n;
}

int minimap_creature_size_class(float scale) {
    if (!isfinite(scale) || !(scale > 0.0f)) return MM_SIZE_STANDARD;
    if (scale < MM_SCALE_STANDARD) return 0;
    if (scale < MM_SCALE_MEDIUM) return 1;
    return scale < MM_SCALE_LARGE ? 2 : 3;
}

float minimap_creature_marker_radius(float minimap_radius, int size_class) {
    static const float factor[MM_SIZE_CLASSES] = MM_CREATURE_SIZE_FACTORS;
    if (size_class < 0 || size_class >= MM_SIZE_CLASSES) size_class = MM_SIZE_STANDARD;
    return minimap_radius * MM_CREATURE_K * factor[size_class];
}

int minimap_vehicle_polygon(const minimap_frame_t* f, const minimap_vehicle_t* v, minimap_point_t out[MM_CLIP_MAX]) {
    static const float length_k[MM_VEHICLE_CLASSES] = MM_VEHICLE_LENGTH_K, width_k[MM_VEHICLE_CLASSES] = MM_VEHICLE_WIDTH_K,
                       min_length[MM_VEHICLE_CLASSES] = MM_VEHICLE_MIN_LENGTH_PX, min_width[MM_VEHICLE_CLASSES] = MM_VEHICLE_MIN_WIDTH_PX;
    float cx, cy, dx, dy, half_length, half_width, axis, limit;
    int size;
    minimap_point_t c[4];
    if (!f || !v || !isfinite(v->x) || !isfinite(v->y) || !isfinite(v->ax) || !isfinite(v->ay)) return 0;
    axis = hypotf(v->ax, v->ay);
    if (!(axis > 1e-6f)) return 0;
    minimap_to_screen(f, v->x, v->y, &cx, &cy);
    dx = (v->ax * f->up_y - v->ay * f->up_x) / axis;
    dy = -(v->ax * f->up_x + v->ay * f->up_y) / axis;
    size = v->size >= 0 && v->size < MM_VEHICLE_CLASSES ? v->size : MM_VEHICLE_SMALL;
    half_length = fmaxf(f->radius * length_k[size], min_length[size]) * 0.5f;
    half_width = fmaxf(f->radius * width_k[size], min_width[size]) * 0.5f;
    c[0] = (minimap_point_t){ cx - dx * half_length + dy * half_width, cy - dy * half_length - dx * half_width };
    c[1] = (minimap_point_t){ cx + dx * half_length + dy * half_width, cy + dy * half_length - dx * half_width };
    c[2] = (minimap_point_t){ cx + dx * half_length - dy * half_width, cy + dy * half_length + dx * half_width };
    c[3] = (minimap_point_t){ cx - dx * half_length - dy * half_width, cy - dy * half_length + dx * half_width };
    limit = f->radius - MM_VEHICLE_OUTLINE_PX;
    for (int i = 0; i < 4; i++)
        if (hypotf(c[i].x - f->cx, c[i].y - f->cy) > limit) return 0;
    memcpy(out, c, sizeof c);
    return 4;
}

int minimap_creature_place(const minimap_frame_t* f, const minimap_creature_t* creature, float* sx, float* sy) {
    float x, y;
    if (!creature || !isfinite(creature->x) || !isfinite(creature->y)) return 0;
    minimap_to_screen(f, creature->x, creature->y, &x, &y);
    if (hypotf(x - f->cx, y - f->cy) >
        f->radius - minimap_creature_marker_radius(f->radius, creature->size) - MM_CREATURE_OUTLINE_PX)
        return 0;
    if (sx) *sx = x;
    if (sy) *sy = y;
    return 1;
}

void minimap_heading_dir(float heading_deg, float* dx, float* dy) {
    const float rad = heading_deg * 0.0174532925f;
    *dx = sinf(rad);
    *dy = -cosf(rad);
}

void minimap_player_shape(float cx, float cy, float dx, float dy, float k, minimap_point_t out[4]) {
    static const float local[4][2] = { { 1.0f, 0.0f }, { -0.78f, -0.72f }, { -0.38f, 0.0f }, { -0.78f, 0.72f } };
    /* area centroid of the tip-wing-notch and tip-notch-wing triangles, so the arrow pivots on the player */
    const float centroid = ((local[0][0] + local[1][0] + local[2][0]) + (local[0][0] + local[2][0] + local[3][0])) / 6.0f;
    for (int i = 0; i < 4; i++) {
        const float fwd = (local[i][0] - centroid) * k, side = local[i][1] * k;
        out[i].x = cx + dx * fwd - dy * side;
        out[i].y = cy + dy * fwd + dx * side;
    }
}

void minimap_arrow(float cx, float cy, float dx, float dy, float k, minimap_point_t out[3]) {
    out[0] = (minimap_point_t){ cx + dx * k, cy + dy * k };
    out[1] = (minimap_point_t){ cx - dx * k * 0.5f - dy * k * 0.5f, cy - dy * k * 0.5f + dx * k * 0.5f };
    out[2] = (minimap_point_t){ cx - dx * k * 0.5f + dy * k * 0.5f, cy - dy * k * 0.5f - dx * k * 0.5f };
}

int minimap_toggle_allowed(int gameplay, int menu_open, int card_shown) {
    return gameplay && !menu_open && !card_shown;
}

int minimap_toggle(void) {
    s_enabled = !s_enabled;
    return s_enabled;
}

void minimap_configure(int enabled) { s_enabled = enabled != 0; }

int minimap_enabled(void) { return s_enabled; }

int minimap_picture_run(const minimap_frame_t* f, const minimap_picture_t* p, int row, int from, int to, minimap_point_t out[MM_CLIP_MAX]) {
    const float w = (p->x1 - p->x0) / (float)p->cols, h = (p->y1 - p->y0) / (float)p->rows;
    const float left = p->x0 + w * (float)from, right = p->x0 + w * (float)to, top = p->y0 + h * (float)row, bottom = top + h;
    const float world[4][2] = { { left, top }, { right, top }, { right, bottom }, { left, bottom } };
    const float reach = f->radius / f->scale;
    minimap_point_t corners[4];
    if ((left < f->player_x - reach && right < f->player_x - reach) || (left > f->player_x + reach && right > f->player_x + reach)) return 0;
    if ((top < f->player_y - reach && bottom < f->player_y - reach) || (top > f->player_y + reach && bottom > f->player_y + reach)) return 0;
    for (int i = 0; i < 4; i++) minimap_to_screen(f, world[i][0], world[i][1], &corners[i].x, &corners[i].y);
    return minimap_clip_polygon(f, corners, 4, out);
}
