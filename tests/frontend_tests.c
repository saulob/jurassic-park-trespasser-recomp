/* Headless checks of the HD frontend: canvas placement and pointer mapping on every aspect ratio, the Options and
 * Audio layouts, scene diffs, and the painter (clipping, determinism, threads). No window, device or game.
 * frontend_bridge_tests.c covers the bridge to the game's objects.
 *   frontend_tests.exe [--dump <dir>]   (--dump writes .ppm pictures of the mock screens) */
#include "frontend_assets.h"
#include "frontend_layout.h"
#include "host_policy.h"
#include "jpt_display.h"
#include "save_preview.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures;

void frontend_bridge_tests(int* checks, int* failures);
void save_preview_tests(int* checks, int* failures);
void loading_tests(int* checks, int* failures, const char* dump_dir);

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

typedef struct {
    const char* aspect;
    int w, h;
} screen_t;

static const screen_t k_screens[] = {
    { "4:3", 1024, 768 },   { "4:3", 1600, 1200 },  { "16:9", 1280, 720 },  { "16:9", 1920, 1080 },
    { "16:9", 2560, 1440 }, { "16:9", 3840, 2160 }, { "16:10", 1920, 1200 }, { "16:10", 2560, 1600 },
    { "21:9", 2560, 1080 }, { "21:9", 3440, 1440 }, { "32:9", 3840, 1080 }, { "32:9", 5120, 1440 },
    { "4:3", 800, 600 },    { "4:3", 641, 481 },
};
#define SCREEN_COUNT (int)(sizeof k_screens / sizeof k_screens[0])

static uint16_t g_backdrop[480][640];
static uint16_t g_preview[75][100];
static sp_image_t g_photo;

static uint16_t rgb565(int r, int g, int b);

/* A save's HD picture: a synthetic scene, or a real game screenshot (user\TPass000.BMP) when dumping. */
static void make_photo(const char* bmp) {
    FILE* f = bmp ? fopen(bmp, "rb") : NULL;
    unsigned char h[54];
    if (f && fread(h, 1, 54, f) == 54 && h[0] == 'B' && h[28] == 24) {
        int w = h[18] | h[19] << 8 | h[20] << 16, ht = h[22] | h[23] << 8 | h[24] << 16;
        size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
        uint16_t* px = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)w * ht);
        unsigned char* row = (unsigned char*)malloc(stride);
        fseek(f, h[10] | h[11] << 8 | h[12] << 16, SEEK_SET);
        for (int y = 0; px && row && y < ht && fread(row, 1, stride, f) == stride; y++)
            for (int x = 0; x < w; x++) px[(size_t)(ht - 1 - y) * w + x] = rgb565(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
        if (px) save_preview_shrink(px, w, ht, w, &g_photo);
        free(px);
        free(row);
    }
    if (f) fclose(f);
    if (!g_photo.pixels) {
        static uint16_t scene[270][480];
        for (int y = 0; y < 270; y++)
            for (int x = 0; x < 480; x++) scene[y][x] = rgb565(30 + x / 6, 60 + y / 3 + ((x / 24 + y / 24) & 1) * 30, 40);
        save_preview_shrink(&scene[0][0], 480, 270, 480, &g_photo);
    }
}

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void synthetic_art(void) {
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            g_backdrop[y][x] = rgb565(20 + (x * 40) / 640, 40 + ((x ^ y) & 31) + y / 12, 18 + y / 20);
    for (int y = 0; y < 75; y++)
        for (int x = 0; x < 100; x++) g_preview[y][x] = rgb565(40 + x, 60 + y, 30);
}

/* The retail art when the game files are present: 16-bit or 24-bit uncompressed TGA, bottom-up or top-down. */
static int load_tga(const char* path, uint16_t* out, int want_w, int want_h) {
    FILE* f = fopen(path, "rb");
    unsigned char h[18];
    int w, ht, bpp, top;
    if (!f) return 0;
    if (fread(h, 1, 18, f) != 18 || h[2] != 2) {
        fclose(f);
        return 0;
    }
    w = h[12] | h[13] << 8;
    ht = h[14] | h[15] << 8;
    bpp = h[16];
    top = (h[17] & 0x20) != 0;
    fseek(f, 18 + h[0], SEEK_SET);
    if (w != want_w || ht != want_h || (bpp != 16 && bpp != 24)) {
        fclose(f);
        return 0;
    }
    for (int y = 0; y < ht; y++) {
        int row = top ? y : ht - 1 - y;
        for (int x = 0; x < w; x++) {
            unsigned char p[3];
            if (fread(p, 1, (size_t)bpp / 8, f) != (size_t)bpp / 8) {
                fclose(f);
                return 0;
            }
            if (bpp == 16) {
                int v = p[0] | p[1] << 8;
                out[row * want_w + x] = rgb565(((v >> 10) & 31) << 3, ((v >> 5) & 31) << 3, (v & 31) << 3);
            } else {
                out[row * want_w + x] = rgb565(p[2], p[1], p[0]);
            }
        }
    }
    fclose(f);
    return 1;
}

static fe_rect_t at(fe_rect_t window, int l, int t, int r, int b) {
    return fe_rect(window.left + l, window.top + t, window.left + r, window.top + b);
}

static fe_rect_t dialog_rect(void) { return fe_rect(160, 140, 480, 340); }

static fe_widget_t* button(fe_scene_t* s, fe_rect_t r, int id, const char* label, int style, int state) {
    fe_widget_t* w = fe_scene_widget(s, FE_W_BUTTON, id, r);
    if (!w) return NULL;
    w->style = (uint8_t)style;
    w->state = (uint16_t)state;
    w->text = fe_scene_text(s, label);
    return w;
}

static fe_widget_t* label(fe_scene_t* s, fe_rect_t r, const char* text, float h, int flags) {
    fe_widget_t* w = fe_scene_widget(s, FE_W_LABEL, -1, r);
    if (!w) return NULL;
    w->text = fe_scene_text(s, text);
    w->text_h = h;
    w->text_flags = (uint16_t)(flags | FE_TEXT_SHADOW);
    return w;
}

/* hover < 0: the main screen under a dialog, which the game draws without its buttons. */
static void main_screen(fe_scene_t* s, int hover) {
    static const char* labels[] = { "NEW GAME", "LOAD", "OPTIONS", "EXIT" };
    fe_window_t* w = fe_scene_window(s, FE_WND_MAIN, fe_rect(0, 0, 640, 480));
    s->backdrop = &g_backdrop[0][0];
    s->backdrop_pitch = 640;
    s->backdrop_hash = fe_hash_pixels(&g_backdrop[0][0], 640, 480, 640, 7);
    if (hover >= 0) {
        int lengths[4];
        fe_rect_t rects[4];
        for (int i = 0; i < 4; i++) lengths[i] = (int)strlen(labels[i]);
        fe_layout_main_menu(lengths, 4, rects);
        for (int i = 0; i < 4; i++) button(s, rects[i], 1000 + i, labels[i], FE_BUTTON_LARGE, i == hover ? FE_ST_HOVER : 0);
    }
    fe_scene_finish_window(s, w);
}

/* The entries' picture sizes in options.ddf / options2.ddf: Audio, Controls, Credits, Close. */
static const int32_t k_option_ids[] = { 1002, 1000, 1003, 1004 };
static const int k_option_w[] = { 87, 130, 115, 87 }, k_option_h[] = { 36, 42, 42, 36 };
static const int k_option_game_w[] = { 87, 130, 87 }, k_option_game_h[] = { 36, 42, 36 };

static void options_screen(fe_scene_t* s, int in_game) {
    static const int32_t ids_full[] = { 1002, 1000, 1003, 1004 }, ids_game[] = { 1002, 1000, 1004 };
    static const char* names_full[] = { "AUDIO", "CONTROLS", "CREDITS", "CLOSE" };
    static const char* names_game[] = { "AUDIO", "CONTROLS", "CLOSE" };
    const int32_t* ids = in_game ? ids_game : ids_full;
    const char** names = in_game ? names_game : names_full;
    int n = in_game ? 3 : 4;
    fe_rect_t rects[4], wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, FE_WND_OPTIONS, wr);
    w->title = fe_scene_text(s, "OPTIONS");
    fe_layout_options(ids, in_game ? k_option_game_w : k_option_w, in_game ? k_option_game_h : k_option_h, n, 320, 200,
                      rects);
    for (int i = 0; i < n; i++)
        button(s, fe_rect_offset(rects[i], wr.left, wr.top), ids[i], names[i], FE_BUTTON_PLAQUE, i == 1 ? FE_ST_HOVER : 0);
    fe_scene_finish_window(s, w);
}

static void audio_screen(fe_scene_t* s) {
    static const char* names[] = { "Play Sound Effects", "Play Ambient Sounds", "Play Voice Overs", "Play Background Music",
                                   "Show Subtitles" };
    fe_rect_t wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, FE_WND_AUDIO, wr);
    int tops[5];
    fe_widget_t* x;
    w->title = fe_scene_text(s, "AUDIO");
    button(s, at(wr, 185, 155, 303, 197), 1000, "CANCEL", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 22, 155, 84, 197), 1001, "OK", FE_BUTTON_PLAQUE, FE_ST_HOVER);
    x = fe_scene_widget(s, FE_W_SLIDER, 1004, at(wr, 144, 20, 290, 43));
    x->range = 101;
    x->value = fe_slider_thumb_x(x->rect, 10, 72, x->range);
        fe_layout_rows(5, 58, 150, 15, tops);
    for (int i = 0; i < 5; i++) {
        x = fe_scene_widget(s, FE_W_CHECK, 1020 + i, at(wr, 80, tops[i], 92, tops[i] + 12));
        x->state = (uint16_t)((i != 4 ? FE_ST_CHECKED : 0) | (i == 2 ? FE_ST_ROW_HOVER : 0));
        x = label(s, at(wr, 120, tops[i] - 2, 300, tops[i] + 15), names[i], 12.0f, 0);
        if (i == 2) x->state = FE_ST_ROW_HOVER;
    }
    label(s, at(wr, 16, 20, 138, 43), "Master Volume", 12.0f, FE_TEXT_RIGHT | FE_TEXT_VCENTER);
    fe_scene_tidy_check_rows(s, w);
    fe_scene_finish_window(s, w);
}

static void controls_screen(fe_scene_t* s) {
    static const char* left[] = { "Walk Forward", "Run Forward", "Walk Backwards", "Sidestep Left", "Sidestep Right", "Jump",
                                  "Crouch" };
    static const char* left_keys[] = { "W", "Shift+W", "S", "A", "D", "Space", "C" };
    static const char* right[] = { "Throw", "Use/Fire", "Rotate Wrist", "Move Hand", "Grab/Drop", "Stow/Retrieve", "Replay" };
    static const char* right_keys[] = { "Right Mouse", "Left Mouse", "R", "Ctrl", "E", "Q", "V" };
    fe_rect_t wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, FE_WND_CONTROLS, wr);
    fe_widget_t* x;
    w->title = fe_scene_text(s, "CONTROLS");
    button(s, at(wr, 4, 157, 66, 199), 1001, "OK", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 67, 157, 197, 199), 1002, "DEFAULTS", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 198, 158, 316, 200), 1000, "CANCEL", FE_BUTTON_PLAQUE, 0);
    for (int i = 0; i < 7; i++) {
        int y = 46 + 14 * i;
        label(s, at(wr, 10, y, 90, y + 14), left[i], 10.0f, FE_TEXT_RIGHT | FE_TEXT_VCENTER);
        x = fe_scene_widget(s, FE_W_FIELD, 1030 + i, at(wr, 94, y, 164, y + 14));
        x->text = fe_scene_text(s, left_keys[i]);
        x->text_h = 10.0f;
        x->text_flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
        if (i == 3) x->state = FE_ST_ROW_HOVER;
        x = fe_scene_widget(s, FE_W_FIELD, 1037 + i, at(wr, 164, y, 246, y + 14));
        x->text = fe_scene_text(s, right_keys[i]);
        x->text_h = 10.0f;
        x->text_flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
        label(s, at(wr, 248, y, 320, y + 14), right[i], 10.0f, FE_TEXT_VCENTER);
    }
    label(s, at(wr, 248, 144, 320, 160), "voice over", 10.0f, 0);
    label(s, at(wr, 10, 32, 90, 46), "Gore", 10.0f, FE_TEXT_RIGHT | FE_TEXT_VCENTER);
    x = fe_scene_widget(s, FE_W_FIELD, 100, at(wr, 94, 32, 164, 46));
    x->text = fe_scene_text(s, "Full");
    x->text_h = 10.0f;
    x->text_flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    x = fe_scene_widget(s, FE_W_CHECK, 102, at(wr, 170, 18, 182, 30));
    x->state = FE_ST_CHECKED;
    label(s, at(wr, 80, 18, 168, 32), "Invert Mouse", 10.0f, FE_TEXT_RIGHT | FE_TEXT_VCENTER);
    x = fe_scene_widget(s, FE_W_FIELD, 1044, at(wr, 164, 32, 246, 46));
    x->text = fe_scene_text(s, "Z");
    x->text_h = 10.0f;
    x->text_flags = FE_TEXT_CENTER | FE_TEXT_VCENTER;
    label(s, at(wr, 248, 32, 320, 46), "Rotate Arm", 10.0f, FE_TEXT_VCENTER);
    fe_scene_finish_window(s, w);
}

static void list_items(fe_scene_t* s, fe_widget_t* x) {
    static const char* saves[] = { "Beach - washed ashore", "Beach - the fence", "Industrial Jungle", "Town - Hammond's house",
                                   "Plains - raptor nest", "Pine Valley", "Summit", "InGen Lab", "Ascent" };
    x->item_count = 9;
    x->first_item = s->item_count;
    for (int i = 0; i < fe_list_shown(x); i++) fe_scene_item(s, saves[x->top + i]);
}

static void load_screen(fe_scene_t* s, int save) {
    fe_rect_t wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, save ? FE_WND_SAVE : FE_WND_LOAD, wr);
    fe_widget_t* x;
    w->title = fe_scene_text(s, save ? "SAVE GAME" : "LOAD GAME");
    x = fe_scene_widget(s, FE_W_IMAGE, 1003, fe_layout_photo(save ? FE_WND_SAVE : FE_WND_LOAD, wr));
    x->style = FE_IMAGE_PHOTO;
    if (save) {
        x->image = &g_preview[0][0];
        x->image_w = 100;
        x->image_h = 75;
        x->image_pitch = 100;
        x->image_hash = fe_hash_pixels(&g_preview[0][0], 100, 75, 100, 1);
    } else {
        x->image32 = g_photo.pixels;
        x->image_w = g_photo.width;
        x->image_h = g_photo.height;
        x->image_pitch = g_photo.width;
        x->image_hash = 1;
    }
    x = fe_scene_widget(s, FE_W_LIST, 1002, save ? at(wr, 125, 41, 297, 155) : at(wr, 124, 14, 300, 150));
    x->text_h = 12.0f;
    x->row_h = 14;
    x->rows = (x->rect.bottom - x->rect.top) / 14;
    x->top = 1;
    x->selected = 3;
    x->track = fe_rect(x->rect.right - 16, x->rect.top, x->rect.right, x->rect.bottom);
    list_items(s, x);
    {
        fe_rect_t col = x->track;
        fe_widget_t* a = button(s, fe_rect(col.left, col.top, col.right, col.top + 16), 0, "", FE_BUTTON_ARROW_UP, 0);
        a->style = FE_BUTTON_ARROW_UP;
        a = button(s, fe_rect(col.left, col.bottom - 16, col.right, col.bottom), 0, "", FE_BUTTON_ARROW_DOWN, FE_ST_HOVER);
        a->style = FE_BUTTON_ARROW_DOWN;
    }
    if (save) {
        x = fe_scene_widget(s, FE_W_EDIT, 1004, at(wr, 25, 11, 297, 31));
        x->text = fe_scene_text(s, "Town - Hammond's house");
        x->text_h = 12.0f;
        x->caret = 22;
        x->state = FE_ST_CARET;
    }
    button(s, at(wr, 15, 159, 77, 201), 1001, "OK", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 82, 159, 186, 201), save ? 1005 : 1004, "DELETE", FE_BUTTON_PLAQUE, FE_ST_DISABLED);
    button(s, at(wr, 190, 159, 308, 201), 1000, "CANCEL", FE_BUTTON_PLAQUE, FE_ST_PRESSED);
    fe_scene_finish_window(s, w);
}

static void quit_screen(fe_scene_t* s) {
    fe_rect_t wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, FE_WND_QUIT, wr);
    w->title = fe_scene_text(s, "QUIT");
    button(s, at(wr, 102, 40, 218, 78), 1001, "DESKTOP", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 87, 84, 232, 126), 1002, "MAIN MENU", FE_BUTTON_PLAQUE, FE_ST_HOVER);
    button(s, at(wr, 101, 132, 219, 174), 1000, "CANCEL", FE_BUTTON_PLAQUE, 0);
    fe_scene_finish_window(s, w);
}

static void game_menu_screen(fe_scene_t* s) {
    fe_rect_t wr = dialog_rect();
    fe_window_t* w;
    fe_scene_window(s, FE_WND_GAME, fe_rect(0, 0, 640, 480));
    w = fe_scene_window(s, FE_WND_GAME_MENU, wr);
    w->title = fe_scene_text(s, "PAUSED");
    button(s, at(wr, 93, 142, 238, 184), 1002, "RESTART LEVEL", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 118, 26, 204, 68), 1003, "SAVE", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 22, 29, 109, 65), 1004, "LOAD", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 38, 82, 156, 124), 1005, "OPTIONS", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 211, 27, 295, 69), 1000, "QUIT", FE_BUTTON_PLAQUE, 0);
    button(s, at(wr, 160, 82, 275, 124), 1001, "RESUME", FE_BUTTON_PLAQUE, FE_ST_HOVER);
    fe_scene_finish_window(s, w);
}

static void message_screen(fe_scene_t* s) {
    fe_rect_t wr = dialog_rect();
    fe_window_t* w = fe_scene_window(s, FE_WND_MESSAGE, wr);
    label(s, at(wr, 10, 10, 320, 180),
          "Are you sure you want to delete the saved game \"Town - Hammond's house\"? This cannot be undone.", 14.0f,
          FE_TEXT_CENTER | FE_TEXT_WRAP | FE_TEXT_VCENTER);
    button(s, at(wr, 205, 190, 267, 232), 1000, "OK", FE_BUTTON_PLAQUE, 0);
    fe_scene_finish_window(s, w);
}

typedef void (*build_fn)(fe_scene_t*);

static void build_main(fe_scene_t* s) { main_screen(s, 1); }
static void build_options(fe_scene_t* s) {
    main_screen(s, -1);
    options_screen(s, 0);
}
static void build_options_game(fe_scene_t* s) {
    fe_scene_window(s, FE_WND_GAME, fe_rect(0, 0, 640, 480));
    options_screen(s, 1);
}
static void build_audio(fe_scene_t* s) {
    main_screen(s, -1);
    audio_screen(s);
}
static void build_controls(fe_scene_t* s) {
    main_screen(s, -1);
    controls_screen(s);
}
static void build_load(fe_scene_t* s) {
    main_screen(s, -1);
    load_screen(s, 0);
}
static void build_save(fe_scene_t* s) {
    fe_scene_window(s, FE_WND_GAME, fe_rect(0, 0, 640, 480));
    load_screen(s, 1);
}
static void build_quit(fe_scene_t* s) {
    fe_scene_window(s, FE_WND_GAME, fe_rect(0, 0, 640, 480));
    quit_screen(s);
}
static void build_message(fe_scene_t* s) {
    main_screen(s, -1);
    message_screen(s);
}

static const struct {
    const char* name;
    build_fn build;
} k_scenes[] = {
    { "main", build_main },   { "options", build_options }, { "options_game", build_options_game },
    { "audio", build_audio }, { "controls", build_controls }, { "load", build_load },
    { "save", build_save },   { "quit", build_quit },        { "game_menu", game_menu_screen },
    { "message", build_message },
};
#define SCENE_COUNT (int)(sizeof k_scenes / sizeof k_scenes[0])

static fe_scene_t g_scene, g_other;

static void make_scene(fe_scene_t* s, int index, int w, int h) {
    fe_scene_reset(s, w, h);
    k_scenes[index].build(s);
}

static void test_view(void) {
    for (int i = 0; i < SCREEN_COUNT; i++) {
        const screen_t* sc = &k_screens[i];
        fe_view_t v = fe_view_make(sc->w, sc->h);
        fe_rect_t canvas = fe_view_rect(&v, fe_rect(0, 0, 640, 480)), panel = fe_view_rect(&v, dialog_rect());
        double aspect = (double)(panel.right - panel.left) / (panel.bottom - panel.top);
        CHECK(fabs(v.sx - v.sy) / v.sy < 0.005, "%s %dx%d: canvas scale x %.4f y %.4f", sc->aspect, sc->w, sc->h, v.sx, v.sy);
        CHECK(canvas.left >= 0 && canvas.top >= 0 && canvas.right <= sc->w && canvas.bottom <= sc->h,
              "%s %dx%d: canvas %d,%d-%d,%d leaves the screen", sc->aspect, sc->w, sc->h, canvas.left, canvas.top,
              canvas.right, canvas.bottom);
        CHECK(abs((canvas.left + canvas.right) - sc->w) <= 1, "%s %dx%d: canvas not centred", sc->aspect, sc->w, sc->h);
        CHECK(fabs(aspect - 1.6) < 0.016, "%s %dx%d: a 320x200 panel is drawn %.3f:1", sc->aspect, sc->w, sc->h, aspect);
        CHECK(abs((panel.bottom - panel.top) - (int)(sc->h * 200.0 / 480.0 + 0.5)) <= 2 || sc->w * 3 < sc->h * 4,
              "%s %dx%d: panel height %d, not its share of the screen height", sc->aspect, sc->w, sc->h, panel.bottom - panel.top);
        if (sc->h >= 1080)
            CHECK(panel.bottom - panel.top >= 440, "%s %dx%d: panel only %d px tall", sc->aspect, sc->w, sc->h,
                  panel.bottom - panel.top);
    }
}

/* The game hit-tests canvas rectangles with the canvas pointer jpt_ui_to_canvas gives: inside a widget's screen
 * rectangle the pointer must land in the widget, clearly outside it must not. */
static void check_hits(const fe_view_t* v, const screen_t* sc, const char* scene, const fe_widget_t* w) {
    fe_rect_t r = fe_view_rect(v, w->rect);
    int margin = (int)ceil(v->sy) + 1;
    for (int k = 0; k < 9; k++) {
        int x = r.left + margin + (r.right - r.left - 2 * margin) * (k % 3) / 2;
        int y = r.top + margin + (r.bottom - r.top - 2 * margin) * (k / 3) / 2, u, t;
        if (r.right - r.left <= 2 * margin || r.bottom - r.top <= 2 * margin) break;
        fe_view_to_canvas(v, x, y, &u, &t);
        CHECK(fe_rect_contains(w->rect, u, t), "%s %s %dx%d: widget %d: screen %d,%d maps to canvas %d,%d outside %d,%d-%d,%d",
              scene, sc->aspect, sc->w, sc->h, w->id, x, y, u, t, w->rect.left, w->rect.top, w->rect.right, w->rect.bottom);
    }
    {
        int u, t, cy = (r.top + r.bottom) / 2, cx = (r.left + r.right) / 2;
        if (r.left - margin >= 0) {
            fe_view_to_canvas(v, r.left - margin, cy, &u, &t);
            CHECK(!fe_rect_contains(w->rect, u, t) || u == 0, "%s %dx%d: widget %d hot left of its picture", scene, sc->w,
                  sc->h, w->id);
        }
        if (r.bottom + margin < sc->h) {
            fe_view_to_canvas(v, cx, r.bottom + margin, &u, &t);
            CHECK(!fe_rect_contains(w->rect, u, t), "%s %dx%d: widget %d hot below its picture", scene, sc->w, sc->h, w->id);
        }
    }
}

static void test_mouse_mapping(void) {
    for (int i = 0; i < SCREEN_COUNT; i++) {
        fe_view_t v = fe_view_make(k_screens[i].w, k_screens[i].h);
        for (int n = 0; n < SCENE_COUNT; n++) {
            make_scene(&g_scene, n, k_screens[i].w, k_screens[i].h);
            for (int k = 0; k < g_scene.widget_count; k++) {
                const fe_widget_t* w = &g_scene.widgets[k];
                fe_rect_t r = fe_view_rect(&v, w->rect);
                if (w->kind == FE_W_BUTTON || w->kind == FE_W_CHECK || w->kind == FE_W_LIST || w->kind == FE_W_SLIDER ||
                    w->kind == FE_W_FIELD)
                    check_hits(&v, &k_screens[i], k_scenes[n].name, w);
                CHECK(r.left >= 0 && r.top >= 0 && r.right <= k_screens[i].w && r.bottom <= k_screens[i].h,
                      "%s %dx%d: widget %d clipped by the screen", k_scenes[n].name, k_screens[i].w, k_screens[i].h, w->id);
            }
        }
    }
}

static int overlaps(fe_rect_t a, fe_rect_t b) { return !fe_rect_empty(fe_rect_intersect(a, b)); }

static void test_options_layout(void) {
    static const int32_t game[] = { 1002, 1000, 1004 };
    const int32_t* sets[2] = { k_option_ids, game };
    const int* widths[2] = { k_option_w, k_option_game_w };
    const int* heights[2] = { k_option_h, k_option_game_h };
    int counts[2] = { 4, 3 };
    for (int k = 0; k < 2; k++) {
        fe_rect_t r[4];
        int close = -1, audio = -1, same_left = 1, same_top = 1;
        fe_layout_options(sets[k], widths[k], heights[k], counts[k], 320, 200, r);
        for (int i = 0; i < counts[k]; i++) {
            CHECK(sets[k][i] != 1001, "the Video entry is laid out");
            CHECK(r[i].left >= 8 && r[i].top >= 8 && r[i].right <= 312 && r[i].bottom <= 192,
                  "options entry %d at %d,%d-%d,%d leaves the plate", sets[k][i], r[i].left, r[i].top, r[i].right, r[i].bottom);
            CHECK(r[i].right - r[i].left == widths[k][i] && r[i].bottom - r[i].top == heights[k][i],
                  "options entry %d is not its picture's size", sets[k][i]);
            for (int j = 0; j < i; j++) CHECK(!overlaps(r[i], r[j]), "options entries %d and %d overlap", sets[k][i], sets[k][j]);
            if (sets[k][i] == FE_OPTIONS_CLOSE_ID) close = i;
            if (sets[k][i] == FE_OPTIONS_AUDIO_ID) audio = i;
            if (i && r[i].left != r[0].left) same_left = 0;
            if (i && r[i].top != r[0].top) same_top = 0;
        }
        CHECK(close >= 0 && audio >= 0, "Close or Audio is missing");
        CHECK(!same_left && !same_top, "options entries form a plain column or row");
        for (int i = 0; i < counts[k]; i++) {
            if (i != close) CHECK(r[i].bottom + 6 <= r[close].top, "Close is not set apart below entry %d", sets[k][i]);
            if (i != audio) CHECK(r[audio].top < r[i].top, "Audio is not the highest entry");
        }
    }
}

static void test_main_menu_layout(void) {
    static const int lengths[4] = { 8, 4, 7, 4 };
    fe_rect_t r[4];
    int widths_differ = 0, lefts_differ = 0;
    fe_layout_main_menu(lengths, 4, r);
    for (int k = 0; k < 4; k++) {
        fe_rect_t original = fe_rect(205, 70 + 90 * k, 413, 151 + 90 * k);
        CHECK(fe_rect_equal(fe_rect_intersect(r[k], fe_rect_inflate(original, 12, 6)), r[k]),
              "main menu plaque %d strays from its place", k);
        CHECK(r[k].right - r[k].left >= 150 && r[k].bottom - r[k].top >= 60, "main menu plaque %d too small to read", k);
        for (int j = 0; j < k; j++) CHECK(!overlaps(r[k], r[j]), "main menu plaques %d and %d overlap", j, k);
        if (k && r[k].right - r[k].left != r[0].right - r[0].left) widths_differ = 1;
        if (k && r[k].left != r[0].left) lefts_differ = 1;
    }
    CHECK(widths_differ && lefts_differ, "the main menu plaques are all alike");
    CHECK(r[0].right - r[0].left > r[1].right - r[1].left, "NEW GAME's plaque is not wider than LOAD's");
}

/* The aspect of what is shown always matches the part of the picture taken: nothing is stretched. */
static void test_photo_fit(void) {
    static const int screens[][2] = { { 2560, 1920 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 }, { 5120, 1440 }, { 640, 480 } };
    static const double frames[][2] = { { 166, 121 }, { 168, 104 } };
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < 6; i++) {
            double src[4], dst[4], fw = frames[f][0], fh = frames[f][1];
            double sa, da, cover;
            fe_photo_fit(screens[i][0], screens[i][1], fw, fh, src, dst);
            sa = (src[2] - src[0]) / (src[3] - src[1]);
            da = (dst[2] - dst[0]) / (dst[3] - dst[1]);
            cover = (dst[2] - dst[0]) * (dst[3] - dst[1]) / (fw * fh);
            CHECK(fabs(sa / da - 1.0) < 0.002, "a %dx%d picture is stretched %.3f in a %.0fx%.0f frame", screens[i][0],
                  screens[i][1], sa / da, fw, fh);
            CHECK(src[0] >= 0 && src[1] >= 0 && src[2] <= screens[i][0] && src[3] <= screens[i][1], "crop outside the picture");
            CHECK(dst[0] >= -0.001 && dst[1] >= -0.001 && dst[2] <= fw + 0.001 && dst[3] <= fh + 0.001, "picture outside its frame");
            CHECK((src[2] - src[0]) / screens[i][0] >= 1.0 / FE_PHOTO_CROP - 0.001 &&
                      (src[3] - src[1]) / screens[i][1] >= 1.0 / FE_PHOTO_CROP - 0.001,
                  "a %dx%d picture loses more than its crop allowance", screens[i][0], screens[i][1]);
            if (screens[i][0] * 9 <= screens[i][1] * 16)
                CHECK(cover > 0.999, "a %dx%d picture leaves its frame partly empty", screens[i][0], screens[i][1]);
            else
                CHECK(cover > 0.35, "a %dx%d picture fills only %.0f%% of its frame", screens[i][0], screens[i][1], cover * 100);
        }
    for (int kind = FE_WND_LOAD; kind <= FE_WND_SAVE; kind++) {
        fe_rect_t window = fe_rect(160, 140, 480, 340), photo = fe_layout_photo((fe_window_kind_t)kind, window);
        fe_rect_t list = kind == FE_WND_LOAD ? fe_rect(284, 154, 460, 290) : fe_rect(285, 181, 457, 295);
        CHECK(photo.right - photo.left >= 170 && photo.bottom - photo.top >= 110, "the save picture's frame is small");
        CHECK(!overlaps(photo, list), "the save picture covers the list");
        CHECK(photo.left >= 0 && photo.bottom < window.top + 159, "the save picture leaves the canvas or covers the buttons");
    }
}

/* The game's frames around a window opening (DoModal: three clear-and-flip presents, then UI frames) and closing. */
typedef enum { EV_UI_FRAME, EV_BLANK_PRESENT } present_event_t;

typedef struct {
    present_event_t event;
    guest_ui_t ui;
    guest_wnd_t top;
} present_step_t;

static int canvas_menu(guest_ui_t ui, guest_wnd_t top) {
    return (ui == GUEST_UI_MENU || ui == GUEST_UI_DIALOG) && top != GUEST_WND_LOADER;
}

/* 1 when a blank frame reaches the screen after the first real one. */
static int shows_blank(const present_step_t* steps, int n) {
    int shown_real = 0;
    for (int i = 0; i < n; i++) {
        int in_frame = steps[i].event == EV_UI_FRAME;
        if (ui_present_held(in_frame, canvas_menu(steps[i].ui, steps[i].top), steps[i].top)) continue;
        if (in_frame) shown_real = 1;
        else if (shown_real) return 1;
    }
    return 0;
}

#define OPEN(ui, top) { EV_BLANK_PRESENT, ui, top }, { EV_BLANK_PRESENT, ui, top }, { EV_BLANK_PRESENT, ui, top }
#define FRAMES(ui, top) { EV_UI_FRAME, ui, top }, { EV_UI_FRAME, ui, top }

static void test_transitions(void) {
    const guest_ui_t M = GUEST_UI_MENU, D = GUEST_UI_DIALOG, G = GUEST_UI_GAMEPLAY;
    const present_step_t root_to_options[] = { FRAMES(M, GUEST_WND_MAIN_MENU), OPEN(M, GUEST_WND_OTHER), FRAMES(M, GUEST_WND_OTHER) };
    const present_step_t options_audio_options[] = { FRAMES(M, GUEST_WND_OTHER), OPEN(M, GUEST_WND_OTHER), FRAMES(M, GUEST_WND_OTHER),
                                                     FRAMES(M, GUEST_WND_OTHER) };
    const present_step_t dialog_open_close[] = { FRAMES(M, GUEST_WND_MAIN_MENU), OPEN(M, GUEST_WND_OTHER), FRAMES(M, GUEST_WND_OTHER),
                                                 FRAMES(M, GUEST_WND_MAIN_MENU) };
    const present_step_t pause_open_close[] = { FRAMES(G, GUEST_WND_GAME), OPEN(D, GUEST_WND_INGAME_OPTIONS),
                                                FRAMES(D, GUEST_WND_INGAME_OPTIONS), OPEN(D, GUEST_WND_OTHER),
                                                FRAMES(D, GUEST_WND_OTHER), FRAMES(D, GUEST_WND_INGAME_OPTIONS),
                                                FRAMES(G, GUEST_WND_GAME) };
    CHECK(!shows_blank(root_to_options, (int)(sizeof root_to_options / sizeof root_to_options[0])), "root -> Options blanks");
    CHECK(!shows_blank(options_audio_options, (int)(sizeof options_audio_options / sizeof options_audio_options[0])),
          "Options -> Audio -> Options blanks");
    CHECK(!shows_blank(dialog_open_close, (int)(sizeof dialog_open_close / sizeof dialog_open_close[0])), "a dialog blanks");
    CHECK(!shows_blank(pause_open_close, (int)(sizeof pause_open_close / sizeof pause_open_close[0])), "the pause menu blanks");
    CHECK(!ui_present_held(0, canvas_menu(M, GUEST_WND_VIDEO), GUEST_WND_VIDEO), "a video's own frames are held");
    CHECK(!ui_present_held(0, canvas_menu(M, GUEST_WND_LOADER), GUEST_WND_LOADER), "a loading window's frames are held");
    CHECK(!ui_present_held(0, canvas_menu(G, GUEST_WND_GAME), GUEST_WND_GAME), "gameplay frames are held");
    CHECK(!ui_present_held(1, 1, GUEST_WND_OTHER), "the UI frame itself is held");
    CHECK(!ui_present_held(0, 0, GUEST_WND_OTHER), "presents are held without the canvas (software renderer)");
}

static void test_rows(void) {
    int tops[5];
    fe_layout_rows(5, 58, 150, 15, tops);
    CHECK(tops[0] == 58 && tops[4] + 15 == 150, "audio rows span %d..%d", tops[0], tops[4] + 15);
    for (int i = 1; i < 5; i++) {
        int step = tops[i] - tops[i - 1];
        CHECK(step >= 15 && abs(step - 19) <= 1, "audio row step %d", step);
    }
    fe_layout_rows(1, 40, 90, 15, tops);
    CHECK(tops[0] == 40, "single row moved");
}

static void test_slider(void) {
    fe_rect_t t = fe_rect(144, 20, 290, 43);
    int last = -1;
    CHECK(fe_slider_thumb_x(t, 10, 0, 101) == 149, "slider at 0: %d", fe_slider_thumb_x(t, 10, 0, 101));
    CHECK(fe_slider_thumb_x(t, 10, 100, 101) == 285, "slider at 100: %d", fe_slider_thumb_x(t, 10, 100, 101));
    for (int v = 0; v <= 100; v++) {
        int x = fe_slider_thumb_x(t, 10, v, 101);
        CHECK(x >= last, "slider thumb moves back at %d", v);
        last = x;
    }
    CHECK(fe_slider_thumb_x(t, 10, 500, 101) == 285 && fe_slider_thumb_x(t, 10, -3, 101) == 149, "slider clamps");
    CHECK(fe_slider_thumb_x(t, 10, 72, 101) == 149 + 97, "slider at 72 is not truncated as the game does");
    CHECK(fe_slider_drag_x(t, 10, 100) == 149 && fe_slider_drag_x(t, 10, 400) == 285 && fe_slider_drag_x(t, 10, 200) == 200,
          "a dragged thumb does not follow the pointer");
    {
        fe_rect_t column = fe_rect(284, 14, 300, 150), thumb = fe_list_thumb(column, 16, 0, 9, 9);
        CHECK(fe_rect_empty(thumb), "a scroll thumb while every item is shown");
        thumb = fe_list_thumb(column, 16, 3, 9, 12);
        CHECK(thumb.top == 56 && thumb.bottom == 134 && thumb.left == 284, "scroll thumb %d..%d", thumb.top, thumb.bottom);
    }
}

static void test_diff(void) {
    fe_rect_t dirty;
    int hovered;
    make_scene(&g_scene, 1, 1920, 1080);
    make_scene(&g_other, 1, 1920, 1080);
    CHECK(fe_scene_diff(&g_scene, &g_other, &dirty) == FE_DIFF_SAME, "identical scenes differ");
    CHECK(fe_scene_diff(NULL, &g_other, &dirty) == FE_DIFF_ALL, "first frame not painted whole");
    hovered = g_other.widget_count - 1;
    g_other.widgets[hovered].state ^= FE_ST_HOVER;
    CHECK(fe_scene_diff(&g_scene, &g_other, &dirty) == FE_DIFF_RECT, "a hover change repaints everything");
    CHECK(fe_rect_equal(fe_rect_intersect(dirty, g_other.widgets[hovered].rect), g_other.widgets[hovered].rect),
          "the dirty rectangle misses the changed button");
    make_scene(&g_other, 0, 1920, 1080);
    CHECK(fe_scene_diff(&g_scene, &g_other, &dirty) == FE_DIFF_ALL, "a closed window is not a full repaint");
    make_scene(&g_other, 1, 2560, 1440);
    CHECK(fe_scene_diff(&g_scene, &g_other, &dirty) == FE_DIFF_ALL, "a new screen size is not a full repaint");
    make_scene(&g_scene, 7, 1920, 1080);
    make_scene(&g_other, 7, 1920, 1080);
    g_other.text_used = fe_scene_text(&g_other, "x") >= 0 ? g_other.text_used : g_other.text_used;
    g_other.widgets[1].text = fe_scene_text(&g_other, "MAIN MENU!");
    CHECK(fe_scene_diff(&g_scene, &g_other, &dirty) == FE_DIFF_RECT, "a label change is not a partial repaint");
}

static uint32_t* surface(int w, int h, uint32_t fill) {
    uint32_t* p = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)w * h);
    if (p)
        for (long i = 0; i < (long)w * h; i++) p[i] = fill;
    return p;
}

static void write_ppm(const char* dir, const char* name, int w, int h, const uint32_t* px) {
    char path[MAX_PATH];
    FILE* f;
    unsigned char* row = (unsigned char*)malloc((size_t)w * 3);
    snprintf(path, sizeof path, "%s\\%s_%dx%d.ppm", dir, name, w, h);
    f = fopen(path, "wb");
    if (!f || !row) {
        if (f) fclose(f);
        free(row);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(long)y * w + x], a = v >> 24;
            /* the game's picture under the layer: a dark green-grey ramp */
            uint32_t bg_r = 40 + (uint32_t)(x * 30 / w), bg_g = 52 + (uint32_t)(y * 20 / h), bg_b = 36;
            row[x * 3 + 0] = (unsigned char)(((v >> 16) & 255) + bg_r * (255 - a) / 255);
            row[x * 3 + 1] = (unsigned char)(((v >> 8) & 255) + bg_g * (255 - a) / 255);
            row[x * 3 + 2] = (unsigned char)((v & 255) + bg_b * (255 - a) / 255);
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    fclose(f);
    free(row);
    printf("wrote %s\n", path);
}

static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void test_painter(const char* dump_dir) {
    fe_painter_t* p = fe_painter_create();
    CHECK(p != NULL, "the painter cannot be created");
    if (!p) return;
    printf("fonts: plaque \"%s\", labels \"%s\", body \"%s\"\n", fe_painter_face(p, FE_FONT_PLAQUE),
           fe_painter_face(p, FE_FONT_LABEL), fe_painter_face(p, FE_FONT_BODY));
    {
        const int w = 1280, h = 720;
        uint32_t *full = surface(w, h, 0), *part = surface(w, h, 0xDEADBEEFu), *mt = surface(w, h, 0);
        fe_rect_t clip = fe_rect(301, 97, 911, 503);
        for (int n = 0; n < SCENE_COUNT && full && part && mt; n++) {
            long diff = 0, outside = 0, threads_diff = 0;
            make_scene(&g_scene, n, w, h);
            CHECK(fe_paint(p, &g_scene, full, w, fe_rect(0, 0, w, h), 1), "%s: paint failed", k_scenes[n].name);
            CHECK(fe_paint(p, &g_scene, mt, w, fe_rect(0, 0, w, h), 4), "%s: threaded paint failed", k_scenes[n].name);
            for (long i = 0; i < (long)w * h; i++) part[i] = 0xDEADBEEFu;
            CHECK(fe_paint(p, &g_scene, part, w, clip, 3), "%s: clipped paint failed", k_scenes[n].name);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    long i = (long)y * w + x;
                    if (fe_rect_contains(clip, x, y)) diff += part[i] != full[i];
                    else outside += part[i] != 0xDEADBEEFu;
                    threads_diff += mt[i] != full[i];
                }
            CHECK(diff == 0, "%s: a clipped repaint differs from the full picture in %ld pixels", k_scenes[n].name, diff);
            CHECK(outside == 0, "%s: %ld pixels written outside the clip", k_scenes[n].name, outside);
            CHECK(threads_diff == 0, "%s: threads change %ld pixels", k_scenes[n].name, threads_diff);
            if (g_scene.windows[0].kind == FE_WND_MAIN) {
                long clear = 0;
                for (long i = 0; i < (long)w * h; i++) clear += (full[i] >> 24) != 255;
                CHECK(clear == 0, "%s: the main screen leaves %ld pixels transparent", k_scenes[n].name, clear);
            } else {
                fe_rect_t panel;
                fe_view_t v = fe_view_make(w, h);
                const fe_window_t* top = &g_scene.windows[g_scene.window_count - 1];
                uint32_t corner = full[2 * w + 2], centre;
                panel = fe_view_rect(&v, top->panel);
                centre = full[(long)((panel.top + panel.bottom) / 2) * w + (panel.left + panel.right) / 2];
                CHECK((corner >> 24) > 40 && (corner >> 24) < 120, "%s: the game picture is dimmed to alpha %u", k_scenes[n].name,
                      corner >> 24);
                CHECK((centre >> 24) == 255, "%s: the panel is not opaque", k_scenes[n].name);
            }
        }
        free(full);
        free(part);
        free(mt);
    }
    {
        static const int sizes[][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
        for (int k = 0; k < 3; k++) {
            int w = sizes[k][0], h = sizes[k][1];
            uint32_t* px = surface(w, h, 0);
            fe_view_t v = fe_view_make(w, h);
            fe_rect_t hover;
            double t0, t1, t2, t3, t4;
            if (!px) continue;
            make_scene(&g_scene, 1, w, h);
            hover = fe_view_rect(&v, fe_rect_inflate(g_scene.widgets[1].rect, 10, 10));
            fe_painter_trim(p);
            t0 = now_ms();
            fe_paint(p, &g_scene, px, w, fe_rect(0, 0, w, h), 8);
            t1 = now_ms();
            fe_paint(p, &g_scene, px, w, fe_rect(0, 0, w, h), 1);
            t2 = now_ms();
            fe_paint(p, &g_scene, px, w, fe_rect(0, 0, w, h), 8);
            t3 = now_ms();
            fe_paint(p, &g_scene, px, w, hover, 1);
            t4 = now_ms();
            fe_paint(p, &g_scene, px, w, fe_rect(0, 0, 1, 1), 1);
            printf("options over the main screen at %dx%d: first %.1f ms (8 threads), again %.1f ms (1) / %.1f ms (8), "
                   "one button %.1f ms (1), display list %.1f ms\n", w, h, t1 - t0, t2 - t1, t3 - t2, t4 - t3,
                   now_ms() - t4);
            free(px);
        }
    }
    if (dump_dir) {
        static const int sizes[][2] = { { 1920, 1080 }, { 1024, 768 }, { 3440, 1440 }, { 5120, 1440 }, { 2560, 1600 } };
        for (int k = 0; k < 5; k++)
            for (int n = 0; n < SCENE_COUNT; n++) {
                int w = sizes[k][0], h = sizes[k][1];
                uint32_t* px;
                if (k > 0 && n != 0 && n != 1 && n != 3) continue;
                px = surface(w, h, 0);
                if (!px) continue;
                make_scene(&g_scene, n, w, h);
                fe_paint(p, &g_scene, px, w, fe_rect(0, 0, w, h), 8);
                write_ppm(dump_dir, k_scenes[n].name, w, h, px);
                free(px);
            }
        {
            int size = fe_cursor_size(3.0), hx, hy;
            uint32_t* px = surface(size, size, 0);
            if (px) {
                fe_paint_cursor(3.0, px, size, &hx, &hy);
                write_ppm(dump_dir, "cursor", size, size, px);
                free(px);
            }
        }
    }
    fe_painter_destroy(p);
}

static void test_cursor(void) {
    double scales[] = { 1.5, 2.25, 3.0, 4.5 };
    int last = 0;
    for (int i = 0; i < 4; i++) {
        int size = fe_cursor_size(scales[i]), hx, hy, opaque = 0;
        uint32_t* px = surface(size, size, 0xFFFFFFFFu);
        if (!px) continue;
        fe_paint_cursor(scales[i], px, size, &hx, &hy);
        CHECK(size > last, "the pointer does not grow with the screen");
        CHECK(hx >= 0 && hy >= 0 && hx < size / 4 && hy < size / 4, "hot spot %d,%d", hx, hy);
        CHECK((px[(long)(hy + 2) * size + hx + 1] >> 24) > 200, "the tip of the pointer is not drawn");
        CHECK((px[(long)(size - 1) * size + size - 1] >> 24) == 0, "the pointer's corner is not transparent");
        for (long k = 0; k < (long)size * size; k++) opaque += (px[k] >> 24) > 128;
        CHECK(opaque > size * size / 10, "the pointer is almost empty");
        last = size;
        free(px);
    }
}

int main(int argc, char** argv) {
    const char* dump = NULL;
    char path[MAX_PATH];
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    synthetic_art();
#ifdef JPT_SOURCE_DIR
    if (dump) {
        snprintf(path, sizeof path, "%s/game/data/menu/ms_bkgnd.tga", JPT_SOURCE_DIR);
        printf("retail main screen art: %s\n", load_tga(path, &g_backdrop[0][0], 640, 480) ? "loaded" : "not found");
        snprintf(path, sizeof path, "%s/user/TPass000.BMP", JPT_SOURCE_DIR);
        make_photo(path);
    }
#else
    (void)path;
#endif
    if (!g_photo.pixels) make_photo(NULL);
    test_view();
    test_mouse_mapping();
    test_options_layout();
    test_main_menu_layout();
    test_photo_fit();
    test_transitions();
    test_rows();
    test_slider();
    test_diff();
    test_cursor();
    test_painter(dump);
    frontend_bridge_tests(&g_checks, &g_failures);
    save_preview_tests(&g_checks, &g_failures);
    loading_tests(&g_checks, &g_failures, dump);
    printf("frontend_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
