/* The translated labels over the game's menu pictures: the English lettering found and taken out of every retail
 * button, title and the main screen's legal lines, and every language's label fitting its plate in a face that has
 * all of its characters. Needs the game files (skipped without them).
 *   frontend_tests.exe --dump <dir> also paints the main menu and the pause menu in every language. */
#include "frontend_art.h"
#include "frontend_assets.h"
#include "frontend_layout.h"
#include "frontend_plaque.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "loading_layout.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
#define ROOT JPT_SOURCE_DIR "/"
#else
#define ROOT ""
#endif
#define MENU_DIR ROOT "game/data/menu/"
#define TRANSLATIONS ROOT "assets/translations/"
#define VIEW_SCALE 4.0
#define MIN_LABEL_SCALE 0.62

typedef struct {
    const char* picture;
    const char* key;
    int x, y; /* where the pause or main menu puts it (window-relative for the pause menu) */
    uint8_t window;
} button_t;

static const button_t k_buttons[] = {
    { "ms_new", "Menu::NEW GAME", 205, 70, FE_WND_MAIN },        { "ms_load", "Menu::LOAD", 205, 160, FE_WND_MAIN },
    { "ms_options", "Menu::OPTIONS", 205, 250, FE_WND_MAIN },    { "ms_quit", "Menu::EXIT", 205, 340, FE_WND_MAIN },
    { "gm_restart", "Menu::RESTART LEVEL", 93, 142, FE_WND_GAME_MENU }, { "gm_save", "Menu::SAVE", 118, 26, FE_WND_GAME_MENU },
    { "gm_load", "Menu::LOAD", 22, 29, FE_WND_GAME_MENU },       { "gm_options", "Menu::OPTIONS", 38, 82, FE_WND_GAME_MENU },
    { "gm_quit", "Menu::QUIT", 211, 27, FE_WND_GAME_MENU },      { "gm_resume", "Menu::RESUME", 160, 82, FE_WND_GAME_MENU },
    { "desktop", "Menu::DESKTOP", 0, 0, 0 },                     { "qt_menu", "Menu::MAIN MENU", 0, 0, 0 },
    { "cancel", "Menu::CANCEL", 0, 0, 0 },                       { "ok", "Menu::OK", 0, 0, 0 },
    { "yes", "Menu::YES", 0, 0, 0 },                             { "no", "Menu::NO", 0, 0, 0 },
    { "delete", "Menu::DELETE", 0, 0, 0 },                       { "defaults", "Menu::DEFAULTS", 0, 0, 0 },
    { "gm_audio", "Menu::AUDIO", 0, 0, 0 },                      { "gm_controls", "Menu::CONTROLS", 0, 0, 0 },
    { "gm_credits", "Menu::CREDITS", 0, 0, 0 },                  { "close", "Menu::CLOSE", 0, 0, 0 },
    { "gm_video", "Menu::VIDEO", 0, 0, 0 },
};
#define BUTTON_COUNT ((int)(sizeof k_buttons / sizeof k_buttons[0]))

static const char* const k_languages[] = { "pt-BR", "fr", "de", "es", "ru", "it" };

typedef struct {
    int w, h;
    uint16_t* normal;
    uint16_t* hover;
    fp_plaque_t plaque;
    int ok;
} retail_button_t;

static retail_button_t g_buttons[BUTTON_COUNT];
static fp_plaque_t g_titles[2], g_footer;
static uint16_t *g_backdrop, *g_ingame;

static uint8_t* read_file(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* data = NULL;
    long size;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (data = (uint8_t*)malloc((size_t)size + 1))) {
        *n = fread(data, 1, (size_t)size, f);
        data[*n] = 0;
    }
    fclose(f);
    return data;
}

static uint16_t* load565(const char* name, int* w, int* h) {
    char path[MAX_PATH];
    size_t n = 0;
    uint8_t* data;
    uint32_t* px = NULL;
    uint16_t* out = NULL;
    snprintf(path, sizeof path, MENU_DIR "%s", name);
    data = read_file(path, &n);
    if (data && fa_tga_decode(data, n, &px, w, h) && (out = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)*w * *h)))
        for (int i = 0; i < *w * *h; i++) {
            uint32_t v = px[i];
            out[i] = (uint16_t)((((v >> 16) & 255) >> 3) << 11 | (((v >> 8) & 255) >> 2) << 5 | ((v & 255) >> 3));
        }
    free(px);
    free(data);
    return out;
}

static void source(fa_source_t* s, const uint16_t* bits, int w, int h, int keyed) {
    memset(s, 0, sizeof *s);
    s->bits = bits;
    s->w = w;
    s->h = h;
    s->pitch = w;
    s->keyed = keyed;
    s->key = bits[0];
}

static int near_black(const uint16_t* bits, int w, fe_rect_t r) {
    int n = 0;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++) {
            uint16_t v = bits[y * w + x];
            n += ((v >> 11) & 31) < 5 && ((v >> 5) & 63) < 10 && (v & 31) < 5;
        }
    return n;
}

/* Every retail button's lettering is found and taken out: hardly any of its black is left in its box. */
static void test_lettering_removed(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        char n0[64], n1[64];
        int w1, h1, before, after;
        fa_source_t a, b;
        retail_button_t* r = &g_buttons[i];
        snprintf(n0, sizeof n0, "%s_0.tga", k_buttons[i].picture);
        snprintf(n1, sizeof n1, "%s_1.tga", k_buttons[i].picture);
        r->normal = load565(n0, &r->w, &r->h);
        r->hover = load565(n1, &w1, &h1);
        CHECK(r->normal && r->hover && w1 == r->w && h1 == r->h, "%s: pictures", k_buttons[i].picture);
        if (!r->normal || !r->hover) continue;
        source(&a, r->normal, r->w, r->h, 1);
        source(&b, r->hover, w1, h1, 1);
        r->ok = fp_button(&a, &b, &r->plaque);
        CHECK(r->ok, "%s: lettering found", k_buttons[i].picture);
        if (!r->ok) continue;
        before = near_black(r->normal, r->w, r->plaque.letters);
        after = near_black(r->plaque.clean, r->w, r->plaque.letters);
        CHECK(before > 40 && after * 10 < before, "%s: %d of %d black pixels left", k_buttons[i].picture, after, before);
        CHECK(fe_rect_contains(r->plaque.room, (r->plaque.letters.left + r->plaque.letters.right) / 2,
                               (r->plaque.letters.top + r->plaque.letters.bottom) / 2) &&
                  r->plaque.room.right - r->plaque.room.left >= r->plaque.letters.right - r->plaque.letters.left,
              "%s: room around the letters", k_buttons[i].picture);
        CHECK(r->plaque.clean[0] == r->normal[0], "%s: transparency kept", k_buttons[i].picture);
    }
    {
        static const char* const names[2] = { "quit_bg.tga", "sound_bg.tga" };
        static const fe_rect_t zones[2] = { { 108, 14, 216, 40 }, { 26, 22, 130, 43 } };
        static const fe_rect_t rooms[2] = { { 96, 13, 234, 41 }, { 24, 20, 132, 44 } };
        for (int i = 0; i < 2; i++) {
            int w, h;
            uint16_t* bits = load565(names[i], &w, &h);
            fa_source_t a;
            CHECK(bits != NULL, "%s", names[i]);
            if (!bits) continue;
            source(&a, bits, w, h, 1);
            CHECK(fp_engraved(&a, zones[i], rooms[i], &g_titles[i]), "%s: title found", names[i]);
            free(bits);
        }
    }
    {
        int w, h;
        fa_source_t a;
        g_backdrop = load565("ms_bkgnd.tga", &w, &h);
        CHECK(g_backdrop && w == 640 && h == 480, "ms_bkgnd.tga");
        if (g_backdrop) {
            source(&a, g_backdrop, w, h, 0);
            CHECK(fp_light(&a, fe_rect(66, 452, 566, 480), &g_footer) && g_footer.lines == 2, "footer: 2 lines found");
        }
        g_ingame = load565("InGameOpt.tga", &w, &h);
    }
}

static int load_language(const char* code) {
    char path[MAX_PATH];
    size_t n = 0;
    uint8_t* data;
    const jpt_l10n_language_t* l = jpt_l10n_language(code);
    int count;
    snprintf(path, sizeof path, TRANSLATIONS "%s.interface.txt", code);
    data = read_file(path, &n);
    if (!data || !l) {
        free(data);
        return 0;
    }
    jpt_l10n_set_display_codepage(l->codepage);
    count = jpt_tr_load((const char*)data, n, l->codepage);
    free(data);
    return count > 0;
}

/* Every label fits its plate, smaller by no more than LABEL_TWO_LINES allows (two lines where it has a space), in a
 * face that has all of its characters. */
/* The loading screen's captions and message in every language, in the face and size the plate draws them: the captions
 * fit their row without being made smaller, the message the plaque's widest. */
typedef struct {
    char text[16][256];
    int has[16];
} loading_strings_t;

static void collect_loading(void* ctx, unsigned id, const char* utf8) {
    loading_strings_t* s = (loading_strings_t*)ctx;
    int slot = id == 10010 ? 0 : id == 10011 ? 1 : id == 37 ? 2 : -1;
    if (slot >= 0 && !s->has[slot]) {
        snprintf(s->text[slot], sizeof s->text[slot], "%s", utf8);
        s->has[slot] = 1;
    }
}

static void test_loading_fit(fe_painter_t* p) {
    ld_layout_t layout;
    const double u = 4.0;
    ld_layout_make(&layout, 2560, 1920, 2, 0);
    for (size_t l = 0; l < sizeof k_languages / sizeof k_languages[0]; l++) {
        loading_strings_t s;
        char path[MAX_PATH];
        size_t n = 0;
        uint8_t* data;
        const jpt_l10n_language_t* lang = jpt_l10n_language(k_languages[l]);
        memset(&s, 0, sizeof s);
        for (int port = 0; port < 2; port++) {
            snprintf(path, sizeof path, TRANSLATIONS "%s.strings%s.txt", k_languages[l], port ? ".port" : "");
            if ((data = read_file(path, &n)) != NULL) jpt_l10n_read_strings((const char*)data, n, collect_loading, &s);
            free(data);
        }
        jpt_l10n_set_display_codepage(lang->codepage);
        for (int k = 0; k < 3; k++) {
            char cp[256];
            double width, room = (layout.captions[0].right - layout.captions[0].left) * u;
            CHECK(s.has[k] && jpt_utf8_to_cp(lang->codepage, s.text[k], cp, sizeof cp) != (size_t)-1, "%s: loading text %d", k_languages[l], k);
            if (!s.has[k]) continue;
            jpt_utf8_to_cp(lang->codepage, s.text[k], cp, sizeof cp);
            CHECK(*fe_painter_face_for(p, FE_FONT_LABEL, cp), "%s: no face has the characters of \"%s\"", k_languages[l], s.text[k]);
            width = fe_painter_text_width(p, k == 2 ? FE_FONT_PLAQUE : FE_FONT_LABEL, FW_NORMAL, k == 2 ? (layout.message.bottom - layout.message.top) * 0.40 * u : 11.5 * u, cp);
            if (k < 2) CHECK(width <= room, "%s: \"%s\" is %.0f px wide in a row of %.0f", k_languages[l], s.text[k], width, room);
            else CHECK(width / u + 64.0 <= FE_CANVAS_W - 40.0, "%s: \"%s\" is wider than the message plaque", k_languages[l], s.text[k]);
        }
    }
    jpt_l10n_set_display_codepage(0);
}

static void test_labels_fit(fe_painter_t* p) {
    for (size_t l = 0; l < sizeof k_languages / sizeof k_languages[0]; l++) {
        CHECK(load_language(k_languages[l]), "%s: interface text", k_languages[l]);
        for (int i = 0; i < BUTTON_COUNT; i++) {
            const retail_button_t* r = &g_buttons[i];
            const char* text = jpt_tr_cp(k_buttons[i].key);
            int lines = 0;
            double scale;
            if (!r->ok) continue;
            scale = fe_painter_label_fit(p, FE_LABEL_STENCIL, text, (r->plaque.letters.bottom - r->plaque.letters.top) * VIEW_SCALE,
                                         (r->plaque.room.right - r->plaque.room.left) * VIEW_SCALE,
                                         (r->plaque.room.bottom - r->plaque.room.top) * VIEW_SCALE, &lines);
            CHECK(scale >= MIN_LABEL_SCALE, "%s %s: \"%s\" fits at %.2f", k_languages[l], k_buttons[i].key, jpt_tr(k_buttons[i].key), scale);
            CHECK(fe_painter_face_for(p, FE_FONT_PLAQUE, text)[0], "%s %s: no face has its characters", k_languages[l],
                  k_buttons[i].key);
        }
        for (int i = 0; i < 2; i++) {
            static const char* const keys[2] = { "Title::QUIT", "Title::SOUND" };
            const fp_plaque_t* t = &g_titles[i];
            const char* text = jpt_tr_cp(keys[i]);
            double scale;
            if (!t->clean) continue;
            scale = fe_painter_label_fit(p, FE_LABEL_ENGRAVED, text, (t->letters.bottom - t->letters.top) * VIEW_SCALE,
                                         (t->room.right - t->room.left) * VIEW_SCALE, (t->room.bottom - t->room.top) * VIEW_SCALE,
                                         NULL);
            CHECK(scale >= MIN_LABEL_SCALE, "%s %s fits at %.2f", k_languages[l], keys[i], scale);
            CHECK(fe_painter_face_for(p, FE_FONT_TITLE, text)[0], "%s %s: face", k_languages[l], keys[i]);
        }
        CHECK(fe_painter_face_for(p, FE_FONT_UI, jpt_tr_cp("Footer::\xC2\xA9 1998 DreamWorks Interactive L.L.C. Trespasser is a "
                                                           "trademark of UCS & Amblin.\nThe Lost World: Jurassic Park\xE2\x84\xA2"
                                                           " & \xC2\xA9 1998 UCS & Amblin.  All Rights Reserved."))[0],
              "%s footer: face", k_languages[l]);
    }
    jpt_tr_reset();
    jpt_l10n_set_display_codepage(0);
}

static void put_button(fe_scene_t* s, const button_t* b, const retail_button_t* r, int dx, int dy, uint16_t state) {
    fe_widget_t* w = fe_scene_widget(s, FE_W_BUTTON, 1000, fe_rect(b->x + dx, b->y + dy, b->x + dx + r->w, b->y + dy + r->h));
    if (!w) return;
    w->style = FE_BUTTON_PLAQUE;
    w->state = state;
    w->image = r->plaque.clean;
    w->image_w = r->w;
    w->image_h = r->h;
    w->image_pitch = r->w;
    w->image_key = r->plaque.clean[0];
    w->image_keyed = 1;
    w->image_hash = fe_hash(0, r->plaque.clean, sizeof(uint16_t) * (size_t)r->w * r->h);
    w->label = FE_LABEL_STENCIL;
    w->text = fe_scene_text(s, jpt_tr_cp(b->key));
    w->label_box = fe_rect_offset(r->plaque.letters, w->rect.left, w->rect.top);
    w->label_room = fe_rect_offset(r->plaque.room, w->rect.left, w->rect.top);
}

static void write_ppm(const char* dir, const char* name, int w, int h, const uint32_t* px) {
    char path[MAX_PATH];
    FILE* f;
    snprintf(path, sizeof path, "%s\\%s.ppm", dir, name);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

/* The main menu and the pause menu in every language, hover on the second button, pressed on the third. */
static void dump_menus(fe_painter_t* p, const char* dir) {
    static fe_scene_t s;
    const int sw = 2560, sh = 1920;
    uint32_t* px = (uint32_t*)malloc(sizeof(uint32_t) * sw * sh);
    if (!px || !g_backdrop) {
        free(px);
        return;
    }
    for (size_t l = 0; l < sizeof k_languages / sizeof k_languages[0]; l++) {
        char name[64];
        int k = 0;
        if (!load_language(k_languages[l])) continue;
        fe_scene_reset(&s, sw, sh);
        fe_scene_window(&s, FE_WND_MAIN, fe_rect(0, 0, 640, 480));
        s.backdrop = g_backdrop;
        s.backdrop_pitch = 640;
        s.backdrop_hash = (uint32_t)l + 1;
        if (g_footer.clean) {
            s.backdrop = g_footer.clean;
            s.footer_text = fe_scene_text(&s, jpt_tr_cp("Footer::\xC2\xA9 1998 DreamWorks Interactive L.L.C. Trespasser is a "
                                                        "trademark of UCS & Amblin.\nThe Lost World: Jurassic Park\xE2\x84\xA2"
                                                        " & \xC2\xA9 1998 UCS & Amblin.  All Rights Reserved."));
            s.footer_box = fe_rect(66, g_footer.letters.top, 566, g_footer.letters.bottom);
        }
        for (int i = 0; i < BUTTON_COUNT; i++)
            if (k_buttons[i].window == FE_WND_MAIN && g_buttons[i].ok)
                put_button(&s, &k_buttons[i], &g_buttons[i], 0, 0, (uint16_t)(k == 1 ? FE_ST_HOVER : k == 2 ? FE_ST_PRESSED : 0)), k++;
        memset(px, 0, sizeof(uint32_t) * sw * sh);
        if (fe_paint(p, &s, px, sw, fe_rect(0, 0, sw, sh), 4)) {
            snprintf(name, sizeof name, "l10n_main_%s", k_languages[l]);
            write_ppm(dir, name, sw, sh, px);
        }
        fe_scene_reset(&s, sw, sh);
        fe_scene_window(&s, FE_WND_GAME_MENU, fe_rect(160, 140, 480, 340));
        if (g_ingame) {
            fe_widget_t* w = fe_scene_widget(&s, FE_W_IMAGE, -1, fe_rect(160, 140, 480, 340));
            w->image = g_ingame;
            w->image_w = 320;
            w->image_h = 200;
            w->image_pitch = 320;
            w->image_key = g_ingame[0];
            w->image_keyed = 1;
            w->image_hash = 77;
        }
        k = 0;
        for (int i = 0; i < BUTTON_COUNT; i++)
            if (k_buttons[i].window == FE_WND_GAME_MENU && g_buttons[i].ok)
                put_button(&s, &k_buttons[i], &g_buttons[i], 160, 140, (uint16_t)(k == 1 ? FE_ST_HOVER : 0)), k++;
        memset(px, 0, sizeof(uint32_t) * sw * sh);
        if (fe_paint(p, &s, px, sw, fe_rect(0, 0, sw, sh), 4)) {
            snprintf(name, sizeof name, "l10n_pause_%s", k_languages[l]);
            write_ppm(dir, name, sw, sh, px);
        }
    }
    free(px);
    jpt_tr_reset();
    jpt_l10n_set_display_codepage(0);
}

void frontend_l10n_tests(int* checks, int* failures, const char* dump_dir) {
    char probe[MAX_PATH];
    fe_painter_t* p;
    g_checks = checks;
    g_failures = failures;
    snprintf(probe, sizeof probe, MENU_DIR "ms_new_1.tga");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
        printf("translated labels: game files not found, skipped\n");
        return;
    }
    test_lettering_removed();
    p = fe_painter_create();
    CHECK(p != NULL, "painter");
    if (!p) return;
    test_labels_fit(p);
    test_loading_fit(p);
    if (dump_dir) dump_menus(p, dump_dir);
    fe_painter_destroy(p);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        free(g_buttons[i].normal);
        free(g_buttons[i].hover);
        fp_free(&g_buttons[i].plaque);
    }
    fp_free(&g_titles[0]);
    fp_free(&g_titles[1]);
    fp_free(&g_footer);
    free(g_backdrop);
    free(g_ingame);
}
