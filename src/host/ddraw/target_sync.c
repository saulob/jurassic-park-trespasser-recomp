#include "target_sync.h"

#include <string.h>

void tsync_attach(tsync_t* t) {
    memset(t, 0, sizeof *t);
    t->valid = TSYNC_CPU;
    t->stale_reference = 1;
}

unsigned tsync_cpu_flush(tsync_t* t) {
    unsigned acts = 0;
    if (t->pending && (t->valid & TSYNC_GPU)) {
        acts |= TSYNC_COMPOSITE;
        t->uniform = 0;
    }
    t->pending = 0;
    return acts;
}

unsigned tsync_cpu_access(tsync_t* t, tsync_access_t access) {
    unsigned acts = tsync_cpu_flush(t);
    if (t->valid & TSYNC_CPU) return acts;
    if (t->uniform) {
        acts |= TSYNC_SYNTHESIZE;
    } else if (t->front_mirror) {
        acts |= TSYNC_COPY_FRONT;
    } else if (access == TSYNC_ACCESS_READ) {
        acts |= TSYNC_READBACK;
    } else {
        acts |= t->stale_reference ? TSYNC_CAPTURE_ALL : TSYNC_CAPTURE;
        t->stale_reference = 0;
        t->exact = 0;
        return acts;
    }
    t->valid |= TSYNC_CPU;
    t->stale_reference = 0;
    t->exact = 1;
    return acts;
}

unsigned tsync_lock_deferred(tsync_t* t, tsync_access_t access, tsync_deferred_t* d) {
    unsigned now = tsync_cpu_flush(t);
    d->untouched = *t;
    d->acts = tsync_cpu_access(t, access);
    return now;
}

unsigned tsync_lock_touched(tsync_deferred_t* d) {
    unsigned acts = d->acts;
    d->acts = 0;
    return acts;
}

void tsync_lock_untouched(tsync_t* t, tsync_deferred_t* d) {
    *t = d->untouched;
    d->acts = 0;
}

void tsync_cpu_wrote(tsync_t* t) { t->pending = 1; }

unsigned tsync_gpu_draw(tsync_t* t) {
    unsigned acts = 0;
    if (!(t->valid & TSYNC_GPU)) {
        if (t->valid & TSYNC_CPU) acts |= TSYNC_UPLOAD;
        t->stale_reference = 0;
    } else if (t->pending) {
        acts |= TSYNC_COMPOSITE;
    }
    t->valid = TSYNC_GPU;
    t->pending = 0;
    t->uniform = 0;
    t->exact = 0;
    t->front_mirror = 0;
    return acts;
}

void tsync_gpu_clear(tsync_t* t, uint16_t color) {
    t->valid = TSYNC_GPU;
    t->pending = 0;
    t->uniform = 1;
    t->color = color;
    t->stale_reference = 1;
    t->exact = 0;
    t->front_mirror = 0;
}

unsigned tsync_present(tsync_t* t) {
    unsigned acts = 0;
    if (!(t->valid & TSYNC_GPU)) return 0;
    if (t->pending) {
        acts |= TSYNC_COMPOSITE;
        t->uniform = 0;
    }
    t->pending = 0;
    return acts;
}

int tsync_presents_gpu(const tsync_t* t) { return (t->valid & TSYNC_GPU) != 0; }

void tsync_flipped(tsync_t* t) {
    if (!(t->valid & TSYNC_GPU)) return;
    t->front_mirror = (t->valid & TSYNC_CPU) != 0;
    t->valid = TSYNC_GPU;
    t->stale_reference = 1;
    t->exact = 0;
}

void tsync_front_changed(tsync_t* t) { t->front_mirror = 0; }

unsigned tsync_gpu_release(tsync_t* t) {
    unsigned acts = 0;
    if ((t->valid & TSYNC_GPU) && !(t->valid & TSYNC_CPU)) {
        if (t->pending) acts |= TSYNC_COMPOSITE;
        if (t->uniform) acts |= TSYNC_SYNTHESIZE;
        else if (t->front_mirror) acts |= TSYNC_COPY_FRONT;
        else acts |= TSYNC_READBACK;
    }
    t->valid = TSYNC_CPU;
    t->pending = 0;
    t->uniform = 0;
    t->stale_reference = 1;
    t->exact = 0;
    t->front_mirror = 0;
    return acts;
}

/* A recreated GPU target starts cleared to black. */
void tsync_gpu_gone(tsync_t* t) {
    if (t->valid & TSYNC_CPU) {
        t->valid = TSYNC_CPU;
        t->uniform = 0;
    } else {
        t->valid = TSYNC_GPU;
        t->uniform = 1;
        t->color = 0;
    }
    t->pending = 0;
    t->stale_reference = 1;
    t->exact = 0;
    t->front_mirror = 0;
}

const char* tsync_location_name(unsigned valid) {
    switch (valid & (TSYNC_CPU | TSYNC_GPU)) {
    case TSYNC_CPU: return "CPU";
    case TSYNC_GPU: return "GPU";
    case TSYNC_CPU | TSYNC_GPU: return "CPU+GPU";
    default: return "none";
    }
}

static const uint16_t* copy_row(const uint8_t* copy, long pitch, long y) {
    return (const uint16_t*)(copy + (size_t)y * (size_t)pitch);
}

/* One array per loop: the stores cannot alias, so the compiler fills with wide stores. */
static void fill16(uint16_t* p, size_t n, uint16_t value) {
    for (size_t i = 0; i < n; i++) p[i] = value;
}

void tsync_fill(uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* r, uint16_t value) {
    size_t n = r->right > r->left ? (size_t)(r->right - r->left) : 0;
    if (!n) return;
    for (long y = r->top; y < r->bottom; y++) {
        fill16((uint16_t*)(copy + (size_t)y * (size_t)pitch) + r->left, n, value);
        fill16(reference + (size_t)y * width + r->left, n, value);
    }
}

int tsync_changed_box(const uint8_t* copy, long pitch, const uint16_t* reference, unsigned width,
                      const d3dt_rect_t* region, d3dt_rect_t* box) {
    d3dt_rect_t found = { 0, 0, 0, 0 };
    unsigned span = (unsigned)(region->right - region->left);
    for (long y = region->top; y < region->bottom; y++) {
        unsigned first, end;
        d3dt_rect_t row;
        if (!d3dt_row_span(copy_row(copy, pitch, y) + region->left, reference + (size_t)y * width + region->left, span,
                           &first, &end))
            continue;
        row.left = region->left + (long)first;
        row.right = region->left + (long)end;
        row.top = y;
        row.bottom = y + 1;
        d3dt_rect_union(&found, &row);
    }
    *box = found;
    return !d3dt_rect_empty(&found);
}

void tsync_overlay_box(const uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* box,
                       uint32_t* out, tsync_counts_t* counts) {
    unsigned bw = (unsigned)(box->right - box->left);
    for (long y = box->top; y < box->bottom; y++) {
        const uint16_t* row = copy_row(copy, pitch, y) + box->left;
        uint16_t* keep = reference + (size_t)y * width + box->left;
        counts->changed += d3dt_overlay_row(row, keep, bw, out + (size_t)(y - box->top) * bw);
        counts->black += d3dt_black_over_content(row, keep, bw);
        memcpy(keep, row, (size_t)bw * 2);
    }
}

void tsync_fill_unwritten(uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* r,
                          const uint16_t* gpu, size_t gpu_pitch_pixels) {
    for (long y = r->top; y < r->bottom; y++) {
        uint16_t* row = (uint16_t*)(copy + (size_t)y * (size_t)pitch);
        uint16_t* keep = reference + (size_t)y * width;
        const uint16_t* in = gpu + (size_t)(y - r->top) * gpu_pitch_pixels - r->left;
        for (long x = r->left; x < r->right; x++)
            if (row[x] == keep[x]) row[x] = keep[x] = in[x];
    }
}

int tsync_tile_claimed(const tsync_tiles_t* m, unsigned tx, unsigned ty) {
    unsigned i = ty * TSYNC_MAX_TILES_X + tx;
    return (m->claimed[i / 8] >> (i % 8)) & 1;
}

#define tile_claimed tsync_tile_claimed

static void claim_tile(tsync_tiles_t* m, unsigned tx, unsigned ty) {
    unsigned i = ty * TSYNC_MAX_TILES_X + tx;
    m->claimed[i / 8] |= (uint8_t)(1u << (i % 8));
}

int tsync_tiles_reset(tsync_tiles_t* m, unsigned width, unsigned height) {
    unsigned tx = (width + TSYNC_TILE - 1) / TSYNC_TILE, ty = (height + TSYNC_TILE - 1) / TSYNC_TILE;
    memset(m->claimed, 0, sizeof m->claimed);
    memset(&m->bounds, 0, sizeof m->bounds);
    m->width = width;
    m->height = height;
    m->tiles_x = tx;
    m->tiles_y = ty;
    return tx <= TSYNC_MAX_TILES_X && ty <= TSYNC_MAX_TILES_Y;
}

void tsync_tiles_claim_all(tsync_tiles_t* m) {
    memset(m->claimed, 0xFF, sizeof m->claimed);
    m->bounds.left = m->bounds.top = 0;
    m->bounds.right = (long)m->width;
    m->bounds.bottom = (long)m->height;
}

d3dt_rect_t tsync_tiles_clip(const tsync_tiles_t* m, const d3dt_rect_t* region) {
    d3dt_rect_t r = *region;
    if (r.left < m->bounds.left) r.left = m->bounds.left;
    if (r.top < m->bounds.top) r.top = m->bounds.top;
    if (r.right > m->bounds.right) r.right = m->bounds.right;
    if (r.bottom > m->bounds.bottom) r.bottom = m->bounds.bottom;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

void tsync_tiles_claim(tsync_tiles_t* m, const d3dt_rect_t* box, tsync_rect_fn fn, void* context) {
    long left = box->left < 0 ? 0 : box->left, top = box->top < 0 ? 0 : box->top;
    long right = box->right > (long)m->width ? (long)m->width : box->right;
    long bottom = box->bottom > (long)m->height ? (long)m->height : box->bottom;
    unsigned tx0, tx1, ty0, ty1;
    if (left >= right || top >= bottom) return;
    tx0 = (unsigned)left / TSYNC_TILE;
    tx1 = (unsigned)(right - 1) / TSYNC_TILE;
    ty0 = (unsigned)top / TSYNC_TILE;
    ty1 = (unsigned)(bottom - 1) / TSYNC_TILE;
    {
        d3dt_rect_t tiles = { (long)(tx0 * TSYNC_TILE), (long)(ty0 * TSYNC_TILE), (long)((tx1 + 1) * TSYNC_TILE),
                              (long)((ty1 + 1) * TSYNC_TILE) };
        if (tiles.right > (long)m->width) tiles.right = (long)m->width;
        if (tiles.bottom > (long)m->height) tiles.bottom = (long)m->height;
        d3dt_rect_union(&m->bounds, &tiles);
    }
    for (unsigned ty = ty0; ty <= ty1; ty++) {
        unsigned tx = tx0;
        while (tx <= tx1) {
            unsigned end;
            d3dt_rect_t r;
            if (tile_claimed(m, tx, ty)) {
                tx++;
                continue;
            }
            for (end = tx; end <= tx1 && !tile_claimed(m, end, ty); end++) claim_tile(m, end, ty);
            r.left = (long)(tx * TSYNC_TILE);
            r.top = (long)(ty * TSYNC_TILE);
            r.right = (long)(end * TSYNC_TILE);
            r.bottom = (long)((ty + 1) * TSYNC_TILE);
            if (r.right > (long)m->width) r.right = (long)m->width;
            if (r.bottom > (long)m->height) r.bottom = (long)m->height;
            fn(context, &r);
            tx = end;
        }
    }
}

/* The next run along a row from x on: [x, *end) all in claimed tiles (1) or all in unclaimed ones (0). */
static int tile_run(const tsync_tiles_t* m, long y, long x, long right, long* end) {
    unsigned ty = (unsigned)y / TSYNC_TILE;
    int claimed = tile_claimed(m, (unsigned)x / TSYNC_TILE, ty);
    long next = (x / TSYNC_TILE + 1) * TSYNC_TILE;
    while (next < right && tile_claimed(m, (unsigned)next / TSYNC_TILE, ty) == claimed) next += TSYNC_TILE;
    *end = next < right ? next : right;
    return claimed;
}

int tsync_changed_box_in(const uint8_t* copy, long pitch, const uint16_t* reference, unsigned width,
                         const d3dt_rect_t* region, const tsync_tiles_t* fresh, d3dt_rect_t* box) {
    d3dt_rect_t found = { 0, 0, 0, 0 };
    if (!fresh) return tsync_changed_box(copy, pitch, reference, width, region, box);
    for (long y = region->top; y < region->bottom; y++) {
        const uint16_t* row = copy_row(copy, pitch, y);
        const uint16_t* keep = reference + (size_t)y * width;
        for (long x = region->left, end; x < region->right; x = end) {
            unsigned first, stop;
            d3dt_rect_t span;
            if (!tile_run(fresh, y, x, region->right, &end)) continue;
            if (!d3dt_row_span(row + x, keep + x, (unsigned)(end - x), &first, &stop)) continue;
            span.left = x + (long)first;
            span.right = x + (long)stop;
            span.top = y;
            span.bottom = y + 1;
            d3dt_rect_union(&found, &span);
        }
    }
    *box = found;
    return !d3dt_rect_empty(&found);
}

void tsync_overlay_box_in(const uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* box,
                          const tsync_tiles_t* fresh, uint32_t* out, tsync_counts_t* counts) {
    unsigned bw = (unsigned)(box->right - box->left);
    if (!fresh) {
        tsync_overlay_box(copy, pitch, reference, width, box, out, counts);
        return;
    }
    for (long y = box->top; y < box->bottom; y++) {
        const uint16_t* row = copy_row(copy, pitch, y);
        uint16_t* keep = reference + (size_t)y * width;
        uint32_t* texels = out + (size_t)(y - box->top) * bw - box->left;
        for (long x = box->left, end; x < box->right; x = end) {
            unsigned n;
            if (!tile_run(fresh, y, x, box->right, &end)) {
                memset(texels + x, 0, (size_t)(end - x) * sizeof *texels);
                continue;
            }
            n = (unsigned)(end - x);
            counts->changed += d3dt_overlay_row(row + x, keep + x, n, texels + x);
            counts->black += d3dt_black_over_content(row + x, keep + x, n);
            memcpy(keep + x, row + x, (size_t)n * 2);
        }
    }
}
