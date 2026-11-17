#include "guest_va.h"
#include "bump_spans.h"

#define DELTA_TEX GVA(0x00737EA0)       /* iUVInt[0], iUVInt[1], uUFrac, uVFrac */
#define TILE_MASK GVA(0x006699F8)       /* u4TextureTileMask */
#define TEXTURE_LINE GVA(0x00737F34)    /* the texel raster's row length */
#define NEG_DU GVA(0x00737E90)          /* bfNegDU: fraction, integer */
#define NEG_DV GVA(0x00737E40)          /* w1dNegDV: fraction, integer */
#define EVEN_ROWS_ONLY GVA(0x00737F68)
#define SCREEN_LINE GVA(0x00737EC4)     /* u4LinePixels */
#define LINE_INCREMENT 0x38u        /* in the base edge: fxX step, fxXDifference step, U, V */

static uint32_t rd(uint32_t va) { return *(const volatile uint32_t*)(uintptr_t)va; }
static void wr(uint32_t va, uint32_t v) { *(volatile uint32_t*)(uintptr_t)va = v; }

int bump_texel_rgb565(const bump_pixel_t* c, uint16_t texel, int transparent, uint16_t* out) {
    uint32_t angle, intensity;
    if (transparent && !(texel & 0xFC00u)) return 0;
    angle = ((uint32_t)texel - c->light_theta) & 0x3FFu;
    intensity = c->bump[angle];
    *out = c->clut[c->clut_base + intensity * 64u + ((uint32_t)texel >> 10)];
    return 1;
}

/* add ecx, dv; sbb ebp, ebp; add esi, du; adc edx, [iUVInt + 4 + ebp * 4]; and edx, mask */
void bump_walk_step(bump_walk_state_t* s, const bump_walk_t* d) {
    uint32_t v = s->vfrac + d->dv_frac, u = s->ufrac + d->du_frac;
    uint32_t v_carry = v < s->vfrac, u_carry = u < s->ufrac;
    s->vfrac = v;
    s->ufrac = u;
    s->index = (s->index + (uint32_t)(v_carry ? d->step_carry : d->step) + u_carry) & d->tile_mask;
}

/* The fractions' low 16 bits are zero, so k additions carry ((f >> 16) + k * (step >> 16)) >> 16 times; inside a tiled
 * mask U never passes 511 between two masks, so U and V can be masked once each. */
uint32_t bump_walk_index(const bump_walk_state_t* s, const bump_walk_t* d, uint32_t k) {
    uint32_t carries_u = ((s->ufrac >> 16) + k * (d->du_frac >> 16)) >> 16;
    uint32_t carries_v = ((s->vfrac >> 16) + k * (d->dv_frac >> 16)) >> 16;
    uint32_t du, dv, u, v;
    if (d->tile_mask == 0xFFFFFFFFu) return s->index + k * (uint32_t)d->step + carries_v * d->line_pixels + carries_u;
    du = (uint32_t)d->step & 0x1FFu;
    dv = (uint32_t)d->step - du;
    u = ((s->index & 0x1FFu) + k * du + carries_u) & (d->tile_mask & 0x1FFu);
    v = ((s->index & ~0x1FFu) + k * dv + (carries_v << 9)) & (d->tile_mask & ~0x1FFu);
    return v | u;
}

void bump_walk_from_guest(bump_walk_t* d) {
    d->step_carry = (int32_t)rd(DELTA_TEX);
    d->step = (int32_t)rd(DELTA_TEX + 4);
    d->du_frac = rd(DELTA_TEX + 8);
    d->dv_frac = rd(DELTA_TEX + 12);
    d->tile_mask = rd(TILE_MASK);
    d->line_pixels = rd(TEXTURE_LINE);
}

void bump_linear_spans(uint32_t pscan, uint32_t pdtri, const bump_walk_t* d, bump_span_sink_t sink, void* context) {
    uint32_t inc = rd(pdtri + 0x58) + LINE_INCREMENT;
    int32_t fx_inc = (int32_t)rd(inc), fx_diff = (int32_t)rd(inc + 4);
    uint32_t iu_frac = rd(inc + 0x10), iv_frac = rd(inc + 0x18);
    int32_t iu_int = (int32_t)rd(inc + 0x14), iv_int = (int32_t)rd(inc + 0x1C);
    uint32_t nu_frac = rd(NEG_DU), nv_frac = rd(NEG_DV);
    int32_t nu_int = (int32_t)rd(NEG_DU + 4), nv_int = (int32_t)rd(NEG_DV + 4);
    uint32_t even = rd(EVEN_ROWS_ONLY), line_pixels = rd(SCREEN_LINE);
    int32_t fx = (int32_t)rd(pscan), fx_rem = (int32_t)rd(pscan + 4);
    uint32_t ufrac = rd(pscan + 0x10), vfrac = rd(pscan + 0x18);
    int32_t uint_ = (int32_t)rd(pscan + 0x14), vint = (int32_t)rd(pscan + 0x1C), v_line = (int32_t)rd(pscan + 0x20);
    int32_t y = (int32_t)rd(pdtri), y_end = (int32_t)rd(pdtri + 4), line = (int32_t)rd(pdtri + 8);
    int32_t len = (int32_t)rd(pdtri + 0xC), dlen = (int32_t)rd(pdtri + 0x10);
    do {
        if (!((uint32_t)y & even)) {
            int32_t xr = (fx + len) >> 16, xl = fx >> 16;
            if (xl - xr < 0) {
                bump_span_t s;
                s.y = y;
                s.x0 = xl;
                s.n = xr - xl;
                s.dst_index = (uint32_t)(line + xl);
                s.start.index = ((uint32_t)uint_ + (uint32_t)vint) & d->tile_mask;
                s.start.ufrac = ufrac;
                s.start.vfrac = vfrac;
                sink(context, &s);
            }
        }
        vint += iv_int;
        {
            uint32_t t = ufrac + iu_frac;
            uint_ += iu_int + (t < ufrac);
            ufrac = t;
        }
        {
            uint32_t t = vfrac + iv_frac;
            if (t < vfrac) vint += v_line;
            vfrac = t;
        }
        fx_rem += fx_diff;
        if (fx_rem < 0 || fx_rem >= 0x10000) {
            fx_rem += fx_rem < 0 ? 0x10000 : -0x10000;
            vint += nv_int;
            {
                uint32_t t = ufrac + nu_frac;
                uint_ += nu_int + (t < ufrac);
                ufrac = t;
            }
            {
                uint32_t t = vfrac + nv_frac;
                if (t < vfrac) vint += v_line;
                vfrac = t;
            }
        }
        len += dlen;
        line += (int32_t)line_pixels;
        fx += fx_inc;
        y++;
    } while (y < y_end);
    wr(pscan, (uint32_t)fx);
    wr(pscan + 4, (uint32_t)fx_rem);
    wr(pscan + 0x10, ufrac);
    wr(pscan + 0x14, (uint32_t)uint_);
    wr(pscan + 0x18, vfrac);
    wr(pscan + 0x1C, (uint32_t)vint);
    wr(pdtri, (uint32_t)y);
    wr(pdtri + 8, (uint32_t)line);
    wr(pdtri + 0xC, (uint32_t)len);
}

uint32_t bump_shader_pixel(const bump_span_t* span, int32_t x, const bump_walk_t* d, const uint16_t* texels,
                           const bump_pixel_t* c, int transparent) {
    uint32_t texel = texels[bump_walk_index(&span->start, d, (uint32_t)(x - span->x0))];
    if (transparent && !(texel & 0xFC00u)) return 0x10000u;
    return c->clut[c->clut_base + (uint32_t)c->bump[(texel - c->light_theta) & 0x3FFu] * 64u + (texel >> 10)];
}

uint32_t texture_shader_pixel(const bump_span_t* span, int32_t x, const bump_walk_t* d, const uint8_t* texels,
                              const uint16_t* clut, uint32_t clut_base) {
    return clut[clut_base + texels[bump_walk_index(&span->start, d, (uint32_t)(x - span->x0))]];
}
