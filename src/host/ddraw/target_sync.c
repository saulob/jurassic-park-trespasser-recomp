#include "target_sync.h"

#include <string.h>

void tsync_attach(tsync_t* t) {
    memset(t, 0, sizeof *t);
    t->valid = TSYNC_CPU;
    t->stale_reference = 1;
}

unsigned tsync_cpu_access(tsync_t* t, tsync_access_t access) {
    unsigned acts = 0;
    if (t->pending && (t->valid & TSYNC_GPU)) {
        acts |= TSYNC_COMPOSITE;
        t->uniform = 0;
    }
    t->pending = 0;
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

void tsync_fill(uint8_t* copy, long pitch, uint16_t* reference, unsigned width, const d3dt_rect_t* r, uint16_t value) {
    for (long y = r->top; y < r->bottom; y++) {
        uint16_t* row = (uint16_t*)(copy + (size_t)y * (size_t)pitch);
        uint16_t* keep = reference + (size_t)y * width;
        for (long x = r->left; x < r->right; x++) row[x] = keep[x] = value;
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
