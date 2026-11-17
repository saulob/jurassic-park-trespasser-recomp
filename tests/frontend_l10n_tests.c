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
#define ROOT JPT_SOURCE_DIR "/"
#else
#define ROOT ""
#endif
#define MENU_DIR ROOT "game/data/menu/"
#define TRANSLATIONS ROOT "assets/translations/"
#define VIEW_SCALE 4.0
#define MIN_LABEL_SCALE 0.62
#define MAX_STRIPE 9.0         /* the largest jump of row brightness across a cleaned band (the old fill left 12 to 124) */
#define MAX_RIDGED_STRIPE 20.0 /* the Desktop plate is ridged by itself */
#define MAX_TITLE_STRIPE 12.0
#define MAX_GHOST 8.0
#define MAX_INK_LEFT 8
#define MAX_NEIGHBOUR_INK 24
#define MAX_MAIN_NEIGHBOUR_INK 60 /* the main plaques' torn corners are black */

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
    uint16_t* pressed;  /* the retail pictures of the pressed and disabled states, when the button has them */
    uint16_t* disabled;
    uint16_t* tone[4];  /* the clean picture in the tones of each state's picture (tone[0] is plaque.clean) */
    fp_plaque_t plaque;
    int ok;
} retail_button_t;

static retail_button_t g_buttons[BUTTON_COUNT];
static fp_plaque_t g_titles[2], g_footer;
static uint16_t* g_title_picture[2];
static int g_title_w[2], g_title_h[2];
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

static uint16_t* load565_at(const char* path, int* w, int* h) {
    size_t n = 0;
    uint8_t* data;
    uint32_t* px = NULL;
    uint16_t* out = NULL;
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

static uint16_t* load565(const char* name, int* w, int* h) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, MENU_DIR "%s", name);
    return load565_at(path, w, h);
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

static double luma565(uint16_t v) {
    return (((v >> 11) & 31) * 255.0 / 31.0 + ((v >> 5) & 63) * 255.0 / 63.0 + (v & 31) * 255.0 / 31.0) / 3.0;
}

/* How well a plate was cleaned inside box (the words' band): stripe is the largest jump between the mean brightness of
 * neighbouring rows, ghost the brightness difference between the pixels the normal picture's letters covered and the
 * plate between them; dark counts the pixels of the letters that are still ink black. */
static void plate_quality(const uint16_t* clean, const uint16_t* normal, int w, fe_rect_t box, double* stripe, double* ghost, int* dark) {
    double rows[64], ink = 0.0, rest = 0.0;
    int count = 0, ink_n = 0, rest_n = 0;
    fe_rect_t in = fe_rect_inflate(box, -2, -1);
    *stripe = *ghost = 0.0;
    *dark = 0;
    for (int y = in.top; y < in.bottom && count < 64; y++) {
        double sum = 0.0;
        for (int x = in.left; x < in.right; x++) {
            double v = luma565(clean[y * w + x]);
            sum += v;
            if (luma565(normal[y * w + x]) < 64.0) ink += v, ink_n++, *dark += v < 40.0;
            else rest += v, rest_n++;
        }
        rows[count++] = sum / (double)(in.right - in.left);
    }
    for (int i = 1; i < count; i++)
        if (fabs(rows[i] - rows[i - 1]) > *stripe) *stripe = fabs(rows[i] - rows[i - 1]);
    if (ink_n >= 30 && rest_n >= 30) *ghost = fabs(ink / ink_n - rest / rest_n);
}

static void make_tones(retail_button_t* r, const char* picture) {
    char name[64];
    int w, h;
    snprintf(name, sizeof name, "%s_2.tga", picture);
    r->pressed = load565(name, &w, &h);
    snprintf(name, sizeof name, "%s_3.tga", picture);
    r->disabled = load565(name, &w, &h);
    r->tone[0] = r->plaque.clean;
    for (int k = 1; k < 4; k++) {
        const uint16_t* bits = k == 1 ? r->hover : k == 2 ? r->pressed : r->disabled;
        fa_source_t a;
        if (!bits) continue;
        source(&a, bits, r->w, r->h, 1);
        r->tone[k] = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)r->w * r->h);
        if (r->tone[k] && !fp_tone(&r->plaque, &a, r->tone[k])) free(r->tone[k]), r->tone[k] = NULL;
    }
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
        make_tones(r, k_buttons[i].picture);
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
            if (g_titles[i].clean) {
                double stripe, ghost;
                int dark;
                plate_quality(g_titles[i].clean, bits, w, zones[i], &stripe, &ghost, &dark);
                CHECK(stripe <= MAX_TITLE_STRIPE, "%s: a stripe of %.1f levels across the cleaned title", names[i], stripe);
            }
            g_title_picture[i] = bits;
            g_title_w[i] = w;
            g_title_h[i] = h;
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

/* No ghost of the lettering is left in any state's picture: no letter-shaped outline, no ink, no stripes across the
 * words' band. */
static void test_clean_plates(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        const retail_button_t* r = &g_buttons[i];
        const uint16_t* state[4] = { r->normal, r->hover, r->pressed, r->disabled };
        if (!r->ok) continue;
        for (int k = 0; k < 4; k++) {
            double stripe, ghost;
            int dark, left = 0;
            fe_rect_t side;
            if (!state[k] || !r->tone[k]) continue;
            plate_quality(r->tone[k], r->normal, r->w, r->plaque.letters, &stripe, &ghost, &dark);
            side = fe_rect_intersect(fe_rect_inflate(r->plaque.letters, r->plaque.letters.bottom - r->plaque.letters.top, 0), r->plaque.room);
            for (int y = side.top; y < side.bottom; y++)
                for (int x = side.left; x < side.right; x++)
                    left += luma565(r->normal[y * r->w + x]) < 20.0 && luma565(r->tone[k][y * r->w + x]) < 24.0;
            CHECK(stripe <= (strcmp(k_buttons[i].picture, "desktop") ? MAX_STRIPE : MAX_RIDGED_STRIPE),
                  "%s state %d: a stripe of %.1f levels across the cleaned band", k_buttons[i].picture, k, stripe);
            CHECK(ghost <= MAX_GHOST, "%s state %d: the old letters outline themselves by %.1f levels", k_buttons[i].picture, k, ghost);
            CHECK(dark <= MAX_INK_LEFT, "%s state %d: %d ink-black pixels left under the old letters", k_buttons[i].picture, k, dark);
            CHECK(left <= (k_buttons[i].window == FE_WND_MAIN ? MAX_MAIN_NEIGHBOUR_INK : MAX_NEIGHBOUR_INK),
                  "%s state %d: %d pixels of a letter the search missed are left", k_buttons[i].picture, k, left);
        }
    }
}

/* A plate with a bright and a dark edge above and below the hole: the fill has no edge in it, keeps the plate's grain
 * and leaves everything outside the hole as it was. */
static void test_fill_edges(void) {
    enum { W = 64, H = 40 };
    static float rgb[W * H * 3], before[W * H * 3];
    static uint8_t valid[W * H], hole[W * H];
    uint32_t seed = 7;
    double rows[H], mean = 0.0, sq = 0.0, jump = 0.0;
    int n = 0, changed = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float v = 110.0f + (float)y * 0.8f;
            seed = seed * 1664525u + 1013904223u;
            v += (float)((seed >> 16) & 15) - 7.5f;
            if (y == 2 || y == 36) v += 70.0f;
            if (y == 4 || y == 34) v -= 50.0f;
            for (int c = 0; c < 3; c++) rgb[(y * W + x) * 3 + c] = v + (float)c * 6.0f;
            valid[y * W + x] = 1;
            hole[y * W + x] = y >= 14 && y < 27 && x >= 14 && x < 50;
        }
    memcpy(before, rgb, sizeof rgb);
    fp_fill(rgb, valid, hole, W, H);
    for (int y = 0; y < H; y++) {
        double sum = 0.0;
        int cnt = 0;
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            for (int c = 0; c < 3; c++) changed += !hole[i] && rgb[i * 3 + c] != before[i * 3 + c];
            if (!hole[i]) continue;
            sum += rgb[i * 3];
            mean += rgb[i * 3];
            sq += rgb[i * 3] * rgb[i * 3];
            n++, cnt++;
        }
        rows[y] = cnt ? sum / cnt : 0.0;
    }
    for (int y = 15; y < 27; y++) jump = fmax(jump, fabs(rows[y] - rows[y - 1]));
    mean /= n;
    CHECK(changed == 0, "the fill changed %d values outside the hole", changed);
    CHECK(jump < 6.0, "an edge of %.1f levels was copied into the hole", jump);
    CHECK(sqrt(sq / n - mean * mean) > 2.0, "the filled hole is flat (spread %.1f)", sqrt(sq / n - mean * mean));
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
    int slot;
    fe_widget_t* w = fe_scene_widget(s, FE_W_BUTTON, 1000, fe_rect(b->x + dx, b->y + dy, b->x + dx + r->w, b->y + dy + r->h));
    if (!w) return;
    w->style = FE_BUTTON_PLAQUE;
    w->state = state;
    slot = (state & FE_ST_DISABLED) ? 3 : (state & FE_ST_PRESSED) ? 2 : (state & FE_ST_HOVER) ? 1 : 0;
    if (!r->tone[slot]) return;
    w->image = r->tone[slot];
    w->image_w = r->w;
    w->image_h = r->h;
    w->image_pitch = r->w;
    w->image_key = r->plaque.clean[0];
    w->image_keyed = 1;
    w->image_hash = fe_hash(slot, r->tone[slot], sizeof(uint16_t) * (size_t)r->w * r->h);
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

/* Every button in every language with its label, in every state it has pictures for: a row per button, a column per
 * state (normal, hover, pressed, disabled). */
static void dump_labelled(fe_painter_t* p, const char* dir) {
    static fe_scene_t s;
    const int sw = 2560, sh = 1920;
    uint32_t* px = (uint32_t*)malloc(sizeof(uint32_t) * sw * sh);
    if (!px) return;
    for (size_t l = 0; l < sizeof k_languages / sizeof k_languages[0]; l++) {
        int sheet = 0, next = 0;
        if (!load_language(k_languages[l])) continue;
        while (next < BUTTON_COUNT) {
            int row = 0, y = 4, big = 0;
            char name[64];
            fe_scene_reset(&s, sw, sh);
            fe_scene_window(&s, FE_WND_MAIN, fe_rect(0, 0, 640, 480));
            for (; next < BUTTON_COUNT; next++) {
                const retail_button_t* r = &g_buttons[next];
                button_t b = k_buttons[next];
                if (!r->ok) continue;
                if (row && (big != (r->w > 150) || y + r->h + 4 > 480)) break;
                big = r->w > 150;
                for (int k = 0; k < 4; k++) {
                    static const uint16_t states[4] = { 0, FE_ST_HOVER, FE_ST_PRESSED, FE_ST_DISABLED };
                    if (!r->tone[k]) continue;
                    b.x = 4 + k * (big ? 212 : 156);
                    b.y = y;
                    put_button(&s, &b, r, 0, 0, states[k]);
                }
                y += r->h + 4;
                row++;
            }
            if (!row) break;
            memset(px, 0x50, sizeof(uint32_t) * sw * sh);
            if (fe_paint(p, &s, px, sw, fe_rect(0, 0, sw, sh), 4)) {
                snprintf(name, sizeof name, "buttons_%s_%d", k_languages[l], sheet);
                write_ppm(dir, name, sw, sh, px);
            }
            sheet++;
        }
    }
    free(px);
    jpt_tr_reset();
    jpt_l10n_set_display_codepage(0);
}

/* The Controls window draws its plaques 1.75 times larger: the label must stay centred in the plaque as drawn. */
static void test_controls_buttons(fe_painter_t* p) {
    static const char* const names[] = { "ok", "defaults", "cancel" };
    static fe_scene_t s;
    const double tolerance = 2.5, margin = 3.0;
    for (size_t l = 0; l < sizeof k_languages / sizeof k_languages[0]; l++) {
        if (!load_language(k_languages[l])) continue;
        for (size_t k = 0; k < 3; k++) {
            const retail_button_t* r = NULL;
            const button_t* b = NULL;
            fe_window_t* w;
            const fe_widget_t* x;
            fe_rect_t plaque;
            double scale;
            int lines = 0;
            for (int i = 0; i < BUTTON_COUNT; i++)
                if (!strcmp(k_buttons[i].picture, names[k])) r = &g_buttons[i], b = &k_buttons[i];
            if (!r || !r->ok) continue;
            fe_scene_reset(&s, 1280, 960);
            w = fe_scene_window(&s, FE_WND_CONTROLS, fe_rect(0, 0, 640, 480));
            put_button(&s, b, r, 200, 300, 0);
            fe_scene_finish_window(&s, w);
            fe_controls_polish(&s, w, FE_CONTROLS_ZOOM);
            x = &s.widgets[w->first];
            plaque = fe_rect(x->rect.left, x->rect.top, x->rect.left + (int)floor(r->w * FE_CONTROLS_ZOOM + 0.5),
                             x->rect.top + (int)floor(r->h * FE_CONTROLS_ZOOM + 0.5));
            CHECK(fabs((x->label_box.left + x->label_box.right) - (plaque.left + plaque.right)) / 2.0 <= tolerance,
                  "%s %s: label centre %.1f px off horizontally", k_languages[l], names[k],
                  ((x->label_box.left + x->label_box.right) - (plaque.left + plaque.right)) / 2.0);
            CHECK(fabs((x->label_box.top + x->label_box.bottom) - (plaque.top + plaque.bottom)) / 2.0 <= tolerance,
                  "%s %s: label centre %.1f px off vertically", k_languages[l], names[k],
                  ((x->label_box.top + x->label_box.bottom) - (plaque.top + plaque.bottom)) / 2.0);
            CHECK(x->label_room.left >= plaque.left + margin && x->label_room.right <= plaque.right - margin &&
                      x->label_room.top >= plaque.top + margin && x->label_room.bottom <= plaque.bottom - margin,
                  "%s %s: room %d,%d-%d,%d outside plaque %d,%d-%d,%d", k_languages[l], names[k], x->label_room.left, x->label_room.top,
                  x->label_room.right, x->label_room.bottom, plaque.left, plaque.top, plaque.right, plaque.bottom);
            scale = fe_painter_label_fit(p, FE_LABEL_STENCIL, jpt_tr_cp(b->key), (x->label_box.bottom - x->label_box.top) * VIEW_SCALE,
                                         (x->label_room.right - x->label_room.left) * VIEW_SCALE,
                                         (x->label_room.bottom - x->label_room.top) * VIEW_SCALE, &lines);
            CHECK(scale >= MIN_LABEL_SCALE, "%s %s: fits at %.2f in %d lines", k_languages[l], names[k], scale, lines);
        }
    }
    jpt_tr_reset();
    jpt_l10n_set_display_codepage(0);
}

/* Each button's pictures beside the same pictures without their lettering: every state's, at four times their size. */
static void dump_plaques(const char* dir) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        const retail_button_t* r = &g_buttons[i];
        const uint16_t* state[4] = { r->normal, r->hover, r->pressed, r->disabled };
        int rows = 0, sw, sh;
        uint32_t* px;
        char name[64];
        if (!r->ok) continue;
        for (int k = 0; k < 4; k++) rows += state[k] != NULL;
        sw = (r->w * 2 + 3) * 4;
        sh = (r->h + 2) * rows * 4;
        px = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)sw * sh);
        if (!px) continue;
        for (int j = 0; j < sw * sh; j++) px[j] = 0x00805090;
        for (int k = 0, row = 0; k < 4; k++) {
            if (!state[k]) continue;
            for (int side = 0; side < 2; side++) {
                const uint16_t* bits = side ? r->tone[k] : state[k];
                if (!bits) continue;
                for (int y = 0; y < r->h * 4; y++)
                    for (int x = 0; x < r->w * 4; x++) {
                        uint16_t v = bits[(y / 4) * r->w + x / 4];
                        if (v == r->normal[0]) continue;
                        px[(size_t)(row * (r->h + 2) * 4 + y) * sw + side * (r->w + 1) * 4 + x] =
                            (uint32_t)(((v >> 11) & 31) * 255 / 31) << 16 | (uint32_t)(((v >> 5) & 63) * 255 / 63) << 8 |
                            (uint32_t)((v & 31) * 255 / 31);
                    }
            }
            row++;
        }
        snprintf(name, sizeof name, "plaque_%s", k_buttons[i].picture);
        write_ppm(dir, name, sw, sh, px);
        free(px);
    }
}

/* The engraved titles and the main screen's legal lines beside the same pictures without their lettering, at four times
 * their size. */
static void dump_titles(const char* dir) {
    for (int i = 0; i < 3; i++) {
        const uint16_t* before = i < 2 ? g_title_picture[i] : g_backdrop;
        const fp_plaque_t* p = i < 2 ? &g_titles[i] : &g_footer;
        fe_rect_t area = i < 2 ? p->room : fe_rect(0, 440, 640, 480);
        int w = i < 2 ? g_title_w[i] : 640, aw = area.right - area.left, ah = area.bottom - area.top, sw = aw * 4, sh = (ah * 2 + 1) * 4;
        uint32_t* px;
        char name[64];
        if (!before || !p->clean) continue;
        px = (uint32_t*)calloc((size_t)sw * sh, sizeof(uint32_t));
        if (!px) continue;
        for (int row = 0; row < 2; row++)
            for (int y = 0; y < ah * 4; y++)
                for (int x = 0; x < aw * 4; x++) {
                    uint16_t v = (row ? p->clean : before)[(area.top + y / 4) * w + area.left + x / 4];
                    px[(size_t)(row * (ah + 1) * 4 + y) * sw + x] = (uint32_t)(((v >> 11) & 31) * 255 / 31) << 16 |
                                                                    (uint32_t)(((v >> 5) & 63) * 255 / 63) << 8 | (uint32_t)((v & 31) * 255 / 31);
                }
        snprintf(name, sizeof name, "title_%d", i);
        write_ppm(dir, name, sw, sh, px);
        free(px);
    }
}

/* The Brazilian release's own credits picture is lettered "AREA DE TRABALHO": its lettering is found like any other, and
 * only that version and button get the corrected label. */
static void test_ptbr_credits_label(void) {
    static const char dir[] = ROOT "game/versions/ptbr-117/data/menu/";
    char path[MAX_PATH];
    int w, h, w1, h1;
    uint16_t *normal, *hover;
    fa_source_t a, b;
    fp_plaque_t plaque;
    CHECK(!strcmp(fe_release_label_fix("ptbr-117", FE_WND_OPTIONS, 1003), "CR\xC9" "DITOS"), "the Brazilian credits button reads CREDITOS");
    CHECK(!fe_release_label_fix("ptbr-117", FE_WND_OPTIONS, 1002) && !fe_release_label_fix("ptbr-117", FE_WND_GAME_MENU, 1003), "only that button");
    CHECK(!fe_release_label_fix("en-116", FE_WND_OPTIONS, 1003) && !fe_release_label_fix("ru-116", FE_WND_OPTIONS, 1003) &&
              !fe_release_label_fix("es-117", FE_WND_OPTIONS, 1003) && !fe_release_label_fix(NULL, FE_WND_OPTIONS, 1003),
          "no other version");
    {
        static const struct {
            const char* id;
            const char* native;
        } releases[] = { { "en-116", "en" }, { "ru-116", "ru" }, { "es-117", "es" }, { "ptbr-117", "pt-BR" } };
        static const char* const chosen[] = { "en", "ru", "es", "pt-BR", "fr" };
        for (size_t v = 0; v < sizeof releases / sizeof releases[0]; v++)
            for (size_t l = 0; l < sizeof chosen / sizeof chosen[0]; l++) {
                const int native = !strcmp(releases[v].native, chosen[l]);
                int fixes = 0, relabels = 0;
                for (int kind = 0; kind < 16; kind++)
                    for (int id = 1000; id < 1012; id++) {
                        const char* text = fe_button_text(native, "TRANSLATED", fe_release_label_fix(releases[v].id, kind, id));
                        fixes += native && text != NULL;
                        relabels += !native && text != NULL && !strcmp(text, "TRANSLATED");
                    }
                if (native) CHECK(fixes == !strcmp(releases[v].id, "ptbr-117"), "%s in %s: the release's own menus, only the Credits fix", releases[v].id, chosen[l]);
                else CHECK(relabels == 16 * 12, "%s in %s: the port's labels for another language", releases[v].id, chosen[l]);
            }
    }
    snprintf(path, sizeof path, "%sgm_credits_0.tga", dir);
    normal = load565_at(path, &w, &h);
    snprintf(path, sizeof path, "%sgm_credits_1.tga", dir);
    hover = load565_at(path, &w1, &h1);
    if (!normal || !hover) {
        printf("Brazilian credits picture: game files not found, skipped\n");
        free(normal);
        free(hover);
        return;
    }
    source(&a, normal, w, h, 1);
    source(&b, hover, w1, h1, 1);
    memset(&plaque, 0, sizeof plaque);
    CHECK(w == w1 && h == h1 && fp_button(&a, &b, &plaque), "the lettering of the Brazilian credits picture is found");
    fp_free(&plaque);
    free(normal);
    free(hover);
}

void frontend_l10n_tests(int* checks, int* failures, const char* dump_dir) {
    char probe[MAX_PATH];
    fe_painter_t* p;
    g_checks = checks;
    g_failures = failures;
    test_fill_edges();
    test_ptbr_credits_label();
    snprintf(probe, sizeof probe, MENU_DIR "ms_new_1.tga");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
        printf("translated labels: game files not found, skipped\n");
        return;
    }
    test_lettering_removed();
    test_clean_plates();
    p = fe_painter_create();
    CHECK(p != NULL, "painter");
    if (!p) return;
    test_labels_fit(p);
    test_loading_fit(p);
    test_controls_buttons(p);
    if (dump_dir) dump_menus(p, dump_dir), dump_plaques(dump_dir), dump_labelled(p, dump_dir), dump_titles(dump_dir);
    fe_painter_destroy(p);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        free(g_buttons[i].normal);
        free(g_buttons[i].hover);
        free(g_buttons[i].pressed);
        free(g_buttons[i].disabled);
        for (int k = 1; k < 4; k++) free(g_buttons[i].tone[k]);
        fp_free(&g_buttons[i].plaque);
    }
    free(g_title_picture[0]);
    free(g_title_picture[1]);
    fp_free(&g_titles[0]);
    fp_free(&g_titles[1]);
    fp_free(&g_footer);
    free(g_backdrop);
    free(g_ingame);
}
