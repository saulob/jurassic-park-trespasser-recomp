#ifndef JPT_VIDEO_FIXTURES_H
#define JPT_VIDEO_FIXTURES_H

#include <stdint.h>
#include <string.h>

#include "video_layout.h"

/* Synthetic decoder frames, one per video stage, the same for the Direct3D 11 and the Vulkan passes. */
typedef struct {
    const char* name;
    int width, stored_rows, shown_rows, rebuild, out_w, out_h;
    int smooth, deblock, stray, palette;
    int pattern;
} vf_fixture_t;

enum { VF_GRADIENT, VF_DITHER, VF_BLOCKS, VF_DIAGONAL, VF_STRAY, VF_EDGES };

static const vf_fixture_t k_vf_fixtures[] = {
    { "convert", 64, 48, 48, 0, 64, 48, 0, 0, 0, 0, VF_GRADIENT },
    { "convert_palette", 64, 48, 48, 0, 64, 48, 0, 0, 0, 1, VF_GRADIENT },
    { "smooth", 64, 48, 48, 0, 64, 48, 1, 0, 0, 0, VF_DITHER },
    { "deblock", 64, 48, 48, 0, 64, 48, 0, 1, 0, 0, VF_BLOCKS },
    { "newgame_rebuild", 64, 24, 48, 1, 64, 48, 0, 0, 0, 0, VF_DIAGONAL },
    { "stray", 160, 24, 24, 0, 160, 24, 0, 0, 1, 0, VF_STRAY },
    { "glow", 64, 48, 48, 0, 192, 48, 0, 0, 0, 0, VF_GRADIENT },
    { "catmull_4x3", 32, 24, 24, 0, 96, 72, 0, 0, 0, 0, VF_EDGES },
    { "catmull_wide", 32, 24, 24, 0, 128, 54, 0, 0, 0, 0, VF_EDGES },
    { "all_stages", 64, 48, 48, 0, 128, 96, 1, 1, 0, 1, VF_BLOCKS },
};
#define VF_COUNT ((int)(sizeof k_vf_fixtures / sizeof k_vf_fixtures[0]))

static inline uint32_t vf_next(uint32_t* state) { return *state = *state * 1103515245u + 12345u; }

static inline void vf_palette(uint8_t pal[768]) {
    for (int i = 0; i < 256; i++) {
        pal[i * 3] = (uint8_t)(i | 3);
        pal[i * 3 + 1] = (uint8_t)((i * 3) & 255);
        pal[i * 3 + 2] = (uint8_t)(255 - i);
    }
}

static inline void vf_codes(const vf_fixture_t* f, uint16_t* out) {
    uint32_t seed = 4242u;
    uint8_t pal[768];
    vf_palette(pal);
    for (int y = 0; y < f->stored_rows; y++)
        for (int x = 0; x < f->width; x++) {
            int v, r, g, b;
            switch (f->pattern) {
            case VF_DITHER:
                v = 90 + ((x + y) & 1 ? 8 : -8) + 3 * (x / 8);
                r = g = v;
                b = v + 10;
                break;
            case VF_BLOCKS:
                v = 40 + 6 * (x / 4) + 4 * (y / 4);
                r = g = b = v;
                if (f->palette) {
                    const uint8_t* c = pal + ((x / 4 + y / 4 * 3) & 255) * 3;
                    r = c[0];
                    g = c[1];
                    b = c[2];
                }
                break;
            case VF_DIAGONAL:
                v = ((x + y * 2) % 24) < 12 ? 40 : 210;
                r = v;
                g = 200 - v / 2;
                b = 30 + v / 3;
                break;
            case VF_STRAY:
                v = y >= f->stored_rows - 6 && x >= 4 && x < 9 ? 180 : 0;
                if (y == f->stored_rows - 1 && x >= 20) v = 32;
                r = g = b = v;
                break;
            case VF_EDGES:
                v = (x < 16 ? 30 : 220);
                r = v;
                g = (y < 12 ? 40 : 200);
                b = ((x + y) % 9 < 4) ? 255 : 20;
                break;
            default:
                r = x * 255 / (f->width - 1);
                g = y * 255 / (f->stored_rows - 1);
                b = (int)((vf_next(&seed) >> 16) & 63) + 96;
                if (f->palette && ((x + y) % 5 == 0)) {
                    const uint8_t* c = pal + ((x * 7 + y * 13) & 255) * 3;
                    r = c[0];
                    g = c[1];
                    b = c[2];
                }
                break;
            }
            out[(size_t)y * f->width + x] = vl_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
}

#endif
