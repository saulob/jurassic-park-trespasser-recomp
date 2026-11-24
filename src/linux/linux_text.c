#include "linux_text.h"

#include <math.h>
#include <string.h>

#include "imstb_truetype.h"
#include "jpt_l10n.h"

extern const unsigned char lx_font_data[];
extern const unsigned long lx_font_data_size;

#define GLYPH_MAX 192

static stbtt_fontinfo s_font;
static int s_ready, s_codepage = 1252;

static int font_ready(void) {
    if (!s_ready) s_ready = stbtt_InitFont(&s_font, lx_font_data, stbtt_GetFontOffsetForIndex(lx_font_data, 0)) ? 1 : -1;
    return s_ready > 0;
}

void lx_text_set_codepage(int codepage) { s_codepage = codepage == 1251 ? 1251 : 1252; }

int lx_text_codepage(void) { return s_codepage; }

int lx_text_active_codepage(void) {
    int cp = jpt_l10n_display_codepage();
    return cp ? cp : s_codepage;
}

const char* lx_text_to_utf8(const char* src, int codepage, char* out, size_t cap) {
    size_t n;
    int cp;
    if (!src) return "";
    n = strlen(src);
    if (jpt_utf8_valid(src, n)) return src;
    if (!out || !cap) return "";
    cp = codepage == 1251 ? 1251 : 1252;
    if (jpt_cp_to_utf8(cp, src, n, out, cap) != (size_t)-1) return out;
    {
        size_t o = 0;
        char tmp[8];
        for (size_t i = 0; i < n; i++) {
            size_t k = jpt_cp_to_utf8(cp, src + i, 1, tmp, sizeof tmp);
            if (k == (size_t)-1 || o + k >= cap) break;
            memcpy(out + o, tmp, k);
            o += k;
        }
        out[o] = 0;
    }
    return out;
}

static int to_unicode(uint8_t c) {
    static const uint16_t cp1252[32] = { 0x20AC, 0x20, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x20,
                                         0x017D, 0x20, 0x20, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
                                         0x0153, 0x20, 0x017E, 0x0178 };
    if (c < 0x80) return c;
    if (s_codepage == 1251) {
        if (c >= 0xC0) return 0x410 + (c - 0xC0);
        if (c == 0xA8) return 0x401;
        if (c == 0xB8) return 0x451;
        if (c == 0xB9) return 0x2116;
        return c >= 0xA0 ? c : cp1252[c - 0x80];
    }
    return c < 0xA0 ? cp1252[c - 0x80] : c;
}

float lx_text_scale(int gdi_height) {
    if (!font_ready()) return 1.0f;
    if (gdi_height < 0) return stbtt_ScaleForMappingEmToPixels(&s_font, (float)-gdi_height);
    return stbtt_ScaleForPixelHeight(&s_font, gdi_height ? (float)gdi_height : 16.0f);
}

void lx_text_metrics(float scale, lx_text_metrics_t* out) {
    int asc = 0, desc = 0, gap = 0, x0, y0, x1, y1;
    memset(out, 0, sizeof *out);
    if (!font_ready()) return;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    out->ascent = (int)ceilf((float)asc * scale);
    out->descent = (int)ceilf((float)-desc * scale);
    out->height = out->ascent + out->descent;
    out->avg_width = lx_text_advance(scale, 'x', 0);
    stbtt_GetFontBoundingBox(&s_font, &x0, &y0, &x1, &y1);
    out->max_width = (int)ceilf((float)(x1 - x0) * scale);
}

int lx_text_unicode_advance(float scale, int codepoint) {
    int adv = 0, lsb = 0;
    if (!font_ready()) return 0;
    stbtt_GetCodepointHMetrics(&s_font, codepoint, &adv, &lsb);
    return (int)lroundf((float)adv * scale);
}

int lx_text_advance(float scale, uint8_t ch, int bold) { return lx_text_unicode_advance(scale, to_unicode(ch)) + (bold ? 1 : 0); }

int lx_text_width(float scale, const char* s, int n, int bold) {
    int w = 0;
    for (int i = 0; i < n; i++) w += lx_text_advance(scale, (uint8_t)s[i], bold);
    return w;
}

void lx_text_unicode_glyph(float scale, int cp, int x, int baseline, lx_text_pixel_fn put, void* ctx) {
    static unsigned char bitmap[GLYPH_MAX * GLYPH_MAX];
    int x0, y0, x1, y1, w, h;
    if (!font_ready() || cp == ' ') return;
    stbtt_GetCodepointBitmapBox(&s_font, cp, scale, scale, &x0, &y0, &x1, &y1);
    w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > GLYPH_MAX || h > GLYPH_MAX) return;
    stbtt_MakeCodepointBitmap(&s_font, bitmap, w, h, GLYPH_MAX, scale, scale, cp);
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
            int cov = bitmap[row * GLYPH_MAX + col];
            if (!cov) continue;
            put(ctx, x + x0 + col, baseline + y0 + row, cov);
        }
}

void lx_text_glyph(float scale, uint8_t ch, int bold, int x, int baseline, lx_text_pixel_fn put, void* ctx) {
    lx_text_unicode_glyph(scale, to_unicode(ch), x, baseline, put, ctx);
    if (bold) lx_text_unicode_glyph(scale, to_unicode(ch), x + 1, baseline, put, ctx);
}
