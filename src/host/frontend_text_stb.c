#include "frontend_text_stb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "imstb_truetype.h"
#include "jpt_l10n.h"

extern const unsigned char lx_font_data[];
extern const unsigned long lx_font_data_size;

#define TEXT_MAX 1024

static stbtt_fontinfo s_font;
static int s_ready;

static int font_ready(void) {
    if (!s_ready) s_ready = stbtt_InitFont(&s_font, lx_font_data, stbtt_GetFontOffsetForIndex(lx_font_data, 0)) ? 1 : -1;
    return s_ready > 0;
}

/* The display code page's text as code points; returns how many. */
static int code_points(const char* text, int len, int* out, int cap) {
    char utf8[TEXT_MAX * 3 + 4];
    const unsigned char* p;
    int n = 0, cp = jpt_l10n_display_codepage() ? jpt_l10n_display_codepage() : 1252;
    size_t bytes;
    if (len <= 0 || len > TEXT_MAX) return 0;
    bytes = jpt_cp_to_utf8(cp, text, (size_t)len, utf8, sizeof utf8);
    if (bytes == (size_t)-1) return 0;
    for (p = (const unsigned char*)utf8; p < (const unsigned char*)utf8 + bytes && n < cap;) {
        int c = *p, extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (int i = 0; i < extra && p[i + 1]; i++) c = c << 6 | (p[i + 1] & 0x3F);
        p += extra + 1;
        out[n++] = c;
    }
    return n;
}

static int advance_of(float scale, int cp, int spacing) {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
    return (int)lroundf((float)adv * scale) + spacing;
}

static int heavy(int weight) { return weight >= 600; }

int fe_stb_measure(int px, int weight, int spacing, const char* text, int len, int* advance, int* line_h) {
    int cps[TEXT_MAX], n, asc = 0, desc = 0, gap = 0, total = 0;
    float scale;
    if (!font_ready() || px < 1 || (n = code_points(text, len, cps, TEXT_MAX)) <= 0) return 0;
    scale = stbtt_ScaleForMappingEmToPixels(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    for (int i = 0; i < n; i++) total += advance_of(scale, cps[i], spacing);
    *advance = total + (heavy(weight) ? 1 : 0);
    *line_h = (int)ceilf((float)asc * scale) + (int)ceilf((float)-desc * scale);
    return 1;
}

uint8_t* fe_stb_render(int px, int weight, int spacing, const char* text, int len, int pad, int* w, int* h) {
    int cps[TEXT_MAX], n, advance, line_h, asc = 0, desc = 0, gap = 0, pen = pad, baseline;
    float scale;
    uint8_t* cell;
    if (!fe_stb_measure(px, weight, spacing, text, len, &advance, &line_h)) return NULL;
    n = code_points(text, len, cps, TEXT_MAX);
    scale = stbtt_ScaleForMappingEmToPixels(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    *w = advance + 2 * pad;
    *h = line_h + 2 * pad;
    if (*w > 8192 || *h > 2048) return NULL;
    cell = (uint8_t*)calloc((size_t)*w * (size_t)*h, 1);
    if (!cell) return NULL;
    baseline = pad + (int)ceilf((float)asc * scale);
    for (int i = 0; i < n; i++) {
        int x0, y0, x1, y1, gw, gh;
        stbtt_GetCodepointBitmapBox(&s_font, cps[i], scale, scale, &x0, &y0, &x1, &y1);
        gw = x1 - x0, gh = y1 - y0;
        if (cps[i] != ' ' && gw > 0 && gh > 0 && gw < 4096 && gh < 2048) {
            uint8_t* bitmap = (uint8_t*)calloc((size_t)gw * (size_t)gh, 1);
            if (bitmap) {
                stbtt_MakeCodepointBitmap(&s_font, bitmap, gw, gh, gw, scale, scale, cps[i]);
                for (int pass = 0; pass < (heavy(weight) ? 2 : 1); pass++)
                    for (int row = 0; row < gh; row++)
                        for (int col = 0; col < gw; col++) {
                            const int x = pen + x0 + col + pass, y = baseline + y0 + row;
                            uint8_t* dst;
                            if (x < 0 || y < 0 || x >= *w || y >= *h) continue;
                            dst = cell + (size_t)y * (size_t)*w + (size_t)x;
                            if (bitmap[row * gw + col] > *dst) *dst = bitmap[row * gw + col];
                        }
                free(bitmap);
            }
        }
        pen += advance_of(scale, cps[i], spacing);
    }
    return cell;
}
