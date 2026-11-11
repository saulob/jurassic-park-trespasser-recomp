/* DEBUG_SAULO: terrain and opaque-object batch diagnostics for the D3D11 renderer (see terrain_diag.h). */
#include "terrain_diag.h"
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define GRID_W 80
#define GRID_H 60
#define MAX_SLOTS 4096
#define MAX_FINGERPRINTS 12
#define FRAME_TEXTURES 512
#define CENSUS_INTERVAL 5.0
#define UPLOAD_LOG_LIMIT 6
#define BLACK_UPLOAD_LOG_LIMIT 12
#define DRAW_LOG_FRAMES 2
#define DRAW_LOG_PER_FRAME 4
#define VERTEX_LOG_DRAWS 2
#define VERTEX_LOG_LIMIT 8
#define CELL_SAMPLE_LIMIT 4
#define NEAR_BLACK 12u
#define UNTEXTURED_LOG_LIMIT 4
#define WATCH_LIMIT 8
#define WATCH_WRITE_LOG_LIMIT 12
#define WATCH_UPLOAD_LOG_LIMIT 8
#define WATER_LOG_LIMIT 6
#define OBJECT_DRAW_LOG_PER_FRAME 3
/* Retail RTTI vtables and the rptr null object. */
#define VT_CRASTER_D3D 0x0064DE24u
#define RPTR_NULL 0x00669B90u

typedef struct {
    const dd_surface_t* tex;
    uint32_t caller, fingerprint, fog_color;
    unsigned ordinal, triangles;
    unsigned long content_gen;
    unsigned char terrain, kind, water, blended, tmb, cull;
} slot_t;

typedef struct {
    float z, u, v;
    uint32_t texel, expected, under;
    uint16_t owner, blend_owner;
    unsigned char fog, has_texel, under_kind;
} cell_t;

typedef struct {
    unsigned long long draws, triangles, vertices, untextured, culled, bad, rhw_nonpositive, z_outside,
        diffuse_not_white, distinct_textures, uploads, upload_pixels, upload_black, created, freed;
    float min_z, max_z, min_rhw, max_rhw, min_u, max_u, min_v, max_v, min_x, max_x, min_y, max_y;
    unsigned min_fog, max_fog;
    uint32_t fog_color;
    unsigned long long fog_color_changes;
    unsigned long long object_draws, object_triangles, object_water_draws, object_untextured, water_updates;
    int seen;
} window_t;

typedef struct {
    const dd_surface_t* s;
    unsigned long first_write, first_upload, first_draw, logged_content, logged_uploaded;
} watch_t;

static struct {
    int flags, object_flags, object_skip;
    unsigned long frame;
    window_t w;
    watch_t watch[WATCH_LIMIT];
    unsigned watch_count, watch_writes_logged, watch_uploads_logged, water_logged, object_draws_logged;
    uint32_t water_rasters[WATER_LOG_LIMIT];
    const void* census_pixels;
    size_t census_pitch;
    unsigned census_w, census_h;
    struct {
        uint32_t fp;
        unsigned long long draws;
    } fps[MAX_FINGERPRINTS];
    unsigned fp_count;
    unsigned long long fp_overflow;
    const dd_surface_t* frame_tex[FRAME_TEXTURES];
    unsigned frame_tex_count;
    int armed;
    unsigned census_frames, terrain_ordinal, nslots, uploads_logged, black_uploads_logged, untextured_logged;
    int overflow;
    double last_census;
    slot_t slots[MAX_SLOTS];
    cell_t cells[GRID_H][GRID_W];
    unsigned short cell_dark[GRID_H][GRID_W], cell_pixels[GRID_H][GRID_W];
} T;

int tdiag_parse_option(const char* arg) {
    static const struct {
        const char* name;
        int* target;
        int flag;
    } options[] = { { "--debug-terrain-wireframe", &T.flags, TDIAG_WIREFRAME },
                    { "--debug-terrain-solid", &T.flags, TDIAG_SOLID },
                    { "--debug-terrain-texture", &T.flags, TDIAG_TEXTURE },
                    { "--debug-terrain-nocull", &T.flags, TDIAG_NOCULL },
                    { "--debug-b560-solid", &T.object_flags, TDIAG_SOLID },
                    { "--debug-b560-texture", &T.object_flags, TDIAG_TEXTURE },
                    { "--debug-b560-nodepth", &T.object_flags, TDIAG_NODEPTH },
                    { "--debug-b560-skip", &T.object_skip, 1 } };
    for (unsigned i = 0; i < sizeof options / sizeof options[0]; i++)
        if (!strcmp(arg, options[i].name)) {
            *options[i].target |= options[i].flag;
            return 1;
        }
    return 0;
}

int tdiag_draw_kind(uint32_t caller) {
    if (caller == TDIAG_TERRAIN_SUBMITTER) return TDIAG_KIND_TERRAIN;
    if (caller == TDIAG_OBJECT_SUBMITTER) return TDIAG_KIND_OBJECTS;
    return TDIAG_KIND_OTHER;
}

int tdiag_debug_for(int kind) {
    return kind == TDIAG_KIND_TERRAIN ? T.flags : kind == TDIAG_KIND_OBJECTS ? T.object_flags : 0;
}

int tdiag_skip(int kind) { return kind == TDIAG_KIND_OBJECTS && T.object_skip; }

static void log_flags(const char* what, uint32_t caller, int flags, int skip) {
    if (!flags && !skip) return;
    host_log("renderer", "%s debug output (diagnostic only, draws from sub_%08X):%s%s%s%s%s%s", what, caller,
             (flags & TDIAG_WIREFRAME) ? " wireframe in green" : "",
             (flags & TDIAG_SOLID) ? " solid magenta, no texture or fog" : "",
             (flags & TDIAG_TEXTURE) ? " texture colour only, no fog/diffuse/alpha test/blend" : "",
             (flags & TDIAG_NOCULL) ? " culling disabled" : "", (flags & TDIAG_NODEPTH) ? " no depth test" : "",
             skip ? " not drawn" : "");
}

void tdiag_log_debug_flags(void) {
    log_flags("terrain", TDIAG_TERRAIN_SUBMITTER, T.flags, 0);
    log_flags("opaque object batch", TDIAG_OBJECT_SUBMITTER, T.object_flags, T.object_skip);
}

static int watched(const dd_surface_t* s) {
    return s && (s->water_texture || ((s->caps & DDSCAPS_TEXTURE) && s->width == 512 && s->height == 512));
}

static watch_t* watch_for(const dd_surface_t* s) {
    for (unsigned i = 0; i < T.watch_count; i++)
        if (T.watch[i].s == s) return &T.watch[i];
    if (T.watch_count == WATCH_LIMIT) return NULL;
    memset(&T.watch[T.watch_count], 0, sizeof T.watch[0]);
    T.watch[T.watch_count].s = s;
    return &T.watch[T.watch_count++];
}

static void draw_state(const d3d_device_t* dev, d3dt_draw_state_t* s) {
    const DWORD* r = dev->render;
    const DWORD* st = dev->stage[0];
    memset(s, 0, sizeof *s);
    s->textured = dev->texture != NULL;
    s->tmb = r[D3DRENDERSTATE_TEXTUREMAPBLEND];
    s->blend_enable = r[D3DRENDERSTATE_ALPHABLENDENABLE] != 0;
    s->src_blend = r[D3DRENDERSTATE_SRCBLEND];
    s->dst_blend = r[D3DRENDERSTATE_DESTBLEND];
    s->alpha_test = r[D3DRENDERSTATE_ALPHATESTENABLE] != 0;
    s->alpha_func = r[D3DRENDERSTATE_ALPHAFUNC];
    s->alpha_ref = r[D3DRENDERSTATE_ALPHAREF];
    s->fog = r[D3DRENDERSTATE_FOGENABLE] != 0;
    s->fog_color = r[D3DRENDERSTATE_FOGCOLOR] & 0xFFFFFFu;
    s->specular = r[D3DRENDERSTATE_SPECULARENABLE] != 0;
    s->shade = r[D3DRENDERSTATE_SHADEMODE];
    s->zenable = r[D3DRENDERSTATE_ZENABLE] != 0;
    s->zwrite = r[D3DRENDERSTATE_ZWRITEENABLE] != 0;
    s->zfunc = r[D3DRENDERSTATE_ZFUNC];
    s->cull = r[D3DRENDERSTATE_CULLMODE];
    s->fill = r[D3DRENDERSTATE_FILLMODE];
    s->colorkey = r[D3DRENDERSTATE_COLORKEYENABLE] != 0;
    s->address_u = st[D3DTSS_ADDRESSU];
    s->address_v = st[D3DTSS_ADDRESSV];
    s->mag = st[D3DTSS_MAGFILTER];
    s->min = st[D3DTSS_MINFILTER];
}

static int surface_format(const dd_surface_t* s) {
    const DDPIXELFORMAT* pf = &s->format;
    return d3dt_format_from_pixelformat(pf->dwFlags, pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask,
                                        pf->dwRGBAlphaBitMask);
}

static float address(float t, uint32_t mode) {
    if (mode == D3DTADDRESS_WRAP || mode == D3DTADDRESS_MIRROR) return t - floorf(t);
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

static int texel_at(const dd_surface_t* s, float u, float v, const d3dt_draw_state_t* st, uint32_t* out) {
    int format = surface_format(s), bytes = d3dt_format_bytes(format);
    long x, y;
    if (!format || format == D3DT_FMT_PAL8 || !s->bits || !s->width || !s->height || d3dt_float_bad(u) ||
        d3dt_float_bad(v))
        return 0;
    x = (long)(address(u, st->address_u) * (float)s->width);
    y = (long)(address(v, st->address_v) * (float)s->height);
    if (x >= (long)s->width) x = (long)s->width - 1;
    if (y >= (long)s->height) y = (long)s->height - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    d3dt_convert_row(format, s->bits + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)bytes, out, 1, s->has_src_key,
                     s->src_key.dwColorSpaceLowValue, s->src_key.dwColorSpaceHighValue);
    return 1;
}

static int near_black(uint32_t c) {
    return ((c >> 16) & 0xFF) <= NEAR_BLACK && ((c >> 8) & 0xFF) <= NEAR_BLACK && (c & 0xFF) <= NEAR_BLACK;
}

static uint32_t lerp_colour(const tdiag_vertex_t* const* p, const float w[3], int specular) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float c = 0.0f;
        for (int k = 0; k < 3; k++) c += w[k] * (float)(((specular ? p[k]->specular : p[k]->diffuse) >> shift) & 0xFF);
        out |= (uint32_t)(c < 0.0f ? 0.0f : c > 255.0f ? 255.0f : c + 0.5f) << shift;
    }
    return out;
}

/* The fog colour follows the scene, so it is reported beside the states rather than inside them. */
static void count_fingerprint(const d3dt_draw_state_t* state) {
    d3dt_draw_state_t copy = *state;
    const d3dt_draw_state_t* st = &copy;
    uint32_t fp;
    char text[384];
    copy.fog_color = 0;
    fp = d3dt_state_fingerprint(st);
    for (unsigned i = 0; i < T.fp_count; i++)
        if (T.fps[i].fp == fp) {
            T.fps[i].draws++;
            return;
        }
    if (T.fp_count == MAX_FINGERPRINTS) {
        T.fp_overflow++;
        return;
    }
    T.fps[T.fp_count].fp = fp;
    T.fps[T.fp_count].draws = 1;
    T.fp_count++;
    d3dt_describe_state(st, text, sizeof text);
    host_log("terrain", "terrain draw state #%u (fingerprint 0x%08X): %s", T.fp_count, fp, text);
}

static void note_frame_texture(const dd_surface_t* s) {
    for (unsigned i = 0; i < T.frame_tex_count; i++)
        if (T.frame_tex[i] == s) return;
    if (T.frame_tex_count < FRAME_TEXTURES) T.frame_tex[T.frame_tex_count++] = s;
    T.w.distinct_textures++;
}

static void widen(float* lo, float* hi, float v, int first) {
    if (first || v < *lo) *lo = v;
    if (first || v > *hi) *hi = v;
}

static void terrain_window(const d3d_device_t* dev, const d3dt_draw_state_t* st, const tdiag_vertex_t* v,
                           unsigned first, unsigned nverts, const uint32_t* idx, unsigned nidx) {
    window_t* w = &T.w;
    w->draws++;
    w->triangles += nidx / 3;
    w->vertices += nverts;
    if (w->draws > 1 && st->fog_color != w->fog_color) w->fog_color_changes++;
    w->fog_color = st->fog_color;
    if (dev->texture) note_frame_texture(dev->texture);
    else {
        w->untextured++;
        if (T.untextured_logged < UNTEXTURED_LOG_LIMIT) {
            T.untextured_logged++;
            host_log("terrain", "terrain draw without a texture (SetTexture NULL) from sub_%08X: %u vertices, first at "
                     "(%.1f,%.1f) sz %.5f; tmb %u, fog %s", g_cur_func, nverts, v[first].x, v[first].y, v[first].z,
                     st->tmb, st->fog ? "on" : "off");
        }
    }
    for (unsigned i = 0; i < nverts; i++) {
        const tdiag_vertex_t* p = &v[first + i];
        unsigned fog = p->specular >> 24;
        int init = !w->seen;
        if (d3dt_float_bad(p->x) || d3dt_float_bad(p->y) || d3dt_float_bad(p->z) || d3dt_float_bad(p->rhw) ||
            d3dt_float_bad(p->u) || d3dt_float_bad(p->v)) {
            w->bad++;
            continue;
        }
        w->rhw_nonpositive += p->rhw <= 0.0f;
        w->z_outside += p->z < 0.0f || p->z > 1.0f;
        w->diffuse_not_white += p->diffuse != 0xFFFFFFFFu;
        widen(&w->min_z, &w->max_z, p->z, init);
        widen(&w->min_rhw, &w->max_rhw, p->rhw, init);
        widen(&w->min_u, &w->max_u, p->u, init);
        widen(&w->min_v, &w->max_v, p->v, init);
        widen(&w->min_x, &w->max_x, p->x, init);
        widen(&w->min_y, &w->max_y, p->y, init);
        if (init || fog < w->min_fog) w->min_fog = fog;
        if (init || fog > w->max_fog) w->max_fog = fog;
        w->seen = 1;
    }
    for (unsigned t = 0; t + 2 < nidx; t += 3) {
        float x[3] = { v[idx[t]].x, v[idx[t + 1]].x, v[idx[t + 2]].x };
        float y[3] = { v[idx[t]].y, v[idx[t + 1]].y, v[idx[t + 2]].y };
        w->culled += d3dt_triangle_culled(st->cull, x, y);
    }
    count_fingerprint(st);
}

static void log_terrain_draw(const d3d_device_t* dev, const tdiag_vertex_t* v, unsigned nverts, unsigned nidx) {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0, r0 = 0, r1 = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    unsigned f0 = 255, f1 = 0;
    const dd_surface_t* s = dev->texture;
    for (unsigned i = 0; i < nverts; i++) {
        widen(&x0, &x1, v[i].x, !i);
        widen(&y0, &y1, v[i].y, !i);
        widen(&z0, &z1, v[i].z, !i);
        widen(&r0, &r1, v[i].rhw, !i);
        widen(&u0, &u1, v[i].u, !i);
        widen(&v0, &v1, v[i].v, !i);
        if ((v[i].specular >> 24) < f0) f0 = v[i].specular >> 24;
        if ((v[i].specular >> 24) > f1) f1 = v[i].specular >> 24;
    }
    host_log("terrain", "census frame terrain draw #%u: %u vertices, %u triangles, screen (%.1f,%.1f)-(%.1f,%.1f), sz "
             "%.5f..%.5f, rhw %.5f..%.5f, uv %.4f..%.4f / %.4f..%.4f, fog alpha %u..%u, texture %s%s %lux%lu",
             T.terrain_ordinal, nverts, nidx / 3, x0, y0, x1, y1, z0, z1, r0, r1, u0, u1, v0, v1, f0, f1,
             s ? "set " : "NULL", s ? d3dt_format_name(surface_format(s)) : "", s ? s->width : 0, s ? s->height : 0);
    if (T.terrain_ordinal > VERTEX_LOG_DRAWS) return;
    for (unsigned i = 0; i < nverts && i < VERTEX_LOG_LIMIT; i++)
        host_log("terrain", "  v%u: x %.2f y %.2f z %.6f rhw %.6f diffuse 0x%08X specular 0x%08X u %.5f v %.5f", i, v[i].x,
                 v[i].y, v[i].z, v[i].rhw, v[i].diffuse, v[i].specular, v[i].u, v[i].v);
}

static void grid_triangle(const d3d_device_t* dev, const d3dt_draw_state_t* st, uint16_t id, const tdiag_vertex_t* const* p,
                          float cw, float ch) {
    float x[3], y[3], z[3], rhw[3], u[3], v[3], w[3];
    float minx, maxx, miny, maxy;
    int i0, i1, j0, j1, zenable = st->zenable && dev->target && dev->target->zbuffer;
    int tex_alpha = dev->texture && d3dt_format_has_alpha(surface_format(dev->texture));
    for (int k = 0; k < 3; k++) {
        x[k] = p[k]->x;
        y[k] = p[k]->y;
        z[k] = p[k]->z;
        rhw[k] = p[k]->rhw;
        u[k] = p[k]->u;
        v[k] = p[k]->v;
        if (d3dt_float_bad(x[k]) || d3dt_float_bad(y[k])) return;
    }
    if (d3dt_triangle_culled(st->cull, x, y)) return;
    minx = fminf(x[0], fminf(x[1], x[2]));
    maxx = fmaxf(x[0], fmaxf(x[1], x[2]));
    miny = fminf(y[0], fminf(y[1], y[2]));
    maxy = fmaxf(y[0], fmaxf(y[1], y[2]));
    i0 = (int)floorf(minx / cw - 0.5f);
    i1 = (int)ceilf(maxx / cw - 0.5f);
    j0 = (int)floorf(miny / ch - 0.5f);
    j1 = (int)ceilf(maxy / ch - 0.5f);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > GRID_W - 1) i1 = GRID_W - 1;
    if (j1 > GRID_H - 1) j1 = GRID_H - 1;
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            cell_t* c = &T.cells[j][i];
            float zc, cu = 0.0f, cv = 0.0f;
            uint32_t texel = 0, colour;
            int has_texel = 0;
            if (!d3dt_barycentric(x, y, (i + 0.5f) * cw, (j + 0.5f) * ch, w)) continue;
            zc = w[0] * z[0] + w[1] * z[1] + w[2] * z[2];
            zc = zc < 0.0f ? 0.0f : zc > 1.0f ? 1.0f : zc;
            if (zenable && !d3dt_depth_test(st->zfunc, zc, c->z)) continue;
            if (dev->texture) {
                d3dt_perspective_uv(u, v, rhw, w, &cu, &cv);
                has_texel = texel_at(dev->texture, cu, cv, st, &texel);
                if (has_texel && st->colorkey && dev->texture->has_src_key && !(texel >> 24)) continue;
                if (has_texel && st->alpha_test && tex_alpha &&
                    !d3dt_depth_test(st->alpha_func, (float)(texel >> 24), (float)(st->alpha_ref & 0xFF)))
                    continue;
            }
            if (zenable && st->zwrite) c->z = zc;
            if (st->blend_enable) {
                c->blend_owner = id;
                continue;
            }
            colour = lerp_colour(p, w, 0);
            if (has_texel) colour = d3dt_texture_blend(st->tmb, texel, colour, tex_alpha);
            if (st->fog) colour = d3dt_apply_fog(colour, st->fog_color, lerp_colour(p, w, 1) >> 24);
            c->under = c->owner ? c->expected : 0;
            c->under_kind = (unsigned char)(c->owner ? T.slots[c->owner - 1].kind + 1 : 0);
            c->owner = id;
            c->blend_owner = 0;
            c->u = cu;
            c->v = cv;
            c->texel = texel;
            c->has_texel = (unsigned char)has_texel;
            c->expected = colour;
            c->fog = (unsigned char)(lerp_colour(p, w, 1) >> 24);
        }
    }
}

static void grid_draw(const d3d_device_t* dev, const d3dt_draw_state_t* st, int kind, const tdiag_vertex_t* v,
                      const uint32_t* idx, unsigned nidx, unsigned legacy_w, unsigned legacy_h) {
    slot_t* slot;
    uint16_t id;
    float cw, ch;
    if (!legacy_w || !legacy_h) return;
    if (T.nslots >= MAX_SLOTS) {
        T.overflow = 1;
        return;
    }
    slot = &T.slots[T.nslots++];
    id = (uint16_t)T.nslots;
    slot->tex = dev->texture;
    slot->caller = g_cur_func;
    slot->fingerprint = d3dt_state_fingerprint(st);
    slot->fog_color = st->fog_color;
    slot->ordinal = kind == TDIAG_KIND_TERRAIN ? T.terrain_ordinal : 0;
    slot->triangles = nidx / 3;
    slot->terrain = (unsigned char)(kind == TDIAG_KIND_TERRAIN);
    slot->kind = (unsigned char)kind;
    slot->water = (unsigned char)(dev->texture && dev->texture->water_texture);
    slot->content_gen = dev->texture ? dev->texture->content_gen : 0;
    slot->blended = (unsigned char)(st->blend_enable != 0);
    slot->tmb = (unsigned char)st->tmb;
    slot->cull = (unsigned char)st->cull;
    cw = (float)legacy_w / GRID_W;
    ch = (float)legacy_h / GRID_H;
    for (unsigned t = 0; t + 2 < nidx; t += 3) {
        const tdiag_vertex_t* p[3] = { &v[idx[t]], &v[idx[t + 1]], &v[idx[t + 2]] };
        grid_triangle(dev, st, id, p, cw, ch);
    }
}

static void object_draw(d3d_device_t* dev, const d3dt_draw_state_t* st, const tdiag_vertex_t* v, unsigned nverts,
                        unsigned nidx) {
    const dd_surface_t* s = dev->texture;
    char text[384];
    T.w.object_draws++;
    T.w.object_triangles += nidx / 3;
    if (!s) T.w.object_untextured++;
    if (s && s->water_texture) T.w.object_water_draws++;
    if (watched(s)) {
        watch_t* w = watch_for(s);
        if (w && !w->first_draw) w->first_draw = T.frame + 1;
    }
    if (!T.armed || T.census_frames > DRAW_LOG_FRAMES || T.object_draws_logged >= OBJECT_DRAW_LOG_PER_FRAME * T.census_frames)
        return;
    if (!s || !watched(s)) return;
    T.object_draws_logged++;
    d3dt_describe_state(st, text, sizeof text);
    host_log("terrain", "opaque batch draw with the %s texture 0x%08X %lux%lu %s (content gen %lu, uploaded gen %lu, "
             "%lu CPU writes): %u vertices; %s", s->water_texture ? "CEntityWater" : "512x512",
             (uint32_t)(uintptr_t)s, s->width, s->height,
             d3dt_format_name(d3dt_format_from_pixelformat(s->format.dwFlags, s->format.dwRGBBitCount, s->format.dwRBitMask,
                                                           s->format.dwGBitMask, s->format.dwBBitMask,
                                                           s->format.dwRGBAlphaBitMask)),
             s->content_gen, s->uploaded_gen, s->cpu_writes, nverts, text);
    for (unsigned i = 0; i < nverts && i < 4; i++)
        host_log("terrain", "  v%u: x %.2f y %.2f z %.6f rhw %.6f diffuse 0x%08X specular 0x%08X u %.5f v %.5f", i, v[i].x,
                 v[i].y, v[i].z, v[i].rhw, v[i].diffuse, v[i].specular, v[i].u, v[i].v);
}

void tdiag_draw(d3d_device_t* dev, int kind, const tdiag_vertex_t* v, unsigned first, unsigned nverts,
                const uint32_t* idx, unsigned nidx, unsigned legacy_w, unsigned legacy_h) {
    d3dt_draw_state_t st;
    int terrain = kind == TDIAG_KIND_TERRAIN;
    draw_state(dev, &st);
    if (kind == TDIAG_KIND_OBJECTS) object_draw(dev, &st, v + first, nverts, nidx);
    if (terrain) {
        if (dev->texture) dev->texture->terrain_texture = 1;
        terrain_window(dev, &st, v, first, nverts, idx, nidx);
        if (T.armed) {
            T.terrain_ordinal++;
            if (T.census_frames <= DRAW_LOG_FRAMES && T.terrain_ordinal <= DRAW_LOG_PER_FRAME)
                log_terrain_draw(dev, v + first, nverts, nidx);
        }
    }
    if (T.armed) grid_draw(dev, &st, kind, v, idx, nidx, legacy_w, legacy_h);
}

void tdiag_clear(int target, int zbuffer, float z) {
    if (!T.armed) return;
    for (int j = 0; j < GRID_H; j++)
        for (int i = 0; i < GRID_W; i++) {
            cell_t* c = &T.cells[j][i];
            if (zbuffer) c->z = z;
            if (target) {
                c->owner = c->blend_owner = 0;
                c->has_texel = 0;
            }
        }
}

void tdiag_texture_written(const dd_surface_t* s, const RECT* r) {
    watch_t* w;
    if (!watched(s) || !(w = watch_for(s))) return;
    if (!w->first_write) w->first_write = T.frame + 1;
    if (T.watch_writes_logged >= WATCH_WRITE_LOG_LIMIT) return;
    T.watch_writes_logged++;
    host_log("terrain", "watched texture write: surface 0x%08X %lux%lu%s by sub_%08X, rect %ld,%ld-%ld,%ld, content gen %lu, "
             "frame %lu", (uint32_t)(uintptr_t)s, s->width, s->height, s->water_texture ? " (CEntityWater)" : "",
             g_cur_func, r ? r->left : 0, r ? r->top : 0, r ? r->right : (long)s->width, r ? r->bottom : (long)s->height,
             s->content_gen, T.frame);
}

static void watched_upload(const dd_surface_t* s, unsigned long long n, unsigned long long black, unsigned long long dark,
                           const unsigned long long* sum, uint32_t lo, uint32_t hi, unsigned w, unsigned h, unsigned left,
                           unsigned top) {
    watch_t* wt = watch_for(s);
    const DDPIXELFORMAT* pf = &s->format;
    if (wt && !wt->first_upload) wt->first_upload = T.frame + 1;
    if (T.watch_uploads_logged >= WATCH_UPLOAD_LOG_LIMIT) return;
    T.watch_uploads_logged++;
    host_log("terrain", "watched texture upload: surface 0x%08X %lux%lu pitch %ld%s, %s, caps 0x%08lX, colour key %s "
             "0x%lX-0x%lX, box (%u,%u) %ux%u: black %.1f%%, near-black %.1f%%, average RGB %llu,%llu,%llu, darkest 0x%06X, "
             "brightest 0x%06X; content gen %lu, %lu CPU writes, frame %lu", (uint32_t)(uintptr_t)s, s->width, s->height,
             s->pitch, s->water_texture ? " (CEntityWater)" : "",
             d3dt_format_name(d3dt_format_from_pixelformat(pf->dwFlags, pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask,
                                                           pf->dwBBitMask, pf->dwRGBAlphaBitMask)),
             s->caps, s->has_src_key ? "set" : "none", s->has_src_key ? s->src_key.dwColorSpaceLowValue : 0,
             s->has_src_key ? s->src_key.dwColorSpaceHighValue : 0, left, top, w, h, 100.0 * (double)black / (double)n,
             100.0 * (double)dark / (double)n, sum[0] / n, sum[1] / n, sum[2] / n, lo, hi, s->content_gen, s->cpu_writes,
             T.frame);
}

void tdiag_upload(const dd_surface_t* s, const uint32_t* bgra, unsigned w, unsigned h, unsigned left, unsigned top) {
    unsigned long long n = (unsigned long long)w * h, black = 0, dark = 0, sum[3] = { 0, 0, 0 };
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    unsigned lo_l = 0xFFFFFFFFu, hi_l = 0;
    double pct;
    if ((!s->terrain_texture && !watched(s)) || !n) return;
    for (unsigned long long i = 0; i < n; i++) {
        uint32_t c = bgra[i] & 0xFFFFFFu, r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF, l = r * 2 + g * 5 + b;
        black += c == 0;
        dark += near_black(c);
        sum[0] += r;
        sum[1] += g;
        sum[2] += b;
        if (l < lo_l) lo_l = l, lo = c;
        if (l > hi_l) hi_l = l, hi = c;
    }
    if (watched(s)) watched_upload(s, n, black, dark, sum, lo, hi, w, h, left, top);
    if (!s->terrain_texture) return;
    T.w.uploads++;
    T.w.upload_pixels += n;
    T.w.upload_black += dark;
    pct = 100.0 * (double)dark / (double)n;
    if (T.uploads_logged < UPLOAD_LOG_LIMIT || (pct >= 25.0 && T.black_uploads_logged < BLACK_UPLOAD_LOG_LIMIT)) {
        if (T.uploads_logged < UPLOAD_LOG_LIMIT) T.uploads_logged++;
        else T.black_uploads_logged++;
        host_log("terrain", "terrain texture upload: surface 0x%08X %lux%lu, box (%u,%u) %ux%u: black %.1f%%, near-black "
                 "%.1f%%, average RGB %llu,%llu,%llu, darkest 0x%06X, brightest 0x%06X, %lu CPU writes",
                 (uint32_t)(uintptr_t)s, s->width, s->height, left, top, w, h, 100.0 * (double)black / (double)n, pct,
                 sum[0] / n, sum[1] / n, sum[2] / n, lo, hi, s->cpu_writes);
    }
}

void tdiag_texture_created(void) { T.w.created++; }

void tdiag_texture_freed(void) { T.w.freed++; }

void tdiag_surface_freed(const dd_surface_t* s) {
    for (unsigned i = 0; i < T.watch_count; i++)
        if (T.watch[i].s == s) T.watch[i] = T.watch[--T.watch_count];
    for (unsigned i = 0; i < T.nslots; i++)
        if (T.slots[i].tex == s) T.slots[i].tex = NULL;
    for (unsigned i = 0; i < T.frame_tex_count; i++)
        if (T.frame_tex[i] == s) T.frame_tex[i] = NULL;
}

int tdiag_census_frame(void) { return T.armed; }

enum { BLACK_NONE, BLACK_TERRAIN, BLACK_OTHER, BLACK_BLENDED, BLACK_OBJECTS, BLACK_WATER, BLACK_KINDS };
enum { CAUSE_TEXEL, CAUSE_UNTEXTURED, CAUSE_FOG, CAUSE_DARK, CAUSE_BRIGHT, CAUSES };

static int terrain_cause(const cell_t* c, const slot_t* s) {
    if (!near_black(c->expected)) return CAUSE_BRIGHT;
    if (!s->tex) return CAUSE_UNTEXTURED;
    if (c->has_texel && near_black(c->texel)) return CAUSE_TEXEL;
    if (c->fog < 32 && near_black(s->fog_color)) return CAUSE_FOG;
    return CAUSE_DARK;
}

static uint16_t census_pixel(unsigned x, unsigned y) {
    if (!T.census_pixels || x >= T.census_w || y >= T.census_h) return 0;
    return ((const uint16_t*)((const BYTE*)T.census_pixels + (size_t)y * T.census_pitch))[x];
}

static void log_cell(int i, int j, unsigned w, unsigned h, const char* what) {
    static const char* const under_names[] = { "the clear", "terrain", "terrain", "an opaque batch draw" };
    const cell_t* c = &T.cells[j][i];
    const slot_t* s = c->owner ? &T.slots[c->owner - 1] : NULL;
    const slot_t* b = c->blend_owner ? &T.slots[c->blend_owner - 1] : NULL;
    unsigned x0 = i * w / GRID_W, y0 = j * h / GRID_H, x1 = (i + 1) * w / GRID_W, y1 = (j + 1) * h / GRID_H;
    uint32_t final = d3dt_rgb565_to_bgra(census_pixel((x0 + x1) / 2, (y0 + y1) / 2)) & 0xFFFFFFu;
    if (s && s->kind == TDIAG_KIND_OBJECTS) {
        const char* under = c->under_kind == 0 ? under_names[0] : c->under_kind == TDIAG_KIND_TERRAIN + 1 ? "terrain"
                          : c->under_kind == TDIAG_KIND_OBJECTS + 1 ? under_names[3] : "another draw";
        host_log("terrain", "census cell (%u,%u)-(%u,%u) %s: opaque batch draw from sub_%08X (%u triangles), texture %s "
                 "%lux%lu%s, content gen at draw %lu, uv %.4f,%.4f, texel 0x%08X%s, tmb %u, fog alpha %u; pixel before "
                 "0x%06X (%s), produced 0x%06X, final 0x%06X", x0, y0, x1, y1, what, s->caller, s->triangles,
                 s->tex ? d3dt_format_name(surface_format(s->tex)) : "none", s->tex ? s->tex->width : 0,
                 s->tex ? s->tex->height : 0, s->water ? " (CEntityWater texture)" : "", s->content_gen, c->u, c->v,
                 c->texel, c->has_texel ? "" : " (not sampled)", s->tmb, c->fog, c->under & 0xFFFFFFu, under,
                 c->expected & 0xFFFFFFu, final);
        return;
    }
    if (!s) {
        host_log("terrain", "census cell (%u,%u)-(%u,%u) %s: no draw covers it (only clears)%s", x0, y0, x1, y1, what,
                 b ? ", a blended draw does" : "");
        return;
    }
    host_log("terrain", "census cell (%u,%u)-(%u,%u) %s: %s draw from sub_%08X%s (terrain #%u, %u triangles), texture %s "
             "%lux%lu, uv %.4f,%.4f, texel 0x%08X%s, tmb %u, fog alpha %u fog colour 0x%06X, z %.5f, expected 0x%06X%s",
             x0, y0, x1, y1, what, s->terrain ? "terrain" : "other", s->caller, s->blended ? " (blended)" : "", s->ordinal,
             s->triangles, s->tex ? d3dt_format_name(surface_format(s->tex)) : "none", s->tex ? s->tex->width : 0,
             s->tex ? s->tex->height : 0, c->u, c->v, c->texel, c->has_texel ? "" : " (not sampled)", s->tmb, c->fog,
             s->fog_color, c->z, c->expected & 0xFFFFFFu, b ? ", then a blended draw" : "");
}

void tdiag_census(const void* pixels, size_t pitch, unsigned w, unsigned h) {
    static const char* const thirds[] = { "top", "middle", "bottom" };
    T.census_pixels = pixels;
    T.census_pitch = pitch;
    T.census_w = w;
    T.census_h = h;
    static const char* const causes[] = { "texel black in the texture", "untextured", "fog to a dark fog colour",
                                          "dark by the reference (vertex colour or blend)",
                                          "expected bright, black on the GPU" };
    unsigned logged[BLACK_KINDS + CAUSES] = { 0 };
    if (!T.armed || !w || !h) return;
    memset(T.cell_dark, 0, sizeof T.cell_dark);
    memset(T.cell_pixels, 0, sizeof T.cell_pixels);
    for (unsigned y = 0; y < h; y++) {
        const uint16_t* row = (const uint16_t*)((const BYTE*)pixels + (size_t)y * pitch);
        unsigned cy = y * GRID_H / h;
        for (unsigned x = 0; x < w; x++) {
            uint16_t p = row[x];
            unsigned cx = x * GRID_W / w;
            T.cell_pixels[cy][cx]++;
            T.cell_dark[cy][cx] += (p >> 11) <= 1 && ((p >> 5) & 0x3F) <= 2 && (p & 0x1F) <= 1;
        }
    }
    for (int third = 0; third < 3; third++) {
        unsigned cells = 0, black = 0, kinds[BLACK_KINDS] = { 0 }, cause[CAUSES] = { 0 }, terrain = 0, none = 0;
        for (int j = third * GRID_H / 3; j < (third + 1) * GRID_H / 3; j++)
            for (int i = 0; i < GRID_W; i++) {
                const cell_t* c = &T.cells[j][i];
                const slot_t* s = c->owner ? &T.slots[c->owner - 1] : NULL;
                int is_black = T.cell_pixels[j][i] && T.cell_dark[j][i] * 5u >= T.cell_pixels[j][i] * 4u;
                cells++;
                terrain += s && s->terrain;
                none += !s;
                if (!is_black) continue;
                black++;
                if (c->blend_owner) kinds[BLACK_BLENDED]++;
                else if (!s) kinds[BLACK_NONE]++;
                else if (s->kind == TDIAG_KIND_OBJECTS) kinds[s->water ? BLACK_WATER : BLACK_OBJECTS]++;
                else if (!s->terrain) kinds[BLACK_OTHER]++;
                else {
                    kinds[BLACK_TERRAIN]++;
                    cause[terrain_cause(c, s)]++;
                }
            }
        host_log("terrain", "census %s third: %u cells, terrain on top in %.1f%%, no draw at all in %.1f%%; black %u "
                 "(%.1f%%): no draw %u, terrain %u, opaque batch %u (CEntityWater texture %u), other draws %u, blended "
                 "over %u; black terrain cells: texel black %u, untextured %u, fog %u, dark by reference %u, expected "
                 "bright %u", thirds[third], cells, 100.0 * terrain / cells, 100.0 * none / cells, black,
                 100.0 * black / cells, kinds[BLACK_NONE], kinds[BLACK_TERRAIN], kinds[BLACK_OBJECTS] + kinds[BLACK_WATER],
                 kinds[BLACK_WATER], kinds[BLACK_OTHER], kinds[BLACK_BLENDED], cause[CAUSE_TEXEL],
                 cause[CAUSE_UNTEXTURED], cause[CAUSE_FOG], cause[CAUSE_DARK], cause[CAUSE_BRIGHT]);
    }
    for (int j = GRID_H - 1; j >= 0; j--)
        for (int i = 0; i < GRID_W; i += 3) {
            const cell_t* c = &T.cells[j][i];
            const slot_t* s = c->owner ? &T.slots[c->owner - 1] : NULL;
            unsigned slot_index;
            const char* what;
            if (!T.cell_pixels[j][i] || T.cell_dark[j][i] * 5u < T.cell_pixels[j][i] * 4u) continue;
            if (c->blend_owner) slot_index = BLACK_BLENDED, what = "black under a blended draw";
            else if (!s) slot_index = BLACK_NONE, what = "black";
            else if (s->kind == TDIAG_KIND_OBJECTS) slot_index = s->water ? BLACK_WATER : BLACK_OBJECTS, what = "black";
            else if (!s->terrain) slot_index = BLACK_OTHER, what = "black";
            else {
                int k = terrain_cause(c, s);
                slot_index = BLACK_KINDS + k;
                what = causes[k];
            }
            if (logged[slot_index] >= CELL_SAMPLE_LIMIT) continue;
            logged[slot_index]++;
            log_cell(i, j, w, h, what);
        }
    if (T.overflow) host_log("terrain", "census: more than %u draws in the frame; later draws were not mapped", MAX_SLOTS);
    T.census_pixels = NULL;
}

void tdiag_frame_end(int gameplay, double now) {
    T.frame++;
    T.frame_tex_count = 0;
    T.armed = 0;
    if (!gameplay || now - T.last_census < CENSUS_INTERVAL) return;
    T.last_census = now;
    memset(T.cells, 0, sizeof T.cells);
    T.nslots = 0;
    T.overflow = 0;
    T.terrain_ordinal = 0;
    T.census_frames++;
    T.armed = 1;
}

void tdiag_report(double frames) {
    window_t* w = &T.w;
    if (frames <= 0.0) frames = 1.0;
    if (w->draws) {
        host_log("terrain", "last 5 s per frame: terrain polygons submitted %.1f (triangles %.1f, vertices %.1f), "
                 "untextured %.1f, distinct textures %.1f; triangles CULLMODE removes %.1f", w->draws / frames,
                 w->triangles / frames, w->vertices / frames, w->untextured / frames, w->distinct_textures / frames,
                 w->culled / frames);
        host_log("terrain", "last 5 s terrain vertices: sz %.5f..%.5f, rhw %.6f..%.6f, NaN/INF %llu, rhw <= 0 %llu, sz "
                 "outside 0..1 %llu, diffuse not white %llu, uv %.3f..%.3f / %.3f..%.3f, fog alpha %u..%u (fog colour "
                 "0x%06X, %llu changes), screen x %.1f..%.1f y %.1f..%.1f", w->min_z, w->max_z, w->min_rhw, w->max_rhw,
                 w->bad, w->rhw_nonpositive, w->z_outside, w->diffuse_not_white, w->min_u, w->max_u, w->min_v, w->max_v,
                 w->min_fog, w->max_fog, w->fog_color, w->fog_color_changes, w->min_x, w->max_x, w->min_y, w->max_y);
    }
    if (w->uploads || w->created || w->freed)
        host_log("terrain", "last 5 s textures: terrain uploads %.1f per frame (%.1f KB), near-black texels %.1f%% of "
                 "them; GPU textures created %llu, freed %llu", w->uploads / frames,
                 w->upload_pixels * 2.0 / 1024.0 / frames,
                 w->upload_pixels ? 100.0 * (double)w->upload_black / (double)w->upload_pixels : 0.0, w->created, w->freed);
    if (w->object_draws || w->water_updates)
        host_log("terrain", "last 5 s per frame: opaque batch (sub_%08X) draws %.1f, triangles %.1f, with the CEntityWater "
                 "texture %.1f, untextured %.1f; water texture updates %.1f", TDIAG_OBJECT_SUBMITTER,
                 w->object_draws / frames, w->object_triangles / frames, w->object_water_draws / frames,
                 w->object_untextured / frames, w->water_updates / frames);
    for (unsigned i = 0; i < T.watch_count; i++) {
        watch_t* wt = &T.watch[i];
        const dd_surface_t* s = wt->s;
        if (wt->logged_content == s->content_gen + 1 && wt->logged_uploaded == s->uploaded_gen + 1) continue;
        wt->logged_content = s->content_gen + 1;
        wt->logged_uploaded = s->uploaded_gen + 1;
        host_log("terrain", "watched texture 0x%08X %lux%lu%s: first write frame %lu, first upload frame %lu, first opaque "
                 "batch draw frame %lu (0 = never; now %lu); content gen %lu, uploaded gen %lu",
                 (uint32_t)(uintptr_t)s, s->width, s->height, s->water_texture ? " (CEntityWater)" : "", wt->first_write,
                 wt->first_upload, wt->first_draw, T.frame, s->content_gen, s->uploaded_gen);
    }
    if (T.fp_count > 1 || T.fp_overflow) {
        char line[256];
        int n = 0;
        for (unsigned i = 0; i < T.fp_count; i++)
            n += _snprintf(line + n, sizeof line - 1 - n, "%s#%u %llu", i ? ", " : "", i + 1, T.fps[i].draws);
        line[sizeof line - 1] = 0;
        host_log("terrain", "terrain draw states since start: %s%s", line, T.fp_overflow ? " (more not listed)" : "");
    }
    memset(w, 0, sizeof *w);
}

/* Guest pointers are host pointers; a stale one must not fault inside the host. */
static int read32(uint32_t va, uint32_t* out) {
    MEMORY_BASIC_INFORMATION mbi;
    if (va < 0x10000 || !VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || (uintptr_t)va + 4 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize)
        return 0;
    *out = *(const uint32_t*)(uintptr_t)va;
    return 1;
}

static dd_surface_t* surface_from_face(uint32_t face) {
    uint32_t owner, first;
    dd_surface_t* s;
    if (!read32(face + 4, &owner) || !read32(owner, &first)) return NULL;
    s = (dd_surface_t*)(uintptr_t)owner;
    if ((uint32_t)(uintptr_t)&s->s4 != face && (uint32_t)(uintptr_t)&s->s1 != face && (uint32_t)(uintptr_t)&s->s2 != face &&
        (uint32_t)(uintptr_t)&s->s3 != face)
        return NULL;
    return s;
}

void tdiag_note_water_node(uint32_t node) {
    uint32_t raster = 0, vtable = 0, d3d = 0, face = 0, width = 0, height = 0, link = RPTR_NULL;
    dd_surface_t* s = NULL;
    unsigned i;
    T.w.water_updates++;
    if (!read32(node + 0x14, &raster) || raster == RPTR_NULL || !read32(raster, &vtable)) return;
    read32(raster + 8, &width);
    read32(raster + 0xC, &height);
    if (vtable == VT_CRASTER_D3D) d3d = raster;
    else if (read32(raster + 0x48, &link) && link != RPTR_NULL && read32(link, &vtable) && vtable == VT_CRASTER_D3D) d3d = link;
    if (d3d && read32(d3d + 0x54, &face) && face) s = surface_from_face(face);
    if (s) s->water_texture = 1;
    for (i = 0; i < T.water_logged; i++)
        if (T.water_rasters[i] == raster) break;
    if (i < T.water_logged || T.water_logged == WATER_LOG_LIMIT || (!s && d3d)) return;
    T.water_rasters[T.water_logged++] = raster;
    host_log("terrain", "water texture: CEntityWater node 0x%08X, raster 0x%08X %ux%u, %s%s; D3D surface %s 0x%08X %lux%lu",
             node, raster, width, height,
             raster == d3d ? "CRasterD3D (hardware water)" : "system-memory raster (software water)",
             raster == d3d ? "" : d3d ? ", linked to a D3D raster" : ", no D3D link yet", s ? "" : "none",
             (uint32_t)(uintptr_t)s, s ? s->width : 0, s ? s->height : 0);
}
