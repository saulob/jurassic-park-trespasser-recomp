#include "loading_layout.h"

#include <math.h>
#include <string.h>

const ld_art_t ld_retail_table[LD_TABLE_ROWS] = {
    { "as.scn", "menu\\li_a1" },  { "as2.scn", "menu\\li_a2" }, { "be.scn", "menu\\li_be" },   { "ij.scn", "menu\\li_ij" },
    { "it.scn", "menu\\li_it" },  { "jr.scn", "menu\\li_jr" },  { "lab.scn", "menu\\li_lab" }, { "pv.scn", "menu\\li_pv" },
    { "sum.scn", "menu\\li_sum" }, { "", "menu\\li_other" },
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

static int same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
    return *a == *b;
}

int ld_select(const char* scene, const char* const* scenes, int count) {
    if (count <= 0) return -1;
    for (int i = 0; i < count; i++)
        if (scenes[i] && same_name(scene ? scene : "", scenes[i])) return i;
    return count - 1;
}

int ld_slot_name(const char* image, char* out, int cap) {
    const char* name = image;
    int n = 0;
    if (!image || !out || cap <= 0) return 0;
    for (const char* c = image; *c; c++)
        if (*c == '\\' || *c == '/') name = c + 1;
    for (; name[n]; n++) {
        char c = name[n];
        if (n + 1 >= cap || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            out[0] = 0;
            return 0;
        }
        out[n] = c;
    }
    out[n] = 0;
    return n > 0;
}

#define FRAME_BORDER 8
#define PLATE_W 380
#define PLATE_PAD 12
#define PLATE_BOTTOM 444
#define ROW_H 36
#define CAPTION_H 14
#define GROOVE_TOP 18
#define GROOVE_H 10
#define ROW_INSET 22
#define NOTE_H 18

static int clamp_count(int n, int most) { return n < 0 ? 0 : n > most ? most : n; }

void ld_layout_make(ld_layout_t* l, int screen_w, int screen_h, int gauge_count, int note_count) {
    int top, left = (FE_CANVAS_W - PLATE_W) / 2, height;
    memset(l, 0, sizeof *l);
    l->view = fe_view_make(screen_w, screen_h);
    l->photo = fe_rect(24, 18, 616, 462);
    l->frame = fe_rect_inflate(l->photo, FRAME_BORDER, FRAME_BORDER);
    l->message = fe_rect(160, 214, 480, 266);
    gauge_count = clamp_count(gauge_count, LD_MAX_GAUGES);
    note_count = clamp_count(note_count, LD_MAX_NOTES);
    l->gauge_count = gauge_count;
    l->note_count = note_count;
    if (!gauge_count && !note_count) return;
    height = 2 * PLATE_PAD + NOTE_H * note_count + (gauge_count ? ROW_H * gauge_count - (ROW_H - GROOVE_TOP - GROOVE_H) : 0);
    l->plate = fe_rect(left, PLATE_BOTTOM - height, left + PLATE_W, PLATE_BOTTOM);
    top = l->plate.top + PLATE_PAD;
    for (int i = 0; i < note_count; i++, top += NOTE_H)
        l->notes[i] = fe_rect(left + ROW_INSET, top, left + PLATE_W - ROW_INSET, top + CAPTION_H);
    for (int i = 0; i < gauge_count; i++, top += ROW_H) {
        l->captions[i] = fe_rect(left + ROW_INSET, top, left + PLATE_W - ROW_INSET, top + CAPTION_H);
        l->grooves[i] = fe_rect(left + ROW_INSET, top + GROOVE_TOP, left + PLATE_W - ROW_INSET, top + GROOVE_TOP + GROOVE_H);
    }
}

void ld_layout_scene(ld_layout_t* l, const ld_scene_t* s) {
    int loader = s->kind == LD_SCENE_LOADER;
    ld_layout_make(l, s->screen_w, s->screen_h, loader ? s->gauge_count : 0, loader ? s->note_count : 0);
}

fe_rect_t ld_gauge_pixels(const ld_layout_t* l, int gauge) {
    if (gauge < 0 || gauge >= l->gauge_count) return fe_rect(0, 0, 0, 0);
    return fe_rect_intersect(fe_rect_inflate(fe_view_rect(&l->view, l->grooves[gauge]), 3, 3),
                             fe_rect(0, 0, l->view.screen_w, l->view.screen_h));
}

int ld_fill_right(const ld_layout_t* l, int gauge, float fraction) {
    double x0, x1;
    if (gauge < 0 || gauge >= l->gauge_count) return 0;
    if (!(fraction > 0.0f)) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    x0 = fe_view_x(&l->view, l->grooves[gauge].left + 1.0);
    x1 = fe_view_x(&l->view, l->grooves[gauge].right - 1.0);
    return (int)floor(x0 + (x1 - x0) * fraction + 0.5);
}

int ld_scene_diff(const ld_scene_t* before, const ld_scene_t* now, const ld_layout_t* l, fe_rect_t* dirty) {
    *dirty = fe_rect(0, 0, 0, 0);
    if (!before || before->kind != now->kind || before->screen_w != now->screen_w || before->screen_h != now->screen_h ||
        before->art_key != now->art_key || before->gauge_count != now->gauge_count ||
        before->note_count != now->note_count || strcmp(before->message, now->message))
        return LD_DIFF_ALL;
    for (int i = 0; i < now->note_count; i++)
        if (before->note_colors[i] != now->note_colors[i] || strcmp(before->notes[i], now->notes[i])) return LD_DIFF_ALL;
    for (int i = 0; i < now->gauge_count; i++) {
        const ld_gauge_t *a = &before->gauges[i], *b = &now->gauges[i];
        if (a->color != b->color || a->text_color != b->text_color || strcmp(a->caption, b->caption)) return LD_DIFF_ALL;
        if (ld_fill_right(l, i, a->fraction) != ld_fill_right(l, i, b->fraction))
            *dirty = fe_rect_union(*dirty, ld_gauge_pixels(l, i));
    }
    return fe_rect_empty(*dirty) ? LD_DIFF_SAME : LD_DIFF_GAUGES;
}
