#include "frontend_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jpt_display.h"
#include "jpt_l10n.h"

/* Plaques glow and cast shadows past their own rectangle. */
#define FE_GLOW_MARGIN 10

static long floor_div(long long a, long long b) {
    long long q = a / b;
    return (long)((a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q);
}

static long ceil_div(long long a, long long b) { return -floor_div(-a, b); }

fe_view_t fe_view_make_for(int guest_w, int guest_h, int screen_w, int screen_h) {
    jpt_ui_canvas_t c = jpt_ui_canvas(guest_w, guest_h);
    double kx = (double)screen_w / guest_w, ky = (double)screen_h / guest_h;
    fe_view_t v;
    v.screen_w = screen_w;
    v.screen_h = screen_h;
    v.guest_w = guest_w;
    v.guest_h = guest_h;
    v.x0 = c.rect.left * kx;
    v.y0 = c.rect.top * ky;
    v.sx = (double)(c.rect.right - c.rect.left) * kx / FE_CANVAS_W;
    v.sy = (double)(c.rect.bottom - c.rect.top) * ky / FE_CANVAS_H;
    return v;
}

fe_view_t fe_view_make(int screen_w, int screen_h) { return fe_view_make_for(screen_w, screen_h, screen_w, screen_h); }

fe_rect_t fe_view_rect(const fe_view_t* v, fe_rect_t r) {
    long long w = (long long)(v->sx * FE_CANVAS_W + 0.5), h = (long long)(v->sy * FE_CANVAS_H + 0.5);
    int x0 = (int)floor(v->x0 + 0.5), y0 = (int)floor(v->y0 + 0.5);
    fe_rect_t out;
    out.left = x0 + (int)floor_div((long long)r.left * w, FE_CANVAS_W);
    out.top = y0 + (int)floor_div((long long)r.top * h, FE_CANVAS_H);
    out.right = x0 + (int)ceil_div((long long)r.right * w, FE_CANVAS_W);
    out.bottom = y0 + (int)ceil_div((long long)r.bottom * h, FE_CANVAS_H);
    return out;
}

double fe_view_x(const fe_view_t* v, double u) { return v->x0 + u * v->sx; }

double fe_view_y(const fe_view_t* v, double u) { return v->y0 + u * v->sy; }

void fe_view_to_canvas(const fe_view_t* v, int x, int y, int* u, int* w) {
    jpt_ui_canvas_t c = jpt_ui_canvas(v->guest_w, v->guest_h);
    jpt_ui_to_canvas(&c, (int)((long long)x * v->guest_w / v->screen_w), (int)((long long)y * v->guest_h / v->screen_h), u, w);
}

fe_rect_t fe_rect(int left, int top, int right, int bottom) {
    fe_rect_t r;
    r.left = left;
    r.top = top;
    r.right = right;
    r.bottom = bottom;
    return r;
}

int fe_rect_empty(fe_rect_t r) { return r.right <= r.left || r.bottom <= r.top; }

fe_rect_t fe_rect_union(fe_rect_t a, fe_rect_t b) {
    if (fe_rect_empty(a)) return b;
    if (fe_rect_empty(b)) return a;
    return fe_rect(a.left < b.left ? a.left : b.left, a.top < b.top ? a.top : b.top,
                   a.right > b.right ? a.right : b.right, a.bottom > b.bottom ? a.bottom : b.bottom);
}

fe_rect_t fe_rect_intersect(fe_rect_t a, fe_rect_t b) {
    fe_rect_t r = fe_rect(a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top,
                          a.right < b.right ? a.right : b.right, a.bottom < b.bottom ? a.bottom : b.bottom);
    return fe_rect_empty(r) ? fe_rect(0, 0, 0, 0) : r;
}

fe_rect_t fe_rect_inflate(fe_rect_t r, int dx, int dy) { return fe_rect(r.left - dx, r.top - dy, r.right + dx, r.bottom + dy); }

fe_rect_t fe_rect_offset(fe_rect_t r, int dx, int dy) { return fe_rect(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy); }

int fe_rect_contains(fe_rect_t r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

int fe_rect_equal(fe_rect_t a, fe_rect_t b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void fe_scene_reset(fe_scene_t* s, int screen_w, int screen_h) { fe_scene_reset_for(s, screen_w, screen_h, screen_w, screen_h); }

void fe_scene_reset_for(fe_scene_t* s, int guest_w, int guest_h, int screen_w, int screen_h) {
    s->screen_w = screen_w;
    s->screen_h = screen_h;
    s->guest_w = guest_w;
    s->guest_h = guest_h;
    s->window_count = s->widget_count = s->item_count = 0;
    s->text_used = 1;
    s->text[0] = 0;
    s->cursor_x = s->cursor_y = -1;
    s->backdrop = NULL;
    s->backdrop_pitch = 0;
    s->backdrop_hash = 0;
    s->footer_text = -1;
    s->footer_box = fe_rect(0, 0, 0, 0);
    s->legend_count = 0;
}

int fe_scene_text(fe_scene_t* s, const char* text) {
    size_t n = text ? strlen(text) : 0;
    int at = s->text_used;
    if (!text) return -1;
    if ((size_t)s->text_used + n + 1 > sizeof s->text) return -1;
    memcpy(s->text + at, text, n + 1);
    s->text_used += (int)n + 1;
    return at;
}

const char* fe_scene_str(const fe_scene_t* s, int offset) {
    return offset > 0 && offset < s->text_used ? s->text + offset : "";
}

fe_window_t* fe_scene_window(fe_scene_t* s, fe_window_kind_t kind, fe_rect_t rect) {
    fe_window_t* w;
    if (s->window_count >= FE_MAX_WINDOWS) return NULL;
    w = &s->windows[s->window_count++];
    memset(w, 0, sizeof *w);
    w->kind = (uint8_t)kind;
    w->rect = rect;
    w->panel = rect;
    w->title = -1;
    w->first = s->widget_count;
    return w;
}

fe_widget_t* fe_scene_widget(fe_scene_t* s, fe_widget_kind_t kind, int32_t id, fe_rect_t rect) {
    fe_widget_t* w;
    if (s->widget_count >= FE_MAX_WIDGETS || !s->window_count) return NULL;
    w = &s->widgets[s->widget_count++];
    memset(w, 0, sizeof *w);
    w->kind = (uint8_t)kind;
    w->id = id;
    w->rect = rect;
    w->text = -1;
    w->color = 0xFFFFFF;
    w->selected = -1;
    s->windows[s->window_count - 1].count++;
    return w;
}

int fe_scene_item(fe_scene_t* s, const char* text) {
    int at;
    if (s->item_count >= FE_MAX_ITEMS) return 0;
    at = fe_scene_text(s, text);
    if (at < 0) return 0;
    s->items[s->item_count++] = at;
    return 1;
}

int fe_scene_legend(fe_scene_t* s, fe_prompt_t prompt, const char* text) {
    int at;
    if (s->legend_count >= FE_LEGEND_MAX || (at = fe_scene_text(s, text)) < 0) return 0;
    s->legend[s->legend_count] = prompt;
    s->legend_text[s->legend_count++] = at;
    return 1;
}

static int same_prompt(const fe_prompt_t* a, const fe_prompt_t* b) {
    return a->kind == b->kind && a->family == b->family && a->control == b->control && a->dir == b->dir &&
           a->tilt == b->tilt && a->vk == b->vk;
}

void fe_scene_finish_window(fe_scene_t* s, fe_window_t* w) {
    fe_rect_t r = w->rect;
    if (w->kind == FE_WND_MAIN || w->kind == FE_WND_GAME) {
        w->panel = w->rect;
        return;
    }
    for (int i = w->first; i < w->first + w->count; i++)
        if (s->widgets[i].kind != FE_W_IMAGE || s->widgets[i].style != FE_IMAGE_PHOTO) r = fe_rect_union(r, s->widgets[i].rect);
    w->panel = fe_rect_inflate(r, FE_PANEL_MARGIN, FE_PANEL_MARGIN);
}

uint32_t fe_hash(uint32_t h, const void* data, size_t bytes) {
    const unsigned char* p = (const unsigned char*)data;
    if (!h) h = 2166136261u;
    for (size_t i = 0; i < bytes; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

uint32_t fe_hash_pixels(const uint16_t* rows, int w, int h, int pitch, int step) {
    uint32_t hash = 2166136261u;
    if (!rows || w <= 0 || h <= 0) return 0;
    if (step < 1) step = 1;
    for (int y = 0; y < h; y += step)
        for (int x = (y / step) % step; x < w; x += step) hash = (hash ^ rows[(long)y * pitch + x]) * 16777619u;
    return hash;
}

static int same_structure(const fe_scene_t* a, const fe_scene_t* b) {
    if (a->screen_w != b->screen_w || a->screen_h != b->screen_h || a->window_count != b->window_count ||
        a->widget_count != b->widget_count || a->backdrop != b->backdrop || a->backdrop_hash != b->backdrop_hash ||
        a->legend_count != b->legend_count || strcmp(fe_scene_str(a, a->footer_text), fe_scene_str(b, b->footer_text)))
        return 0;
    for (int i = 0; i < a->legend_count; i++)
        if (!same_prompt(&a->legend[i], &b->legend[i]) ||
            strcmp(fe_scene_str(a, a->legend_text[i]), fe_scene_str(b, b->legend_text[i])))
            return 0;
    for (int i = 0; i < a->window_count; i++) {
        const fe_window_t *x = &a->windows[i], *y = &b->windows[i];
        if (x->kind != y->kind || !fe_rect_equal(x->rect, y->rect) || !fe_rect_equal(x->panel, y->panel) ||
            x->first != y->first || x->count != y->count ||
            strcmp(fe_scene_str(a, x->title), fe_scene_str(b, y->title)))
            return 0;
    }
    for (int i = 0; i < a->widget_count; i++) {
        const fe_widget_t *x = &a->widgets[i], *y = &b->widgets[i];
        if (x->kind != y->kind || x->style != y->style || x->id != y->id || !fe_rect_equal(x->rect, y->rect)) return 0;
    }
    return 1;
}

int fe_list_shown(const fe_widget_t* w) {
    int n = w->item_count - (w->top > 0 ? w->top : 0);
    if (n > w->rows) n = w->rows;
    return n > 0 ? n : 0;
}

static int same_items(const fe_scene_t* a, const fe_widget_t* x, const fe_scene_t* b, const fe_widget_t* y) {
    if (x->item_count != y->item_count || fe_list_shown(x) != fe_list_shown(y)) return 0;
    for (int i = 0; i < fe_list_shown(x); i++) {
        int ia = x->first_item + i, ib = y->first_item + i;
        if (ia >= a->item_count || ib >= b->item_count) return 0;
        if (strcmp(fe_scene_str(a, a->items[ia]), fe_scene_str(b, b->items[ib]))) return 0;
    }
    return 1;
}

static int same_content(const fe_scene_t* a, const fe_widget_t* x, const fe_scene_t* b, const fe_widget_t* y) {
    return x->state == y->state && x->text_h == y->text_h && x->text_flags == y->text_flags && x->weight == y->weight &&
           x->color == y->color && x->value == y->value && x->range == y->range && x->top == y->top &&
           x->selected == y->selected && x->row_h == y->row_h && x->rows == y->rows && x->caret == y->caret &&
           fe_rect_equal(x->track, y->track) && x->image == y->image && x->image_w == y->image_w &&
           x->label == y->label && fe_rect_equal(x->label_box, y->label_box) && fe_rect_equal(x->label_room, y->label_room) &&
           x->image_h == y->image_h && x->image_hash == y->image_hash && x->image32 == y->image32 &&
           !strcmp(fe_scene_str(a, x->text), fe_scene_str(b, y->text)) && same_items(a, x, b, y) &&
           same_prompt(&x->prompt, &y->prompt);
}

int fe_scene_diff(const fe_scene_t* before, const fe_scene_t* now, fe_rect_t* dirty) {
    fe_rect_t r = fe_rect(0, 0, 0, 0);
    if (!before || !same_structure(before, now)) return FE_DIFF_ALL;
    for (int i = 0; i < now->widget_count; i++)
        if (!same_content(before, &before->widgets[i], now, &now->widgets[i]))
            r = fe_rect_union(r, fe_rect_inflate(now->widgets[i].rect, FE_GLOW_MARGIN, FE_GLOW_MARGIN));
    *dirty = r;
    return fe_rect_empty(r) ? FE_DIFF_SAME : FE_DIFF_RECT;
}

#define FE_OPTIONS_EDGE 10

static fe_rect_t sized_at(int left, int top, int w, int h, int window_w) {
    if (left + w > window_w - FE_OPTIONS_EDGE) left = window_w - FE_OPTIONS_EDGE - w;
    if (left < FE_OPTIONS_EDGE) left = FE_OPTIONS_EDGE;
    return fe_rect(left, top, left + w, top + h);
}

void fe_layout_options(const int32_t* ids, const int* widths, const int* heights, int count, int window_w, int window_h,
                       fe_rect_t* rects) {
    int credits = 0;
    for (int i = 0; i < count; i++)
        if (ids[i] == FE_OPTIONS_CREDITS_ID) credits = 1;
    for (int i = 0; i < count; i++) {
        int w = widths[i], h = heights[i], left, top;
        switch (ids[i]) {
        case FE_OPTIONS_AUDIO_ID:
            left = credits ? window_w / 2 + 8 - w / 2 : 40;
            top = credits ? 22 : 34;
            break;
        case FE_OPTIONS_CONTROLS_ID:
            left = credits ? 26 : window_w - 34 - w;
            top = credits ? 70 : 62;
            break;
        case FE_OPTIONS_CREDITS_ID:
            left = window_w - 24 - w;
            top = 76;
            break;
        case FE_OPTIONS_CLOSE_ID:
            left = window_w / 2 + (credits ? -6 : 4) - w / 2;
            top = window_h - 12 - h;
            break;
        default:
            left = (window_w - w) / 2;
            top = 30 + 44 * i;
            break;
        }
        rects[i] = sized_at(left, top, w, h, window_w);
    }
}

void fe_layout_rows(int count, int top, int bottom, int row_h, int* row_tops) {
    int span = bottom - top - row_h;
    if (span < 0) span = 0;
    for (int i = 0; i < count; i++)
        row_tops[i] = count > 1 ? top + (int)floor((double)i * span / (count - 1) + 0.5) : top;
}

/* The game's float position truncated by _ftol (CUISlider draw, 0x004065D0). */
int fe_slider_thumb_x(fe_rect_t track, int thumb_w, int value, int range) {
    int travel = track.right - track.left - thumb_w;
    if (range <= 1 || travel <= 0) return track.left + thumb_w / 2;
    if (value < 0) value = 0;
    if (value > range - 1) value = range - 1;
    return track.left + thumb_w / 2 + (int)((double)value / (range - 1) * travel);
}

int fe_slider_drag_x(fe_rect_t track, int thumb_w, int pointer_x) {
    int pos = pointer_x - track.left - thumb_w / 2, travel = track.right - track.left - thumb_w;
    if (pos > travel) pos = travel;
    if (pos < 0) pos = 0;
    return track.left + thumb_w / 2 + pos;
}

fe_rect_t fe_list_thumb(fe_rect_t column, int arrow_h, int top, int rows, int count) {
    int track_top = column.top + arrow_h, track_h = column.bottom - arrow_h - track_top;
    if (count <= rows || count <= 0 || track_h <= 0) return fe_rect(0, 0, 0, 0);
    return fe_rect(column.left, track_top + (int)((double)top / count * track_h), column.right,
                   track_top + (int)((double)(top + rows) / count * track_h));
}

void fe_photo_fit(int image_w, int image_h, double frame_w, double frame_h, double src[4], double dst[4]) {
    double ai = (double)image_w / image_h, af = frame_w / frame_h, shown = af, w, h;
    if (ai > af && ai / FE_PHOTO_CROP > af) shown = ai / FE_PHOTO_CROP;
    if (ai < af && ai * FE_PHOTO_CROP < af) shown = ai * FE_PHOTO_CROP;
    if (ai > shown) {
        double cut = image_w - image_h * shown;
        src[0] = cut * 0.5;
        src[1] = 0;
        src[2] = image_w - cut * 0.5;
        src[3] = image_h;
    } else {
        double cut = image_h - image_w / shown;
        src[0] = 0;
        src[1] = cut * 0.5;
        src[2] = image_w;
        src[3] = image_h - cut * 0.5;
    }
    w = frame_w;
    h = frame_w / shown;
    if (h > frame_h) {
        h = frame_h;
        w = frame_h * shown;
    }
    dst[0] = (frame_w - w) * 0.5;
    dst[1] = (frame_h - h) * 0.5;
    dst[2] = dst[0] + w;
    dst[3] = dst[1] + h;
}

const char* fe_level_picture(const char* scene_file) {
    static const struct {
        const char* scene;
        const char* picture;
    } k_levels[] = {
        { "as", "li_a1_0.tga" }, { "as2", "li_a2_0.tga" }, { "be", "li_be_0.tga" },   { "ij", "li_ij_0.tga" },
        { "it", "li_it_0.tga" }, { "jr", "li_jr_0.tga" },  { "lab", "li_lab_0.tga" }, { "pv", "li_pv_0.tga" },
        { "sum", "li_sum_0.tga" },
    };
    char name[32];
    size_t n = 0;
    if (scene_file)
        while (scene_file[n] && scene_file[n] != '.' && n < sizeof name - 1) {
            char c = scene_file[n];
            name[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
    name[n] = 0;
    for (size_t i = 0; i < sizeof k_levels / sizeof k_levels[0]; i++)
        if (!strcmp(name, k_levels[i].scene)) return k_levels[i].picture;
    return "li_other_0.tga";
}

int fe_layout_direct_dx(fe_rect_t window) {
    int total = window.right - window.left + FE_DIRECT_GAP + FE_DIRECT_PHOTO_W;
    int left = (FE_CANVAS_W - total) / 2;
    return left < 0 ? 0 : left - window.left;
}

fe_rect_t fe_layout_direct_photo(fe_rect_t window) {
    int left = window.right + FE_DIRECT_GAP, top = window.top + 10;
    return fe_rect(left, top, left + FE_DIRECT_PHOTO_W, top + FE_DIRECT_PHOTO_H);
}

fe_rect_t fe_controls_window(fe_rect_t window, int canvas_w, int canvas_h) {
    int w = (int)floor(FE_CONTROLS_W * FE_CONTROLS_ZOOM + 0.5), h = (int)floor(FE_CONTROLS_H * FE_CONTROLS_ZOOM + 0.5);
    int left = (window.left + window.right - w) / 2, top = (window.top + window.bottom - h) / 2;
    if (left + w > canvas_w) left = canvas_w - w;
    if (top + h > canvas_h) top = canvas_h - h;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    return fe_rect(left, top, left + w, top + h);
}

static double map_edge(const double (*points)[2], int count, double x) {
    for (int i = 1; i < count; i++)
        if (x <= points[i][0] || i == count - 1) {
            double x0 = points[i - 1][0], x1 = points[i][0], y0 = points[i - 1][1], y1 = points[i][1];
            return x1 > x0 ? y0 + (x - x0) * (y1 - y0) / (x1 - x0) : y1;
        }
    return x;
}

fe_rect_t fe_controls_rect(fe_rect_t r, int picture) {
    static const double lefts[][2] = { { 0, 0 }, { 10, 4 }, { 94, 94 }, { 164, 162 }, { 248, 230 }, { 320, 318 } };
    static const double rights[][2] = { { 0, 0 }, { 90, 90 }, { 164, 158 }, { 246, 226 }, { 320, 318 } };
    double z = FE_CONTROLS_ZOOM, left = (picture ? r.left : map_edge(lefts, 6, r.left)) * z;
    double right = picture ? left + (r.right - r.left) * z : map_edge(rights, 5, r.right) * z;
    return fe_rect((int)floor(left + 0.5), (int)floor(r.top * z + 0.5), (int)floor(right + 0.5),
                   (int)floor(r.bottom * z + 0.5));
}

static fe_widget_t* label_with(fe_scene_t* s, const fe_window_t* w, const char* text) {
    for (int i = w->first; i < w->first + w->count; i++)
        if (s->widgets[i].kind == FE_W_LABEL && !strcmp(fe_scene_str(s, s->widgets[i].text), text)) return &s->widgets[i];
    return NULL;
}

void fe_controls_polish(fe_scene_t* s, const fe_window_t* w, double zoom) {
    const char *replay_text = jpt_l10n_text_or(10033, "Replay"), *voice_text = jpt_l10n_text_or(10034, "voice over");
    fe_widget_t *replay = label_with(s, w, replay_text), *voice = label_with(s, w, voice_text);
    for (int i = w->first; i < w->first + w->count; i++) {
        fe_widget_t* x = &s->widgets[i];
        x->text_h = (float)(x->text_h * zoom);
        x->image_zoom = (float)zoom;
        if (x->kind == FE_W_LABEL || x->kind == FE_W_FIELD)
            x->text_flags = (uint16_t)((x->text_flags & ~FE_TEXT_WRAP) | FE_TEXT_FIT | FE_TEXT_VCENTER);
    }
    /* the ddf breaks "Replay voice over" over two text boxes; one line reads as the other rows do */
    if (replay && voice) {
        char joined[128];
        int text;
        snprintf(joined, sizeof joined, "%s %s", replay_text, voice_text);
        text = fe_scene_text(s, joined);
        if (text >= 0) {
            replay->text = text;
            voice->text = -1;
        }
    }
}
