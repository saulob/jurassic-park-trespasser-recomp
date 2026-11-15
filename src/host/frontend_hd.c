/*
 * The HD frontend. While the game shows its menus on the UI canvas (ui_canvas.c), its windows and controls are read
 * every UI frame and drawn again at the screen's resolution (frontend_assets.c). The game keeps all of its own rules:
 * hit tests, hover, values and callbacks stay in its objects, and its canvas is shown instead whenever a window or a
 * control is not one the frontend knows.
 */
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend_art.h"
#include "frontend_assets.h"
#include "frontend_hd.h"
#include "frontend_plaque.h"
#include "gamepad.h"
#include "jpt_tr.h"
#include "l10n.h"
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
#define VT_DIRECT_LOAD 0x00646888u
#define VT_CONTROLS_WND 0x00645F38u

#define BUTTON_STATE 0x28u
#define BUTTON_IMAGES 0x2Cu
#define CHECK_IMAGES 0x2Cu
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
#define TEXT_BK 0x2Cu
#define TEXT_FG 0x30u
#define TEXT_SHADOW_COLOR 0x34u
#define TEXT_SHADOW_OFFSET 0x38u
#define TEXT_WEIGHT 0x50u
#define TEXT_DT 0x3Cu
#define TEXT_SHADOW 0x40u
#define TEXT_BORDER 0x44u
#define TEXT_TRANSPARENT 0x48u
#define TEXT_POINTS 0x4Eu
#define EDIT_TEXT 0x24u
#define EDIT_FG 0x3Cu
#define EDIT_POINTS 0x48u
#define EDIT_SHADOW_COLOR 0x58u
#define EDIT_SHADOW_OFFSET 0x5Cu
#define EDIT_SHADOW 0x60u
#define EDIT_BG 0x38u
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
#define LIST_BG 0x64u
#define LIST_FG 0x68u
#define LIST_SHADOW_COLOR 0x78u
#define LIST_SHADOW_OFFSET 0x7Cu
#define LIST_POINTS 0x6Cu
#define LIST_ITEM_SIZE 12u
#define LIST_SCROLLBAR 0x0Cu
#define STATIC_TRANSPARENT 0x20u
#define STATIC_KEY 0x22u
#define STATIC_IMAGE 0x24u

#define CURSOR_MAX 128
#define CURSOR_SHARE 0.7
#define MAX_STACK 16u
#define MAX_CONTROLS 160u
#define TEXT_CAP 512
#define IDLE_FRAMES 120
#define LOG_LIMIT 8
#define TITLE_PICTURE_W 320
#define TITLE_PICTURE_H 200
#define TITLE_QUIT_ZONE 108, 14, 216, 40
#define TITLE_QUIT_ROOM 96, 13, 234, 41
#define TITLE_SOUND_ZONE 26, 22, 130, 43
#define TITLE_SOUND_ROOM 24, 20, 132, 44
#define FOOTER_LEFT 66
#define FOOTER_RIGHT 566
#define FOOTER_AREA FOOTER_LEFT, 452, FOOTER_RIGHT, 480

typedef struct {
    uint32_t vtable;
    uint8_t kind;
} window_class_t;

static const window_class_t k_windows[] = {
    { 0x00645C3Cu, FE_WND_GAME },    { 0x006466B8u, FE_WND_MAIN },    { VT_OPTIONS, FE_WND_OPTIONS },
    { 0x00646B40u, FE_WND_GAME_MENU }, { VT_AUDIO, FE_WND_AUDIO },   { 0x00645F38u, FE_WND_CONTROLS },
    { 0x006460D0u, FE_WND_LOAD },    { 0x006462A8u, FE_WND_SAVE },    { 0x00646970u, FE_WND_QUIT },
    { 0x00645D34u, FE_WND_MESSAGE }, { 0x00645E24u, FE_WND_MESSAGE }, { 0x006461B8u, FE_WND_MESSAGE },
    { 0x00646390u, FE_WND_MESSAGE }, { 0x00646480u, FE_WND_MESSAGE }, { 0x00646D10u, FE_WND_MESSAGE },
    { VT_DIRECT_LOAD, FE_WND_DIRECT_LOAD },
};

#define PREVIEW_ID 1003
#define AUDIO_3D_CHECK 1025
#define AUDIO_3D_HOTSPOT 1105
#define AUDIO_FIRST_CHECK 1020
#define AUDIO_FIRST_HOTSPOT 1100
#define AUDIO_ROWS 5
#define AUDIO_ROW_H 15
#define OPTIONS_VIDEO_ID 1001
#define CONTROLS_FIRST_FIELD 1030

static struct {
    fe_painter_t* painter;
    int painter_failed, threads;
    fe_scene_t scenes[2];
    int current, have_previous;
    uint32_t* layer;
    int layer_w, layer_h;
    uint32_t cursor[CURSOR_MAX * CURSOR_MAX];
    uint32_t cursor_raster;
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
    int cursor_x, cursor_y;
    int selected_item; /* the list's selected row, -1 for none */
    char selected_text[64];
} reader_t;

static int is_arrow(const reader_t* r, uint32_t control, int* down) {
    for (int i = 0; i < r->arrow_count; i++)
        if (r->arrows[i] == control) {
            *down = i & 1;
            return 1;
        }
    return 0;
}

static uint32_t image_hash(const uint16_t* bits, int w, int h, int pitch) {
    return fe_hash_pixels(bits, w, h, pitch, w * h > 20000 ? 5 : 1);
}

/* The game's picture in a raster, drawn at the widget's corner; keyed by its top-left pixel as the game does. */
static int set_picture(fe_widget_t* w, uint32_t raster, int keyed) {
    const uint16_t* bits;
    int iw, ih, pitch;
    if (!read_image(raster, &bits, &iw, &ih, &pitch)) return 0;
    w->image = bits;
    w->image_w = iw;
    w->image_h = ih;
    w->image_pitch = pitch;
    w->image_key = bits[0];
    w->image_keyed = keyed != 0;
    w->image_hash = image_hash(bits, iw, ih, pitch);
    return 1;
}

/* CUIButton::Draw (0x004036D0): the image slot a state selects (0x004035D0). */
static uint32_t button_slot(uint32_t flags, uint32_t state) {
    if (!(flags & CTRL_ENABLED)) return 3;
    if (state & BUTTON_PRESSED) return 2;
    if (state & BUTTON_HOVER) return 1;
    return 0;
}

/* ---- translated labels: the English lettering of the game's pictures replaced by the language's */

typedef struct {
    uint8_t window;
    int32_t id;
    int w, h; /* the retail picture's size; 0 for any */
    const char* key;
} picture_label_t;

/* Message boxes share their IDs (yes/no, ok): the retail picture's size tells the buttons apart. */
static const picture_label_t k_button_labels[] = {
    { FE_WND_MAIN, 1000, 0, 0, "Menu::NEW GAME" },     { FE_WND_MAIN, 1001, 0, 0, "Menu::LOAD" },
    { FE_WND_MAIN, 1002, 0, 0, "Menu::OPTIONS" },      { FE_WND_MAIN, 1003, 0, 0, "Menu::EXIT" },
    { FE_WND_GAME_MENU, 1000, 0, 0, "Menu::QUIT" },    { FE_WND_GAME_MENU, 1001, 0, 0, "Menu::RESUME" },
    { FE_WND_GAME_MENU, 1002, 0, 0, "Menu::RESTART LEVEL" }, { FE_WND_GAME_MENU, 1003, 0, 0, "Menu::SAVE" },
    { FE_WND_GAME_MENU, 1004, 0, 0, "Menu::LOAD" },    { FE_WND_GAME_MENU, 1005, 0, 0, "Menu::OPTIONS" },
    { FE_WND_QUIT, 1000, 0, 0, "Menu::CANCEL" },       { FE_WND_QUIT, 1001, 0, 0, "Menu::DESKTOP" },
    { FE_WND_QUIT, 1002, 0, 0, "Menu::MAIN MENU" },    { FE_WND_OPTIONS, 1000, 0, 0, "Menu::CONTROLS" },
    { FE_WND_OPTIONS, 1001, 0, 0, "Menu::VIDEO" },     { FE_WND_OPTIONS, 1002, 0, 0, "Menu::AUDIO" },
    { FE_WND_OPTIONS, 1003, 0, 0, "Menu::CREDITS" },   { FE_WND_OPTIONS, 1004, 0, 0, "Menu::CLOSE" },
    { FE_WND_AUDIO, 1000, 0, 0, "Menu::CANCEL" },      { FE_WND_AUDIO, 1001, 0, 0, "Menu::OK" },
    { FE_WND_CONTROLS, 1000, 0, 0, "Menu::CANCEL" },   { FE_WND_CONTROLS, 1001, 0, 0, "Menu::OK" },
    { FE_WND_CONTROLS, 1002, 0, 0, "Menu::DEFAULTS" }, { FE_WND_LOAD, 1000, 0, 0, "Menu::CANCEL" },
    { FE_WND_LOAD, 1001, 0, 0, "Menu::OK" },           { FE_WND_LOAD, 1004, 0, 0, "Menu::DELETE" },
    { FE_WND_SAVE, 1000, 0, 0, "Menu::CANCEL" },       { FE_WND_SAVE, 1001, 0, 0, "Menu::OK" },
    { FE_WND_SAVE, 1005, 0, 0, "Menu::DELETE" },       { FE_WND_DIRECT_LOAD, 1000, 0, 0, "Menu::CANCEL" },
    { FE_WND_DIRECT_LOAD, 1001, 0, 0, "Menu::OK" },    { FE_WND_MESSAGE, -1, 80, 42, "Menu::YES" },
    { FE_WND_MESSAGE, -1, 65, 42, "Menu::NO" },        { FE_WND_MESSAGE, -1, 62, 42, "Menu::OK" },
    { FE_WND_MESSAGE, -1, 118, 42, "Menu::CANCEL" },
};

/* The engraved titles of window backgrounds (retail 320x200 pictures): where their plate's letters are. */
typedef struct {
    uint8_t window;
    fe_rect_t zone, room;
    const char* key;
} title_label_t;

static const title_label_t k_titles[] = {
    { FE_WND_QUIT, { TITLE_QUIT_ZONE }, { TITLE_QUIT_ROOM }, "Title::QUIT" },
    { FE_WND_AUDIO, { TITLE_SOUND_ZONE }, { TITLE_SOUND_ROOM }, "Title::SOUND" },
};

static const char* button_label(uint8_t window, int32_t id, int w, int h) {
    for (size_t i = 0; i < sizeof k_button_labels / sizeof k_button_labels[0]; i++) {
        const picture_label_t* l = &k_button_labels[i];
        if (l->window == window && (l->id == id || (l->id < 0 && l->w == w && l->h == h))) return l->key;
    }
    return NULL;
}

#define LABEL_CACHE 48
#define LABEL_STATES 4

typedef struct {
    const uint16_t* source;
    uint32_t hash;
    int failed;
    fp_plaque_t plaque;
    uint16_t* states[LABEL_STATES]; /* the clean picture in the tones of each state's own picture */
    uint32_t state_hash[LABEL_STATES];
} label_entry_t;

static struct {
    label_entry_t entries[LABEL_CACHE];
    int next, made, failed;
} LB;

static void release_labels(void) {
    for (int i = 0; i < LABEL_CACHE; i++) {
        fp_free(&LB.entries[i].plaque);
        for (int k = 1; k < LABEL_STATES; k++) free(LB.entries[i].states[k]);
    }
    memset(&LB, 0, sizeof LB);
}

static label_entry_t* label_entry(const uint16_t* bits, uint32_t hash) {
    label_entry_t* e;
    for (int i = 0; i < LABEL_CACHE; i++)
        if (LB.entries[i].source == bits && LB.entries[i].hash == hash) return &LB.entries[i];
    e = &LB.entries[LB.next];
    LB.next = (LB.next + 1) % LABEL_CACHE;
    fp_free(&e->plaque);
    for (int k = 1; k < LABEL_STATES; k++) free(e->states[k]);
    memset(e, 0, sizeof *e);
    e->source = bits;
    e->hash = hash;
    return e;
}

static void source_of(fa_source_t* s, const uint16_t* bits, int w, int h, int pitch, int keyed) {
    memset(s, 0, sizeof *s);
    s->bits = bits;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->keyed = keyed;
    s->key = bits[0];
}

static void set_label(fe_scene_t* s, fe_widget_t* w, const label_entry_t* e, int slot, const char* key, uint8_t kind) {
    w->image = e->states[slot];
    w->image_pitch = e->plaque.w;
    w->image_hash = e->state_hash[slot];
    w->normal_image = NULL;
    w->label = kind;
    w->text = fe_scene_text(s, jpt_tr_cp(key));
    w->color = e->plaque.ink;
    w->label_box = fe_rect_offset(e->plaque.letters, w->rect.left, w->rect.top);
    w->label_room = fe_rect_offset(e->plaque.room, w->rect.left, w->rect.top);
}

static void log_label(const char* key, const label_entry_t* e) {
    if (e->failed) {
        if (LB.failed++ < LOG_LIMIT)
            host_log("frontend", "\"%s\": its picture's lettering was not found, the picture is shown as it is", key);
    } else if (LB.made++ < LOG_LIMIT) {
        host_log("frontend", "\"%s\": lettering %d,%d-%d,%d taken out, \"%s\" set in its place", key, e->plaque.letters.left,
                 e->plaque.letters.top, e->plaque.letters.right, e->plaque.letters.bottom, jpt_tr(key));
    }
}

/* A button whose picture spells an English word: the word is taken out of every state's picture and the language's
 * label is drawn over it. The button keeps its own pictures when the lettering cannot be found. */
static void relabel_button(reader_t* r, uint32_t c, fe_widget_t* w, uint32_t slot) {
    const uint16_t *normal, *hover, *own;
    int nw, nh, np, hw, hh, hp, ow, oh, op;
    const char* key = button_label(r->wc->kind, w->id, w->image_w, w->image_h);
    label_entry_t* e;
    fa_source_t a, b;
    if (!key || !jpt_tr_has(key) || slot >= LABEL_STATES) return;
    if (!read_image(peek32(c + BUTTON_IMAGES), &normal, &nw, &nh, &np) ||
        !read_image(peek32(c + BUTTON_IMAGES + 4), &hover, &hw, &hh, &hp) || hw != nw || hh != nh)
        return;
    e = label_entry(normal, image_hash(normal, nw, nh, np));
    if (!e->plaque.clean && !e->failed) {
        source_of(&a, normal, nw, nh, np, 1);
        source_of(&b, hover, hw, hh, hp, 1);
        e->failed = !fp_button(&a, &b, &e->plaque);
        if (!e->failed) {
            e->states[0] = e->plaque.clean;
            e->state_hash[0] = fe_hash(e->hash, "clean", 5);
        }
        log_label(key, e);
    }
    if (e->failed) return;
    if (!e->states[slot]) {
        if (!read_image(peek32(c + BUTTON_IMAGES + 4 * slot), &own, &ow, &oh, &op) || ow != nw || oh != nh) return;
        e->states[slot] = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)nw * (size_t)nh);
        source_of(&a, own, ow, oh, op, 1);
        if (!e->states[slot] || !fp_tone(&e->plaque, &a, e->states[slot])) {
            free(e->states[slot]);
            e->states[slot] = NULL;
            return;
        }
        e->state_hash[slot] = fe_hash(e->hash, &slot, sizeof slot);
    }
    w->image_key = e->plaque.clean[0];
    set_label(r->s, w, e, (int)slot, key, FE_LABEL_STENCIL);
}

/* A window background with an engraved English title. */
static void relabel_static(reader_t* r, fe_widget_t* w) {
    label_entry_t* e;
    fa_source_t a;
    for (size_t i = 0; i < sizeof k_titles / sizeof k_titles[0]; i++) {
        const title_label_t* t = &k_titles[i];
        if (t->window != r->wc->kind || w->image_w != TITLE_PICTURE_W || w->image_h != TITLE_PICTURE_H ||
            !jpt_tr_has(t->key))
            continue;
        e = label_entry(w->image, w->image_hash);
        if (!e->plaque.clean && !e->failed) {
            source_of(&a, w->image, w->image_w, w->image_h, w->image_pitch, w->image_keyed);
            e->failed = !fp_engraved(&a, t->zone, t->room, &e->plaque);
            if (!e->failed) {
                e->states[0] = e->plaque.clean;
                e->state_hash[0] = fe_hash(e->hash, "title", 5);
            }
            log_label(t->key, e);
        }
        if (!e->failed) set_label(r->s, w, e, 0, t->key, FE_LABEL_ENGRAVED);
        return;
    }
}

/* The main screen's legal lines, taken out of its backdrop and set again in the language's wording. */
static void relabel_backdrop(fe_scene_t* s) {
    static const char* const k_footer = "Footer::\xC2\xA9 1998 DreamWorks Interactive L.L.C. Trespasser is a trademark of UCS & "
                                        "Amblin.\nThe Lost World: Jurassic Park\xE2\x84\xA2 & \xC2\xA9 1998 UCS & Amblin.  "
                                        "All Rights Reserved.";
    label_entry_t* e;
    fa_source_t a;
    if (!jpt_tr_has(k_footer)) return;
    e = label_entry(s->backdrop, s->backdrop_hash);
    if (!e->plaque.clean && !e->failed) {
        source_of(&a, s->backdrop, FE_CANVAS_W, FE_CANVAS_H, s->backdrop_pitch, 0);
        e->failed = !fp_light(&a, fe_rect(FOOTER_AREA), &e->plaque) || e->plaque.lines != 2;
        log_label("Footer", e);
    }
    if (e->failed) return;
    s->backdrop = e->plaque.clean;
    s->backdrop_pitch = FE_CANVAS_W;
    s->backdrop_hash = fe_hash(s->backdrop_hash, "footer", 6);
    s->footer_text = fe_scene_text(s, jpt_tr_cp(k_footer));
    s->footer_box = fe_rect(FOOTER_LEFT, e->plaque.letters.top, FOOTER_RIGHT, e->plaque.letters.bottom);
}

static int add_button(reader_t* r, uint32_t c, uint32_t flags, int32_t id, fe_rect_t rect) {
    uint32_t state = peek32(c + BUTTON_STATE), slot = button_slot(flags, state);
    uint32_t raster = peek32(c + BUTTON_IMAGES + 4 * slot);
    int down;
    fe_widget_t* w;
    if (!raster) return 1;
    w = fe_scene_widget(r->s, FE_W_BUTTON, id, rect);
    if (!w) return 0;
    if (!(flags & CTRL_ENABLED)) w->state = FE_ST_DISABLED;
    else w->state = (uint16_t)(((state & BUTTON_HOVER) ? FE_ST_HOVER : 0) | ((state & BUTTON_PRESSED) ? FE_ST_PRESSED : 0));
    w->style = is_arrow(r, c, &down) ? (down ? FE_BUTTON_ARROW_DOWN : FE_BUTTON_ARROW_UP) : FE_BUTTON_PLAQUE;
    if (!set_picture(w, raster, state & 1)) w->image = NULL;
    if (w->image && slot == 1 && w->style == FE_BUTTON_PLAQUE) {
        const uint16_t* bits;
        int iw, ih, pitch;
        if (read_image(peek32(c + BUTTON_IMAGES), &bits, &iw, &ih, &pitch) && iw == w->image_w && ih == w->image_h) {
            w->normal_image = bits;
            w->normal_pitch = pitch;
        }
    }
    if (w->image && w->style == FE_BUTTON_PLAQUE) relabel_button(r, c, w, slot);
    return 1;
}

/* CUICheckBox::Draw (0x00405890): the image checked * 2 + hover, opaque. */
static int add_check(reader_t* r, uint32_t c, uint32_t flags, int32_t id, fe_rect_t rect) {
    uint32_t state = peek32(c + CHECK_STATE);
    uint32_t index = ((state & CHECK_CHECKED) ? 2u : 0u) | ((state & CHECK_HOVER) ? 1u : 0u);
    fe_widget_t* w;
    if (!(flags & CTRL_ENABLED)) return 1;
    w = fe_scene_widget(r->s, FE_W_CHECK, id, rect);
    if (!w) return 0;
    w->state = (uint16_t)(((state & CHECK_HOVER) ? FE_ST_HOVER : 0) | ((state & CHECK_CHECKED) ? FE_ST_CHECKED : 0));
    if (!set_picture(w, peek32(c + CHECK_IMAGES + 4 * index), 0)) w->image = NULL;
    return 1;
}

/* CUISlider::Draw (0x004065D0): its track is part of the window's picture; the thumb is slider.tga. */
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
    if (!set_picture(w, thumb, 0)) w->image = NULL;
    return 1;
}

static void text_shadow(fe_widget_t* w, uint32_t enabled, uint32_t color, uint32_t offset) {
    if (!enabled || !offset || offset > 8) return;
    w->shadow_offset = (uint8_t)offset;
    w->shadow_color = colorref_rgb(color);
}

static int add_textbox(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    char text[TEXT_CAP];
    uint8_t points = 12, transparent = 1;
    uint32_t border = peek32(c + TEXT_BORDER);
    fe_widget_t* w;
    if (!guest_read_string(peek32(c + TEXT_STRING), text, sizeof text)) text[0] = 0;
    {
        const char* label = l10n_label(text);
        if (label) snprintf(text, sizeof text, "%s", label);
    }
    read8(c + TEXT_POINTS, &points);
    read8(c + TEXT_TRANSPARENT, &transparent);
    w = fe_scene_widget(r->s, border ? FE_W_FIELD : FE_W_LABEL, id, rect);
    if (!w) return 0;
    w->text = fe_scene_text(r->s, text);
    w->text_h = points;
    w->weight = (uint16_t)(peek32(c + TEXT_WEIGHT) & 0xFFFF);
    w->color = colorref_rgb(peek32(c + TEXT_FG));
    w->text_flags = (uint16_t)(text_flags(peek32(c + TEXT_DT), 0) | FE_TEXT_EXACT);
    w->border = border != 0;
    text_shadow(w, peek32(c + TEXT_SHADOW), peek32(c + TEXT_SHADOW_COLOR), peek32(c + TEXT_SHADOW_OFFSET));
    if (!transparent) {
        w->filled = 1;
        w->fill_color = colorref_rgb(peek32(c + TEXT_BK));
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
    w->text_flags = FE_TEXT_EXACT | FE_TEXT_VCENTER;
    w->caret = (int)(len - scroll);
    w->state = FE_ST_CARET;
    text_shadow(w, peek32(c + EDIT_SHADOW) & 0xFF, peek32(c + EDIT_SHADOW_COLOR), peek32(c + EDIT_SHADOW_OFFSET) & 0xFF);
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
    w->arrow_w = side > 0 && side <= 64 ? side : 16;
    w->arrow_w = side > 0 && side <= 64 ? side : 16;
    w->arrow_w = side > 0 && side <= 64 ? side : 16;
    w->arrow_w = side > 0 && side <= 64 ? side : 16;
    w->text_h = points;
    w->color = colorref_rgb(peek32(c + LIST_FG));
    w->selected_color = colorref_rgb(peek32(c + LIST_BG));
    text_shadow(w, 1, peek32(c + LIST_SHADOW_COLOR), peek32(c + LIST_SHADOW_OFFSET));
    w->text_flags = FE_TEXT_EXACT;
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
    if (w->selected >= 0 && w->selected < w->item_count &&
        guest_read_string(peek32(begin + (uint32_t)w->selected * LIST_ITEM_SIZE), r->selected_text, sizeof r->selected_text))
        r->selected_item = w->selected;
    return 1;
}

static int add_photo(reader_t* r, fe_rect_t at, uint32_t fallback_raster);

/* CUIStatic::Draw (0x00403AE0): the picture at the control's corner, keyed when the control says so. */
static int add_static(reader_t* r, uint32_t c, int32_t id, fe_rect_t rect) {
    uint8_t transparent = 1;
    fe_widget_t* w;
    uint32_t raster = peek32(c + STATIC_IMAGE);
    if (!raster) return 1;
    if ((r->wc->kind == FE_WND_LOAD || r->wc->kind == FE_WND_SAVE) && id == PREVIEW_ID) return add_photo(r, rect, raster);
    read8(c + STATIC_TRANSPARENT, &transparent);
    w = fe_scene_widget(r->s, FE_W_IMAGE, id, rect);
    if (!w) return 0;
    if (!set_picture(w, raster, transparent)) w->image = NULL;
    else if (id == -1) relabel_static(r, w);
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
    case VT_HOTSPOT: return 1;
    case VT_PROGRESS: refuse("a progress bar"); return 0;
    default: log_limited("a control the frontend does not know is not drawn: %s", "its window stays HD"); return 1;
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
 * will get. The picture takes the place and size of the game's own 100x75 thumbnail. */
static int add_photo(reader_t* r, fe_rect_t at, uint32_t fallback_raster) {
    char path[MAX_PATH];
    const preview_slot_t* slot = NULL;
    const sp_image_t* still = NULL;
    const uint16_t* bits;
    int bw, bh, pitch, chosen = selected_save(r, path, sizeof path);
    fe_rect_t frame = at;
    fe_widget_t* w;
    if (read_image(fallback_raster, &bits, &bw, &bh, &pitch)) frame = fe_rect(at.left, at.top, at.left + bw, at.top + bh);
    else bits = NULL;
    w = fe_scene_widget(r->s, FE_W_IMAGE, PREVIEW_ID, frame);
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
    } else if (bits) {
        w->image = bits;
        w->image_w = bw;
        w->image_h = bh;
        w->image_pitch = pitch;
        w->image_key = bits[0];
        w->image_hash = image_hash(bits, bw, bh, pitch);
    }
    return 1;
}

/* Direct Load's picture of the selected level: the level's own 640x480 loading picture, read from the game's menu
 * folder once and kept while that level stays selected. */
static struct {
    char name[32];
    uint32_t* pixels;
    int w, h, failed;
    uint32_t hash;
} L;

static void release_level_picture(void) {
    free(L.pixels);
    memset(&L, 0, sizeof L);
}

static int load_level_picture(const char* file) {
    static const char* const k_dirs[] = { "data\\menu\\", "menu\\" };
    uint8_t* data = NULL;
    size_t size = 0;
    if (!strcmp(L.name, file)) return L.pixels != NULL;
    release_level_picture();
    strncpy(L.name, file, sizeof L.name - 1);
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0] && !data; i++) {
        char path[MAX_PATH];
        FILE* f;
        _snprintf(path, sizeof path - 1, "%s%s", k_dirs[i], file);
        path[sizeof path - 1] = 0;
        f = fopen(path, "rb");
        if (!f) continue;
        if (!fseek(f, 0, SEEK_END) && (size = (size_t)ftell(f)) > 0 && size < (64u << 20) && !fseek(f, 0, SEEK_SET) &&
            (data = (uint8_t*)malloc(size)) != NULL && fread(data, 1, size, f) != size) {
            free(data);
            data = NULL;
        }
        fclose(f);
    }
    if (!data || !fa_tga_decode(data, size, &L.pixels, &L.w, &L.h)) {
        free(data);
        return 0;
    }
    free(data);
    L.hash = fe_hash(0, file, strlen(file));
    return 1;
}

static int add_level_photo(reader_t* r, const fe_window_t* win) {
    fe_widget_t* w = fe_scene_widget(r->s, FE_W_IMAGE, PREVIEW_ID, fe_layout_direct_photo(win->rect));
    if (!w) return 0;
    w->style = FE_IMAGE_PHOTO;
    if (r->selected_item >= 0 && load_level_picture(fe_level_picture(r->selected_text))) {
        w->image32 = L.pixels;
        w->image_w = L.w;
        w->image_h = L.h;
        w->image_pitch = L.w;
        w->image_hash = L.hash;
        w->text = fe_scene_text(r->s, r->selected_text);
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

static int read_window(reader_t* r, uint32_t window, int top) {
    uint32_t controls[MAX_CONTROLS];
    unsigned count = 0;
    fe_rect_t rect;
    fe_window_t* w;
    if (!read_rect(window + WND_RECT, &rect)) return 0;
    w = fe_scene_window(r->s, (fe_window_kind_t)r->wc->kind, rect);
    if (!w) return 0;
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
        relabel_backdrop(r->s);
    }
    if (top && r->wc->kind != FE_WND_GAME) {
        if (!read_controls(window, controls, &count)) return 0;
        r->arrow_count = 0;
        r->selected_item = -1;
        for (unsigned i = 0; i < count; i++)
            if (peek32(controls[i]) == VT_LISTBOX && r->arrow_count + 2 <= (int)(sizeof r->arrows / sizeof r->arrows[0])) {
                r->arrows[r->arrow_count++] = peek32(controls[i] + LIST_UP);
                r->arrows[r->arrow_count++] = peek32(controls[i] + LIST_DOWN);
            }
        for (unsigned i = 0; i < count; i++)
            if (!add_control(r, controls[i])) return 0;
        if (r->wc->kind == FE_WND_DIRECT_LOAD && !add_level_photo(r, w)) return 0;
        if (r->wc->kind == FE_WND_CONTROLS && rect.right - rect.left > FE_CONTROLS_W)
            fe_controls_polish(r->s, w, (double)(rect.right - rect.left) / FE_CONTROLS_W);
    }
    fe_scene_finish_window(r->s, w);
    return 1;
}

/* With the gamepad in use, the Controls window's binding fields show the gamepad's buttons for the game's commands,
 * and the corner shows the menu's buttons; with the keyboard and mouse the game's own key names stay. */
void frontend_hd_prompts(fe_scene_t* s) {
    gamepad_prompts_t g;
    const fe_window_t* top;
    fe_prompt_t pad;
    if (!s->window_count) return;
    gamepad_prompts(&g);
    if (!g.gamepad || !g.settings) return;
    top = &s->windows[s->window_count - 1];
    memset(&pad, 0, sizeof pad);
    pad.kind = FE_PROMPT_PAD;
    pad.family = (uint8_t)g.family;
    for (int i = top->first; top->kind == FE_WND_CONTROLS && i < top->first + top->count; i++) {
        fe_widget_t* w = &s->widgets[i];
        int row = w->id - CONTROLS_FIRST_FIELD;
        jpt_prompt_t binding;
        if (w->kind != FE_W_FIELD || w->filled || row < 0 || row >= JPT_GAME_CONTROL_COUNT) continue;
        if (!jpt_prompt_for_command(jpt_game_controls[row].command, 1, g.settings, NULL, 0, &binding)) {
            w->text = fe_scene_text(s, "-");
            continue;
        }
        w->prompt = pad;
        w->prompt.control = (uint8_t)binding.control;
        w->prompt.dir = (uint8_t)binding.dir;
        w->prompt.tilt = (uint8_t)binding.tilt;
    }
    pad.control = JPT_PAD_SOUTH;
    fe_scene_legend(s, pad, jpt_tr_cp("Select"));
    pad.control = JPT_PAD_EAST;
    fe_scene_legend(s, pad, jpt_tr_cp("Back"));
}

int frontend_hd_read_scene(fe_scene_t* s, int screen_w, int screen_h) {
    return frontend_hd_read_scene_for(s, screen_w, screen_h, screen_w, screen_h);
}

#define LAYER_MAX_PIXELS (17 << 20)

void frontend_hd_layer_size(int guest_w, int guest_h, int frame_w, int frame_h, int* layer_w, int* layer_h) {
    double k = 1.0;
    if (frame_w < guest_w || frame_h < guest_h || frame_w <= 0 || frame_h <= 0) {
        frame_w = guest_w;
        frame_h = guest_h;
    }
    if ((double)frame_w * frame_h > LAYER_MAX_PIXELS) k = sqrt((double)LAYER_MAX_PIXELS / ((double)frame_w * frame_h));
    *layer_w = (int)(frame_w * k);
    *layer_h = (int)(frame_h * k);
    if (*layer_w < guest_w) *layer_w = guest_w;
    if (*layer_h < guest_h) *layer_h = guest_h;
}

int frontend_hd_read_scene_for(fe_scene_t* s, int screen_w, int screen_h, int layer_w, int layer_h) {
    uint32_t main_wnd, manager, begin, end, stack[MAX_STACK];
    unsigned depth;
    reader_t r;
    fe_scene_reset_for(s, screen_w, screen_h, layer_w, layer_h);
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

static uint32_t pointer_raster(void) {
    uint32_t main_wnd = peek32(MAIN_WINDOW_OBJECT), manager = main_wnd ? peek32(main_wnd + MANAGER_OFFSET) : 0, index;
    if (!manager) return 0;
    index = peek32(manager + MGR_CURSOR_INDEX);
    return index < 16 ? peek32(manager + MGR_CURSOR_IMAGES + 4 * index) : 0;
}

/* The game's own pointer picture (a keyed 16-bit raster), scaled like the menus; 0 when it cannot be read. */
static int game_pointer(uint32_t raster, double scale) {
    const uint16_t* bits;
    int w, h, pitch, dw, dh;
    fa_source_t src;
    fa_bitmap_t big;
    if (!read_image(raster, &bits, &w, &h, &pitch) || w > 64 || h > 64) return 0;
    dw = (int)ceil(w * scale * CURSOR_SHARE);
    dh = (int)ceil(h * scale * CURSOR_SHARE);
    if (dw < 1 || dh < 1 || dw > CURSOR_MAX || dh > CURSOR_MAX) return 0;
    memset(&src, 0, sizeof src);
    src.bits = bits;
    src.w = w;
    src.h = h;
    src.pitch = pitch;
    src.keyed = 1;
    src.key = bits[0];
    if (!fa_resample16(&src, NULL, dw, dh, FA_CRISP_EDGE, &big)) return 0;
    F.cursor_size = dw > dh ? dw : dh;
    memset(F.cursor, 0, sizeof(uint32_t) * (size_t)F.cursor_size * F.cursor_size);
    for (int y = 0; y < dh; y++) memcpy(F.cursor + (size_t)y * F.cursor_size, big.px + (size_t)y * dw, sizeof(uint32_t) * (size_t)dw);
    fa_bitmap_free(&big);
    F.cursor_hot_x = F.cursor_hot_y = (int)(scale * CURSOR_SHARE + 0.5);
    return 1;
}

static void update_pointer(const fe_scene_t* s) {
    fe_view_t v = fe_view_make_for(s->guest_w, s->guest_h, s->screen_w, s->screen_h);
    uint32_t raster = pointer_raster();
    int x, y;
    if (v.sy != F.cursor_scale || raster != F.cursor_raster) {
        if (!game_pointer(raster, v.sy)) {
            int size = fe_cursor_size(v.sy);
            if (size > 96) size = 96;
            fe_paint_cursor(size == fe_cursor_size(v.sy) ? v.sy : v.sy * 96.0 / fe_cursor_size(v.sy), F.cursor, size,
                            &F.cursor_hot_x, &F.cursor_hot_y);
            F.cursor_size = size;
        }
        F.cursor_scale = v.sy;
        F.cursor_raster = raster;
        F.cursor_generation++;
    }
    if (!pointer_shown() || gamepad_menu_focus() || !window_host_pointer(&x, &y)) {
        d3dr_set_frontend_pointer(NULL, 0, 0, 0, 0);
        return;
    }
    x = (int)((long long)x * s->screen_w / s->guest_w);
    y = (int)((long long)y * s->screen_h / s->guest_h);
    d3dr_set_frontend_pointer(F.cursor, F.cursor_size, F.cursor_generation, x - F.cursor_hot_x, y - F.cursor_hot_y);
}

static void release_all(void) {
    release_labels();
    release_level_picture();
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
    int diff, frame_w = 0, frame_h = 0, layer_w, layer_h;
    LARGE_INTEGER t0, t1, freq;
    F.idle_frames = 0;
    if (d3dr_frontend_failed()) {
        refuse("the renderer could not show the HD layer");
        return 0;
    }
    if (screen_w <= 0 || screen_h <= 0 || !ensure_painter()) return 0;
    if (!d3dr_frame_size(screen_w, screen_h, &frame_w, &frame_h)) frame_w = frame_h = 0;
    frontend_hd_layer_size(screen_w, screen_h, frame_w, frame_h, &layer_w, &layer_h);
    if (!frontend_hd_read_scene_for(now, screen_w, screen_h, layer_w, layer_h)) {
        if (!F.last_refusal[0]) refuse("a window the frontend could not read");
        return 0;
    }
    frontend_hd_prompts(now);
    if (!ensure_layer(layer_w, layer_h)) {
        refuse("no memory for the HD layer");
        return 0;
    }
    diff = fe_scene_diff(F.have_previous ? before : NULL, now, &dirty);
    if (diff == FE_DIFF_ALL) clip = fe_rect(0, 0, layer_w, layer_h);
    else if (diff == FE_DIFF_RECT) {
        fe_view_t v = fe_view_make_for(screen_w, screen_h, layer_w, layer_h);
        clip = fe_rect_intersect(fe_view_rect(&v, dirty), fe_rect(0, 0, layer_w, layer_h));
    } else clip = fe_rect(0, 0, 0, 0);
    QueryPerformanceCounter(&t0);
    if (!fe_rect_empty(clip) && !fe_paint(F.painter, now, F.layer, layer_w, clip, F.threads)) {
        refuse("the HD layer could not be painted");
        F.have_previous = 0;
        return 0;
    }
    QueryPerformanceCounter(&t1);
    if (diff == FE_DIFF_ALL && F.sizes_logged < 4) {
        QueryPerformanceFrequency(&freq);
        F.sizes_logged++;
        host_log("frontend", "HD menus at %dx%d over the %dx%d game screen (canvas scale %.3f): painted in %.1f ms", layer_w,
                 layer_h, screen_w, screen_h, fe_view_make_for(screen_w, screen_h, layer_w, layer_h).sy, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart);
    }
    F.current ^= 1;
    F.have_previous = 1;
    F.last_refusal[0] = 0;
    box.left = clip.left;
    box.top = clip.top;
    box.right = clip.right;
    box.bottom = clip.bottom;
    d3dr_set_frontend(F.layer, layer_w, layer_h, fe_rect_empty(clip) ? NULL : &box);
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

fe_painter_t* frontend_hd_painter(int* threads) {
    if (!ensure_painter()) return NULL;
    *threads = F.threads;
    return F.painter;
}

void frontend_hd_release(void) {
    F.shown = 0;
    F.idle_frames = 0;
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

/* Moving the game's own rects keeps its hit tests, the gamepad focus and the remap hotspots on what is drawn. */
static void edit_controls(uint32_t window, fe_rect_t wr) {
    static int logged;
    uint32_t controls[MAX_CONTROLS];
    unsigned count;
    fe_rect_t to;
    if (wr.right - wr.left != FE_CONTROLS_W || wr.bottom - wr.top != FE_CONTROLS_H || !read_controls(window, controls, &count))
        return;
    to = fe_controls_window(wr, FE_CANVAS_W, FE_CANVAS_H);
    for (unsigned i = 0; i < count; i++) {
        uint32_t vt = peek32(controls[i]);
        int picture = vt == VT_BUTTON || vt == VT_STATIC || vt == VT_CHECKBOX;
        fe_rect_t r;
        if (!read_rect(controls[i] + CTRL_RECT, &r)) continue;
        r = fe_controls_rect(fe_rect_offset(r, -wr.left, -wr.top), picture);
        write_rect(controls[i], fe_rect_offset(r, to.left, to.top));
    }
    write_rect(window, to);
    if (!logged++)
        host_log("frontend", "Controls: laid out %.1fx larger with wider label columns, %u controls moved (%d,%d-%d,%d)",
                 FE_CONTROLS_ZOOM, count, to.left, to.top, to.right, to.bottom);
}

/* Direct Load is a 320x240 list; its window and controls move left to leave room for the level's picture. */
static void edit_direct_load(uint32_t window, fe_rect_t wr) {
    static int logged;
    uint32_t controls[MAX_CONTROLS];
    unsigned count;
    int dx = fe_layout_direct_dx(wr);
    if (!dx || !read_controls(window, controls, &count)) return;
    for (unsigned i = 0; i < count; i++) {
        fe_rect_t r;
        if (read_rect(controls[i] + CTRL_RECT, &r)) write_rect(controls[i], fe_rect_offset(r, dx, 0));
    }
    write_rect(window, fe_rect_offset(wr, dx, 0)); /* a window's rectangle is at the offset a control's is */
    if (!logged++) host_log("frontend", "Direct Load: the window moved %d units left for the level's picture", -dx);
}

void frontend_hd_window_centred(uint32_t window) {
    uint32_t vt;
    fe_rect_t wr;
    if (!window || !read32(window, &vt) || !read_rect(window + WND_RECT, &wr)) return;
    if (vt == VT_OPTIONS) edit_options(window, wr);
    else if (vt == VT_AUDIO) edit_audio(window);
    else if (vt == VT_DIRECT_LOAD) edit_direct_load(window, wr);
    else if (vt == VT_CONTROLS_WND) edit_controls(window, wr);
}
