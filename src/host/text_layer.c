#include "text_layer.h"

#include <stdlib.h>
#include <string.h>

#define BOXES 64

static struct {
    uint16_t* raw;
    unsigned w, h;
    tl_rect_t dirty;
    unsigned stamp, frame;
    tl_rect_t boxes[BOXES];
    int box_count;
    uint8_t* bits;
    long pitch;
    uint8_t* claimed;
    uint16_t* saved;
    unsigned claim_w, claim_h;
    tl_rect_t claims[BOXES];
    int claim_count;
} L;

static int empty(const tl_rect_t* r) { return r->right <= r->left || r->bottom <= r->top; }

static void merge(tl_rect_t* acc, const tl_rect_t* add) {
    if (empty(add)) return;
    if (empty(acc)) {
        *acc = *add;
        return;
    }
    if (add->left < acc->left) acc->left = add->left;
    if (add->top < acc->top) acc->top = add->top;
    if (add->right > acc->right) acc->right = add->right;
    if (add->bottom > acc->bottom) acc->bottom = add->bottom;
}

static void fill_marker(tl_rect_t r) {
    for (long y = r.top; y < r.bottom; y++)
        for (long x = r.left; x < r.right; x++) L.raw[(size_t)y * L.w + (size_t)x] = TL_MARKER;
}

/* The layer for this frame on a w x h surface, empty if it was last written in an earlier frame. */
static int begin(unsigned w, unsigned h) {
    if (!w || !h) return 0;
    if (L.w != w || L.h != h || !L.raw) {
        free(L.raw);
        L.raw = (uint16_t*)malloc((size_t)w * h * sizeof *L.raw);
        L.w = L.raw ? w : 0;
        L.h = L.raw ? h : 0;
        memset(&L.dirty, 0, sizeof L.dirty);
        if (!L.raw) return 0;
        fill_marker((tl_rect_t){ 0, 0, (long)w, (long)h });
    } else if (L.stamp != L.frame) {
        fill_marker(L.dirty);
        memset(&L.dirty, 0, sizeof L.dirty);
    }
    L.stamp = L.frame;
    return 1;
}

/* A box over a quarter of the surface is not a line of text: that draw stays in the picture. */
static int clamp_box(unsigned w, unsigned h, const tl_rect_t* in, tl_rect_t* out) {
    *out = *in;
    if (out->left < 0) out->left = 0;
    if (out->top < 0) out->top = 0;
    if (out->right > (long)w) out->right = (long)w;
    if (out->bottom > (long)h) out->bottom = (long)h;
    if (empty(out)) return 0;
    return (unsigned long long)(out->right - out->left) * (unsigned long long)(out->bottom - out->top) * 4u <=
           (unsigned long long)w * h;
}

static void add_box(tl_rect_t* list, int* count, const tl_rect_t* box) {
    if (*count < BOXES) list[(*count)++] = *box;
    else merge(&list[BOXES - 1], box);
}

void tl_note(unsigned w, unsigned h, const tl_rect_t* box) {
    tl_rect_t c;
    if (clamp_box(w, h, box, &c)) add_box(L.boxes, &L.box_count, &c);
}

void tl_scan(const uint8_t* bits, long pitch, unsigned w, unsigned h) {
    if (!L.box_count) return;
    if (!bits || !begin(w, h)) {
        L.box_count = 0;
        return;
    }
    for (int i = 0; i < L.box_count; i++) {
        const tl_rect_t* b = &L.boxes[i];
        int wrote = 0;
        for (long y = b->top; y < b->bottom; y++) {
            const uint16_t* row = (const uint16_t*)(bits + (size_t)y * (size_t)pitch);
            uint16_t* out = L.raw + (size_t)y * L.w;
            for (long x = b->left; x < b->right; x++)
                if (row[x] != TL_MARKER) {
                    out[x] = row[x];
                    wrote = 1;
                }
        }
        if (wrote) merge(&L.dirty, b);
    }
    L.box_count = 0;
}

static int claim_arrays(unsigned w, unsigned h) {
    if (L.claim_w == w && L.claim_h == h && L.claimed && L.saved) return 1;
    free(L.claimed);
    free(L.saved);
    L.claimed = (uint8_t*)calloc((size_t)w * h, 1);
    L.saved = (uint16_t*)malloc((size_t)w * h * sizeof *L.saved);
    L.claim_w = L.claimed && L.saved ? w : 0;
    L.claim_h = L.claimed && L.saved ? h : 0;
    return L.claim_w != 0;
}

static void settle(void) {
    int keep;
    if (!L.bits) return;
    keep = begin(L.claim_w, L.claim_h);
    for (int i = 0; i < L.claim_count; i++) {
        const tl_rect_t* b = &L.claims[i];
        int wrote = 0;
        for (long y = b->top; y < b->bottom; y++) {
            uint16_t* row = (uint16_t*)(L.bits + (size_t)y * (size_t)L.pitch);
            for (long x = b->left; x < b->right; x++) {
                const size_t at = (size_t)y * L.claim_w + (size_t)x;
                if (!L.claimed[at]) continue;
                L.claimed[at] = 0;
                if (row[x] == TL_MARKER) {
                    row[x] = L.saved[at];
                } else if (keep) {
                    L.raw[at] = row[x];
                    wrote = 1;
                }
            }
        }
        if (wrote) merge(&L.dirty, b);
    }
    L.claim_count = 0;
    L.bits = NULL;
}

int tl_claim(uint8_t* bits, long pitch, unsigned w, unsigned h, const tl_rect_t* box) {
    tl_rect_t c;
    if (!bits || !clamp_box(w, h, box, &c)) return 0;
    if (L.bits && L.bits != bits) settle();
    if (!claim_arrays(w, h)) return 0;
    L.bits = bits;
    L.pitch = pitch;
    for (long y = c.top; y < c.bottom; y++) {
        uint16_t* row = (uint16_t*)(bits + (size_t)y * (size_t)pitch);
        for (long x = c.left; x < c.right; x++) {
            const size_t at = (size_t)y * w + (size_t)x;
            if (L.claimed[at]) continue;
            L.claimed[at] = 1;
            L.saved[at] = row[x];
            row[x] = TL_MARKER;
        }
    }
    add_box(L.claims, &L.claim_count, &c);
    return 1;
}

void tl_settle(const uint8_t* bits) {
    if (L.bits && L.bits == bits) settle();
}

void tl_frame_end(void) {
    settle();
    L.frame++;
}

int tl_current(tl_view_t* out) {
    if (!L.raw || L.stamp != L.frame || empty(&L.dirty)) return 0;
    out->w = L.w;
    out->h = L.h;
    out->dirty = L.dirty;
    return 1;
}

void tl_expand(const tl_rect_t* region, const uint32_t* table, uint32_t* out, size_t out_pitch_pixels) {
    for (long y = region->top; y < region->bottom; y++) {
        const uint16_t* in = L.raw + (size_t)y * L.w;
        uint32_t* row = out + (size_t)(y - region->top) * out_pitch_pixels;
        for (long x = region->left; x < region->right; x++)
            row[x - region->left] = in[x] == TL_MARKER ? 0u : table[in[x]] | 0xFF000000u;
    }
}
