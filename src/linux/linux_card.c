#include "linux_card.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gamepad.h"
#include "guest_abi.h"
#include "jpt_prompt_text.h"
#include "jpt_sync.h"
#include "jpt_tr.h"
#include "linux_text.h"
#include "pad_glyphs.h"

#define KEYMAP_VA GVA(0x0066AE78)
#define GAME_SECONDS GVA(0x0073F350)
#define CARD_SECONDS 14.0f
#define HINT_SECONDS 7.0f
#define PIECES 160
#define TEXT_CAP 2048
#define GLYPH_PERCENT 92
#define TALLEST_PER_480 18
#define DIM_ALPHA 140

typedef struct {
    int line, dx, width, token, text_at, len;
} piece_t;

typedef struct {
    int first, count;
} group_t;

typedef struct {
    int pic_w, pic_h, gamepad, family, kind, codepage;
    int bind[JPT_ACT_COUNT];
    jpt_key_binding_t keymap[JPT_KEYMAP_COUNT];
} signature_t;

static struct {
    float card_until, hint_until;
    int host_clock_ms;
    signature_t sig;
    int valid, kind;
    piece_t pieces[PIECES];
    int piece_count, token_count, text_used;
    char text[TEXT_CAP];
    pad_glyph_t glyphs[JPT_PROMPT_MAX_TOKENS];
    int text_h, glyph_h, glyph_margin, shadow;
    group_t title, binding[JPT_CARD_MAX_ROWS], action[JPT_CARD_MAX_ROWS];
    int row_count, line_w[JPT_CARD_MAX_ROWS + 1];
    jpt_card_layout_t layout;
    lx_card_stats_t stats;
} C;

typedef struct {
    const jpt_card_t* card;
    const jpt_prompt_context_t* ctx;
} measure_t;

static float seconds(void) {
    uint32_t bits;
    float value;
    if (guest_read32(GAME_SECONDS, &bits)) {
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    return (float)((double)jpt_time_ms() / 1000.0);
}

void lx_card_reset(void) { memset(&C, 0, sizeof C); }

void lx_card_show(void) {
    const float now = seconds();
    C.hint_until = now;
    C.card_until = now + CARD_SECONDS;
}

void lx_card_hide(void) { C.card_until = seconds(); }

int lx_card_shown(void) { return seconds() < C.card_until; }
int lx_card_hint_shown(void) { return seconds() < C.hint_until; }

int lx_card_hint(void) {
    const float now = seconds();
    if (now < C.hint_until) return 0;
    if (now < C.card_until) return 1;
    C.hint_until = now + HINT_SECONDS;
    return 1;
}

void lx_card_stats(lx_card_stats_t* out) { *out = C.stats; }

/* ---- layout ---- */

static int glyph_width(const pad_glyph_t* g, int h) { return (int)ceilf(g->width * (float)h); }

static piece_t* add_piece(void) {
    piece_t* p;
    if (C.piece_count >= PIECES) return NULL;
    p = &C.pieces[C.piece_count++];
    memset(p, 0, sizeof *p);
    p->token = -1;
    return p;
}

/* The text for the device, each token a glyph piece and each run of text a measured piece; the width of the whole, -1 on overflow. */
static int template(const char* source, const jpt_prompt_context_t* ctx, float scale) {
    char marked[256];
    jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
    int count, next = 0, x = 0;
    if (!source || !*source) return 0;
    count = jpt_prompt_text_render(source, ctx, marked, sizeof marked, tokens, JPT_PROMPT_MAX_TOKENS);
    if (count < 0) {
        if (strlen(source) >= sizeof marked || strchr(source, '<')) return -1;
        strcpy(marked, source);
        count = 0;
    }
    for (const char* s = marked; *s;) {
        piece_t* p = add_piece();
        if (!p) return -1;
        p->dx = x;
        if (*s == JPT_PROMPT_MARK) {
            pad_glyph_t* glyph;
            if (next >= count || C.token_count >= JPT_PROMPT_MAX_TOKENS) return -1;
            glyph = &C.glyphs[C.token_count];
            if (!pad_glyph_token(&tokens[next], ctx->family, glyph)) pad_glyph_key(0, tokens[next].label, glyph);
            p->token = C.token_count++;
            p->width = glyph_width(glyph, C.glyph_h) + 2 * C.glyph_margin;
            next++;
            s++;
        } else {
            const char* end = s;
            while (*end && *end != JPT_PROMPT_MARK) end++;
            p->len = (int)(end - s);
            if (C.text_used + p->len + 1 > TEXT_CAP) return -1;
            p->text_at = C.text_used;
            memcpy(C.text + C.text_used, s, (size_t)p->len);
            C.text_used += p->len;
            C.text[C.text_used++] = 0;
            p->width = lx_text_width(scale, C.text + p->text_at, p->len, 0);
            s = end;
        }
        x += p->width;
    }
    return x;
}

static int measure_pass(void* context, int h, jpt_card_metrics_t* m) {
    const measure_t* mt = (const measure_t*)context;
    const float scale = lx_text_scale(-h);
    int ok = 1;
    C.piece_count = C.token_count = C.text_used = 0;
    C.text_h = h;
    C.glyph_h = (h * GLYPH_PERCENT + 50) / 100;
    C.glyph_margin = C.glyph_h / 8 > 1 ? C.glyph_h / 8 : 1;
    C.shadow = h / 18 > 1 ? h / 18 : 1;
    memset(m, 0, sizeof *m);
    m->count = mt->card->count;
    m->first_column = mt->card->first_column;
    C.title.first = C.piece_count;
    m->title_w = template(jpt_tr_cp(mt->card->title), mt->ctx, scale);
    C.title.count = C.piece_count - C.title.first;
    ok &= m->title_w >= 0;
    for (int i = 0; i < mt->card->count && ok; i++) {
        C.binding[i].first = C.piece_count;
        m->binding_w[i] = template(mt->card->rows[i].binding, mt->ctx, scale);
        C.binding[i].count = C.piece_count - C.binding[i].first;
        C.action[i].first = C.piece_count;
        m->action_w[i] = template(jpt_tr_cp(mt->card->rows[i].action), mt->ctx, scale);
        C.action[i].count = C.piece_count - C.action[i].first;
        ok &= m->binding_w[i] >= 0 && m->action_w[i] >= 0;
    }
    return ok;
}

static void place_group(const group_t* g, int line, int shift) {
    for (int i = g->first; i < g->first + g->count; i++) {
        int right;
        C.pieces[i].line = line;
        C.pieces[i].dx += shift;
        right = C.pieces[i].dx + C.pieces[i].width;
        if (right > C.line_w[line]) C.line_w[line] = right;
    }
}

static void signature_of(signature_t* s, int pic_w, int pic_h, int kind, const gamepad_prompts_t* g) {
    memset(s, 0, sizeof *s);
    s->pic_w = pic_w;
    s->pic_h = pic_h;
    s->kind = kind;
    s->gamepad = g->gamepad;
    s->family = (int)g->family;
    s->codepage = lx_text_codepage();
    for (int a = 0; a < JPT_ACT_COUNT && g->settings; a++) s->bind[a] = g->settings->bind[a];
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++) {
        uint32_t vk = 0, command = 0;
        guest_read32(KEYMAP_VA + 8u * (uint32_t)i, &vk);
        guest_read32(KEYMAP_VA + 8u * (uint32_t)i + 4u, &command);
        s->keymap[i].vk = vk;
        s->keymap[i].command = command;
    }
}

static int build(int pic_w, int pic_h, int kind, const gamepad_prompts_t* g) {
    const jpt_card_t* card = kind == JPT_PANEL_CARD ? jpt_prompt_card(g->gamepad) : jpt_prompt_hint_card(g->gamepad);
    jpt_prompt_context_t ctx;
    jpt_card_metrics_t m;
    jpt_box_t area;
    measure_t mt;
    int max_h, fits, tallest = pic_h * TALLEST_PER_480 / 480 > 16 ? pic_h * TALLEST_PER_480 / 480 : 16;
    if (!card || card->count > JPT_CARD_MAX_ROWS || pic_w <= 0 || pic_h <= 0) return 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.gamepad = g->gamepad;
    ctx.family = g->family;
    ctx.settings = g->settings;
    ctx.keymap = C.sig.keymap;
    ctx.keymap_count = JPT_KEYMAP_COUNT;
    mt.card = card;
    mt.ctx = &ctx;
    jpt_card_area(0, 0, pic_w, pic_h, tallest, jpt_card_band_floor(tallest), &area, &max_h);
    memset(&C.layout, 0, sizeof C.layout);
    fits = jpt_card_fit(measure_pass, &mt, max_h, JPT_CARD_FLOOR_TEXT, area, &m, &C.layout);
    if (fits < 0) return 0;
    memset(C.line_w, 0, sizeof C.line_w);
    C.row_count = card->count;
    if (C.title.count) place_group(&C.title, card->count, 0);
    for (int i = 0; i < card->count; i++) {
        place_group(&C.binding[i], i, 0);
        place_group(&C.action[i], i, C.layout.action_x[i] - C.layout.binding_x[i]);
    }
    C.stats.text_h = C.text_h;
    C.stats.columns = C.layout.columns;
    C.stats.rows = card->count;
    C.stats.fits = fits;
    C.stats.box_left = C.layout.box.left;
    C.stats.box_top = C.layout.box.top;
    C.stats.box_right = C.layout.box.right;
    C.stats.box_bottom = C.layout.box.bottom;
    C.stats.glyphs = 0;
    for (int i = 0; i < C.piece_count; i++) C.stats.glyphs += C.pieces[i].token >= 0;
    return 1;
}

/* ---- drawing ---- */

typedef struct {
    uint32_t* pixels;
    int stride, width, height, ox, oy;
    uint32_t color;
} canvas_t;

static void blend(canvas_t* c, int x, int y, uint32_t rgb, int coverage) {
    uint32_t* p;
    uint32_t old, mix = 0;
    if (x < 0 || y < 0 || x >= c->width || y >= c->height || coverage <= 0) return;
    if (coverage > 255) coverage = 255;
    p = c->pixels + (size_t)y * (size_t)c->stride + (size_t)x;
    old = *p;
    for (int shift = 0; shift <= 16; shift += 8) {
        const uint32_t a = (old >> shift) & 0xFFu, b = (rgb >> shift) & 0xFFu;
        mix |= ((a * (uint32_t)(255 - coverage) + b * (uint32_t)coverage) / 255u) << shift;
    }
    *p = 0xFF000000u | mix;
}

static void text_pixel(void* context, int x, int y, int coverage) {
    canvas_t* c = (canvas_t*)context;
    blend(c, x + c->ox, y + c->oy, c->color, coverage);
}

static void draw_text(canvas_t* c, float scale, const char* text, int len, int x, int baseline, uint32_t rgb) {
    int pen = x;
    c->color = rgb;
    for (int i = 0; i < len; i++) {
        lx_text_glyph(scale, (uint8_t)text[i], 0, pen, baseline, text_pixel, c);
        pen += lx_text_advance(scale, (uint8_t)text[i], 0);
    }
}

static void draw_glyph(canvas_t* c, const pad_glyph_t* g, int x0, int y0, int h) {
    const int w = glyph_width(g, h);
    const float aa = 1.0f / (float)h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float u = ((float)x + 0.5f) * aa, v = ((float)y + 0.5f) * aa;
            for (int layer = 0; layer < PAD_LAYER_COUNT; layer++) {
                const float cov = pad_glyph_coverage(g, layer, u, v, aa);
                if (cov > 0.0f) blend(c, c->ox + x0 + x, c->oy + y0 + y, g->color[layer], (int)(cov * 255.0f + 0.5f));
            }
        }
    if (g->text[0] && g->text_size > 0.0f) {
        const float scale = lx_text_scale(-(int)(g->text_size * (float)h + 0.5f));
        lx_text_metrics_t m;
        const int len = (int)strlen(g->text), tw = lx_text_width(scale, g->text, len, 1);
        lx_text_metrics(scale, &m);
        draw_text(c, scale, g->text, len, x0 + (int)(g->text_x * (float)h) - tw / 2, y0 + (int)(g->text_y * (float)h) - m.height / 2 + m.ascent, g->text_color);
    }
}

static void dim_box(canvas_t* c, int left, int top, int right, int bottom) {
    for (int y = top; y < bottom; y++)
        for (int x = left; x < right; x++) blend(c, c->ox + x, c->oy + y, 0x000000u, DIM_ALPHA);
}

static void draw_line(canvas_t* c, float scale, const lx_text_metrics_t* m, int line, int x, int y) {
    const int h = C.text_h, shadow = C.shadow;
    if (C.line_w[line] <= 0) return;
    dim_box(c, x - 2, y - 2, x + C.line_w[line] + shadow + 2, y + h + shadow + 2);
    for (int i = 0; i < C.piece_count; i++) {
        const piece_t* p = &C.pieces[i];
        const char* text = C.text + p->text_at;
        if (p->line != line) continue;
        if (p->token < 0) {
            const int baseline = y + (h - m->height) / 2 + m->ascent;
            draw_text(c, scale, text, p->len, x + p->dx + shadow, baseline + shadow, 0x000000u);
            draw_text(c, scale, text, p->len, x + p->dx + shadow, baseline, 0x000000u);
            draw_text(c, scale, text, p->len, x + p->dx, baseline + shadow, 0x000000u);
            draw_text(c, scale, text, p->len, x + p->dx, baseline, 0xFFFFFFu);
        } else {
            draw_glyph(c, &C.glyphs[p->token], x + p->dx + C.glyph_margin, y + (h - C.glyph_h) / 2, C.glyph_h);
        }
    }
}

int lx_card_draw(uint32_t* pixels, int stride, int fw, int fh, int pic_x, int pic_y, int pic_w, int pic_h) {
    const int kind = lx_card_shown() ? JPT_PANEL_CARD : lx_card_hint_shown() ? JPT_PANEL_HINT : JPT_PANEL_NONE;
    gamepad_prompts_t g;
    signature_t now;
    canvas_t canvas;
    lx_text_metrics_t m;
    float scale;
    if (kind == JPT_PANEL_NONE || !pixels || pic_w <= 0 || pic_h <= 0) return 0;
    gamepad_prompts(&g);
    signature_of(&now, pic_w, pic_h, kind, &g);
    if (!C.valid || memcmp(&now, &C.sig, sizeof now)) {
        C.sig = now;
        C.valid = build(pic_w, pic_h, kind, &g);
    }
    if (!C.valid) return 0;
    canvas.pixels = pixels;
    canvas.stride = stride;
    canvas.width = fw;
    canvas.height = fh;
    canvas.ox = pic_x;
    canvas.oy = pic_y;
    scale = lx_text_scale(-C.text_h);
    lx_text_metrics(scale, &m);
    if (C.title.count) draw_line(&canvas, scale, &m, C.row_count, C.layout.title_x, C.layout.title_y);
    for (int i = 0; i < C.row_count; i++) draw_line(&canvas, scale, &m, i, C.layout.binding_x[i], C.layout.row_y[i]);
    return 1;
}
