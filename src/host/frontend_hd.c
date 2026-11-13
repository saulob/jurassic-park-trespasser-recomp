/*
 * The HD frontend. While the game shows its menus on the UI canvas (ui_canvas.c), its windows and controls are read
 * every UI frame and drawn again at the screen's resolution (frontend_assets.c). The game keeps all of its own rules:
 * hit tests, hover, values and callbacks stay in its objects, and its canvas is shown instead whenever a window or a
 * control is not one the frontend knows.
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

#include "frontend_assets.h"
#include "frontend_hd.h"
#include "save_preview.h"

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define MANAGER_OFFSET 0x08u
#define MGR_CURSOR_X 0x18u
#define MGR_CURSOR_Y 0x1Cu
#define MGR_CURSOR_SHOWN 0x20u
#define MGR_CURSOR_INDEX 0x24u
#define MGR_CURSOR_IMAGES 0x28u
#define MGR_STACK_BEGIN 0x38u
#define MGR_STACK_END 0x3Cu
#define WND_RECT 0x10u
#define WND_CONTROLS_BEGIN 0x20u
#define WND_CONTROLS_END 0x24u
#define WND_BACKGROUND 0x2Cu
#define CTRL_FLAGS 0x08u
#define CTRL_ID 0x0Cu
#define CTRL_RECT 0x10u
#define CTRL_ENABLED 2u
#define CTRL_VISIBLE 4u
#define RASTER_W 0x08u
#define RASTER_H 0x0Cu
#define RASTER_BPP 0x10u
#define RASTER_PITCH 0x14u
#define RASTER_BITS 0x18u

#define VT_BUTTON 0x0064554Cu
#define VT_LISTBOX 0x00645650u
#define VT_CHECKBOX 0x00645758u
#define VT_TEXTBOX 0x006457B8u
#define VT_SLIDER 0x00645864u
#define VT_EDITBOX 0x006458DCu
#define VT_STATIC 0x006455B0u
#define VT_PROGRESS 0x00645974u
#define VT_HOTSPOT 0x006459CCu
#define VT_OPTIONS 0x00646A58u
#define VT_AUDIO 0x00646C28u

#define BUTTON_STATE 0x28u
#define BUTTON_IMAGES 0x2Cu
#define BUTTON_HOVER 2u
#define BUTTON_PRESSED 4u
#define CHECK_STATE 0x28u
#define CHECK_HOVER 1u
#define CHECK_CHECKED 2u
#define SLIDER_STATE 0x24u
#define SLIDER_DRAG_X 0x28u
#define SLIDER_UNITS 0x2Cu
#define SLIDER_VALUE 0x30u
#define SLIDER_THUMB 0x40u
#define SLIDER_HOVER 1u
#define SLIDER_GRABBED 2u
#define SLIDER_DRAGGING 4u
#define TEXT_STRING 0x28u
#define TEXT_FG 0x30u
#define TEXT_DT 0x3Cu
#define TEXT_SHADOW 0x40u
#define TEXT_BORDER 0x44u
#define TEXT_TRANSPARENT 0x48u
#define TEXT_POINTS 0x4Eu
#define EDIT_TEXT 0x24u
#define EDIT_FG 0x3Cu
#define EDIT_POINTS 0x48u
#define EDIT_SCROLL 0x54u
#define LIST_ITEMS_BEGIN 0x24u
#define LIST_ITEMS_END 0x28u
#define LIST_UP 0x30u
#define LIST_DOWN 0x34u
#define LIST_FLAGS 0x38u
#define LIST_SELECTED 0x3Cu
#define LIST_TOP 0x40u
#define LIST_ROWS 0x44u
#define LIST_BUTTON_W 0x48u
#define LIST_LINE_H 0x4Cu
#define LIST_FG 0x68u
#define LIST_POINTS 0x6Cu
#define LIST_ITEM_SIZE 12u
#define LIST_SCROLLBAR 0x0Cu
#define STATIC_TRANSPARENT 0x20u
#define STATIC_KEY 0x22u
#define STATIC_IMAGE 0x24u

#define MAX_STACK 16u
#define MAX_CONTROLS 160u
#define TEXT_CAP 512
#define IDLE_FRAMES 120
#define LOG_LIMIT 8

typedef struct {
    uint32_t vtable;
    uint8_t kind, yes_no;
    const char* title;
} window_class_t;

static const window_class_t k_windows[] = {
    { 0x00645C3Cu, FE_WND_GAME, 0, NULL },          { 0x006466B8u, FE_WND_MAIN, 0, NULL },
    { VT_OPTIONS, FE_WND_OPTIONS, 0, "OPTIONS" },   { 0x00646B40u, FE_WND_GAME_MENU, 0, "PAUSED" },
    { VT_AUDIO, FE_WND_AUDIO, 0, "AUDIO" },         { 0x00645F38u, FE_WND_CONTROLS, 0, "CONTROLS" },
    { 0x006460D0u, FE_WND_LOAD, 0, "LOAD GAME" },   { 0x006462A8u, FE_WND_SAVE, 0, "SAVE GAME" },
    { 0x00646970u, FE_WND_QUIT, 0, "QUIT" },        { 0x00645D34u, FE_WND_MESSAGE, 1, NULL },
    { 0x00645E24u, FE_WND_MESSAGE, 1, NULL },       { 0x006461B8u, FE_WND_MESSAGE, 1, NULL },
    { 0x00646390u, FE_WND_MESSAGE, 0, NULL },       { 0x00646480u, FE_WND_MESSAGE, 0, NULL },
    { 0x00646D10u, FE_WND_MESSAGE, 0, NULL },
};

static const struct {
    uint8_t window;
    int32_t id;
    const char* label;
} k_labels[] = {
    { FE_WND_MAIN, 1000, "NEW GAME" },     { FE_WND_MAIN, 1001, "LOAD" },        { FE_WND_MAIN, 1002, "OPTIONS" },
    { FE_WND_MAIN, 1003, "EXIT" },         { FE_WND_OPTIONS, 1000, "CONTROLS" }, { FE_WND_OPTIONS, 1001, "VIDEO" },
    { FE_WND_OPTIONS, 1002, "AUDIO" },     { FE_WND_OPTIONS, 1003, "CREDITS" },  { FE_WND_OPTIONS, 1004, "CLOSE" },
    { FE_WND_GAME_MENU, 1000, "QUIT" },    { FE_WND_GAME_MENU, 1001, "RESUME" }, { FE_WND_GAME_MENU, 1002, "RESTART LEVEL" },
    { FE_WND_GAME_MENU, 1003, "SAVE" },    { FE_WND_GAME_MENU, 1004, "LOAD" },   { FE_WND_GAME_MENU, 1005, "OPTIONS" },
    { FE_WND_AUDIO, 1000, "CANCEL" },      { FE_WND_AUDIO, 1001, "OK" },         { FE_WND_CONTROLS, 1000, "CANCEL" },
    { FE_WND_CONTROLS, 1001, "OK" },       { FE_WND_CONTROLS, 1002, "DEFAULTS" }, { FE_WND_LOAD, 1000, "CANCEL" },
    { FE_WND_LOAD, 1001, "OK" },           { FE_WND_LOAD, 1004, "DELETE" },      { FE_WND_SAVE, 1000, "CANCEL" },
    { FE_WND_SAVE, 1001, "OK" },           { FE_WND_SAVE, 1005, "DELETE" },      { FE_WND_QUIT, 1000, "CANCEL" },
    { FE_WND_QUIT, 1001, "DESKTOP" },      { FE_WND_QUIT, 1002, "MAIN MENU" },
};

#define MAIN_LOGO_ID 1005
#define PREVIEW_ID 1003
#define AUDIO_3D_CHECK 1025
#define AUDIO_3D_HOTSPOT 1105
#define AUDIO_FIRST_CHECK 1020
#define AUDIO_FIRST_HOTSPOT 1100
#define AUDIO_ROWS 5
#define AUDIO_ROW_H 15
#define OPTIONS_VIDEO_ID 1001

static struct {
    fe_painter_t* painter;
    int painter_failed, threads;
    fe_scene_t scenes[2];
    int current, have_previous;
    uint32_t* layer;
    int layer_w, layer_h;
    uint32_t cursor[96 * 96];
    int cursor_size, cursor_hot_x, cursor_hot_y;
    double cursor_scale;
    unsigned cursor_generation;
    int idle_frames, shown;
    int logged, sizes_logged;
    char last_refusal[96];
} F;

static uint32_t find_control(uint32_t window, int32_t id, uint32_t vtable);
static int write_rect(uint32_t control, fe_rect_t r);

static void log_limited(const char* fmt, const char* detail) {
    if (F.logged >= LOG_LIMIT) return;
    host_log("frontend", fmt, detail);
    if (++F.logged == LOG_LIMIT) host_log("frontend", "frontend log limit reached");
}

static void refuse(const char* why) {
    if (strcmp(F.last_refusal, why)) {
        strncpy(F.last_refusal, why, sizeof F.last_refusal - 1);
        log_limited("the game's own 640x480 menu is shown: %s", why);
    }
}

static int read32(uint32_t va, uint32_t* out) { return guest_read32(va, out); }

static uint32_t peek32(uint32_t va) {
    uint32_t v = 0;
    return read32(va, &v) ? v : 0;
}

static int read8(uint32_t va, uint8_t* out) {
    if (!guest_readable(va, 1)) return 0;
    *out = *(const volatile uint8_t*)(uintptr_t)va;
    return 1;
}

static int read_rect(uint32_t va, fe_rect_t* r) {
    uint32_t v[4];
    for (int i = 0; i < 4; i++)
        if (!read32(va + 4u * (uint32_t)i, &v[i])) return 0;
    *r = fe_rect((int32_t)v[0], (int32_t)v[1], (int32_t)v[2], (int32_t)v[3]);
    return 1;
}

static const window_class_t* window_class(uint32_t vtable) {
    for (size_t i = 0; i < sizeof k_windows / sizeof k_windows[0]; i++)
        if (k_windows[i].vtable == vtable) return &k_windows[i];
    return NULL;
}

static const char* button_label(const window_class_t* wc, int32_t id) {
    if (wc->kind == FE_WND_MESSAGE) return id == 1000 ? (wc->yes_no ? "YES" : "OK") : id == 1001 && wc->yes_no ? "NO" : NULL;
    for (size_t i = 0; i < sizeof k_labels / sizeof k_labels[0]; i++)
        if (k_labels[i].window == wc->kind && k_labels[i].id == id) return k_labels[i].label;
    return NULL;
}

static uint32_t colorref_rgb(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

static uint16_t text_flags(uint32_t dt, int shadow) {
    uint16_t f = FE_TEXT_VCENTER * ((dt & DT_VCENTER) != 0);
    if (dt & DT_CENTER) f |= FE_TEXT_CENTER;
    if (dt & DT_RIGHT) f |= FE_TEXT_RIGHT;
    if (dt & DT_BOTTOM) f |= FE_TEXT_BOTTOM;
    if (dt & DT_WORDBREAK) f |= FE_TEXT_WRAP;
    if (shadow) f |= FE_TEXT_SHADOW;
    return f;
}

/* A 16-bit raster of the game's own image classes (memory and DIB rasters: their Lock does nothing). */
static int read_image(uint32_t raster, const uint16_t** bits, int* w, int* h, int* pitch) {
    uint32_t rw, rh, bpp, rp, rb;
    if (!raster || !read32(raster + RASTER_W, &rw) || !read32(raster + RASTER_H, &rh) || !read32(raster + RASTER_BPP, &bpp) ||
        !read32(raster + RASTER_PITCH, &rp) || !read32(raster + RASTER_BITS, &rb))
        return 0;
    if (bpp != 16 || !rb || rw == 0 || rh == 0 || rw > 4096 || rh > 4096 || rp < rw ||
        !guest_readable(rb, (rp * (rh - 1) + rw) * 2u))
        return 0;
    *bits = (const uint16_t*)(uintptr_t)rb;
    *w = (int)rw;
    *h = (int)rh;
    *pitch = (int)rp;
    return 1;
}

typedef struct {
    fe_scene_t* s;
    const window_class_t* wc;
    uint32_t window;
    uint32_t arrows[8];
    int arrow_count;
    fe_rect_t hotspots[32];
    int hotspot_count;
    int cursor_x, cursor_y;
    const uint16_t* thumb; /* the game's own thumbnail of the selected save */
    int thumb_w, thumb_h, thumb_pitch;
} reader_t;

static int is_arrow(const reader_t* r, uint32_t control, int* down) {
    for (int i = 0; i < r->arrow_count; i++)
        if (r->arrows[i] == control) {
            *down = i & 1;
            return 1;
        }
    return 0;
}

static int add_button(reader_t* r, uint32_t c, uint32_t flags, int32_t id, fe_rect_t rect) {
    uint32_t state = peek32(c + BUTTON_STATE), images = 0;
    const char* label;
    int down;
    fe_widget_t* w;
    for (uint32_t i = 0; i < 4; i++) images |= peek32(c + BUTTON_IMAGES + 4 * i);
    if (!images) return 1;
    w = fe_scene_widget(r->s, FE_W_BUTTON, id, rect);
    if (!w) return 0;
    if (!(flags & CTRL_ENABLED)) w->state = FE_ST_DISABLED;
    else w->state = (uint16_t)(((state & BUTTON_HOVER) ? FE_ST_HOVER : 0) | ((state & BUTTON_PRESSED) ? FE_ST_PRESSED : 0));
    if (is_arrow(r, c, &down)) {
        w->style = down ? FE_BUTTON_ARROW_DOWN : FE_BUTTON_ARROW_UP;
        return 1;
    }
    label = button_label(r->wc, id);
    if (!label) {
        refuse("a button the frontend has no label for");
        return 0;
    }
    w->style = r->wc->kind == FE_WND_MAIN ? FE_BUTTON_LARGE : FE_BUTTON_PLAQUE;
    w->text = fe_scene_text(r->s, label);
    return 1;
}

static int add_check(reader_t* r, uint32_t c, uint32_t flags, int32_t id, fe_rect_t rect) {
    uint32_t state = peek32(c + CHECK_STATE);
    fe_widget_t* w;
    if (!(flags & CTRL_ENABLED)) return 1;
    w = fe_scene_widget(r->s, FE_W_CHECK, id, rect);
    if (!w) return 0;
    w->state = (uint16_t)(((state & CHECK_HOVER) ? FE_ST_HOVER : 0) | ((state & CHECK_CHECKED) ? FE_ST_CHECKED : 0));
    return 1;
}

static int add_slider(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    uint32_t state = peek32(c + SLIDER_STATE), units = peek32(c + SLIDER_UNITS), thumb = peek32(c + SLIDER_THUMB);
    int thumb_w = thumb ? (int)peek32(thumb + RASTER_W) : 10;
    fe_widget_t* w = fe_scene_widget(r->s, FE_W_SLIDER, id, rect);
    if (!w) return 0;
    if (thumb_w <= 0 || thumb_w > 64) thumb_w = 10;
    w->range = (int)units;
    w->value = (state & SLIDER_DRAGGING) ? fe_slider_drag_x(rect, thumb_w, (int32_t)peek32(c + SLIDER_DRAG_X))
                                         : fe_slider_thumb_x(rect, thumb_w, (int32_t)peek32(c + SLIDER_VALUE), (int)units);
    w->state = (uint16_t)(((state & SLIDER_HOVER) ? FE_ST_HOVER : 0) | ((state & (SLIDER_GRABBED | SLIDER_DRAGGING)) ? FE_ST_PRESSED : 0));
    return 1;
}

static int add_textbox(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    char text[TEXT_CAP];
    uint8_t points = 12, transparent = 1;
    uint32_t border = peek32(c + TEXT_BORDER);
    fe_widget_t* w;
    if (!guest_read_string(peek32(c + TEXT_STRING), text, sizeof text)) text[0] = 0;
    read8(c + TEXT_POINTS, &points);
    read8(c + TEXT_TRANSPARENT, &transparent);
    w = fe_scene_widget(r->s, border ? FE_W_FIELD : FE_W_LABEL, id, rect);
    if (!w) return 0;
    w->text = fe_scene_text(r->s, text);
    w->text_h = points;
    w->color = colorref_rgb(peek32(c + TEXT_FG));
    w->text_flags = text_flags(peek32(c + TEXT_DT), peek32(c + TEXT_SHADOW) != 0);
    if (!transparent) {
        w->state = FE_ST_FOCUS;
        w->color = 0xFFFFFF;
    }
    return 1;
}

static int add_editbox(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    char text[TEXT_CAP];
    uint8_t points = 12;
    uint32_t scroll = peek32(c + EDIT_SCROLL);
    size_t len;
    fe_widget_t* w;
    if (!guest_read_string(peek32(c + EDIT_TEXT), text, sizeof text)) text[0] = 0;
    len = strlen(text);
    if (scroll > len) scroll = (uint32_t)len;
    read8(c + EDIT_POINTS, &points);
    w = fe_scene_widget(r->s, FE_W_EDIT, id, rect);
    if (!w) return 0;
    w->text = fe_scene_text(r->s, text + scroll);
    w->text_h = points;
    w->color = colorref_rgb(peek32(c + EDIT_FG));
    w->caret = (int)(len - scroll);
    w->state = FE_ST_CARET;
    return 1;
}

static int add_listbox(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    uint32_t begin = peek32(c + LIST_ITEMS_BEGIN), end = peek32(c + LIST_ITEMS_END), flags = peek32(c + LIST_FLAGS);
    int32_t line_h = (int32_t)peek32(c + LIST_LINE_H), side = (int32_t)peek32(c + LIST_BUTTON_W);
    uint8_t points = 12;
    fe_widget_t* w;
    if (end < begin || (end - begin) % LIST_ITEM_SIZE || (end - begin) / LIST_ITEM_SIZE > 100000u || line_h <= 0 ||
        line_h > 200) {
        refuse("a list the frontend cannot read");
        return 0;
    }
    read8(c + LIST_POINTS, &points);
    w = fe_scene_widget(r->s, FE_W_LIST, id, rect);
    if (!w) return 0;
    w->item_count = (int)((end - begin) / LIST_ITEM_SIZE);
    w->top = (int32_t)peek32(c + LIST_TOP);
    w->selected = (int32_t)peek32(c + LIST_SELECTED);
    w->rows = (int32_t)peek32(c + LIST_ROWS);
    w->row_h = line_h;
    w->text_h = points;
    w->color = colorref_rgb(peek32(c + LIST_FG));
    if (w->top < 0) w->top = 0;
    if (side <= 0 || side > 64) side = 16;
    if (flags & LIST_SCROLLBAR) w->track = fe_rect(rect.right - side, rect.top, rect.right, rect.bottom);
    w->first_item = r->s->item_count;
    for (int i = 0; i < fe_list_shown(w); i++) {
        char text[TEXT_CAP];
        if (!guest_read_string(peek32(begin + (uint32_t)(w->top + i) * LIST_ITEM_SIZE), text, sizeof text)) text[0] = 0;
        if (!fe_scene_item(r->s, text)) {
            refuse("a list longer than the frontend's text room");
            return 0;
        }
    }
    return 1;
}

static int add_static(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    uint8_t transparent = 1;
    uint16_t key = 0;
    const uint16_t* bits;
    int w_, h_, pitch;
    fe_widget_t* w;
    int logo = r->wc->kind == FE_WND_MAIN && id == MAIN_LOGO_ID;
    int preview = (r->wc->kind == FE_WND_LOAD || r->wc->kind == FE_WND_SAVE) && id == PREVIEW_ID;
    if (!logo && !preview) return 1;
    if (!read_image(peek32(c + STATIC_IMAGE), &bits, &w_, &h_, &pitch)) return 1;
    if (preview) {
        r->thumb = bits;
        r->thumb_w = w_;
        r->thumb_h = h_;
        r->thumb_pitch = pitch;
        return 1;
    }
    read8(c + STATIC_TRANSPARENT, &transparent);
    if (guest_readable(c + STATIC_KEY, 2)) key = *(const volatile uint16_t*)(uintptr_t)(c + STATIC_KEY);
    w = fe_scene_widget(r->s, FE_W_IMAGE, id, rect);
    if (!w) return 0;
    w->style = (uint8_t)preview;
    w->image = bits;
    w->image_w = w_;
    w->image_h = h_;
    w->image_pitch = pitch;
    w->image_key = key;
    w->image_keyed = transparent != 0;
    w->image_hash = fe_hash_pixels(bits, w_, h_, pitch, w_ * h_ > 20000 ? 3 : 1);
    return 1;
}

static int add_control(reader_t* r, uint32_t c) {
    uint32_t vt, flags, id;
    fe_rect_t rect;
    if (!read32(c, &vt) || !read32(c + CTRL_FLAGS, &flags) || !read32(c + CTRL_ID, &id) || !read_rect(c + CTRL_RECT, &rect))
        return 0;
    if (!(flags & CTRL_VISIBLE)) return 1;
    switch (vt) {
    case VT_BUTTON: return add_button(r, c, flags, (int32_t)id, rect);
    case VT_CHECKBOX: return add_check(r, c, flags, (int32_t)id, rect);
    case VT_SLIDER: return add_slider(r, c, (int32_t)id, rect);
    case VT_TEXTBOX: return add_textbox(r, c, (int32_t)id, rect);
    case VT_EDITBOX: return add_editbox(r, c, (int32_t)id, rect);
    case VT_LISTBOX: return add_listbox(r, c, (int32_t)id, rect);
    case VT_STATIC: return add_static(r, c, (int32_t)id, rect);
    case VT_HOTSPOT:
        if (r->hotspot_count < (int)(sizeof r->hotspots / sizeof r->hotspots[0])) r->hotspots[r->hotspot_count++] = rect;
        return 1;
    case VT_PROGRESS: refuse("a progress bar"); return 0;
    default: refuse("a control the frontend does not know"); return 0;
    }
}

static int read_controls(uint32_t window, uint32_t* out, unsigned* count) {
    uint32_t begin, end;
    if (!read32(window + WND_CONTROLS_BEGIN, &begin) || !read32(window + WND_CONTROLS_END, &end) || end < begin ||
        (end - begin) % 4 || (end - begin) / 4 > MAX_CONTROLS || !guest_readable(begin, end - begin))
        return 0;
    *count = (end - begin) / 4;
    for (unsigned i = 0; i < *count; i++) out[i] = ((const uint32_t*)(uintptr_t)begin)[i];
    return 1;
}

/* Labels, fields and check boxes under the pointer's row (the game's hotspots, which it never draws). */
static void mark_row_hover(reader_t* r, const fe_window_t* w) {
    for (int h = 0; h < r->hotspot_count; h++) {
        if (!fe_rect_contains(r->hotspots[h], r->cursor_x, r->cursor_y)) continue;
        for (int i = w->first; i < w->first + w->count; i++) {
            fe_widget_t* x = &r->s->widgets[i];
            if ((x->kind == FE_W_LABEL || x->kind == FE_W_FIELD || x->kind == FE_W_CHECK) &&
                !fe_rect_empty(fe_rect_intersect(x->rect, r->hotspots[h])))
                x->state |= FE_ST_ROW_HOVER;
        }
    }
}

static void add_volume_caption(fe_scene_t* s, const fe_window_t* w) {
    for (int i = w->first; i < w->first + w->count; i++) {
        const fe_widget_t* x = &s->widgets[i];
        fe_widget_t* c;
        fe_rect_t r;
        if (x->kind != FE_W_SLIDER) continue;
        r = fe_rect(w->rect.left + 16, x->rect.top, x->rect.left - 6, x->rect.bottom);
        if (r.right - r.left < 40) return;
        c = fe_scene_widget(s, FE_W_LABEL, -1, r);
        if (!c) return;
        c->text = fe_scene_text(s, "Master Volume");
        c->text_h = 12.0f;
        c->text_flags = FE_TEXT_RIGHT | FE_TEXT_VCENTER | FE_TEXT_SHADOW;
        return;
    }
}

/* Load (CLoadGameWnd) and Save (CSaveGameWnd) keep one 84-byte entry per listed save, in list order, with the save's
 * file name at +0x28; their list is at +0x34 and +0x38. */
#define LOAD_LIST 0x34u
#define LOAD_ENTRIES 0x38u
#define SAVE_LIST 0x38u
#define SAVE_ENTRIES 0x3Cu
#define SAVE_ENTRY_SIZE 0x54u
#define SAVE_ENTRY_FILE 0x28u
#define GAME_INSTANCE 0x0066E05Cu
#define SAVE_NAME_STRING 10
#define PREVIEW_SLOTS 2

typedef struct {
    char path[MAX_PATH];
    FILETIME written;
    DWORD size;
    sp_image_t image;
    int used;
} preview_slot_t;

static struct {
    preview_slot_t slots[PREVIEW_SLOTS];
    int next;
    const uint16_t* frozen_bits;
    uint32_t frozen_hash;
    sp_image_t frozen;
    char base[32];
    int written_logged, failed_logged;
} P;

/* The saves' file name before the number: the game lists <Installed Directory><string 10>.* */
static const char* save_base(void) {
    if (!P.base[0] && !LoadStringA((HINSTANCE)(uintptr_t)peek32(GAME_INSTANCE), SAVE_NAME_STRING, P.base, sizeof P.base))
        strcpy(P.base, "SAVEGAME");
    return P.base;
}

static int save_path(const char* user_dir, const char* name, char* out, size_t cap) {
    size_t n = strlen(user_dir);
    int written;
    if (!n || !name[0] || strpbrk(name, "\\/:")) return 0;
    written = _snprintf(out, cap, "%s%s%s", user_dir, user_dir[n - 1] == '\\' ? "" : "\\", name);
    return written > 0 && (size_t)written < cap;
}

static int selected_save(const reader_t* r, char* out, size_t cap) {
    int load = r->wc->kind == FE_WND_LOAD;
    uint32_t entries = r->window + (load ? LOAD_ENTRIES : SAVE_ENTRIES);
    uint32_t list = peek32(r->window + (load ? LOAD_LIST : SAVE_LIST)), begin = peek32(entries), end = peek32(entries + 4);
    int32_t selected = list && peek32(list) == VT_LISTBOX ? (int32_t)peek32(list + LIST_SELECTED) : -1;
    char name[64];
    if (selected < 0 || end < begin || (end - begin) % SAVE_ENTRY_SIZE || (uint32_t)selected >= (end - begin) / SAVE_ENTRY_SIZE)
        return 0;
    if (!guest_read_string(begin + (uint32_t)selected * SAVE_ENTRY_SIZE + SAVE_ENTRY_FILE, name, sizeof name)) return 0;
    return save_path(io_trace_user_folder(), name, out, cap);
}

static void forget_preview(const char* path) {
    for (int i = 0; i < PREVIEW_SLOTS; i++)
        if (P.slots[i].used && !_stricmp(P.slots[i].path, path)) {
            save_preview_free(&P.slots[i].image);
            P.slots[i].used = 0;
        }
}

/* The decoded HD picture of a save, kept while its file is unchanged; NULL when it has none or it is unreadable. */
static const preview_slot_t* preview_of(const char* path) {
    char file[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info;
    preview_slot_t* slot;
    if (!save_preview_path(path, file, sizeof file) || !GetFileAttributesExA(file, GetFileExInfoStandard, &info)) return NULL;
    for (int i = 0; i < PREVIEW_SLOTS; i++) {
        slot = &P.slots[i];
        if (slot->used && !_stricmp(slot->path, path) && !CompareFileTime(&slot->written, &info.ftLastWriteTime) &&
            slot->size == info.nFileSizeLow)
            return slot->image.pixels ? slot : NULL;
    }
    slot = &P.slots[P.next];
    P.next = (P.next + 1) % PREVIEW_SLOTS;
    save_preview_free(&slot->image);
    strncpy(slot->path, path, sizeof slot->path - 1);
    slot->path[sizeof slot->path - 1] = 0;
    slot->written = info.ftLastWriteTime;
    slot->size = info.nFileSizeLow;
    slot->used = 1;
    if (!save_preview_load(path, &slot->image)) log_limited("a save picture could not be read, the game's thumbnail is shown: %s", file);
    return slot->image.pixels ? slot : NULL;
}

static const sp_image_t* frozen_preview(void) {
    const uint16_t* bits;
    int w, h;
    long pitch;
    uint32_t hash;
    if (!ui_frozen_picture(&bits, &w, &h, &pitch)) return NULL;
    hash = fe_hash_pixels(bits, w, h, (int)pitch, 13);
    if (P.frozen.pixels && P.frozen_bits == bits && P.frozen_hash == hash) return &P.frozen;
    save_preview_free(&P.frozen);
    P.frozen_bits = bits;
    P.frozen_hash = hash;
    return save_preview_shrink(bits, w, h, pitch, &P.frozen) ? &P.frozen : NULL;
}

/* The selected save's HD picture, else the game's own thumbnail; in Save with nothing selected, the picture a new save
 * will get. */
static int add_photo(reader_t* r) {
    char path[MAX_PATH];
    const preview_slot_t* slot = NULL;
    const sp_image_t* still = NULL;
    int chosen = selected_save(r, path, sizeof path);
    fe_rect_t frame = fe_layout_photo((fe_window_kind_t)r->wc->kind, r->s->windows[r->s->window_count - 1].rect);
    fe_widget_t* w = fe_scene_widget(r->s, FE_W_IMAGE, PREVIEW_ID, frame);
    if (!w) return 0;
    w->style = FE_IMAGE_PHOTO;
    if (chosen) slot = preview_of(path);
    else if (r->wc->kind == FE_WND_SAVE) still = frozen_preview();
    if (slot || still) {
        const sp_image_t* image = slot ? &slot->image : still;
        w->image32 = image->pixels;
        w->image_w = image->width;
        w->image_h = image->height;
        w->image_pitch = image->width;
        w->image_hash = slot ? fe_hash(fe_hash(0, slot->path, strlen(slot->path)), &slot->written, sizeof slot->written)
                             : P.frozen_hash;
    } else if (chosen && r->thumb) {
        w->image = r->thumb;
        w->image_w = r->thumb_w;
        w->image_h = r->thumb_h;
        w->image_pitch = r->thumb_pitch;
        w->image_hash = fe_hash_pixels(r->thumb, r->thumb_w, r->thumb_h, r->thumb_pitch, 1);
    }
    return 1;
}

static void release_pictures(void) {
    for (int i = 0; i < PREVIEW_SLOTS; i++) {
        save_preview_free(&P.slots[i].image);
        P.slots[i].used = 0;
    }
    save_preview_free(&P.frozen);
    P.frozen_bits = NULL;
}

void frontend_hd_save_opened(const char* path, const char* user_dir) {
    const uint16_t* bits;
    int w, h;
    long pitch;
    LARGE_INTEGER t0, t1, freq;
    if (!path || !save_preview_is_save(path, user_dir, save_base())) return;
    forget_preview(path);
    QueryPerformanceCounter(&t0);
    if (ui_frozen_picture(&bits, &w, &h, &pitch) && save_preview_write(path, bits, w, h, pitch)) {
        sp_size_t size = save_preview_size(w, h);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        if (P.written_logged++ < 4)
            host_log("frontend", "HD picture of %s: %dx%d from the %dx%d game picture in %.1f ms", path, size.width,
                     size.height, w, h, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    } else if (!P.failed_logged++) {
        host_log("frontend", "warning: no HD picture for %s (the save itself is unaffected; Load shows its thumbnail)", path);
    }
}

void frontend_hd_save_deleted(const char* path, const char* user_dir) {
    if (!path || !save_preview_is_save(path, user_dir, save_base())) return;
    forget_preview(path);
    save_preview_delete(path);
}

/* The main screen is never centred: its plaques get their sizes when the frontend first draws it. */
static void edit_main_screen(const reader_t* r) {
    static int logged;
    int lengths[4];
    fe_rect_t rects[4];
    for (int k = 0; k < 4; k++) {
        const char* label = button_label(r->wc, 1000 + k);
        lengths[k] = label ? (int)strlen(label) : 8;
    }
    fe_layout_main_menu(lengths, 4, rects);
    for (int k = 0; k < 4; k++) {
        uint32_t c = find_control(r->window, 1000 + k, VT_BUTTON);
        fe_rect_t now;
        if (c && read_rect(c + CTRL_RECT, &now) && !fe_rect_equal(now, rects[k]) && write_rect(c, rects[k]) && !logged++)
            host_log("frontend", "main menu: plaques sized to their labels, the game's buttons moved with them");
    }
}

static int read_window(reader_t* r, uint32_t window, int top) {
    uint32_t controls[MAX_CONTROLS];
    unsigned count = 0;
    fe_rect_t rect;
    fe_window_t* w;
    if (!read_rect(window + WND_RECT, &rect)) return 0;
    w = fe_scene_window(r->s, (fe_window_kind_t)r->wc->kind, rect);
    if (!w) return 0;
    if (r->wc->title) w->title = fe_scene_text(r->s, r->wc->title);
    if (r->wc->kind == FE_WND_MAIN) {
        const uint16_t* bits;
        int bw, bh, pitch;
        if (!read_image(peek32(window + WND_BACKGROUND), &bits, &bw, &bh, &pitch) || bw != FE_CANVAS_W || bh != FE_CANVAS_H) {
            refuse("the main screen's art cannot be read");
            return 0;
        }
        r->s->backdrop = bits;
        r->s->backdrop_pitch = pitch;
        r->s->backdrop_hash = fe_hash_pixels(bits, bw, bh, pitch, 7);
    }
    if (top && r->wc->kind != FE_WND_GAME) {
        if (r->wc->kind == FE_WND_MAIN) edit_main_screen(r);
        if (!read_controls(window, controls, &count)) return 0;
        r->arrow_count = r->hotspot_count = 0;
        for (unsigned i = 0; i < count; i++)
            if (peek32(controls[i]) == VT_LISTBOX && r->arrow_count + 2 <= (int)(sizeof r->arrows / sizeof r->arrows[0])) {
                r->arrows[r->arrow_count++] = peek32(controls[i] + LIST_UP);
                r->arrows[r->arrow_count++] = peek32(controls[i] + LIST_DOWN);
            }
        for (unsigned i = 0; i < count; i++)
            if (!add_control(r, controls[i])) return 0;
        mark_row_hover(r, w);
        if ((r->wc->kind == FE_WND_LOAD || r->wc->kind == FE_WND_SAVE) && !add_photo(r)) return 0;
        if (r->wc->kind == FE_WND_AUDIO) {
            fe_scene_tidy_check_rows(r->s, w);
            add_volume_caption(r->s, w);
        }
    }
    fe_scene_finish_window(r->s, w);
    return 1;
}

int frontend_hd_read_scene(fe_scene_t* s, int screen_w, int screen_h) {
    uint32_t main_wnd, manager, begin, end, stack[MAX_STACK];
    unsigned depth;
    reader_t r;
    fe_scene_reset(s, screen_w, screen_h);
    memset(&r, 0, sizeof r);
    r.s = s;
    if (!read32(MAIN_WINDOW_OBJECT, &main_wnd) || !read32(main_wnd + MANAGER_OFFSET, &manager) ||
        !read32(manager + MGR_STACK_BEGIN, &begin) || !read32(manager + MGR_STACK_END, &end) || end <= begin ||
        (end - begin) % 4 || (end - begin) / 4 > MAX_STACK || !guest_readable(begin, end - begin))
        return 0;
    depth = (end - begin) / 4;
    for (unsigned i = 0; i < depth; i++) stack[i] = ((const uint32_t*)(uintptr_t)begin)[i];
    r.cursor_x = (int32_t)peek32(manager + MGR_CURSOR_X);
    r.cursor_y = (int32_t)peek32(manager + MGR_CURSOR_Y);
    s->cursor_x = r.cursor_x;
    s->cursor_y = r.cursor_y;
    for (unsigned i = 0; i < depth; i++) {
        r.window = stack[i];
        r.wc = window_class(peek32(stack[i]));
        if (!r.wc) {
            refuse(guest_ui_class_name(peek32(stack[i])));
            return 0;
        }
        if (r.wc->kind == FE_WND_GAME && i + 1 == depth) {
            refuse("the gameplay view is in front");
            return 0;
        }
        if (!read_window(&r, stack[i], i + 1 == depth)) return 0;
    }
    return 1;
}

static int pointer_shown(void) {
    uint32_t main_wnd = peek32(MAIN_WINDOW_OBJECT), manager = main_wnd ? peek32(main_wnd + MANAGER_OFFSET) : 0, index;
    if (!manager || !peek32(manager + MGR_CURSOR_SHOWN)) return 0;
    index = peek32(manager + MGR_CURSOR_INDEX);
    return index < 16 && peek32(manager + MGR_CURSOR_IMAGES + 4 * index) != 0;
}

static int ensure_painter(void) {
    SYSTEM_INFO si;
    if (F.painter) return 1;
    if (F.painter_failed) return 0;
    F.painter = fe_painter_create();
    if (!F.painter) {
        F.painter_failed = 1;
        host_log("frontend", "the HD menus could not start (fonts or memory): the game's own menus are shown");
        return 0;
    }
    GetSystemInfo(&si);
    F.threads = si.dwNumberOfProcessors > 8 ? 8 : (int)si.dwNumberOfProcessors;
    if (F.threads < 1) F.threads = 1;
    host_log("frontend", "HD menus: plaques \"%s\", labels \"%s\", text \"%s\", %d painting threads",
             fe_painter_face(F.painter, FE_FONT_PLAQUE), fe_painter_face(F.painter, FE_FONT_LABEL),
             fe_painter_face(F.painter, FE_FONT_BODY), F.threads);
    return 1;
}

static int ensure_layer(int w, int h) {
    if (F.layer && F.layer_w == w && F.layer_h == h) return 1;
    free(F.layer);
    F.layer = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
    F.layer_w = F.layer ? w : 0;
    F.layer_h = F.layer ? h : 0;
    F.have_previous = 0;
    return F.layer != NULL;
}

static void update_pointer(const fe_scene_t* s) {
    fe_view_t v = fe_view_make(s->screen_w, s->screen_h);
    int x, y;
    if (v.sy != F.cursor_scale) {
        int size = fe_cursor_size(v.sy);
        if (size > 96) size = 96;
        fe_paint_cursor(size == fe_cursor_size(v.sy) ? v.sy : v.sy * 96.0 / fe_cursor_size(v.sy), F.cursor, size,
                        &F.cursor_hot_x, &F.cursor_hot_y);
        F.cursor_size = size;
        F.cursor_scale = v.sy;
        F.cursor_generation++;
    }
    if (!pointer_shown() || !window_host_pointer(&x, &y)) {
        d3dr_set_frontend_pointer(NULL, 0, 0, 0, 0);
        return;
    }
    d3dr_set_frontend_pointer(F.cursor, F.cursor_size, F.cursor_generation, x - F.cursor_hot_x, y - F.cursor_hot_y);
}

static void release_all(void) {
    free(F.layer);
    F.layer = NULL;
    F.layer_w = F.layer_h = 0;
    F.have_previous = 0;
    fe_painter_trim(F.painter);
    release_pictures();
    d3dr_frontend_release();
}

int frontend_hd_frame(int screen_w, int screen_h) {
    fe_scene_t* now = &F.scenes[F.current ^ 1];
    const fe_scene_t* before = F.have_previous ? &F.scenes[F.current] : NULL;
    fe_rect_t dirty, clip;
    RECT box;
    int diff;
    LARGE_INTEGER t0, t1, freq;
    F.idle_frames = 0;
    if (d3dr_frontend_failed()) {
        refuse("the renderer could not show the HD layer");
        return 0;
    }
    if (screen_w <= 0 || screen_h <= 0 || !ensure_painter()) return 0;
    if (!frontend_hd_read_scene(now, screen_w, screen_h)) {
        if (!F.last_refusal[0]) refuse("a window the frontend could not read");
        return 0;
    }
    if (!ensure_layer(screen_w, screen_h)) {
        refuse("no memory for the HD layer");
        return 0;
    }
    diff = fe_scene_diff(F.have_previous ? before : NULL, now, &dirty);
    if (diff == FE_DIFF_ALL) clip = fe_rect(0, 0, screen_w, screen_h);
    else if (diff == FE_DIFF_RECT) {
        fe_view_t v = fe_view_make(screen_w, screen_h);
        clip = fe_rect_intersect(fe_view_rect(&v, dirty), fe_rect(0, 0, screen_w, screen_h));
    } else clip = fe_rect(0, 0, 0, 0);
    QueryPerformanceCounter(&t0);
    if (!fe_rect_empty(clip) && !fe_paint(F.painter, now, F.layer, screen_w, clip, F.threads)) {
        refuse("the HD layer could not be painted");
        F.have_previous = 0;
        return 0;
    }
    QueryPerformanceCounter(&t1);
    if (diff == FE_DIFF_ALL && F.sizes_logged < 4) {
        QueryPerformanceFrequency(&freq);
        F.sizes_logged++;
        host_log("frontend", "HD menus at %dx%d (canvas scale %.3f): painted in %.1f ms", screen_w, screen_h,
                 fe_view_make(screen_w, screen_h).sy, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    }
    F.current ^= 1;
    F.have_previous = 1;
    F.last_refusal[0] = 0;
    box.left = clip.left;
    box.top = clip.top;
    box.right = clip.right;
    box.bottom = clip.bottom;
    d3dr_set_frontend(F.layer, screen_w, screen_h, fe_rect_empty(clip) ? NULL : &box);
    update_pointer(now);
    F.shown = 1;
    return 1;
}

void frontend_hd_frame_done(void) {
    d3dr_set_frontend(NULL, 0, 0, NULL);
    d3dr_set_frontend_pointer(NULL, 0, 0, 0, 0);
}

void frontend_hd_idle(void) {
    if (!F.shown || ++F.idle_frames < IDLE_FRAMES) return;
    F.shown = 0;
    release_all();
}

static uint32_t find_control(uint32_t window, int32_t id, uint32_t vtable) {
    uint32_t controls[MAX_CONTROLS];
    unsigned count;
    if (!read_controls(window, controls, &count)) return 0;
    for (unsigned i = 0; i < count; i++)
        if ((int32_t)peek32(controls[i] + CTRL_ID) == id && (!vtable || peek32(controls[i]) == vtable)) return controls[i];
    return 0;
}

static int write_rect(uint32_t control, fe_rect_t r) {
    return guest_write32(control + CTRL_RECT, (uint32_t)r.left) && guest_write32(control + CTRL_RECT + 4, (uint32_t)r.top) &&
           guest_write32(control + CTRL_RECT + 8, (uint32_t)r.right) &&
           guest_write32(control + CTRL_RECT + 12, (uint32_t)r.bottom);
}

/* Not drawn, never hovered or clicked: the game's hit test needs the visible bit, and its UI has no keyboard focus. */
static int hide(uint32_t control) {
    uint32_t flags;
    return control && read32(control + CTRL_FLAGS, &flags) &&
           guest_write32(control + CTRL_FLAGS, flags & ~(CTRL_VISIBLE | CTRL_ENABLED));
}

static void move_by(uint32_t control, int dy) {
    fe_rect_t r;
    if (control && dy && read_rect(control + CTRL_RECT, &r)) write_rect(control, fe_rect_offset(r, 0, dy));
}

static void edit_options(uint32_t window, fe_rect_t wr) {
    static const int32_t order[] = { 1002, 1000, 1003, 1004 };
    static int logged;
    int32_t ids[4];
    int widths[4], heights[4], n = 0;
    uint32_t controls[4];
    fe_rect_t rects[4];
    uint32_t video = find_control(window, OPTIONS_VIDEO_ID, VT_BUTTON);
    if (!video || !hide(video)) return;
    for (int i = 0; i < 4; i++) {
        uint32_t c = find_control(window, order[i], VT_BUTTON);
        fe_rect_t r;
        if (!c || !(peek32(c + CTRL_FLAGS) & CTRL_VISIBLE) || !read_rect(c + CTRL_RECT, &r)) continue;
        controls[n] = c;
        ids[n] = order[i];
        widths[n] = r.right - r.left;
        heights[n] = r.bottom - r.top;
        n++;
    }
    fe_layout_options(ids, widths, heights, n, wr.right - wr.left, wr.bottom - wr.top, rects);
    for (int i = 0; i < n; i++) write_rect(controls[i], fe_rect_offset(rects[i], wr.left, wr.top));
    if (!logged++) host_log("frontend", "Options: the legacy Video entry is hidden, %d entries laid out again", n);
}

/* A row of the audio options: its check box, the hotspot over it and the label text box beside it. */
static uint32_t row_label(uint32_t window, fe_rect_t check) {
    uint32_t controls[MAX_CONTROLS];
    unsigned count;
    if (!read_controls(window, controls, &count)) return 0;
    for (unsigned i = 0; i < count; i++) {
        fe_rect_t r;
        int mid;
        if (peek32(controls[i]) != VT_TEXTBOX || !(peek32(controls[i] + CTRL_FLAGS) & CTRL_VISIBLE) ||
            !read_rect(controls[i] + CTRL_RECT, &r))
            continue;
        mid = (r.top + r.bottom) / 2;
        if (r.left >= check.left && mid >= check.top - 3 && mid <= check.top + AUDIO_ROW_H + 3) return controls[i];
    }
    return 0;
}

static void edit_audio(uint32_t window) {
    static int logged;
    uint32_t check3d = find_control(window, AUDIO_3D_CHECK, VT_CHECKBOX), first = find_control(window, AUDIO_FIRST_CHECK, VT_CHECKBOX);
    fe_rect_t r3d, r0;
    int tops[AUDIO_ROWS];
    if (!check3d || !first || !read_rect(check3d + CTRL_RECT, &r3d) || !read_rect(first + CTRL_RECT, &r0)) return;
    if (peek32(check3d + CTRL_FLAGS) & CTRL_VISIBLE) {
        hide(row_label(window, r3d));
        hide(find_control(window, AUDIO_3D_HOTSPOT, VT_HOTSPOT));
        hide(check3d);
    }
    fe_layout_rows(AUDIO_ROWS, r0.top, r3d.top + AUDIO_ROW_H, AUDIO_ROW_H, tops);
    {
        uint32_t checks[AUDIO_ROWS], labels[AUDIO_ROWS], hotspots[AUDIO_ROWS];
        int dy[AUDIO_ROWS];
        for (int k = 0; k < AUDIO_ROWS; k++) {
            fe_rect_t rc;
            checks[k] = find_control(window, AUDIO_FIRST_CHECK + k, VT_CHECKBOX);
            labels[k] = hotspots[k] = 0;
            dy[k] = 0;
            if (!checks[k] || !read_rect(checks[k] + CTRL_RECT, &rc)) continue;
            labels[k] = row_label(window, rc);
            hotspots[k] = find_control(window, AUDIO_FIRST_HOTSPOT + k, VT_HOTSPOT);
            dy[k] = tops[k] - rc.top;
        }
        for (int k = 0; k < AUDIO_ROWS; k++) {
            move_by(labels[k], dy[k]);
            move_by(hotspots[k], dy[k]);
            move_by(checks[k], dy[k]);
        }
    }
    if (!logged++)
        host_log("frontend", "Audio: \"3D Audio Accel. Enabled\" is hidden (no hardware 3D voices since Windows Vista); "
                             "the other rows are spread over its space");
}

void frontend_hd_window_centred(uint32_t window) {
    uint32_t vt;
    fe_rect_t wr;
    if (!window || !read32(window, &vt) || !read_rect(window + WND_RECT, &wr)) return;
    if (vt == VT_OPTIONS) edit_options(window, wr);
    else if (vt == VT_AUDIO) edit_audio(window);
}
