#include "texture_gpu_policy.h"

#include <math.h>
#include <string.h>

#define TILE_ROW_SHIFT 9
#define MAX_TEXTURE_BYTES (4u << 20)

int tgpu_row_span(const tgpu_scan_t* s, const tgpu_rows_t* r, int32_t* x0, int32_t* x1) {
    *x0 = s->fx >> 16;
    *x1 = (int32_t)((uint32_t)s->fx + (uint32_t)r->len) >> 16;
    return *x0 < *x1;
}

/* add eax, fx_inc; add ebx, rem_inc; jge: a negative remainder or one past 0x10000 carries, and the carry adds its own
 * extra to the three values held on the x87 stack before they are stored. */
void tgpu_next_row(tgpu_scan_t* s, tgpu_rows_t* r, const tgpu_steps_t* d) {
    int64_t rem = (int64_t)s->rem + d->rem_inc;
    double a[3] = { (double)s->uz + d->step[0], (double)s->vz + d->step[1], (double)s->wz + d->step[2] };
    int carry = 0;
    if (rem < 0) {
        rem += 0x10000;
        carry = 1;
    } else if ((int32_t)rem > 0x10000) {
        rem -= 0x10000;
        carry = 1;
    }
    if (carry)
        for (int i = 0; i < 3; i++) a[i] += d->carry[i];
    s->uz = (float)a[0];
    s->vz = (float)a[1];
    s->wz = (float)a[2];
    s->fx = (int32_t)((uint32_t)s->fx + (uint32_t)d->fx_inc);
    s->rem = (int32_t)rem;
    r->len = (int32_t)((uint32_t)r->len + (uint32_t)r->dlen);
    r->line = (int32_t)((uint32_t)r->line + (uint32_t)d->line_pixels);
    r->y++;
}

void tgpu_plane(tgpu_plane_t* p, float x0, float y0, const float a0[3], float x1, float y1, const float a1[3],
                const float gx[3]) {
    float* out[3] = { p->u, p->v, p->w };
    p->origin_x = x0;
    p->origin_y = y0;
    for (int i = 0; i < 3; i++) {
        out[i][0] = a0[i];
        out[i][1] = gx[i];
        out[i][2] = y1 != y0 ? (a1[i] - a0[i] - gx[i] * (x1 - x0)) / (y1 - y0) : 0.0f;
    }
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

uint32_t tgpu_texel_index(const tgpu_texture_t* t, float u, float v) {
    int32_t iu, iv;
    uint32_t row;
    if (t->clamped) {
        u = clampf(u, t->umin, t->umax);
        v = clampf(v, t->umin, t->vmax);
    }
    iu = (int32_t)floorf(u);
    iv = (int32_t)floorf(v);
    row = t->clamped ? (uint32_t)iv * t->line : (uint32_t)iv << TILE_ROW_SHIFT;
    return (row + ((uint32_t)iu & t->umask)) & t->tile_mask;
}

uint32_t tgpu_texture_bytes(const tgpu_texture_t* t) {
    uint64_t n = 0xFFFFFFFFull + 1u;
    if (t->tile_mask != 0xFFFFFFFFu) n = (uint64_t)t->tile_mask + 1u;
    if (t->clamped && t->umax >= 0.0f && t->vmax >= 0.0f && t->umax <= 65535.0f && t->vmax <= 65535.0f) {
        uint64_t reach = (uint64_t)floorf(t->vmax) * t->line + (uint64_t)floorf(t->umax) + 1u;
        if (reach < n) n = reach;
    }
    return n && n <= MAX_TEXTURE_BYTES ? (uint32_t)n : 0;
}

void tgpu_session_init(tgpu_session_t* s, int enabled) {
    memset(s, 0, sizeof *s);
    s->enabled = enabled != 0;
}

tgpu_mode_t tgpu_polygon_begin(tgpu_session_t* s) {
    s->unsafe = 0;
    s->mode = s->enabled ? TGPU_PENDING : TGPU_OFF;
    return s->mode;
}

tgpu_mode_t tgpu_polygon_lock(tgpu_session_t* s, tgpu_reason_t reason) {
    if (s->mode != TGPU_PENDING) return s->mode;
    s->mode = reason == TGPU_REASON_NONE ? TGPU_GPU : TGPU_CPU;
    if (s->mode == TGPU_CPU) s->reason[reason < TGPU_REASON_COUNT ? reason : TGPU_REASON_OTHER]++;
    return s->mode;
}

void tgpu_polygon_unsafe(tgpu_session_t* s) {
    if (s->mode == TGPU_GPU) s->unsafe = 1;
}

int tgpu_polygon_end(tgpu_session_t* s, int drawn) {
    tgpu_mode_t mode = s->mode;
    s->mode = TGPU_OFF;
    if (mode == TGPU_PENDING) s->reason[TGPU_REASON_MEMORY_RASTER]++;
    if (mode == TGPU_CPU || mode == TGPU_PENDING) s->cpu++;
    if (mode != TGPU_GPU) return 0;
    if (drawn && !s->unsafe) {
        s->gpu++;
        return 0;
    }
    s->dropped++;
    s->enabled = 0;
    if (s->disabled_after_failure) return 0;
    s->disabled_after_failure = 1;
    return 1;
}

tgpu_run_t tgpu_run_action(int locked, int run_active, int lock_untouched, int run_captured, int overlaps) {
    if (!locked) return TGPU_RUN_FRESH;
    if (run_active) return run_captured && !overlaps ? TGPU_RUN_KEEP : TGPU_RUN_SPLIT;
    return lock_untouched ? TGPU_RUN_ADOPT : TGPU_RUN_BUSY;
}

const char* tgpu_reason_name(tgpu_reason_t reason) {
    static const char* const names[TGPU_REASON_COUNT] = {
        "none", "raster_locked", "memory_raster", "no_surface", "texture_invalid", "texture_unbounded",
        "texture_unreadable", "clut_invalid", "alternate_lines", "invalid_subdivision", "invalid_vertices",
        "target_unavailable", "resources_unavailable", "unsupported_state", "other",
    };
    return reason < TGPU_REASON_COUNT ? names[reason] : names[TGPU_REASON_OTHER];
}

static uint32_t texture_hash(const uint8_t* p, uint32_t n) {
    uint32_t a = 2166136261u, b = 0x9E3779B9u, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint32_t w0, w1;
        memcpy(&w0, p + i, 4);
        memcpy(&w1, p + i + 4, 4);
        a = (a ^ w0) * 16777619u;
        b = (b ^ w1) * 2246822519u;
    }
    for (; i < n; i++) a = (a ^ p[i]) * 16777619u;
    return a ^ (b >> 1) ^ n;
}

static uint32_t slot_of(const void* src, uint32_t slots) {
    return (uint32_t)(((uintptr_t)src >> 4) * 2654435761u) % slots;
}

void tgpu_texture_cache_reset(tgpu_texture_cache_t* c, uint32_t pool) {
    memset(c->slot, 0, sizeof c->slot);
    c->top = 0;
    c->pool = pool;
}

/* A full table or pool starts again from empty. */
static int64_t cache_find(tgpu_texture_cache_t* c, const uint8_t* src, uint32_t bytes, unsigned long long frame,
                          int always, int* upload) {
    uint32_t start = slot_of(src, TGPU_TEXTURE_SLOTS), i = start, hash;
    *upload = 0;
    if (!src || !bytes || bytes > c->pool / 4u) return -1;
    do {
        if (c->slot[i].src == src && c->slot[i].bytes == bytes) {
            if (always || c->slot[i].frame != frame) {
                c->slot[i].frame = frame;
                hash = texture_hash(src, bytes);
                if (hash != c->slot[i].hash) {
                    c->slot[i].hash = hash;
                    *upload = 1;
                }
            }
            return c->slot[i].offset;
        }
        if (!c->slot[i].src) break;
        i = (i + 1u) % TGPU_TEXTURE_SLOTS;
    } while (i != start);
    if (c->slot[i].src || c->top + bytes > c->pool) {
        tgpu_texture_cache_reset(c, c->pool);
        i = start;
    }
    c->slot[i].src = src;
    c->slot[i].bytes = bytes;
    c->slot[i].offset = c->top;
    c->slot[i].frame = frame;
    c->slot[i].hash = texture_hash(src, bytes);
    c->top += (bytes + 15u) & ~15u;
    *upload = 1;
    return c->slot[i].offset;
}

int64_t tgpu_texture_cache_find(tgpu_texture_cache_t* c, const uint8_t* src, uint32_t bytes, unsigned long long frame,
                                int* upload) {
    return cache_find(c, src, bytes, frame, 0, upload);
}

int64_t tgpu_texture_cache_find_checked(tgpu_texture_cache_t* c, const uint8_t* src, uint32_t bytes, int* upload) {
    return cache_find(c, src, bytes, 0, 1, upload);
}

uint32_t tgpu_bump_clut_entries(const uint8_t table[TGPU_BUMP_TABLE_BYTES]) {
    uint32_t most = 0;
    for (uint32_t i = 0; i < TGPU_BUMP_TABLE_BYTES; i++)
        if (table[i] > most) most = table[i];
    return (most + 1u) * TGPU_BUMP_ROW_ENTRIES;
}

uint32_t tgpu_bump_clut_index(uint32_t texel, uint32_t theta, const uint8_t table[TGPU_BUMP_TABLE_BYTES]) {
    return (uint32_t)table[(texel - theta) & 0x3FFu] * TGPU_BUMP_ROW_ENTRIES + ((texel & 0xFFFFu) >> 10);
}

void tgpu_clut_cache_reset(tgpu_clut_cache_t* c) { memset(c->slot, 0, sizeof c->slot); }

uint32_t tgpu_clut_cache_find(tgpu_clut_cache_t* c, const uint16_t* src, int* upload) {
    uint32_t start = slot_of(src, TGPU_CLUT_SLOTS), i = start;
    do {
        if (c->slot[i].src == src || !c->slot[i].src) break;
        i = (i + 1u) % TGPU_CLUT_SLOTS;
    } while (i != start);
    if (c->slot[i].src && c->slot[i].src != src) i = start;
    *upload = c->slot[i].src != src || memcmp(c->slot[i].copy, src, sizeof c->slot[i].copy) != 0;
    if (*upload) {
        c->slot[i].src = src;
        memcpy(c->slot[i].copy, src, sizeof c->slot[i].copy);
    }
    return i;
}
