#include "jpt_sky.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_compat.h"

/* Trespasser CE's own dimensions. */
#define DOME_RADIUS 3500.0f
#define DOME_RINGS 6
#define DOME_COLUMNS 24
#define DOME_COLUMN_DEGREES (-15.0f)
#define DOME_TOP 1400.0f
#define DOME_UV_BASE 400.0f
#define DOME_HIGHEST 0.750879f /* 1 - cos of the last ring */
#define BODY_HALF 250.0f
#define STRIP_COLUMNS 25
#define STRIP_UNIT 100.0f
#define TEXELS 256.0f
#define LEAST_HEIGHT 100.0f
#define NEAREST 0.5f
#define SIDE_GUARD 1.02f
#define CLIP_PLANES 5
#define CLIP_VERTICES 16
#define LINE_MAX_BYTES 512
#define PI 3.14159265358979323846f

/* ---- the file ---- */

static float clampf(float v, float lowest, float highest) { return v >= lowest ? (v <= highest ? v : highest) : lowest; }

static char* trimmed(char* s) {
    size_t n;
    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* A quoted value keeps its ';'; otherwise one starts a comment. */
static char* value_of(char* s) {
    char* end;
    s = trimmed(s);
    if (*s == '"') {
        end = strchr(++s, '"');
        if (end) *end = 0;
        return s;
    }
    end = strchr(s, ';');
    if (end) *end = 0;
    return trimmed(s);
}

static float number(const char* s, float fallback) {
    char* end;
    const float v = strtof(s, &end);
    return end != s && v == v && v > -FLT_MAX && v < FLT_MAX ? v : fallback;
}

static int one_of(const char* value, const char* const* names, int count) {
    for (int i = 0; i < count; i++)
        if (!_stricmp(value, names[i])) return i;
    return -1;
}

static const char* const k_types[] = { "DOME", "BODY", "STRIP", "STARS", "METEORS" };
static const char* const k_blends[] = { "OPAQUE", "ALPHA", "BURN", "2X_ALPHA", "2X_BURN" };

const char* jpt_sky_type_name(jpt_sky_type_t type) { return (unsigned)type < 5u ? k_types[type] : ""; }

static void defaults(jpt_sky_layer_t* l, int number_of) {
    float* const colors[] = { l->top, l->horizon, l->horizon2, l->bottom, l->color };
    memset(l, 0, sizeof *l);
    l->number = number_of;
    l->type = (jpt_sky_type_t)-1;
    l->fogged = 1;
    for (int c = 0; c < 5; c++)
        for (int i = 0; i < 4; i++) colors[c][i] = 1.0f;
    l->gradient = l->horizon_gradient = l->uv_scale = l->width_scale = l->height_scale = 1.0f;
    l->curvature = 0.02f;
    l->top_z = 10.0f;
    l->scroll_speed = FLT_MAX;
}

static int color_key(jpt_sky_layer_t* l, const char* key, const char* value) {
    static const struct {
        const char* name;
        size_t at;
    } k[] = { { "ColorTop", offsetof(jpt_sky_layer_t, top) },
              { "ColorHorizon2", offsetof(jpt_sky_layer_t, horizon2) },
              { "ColorHorizon", offsetof(jpt_sky_layer_t, horizon) },
              { "ColorBottom", offsetof(jpt_sky_layer_t, bottom) },
              { "Color", offsetof(jpt_sky_layer_t, color) } };
    static const char k_channels[] = "RGBA";
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        const size_t n = strlen(k[i].name);
        for (int c = 0; c < 4; c++) {
            float* channel = (float*)((char*)l + k[i].at) + c;
            if (_strnicmp(key, k[i].name, n) || (key[n] & ~0x20) != k_channels[c] || key[n + 1]) continue;
            if (k[i].at == offsetof(jpt_sky_layer_t, horizon2) && !l->second_horizon) {
                memcpy(l->horizon2, l->horizon, sizeof l->horizon2);
                l->second_horizon = 1;
            }
            *channel = clampf(number(value, *channel * 255.0f), 0.0f, 255.0f) / 255.0f;
            return 1;
        }
    }
    return 0;
}

static void layer_key(jpt_sky_layer_t* l, const char* key, const char* value) {
    static const struct {
        const char* name;
        size_t at;
        float lowest, highest;
    } k[] = {
        { "GradientCurve", offsetof(jpt_sky_layer_t, gradient), 0.01f, 100.0f },
        { "HorizonGradientCurve", offsetof(jpt_sky_layer_t, horizon_gradient), 0.01f, 100.0f },
        { "HorizonColor2Rotation", offsetof(jpt_sky_layer_t, horizon_rotation), -FLT_MAX, FLT_MAX },
        { "UVScale", offsetof(jpt_sky_layer_t, uv_scale), -FLT_MAX, FLT_MAX },
        { "OffsetU", offsetof(jpt_sky_layer_t, offset_u), -FLT_MAX, FLT_MAX },
        { "OffsetV", offsetof(jpt_sky_layer_t, offset_v), -FLT_MAX, FLT_MAX },
        { "UVScrollSpeed", offsetof(jpt_sky_layer_t, scroll_speed), -FLT_MAX, FLT_MAX },
        { "Curvature", offsetof(jpt_sky_layer_t, curvature), 0.0f, 0.8f },
        { "HeightOffset", offsetof(jpt_sky_layer_t, height_offset), -FLT_MAX, FLT_MAX },
        { "HorizonHeightBias", offsetof(jpt_sky_layer_t, horizon_bias), 0.0f, 1.0f },
        { "AbsoluteHeightStrength", offsetof(jpt_sky_layer_t, absolute_height), 0.0f, FLT_MAX },
        { "WidthScale", offsetof(jpt_sky_layer_t, width_scale), -FLT_MAX, FLT_MAX },
        { "HeightScale", offsetof(jpt_sky_layer_t, height_scale), -FLT_MAX, FLT_MAX },
        { "Elevation", offsetof(jpt_sky_layer_t, elevation), -FLT_MAX, FLT_MAX },
        { "Azimuth", offsetof(jpt_sky_layer_t, azimuth), -FLT_MAX, FLT_MAX },
        { "TopZ", offsetof(jpt_sky_layer_t, top_z), -FLT_MAX, FLT_MAX },
        { "BottomZ", offsetof(jpt_sky_layer_t, bottom_z), -FLT_MAX, FLT_MAX },
        { "TopFog", offsetof(jpt_sky_layer_t, top_fog), 0.0f, 1.0f },
        { "BottomFog", offsetof(jpt_sky_layer_t, bottom_fog), 0.0f, 1.0f },
        { "VerticalParallaxRangeZ", offsetof(jpt_sky_layer_t, parallax_range), 0.0f, FLT_MAX },
        { "VerticalParallaxAmplitude", offsetof(jpt_sky_layer_t, parallax_amplitude), 0.0f, FLT_MAX },
    };
    int index;
    if (!_stricmp(key, "Type")) {
        l->type = (jpt_sky_type_t)one_of(value, k_types, 5);
    } else if (!_stricmp(key, "BlendMode")) {
        if ((index = one_of(value, k_blends, 5)) >= 0) l->blend = (jpt_sky_blend_t)index;
    } else if (!_stricmp(key, "Texture")) {
        snprintf(l->texture, sizeof l->texture, "%s", value);
    } else if (!_stricmp(key, "Fogged")) {
        l->fogged = number(value, 1.0f) != 0.0f;
    } else if (!_stricmp(key, "FogAsAlpha")) {
        l->fog_as_alpha = number(value, 0.0f) != 0.0f;
    } else if (!color_key(l, key, value)) {
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
            float* field = (float*)((char*)l + k[i].at);
            if (!_stricmp(key, k[i].name)) *field = clampf(number(value, *field), k[i].lowest, k[i].highest);
        }
    }
}

static void sky_key(jpt_sky_t* s, const char* key, const char* value) {
    if (!_stricmp(key, "FogNear")) s->has_fog_near = 1, s->fog_near = clampf(number(value, 0.0f), 0.0f, 1.0f);
    if (!_stricmp(key, "FogFar")) s->has_fog_far = 1, s->fog_far = number(value, 1.0f);
    if (!_stricmp(key, "Height")) s->has_height = 1, s->height = clampf(number(value, 0.0f), 0.0f, FLT_MAX);
}

/* 1 to JPT_SKY_LAYERS_MAX for "LAYER<n>", 0 for "SKY", -1 for another section. */
static int section_of(const char* name) {
    char* end;
    long n;
    if (!_stricmp(name, "SKY")) return 0;
    if (_strnicmp(name, "LAYER", 5)) return -1;
    n = strtol(name + 5, &end, 10);
    return end != name + 5 && !*end && n >= 1 && n <= JPT_SKY_LAYERS_MAX ? (int)n : -1;
}

int jpt_sky_parse(const char* text, size_t size, jpt_sky_t* out) {
    jpt_sky_layer_t* layers = (jpt_sky_layer_t*)malloc(JPT_SKY_LAYERS_MAX * sizeof *layers);
    const char *p = text, *end = text + size;
    int section = -1;
    memset(out, 0, sizeof *out);
    if (!layers) return 0;
    for (int i = 0; i < JPT_SKY_LAYERS_MAX; i++) defaults(&layers[i], i + 1);
    while (p < end) {
        const char* newline = (const char*)memchr(p, '\n', (size_t)(end - p));
        const size_t n = (size_t)((newline ? newline : end) - p);
        char line[LINE_MAX_BYTES], *key, *equals;
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p = newline ? newline + 1 : end;
        key = trimmed(line);
        if (*key == '[') {
            char* close = strchr(key, ']');
            if (close) *close = 0;
            section = section_of(trimmed(key + 1));
            continue;
        }
        equals = *key == ';' ? NULL : strchr(key, '=');
        if (!equals || section < 0) continue;
        *equals = 0;
        key = trimmed(key);
        if (section) layer_key(&layers[section - 1], key, value_of(equals + 1));
        else sky_key(out, key, value_of(equals + 1));
    }
    for (int i = 0; i < JPT_SKY_LAYERS_MAX; i++) {
        jpt_sky_layer_t* l = &layers[i];
        if ((int)l->type < 0) continue;
        if (l->scroll_speed == FLT_MAX) l->scroll_speed = l->type == JPT_SKY_DOME ? 1.0f : 0.0f;
        l->fog_as_alpha &= l->fogged;
        out->layers[out->count++] = *l;
    }
    free(layers);
    return out->count > 0;
}

int jpt_sky_drawn(const jpt_sky_layer_t* l) {
    if (l->type == JPT_SKY_BODY || l->type == JPT_SKY_STRIP) return l->texture[0] && l->texture[0] != '$';
    return l->type == JPT_SKY_DOME && l->texture[0] != '$';
}

static int burns(const jpt_sky_layer_t* l) { return l->blend == JPT_SKY_BURN || l->blend == JPT_SKY_BURN_2X; }

void jpt_sky_pass(const jpt_sky_layer_t* l, jpt_sky_pass_t* out) {
    out->twice = l->blend == JPT_SKY_ALPHA_2X || l->blend == JPT_SKY_BURN_2X;
    out->blend = burns(l) ? JPT_SKY_BURN : out->twice ? JPT_SKY_ALPHA : l->blend;
    out->fog = !burns(l) && (l->type == JPT_SKY_STRIP || l->fogged) && !l->fog_as_alpha;
    out->wrap_u = l->type != JPT_SKY_BODY;
    out->wrap_v = l->type == JPT_SKY_DOME;
}

/* ---- the shapes ---- */

static float radians(float degrees) { return degrees * PI / 180.0f; }

static float fog_at(float value, float near_at, float far_at) {
    if (value <= near_at) return 0.0f;
    if (value >= far_at) return 1.0f;
    return (value - near_at) / (far_at - near_at);
}

typedef struct {
    float near_at, far_at, height;
} effective_t;

static effective_t effective(const jpt_sky_t* sky, const jpt_sky_moment_t* m) {
    effective_t e;
    e.near_at = sky->has_fog_near ? sky->fog_near : m->fog_near;
    e.far_at = sky->has_fog_far ? sky->fog_far : m->fog_far;
    e.height = sky->has_height ? sky->height : m->height;
    return e;
}

static void dome_color(const jpt_sky_layer_t* l, const float dir[3], float cosine, float out[4]) {
    const float g = powf(clampf((1.0f - cosine) / DOME_HIGHEST, 0.0f, 1.0f), l->gradient);
    float second = 0.0f;
    if (l->second_horizon) {
        const float turn = radians(-l->horizon_rotation), length = cosine > 0.0f ? cosine : 1.0f;
        const float x = (dir[0] * cosf(turn) - dir[1] * sinf(turn)) / length;
        second = 1.0f - powf(clampf(0.5f + 0.5f * x, 0.0f, 1.0f), l->horizon_gradient);
    }
    for (int i = 0; i < 4; i++) {
        const float horizon = l->horizon[i] + (l->horizon2[i] - l->horizon[i]) * second;
        out[i] = horizon + (l->top[i] - horizon) * g;
    }
}

static void dome_indices(jpt_sky_shape_t* out) {
    int n = 0;
    for (int ring = 0; ring + 1 < DOME_RINGS; ring++)
        for (int c = 0; c < DOME_COLUMNS; c++) {
            const int a = ring * DOME_COLUMNS + c, b = ring * DOME_COLUMNS + (c + 1) % DOME_COLUMNS;
            const int six[6] = { a, b, a + DOME_COLUMNS, b, b + DOME_COLUMNS, a + DOME_COLUMNS };
            for (int i = 0; i < 6; i++) out->indices[n++] = (uint16_t)six[i];
        }
    for (int c = 1; c + 1 < DOME_COLUMNS; c++) {
        const int first = (DOME_RINGS - 1) * DOME_COLUMNS, three[3] = { first, first + c, first + c + 1 };
        for (int i = 0; i < 3; i++) out->indices[n++] = (uint16_t)three[i];
    }
    out->index_count = n;
}

static void dome(const jpt_sky_layer_t* l, const jpt_sky_moment_t* m, const effective_t* e, jpt_sky_shape_t* out) {
    static const float k_sine[DOME_RINGS] = { 0.0f, 0.268838f, 0.515382f, 0.721388f, 0.874639f, 0.968472f };
    static const float k_cosine[DOME_RINGS] = { 1.0f, 0.963185f, 0.856961f, 0.692532f, 0.484775f, 0.249121f };
    const float bias = l->horizon_bias * DOME_TOP, base = bias + l->curvature * (DOME_TOP - bias), rise = DOME_TOP - base;
    const float scale = m->texture_scale / TEXELS * l->uv_scale;
    const float level = l->absolute_height != 0.0f ? fmaxf(LEAST_HEIGHT, e->height - m->camera[2] * l->absolute_height) : e->height;
    const float height = level + l->height_offset;
    const float scroll_u = fmodf(m->seconds * l->scroll_speed * m->wind[0] / TEXELS, 1.0f);
    const float scroll_v = fmodf(m->seconds * l->scroll_speed * m->wind[1] / TEXELS, 1.0f);
    const float u0 = fmodf(m->camera[0] * scale, 1.0f) + scroll_u + l->offset_u;
    const float v0 = fmodf(m->camera[1] * scale, 1.0f) + scroll_v + l->offset_v;
    out->vertex_count = DOME_RINGS * DOME_COLUMNS;
    for (int ring = 0; ring < DOME_RINGS; ring++)
        for (int c = 0; c < DOME_COLUMNS; c++) {
            jpt_sky_vertex_t* v = &out->vertices[ring * DOME_COLUMNS + c];
            const float angle = radians(DOME_COLUMN_DEGREES * (float)c);
            const float dir[3] = { k_cosine[ring] * cosf(angle), k_cosine[ring] * sinf(angle), k_sine[ring] };
            const float depth = (DOME_UV_BASE - bias) + base + rise * dir[2];
            const float t = depth != 0.0f ? (height + DOME_UV_BASE - bias) / depth : 0.0f;
            const float fog = l->fogged ? fog_at(k_cosine[ring], e->near_at, e->far_at) : 0.0f;
            v->position[0] = dir[0] * DOME_RADIUS;
            v->position[1] = dir[1] * DOME_RADIUS;
            v->position[2] = bias + (DOME_TOP - bias) * dir[2];
            v->u = dir[0] * DOME_RADIUS * t * scale + u0;
            v->v = dir[1] * DOME_RADIUS * t * scale + v0;
            v->clear = 1.0f - fog;
            dome_color(l, dir, k_cosine[ring], v->color);
            if (l->fog_as_alpha) v->color[3] *= 1.0f - fog;
        }
    dome_indices(out);
}

/* Up by the elevation, then round by the azimuth. */
static void body_corner(const jpt_sky_layer_t* l, float x, float z, float out[3]) {
    const float el = radians(l->elevation), az = radians(l->azimuth);
    const float y = DOME_RADIUS * cosf(el) - z * sinf(el);
    out[2] = DOME_RADIUS * sinf(el) + z * cosf(el);
    out[0] = x * cosf(az) - y * sinf(az);
    out[1] = x * sinf(az) + y * cosf(az);
}

static int body(const jpt_sky_layer_t* l, const effective_t* e, float aspect, jpt_sky_shape_t* out) {
    static const uint16_t k_indices[6] = { 0, 1, 2, 0, 2, 3 };
    const float half_x = BODY_HALF * l->width_scale, half_z = BODY_HALF * l->height_scale * aspect;
    float corner[4][3], v[2] = { 0.0f, 1.0f };
    body_corner(l, -half_x, half_z, corner[0]);
    body_corner(l, half_x, half_z, corner[1]);
    body_corner(l, half_x, -half_z, corner[2]);
    body_corner(l, -half_x, -half_z, corner[3]);
    if (corner[0][2] < 0.0f && corner[2][2] < 0.0f) return 0;
    for (int edge = 0; edge < 2; edge++) {
        const int at = edge ? 2 : 0, other = edge ? 0 : 2;
        const float part = corner[at][2] < 0.0f ? corner[at][2] / (corner[at][2] - corner[other][2]) : 0.0f;
        if (part <= 0.0f) continue;
        for (int i = 0; i < 3; i++) {
            corner[at][i] += (corner[edge ? 1 : 3][i] - corner[at][i]) * part;
            corner[at + 1][i] += (corner[edge ? 0 : 2][i] - corner[at + 1][i]) * part;
        }
        v[edge] += (v[1 - edge] - v[edge]) * part;
    }
    out->vertex_count = 4;
    for (int i = 0; i < 4; i++) {
        jpt_sky_vertex_t* p = &out->vertices[i];
        const float* c = corner[i];
        const float length = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        const float fog = l->fogged && length > 0.0f ? fog_at(1.0f - fmaxf(c[2] / length, 0.0f), e->near_at, e->far_at) : 0.0f;
        memcpy(p->position, c, sizeof p->position);
        p->u = i == 1 || i == 2 ? 1.0f : 0.0f;
        p->v = v[i >= 2];
        p->clear = 1.0f - fog;
        memcpy(p->color, l->color, sizeof p->color);
        if (burns(l)) p->color[3] *= 1.0f - fog;
    }
    memcpy(out->indices, k_indices, sizeof k_indices);
    out->index_count = 6;
    return 1;
}

static void strip(const jpt_sky_layer_t* l, const jpt_sky_moment_t* m, jpt_sky_shape_t* out) {
    const float scroll = l->offset_u + fmodf(m->seconds * l->scroll_speed, 1.0f);
    const float shift = l->parallax_range > 0.0f ? clampf(m->camera[2] / l->parallax_range, -1.0f, 1.0f) * l->parallax_amplitude * STRIP_UNIT : 0.0f;
    out->vertex_count = 2 * STRIP_COLUMNS;
    out->index_count = 0;
    for (int c = 0; c < STRIP_COLUMNS; c++) {
        const float angle = radians(15.0f * (float)c);
        for (int bottom = 0; bottom < 2; bottom++) {
            jpt_sky_vertex_t* v = &out->vertices[2 * c + bottom];
            v->position[0] = cosf(angle) * DOME_RADIUS;
            v->position[1] = sinf(angle) * DOME_RADIUS;
            v->position[2] = (bottom ? l->bottom_z : l->top_z) * STRIP_UNIT - shift;
            v->u = (float)c / (float)(STRIP_COLUMNS - 1) * l->uv_scale + scroll;
            v->v = (float)bottom;
            v->clear = 1.0f - (bottom ? l->bottom_fog : l->top_fog);
            memcpy(v->color, bottom ? l->bottom : l->top, sizeof v->color);
        }
        if (c + 1 < STRIP_COLUMNS) {
            const int a = 2 * c, six[6] = { a, a + 1, a + 2, a + 2, a + 1, a + 3 };
            for (int i = 0; i < 6; i++) out->indices[out->index_count++] = (uint16_t)six[i];
        }
    }
}

int jpt_sky_shape(const jpt_sky_t* sky, const jpt_sky_layer_t* l, const jpt_sky_moment_t* m, float aspect, jpt_sky_shape_t* out) {
    const effective_t e = effective(sky, m);
    out->vertex_count = out->index_count = 0;
    if (!jpt_sky_drawn(l)) return 0;
    if (l->type == JPT_SKY_BODY) return body(l, &e, aspect > 0.0f ? aspect : 1.0f, out);
    if (l->type == JPT_SKY_DOME) dome(l, m, &e, out);
    else strip(l, m, out);
    return 1;
}

/* ---- the screen ---- */

typedef struct {
    float x, y, w, u, v, color[4], clear; /* x, y: of the screen's half width and height, by w */
} clip_vertex_t;

static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static float inside(const clip_vertex_t* v, int plane) {
    switch (plane) {
    case 0: return v->w - NEAREST;
    case 1: return SIDE_GUARD * v->w - v->x;
    case 2: return SIDE_GUARD * v->w + v->x;
    case 3: return SIDE_GUARD * v->w - v->y;
    default: return SIDE_GUARD * v->w + v->y;
    }
}

static clip_vertex_t between(const clip_vertex_t* a, const clip_vertex_t* b, float t) {
    clip_vertex_t out;
    const float *from = (const float*)a, *to = (const float*)b;
    float* o = (float*)&out;
    for (size_t i = 0; i < sizeof out / sizeof(float); i++) o[i] = from[i] + (to[i] - from[i]) * t;
    return out;
}

static int clip(clip_vertex_t* polygon, int count, int plane) {
    clip_vertex_t out[CLIP_VERTICES];
    int n = 0;
    for (int i = 0; i < count && n + 2 <= CLIP_VERTICES; i++) {
        const clip_vertex_t *a = &polygon[i], *b = &polygon[(i + 1) % count];
        const float da = inside(a, plane), db = inside(b, plane);
        if (da >= 0.0f) out[n++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) out[n++] = between(a, b, da / (da - db));
    }
    memcpy(polygon, out, (size_t)n * sizeof *out);
    return n;
}

static uint32_t channel(float v) { return (uint32_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

static void to_screen(const clip_vertex_t* c, const jpt_sky_view_t* view, jpt_sky_screen_vertex_t* out) {
    const float rhw = 1.0f / c->w;
    out->x = view->left + (c->x * rhw + 1.0f) * 0.5f * (view->right - view->left);
    out->y = view->top + (c->y * rhw + 1.0f) * 0.5f * (view->bottom - view->top);
    out->z = 0.5f;
    out->rhw = rhw;
    out->diffuse = channel(c->color[3]) << 24 | channel(c->color[0]) << 16 | channel(c->color[1]) << 8 | channel(c->color[2]);
    out->specular = channel(c->clear) << 24;
    out->u = c->u;
    out->v = c->v;
}

int jpt_sky_project(const jpt_sky_shape_t* shape, const jpt_sky_view_t* view, jpt_sky_screen_vertex_t* out, int cap) {
    clip_vertex_t all[JPT_SKY_SHAPE_VERTICES], polygon[CLIP_VERTICES];
    float right[3], down[3], forward[3], rr, dd, ff;
    int n = 0;
    for (int i = 0; i < 3; i++) {
        right[i] = (view->top_right[i] - view->top_left[i]) * 0.5f;
        down[i] = (view->bottom_left[i] - view->top_left[i]) * 0.5f;
        forward[i] = (view->top_left[i] + view->bottom_right[i]) * 0.5f;
    }
    rr = dot(right, right), dd = dot(down, down), ff = dot(forward, forward);
    if (!(rr > 0.0f) || !(dd > 0.0f) || !(ff > 0.0f)) return 0;
    for (int i = 0; i < shape->vertex_count; i++) {
        const jpt_sky_vertex_t* v = &shape->vertices[i];
        clip_vertex_t* c = &all[i];
        c->x = dot(v->position, right) / rr;
        c->y = dot(v->position, down) / dd;
        c->w = dot(v->position, forward) / ff;
        c->u = v->u, c->v = v->v;
        memcpy(c->color, v->color, sizeof c->color);
        c->clear = v->clear;
    }
    for (int i = 0; i + 2 < shape->index_count; i += 3) {
        int count = 3;
        for (int k = 0; k < 3; k++) polygon[k] = all[shape->indices[i + k]];
        for (int plane = 0; plane < CLIP_PLANES && count >= 3; plane++) count = clip(polygon, count, plane);
        for (int k = 1; k + 1 < count && n + 3 <= cap; k++) {
            to_screen(&polygon[0], view, &out[n++]);
            to_screen(&polygon[k], view, &out[n++]);
            to_screen(&polygon[k + 1], view, &out[n++]);
        }
    }
    return n;
}
