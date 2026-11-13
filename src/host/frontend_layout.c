#include "frontend_layout.h"

#include <math.h>
#include <string.h>

#include "jpt_display.h"

/* Plaques glow and cast shadows past their own rectangle. */
#define FE_GLOW_MARGIN 10

static long floor_div(long long a, long long b) {
    long long q = a / b;
    return (long)((a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q);
}

static long ceil_div(long long a, long long b) { return -floor_div(-a, b); }

fe_view_t fe_view_make(int screen_w, int screen_h) {
    jpt_ui_canvas_t c = jpt_ui_canvas(screen_w, screen_h);
    fe_view_t v;
    v.screen_w = screen_w;
    v.screen_h = screen_h;
    v.x0 = c.rect.left;
    v.y0 = c.rect.top;
    v.sx = (double)(c.rect.right - c.rect.left) / FE_CANVAS_W;
    v.sy = (double)(c.rect.bottom - c.rect.top) / FE_CANVAS_H;
    return v;
}

fe_rect_t fe_view_rect(const fe_view_t* v, fe_rect_t r) {
    long long w = (long long)(v->sx * FE_CANVAS_W + 0.5), h = (long long)(v->sy * FE_CANVAS_H + 0.5);
    fe_rect_t out;
    out.left = (int)v->x0 + (int)floor_div((long long)r.left * w, FE_CANVAS_W);
    out.top = (int)v->y0 + (int)floor_div((long long)r.top * h, FE_CANVAS_H);
    out.right = (int)v->x0 + (int)ceil_div((long long)r.right * w, FE_CANVAS_W);
    out.bottom = (int)v->y0 + (int)ceil_div((long long)r.bottom * h, FE_CANVAS_H);
    return out;
}

double fe_view_x(const fe_view_t* v, double u) { return v->x0 + u * v->sx; }

double fe_view_y(const fe_view_t* v, double u) { return v->y0 + u * v->sy; }

void fe_view_to_canvas(const fe_view_t* v, int x, int y, int* u, int* w) {
    jpt_ui_canvas_t c = jpt_ui_canvas(v->screen_w, v->screen_h);
    jpt_ui_to_canvas(&c, x, y, u, w);
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

void fe_scene_reset(fe_scene_t* s, int screen_w, int screen_h) {
    s->screen_w = screen_w;
    s->screen_h = screen_h;
    s->window_count = s->widget_count = s->item_count = 0;
    s->text_used = 1;
    s->text[0] = 0;
    s->cursor_x = s->cursor_y = -1;
    s->backdrop = NULL;
    s->backdrop_pitch = 0;
    s->backdrop_hash = 0;
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

#define FE_CHECK_LABEL_GAP 8

void fe_scene_tidy_check_rows(fe_scene_t* s, const fe_window_t* w) {
    for (int i = w->first; i < w->first + w->count; i++) {
        const fe_widget_t* c = &s->widgets[i];
        if (c->kind != FE_W_CHECK) continue;
        for (int k = w->first; k < w->first + w->count; k++) {
            fe_widget_t* l = &s->widgets[k];
            int mid = (l->rect.top + l->rect.bottom) / 2;
            if (l->kind != FE_W_LABEL || l->rect.left < c->rect.right || mid < c->rect.top - 2 || mid > c->rect.bottom + 2 ||
                (l->text_flags & (FE_TEXT_CENTER | FE_TEXT_RIGHT)))
                continue;
            if (l->rect.left > c->rect.right + FE_CHECK_LABEL_GAP) l->rect.left = c->rect.right + FE_CHECK_LABEL_GAP;
        }
    }
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
        a->widget_count != b->widget_count || a->backdrop != b->backdrop || a->backdrop_hash != b->backdrop_hash)
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
           x->image_h == y->image_h && x->image_hash == y->image_hash && x->image32 == y->image32 &&
           !strcmp(fe_scene_str(a, x->text), fe_scene_str(b, y->text)) && same_items(a, x, b, y);
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

void fe_layout_main_menu(const int* label_lengths, int count, fe_rect_t* rects) {
    static const int heights[] = { 72, 66, 70, 64 }, dx[] = { 0, -9, 7, -4 }, dy[] = { 0, 3, -2, 4 };
    for (int k = 0; k < count; k++) {
        int w = 116 + 11 * label_lengths[k], h = heights[k % 4], cx = 309 + dx[k % 4];
        int top = 70 + 90 * k + (81 - h) / 2 + dy[k % 4];
        if (w > 208) w = 208;
        if (w < 150) w = 150;
        rects[k] = fe_rect(cx - w / 2, top, cx - w / 2 + w, top + h);
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

fe_rect_t fe_layout_photo(fe_window_kind_t kind, fe_rect_t window) {
    if (kind == FE_WND_SAVE) return fe_rect(window.left - 62, window.top + 38, window.left + 120, window.top + 156);
    return fe_rect(window.left - 62, window.top + 10, window.left + 118, window.top + 145);
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
