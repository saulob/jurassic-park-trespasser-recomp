/*
 * Button and key glyphs inside the game's text overlay. Subtitles are laid out again by the game's own routine with a
 * placeholder run as wide as each glyph; the controls card and its hint are panels laid out here.
 */
#include "host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gamepad.h"
#include "jpt_prompt_text.h"
#include "pad_glyphs.h"
#include "tutorial_prompts.h"

/* (text, out, flags), thiscall on the font: word-wraps into {size, lines, {x, y, count, chars, 0}...}. */
#define LAYOUT_VA 0x0050E8C0u
/* CTextOverlay at [0x66F03C]: add(text, seconds, flags, colour, after, category) and find(category). */
#define OVERLAY_PTR 0x0066F03Cu
#define OVERLAY_ADD_VA 0x00468710u
#define OVERLAY_FIND_VA 0x00468980u
#define GAME_SECONDS 0x0073F350u
#define RASTER_PTR 0x0073DC40u
#define RASTER_WIDTH 0x08u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_LEFT 0x88u
#define RASTER_TOP 0x8Cu
#define KEYMAP_VA 0x0066AE78u
#define FONT_HFONT 0x00u
#define FONT_TALLEST 0x0Cu
/* CTextOverlay: its entries, a ring from the sentinel at +0x54; an entry's laid-out text, flags and category. */
#define OVERLAY_LIST 0x54u
#define NODE_BLOB 0x0Cu
#define NODE_FLAGS 0x1Cu
#define NODE_CATEGORY 0x24u
#define NODE_SINGLE_LINE 0x100u
#define NODE_WAITING 0x400u
#define NODE_LIMIT 64
#define FONT_WIDTH(c) (0x10u + 4u * (uint32_t)(c))

#define LAYOUT_CENTRE 0x01u
#define LAYOUT_LEFT 0x02u
#define LAYOUT_RIGHT 0x04u
#define LAYOUT_BOTTOM 0x10u
#define LAYOUT_MARGIN 8

#define MAX_LINES 32
#define MAX_TEXT 1024
#define EXPANDED_CAP 2048
#define SCRATCH_BYTES 0x2000
#define CACHE_ENTRIES 8
#define GLYPH_CACHE 32
#define KEY_COLOR RGB(255, 0, 255)
#define KEY_PIXEL 0x00FF00FFu
#define LOG_LIMIT 24

#define CARD_CATEGORY 0x4A50u
#define HINT_CATEGORY 0x4A51u
#define CARD_SECONDS 14.0f
#define HINT_SECONDS 7.0f
#define WHITE_TEXT 0x00FFFFFFu

#define PANEL_PIECES 160
#define PANEL_LOG_LIMIT 12

typedef struct {
    uint32_t text;
    int x, y, count;
} line_t;

typedef struct {
    int gamepad, family;
    int bind[JPT_ACT_COUNT];
    jpt_key_binding_t keymap[JPT_KEYMAP_COUNT];
    uint32_t font, tallest, raster_w, raster_h, left, top, flags;
    int panel, band;
} signature_t;

typedef struct {
    int line, dx, width;
    int token; /* glyph index, -1 for text */
    const char* text;
    int len;
} piece_t;

typedef struct {
    int first, count;
} group_t;

typedef struct {
    uint32_t blob, hash, used;
    signature_t sig;
    int valid, active;
    uint8_t* scratch;
    char* text;
    int line_count;
    uint32_t line_text[MAX_LINES];
    int line_token[MAX_LINES];
    int token_count, glyph_h;
    uint32_t unit;
    pad_glyph_t glyphs[JPT_PROMPT_MAX_TOKENS];
    char plain[MAX_TEXT];
    int panel, text_h, shadow, glyph_margin, piece_count, arena_used;
    int line_w[MAX_LINES];
    piece_t pieces[PANEL_PIECES];
} entry_t;

typedef struct {
    pad_glyph_t g;
    int h, w;
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
    uint32_t used;
} glyph_bitmap_t;

static struct {
    entry_t cache[CACHE_ENTRIES];
    glyph_bitmap_t glyphs[GLYPH_CACHE];
    uint32_t clock;
    const entry_t* drawing;
    const char* last_text;
    int logged, last_logged_gamepad, last_logged_family;
    char card[16], hint[32];
    struct {
        HFONT font;
        int h;
        uint32_t game_font;
    } fonts[2];
    int next_font, band_hold, panel_logs;
    HDC measure;
} P = { .last_logged_gamepad = -1, .last_logged_family = -1 };

static void call_guest(uint32_t va, uint32_t self, const uint32_t* args, int count) {
    recomp_func_t f = recomp_lookup(va);
    uint32_t ecx = g_ecx;
    g_eax = 0;
    if (!f) return;
    for (int i = count - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    g_ecx = ecx;
}

static int blob_lines(uint32_t blob, line_t* lines, int max) {
    uint32_t size, count, p, end;
    if (!blob) return -1;
    size = MEM32(blob);
    count = MEM32(blob + 4);
    if (size < 8 || size > SCRATCH_BYTES || count > (uint32_t)max) return -1;
    end = blob + size;
    p = blob + 8;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 12 > end) return -1;
        lines[i].x = (int)MEM32(p);
        lines[i].y = (int)MEM32(p + 4);
        lines[i].count = (int)MEM32(p + 8);
        lines[i].text = p + 12;
        if (lines[i].count < 0 || p + 12 + (uint32_t)lines[i].count + 1 > end) return -1;
        p += 12 + (uint32_t)lines[i].count + 1;
    }
    return (int)count;
}

static uint32_t fnv(uint32_t blob) {
    uint32_t size = MEM32(blob), h = 2166136261u;
    for (uint32_t i = 0; i < size; i++) h = (h ^ MEM8(blob + i)) * 16777619u;
    return h;
}

static int line_width(uint32_t font, const line_t* l) {
    int w = 0;
    for (int i = 0; i < l->count; i++) {
        int8_t c = (int8_t)MEM8(l->text + (uint32_t)i);
        w += (int)MEM32(font + FONT_WIDTH(c));
    }
    return w;
}

/* The flags the text was laid out with, from where its first line landed. */
static uint32_t layout_flags(uint32_t font, const line_t* first) {
    uint32_t raster = MEM32(RASTER_PTR), flags;
    int width = (int)MEM32(raster + RASTER_WIDTH), left = (int)MEM32(raster + RASTER_LEFT);
    int top = (int)MEM32(raster + RASTER_TOP), w = line_width(font, first);
    if (first->x == left + LAYOUT_MARGIN) flags = LAYOUT_LEFT;
    else if (first->x == left + width - w - LAYOUT_MARGIN) flags = LAYOUT_RIGHT;
    else flags = LAYOUT_CENTRE;
    if (first->y != top + LAYOUT_MARGIN) flags |= LAYOUT_BOTTOM;
    return flags;
}

/* Below the game's top-anchored entries; it only grows while a panel is shown, so the card never jumps up. */
static int panel_band(uint32_t tallest, int top) {
    uint32_t overlay = MEM32(OVERLAY_PTR), head, node;
    int band = jpt_card_band_floor((int)tallest), n = 0;
    head = overlay ? MEM32(overlay + OVERLAY_LIST) : 0;
    for (node = head ? MEM32(head) : 0; node && node != head && n < NODE_LIMIT; node = MEM32(node), n++) {
        uint32_t category = MEM32(node + NODE_CATEGORY), flags = MEM32(node + NODE_FLAGS);
        line_t lines[MAX_LINES];
        int count, bottom;
        if (category == CARD_CATEGORY || category == HINT_CATEGORY || (flags & (NODE_SINGLE_LINE | NODE_WAITING)))
            continue;
        count = blob_lines(MEM32(node + NODE_BLOB), lines, MAX_LINES);
        if (count <= 0 || lines[0].y > top + LAYOUT_MARGIN) continue;
        bottom = lines[count - 1].y - top + (int)tallest + (int)tallest / 4;
        if (bottom > band) band = bottom;
    }
    if (P.band_hold > band) band = P.band_hold;
    P.band_hold = band;
    return band;
}

static void current_signature(uint32_t font, const line_t* first, int panel, signature_t* s, gamepad_prompts_t* g) {
    uint32_t raster = MEM32(RASTER_PTR);
    memset(s, 0, sizeof *s);
    gamepad_prompts(g);
    s->gamepad = g->gamepad;
    s->family = g->family;
    for (int a = 0; a < JPT_ACT_COUNT && g->settings; a++) s->bind[a] = g->settings->bind[a];
    for (int i = 0; i < JPT_KEYMAP_COUNT; i++) {
        s->keymap[i].vk = MEM32(KEYMAP_VA + 8u * (uint32_t)i);
        s->keymap[i].command = MEM32(KEYMAP_VA + 8u * (uint32_t)i + 4);
    }
    s->font = font;
    s->tallest = MEM32(font + FONT_TALLEST);
    s->raster_w = MEM32(raster + RASTER_WIDTH);
    s->raster_h = MEM32(raster + RASTER_HEIGHT);
    s->left = MEM32(raster + RASTER_LEFT);
    s->top = MEM32(raster + RASTER_TOP);
    s->flags = layout_flags(font, first);
    s->panel = panel;
    s->band = panel ? panel_band(s->tallest, (int)s->top) : 0;
}

static int glyph_width(const pad_glyph_t* g, int h) { return (int)ceilf(g->width * (float)h); }

static int count_runs(const char* t, int n) {
    int runs = 0;
    for (int i = 0; i < n; i++)
        if (t[i] == JPT_PROMPT_MARK && (i == 0 || t[i - 1] != JPT_PROMPT_MARK)) runs++;
    return runs;
}

/* The text rewritten for the device, each token widened to a run of marks as wide as its glyph. */
static int build(entry_t* e, uint32_t font, const char* original, const gamepad_prompts_t* g) {
    jpt_prompt_context_t ctx;
    jpt_prompt_token_t tokens[JPT_PROMPT_MAX_TOKENS];
    char marked[EXPANDED_CAP];
    int count, used = 0, margin, next = 0, runs = 0;
    line_t lines[MAX_LINES];
    uint32_t args[3];
    memset(&ctx, 0, sizeof ctx);
    ctx.gamepad = e->sig.gamepad;
    ctx.family = (jpt_pad_family_t)e->sig.family;
    ctx.settings = g->settings;
    ctx.keymap = e->sig.keymap;
    ctx.keymap_count = JPT_KEYMAP_COUNT;
    count = jpt_prompt_text_render(original, &ctx, marked, sizeof marked, tokens, JPT_PROMPT_MAX_TOKENS);
    if (count <= 0) return 0;
    jpt_prompt_text_plain(original, &ctx, e->plain, sizeof e->plain);
    e->glyph_h = (int)((float)e->sig.tallest * 0.92f + 0.5f);
    if (e->glyph_h < 6) return 0;
    e->unit = e->sig.tallest / 10u ? e->sig.tallest / 10u : 1u;
    margin = e->glyph_h / 6;
    if (!e->scratch) e->scratch = (uint8_t*)malloc(SCRATCH_BYTES);
    if (!e->text) e->text = (char*)malloc(EXPANDED_CAP);
    if (!e->scratch || !e->text) return 0;
    for (const char* p = marked; *p; p++) {
        int n = 1;
        if (*p == JPT_PROMPT_MARK) {
            pad_glyph_t* glyph = &e->glyphs[next];
            if (!pad_glyph_token(&tokens[next], (jpt_pad_family_t)e->sig.family, glyph))
                pad_glyph_key(0, tokens[next].label, glyph);
            n = (glyph_width(glyph, e->glyph_h) + 2 * margin + (int)e->unit - 1) / (int)e->unit;
            next++;
        }
        if (used + n + 1 > EXPANDED_CAP) return 0;
        memset(e->text + used, *p, (size_t)n);
        used += n;
    }
    e->text[used] = 0;
    e->token_count = next;
    MEM32(font + FONT_WIDTH(JPT_PROMPT_MARK)) = e->unit;
    memset(e->scratch, 0, SCRATCH_BYTES);
    args[0] = (uint32_t)(uintptr_t)e->text;
    args[1] = (uint32_t)(uintptr_t)e->scratch;
    args[2] = e->sig.flags;
    call_guest(LAYOUT_VA, font, args, 3);
    e->line_count = blob_lines((uint32_t)(uintptr_t)e->scratch, lines, MAX_LINES);
    if (e->line_count <= 0) return 0;
    for (int i = 0; i < e->line_count; i++) {
        e->line_text[i] = lines[i].text;
        e->line_token[i] = runs;
        runs += count_runs((const char*)(uintptr_t)lines[i].text, lines[i].count);
    }
    return runs == e->token_count;
}

/* The panel font: the game's own face, aliased (the overlay's draw stays a pure write), at the panel's cell height. */
static HFONT panel_font(uint32_t game_font, int h) {
    HFONT base = (HFONT)(uintptr_t)MEM32(game_font + FONT_HFONT), made;
    LOGFONTA lf;
    int slot;
    for (int i = 0; i < 2; i++)
        if (P.fonts[i].font && P.fonts[i].h == h && P.fonts[i].game_font == game_font) return P.fonts[i].font;
    if (!base || GetObjectA(base, sizeof lf, &lf) != sizeof lf) {
        memset(&lf, 0, sizeof lf);
        lf.lfWeight = FW_BOLD;
        lf.lfCharSet = ANSI_CHARSET;
        strcpy(lf.lfFaceName, "Arial");
    }
    lf.lfHeight = h;
    lf.lfWidth = 0;
    lf.lfEscapement = lf.lfOrientation = 0;
    lf.lfQuality = NONANTIALIASED_QUALITY;
    made = CreateFontIndirectA(&lf);
    if (!made) return NULL;
    slot = P.next_font;
    P.next_font ^= 1;
    if (P.fonts[slot].font) DeleteObject(P.fonts[slot].font);
    P.fonts[slot].font = made;
    P.fonts[slot].h = h;
    P.fonts[slot].game_font = game_font;
    return made;
}

static piece_t* add_piece(entry_t* e) {
    piece_t* p;
    if (e->piece_count >= PANEL_PIECES) return NULL;
    p = &e->pieces[e->piece_count++];
    memset(p, 0, sizeof *p);
    p->token = -1;
    return p;
}

static int panel_template(entry_t* e, const char* source, const jpt_prompt_context_t* ctx, HDC dc) {
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
        piece_t* p = add_piece(e);
        if (!p) return -1;
        p->dx = x;
        if (*s == JPT_PROMPT_MARK) {
            pad_glyph_t* glyph;
            if (next >= count || e->token_count >= JPT_PROMPT_MAX_TOKENS) return -1;
            glyph = &e->glyphs[e->token_count];
            if (!pad_glyph_token(&tokens[next], ctx->family, glyph)) pad_glyph_key(0, tokens[next].label, glyph);
            p->token = e->token_count++;
            p->width = glyph_width(glyph, e->glyph_h) + 2 * e->glyph_margin;
            next++;
            s++;
        } else {
            const char* end = s;
            SIZE size;
            while (*end && *end != JPT_PROMPT_MARK) end++;
            p->len = (int)(end - s);
            if (e->arena_used + p->len + 1 > EXPANDED_CAP) return -1;
            p->text = e->text + e->arena_used;
            memcpy(e->text + e->arena_used, s, (size_t)p->len);
            e->arena_used += p->len;
            e->text[e->arena_used++] = 0;
            p->width = GetTextExtentPoint32A(dc, p->text, p->len, &size) ? (int)size.cx : 0;
            s = end;
        }
        x += p->width;
    }
    return x;
}

static int panel_pass(entry_t* e, const jpt_card_t* card, const jpt_prompt_context_t* ctx, int h, jpt_card_metrics_t* m,
                      group_t* title, group_t* binding, group_t* action) {
    HFONT font = panel_font(e->sig.font, h);
    HGDIOBJ old;
    int ok = 1;
    if (!P.measure) P.measure = CreateCompatibleDC(NULL);
    if (!P.measure || !font) return 0;
    old = SelectObject(P.measure, font);
    e->piece_count = e->token_count = e->arena_used = 0;
    e->text_h = h;
    e->glyph_h = (h * 92 + 50) / 100;
    e->glyph_margin = e->glyph_h / 8 > 1 ? e->glyph_h / 8 : 1;
    e->shadow = h / 18 > 1 ? h / 18 : 1;
    memset(m, 0, sizeof *m);
    m->count = card->count;
    m->first_column = card->first_column;
    title->first = e->piece_count;
    m->title_w = panel_template(e, card->title, ctx, P.measure);
    title->count = e->piece_count - title->first;
    ok &= m->title_w >= 0;
    for (int i = 0; i < card->count && ok; i++) {
        binding[i].first = e->piece_count;
        m->binding_w[i] = panel_template(e, card->rows[i].binding, ctx, P.measure);
        binding[i].count = e->piece_count - binding[i].first;
        action[i].first = e->piece_count;
        m->action_w[i] = panel_template(e, card->rows[i].action, ctx, P.measure);
        action[i].count = e->piece_count - action[i].first;
        ok &= m->binding_w[i] >= 0 && m->action_w[i] >= 0;
    }
    SelectObject(P.measure, old);
    return ok;
}

static void place_group(entry_t* e, const group_t* g, int line, int shift) {
    for (int i = g->first; i < g->first + g->count; i++) {
        int right;
        e->pieces[i].line = line;
        e->pieces[i].dx += shift;
        right = e->pieces[i].dx + e->pieces[i].width;
        if (right > e->line_w[line]) e->line_w[line] = right;
    }
}

/* A one-character line the TextOutA hook recognises by its address. */
static int blob_line(entry_t* e, uint32_t* used, int x, int y) {
    uint32_t at = (uint32_t)(uintptr_t)e->scratch + *used;
    if (e->line_count >= MAX_LINES || *used + 14 > SCRATCH_BYTES) return -1;
    MEM32(at) = (uint32_t)x;
    MEM32(at + 4) = (uint32_t)y;
    MEM32(at + 8) = 1;
    MEM8(at + 12) = '#';
    MEM8(at + 13) = 0;
    e->line_text[e->line_count] = at + 12;
    e->line_w[e->line_count] = 0;
    *used += 14;
    return e->line_count++;
}

static void log_panel(const entry_t* e, const jpt_card_t* card, const jpt_card_layout_t* lay, int fits) {
    if (P.panel_logs >= PANEL_LOG_LIMIT) return;
    if (++P.panel_logs == PANEL_LOG_LIMIT) {
        host_log("prompts", "panel line limit reached");
        return;
    }
    if (!e->line_count) {
        host_log("prompts", "controls hint: nothing to show with the keyboard and mouse");
        return;
    }
    host_log("prompts", "%s laid out for %s: %d rows in %d column%s, text %d px (the game's %u px) on a %ux%u screen, "
             "%dx%d at %d,%d below a top band of %d px%s",
             e->panel == JPT_PANEL_CARD ? "controls card" : "controls hint",
             e->sig.gamepad ? "the gamepad" : "the keyboard", card->count, lay->columns, lay->columns == 1 ? "" : "s",
             e->text_h, e->sig.tallest, e->sig.raster_w, e->sig.raster_h, lay->box.right - lay->box.left,
             lay->box.bottom - lay->box.top, lay->box.left, lay->box.top, e->sig.band,
             fits ? "" : " (did not fit at the smallest size)");
}

typedef struct {
    entry_t* e;
    const jpt_card_t* card;
    const jpt_prompt_context_t* ctx;
    group_t *title, *binding, *action;
} panel_measure_t;

static int measure_panel(void* context, int h, jpt_card_metrics_t* m) {
    panel_measure_t* pm = (panel_measure_t*)context;
    return panel_pass(pm->e, pm->card, pm->ctx, h, m, pm->title, pm->binding, pm->action);
}

static int build_panel(entry_t* e, const gamepad_prompts_t* g) {
    const jpt_card_t* card =
        e->panel == JPT_PANEL_CARD ? jpt_prompt_card(e->sig.gamepad) : jpt_prompt_hint_card(e->sig.gamepad);
    jpt_prompt_context_t ctx;
    jpt_card_metrics_t m;
    jpt_card_layout_t lay;
    jpt_box_t area;
    group_t title, binding[JPT_CARD_MAX_ROWS], action[JPT_CARD_MAX_ROWS];
    panel_measure_t pm = { e, card, NULL, &title, binding, action };
    int rw = (int)e->sig.raster_w, rh = (int)e->sig.raster_h, left = (int)e->sig.left, top = (int)e->sig.top;
    int max_h, fits, line;
    uint32_t used = 8;
    if (card->count > JPT_CARD_MAX_ROWS || rw <= 0 || rh <= 0) return 0;
    if (!e->scratch) e->scratch = (uint8_t*)malloc(SCRATCH_BYTES);
    if (!e->text) e->text = (char*)malloc(EXPANDED_CAP);
    if (!e->scratch || !e->text) return 0;
    memset(&ctx, 0, sizeof ctx);
    memset(&lay, 0, sizeof lay);
    ctx.gamepad = e->sig.gamepad;
    ctx.family = (jpt_pad_family_t)e->sig.family;
    ctx.settings = g->settings;
    ctx.keymap = e->sig.keymap;
    ctx.keymap_count = JPT_KEYMAP_COUNT;
    jpt_card_area(left, top, rw, rh, (int)e->sig.tallest, e->sig.band, &area, &max_h);
    pm.ctx = &ctx;
    fits = jpt_card_fit(measure_panel, &pm, max_h, JPT_CARD_FLOOR_TEXT, area, &m, &lay);
    if (fits < 0) return 0;
    e->line_count = 0;
    if (title.count) {
        if ((line = blob_line(e, &used, lay.title_x, lay.title_y)) < 0) return 0;
        place_group(e, &title, line, 0);
    }
    for (int i = 0; i < card->count; i++) {
        if ((line = blob_line(e, &used, lay.binding_x[i], lay.row_y[i])) < 0) return 0;
        place_group(e, &binding[i], line, 0);
        place_group(e, &action[i], line, lay.action_x[i] - lay.binding_x[i]);
    }
    MEM32((uint32_t)(uintptr_t)e->scratch) = used;
    MEM32((uint32_t)(uintptr_t)e->scratch + 4) = (uint32_t)e->line_count;
    log_panel(e, card, &lay, fits);
    return 1;
}

static int original_text(const line_t* lines, int n, char* out) {
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (used + lines[i].count + 1 > MAX_TEXT) return 0;
        memcpy(out + used, (const void*)(uintptr_t)lines[i].text, (size_t)lines[i].count);
        used += lines[i].count;
    }
    out[used] = 0;
    return 1;
}

static entry_t* entry_for(uint32_t font, uint32_t blob) {
    line_t lines[MAX_LINES];
    char original[MAX_TEXT];
    int n = blob_lines(blob, lines, MAX_LINES), panel, have_original = 0;
    uint32_t hash;
    signature_t sig;
    gamepad_prompts_t g;
    entry_t* e = NULL;
    if (n <= 0) return NULL;
    hash = fnv(blob);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        entry_t* c = &P.cache[i];
        if (c->valid && c->blob == blob && c->hash == hash) {
            e = c;
            break;
        }
    }
    if (e) {
        panel = e->panel;
    } else {
        if (!original_text(lines, n, original)) return NULL;
        have_original = 1;
        panel = jpt_prompt_panel_kind(original);
    }
    current_signature(font, &lines[0], panel, &sig, &g);
    if (e && !memcmp(&e->sig, &sig, sizeof sig)) {
        e->used = ++P.clock;
        return e->active ? e : NULL;
    }
    if (!have_original && !original_text(lines, n, original)) return NULL;
    if (!e) {
        e = &P.cache[0];
        for (int i = 1; i < CACHE_ENTRIES; i++)
            if (!P.cache[i].valid || (e->valid && P.cache[i].used < e->used)) e = &P.cache[i];
    }
    e->valid = 1;
    e->blob = blob;
    e->hash = hash;
    e->sig = sig;
    e->used = ++P.clock;
    e->panel = panel;
    if (panel) e->active = build_panel(e, &g);
    else e->active = jpt_prompt_text_has_prompts(original) && build(e, font, original, &g);
    return e->active ? e : NULL;
}

int tutorial_prompts_draw(recomp_func_t lifted) {
    uint32_t font = g_ecx, slot = g_esp + 4;
    const entry_t* e = entry_for(font, MEM32(slot));
    if (!e) return 0;
    MEM32(slot) = (uint32_t)(uintptr_t)e->scratch;
    P.drawing = e;
    P.last_text = NULL;
    lifted();
    P.drawing = NULL;
    return 1;
}

static void release_glyph(glyph_bitmap_t* b) {
    if (b->dc) {
        SelectObject(b->dc, b->old);
        DeleteDC(b->dc);
    }
    if (b->bmp) DeleteObject(b->bmp);
    memset(b, 0, sizeof *b);
}

static void glyph_label(HDC dc, const pad_glyph_t* g, int h) {
    HFONT font, old;
    RECT r;
    int cx = (int)(g->text_x * (float)h), cy = (int)(g->text_y * (float)h);
    if (!g->text[0] || g->text_size <= 0.0f) return;
    font = CreateFontA(-(int)(g->text_size * (float)h + 0.5f), 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    if (!font) return;
    old = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB((g->text_color >> 16) & 0xFF, (g->text_color >> 8) & 0xFF, g->text_color & 0xFF));
    r.left = cx - h * 4;
    r.right = cx + h * 4;
    r.top = cy - h;
    r.bottom = cy + h;
    DrawTextA(dc, g->text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dc, old);
    DeleteObject(font);
}

/* Opaque pixels only: under the overlay's DC whatever is written replaces the frame, so there is no blending. */
static const glyph_bitmap_t* glyph_bitmap(const pad_glyph_t* g, int h) {
    glyph_bitmap_t* b = &P.glyphs[0];
    BITMAPINFO bi;
    uint32_t* px = NULL;
    float aa = 1.0f / (float)h;
    for (int i = 0; i < GLYPH_CACHE; i++) {
        glyph_bitmap_t* c = &P.glyphs[i];
        if (c->dc && c->h == h && !memcmp(&c->g, g, sizeof *g)) {
            c->used = ++P.clock;
            return c;
        }
        if (!c->dc || (b->dc && c->used < b->used)) b = c;
    }
    release_glyph(b);
    b->g = *g;
    b->h = h;
    b->w = glyph_width(g, h);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = b->w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    b->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
    b->dc = b->bmp ? CreateCompatibleDC(NULL) : NULL;
    if (!b->dc || !px) {
        release_glyph(b);
        return NULL;
    }
    b->old = SelectObject(b->dc, b->bmp);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < b->w; x++) {
            float u = ((float)x + 0.5f) * aa, v = ((float)y + 0.5f) * aa;
            uint32_t color = KEY_PIXEL;
            for (int layer = 0; layer < PAD_LAYER_COUNT; layer++)
                if (pad_glyph_coverage(g, layer, u, v, aa) >= 0.5f) color = g->color[layer];
            px[y * b->w + x] = color;
        }
    GdiFlush();
    glyph_label(b->dc, g, h);
    GdiFlush();
    b->used = ++P.clock;
    return b;
}

static void log_drawn(const entry_t* e) {
    if (e->sig.gamepad == P.last_logged_gamepad && e->sig.family == P.last_logged_family) return;
    P.last_logged_gamepad = e->sig.gamepad;
    P.last_logged_family = e->sig.family;
    if (P.logged >= LOG_LIMIT) return;
    if (++P.logged == LOG_LIMIT) {
        host_log("prompts", "prompt line limit reached");
        return;
    }
    if (e->sig.gamepad)
        host_log("prompts", "prompt renderer: %s glyph set drawn in the game's text: \"%s\"",
                 jpt_pad_family_name((jpt_pad_family_t)e->sig.family), e->plain);
    else
        host_log("prompts", "prompt renderer: keyboard and mouse keycaps drawn in the game's text: \"%s\"", e->plain);
}

/* Drawn on the colour pass only (the game's first pass is its 1-pixel shadow). */
static int panel_textout(const entry_t* e, HDC dc, int x, int y, int line, int main_pass) {
    HFONT font;
    HGDIOBJ old = NULL;
    COLORREF colour;
    POINT corner[2];
    RECT box;
    int h = e->text_h, shadow = e->shadow;
    if (!main_pass) return 1;
    corner[0].x = x - 2;
    corner[0].y = y - 2;
    corner[1].x = x + e->line_w[line] + shadow + 2;
    corner[1].y = y + h + shadow + 2;
    if (LPtoDP(dc, corner, 2)) {
        box.left = min(corner[0].x, corner[1].x);
        box.right = max(corner[0].x, corner[1].x);
        box.top = min(corner[0].y, corner[1].y);
        box.bottom = max(corner[0].y, corner[1].y);
        d3dr_text_box(dc, &box);
    } else {
        d3dr_text_box(dc, NULL);
    }
    font = panel_font(e->sig.font, h);
    if (font) old = SelectObject(dc, font);
    colour = GetTextColor(dc);
    for (int i = 0; i < e->piece_count; i++) {
        const piece_t* p = &e->pieces[i];
        if (p->line != line) continue;
        if (p->token < 0) {
            SetTextColor(dc, RGB(0, 0, 0));
            TextOutA(dc, x + p->dx + shadow, y + shadow, p->text, p->len);
            TextOutA(dc, x + p->dx + shadow, y, p->text, p->len);
            TextOutA(dc, x + p->dx, y + shadow, p->text, p->len);
            SetTextColor(dc, colour);
            TextOutA(dc, x + p->dx, y, p->text, p->len);
        } else {
            const glyph_bitmap_t* b = glyph_bitmap(&e->glyphs[p->token], e->glyph_h);
            if (b)
                TransparentBlt(dc, x + p->dx + e->glyph_margin, y + (h - b->h) / 2, b->w, b->h, b->dc, 0, 0, b->w, b->h,
                               KEY_COLOR);
        }
    }
    if (old) SelectObject(dc, old);
    return 1;
}

int tutorial_prompts_textout(HDC dc, int x, int y, const char* text, int count) {
    const entry_t* e = P.drawing;
    uint32_t font;
    int line = -1, main_pass, advance = 0, run = 0, total = 0;
    POINT corner[2];
    RECT box;
    if (!e) return 0;
    for (int i = 0; i < e->line_count; i++)
        if (e->line_text[i] == (uint32_t)(uintptr_t)text) line = i;
    if (line < 0) return 0;
    main_pass = text == P.last_text;
    P.last_text = text;
    if (e->panel) return panel_textout(e, dc, x, y, line, main_pass);
    font = e->sig.font;
    for (int i = 0; i < count; i++) total += (int)MEM32(font + FONT_WIDTH((int8_t)text[i]));
    corner[0].x = x - (int)e->sig.tallest;
    corner[0].y = y - 2;
    corner[1].x = x + total + (int)e->sig.tallest;
    corner[1].y = y + (int)e->sig.tallest + 2;
    if (LPtoDP(dc, corner, 2)) {
        box.left = min(corner[0].x, corner[1].x);
        box.right = max(corner[0].x, corner[1].x);
        box.top = min(corner[0].y, corner[1].y);
        box.bottom = max(corner[0].y, corner[1].y);
        d3dr_text_box(dc, &box);
    } else {
        d3dr_text_box(dc, NULL);
    }
    for (int i = 0; i < count;) {
        int j = i;
        if (text[i] != JPT_PROMPT_MARK) {
            int w = 0;
            while (j < count && text[j] != JPT_PROMPT_MARK) w += (int)MEM32(font + FONT_WIDTH((int8_t)text[j++]));
            TextOutA(dc, x + advance, y, text + i, j - i);
            advance += w;
        } else {
            int token = e->line_token[line] + run++, span;
            while (j < count && text[j] == JPT_PROMPT_MARK) j++;
            span = (j - i) * (int)e->unit;
            if (main_pass && token < e->token_count) {
                const glyph_bitmap_t* b = glyph_bitmap(&e->glyphs[token], e->glyph_h);
                if (b)
                    TransparentBlt(dc, x + advance + (span - b->w) / 2, y + ((int)e->sig.tallest - b->h) / 2, b->w,
                                   b->h, b->dc, 0, 0, b->w, b->h, KEY_COLOR);
            }
            advance += span;
        }
        i = j;
    }
    if (main_pass) log_drawn(e);
    return 1;
}

static int overlay_entry(uint32_t overlay, uint32_t category) {
    uint32_t args[1] = { category };
    call_guest(OVERLAY_FIND_VA, overlay, args, 1);
    return (int)g_eax;
}

static void overlay_add(uint32_t overlay, const char* text, float seconds, uint32_t flags, uint32_t category) {
    uint32_t args[6];
    memcpy(&args[1], &seconds, sizeof seconds);
    args[0] = (uint32_t)(uintptr_t)text;
    args[2] = flags;
    args[3] = WHITE_TEXT;
    args[4] = 0;
    args[5] = category;
    call_guest(OVERLAY_ADD_VA, overlay, args, 6);
}

void tutorial_prompts_toggle_card(void) {
    uint32_t overlay = MEM32(OVERLAY_PTR), shown, hint;
    if (!overlay) return;
    shown = (uint32_t)overlay_entry(overlay, CARD_CATEGORY);
    if (shown) {
        *(float*)ADDR(shown) = *(float*)ADDR(GAME_SECONDS);
        return;
    }
    hint = (uint32_t)overlay_entry(overlay, HINT_CATEGORY);
    if (hint) *(float*)ADDR(hint) = *(float*)ADDR(GAME_SECONDS);
    P.band_hold = 0;
    memcpy(P.card, "<CARD>", sizeof "<CARD>");
    overlay_add(overlay, P.card, CARD_SECONDS, LAYOUT_CENTRE, CARD_CATEGORY);
    host_log("prompts", "controls card shown");
}

int tutorial_prompts_hint(void) {
    uint32_t overlay = MEM32(OVERLAY_PTR);
    if (!overlay || overlay_entry(overlay, HINT_CATEGORY)) return 0;
    if (overlay_entry(overlay, CARD_CATEGORY)) return 1;
    P.band_hold = 0;
    strncpy(P.hint, jpt_prompt_controls_hint(), sizeof P.hint - 1);
    overlay_add(overlay, P.hint, HINT_SECONDS, LAYOUT_CENTRE, HINT_CATEGORY);
    return 1;
}
