/* The retail menus through the HD painter: each window is built from the game's own .ddf files and .tga pictures
 * (game\data\menu) the way the game lays it out, painted at the screen sizes the frontend supports, and checked for
 * the placement of every picture, the hit boxes and the cache. Skipped when the game files are not there.
 *   frontend_tests.exe --dump <dir> also writes the pictures as .ppm files. */
#include "frontend_art.h"
#include "frontend_assets.h"
#include "frontend_layout.h"
#include "jpt_display.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int* g_checks;
static int* g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        (*g_checks)++;                                                                                                  \
        if (!(cond)) {                                                                                                  \
            (*g_failures)++;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

#ifdef JPT_SOURCE_DIR
#define MENU_DIR JPT_SOURCE_DIR "/game/data/menu/"
#else
#define MENU_DIR "game/data/menu/"
#endif

#define MAX_PICTURES 160
#define MAX_TOKENS 4096

typedef struct {
    char name[48];
    uint16_t* bits;
    int w, h;
} picture_t;

static picture_t g_pictures[MAX_PICTURES];
static int g_picture_count;

static uint16_t to565(uint32_t bgra) {
    return (uint16_t)((((bgra >> 16) & 255) >> 3 << 11) | ((((bgra >> 8) & 255) >> 2) << 5) | ((bgra & 255) >> 3));
}

/* A menu picture as the game holds it: a 16-bit raster. */
static const picture_t* picture(const char* path) {
    char name[48], full[MAX_PATH];
    const char* base = strrchr(path, '\\');
    FILE* f;
    uint8_t* data;
    size_t size;
    uint32_t* px = NULL;
    picture_t* p;
    int w, h;
    base = base ? base + 1 : path;
    if (!*base) return NULL;
    strncpy(name, base, sizeof name - 1);
    name[sizeof name - 1] = 0;
    for (int i = 0; i < g_picture_count; i++)
        if (!_stricmp(g_pictures[i].name, name)) return g_pictures[i].bits ? &g_pictures[i] : NULL;
    if (g_picture_count >= MAX_PICTURES) return NULL;
    p = &g_pictures[g_picture_count++];
    strcpy(p->name, name);
    snprintf(full, sizeof full, "%s%s", MENU_DIR, name);
    f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(size);
    if (!data || fread(data, 1, size, f) != size || !fa_tga_decode(data, size, &px, &w, &h)) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    free(data);
    p->bits = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)w * h);
    p->w = w;
    p->h = h;
    for (int i = 0; p->bits && i < w * h; i++) p->bits[i] = to565(px[i]);
    free(px);
    return p->bits ? p : NULL;
}

typedef struct {
    char text[256];
    int quoted;
    double number;
} token_t;

static int tokenize(const char* path, token_t* out, int cap) {
    FILE* f = fopen(path, "r");
    char line[512];
    int n = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char* c = line;
        while (*c) {
            if (*c == ';') break;
            if (isspace((unsigned char)*c)) {
                c++;
                continue;
            }
            if (n >= cap) break;
            memset(&out[n], 0, sizeof out[n]);
            if (*c == '"') {
                char* e = strchr(c + 1, '"');
                size_t len = e ? (size_t)(e - c - 1) : strlen(c + 1);
                if (len >= sizeof out[n].text) len = sizeof out[n].text - 1;
                memcpy(out[n].text, c + 1, len);
                out[n].quoted = 1;
                c = e ? e + 1 : c + 1 + len;
            } else {
                char* e = c;
                while (*e && !isspace((unsigned char)*e) && *e != ';') e++;
                memcpy(out[n].text, c, (size_t)(e - c) < sizeof out[n].text ? (size_t)(e - c) : sizeof out[n].text - 1);
                out[n].number = strtod(out[n].text, NULL);
                if (!_strnicmp(out[n].text, "0x", 2)) out[n].number = (double)strtoul(out[n].text, NULL, 16);
                c = e;
            }
            n++;
        }
    }
    fclose(f);
    return n;
}

typedef struct {
    token_t* t;
    int n, at;
} cursor_t;

static int num(cursor_t* c) { return c->at < c->n ? (int)c->t[c->at++].number : 0; }
static const char* str(cursor_t* c) { return c->at < c->n ? c->t[c->at++].text : ""; }

static uint32_t rgb_next(cursor_t* c) {
    int r = num(c), g = num(c), b = num(c);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void set_picture(fe_widget_t* w, const picture_t* p, int keyed) {
    if (!p) return;
    w->image = p->bits;
    w->image_w = p->w;
    w->image_h = p->h;
    w->image_pitch = p->w;
    w->image_key = p->bits[0];
    w->image_keyed = keyed != 0;
    w->image_hash = fe_hash_pixels(p->bits, p->w, p->h, p->w, 3);
}

/* How a window is shown: hover on a control id, a pressed one, list rows, text for a field. */
typedef struct {
    int hover_id, pressed_id, selected_row, checked_mask;
    const char* items[16];
    int item_count;
    const char* message;
    int main_backdrop;
    int in_game;
} look_t;

static const int k_wide_flags = FE_TEXT_EXACT;

static int load_window(fe_scene_t* s, const char* ddf, fe_window_kind_t kind, fe_rect_t window, const look_t* look,
                       const char* texts[][2], int text_count) {
    static token_t tokens[MAX_TOKENS];
    char path[MAX_PATH];
    cursor_t c;
    fe_window_t* win;
    int n;
    snprintf(path, sizeof path, "%s%s", MENU_DIR, ddf);
    n = tokenize(path, tokens, MAX_TOKENS);
    if (n <= 0) return 0;
    c.t = tokens;
    c.n = n;
    c.at = 0;
    win = fe_scene_window(s, kind, window);
    while (c.at < c.n) {
        const char* word = str(&c);
        if (!_stricmp(word, "BACKGROUND")) {
            int l = num(&c), t = num(&c), r = num(&c), b = num(&c);
            const char* img = str(&c);
            (void)l;
            (void)t;
            (void)r;
            (void)b;
            if (*img && kind == FE_WND_MAIN) {
                const picture_t* p = picture(img);
                if (p) {
                    s->backdrop = p->bits;
                    s->backdrop_pitch = p->w;
                    s->backdrop_hash = fe_hash_pixels(p->bits, p->w, p->h, p->w, 7);
                }
            }
        } else if (!_stricmp(word, "UICTRL")) {
            const char* type = str(&c);
            int visible = num(&c), enabled = num(&c), id = num(&c), l = num(&c), t = num(&c), r = num(&c), b = num(&c);
            fe_rect_t rect = fe_rect(window.left + l, window.top + t, window.left + r, window.top + b);
            fe_widget_t* w = NULL;
            if (!_stricmp(type, "BUTTON")) {
                const picture_t* pic[4];
                int keyed = num(&c), slot;
                for (int i = 0; i < 4; i++) pic[i] = picture(str(&c));
                if (!visible) continue;
                slot = !enabled ? 3 : id == look->pressed_id ? 2 : id == look->hover_id ? 1 : 0;
                if (!pic[slot]) continue;
                rect = fe_rect(rect.left, rect.top, rect.left + pic[slot]->w, rect.top + pic[slot]->h);
                w = fe_scene_widget(s, FE_W_BUTTON, id, rect);
                w->style = FE_BUTTON_PLAQUE;
                w->state = (uint16_t)(!enabled ? FE_ST_DISABLED : id == look->pressed_id ? FE_ST_PRESSED : id == look->hover_id ? FE_ST_HOVER : 0);
                set_picture(w, pic[slot], keyed);
            } else if (!_stricmp(type, "static")) {
                int keyed = num(&c);
                const picture_t* p = picture(str(&c));
                if (!visible || !p) continue;
                rect = fe_rect(rect.left, rect.top, rect.left + p->w, rect.top + p->h);
                w = fe_scene_widget(s, FE_W_IMAGE, id, rect);
                set_picture(w, p, keyed > 0);
            } else if (!_stricmp(type, "checkbox")) {
                const picture_t* pic[4];
                int checked = (look->checked_mask >> (id - 1020)) & 1, hover = id == look->hover_id;
                for (int i = 0; i < 4; i++) pic[i] = picture(str(&c));
                if (!visible) continue;
                w = fe_scene_widget(s, FE_W_CHECK, id, fe_rect(rect.left, rect.top, rect.left + 12, rect.top + 12));
                w->state = (uint16_t)((checked ? FE_ST_CHECKED : 0) | (hover ? FE_ST_HOVER : 0));
                set_picture(w, pic[(checked ? 2 : 0) | (hover ? 1 : 0)], 0);
            } else if (!_stricmp(type, "slider")) {
                const picture_t* thumb = picture(str(&c));
                int units = num(&c);
                num(&c);
                num(&c);
                if (!visible) continue;
                w = fe_scene_widget(s, FE_W_SLIDER, id, rect);
                w->range = units;
                w->value = fe_slider_thumb_x(rect, 10, 72, units);
                set_picture(w, thumb, 0);
            } else if (!_stricmp(type, "textbox")) {
                int transparent = num(&c), backlit = num(&c), offset = num(&c);
                uint32_t shadow = rgb_next(&c), bk, fg;
                const char* text = str(&c);
                int points = num(&c), weight = num(&c), dt, border;
                bk = rgb_next(&c);
                fg = rgb_next(&c);
                dt = num(&c);
                border = num(&c);
                (void)bk;
                if (!visible) continue;
                if (id == 1002 && look->message) text = look->message;
                for (int i = 0; i < text_count; i++)
                    if (texts[i][0] && atoi(texts[i][0]) == id) text = texts[i][1];
                w = fe_scene_widget(s, border ? FE_W_FIELD : FE_W_LABEL, id, rect);
                w->text = fe_scene_text(s, text);
                w->text_h = (float)points;
                w->weight = (uint16_t)weight;
                w->color = fg;
                w->border = border != 0;
                w->text_flags = (uint16_t)(k_wide_flags | ((dt & 1) ? FE_TEXT_CENTER : 0) | ((dt & 2) ? FE_TEXT_RIGHT : 0) |
                                           ((dt & 4) ? FE_TEXT_VCENTER : 0) | ((dt & 0x10) ? FE_TEXT_WRAP : 0));
                if (backlit && offset) {
                    w->shadow_offset = (uint8_t)offset;
                    w->shadow_color = shadow;
                }
                (void)transparent;
            } else if (!_stricmp(type, "hotspot")) {
                num(&c);
            } else if (!_stricmp(type, "listbox")) {
                int points = num(&c), weight = num(&c), sel = num(&c), transparent = num(&c), backlit, offset;
                uint32_t key, shadow, bg, fg;
                int show, border;
                (void)sel;
                (void)transparent;
                key = rgb_next(&c);
                backlit = num(&c);
                offset = num(&c);
                shadow = rgb_next(&c);
                bg = rgb_next(&c);
                fg = rgb_next(&c);
                show = num(&c);
                border = num(&c);
                (void)key;
                (void)backlit;
                (void)show;
                (void)border;
                if (!visible) continue;
                w = fe_scene_widget(s, FE_W_LIST, id, rect);
                w->text_h = (float)points;
                w->weight = (uint16_t)weight;
                w->color = fg;
                w->selected_color = bg;
                w->shadow_offset = (uint8_t)offset;
                w->shadow_color = shadow;
                w->text_flags = FE_TEXT_EXACT;
                w->arrow_w = 16;
                w->row_h = (int)(points * 1.15 + 0.5);
                w->rows = (rect.bottom - rect.top) / w->row_h;
                w->selected = look->selected_row;
                w->track = fe_rect(rect.right - 16, rect.top, rect.right, rect.bottom);
                w->item_count = look->item_count;
                w->first_item = s->item_count;
                for (int i = 0; i < w->item_count && i < w->rows; i++) fe_scene_item(s, look->items[i]);
                {
                    fe_widget_t* a;
                    const picture_t* up = picture("sarrow_up_0.tga");
                    const picture_t* dn = picture("sarrow_dn_0.tga");
                    a = fe_scene_widget(s, FE_W_BUTTON, 0, fe_rect(rect.right - 16, rect.top, rect.right, rect.top + 16));
                    a->style = FE_BUTTON_ARROW_UP;
                    set_picture(a, up, 1);
                    a = fe_scene_widget(s, FE_W_BUTTON, 0, fe_rect(rect.right - 16, rect.bottom - 16, rect.right, rect.bottom));
                    a->style = FE_BUTTON_ARROW_DOWN;
                    set_picture(a, dn, 1);
                }
            } else if (!_stricmp(type, "editbox")) {
                int points = num(&c), weight = num(&c), transparent = num(&c), backlit = num(&c), offset = num(&c);
                uint32_t shadow = rgb_next(&c), fg;
                const char* text = str(&c);
                rgb_next(&c);
                fg = rgb_next(&c);
                num(&c);
                num(&c);
                (void)transparent;
                (void)backlit;
                (void)text;
                w = fe_scene_widget(s, FE_W_EDIT, id, rect);
                w->text = fe_scene_text(s, look->message ? look->message : "Town - Hammond's house");
                w->text_h = (float)points;
                w->weight = (uint16_t)weight;
                w->color = fg;
                w->text_flags = FE_TEXT_EXACT | FE_TEXT_VCENTER;
                w->state = FE_ST_CARET;
                w->caret = (int)strlen(fe_scene_str(s, w->text));
                w->shadow_offset = (uint8_t)offset;
                w->shadow_color = shadow;
            }
            (void)w;
        }
    }
    fe_scene_finish_window(s, win);
    return 1;
}

static int g_available = -1;

static int available(void) {
    if (g_available < 0) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%smainscreen.ddf", MENU_DIR);
        g_available = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        if (!g_available) printf("frontend_retail_tests: the game files are not here, the retail menus are not painted\n");
    }
    return g_available;
}

typedef struct {
    const char* name;
    const char* ddf;
    fe_window_kind_t kind;
    int width, height;
    int under_main, under_game;
    look_t look;
} scene_t;

static const char* k_saves[] = { "Beach - washed ashore", "Beach - the fence", "Industrial Jungle", "Town - Hammond's house",
                                 "Plains - raptor nest", "Pine Valley" };

static const scene_t k_scenes[] = {
    { "main", "mainscreen.ddf", FE_WND_MAIN, 640, 480, 0, 0, { 1001, 0, 0, 0, { 0 }, 0, NULL, 1, 0 } },
    { "options", "options.ddf", FE_WND_OPTIONS, 320, 200, 1, 0, { 1002, 0, 0, 0, { 0 }, 0, NULL, 0, 0 } },
    { "options_game", "options2.ddf", FE_WND_OPTIONS, 320, 200, 0, 1, { 1000, 0, 0, 0, { 0 }, 0, NULL, 0, 1 } },
    { "audio", "audio.ddf", FE_WND_AUDIO, 320, 200, 1, 0, { 1021, 0, 0, 0x0F, { 0 }, 0, NULL, 0, 0 } },
    { "controls", "controls.ddf", FE_WND_CONTROLS, 320, 200, 1, 0, { 1002, 0, 0, 0, { 0 }, 0, NULL, 0, 0 } },
    { "yesno", "yesno.ddf", FE_WND_MESSAGE, 320, 200, 1, 0, { 1001, 0, 0, 0, { 0 }, 0, "Do you wish to exit Trespasser?", 0, 0 } },
    { "quit", "quit.ddf", FE_WND_QUIT, 320, 200, 0, 1, { 1002, 0, 0, 0, { 0 }, 0, NULL, 0, 1 } },
    { "game_menu", "ingameopt.ddf", FE_WND_GAME_MENU, 320, 200, 0, 1, { 1001, 0, 0, 0, { 0 }, 0, NULL, 0, 1 } },
    { "load", "loadgame.ddf", FE_WND_LOAD, 320, 200, 1, 0, { 1001, 0, 3, 0, { 0 }, 6, NULL, 0, 0 } },
    { "save", "savegame.ddf", FE_WND_SAVE, 320, 200, 0, 1, { 1000, 0, 3, 0, { 0 }, 6, NULL, 0, 1 } },
    { "direct_load", "directload.ddf", FE_WND_DIRECT_LOAD, 320, 240, 1, 0, { 1001, 0, 1, 0, { 0 }, 6, NULL, 0, 0 } },
};
#define SCENE_COUNT (int)(sizeof k_scenes / sizeof k_scenes[0])

static fe_painter_t* g_painter;

static void window_for(const scene_t* sc, fe_rect_t* r) {
    *r = sc->kind == FE_WND_MAIN ? fe_rect(0, 0, 640, 480)
                                 : fe_rect((640 - sc->width) / 2, (480 - sc->height) / 2, (640 + sc->width) / 2,
                                           (480 + sc->height) / 2);
}

/* What frontend_hd does to the Controls window: every control moved into the host's layout, then the text fitted. */
static void controls_layout(fe_scene_t* s) {
    fe_window_t* w = &s->windows[s->window_count - 1];
    fe_rect_t from = w->rect, to = fe_controls_window(from, FE_CANVAS_W, FE_CANVAS_H);
    for (int i = w->first; i < w->first + w->count; i++) {
        fe_widget_t* x = &s->widgets[i];
        int picture = x->kind == FE_W_BUTTON || x->kind == FE_W_IMAGE || x->kind == FE_W_CHECK;
        fe_rect_t r = fe_controls_rect(fe_rect_offset(x->rect, -from.left, -from.top), picture);
        x->rect = fe_rect_offset(r, to.left, to.top);
    }
    w->rect = to;
    fe_controls_polish(s, w, (double)(to.right - to.left) / FE_CONTROLS_W);
    fe_scene_finish_window(s, w);
}

static int build_scene(fe_scene_t* s, const scene_t* sc, int guest_w, int guest_h, int layer_w, int layer_h) {
    fe_rect_t wr;
    look_t look = sc->look;
    static const char* texts[][2] = { { "1030", "W" },          { "1031", "Shift+W" }, { "1032", "S" },
                                      { "1033", "A" },          { "1019", "" },        { "1039", "Left Mouse" },
                                      { "1040", "Right Mouse" } };
    for (int i = 0; i < look.item_count; i++) look.items[i] = k_saves[i];
    fe_scene_reset_for(s, guest_w, guest_h, layer_w, layer_h);
    if (sc->under_game) fe_scene_window(s, FE_WND_GAME, fe_rect(0, 0, 640, 480));
    if (sc->under_main) {
        fe_window_t* w = fe_scene_window(s, FE_WND_MAIN, fe_rect(0, 0, 640, 480));
        const picture_t* p = picture("ms_bkgnd.tga");
        if (!p) return 0;
        s->backdrop = p->bits;
        s->backdrop_pitch = p->w;
        s->backdrop_hash = fe_hash_pixels(p->bits, p->w, p->h, p->w, 7);
        fe_scene_finish_window(s, w);
    }
    window_for(sc, &wr);
    if (!load_window(s, sc->ddf, sc->kind, wr, &look, texts, 7)) return 0;
    if (sc->kind == FE_WND_CONTROLS) controls_layout(s);
    return 1;
}

static void write_ppm(const char* dir, const char* name, int w, int h, const uint32_t* px, int bg) {
    char path[MAX_PATH];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t v = px[i];
        unsigned a = v >> 24, inv = 255 - a;
        unsigned char rgb[3] = { (unsigned char)(((v >> 16) & 255) + bg * inv / 255), (unsigned char)(((v >> 8) & 255) + bg * inv / 255),
                                 (unsigned char)((v & 255) + bg * inv / 255) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int opaque_share(const uint32_t* px, int pitch, fe_rect_t r) {
    long solid = 0, total = 0;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++, total++)
            if ((px[(long)y * pitch + x] >> 24) > 64) solid++;
    return total ? (int)(solid * 100 / total) : 0;
}

static void paint_all(const char* dump_dir) {
    static const struct {
        int w, h;
    } k_sizes[] = { { 1280, 720 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1920 }, { 3440, 1440 }, { 3840, 2160 }, { 5120, 1440 }, { 7680, 2160 } };
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    if (!s) return;
    for (int i = 0; i < SCENE_COUNT; i++) {
        const scene_t* sc = &k_scenes[i];
        for (int k = 0; k < (int)(sizeof k_sizes / sizeof k_sizes[0]); k++) {
            int w = k_sizes[k].w, h = k_sizes[k].h;
            uint32_t* px = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
            fe_view_t v = fe_view_make(w, h);
            int ok;
            if (!px) continue;
            ok = build_scene(s, sc, w, h, w, h);
            CHECK(ok, "%s: the scene is built from the retail files", sc->name);
            if (ok) {
                memset(px, 0, sizeof(uint32_t) * (size_t)w * h);
                CHECK(fe_paint(g_painter, s, px, w, fe_rect(0, 0, w, h), 4), "%s %dx%d: painted", sc->name, w, h);
                for (int wi = 0; wi < s->widget_count; wi++) {
                    const fe_widget_t* x = &s->widgets[wi];
                    fe_rect_t pr;
                    if (x->kind != FE_W_BUTTON || !x->image) continue;
                    pr = fe_view_rect(&v, fe_rect(x->rect.left, x->rect.top, x->rect.left + x->image_w, x->rect.top + x->image_h));
                    pr = fe_rect_intersect(pr, fe_rect(0, 0, w, h));
                    CHECK(opaque_share(px, w, pr) > 25, "%s %dx%d: the picture of control %d is on screen (%d%% covered)", sc->name, w, h,
                          x->id, opaque_share(px, w, pr));
                }
                if (dump_dir && (k == 3 || k == 7 || k == 4) && !(i == 0 && k == 3)) {
                    char name[96];
                    snprintf(name, sizeof name, "retail_%s_%dx%d", sc->name, w, h);
                    write_ppm(dump_dir, name, w, h, px, sc->under_game ? 40 : 0);
                }
            }
            free(px);
        }
    }
    free(s);
}

/* A pointer over the middle of every drawn picture reaches its control in the game's own canvas coordinates, with the
 * layer larger than the game's screen: 640x480 shown at 2880x2160, a 5424x1520 gameplay screen over a 7680x2160 frame. */
static void test_hit_boxes(void) {
    static const int k_pairs[][4] = {
        { 640, 480, 2880, 2160 }, { 640, 480, 1920, 1440 }, { 1920, 1080, 3840, 2160 }, { 5424, 1520, 7680, 2160 },
        { 3440, 1440, 3440, 1440 }, { 2560, 1600, 2560, 1600 }, { 5120, 1440, 5120, 1440 },
    };
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    if (!s) return;
    for (int i = 0; i < SCENE_COUNT; i++)
        for (int k = 0; k < (int)(sizeof k_pairs / sizeof k_pairs[0]); k++) {
            const int* q = k_pairs[k];
            fe_view_t v = fe_view_make_for(q[0], q[1], q[2], q[3]);
            if (!build_scene(s, &k_scenes[i], q[0], q[1], q[2], q[3])) continue;
            for (int wi = 0; wi < s->widget_count; wi++) {
                const fe_widget_t* x = &s->widgets[wi];
                fe_rect_t art, control;
                int u, w, cx, cy;
                if (x->kind != FE_W_BUTTON || !x->image) continue;
                control = fe_rect(x->rect.left, x->rect.top, x->rect.left + x->image_w, x->rect.top + x->image_h);
                art = fe_view_rect(&v, control);
                cx = (art.left + art.right) / 2;
                cy = (art.top + art.bottom) / 2;
                fe_view_to_canvas(&v, cx, cy, &u, &w);
                CHECK(fe_rect_contains(control, u, w), "%s on %dx%d over %dx%d: the centre of control %d maps to %d,%d, outside %d,%d-%d,%d",
                      k_scenes[i].name, q[2], q[3], q[0], q[1], x->id, u, w, control.left, control.top, control.right, control.bottom);
                fe_view_to_canvas(&v, art.left - 4, cy, &u, &w);
                CHECK(u < control.left || art.left - 4 < 0, "%s: a pointer left of control %d is inside its box", k_scenes[i].name, x->id);
                fe_view_to_canvas(&v, cx, art.bottom + 4, &u, &w);
                CHECK(w >= control.bottom || art.bottom + 4 >= q[3], "%s: a pointer below control %d is inside its box", k_scenes[i].name, x->id);
            }
        }
    free(s);
}

static void test_cache_between_frames(void) {
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    uint32_t* px = (uint32_t*)malloc(sizeof(uint32_t) * 1920 * 1080);
    LARGE_INTEGER t0, t1, t2, f;
    double first, second;
    unsigned long before;
    if (!s || !px) return;
    QueryPerformanceFrequency(&f);
    CHECK(build_scene(s, &k_scenes[0], 1920, 1080, 1920, 1080), "main scene");
    QueryPerformanceCounter(&t0);
    CHECK(fe_paint(g_painter, s, px, 1920, fe_rect(0, 0, 1920, 1080), 4), "first");
    QueryPerformanceCounter(&t1);
    CHECK(fe_paint(g_painter, s, px, 1920, fe_rect(0, 0, 1920, 1080), 4), "second");
    QueryPerformanceCounter(&t2);
    first = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    second = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
    printf("retail main menu at 1920x1080: first paint %.1f ms (pictures scaled), again %.1f ms (cached)\n", first, second);
    CHECK(second < first, "a repaint reuses the scaled pictures (%.1f ms against %.1f ms)", second, first);
    before = 0;
    (void)before;
    fe_painter_trim(g_painter);
    free(px);
    free(s);
}

/* The selected (hover) plaque: its dark red lettering becomes clearly red, and the rest of the original picture stays. */
static void test_hover_lift(void) {
    static const char* const names[] = { "ms_new",   "ms_load", "ms_options", "ms_credits", "ms_quit", "gm_resume",
                                         "gm_controls", "gm_save", "gm_load", "qt_menu", "qt_game", "yes",
                                         "no",       "ok" };
    for (size_t n = 0; n < sizeof names / sizeof names[0]; n++) {
        char file[64];
        const picture_t *normal, *hover;
        float *rgba;
        uint8_t *solid, *letter;
        double letter_before = 0, letter_after = 0, plate_change = 0;
        int letters = 0, plate = 0, w, h;
        snprintf(file, sizeof file, "%s_0.tga", names[n]);
        normal = picture(file);
        snprintf(file, sizeof file, "%s_1.tga", names[n]);
        hover = picture(file);
        if (!normal || !hover) continue;
        CHECK(normal->w == hover->w && normal->h == hover->h, "%s: hover and normal pictures differ in size", names[n]);
        if (normal->w != hover->w || normal->h != hover->h) continue;
        w = hover->w;
        h = hover->h;
        rgba = (float*)malloc(sizeof(float) * 4 * (size_t)w * h);
        solid = (uint8_t*)malloc((size_t)w * h);
        letter = (uint8_t*)malloc((size_t)w * h);
        if (!rgba || !solid || !letter) {
            free(rgba);
            free(solid);
            free(letter);
            return;
        }
        for (int i = 0; i < w * h; i++) {
            uint16_t v = hover->bits[i];
            solid[i] = v != hover->bits[0];
            rgba[i * 4] = (float)((v >> 11) & 31) / 31.0f;
            rgba[i * 4 + 1] = (float)((v >> 5) & 63) / 63.0f;
            rgba[i * 4 + 2] = (float)(v & 31) / 31.0f;
            rgba[i * 4 + 3] = solid[i] ? 1.0f : 0.0f;
        }
        fa_hover_lift(rgba, solid, w, h, normal->bits, w, hover->bits[0], 1);
        /* lettering: deep red in the hover picture (its letters can sit a few pixels off the normal ones) */
        for (int i = 0; i < w * h; i++) {
            uint16_t v = hover->bits[i];
            float r1 = (float)((v >> 11) & 31) / 31.0f, g1 = (float)((v >> 5) & 63) / 63.0f, b1 = (float)(v & 31) / 31.0f;
            letter[i] = solid[i] && r1 - (g1 > b1 ? g1 : b1) > 0.10f && g1 < 0.36f * r1;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int i = y * w + x, close = 0;
                uint16_t v = hover->bits[i];
                float r1 = (float)((v >> 11) & 31) / 31.0f, g1 = (float)((v >> 5) & 63) / 63.0f;
                if (!solid[i]) continue;
                if (letter[i]) {
                    letters++;
                    letter_before += r1;
                    letter_after += rgba[i * 4];
                    continue;
                }
                for (int dy = -3; dy <= 3 && !close; dy++)
                    for (int dx = -3; dx <= 3 && !close; dx++) {
                        int nx = x + dx, ny = y + dy;
                        close = nx >= 0 && ny >= 0 && nx < w && ny < h && letter[ny * w + nx];
                    }
                if (close) continue;
                /* the plate away from the letters, rust included */
                plate++;
                plate_change += fabs(rgba[i * 4] - r1) + fabs(rgba[i * 4 + 1] - g1);
            }
        CHECK(letters > 40, "%s: no hover lettering found (%d pixels)", names[n], letters);
        if (letters > 40)
            CHECK(letter_after / letters > 0.62 && letter_after / letters > letter_before / letters + 0.25,
                  "%s: the selected lettering is not lifted (red %.2f -> %.2f)", names[n], letter_before / letters,
                  letter_after / letters);
        CHECK(plate > 0 && plate_change / plate < 0.01, "%s: the plate around the letters changed (%.3f)", names[n],
              plate > 0 ? plate_change / plate : 0.0);
        free(rgba);
        free(solid);
        free(letter);
    }
}

/* Controls in the host's layout: every label and binding whole on one line at the window's text size without
 * shrinking, labels clear of their fields, the columns apart, everything inside the window. */
static void test_controls_layout(void) {
    static const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 5120, 1440 } };
    fe_scene_t* s = (fe_scene_t*)calloc(1, sizeof *s);
    HDC dc = CreateCompatibleDC(NULL);
    const scene_t* sc = NULL;
    for (int i = 0; i < SCENE_COUNT; i++)
        if (k_scenes[i].kind == FE_WND_CONTROLS) sc = &k_scenes[i];
    if (!s || !dc || !sc) {
        free(s);
        if (dc) DeleteDC(dc);
        return;
    }
    for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
        int w = sizes[k][0], h = sizes[k][1], labels = 0, merged = 0;
        fe_view_t v = fe_view_make(w, h);
        const fe_window_t* win;
        if (!build_scene(s, sc, w, h, w, h)) continue;
        win = &s->windows[s->window_count - 1];
        CHECK(win->rect.right - win->rect.left == 560 && win->rect.bottom - win->rect.top == 350 && win->rect.left >= 0 &&
                  win->rect.right <= FE_CANVAS_W && win->rect.top >= 0 && win->rect.bottom <= FE_CANVAS_H,
              "controls %dx%d: the window is not the larger one inside the canvas", w, h);
        for (int i = win->first; i < win->first + win->count; i++) {
            const fe_widget_t* x = &s->widgets[i];
            const char* text = fe_scene_str(s, x->text);
            CHECK(x->rect.left >= win->rect.left && x->rect.right <= win->rect.right + 1 && x->rect.top >= win->rect.top &&
                      x->rect.bottom <= win->rect.bottom + 1,
                  "controls %dx%d: control %d leaves the window", w, h, x->id);
            if ((x->kind == FE_W_LABEL || x->kind == FE_W_FIELD) && *text) {
                int px = (int)floor(x->text_h * v.sy + 0.5);
                double room = (x->rect.right - x->rect.left - 2 * 2.5 * x->image_zoom) * v.sx;
                HFONT f = CreateFontA(-px, 0, 0, 0, x->weight ? x->weight : 500, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
                HGDIOBJ old = SelectObject(dc, f);
                SIZE size;
                int replay = !strcmp(text, "Replay voice over");
                GetTextExtentPoint32A(dc, text, (int)strlen(text), &size);
                SelectObject(dc, old);
                DeleteObject(f);
                labels++;
                merged += replay;
                CHECK((x->text_flags & FE_TEXT_FIT) && !(x->text_flags & FE_TEXT_WRAP), "controls: \"%s\" is not one fitted line",
                      text);
                CHECK(size.cx <= room, "controls %dx%d: \"%s\" needs %ld px in %.0f", w, h, text,
                      (long)size.cx, room);
                CHECK(x->text_h >= 17.4f, "controls: \"%s\" is not drawn larger (%.1f units)", text, x->text_h);
            }
            for (int j = win->first; j < win->first + win->count; j++) {
                const fe_widget_t* y = &s->widgets[j];
                if (j == i || (x->kind != FE_W_LABEL && x->kind != FE_W_FIELD) || (y->kind != FE_W_LABEL && y->kind != FE_W_FIELD))
                    continue;
                CHECK(fe_rect_empty(fe_rect_intersect(x->rect, y->rect)), "controls %dx%d: \"%s\" and \"%s\" overlap", w, h,
                      fe_scene_str(s, x->text), fe_scene_str(s, y->text));
            }
        }
        CHECK(labels >= 20 && merged == 1, "controls %dx%d: %d texts, \"Replay voice over\" %d times", w, h, labels, merged);
    }
    {
        /* a picture keeps its proportions, the columns keep their order and gaps */
        fe_rect_t b = fe_controls_rect(fe_rect(4, 157, 66, 199), 1), lf = fe_controls_rect(fe_rect(94, 46, 164, 60), 0);
        fe_rect_t rf = fe_controls_rect(fe_rect(164, 46, 246, 60), 0), ll = fe_controls_rect(fe_rect(10, 46, 90, 60), 0);
        fe_rect_t rl = fe_controls_rect(fe_rect(248, 46, 320, 60), 0);
        CHECK(abs((b.right - b.left) * 42 - (b.bottom - b.top) * 62) <= 62 && b.right - b.left >= 108,
              "a button keeps its proportions (%dx%d)", b.right - b.left, b.bottom - b.top);
        CHECK(ll.right < lf.left && lf.right < rf.left && rf.right < rl.left && rl.right <= 560, "the columns overlap");
        CHECK(ll.right - ll.left > 145 && rl.right - rl.left > 150 && lf.right - lf.left >= 110 && rf.right - rf.left >= 110,
              "a column is too narrow");
        /* the buttons keep their places: DEFAULTS still ends before CANCEL starts */
        CHECK(fe_controls_rect(fe_rect(67, 157, 197, 199), 1).right <= fe_controls_rect(fe_rect(198, 158, 318, 200), 1).left,
              "DEFAULTS and CANCEL overlap");
    }
    DeleteDC(dc);
    free(s);
}

void frontend_retail_tests(int* checks, int* failures, const char* dump_dir) {
    g_checks = checks;
    g_failures = failures;
    if (!available()) return;
    g_painter = fe_painter_create();
    CHECK(g_painter != NULL, "the painter starts");
    if (!g_painter) return;
    paint_all(dump_dir);
    test_hit_boxes();
    test_cache_between_frames();
    test_hover_lift();
    test_controls_layout();
    fe_painter_destroy(g_painter);
}
