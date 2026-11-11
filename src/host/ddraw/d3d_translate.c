#include "d3d_translate.h"

#include <stdio.h>
#include <string.h>

unsigned d3dt_primitive_count(int type, unsigned vertices) {
    switch (type) {
    case D3DT_PT_POINTLIST: return vertices;
    case D3DT_PT_LINELIST: return vertices / 2;
    case D3DT_PT_LINESTRIP: return vertices >= 2 ? vertices - 1 : 0;
    case D3DT_PT_TRIANGLELIST: return vertices / 3;
    case D3DT_PT_TRIANGLESTRIP:
    case D3DT_PT_TRIANGLEFAN: return vertices >= 3 ? vertices - 2 : 0;
    default: return 0;
    }
}

/* Fan triangle i is (v[i], v[i+1], v[0]): the same winding as (v0, vi, vi+1), with v[i] first. */
unsigned d3dt_fan_indices(uint32_t first, unsigned count, uint32_t* out) {
    unsigned n = 0;
    for (unsigned i = 1; i + 1 < count; i++) {
        out[n++] = first + i;
        out[n++] = first + i + 1;
        out[n++] = first;
    }
    return n;
}

unsigned d3dt_strip_indices(uint32_t first, unsigned count, uint32_t* out) {
    unsigned n = 0;
    for (unsigned i = 0; i + 2 < count; i++) {
        out[n++] = first + i;
        out[n++] = first + i + ((i & 1) ? 2 : 1);
        out[n++] = first + i + ((i & 1) ? 1 : 2);
    }
    return n;
}

unsigned d3dt_list_indices(uint32_t first, unsigned count, uint32_t* out) {
    unsigned n = count - count % 3;
    for (unsigned i = 0; i < n; i++) out[i] = first + i;
    return n;
}

unsigned d3dt_indexed_triangles(int type, const uint16_t* in, unsigned count, uint32_t base, uint32_t* out) {
    unsigned n = 0;
    switch (type) {
    case D3DT_PT_TRIANGLELIST:
        for (unsigned i = 0; i + 2 < count; i += 3) {
            out[n++] = base + in[i];
            out[n++] = base + in[i + 1];
            out[n++] = base + in[i + 2];
        }
        break;
    case D3DT_PT_TRIANGLESTRIP:
        for (unsigned i = 0; i + 2 < count; i++) {
            out[n++] = base + in[i];
            out[n++] = base + in[i + ((i & 1) ? 2 : 1)];
            out[n++] = base + in[i + ((i & 1) ? 1 : 2)];
        }
        break;
    case D3DT_PT_TRIANGLEFAN:
        for (unsigned i = 1; i + 1 < count; i++) {
            out[n++] = base + in[i];
            out[n++] = base + in[i + 1];
            out[n++] = base + in[0];
        }
        break;
    default: break;
    }
    return n;
}

/* D3DBLEND 1..11 share D3D11_BLEND's numbering; colour factors have alpha twins for the alpha channel. */
int d3dt_blend(uint32_t value, int alpha_channel) {
    if (value < 1 || value > 11) return 0;
    if (alpha_channel) {
        if (value == 3) return 5;
        if (value == 4) return 6;
        if (value == 9) return 7;
        if (value == 10) return 8;
    }
    return (int)value;
}

int d3dt_blend_pair(uint32_t src, uint32_t dst, int* src11, int* dst11, int* src_alpha11, int* dst_alpha11) {
    if (src == 12 || src == 13) {
        dst = src == 12 ? 6 : 5;
        src = src == 12 ? 5 : 6;
    }
    *src11 = d3dt_blend(src, 0);
    *dst11 = d3dt_blend(dst, 0);
    *src_alpha11 = d3dt_blend(src, 1);
    *dst_alpha11 = d3dt_blend(dst, 1);
    return *src11 && *dst11;
}

int d3dt_compare(uint32_t value) { return value >= 1 && value <= 8 ? (int)value : 0; }

int d3dt_address(uint32_t value) { return value >= 1 && value <= 4 ? (int)value : 0; }

/* D3DCULL_CCW (the default) removes counter-clockwise triangles: D3D11 back faces with clockwise fronts. */
int d3dt_cull(uint32_t value) {
    switch (value) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    default: return 0;
    }
}

int d3dt_fill(uint32_t value) { return value == 2 ? 2 : value == 3 ? 3 : 0; }

int d3dt_mag_linear(uint32_t value) {
    if (value == 1) return 0;
    if (value >= 2 && value <= 5) return 1;
    return -1;
}

int d3dt_min_linear(uint32_t value) {
    if (value == 1) return 0;
    if (value == 2 || value == 3) return 1;
    return -1;
}

/* D3DTFP_NONE 1, POINT 2, LINEAR 3 -> 0 none, 1 point, 2 linear. */
int d3dt_mip_mode(uint32_t value) { return value >= 1 && value <= 3 ? (int)value - 1 : -1; }

/* Direct3D 6 reads the reference from the low byte (0x1000 is the game's "alpha > 0"). */
uint32_t d3dt_alpha_ref(uint32_t value) { return value & 0xFF; }

const char* d3dt_render_state_name(uint32_t state) {
    static const char* const names[] = {
        NULL, "TEXTUREHANDLE", "ANTIALIAS", "TEXTUREADDRESS", "TEXTUREPERSPECTIVE", "WRAPU", "WRAPV", "ZENABLE",
        "FILLMODE", "SHADEMODE", "LINEPATTERN", "MONOENABLE", "ROP2", "PLANEMASK", "ZWRITEENABLE", "ALPHATESTENABLE",
        "LASTPIXEL", "TEXTUREMAG", "TEXTUREMIN", "SRCBLEND", "DESTBLEND", "TEXTUREMAPBLEND", "CULLMODE", "ZFUNC",
        "ALPHAREF", "ALPHAFUNC", "DITHERENABLE", "ALPHABLENDENABLE", "FOGENABLE", "SPECULARENABLE", "ZVISIBLE",
        "SUBPIXEL", "SUBPIXELX", "STIPPLEDALPHA", "FOGCOLOR", "FOGTABLEMODE", "FOGTABLESTART", "FOGTABLEEND",
        "FOGTABLEDENSITY", "STIPPLEENABLE", "EDGEANTIALIAS", "COLORKEYENABLE", "OLDALPHABLENDENABLE", "BORDERCOLOR",
        "TEXTUREADDRESSU", "TEXTUREADDRESSV", "MIPMAPLODBIAS", "ZBIAS", "RANGEFOGENABLE", "ANISOTROPY", "FLUSHBATCH",
        "TRANSLUCENTSORTINDEPENDENT", "STENCILENABLE", "STENCILFAIL", "STENCILZFAIL", "STENCILPASS", "STENCILFUNC",
        "STENCILREF", "STENCILMASK", "STENCILWRITEMASK", "TEXTUREFACTOR",
    };
    if (state < sizeof names / sizeof names[0] && names[state]) return names[state];
    if (state >= 64 && state < 96) return "STIPPLEPATTERN";
    if (state >= 128 && state < 136) return "WRAP";
    return "unknown";
}

const char* d3dt_stage_state_name(uint32_t type) {
    static const char* const names[] = {
        NULL, "COLOROP", "COLORARG1", "COLORARG2", "ALPHAOP", "ALPHAARG1", "ALPHAARG2", "BUMPENVMAT00",
        "BUMPENVMAT01", "BUMPENVMAT10", "BUMPENVMAT11", "TEXCOORDINDEX", "ADDRESS", "ADDRESSU", "ADDRESSV",
        "BORDERCOLOR", "MAGFILTER", "MINFILTER", "MIPFILTER", "MIPMAPLODBIAS", "MAXMIPLEVEL", "MAXANISOTROPY",
        "BUMPENVLSCALE", "BUMPENVLOFFSET",
    };
    return type < sizeof names / sizeof names[0] && names[type] ? names[type] : "unknown";
}

#define DEPTH_BIAS_UNITS_PER_STEP 84

int d3dt_depth_bias(uint32_t zbias, uint32_t zfunc) {
    int steps = (int)(zbias > 16 ? 16 : zbias);
    int towards_greater = zfunc == 5 || zfunc == 7;
    return (towards_greater ? DEPTH_BIAS_UNITS_PER_STEP : -DEPTH_BIAS_UNITS_PER_STEP) * steps;
}

static uint32_t channel(uint32_t c, int shift) { return (c >> shift) & 0xFF; }

static uint32_t pack_argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return ((a > 255 ? 255 : a) << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

static uint32_t mul8(uint32_t a, uint32_t b) { return (a * b + 127) / 255; }

int d3dt_texture_blend_known(uint32_t mode) { return mode >= 1 && mode <= 8; }

uint32_t d3dt_texture_blend(uint32_t mode, uint32_t texel, uint32_t diffuse, int texture_alpha) {
    uint32_t ta = texture_alpha ? channel(texel, 24) : 255, da = channel(diffuse, 24);
    uint32_t tr = channel(texel, 16), tg = channel(texel, 8), tb = channel(texel, 0);
    uint32_t dr = channel(diffuse, 16), dg = channel(diffuse, 8), db = channel(diffuse, 0);
    switch (mode) {
    case 1: /* DECAL */
    case 7: /* COPY */
        return pack_argb(ta, tr, tg, tb);
    case 5: /* DECALMASK: the texture where its alpha's low bit is set, else the vertex colour */
        return (ta & 1) ? pack_argb(da, tr, tg, tb) : pack_argb(da, dr, dg, db);
    case 6: /* MODULATEMASK */
        return (ta & 1) ? pack_argb(da, mul8(tr, dr), mul8(tg, dg), mul8(tb, db)) : pack_argb(da, dr, dg, db);
    case 3: /* DECALALPHA */
        return pack_argb(da, mul8(dr, 255 - ta) + mul8(tr, ta), mul8(dg, 255 - ta) + mul8(tg, ta),
                         mul8(db, 255 - ta) + mul8(tb, ta));
    case 4: /* MODULATEALPHA */
        return pack_argb(mul8(ta, da), mul8(tr, dr), mul8(tg, dg), mul8(tb, db));
    case 8: /* ADD */
        return pack_argb(da, tr + dr, tg + dg, tb + db);
    default: /* MODULATE: texture alpha when the format has one, else the vertex alpha */
        return pack_argb(texture_alpha ? ta : da, mul8(tr, dr), mul8(tg, dg), mul8(tb, db));
    }
}

uint32_t d3dt_apply_fog(uint32_t color, uint32_t fog_color, uint32_t fog_alpha) {
    uint32_t f = fog_alpha > 255 ? 255 : fog_alpha, out = color & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t c = channel(color, shift), g = channel(fog_color, shift);
        out |= ((c * f + g * (255 - f) + 127) / 255) << shift;
    }
    return out;
}

int d3dt_fvf_layout(uint32_t fvf, unsigned* stride, unsigned* diffuse, unsigned* specular, unsigned* uv) {
    unsigned offset = 16, textures = (fvf & D3DT_FVF_TEXCOUNT_MASK) >> D3DT_FVF_TEXCOUNT_SHIFT;
    if ((fvf & D3DT_FVF_POSITION_MASK) != D3DT_FVF_XYZRHW) return 0;
    if (fvf & ~(uint32_t)(D3DT_FVF_POSITION_MASK | D3DT_FVF_DIFFUSE | D3DT_FVF_SPECULAR | D3DT_FVF_TEXCOUNT_MASK)) return 0;
    *diffuse = *specular = *uv = 0;
    if (fvf & D3DT_FVF_DIFFUSE) *diffuse = offset, offset += 4;
    if (fvf & D3DT_FVF_SPECULAR) *specular = offset, offset += 4;
    if (textures) *uv = offset;
    *stride = offset + textures * 8;
    return 1;
}

int d3dt_depth_test(uint32_t func, float incoming, float stored) {
    switch (func) {
    case 1: return 0;
    case 2: return incoming < stored;
    case 3: return incoming == stored;
    case 4: return incoming <= stored;
    case 5: return incoming > stored;
    case 6: return incoming != stored;
    case 7: return incoming >= stored;
    default: return 1;
    }
}

/* Screen space has y down, so a clockwise triangle has a positive signed area. */
float d3dt_signed_area(const float* x, const float* y) {
    return (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
}

int d3dt_triangle_culled(uint32_t cull, const float* x, const float* y) {
    float area = d3dt_signed_area(x, y);
    if (cull == 2) return area > 0.0f;
    if (cull == 3) return area < 0.0f;
    return 0;
}

int d3dt_barycentric(const float* x, const float* y, float px, float py, float w[3]) {
    float area = d3dt_signed_area(x, y);
    if (area == 0.0f || area != area) return 0;
    w[0] = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
    w[1] = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
    w[2] = 1.0f - w[0] - w[1];
    return w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f;
}

void d3dt_perspective_uv(const float* u, const float* v, const float* rhw, const float w[3], float* out_u, float* out_v) {
    float q = w[0] * rhw[0] + w[1] * rhw[1] + w[2] * rhw[2];
    if (q == 0.0f || q != q) {
        *out_u = w[0] * u[0] + w[1] * u[1] + w[2] * u[2];
        *out_v = w[0] * v[0] + w[1] * v[1] + w[2] * v[2];
        return;
    }
    *out_u = (w[0] * u[0] * rhw[0] + w[1] * u[1] * rhw[1] + w[2] * u[2] * rhw[2]) / q;
    *out_v = (w[0] * v[0] * rhw[0] + w[1] * v[1] * rhw[1] + w[2] * v[2] * rhw[2]) / q;
}

int d3dt_float_bad(float f) { return f != f || f > 3.0e38f || f < -3.0e38f; }

uint32_t d3dt_state_fingerprint(const d3dt_draw_state_t* s) {
    const unsigned char* p = (const unsigned char*)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *s; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void d3dt_describe_state(const d3dt_draw_state_t* s, char* out, unsigned cap) {
    if (!cap) return;
    _snprintf(out, cap - 1, "tex %s, tmb %u, blend %s %u/%u, alphatest %s func %u ref 0x%X, fog %s 0x%06X, specular %s, "
              "shade %u, z %s%s func %u, cull %u, fill %u, colourkey %s, address %u/%u, filter mag %u min %u",
              s->textured ? "set" : "none", s->tmb, s->blend_enable ? "on" : "off", s->src_blend, s->dst_blend,
              s->alpha_test ? "on" : "off", s->alpha_func, s->alpha_ref, s->fog ? "on" : "off", s->fog_color & 0xFFFFFFu,
              s->specular ? "on" : "off", s->shade, s->zenable ? "on" : "off", s->zwrite ? "+write" : "", s->zfunc,
              s->cull, s->fill, s->colorkey ? "on" : "off", s->address_u, s->address_v, s->mag, s->min);
    out[cap - 1] = 0;
}

int d3dt_format_from_masks(uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (bits == 16) {
        if (r == 0xF800 && g == 0x07E0 && b == 0x001F && !a) return D3DT_FMT_RGB565;
        if (r == 0x7C00 && g == 0x03E0 && b == 0x001F) return a == 0x8000 ? D3DT_FMT_ARGB1555 : a ? 0 : D3DT_FMT_XRGB1555;
        if (r == 0x0F00 && g == 0x00F0 && b == 0x000F) return a == 0xF000 ? D3DT_FMT_ARGB4444 : a ? 0 : D3DT_FMT_XRGB4444;
    } else if (bits == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF) {
        return a == 0xFF000000u ? D3DT_FMT_ARGB8888 : a ? 0 : D3DT_FMT_XRGB8888;
    }
    return D3DT_FMT_UNKNOWN;
}

int d3dt_format_from_pixelformat(uint32_t flags, uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (flags & D3DT_PF_PALETTEINDEXED8) return bits == 8 ? D3DT_FMT_PAL8 : D3DT_FMT_UNKNOWN;
    if (!(flags & D3DT_PF_RGB)) return D3DT_FMT_UNKNOWN;
    return d3dt_format_from_masks(bits, r, g, b, (flags & D3DT_PF_ALPHAPIXELS) ? a : 0);
}

int d3dt_format_has_alpha(int format) {
    return format == D3DT_FMT_ARGB1555 || format == D3DT_FMT_ARGB4444 || format == D3DT_FMT_ARGB8888;
}

int d3dt_format_bytes(int format) {
    if (format == D3DT_FMT_XRGB8888 || format == D3DT_FMT_ARGB8888) return 4;
    if (format == D3DT_FMT_PAL8) return 1;
    return format > D3DT_FMT_UNKNOWN && format < D3DT_FMT_COUNT ? 2 : 0;
}

const char* d3dt_format_name(int format) {
    static const char* const names[D3DT_FMT_COUNT] = { "unknown", "RGB565", "XRGB1555", "ARGB1555", "ARGB4444",
                                                       "XRGB8888", "ARGB8888", "XRGB4444", "PAL8" };
    return format >= 0 && format < D3DT_FMT_COUNT ? names[format] : "unknown";
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t expand4(uint32_t v) { return (v << 4) | v; }

uint32_t d3dt_rgb565_to_bgra(uint16_t v) {
    return 0xFF000000u | (expand5((v >> 11) & 0x1F) << 16) | (expand6((v >> 5) & 0x3F) << 8) | expand5(v & 0x1F);
}

uint16_t d3dt_bgra_to_rgb565(uint32_t v) {
    uint32_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return (uint16_t)((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) | ((b * 31 + 127) / 255));
}

static uint32_t xrgb1555(uint32_t v) {
    return 0xFF000000u | (expand5((v >> 10) & 0x1F) << 16) | (expand5((v >> 5) & 0x1F) << 8) | expand5(v & 0x1F);
}

static uint32_t argb1555(uint32_t v) { return (v & 0x8000) ? xrgb1555(v) : xrgb1555(v) & 0x00FFFFFFu; }

static uint32_t argb4444(uint32_t v) {
    return (expand4((v >> 12) & 0xF) << 24) | (expand4((v >> 8) & 0xF) << 16) | (expand4((v >> 4) & 0xF) << 8) |
           expand4(v & 0xF);
}

static uint32_t xrgb4444(uint32_t v) { return argb4444(v) | 0xFF000000u; }
static uint32_t rgb565(uint32_t v) { return d3dt_rgb565_to_bgra((uint16_t)v); }
static uint32_t xrgb8888(uint32_t v) { return v | 0xFF000000u; }
static uint32_t argb8888(uint32_t v) { return v; }

/* One loop per format, so a whole texture row converts without a per-texel switch. */
#define CONVERT_ROW(type, convert)                                                                                         for (unsigned x = 0; x < count; x++) {                                                                                     uint32_t raw = ((const type*)src)[x], c = convert(raw);                                                                dst[x] = keyed && raw >= key_low && raw <= key_high ? c & 0x00FFFFFFu : c;                                         }

void d3dt_convert_row(int format, const void* src, uint32_t* dst, unsigned count, int keyed, uint32_t key_low,
                      uint32_t key_high) {
    switch (format) {
    case D3DT_FMT_RGB565: CONVERT_ROW(uint16_t, rgb565) break;
    case D3DT_FMT_XRGB1555: CONVERT_ROW(uint16_t, xrgb1555) break;
    case D3DT_FMT_ARGB1555: CONVERT_ROW(uint16_t, argb1555) break;
    case D3DT_FMT_ARGB4444: CONVERT_ROW(uint16_t, argb4444) break;
    case D3DT_FMT_XRGB4444: CONVERT_ROW(uint16_t, xrgb4444) break;
    case D3DT_FMT_XRGB8888: CONVERT_ROW(uint32_t, xrgb8888) break;
    case D3DT_FMT_ARGB8888: CONVERT_ROW(uint32_t, argb8888) break;
    default:
        for (unsigned x = 0; x < count; x++) dst[x] = 0;
    }
}

void d3dt_convert_row_pal8(const uint8_t* src, uint32_t* dst, unsigned count, const uint32_t* palette, int keyed,
                           uint32_t key_low, uint32_t key_high) {
    for (unsigned x = 0; x < count; x++) {
        uint32_t c = palette[src[x]];
        dst[x] = keyed && src[x] >= key_low && src[x] <= key_high ? c & 0x00FFFFFFu : c;
    }
}

int d3dt_row_black(const uint32_t* bgra, unsigned count) {
    uint32_t any = 0;
    for (unsigned x = 0; x < count; x++) any |= bgra[x];
    return (any & 0x00FFFFFFu) == 0;
}

unsigned d3dt_overlay_row(const uint16_t* current, const uint16_t* pristine, unsigned count, uint32_t* out) {
    unsigned changed = 0;
    for (unsigned x = 0; x < count; x++) {
        if (current[x] != pristine[x]) {
            out[x] = d3dt_rgb565_to_bgra(current[x]);
            changed++;
        } else {
            out[x] = 0;
        }
    }
    return changed;
}

unsigned d3dt_black_over_content(const uint16_t* current, const uint16_t* previous, unsigned count) {
    unsigned n = 0;
    for (unsigned x = 0; x < count; x++) n += current[x] == 0 && previous[x] != 0;
    return n;
}

int d3dt_row_span(const uint16_t* current, const uint16_t* pristine, unsigned count, unsigned* first, unsigned* end) {
    unsigned a = 0, b = count;
    if (!memcmp(current, pristine, (size_t)count * 2)) return 0;
    while (a < count && current[a] == pristine[a]) a++;
    while (b > a && current[b - 1] == pristine[b - 1]) b--;
    *first = a;
    *end = b;
    return 1;
}

static long scale_floor(long v, unsigned from, unsigned to) { return (long)(((long long)v * to) / from); }

static long scale_ceil(long v, unsigned from, unsigned to) { return (long)(((long long)v * to + from - 1) / from); }

d3dt_rect_t d3dt_scale_rect(long x, long y, long w, long h, unsigned legacy_w, unsigned legacy_h, unsigned target_w,
                            unsigned target_h) {
    d3dt_rect_t r = { 0, 0, 0, 0 };
    long right = x + w, bottom = y + h;
    if (!legacy_w || !legacy_h) return r;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (long)legacy_w) right = (long)legacy_w;
    if (bottom > (long)legacy_h) bottom = (long)legacy_h;
    if (right <= x || bottom <= y) return r;
    r.left = scale_floor(x, legacy_w, target_w);
    r.top = scale_floor(y, legacy_h, target_h);
    r.right = scale_ceil(right, legacy_w, target_w);
    r.bottom = scale_ceil(bottom, legacy_h, target_h);
    return r;
}

int d3dt_rect_empty(const d3dt_rect_t* r) { return r->right <= r->left || r->bottom <= r->top; }

void d3dt_rect_union(d3dt_rect_t* acc, const d3dt_rect_t* add) {
    if (d3dt_rect_empty(add)) return;
    if (d3dt_rect_empty(acc)) {
        *acc = *add;
        return;
    }
    if (add->left < acc->left) acc->left = add->left;
    if (add->top < acc->top) acc->top = add->top;
    if (add->right > acc->right) acc->right = add->right;
    if (add->bottom > acc->bottom) acc->bottom = add->bottom;
}

int d3dt_parse_resolution(const char* text, unsigned* w, unsigned* h) {
    unsigned a = 0, b = 0;
    char extra;
    if (!text) return 0;
    if (!strcmp(text, "auto")) {
        *w = *h = 0;
        return 1;
    }
    if (sscanf(text, "%ux%u%c", &a, &b, &extra) != 2 || a < 320 || b < 240 || a > 8192 || b > 8192) return 0;
    *w = a;
    *h = b;
    return 1;
}

/* The smallest 4:3 size that covers the view, between the legacy mode and 2560x1920. */
void d3dt_auto_resolution(unsigned view_w, unsigned view_h, unsigned legacy_w, unsigned legacy_h, unsigned* w,
                          unsigned* h) {
    static const unsigned heights[] = { 480, 600, 768, 960, 1200, 1440, 1920 };
    unsigned need = view_h;
    unsigned i;
    if (view_w * 3 < view_h * 4) need = (view_w * 3 + 3) / 4;
    if (need < legacy_h) need = legacy_h;
    for (i = 0; i + 1 < sizeof heights / sizeof heights[0] && heights[i] < need; i++) {}
    *h = heights[i];
    *w = heights[i] * 4 / 3;
    if (legacy_w * 3 != legacy_h * 4 && legacy_h) *w = (unsigned)(((unsigned long long)heights[i] * legacy_w) / legacy_h);
}

unsigned d3dt_pick_msaa(unsigned requested, uint32_t supported) {
    for (unsigned n = requested > 16 ? 16 : requested; n > 1; n--)
        if (supported & (1u << n)) return n;
    return 1;
}

void d3dt_matrix_multiply(const float* a, const float* b, float* out) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof r);
}
