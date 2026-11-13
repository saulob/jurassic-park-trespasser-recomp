/* Headless checks of the HD loading screen: the retail inventory and the game's choice of picture, the layout on every
 * screen shape, what a progress step repaints, the painter's output, and HD art files (missing, corrupt, oversized).
 * With --dump, the loading screens of every level are written as .ppm pictures (the retail art when it is installed). */
#include "frontend_assets.h"
#include "host_policy.h"
#include "loading_layout.h"
#include "save_preview.h"

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

typedef struct {
    const char* aspect;
    int w, h;
} screen_t;

static const screen_t k_screens[] = {
    { "4:3", 1024, 768 },   { "4:3", 2560, 1920 },  { "16:9", 1280, 720 },   { "16:9", 1920, 1080 },
    { "16:9", 2560, 1440 }, { "16:9", 3840, 2160 }, { "16:10", 1920, 1200 }, { "16:10", 2560, 1600 },
    { "21:9", 2560, 1080 }, { "21:9", 3440, 1440 }, { "32:9", 3840, 1080 },  { "32:9", 5120, 1440 },
    { "5:4", 1280, 1024 },  { "4:3", 800, 600 },
};
#define SCREEN_COUNT (int)(sizeof k_screens / sizeof k_screens[0])

static uint16_t g_art[480][640];

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

/* A level picture: sky over a green slope, with a dithered gradient as the 1998 renders have. */
static void synthetic_art(void) {
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            int d = ((x ^ y) & 1) * 6;
            g_art[y][x] = y < 200 + x / 8 ? rgb565(70 + y / 4 + d, 150 + y / 6, 220) : rgb565(60 + d, 110 + x / 12, 40 + d);
        }
}

/* menu\li_*_0.tga: uncompressed 16-bit (ARGB1555), rows from the bottom. */
static int load_li_tga(const char* path, uint16_t* out) {
    unsigned char h[18];
    FILE* f = fopen(path, "rb");
    int ok = 0;
    if (!f) return 0;
    if (fread(h, 1, 18, f) == 18 && h[2] == 2 && h[16] == 16 && (h[12] | h[13] << 8) == 640 && (h[14] | h[15] << 8) == 480) {
        fseek(f, 18 + h[0], SEEK_SET);
        ok = 1;
        for (int y = 0; y < 480 && ok; y++) {
            uint16_t row[640];
            int to = (h[17] & 0x20) ? y : 479 - y;
            if (fread(row, 2, 640, f) != 640) ok = 0;
            for (int x = 0; ok && x < 640; x++) {
                uint16_t v = row[x];
                out[to * 640 + x] = (uint16_t)(((v >> 10) & 31) << 11 | ((v >> 5) & 31) << 6 | (v & 31));
            }
        }
    }
    fclose(f);
    return ok;
}

static void test_inventory(void) {
    const char* scenes[LD_TABLE_ROWS];
    char slot[LD_SLOT_BYTES];
    static const char* const k_levels[][2] = { { "BE.SCN", "li_be" },  { "be.scn", "li_be" },   { "Jr.Scn", "li_jr" },
                                               { "pv.scn", "li_pv" },  { "ij.scn", "li_ij" },   { "it.scn", "li_it" },
                                               { "lab.scn", "li_lab" }, { "as.scn", "li_a1" },  { "as2.scn", "li_a2" },
                                               { "sum.scn", "li_sum" }, { "TestScene.scn", "li_other" },
                                               { "TestScnNght.scn", "li_other" }, { "", "li_other" },
                                               { "data\\be.scn", "li_other" }, { "as2", "li_other" } };
    for (int i = 0; i < LD_TABLE_ROWS; i++) scenes[i] = ld_retail_table[i].scene;
    CHECK(LD_TABLE_ROWS == 10 && !ld_retail_table[LD_TABLE_ROWS - 1].scene[0], "ten rows, the last one the catch-all");
    for (int i = 0; i < (int)(sizeof k_levels / sizeof k_levels[0]); i++) {
        int row = ld_select(k_levels[i][0], scenes, LD_TABLE_ROWS);
        CHECK(row >= 0 && ld_slot_name(ld_retail_table[row].image, slot, sizeof slot) && !strcmp(slot, k_levels[i][1]),
              "scene \"%s\" shows %s, got %s", k_levels[i][0], k_levels[i][1], row >= 0 ? slot : "nothing");
    }
    CHECK(ld_select(NULL, scenes, LD_TABLE_ROWS) == LD_TABLE_ROWS - 1, "no scene name shows the catch-all");
    CHECK(ld_select("be.scn", scenes, 0) == -1, "an empty table selects nothing");
    CHECK(ld_slot_name("li_be", slot, sizeof slot) && !strcmp(slot, "li_be"), "a plain name is its own slot");
    CHECK(ld_slot_name("..\\..\\li_be", slot, sizeof slot) && !strcmp(slot, "li_be"), "only the file name makes the slot");
    CHECK(!ld_slot_name("menu\\li_be.tga", slot, sizeof slot) && !slot[0], "a name with a dot is no slot");
    CHECK(!ld_slot_name("menu\\", slot, sizeof slot), "an empty name is no slot");
    CHECK(!ld_slot_name("menu\\li_a_very_long_slot_name_for_this", slot, 16), "a name longer than the slot is refused");
    CHECK(!ld_slot_name(NULL, slot, sizeof slot), "no image, no slot");
#ifdef JPT_SOURCE_DIR
    {
        char path[MAX_PATH];
        FILE* f;
        snprintf(path, sizeof path, "%s/game/trespass.exe", JPT_SOURCE_DIR);
        f = fopen(path, "rb");
        if (f) {
            /* the retail .data (0x0065D000) is stored at file offset 0x0025D000 */
            static const long k_table_file = 0x006610D0L - 0x0065D000L + 0x0025D000L;
            char row[0x28];
            int same = 1;
            fseek(f, k_table_file, SEEK_SET);
            for (int i = 0; i < LD_TABLE_ROWS && same; i++)
                same = fread(row, 1, sizeof row, f) == sizeof row && !strcmp(row, ld_retail_table[i].scene) &&
                       !strcmp(row + 0x14, ld_retail_table[i].image);
            fclose(f);
            CHECK(same, "the host's table is the retail executable's table at 0x006610D0");
        }
        for (int i = 0; i < LD_TABLE_ROWS; i++) {
            static uint16_t art[480 * 640];
            snprintf(path, sizeof path, "%s/game/data/%s_0.tga", JPT_SOURCE_DIR, ld_retail_table[i].image);
            for (char* c = path; *c; c++)
                if (*c == '\\') *c = '/';
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
            CHECK(load_li_tga(path, art), "%s is a 640x480 16-bit picture", path);
        }
    }
#endif
}

static fe_rect_t screen_rect(const ld_layout_t* l, fe_rect_t r) { return fe_view_rect(&l->view, r); }

static int inside(fe_rect_t outer, fe_rect_t r) {
    return r.left >= outer.left && r.top >= outer.top && r.right <= outer.right && r.bottom <= outer.bottom && !fe_rect_empty(r);
}

static void test_layout(void) {
    for (int k = 0; k < SCREEN_COUNT; k++) {
        const screen_t* s = &k_screens[k];
        fe_rect_t all = fe_rect(0, 0, s->w, s->h);
        for (int combo = 0; combo < (LD_MAX_GAUGES + 2) * (LD_MAX_NOTES + 2); combo++) {
            int n = combo % (LD_MAX_GAUGES + 2), notes = combo / (LD_MAX_GAUGES + 2);
            ld_layout_t l;
            fe_rect_t photo, frame, plate, message;
            double ratio;
            ld_layout_make(&l, s->w, s->h, n, notes);
            photo = screen_rect(&l, l.photo);
            frame = screen_rect(&l, l.frame);
            message = screen_rect(&l, l.message);
            ratio = (double)(photo.right - photo.left) / (photo.bottom - photo.top);
            CHECK(inside(all, frame) && inside(frame, photo), "%s %dx%d: the framed picture is on the screen", s->aspect, s->w, s->h);
            CHECK(fabs(ratio - 4.0 / 3.0) < 0.01, "%s %dx%d: the picture keeps 4:3 (%.4f)", s->aspect, s->w, s->h, ratio);
            CHECK(abs((photo.left + photo.right) - s->w) <= 2 && abs((photo.top + photo.bottom) - s->h) <= 2,
                  "%s %dx%d: the picture is centred", s->aspect, s->w, s->h);
            CHECK((s->w * 3 >= s->h * 4 ? (photo.bottom - photo.top) >= s->h * 0.9 : (photo.right - photo.left) >= s->w * 0.9),
                  "%s %dx%d: the picture fills the screen's shorter side (%dx%d)", s->aspect, s->w, s->h,
                  photo.right - photo.left, photo.bottom - photo.top);
            CHECK(inside(photo, message), "%s %dx%d: the message stays on the picture", s->aspect, s->w, s->h);
            CHECK(l.gauge_count == (n > LD_MAX_GAUGES ? LD_MAX_GAUGES : n), "gauge count clamped");
            CHECK(l.note_count == (notes > LD_MAX_NOTES ? LD_MAX_NOTES : notes), "note count clamped");
            if (!l.gauge_count && !l.note_count) {
                CHECK(fe_rect_empty(l.plate), "nothing to show, no plate");
                continue;
            }
            plate = screen_rect(&l, l.plate);
            CHECK(inside(photo, plate), "%s %dx%d %d gauges %d notes: the plate stays on the picture (safe area)", s->aspect, s->w,
                  s->h, n, notes);
            for (int i = 0; i < l.note_count; i++) {
                fe_rect_t note = screen_rect(&l, l.notes[i]);
                CHECK(inside(plate, note) && (i == 0 || note.top >= screen_rect(&l, l.notes[i - 1]).bottom) &&
                          (!l.gauge_count || note.bottom <= screen_rect(&l, l.captions[0]).top),
                      "%s %dx%d: note %d on the plate, over the gauges", s->aspect, s->w, s->h, i);
            }
            for (int i = 0; i < l.gauge_count; i++) {
                fe_rect_t caption = screen_rect(&l, l.captions[i]), groove = screen_rect(&l, l.grooves[i]);
                CHECK(inside(plate, caption) && inside(plate, groove), "%s %dx%d: gauge %d on its plate", s->aspect, s->w, s->h, i);
                CHECK(groove.bottom - groove.top >= 6 && groove.right - groove.left >= 300, "%s %dx%d: gauge %d is %dx%d",
                      s->aspect, s->w, s->h, i, groove.right - groove.left, groove.bottom - groove.top);
                CHECK(i == 0 || caption.top >= screen_rect(&l, l.grooves[i - 1]).bottom, "gauges do not overlap");
                CHECK(ld_fill_right(&l, i, 0.0f) < ld_fill_right(&l, i, 0.5f) &&
                          ld_fill_right(&l, i, 0.5f) < ld_fill_right(&l, i, 1.0f) &&
                          ld_fill_right(&l, i, 2.0f) == ld_fill_right(&l, i, 1.0f) &&
                          ld_fill_right(&l, i, -1.0f) == ld_fill_right(&l, i, 0.0f) &&
                          ld_fill_right(&l, i, NAN) == ld_fill_right(&l, i, 0.0f),
                      "fills grow with progress and stay inside the groove");
                CHECK(inside(all, ld_gauge_pixels(&l, i)), "gauge pixels on the screen");
            }
        }
    }
}

static ld_scene_t loader_scene(int w, int h, float a, float b) {
    ld_scene_t s;
    memset(&s, 0, sizeof s);
    s.kind = LD_SCENE_LOADER;
    s.screen_w = w;
    s.screen_h = h;
    s.art565 = &g_art[0][0];
    s.art_w = 640;
    s.art_h = 480;
    s.art_pitch = 640;
    s.art_key = 0xA11;
    s.gauge_count = 2;
    strcpy(s.gauges[0].caption, "World Initialization");
    strcpy(s.gauges[1].caption, "Level File Copy");
    s.gauges[0].fraction = a;
    s.gauges[1].fraction = b;
    s.gauges[0].color = s.gauges[1].color = 0xFF4000;
    s.gauges[0].text_color = s.gauges[1].text_color = 0xFFFFFF;
    return s;
}

static void test_diff(void) {
    ld_layout_t l;
    ld_scene_t a = loader_scene(2560, 1440, 0.30f, 0.0f), b = a;
    fe_rect_t dirty;
    ld_layout_make(&l, 2560, 1440, 2, 0);
    CHECK(ld_scene_diff(NULL, &a, &l, &dirty) == LD_DIFF_ALL, "the first draw paints everything");
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_SAME && fe_rect_empty(dirty), "an unchanged draw paints nothing");
    b.gauges[0].fraction = 0.30001f;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_SAME, "a step that moves no pixel paints nothing");
    b.gauges[0].fraction = 0.31f;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_GAUGES && fe_rect_equal(dirty, ld_gauge_pixels(&l, 0)),
          "a progress step repaints its own gauge only");
    b.gauges[1].fraction = 0.5f;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_GAUGES &&
              fe_rect_equal(dirty, fe_rect_union(ld_gauge_pixels(&l, 0), ld_gauge_pixels(&l, 1))),
          "two steps repaint both gauges");
    b = a;
    strcpy(b.gauges[1].caption, "Something else");
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "a new caption repaints everything");
    b = a;
    b.art_key = 7;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "another picture repaints everything");
    b = a;
    b.screen_w = 1920;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "another screen size repaints everything");
    b = a;
    b.kind = LD_SCENE_MESSAGE;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "the message before a load and the loader are different screens");
    b = a;
    b.gauge_count = 1;
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "a gauge less repaints everything");
    b = a;
    b.note_count = 1;
    strcpy(b.notes[0], "Please Wait");
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "a new line of text repaints everything");
    a = b;
    strcpy(b.notes[0], "Please wait");
    CHECK(ld_scene_diff(&a, &b, &l, &dirty) == LD_DIFF_ALL, "a changed line of text repaints everything");
}

static uint32_t* surface(int w, int h, uint32_t fill);

/* The test of log 042's machine: New Game on a 2560x1920 render box with only the game's own li_be_0.tga. */
static void test_retail_beach(void) {
#ifdef JPT_SOURCE_DIR
    static uint16_t art[480 * 640];
    char path[MAX_PATH];
    const int w = 2560, h = 1920;
    fe_painter_t* p;
    uint32_t* px;
    ld_scene_t s;
    ld_layout_t l;
    fe_rect_t photo;
    long clear = 0, art_like = 0, sampled = 0;
    snprintf(path, sizeof path, "%s/game/data/menu/li_be_0.tga", JPT_SOURCE_DIR);
    if (!load_li_tga(path, art)) {
        printf("li_be_0.tga not installed: the retail Beach check is skipped\n");
        return;
    }
    p = fe_painter_create();
    px = surface(w, h, 0);
    s = loader_scene(w, h, 0.5f, 0.0f);
    s.art565 = art;
    s.art_key = 0xBEAC;
    s.note_count = 1;
    strcpy(s.notes[0], "Please Wait");
    s.note_colors[0] = 0xFFFFFF;
    CHECK(p && px && fe_paint_loading(p, &s, px, w, fe_rect(0, 0, w, h), 8), "Beach at 2560x1920 cannot be painted");
    if (p && px) {
        ld_layout_scene(&l, &s);
        photo = fe_view_rect(&l.view, l.photo);
        for (long i = 0; i < (long)w * h; i++) clear += (px[i] >> 24) != 255;
        CHECK(clear == 0, "Beach at 2560x1920 leaves %ld pixels to the game's screen", clear);
        CHECK(photo.bottom - photo.top >= h * 9 / 10 && photo.right - photo.left >= w * 9 / 10,
              "Beach's picture fills 2560x1920 (%dx%d), no longer 640x480 in the middle", photo.right - photo.left,
              photo.bottom - photo.top);
        /* the painted picture is li_be_0.tga scaled into the frame (above the plate), within filtering and grain */
        for (int gy = 1; gy < 20; gy++)
            for (int gx = 1; gx < 30; gx++) {
                int x = photo.left + (photo.right - photo.left) * gx / 30, y = photo.top + (photo.bottom - photo.top) * 7 * gy / 200;
                int sx = (int)((x + 0.5 - photo.left) * 640 / (photo.right - photo.left)), sy = (int)((y + 0.5 - photo.top) * 480 / (photo.bottom - photo.top));
                float want[3] = { 0, 0, 0 };
                uint32_t v = px[(long)y * w + x];
                for (int j = -1; j <= 1; j++)
                    for (int i = -1; i <= 1; i++) {
                        uint16_t t = art[(sy + j < 0 ? 0 : sy + j > 479 ? 479 : sy + j) * 640 + (sx + i < 0 ? 0 : sx + i > 639 ? 639 : sx + i)];
                        want[0] += ((t >> 11) & 31) * 255.0f / 31.0f / 9.0f;
                        want[1] += ((t >> 5) & 63) * 255.0f / 63.0f / 9.0f;
                        want[2] += (t & 31) * 255.0f / 31.0f / 9.0f;
                    }
                sampled++;
                art_like += fabsf(want[0] - (float)((v >> 16) & 255)) < 48 && fabsf(want[1] - (float)((v >> 8) & 255)) < 48 &&
                            fabsf(want[2] - (float)(v & 255)) < 48;
            }
        CHECK(art_like * 100 > sampled * 85, "Beach's picture is li_be_0.tga scaled up (%ld of %ld samples match)", art_like, sampled);
    }
    free(px);
    fe_painter_destroy(p);
#endif
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
            uint32_t v = px[(long)y * w + x], a = v >> 24, game = 70 * (255 - a) / 255;
            row[x * 3 + 0] = (unsigned char)(((v >> 16) & 255) + game);
            row[x * 3 + 1] = (unsigned char)(((v >> 8) & 255) + game);
            row[x * 3 + 2] = (unsigned char)((v & 255) + game);
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    fclose(f);
    free(row);
    printf("wrote %s\n", path);
}

static double luma(uint32_t v) { return 0.30 * ((v >> 16) & 255) + 0.59 * ((v >> 8) & 255) + 0.11 * (v & 255); }

static void test_painter(void) {
    fe_painter_t* p = fe_painter_create();
    CHECK(p != NULL, "the painter cannot be created");
    if (!p) return;
    for (int k = 0; k < SCREEN_COUNT; k++) {
        const int w = k_screens[k].w, h = k_screens[k].h;
        ld_scene_t s = loader_scene(w, h, 0.42f, 0.0f), next;
        ld_layout_t l;
        uint32_t *full = surface(w, h, 0), *part = surface(w, h, 0), *mt = surface(w, h, 0);
        fe_rect_t dirty, photo;
        long clear = 0, threads_diff = 0, step_diff = 0;
        if (!full || !part || !mt) {
            free(full);
            free(part);
            free(mt);
            continue;
        }
        CHECK(fe_paint_loading(p, &s, full, w, fe_rect(0, 0, w, h), 1), "%dx%d: paint failed", w, h);
        CHECK(fe_paint_loading(p, &s, mt, w, fe_rect(0, 0, w, h), 4), "%dx%d: threaded paint failed", w, h);
        for (long i = 0; i < (long)w * h; i++) {
            clear += (full[i] >> 24) != 255;
            threads_diff += mt[i] != full[i];
        }
        CHECK(clear == 0, "%s %dx%d: the loading screen leaves %ld pixels transparent", k_screens[k].aspect, w, h, clear);
        CHECK(threads_diff == 0, "%dx%d: threads change %ld pixels", w, h, threads_diff);
        memcpy(part, full, sizeof(uint32_t) * (size_t)w * h);
        next = s;
        next.gauges[0].fraction = 0.77f;
        next.gauges[1].fraction = 0.25f;
        ld_layout_make(&l, w, h, 2, 0);
        CHECK(ld_scene_diff(&s, &next, &l, &dirty) == LD_DIFF_GAUGES, "%dx%d: a progress step is a gauge repaint", w, h);
        CHECK(fe_paint_loading(p, &next, part, w, dirty, 3), "%dx%d: gauge repaint failed", w, h);
        CHECK(fe_paint_loading(p, &next, mt, w, fe_rect(0, 0, w, h), 2), "%dx%d: repaint failed", w, h);
        for (long i = 0; i < (long)w * h; i++) step_diff += part[i] != mt[i];
        CHECK(step_diff == 0, "%dx%d: repainting the gauges alone differs from a full paint in %ld pixels", w, h, step_diff);
        photo = fe_view_rect(&l.view, l.photo);
        {
            uint32_t sky = full[(long)(photo.top + (photo.bottom - photo.top) / 10) * w + photo.left + (photo.right - photo.left) / 10];
            uint32_t side = full[(long)(h / 2) * w + 2];
            CHECK(((sky & 255) > ((sky >> 16) & 255)) && (sky & 255) > 120, "%dx%d: the level's sky shows in the picture (%06X)",
                  w, h, sky & 0xFFFFFF);
            CHECK(luma(side) < luma(sky) && luma(side) > 1.0, "%dx%d: the surroundings are the picture, darker (%06X)", w, h,
                  side & 0xFFFFFF);
        }
        free(full);
        free(part);
        free(mt);
    }
    for (int k = 0; k < SCREEN_COUNT; k++) {
        const int w = k_screens[k].w, h = k_screens[k].h;
        uint32_t* px = surface(w, h, 0x12345678u);
        ld_scene_t s;
        long uncovered = 0;
        memset(&s, 0, sizeof s);
        s.kind = LD_SCENE_MESSAGE;
        s.screen_w = w;
        s.screen_h = h;
        strcpy(s.message, "Loading Level.  Please Wait.");
        CHECK(px && fe_paint_loading(p, &s, px, w, fe_rect(0, 0, w, h), 2), "%dx%d: the message cannot be painted", w, h);
        if (!px) continue;
        CHECK((px[0] >> 24) == 0 && (px[(long)(h - 1) * w + w - 1] >> 24) == 0, "%dx%d: the message leaves the screen clear",
              w, h);
        /* the game's own line: DrawTextEx centred in the System font, about 230x16 screen pixels at any resolution */
        for (int y = h / 2 - 12; y < h / 2 + 12; y++)
            for (int x = w / 2 - 140; x < w / 2 + 140; x++) uncovered += (px[(long)y * w + x] >> 24) != 255;
        CHECK(uncovered == 0, "%dx%d: the plaque leaves %ld pixels of the game's line uncovered", w, h, uncovered);
        free(px);
    }
    {
        uint32_t px[16];
        ld_scene_t s = loader_scene(4, 4, 0.0f, 0.0f), none;
        memset(&none, 0, sizeof none);
        CHECK(!fe_paint_loading(p, &none, px, 4, fe_rect(0, 0, 4, 4), 1), "an empty scene is not painted");
        s.screen_w = 0;
        CHECK(!fe_paint_loading(p, &s, px, 4, fe_rect(0, 0, 4, 4), 1), "a zero-sized screen is not painted");
        s = loader_scene(4, 4, 0.0f, 0.0f);
        s.art565 = NULL;
        s.art_key = 0xBAD;
        CHECK(fe_paint_loading(p, &s, px, 4, fe_rect(0, 0, 0, 0), 1), "nothing to repaint is not an error");
        CHECK(!fe_paint_loading(p, &s, px, 4, fe_rect(0, 0, 4, 4), 1), "a loading screen without a picture is not painted");
    }
    fe_painter_destroy(p);
}

/* A bottom-up 24-bit BMP of a horizontal gradient, which WIC decodes like any HD art file. */
static int write_bmp(const char* path, int w, int h) {
    unsigned char head[54];
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    unsigned char* row = (unsigned char*)calloc(1, stride);
    FILE* f = fopen(path, "wb");
    uint32_t size = (uint32_t)(54 + stride * h);
    if (!f || !row) {
        if (f) fclose(f);
        free(row);
        return 0;
    }
    memset(head, 0, sizeof head);
    head[0] = 'B';
    head[1] = 'M';
    memcpy(head + 2, &size, 4);
    head[10] = 54;
    head[14] = 40;
    memcpy(head + 18, &w, 4);
    memcpy(head + 22, &h, 4);
    head[26] = 1;
    head[28] = 24;
    fwrite(head, 1, 54, f);
    for (int x = 0; x < w; x++) {
        row[x * 3 + 0] = (unsigned char)(x * 255 / w);
        row[x * 3 + 1] = 90;
        row[x * 3 + 2] = (unsigned char)(255 - x * 255 / w);
    }
    for (int y = 0; y < h; y++) fwrite(row, 1, stride, f);
    fclose(f);
    free(row);
    return 1;
}

static void test_hd_art(void) {
    char dir[MAX_PATH], good[MAX_PATH], bad[MAX_PATH], missing[MAX_PATH];
    sp_image_t art;
    fe_painter_t* p;
    GetTempPathA(sizeof dir, dir);
    snprintf(good, sizeof good, "%sjpt_loading_art_%lu.bmp", dir, GetCurrentProcessId());
    snprintf(bad, sizeof bad, "%sjpt_loading_art_%lu.png", dir, GetCurrentProcessId());
    snprintf(missing, sizeof missing, "%sjpt_loading_art_none_%lu.png", dir, GetCurrentProcessId());
    CHECK(write_bmp(good, 1440, 1080), "the test picture cannot be written");
    {
        FILE* f = fopen(bad, "wb");
        if (f) {
            fputs("\x89PNG\r\n\x1a\nnot really a picture at all", f);
            fclose(f);
        }
    }
    CHECK(sp_image_load(good, 1000, 500, &art) && art.width == 667 && art.height == 500,
          "an HD picture larger than its place is made smaller, shape kept (%dx%d)", art.width, art.height);
    CHECK(art.pixels && (art.pixels[0] >> 24) == 255 && (art.pixels[0] & 0xFF) < 40 && ((art.pixels[0] >> 16) & 0xFF) > 200,
          "the decoded picture is opaque BGRA");
    p = fe_painter_create();
    if (p && art.pixels) {
        const int w = 3440, h = 1440;
        uint32_t* px = surface(w, h, 0);
        ld_scene_t s = loader_scene(w, h, 0.5f, 0.0f);
        s.art565 = NULL;
        s.art32 = art.pixels;
        s.art_w = art.width;
        s.art_h = art.height;
        s.art_key = 0x4D;
        CHECK(px && fe_paint_loading(p, &s, px, w, fe_rect(0, 0, w, h), 4), "an HD picture is painted");
        free(px);
        fe_painter_trim(p);
    }
    fe_painter_destroy(p);
    save_preview_free(&art);
    CHECK(sp_image_load(good, 4000, 4000, &art) && art.width == 1440 && art.height == 1080, "a picture that fits keeps its size");
    save_preview_free(&art);
    CHECK(!sp_image_load(bad, 2000, 1000, &art) && !art.pixels, "a corrupt picture is refused");
    CHECK(!sp_image_load(missing, 2000, 1000, &art) && !art.pixels, "a missing picture is refused");
    CHECK(!sp_image_load(good, 0, 1000, &art) && !art.pixels, "a picture with no room is refused");
    DeleteFileA(good);
    DeleteFileA(bad);
}

static void dump_screens(const char* dir) {
    static const int sizes[][2] = { { 2560, 1920 }, { 1920, 1080 }, { 2560, 1600 }, { 3440, 1440 }, { 5120, 1440 } };
    fe_painter_t* p = fe_painter_create();
    if (!p) return;
    for (int i = 0; i < LD_TABLE_ROWS; i++) {
        char path[MAX_PATH], slot[LD_SLOT_BYTES];
        int retail = 0;
        ld_slot_name(ld_retail_table[i].image, slot, sizeof slot);
#ifdef JPT_SOURCE_DIR
        snprintf(path, sizeof path, "%s/game/data/%s_0.tga", JPT_SOURCE_DIR, ld_retail_table[i].image);
        for (char* c = path; *c; c++)
            if (*c == '\\') *c = '/';
        retail = load_li_tga(path, &g_art[0][0]);
#else
        (void)path;
#endif
        if (!retail) synthetic_art();
        for (int k = 0; k < 5; k++) {
            int w = sizes[k][0], h = sizes[k][1];
            uint32_t* px;
            ld_scene_t s;
            if (i != 2 && k != 1) continue;
            px = surface(w, h, 0);
            s = loader_scene(w, h, 0.63f, 0.0f);
            s.art_key = 0x100u + (uint32_t)i;
            if (px && fe_paint_loading(p, &s, px, w, fe_rect(0, 0, w, h), 8)) write_ppm(dir, slot, w, h, px);
            free(px);
        }
    }
    {
        uint32_t* px = surface(1920, 1080, 0);
        ld_scene_t s;
        memset(&s, 0, sizeof s);
        s.kind = LD_SCENE_MESSAGE;
        s.screen_w = 1920;
        s.screen_h = 1080;
        strcpy(s.message, "Loading Level.  Please Wait.");
        if (px && fe_paint_loading(p, &s, px, 1920, fe_rect(0, 0, 1920, 1080), 8)) write_ppm(dir, "loading_message", 1920, 1080, px);
        free(px);
    }
    fe_painter_destroy(p);
    synthetic_art();
}

/* Presents around a level load, in the game's order; the hold decides what reaches the screen. */
enum { P_GAME, P_VIDEO, P_MESSAGE, P_LOAD_START, P_CLEAR, P_LOADER, P_LOAD_END, P_UI_FRAME };

typedef struct {
    int shown_blank, shown_loader, shown_game, held, shown_video;
} hold_run_t;

static hold_run_t run_hold(const int* steps, int n, uint32_t ms_per_step) {
    load_hold_t h;
    hold_run_t r;
    uint32_t now = 1000;
    memset(&h, 0, sizeof h);
    memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++, now += ms_per_step) {
        switch (steps[i]) {
        case P_LOAD_START:
        case P_LOAD_END: load_hold_arm(&h, now); break;
        case P_UI_FRAME: load_hold_release(&h); break;
        case P_GAME: r.shown_game++; break;
        case P_VIDEO:
            if (load_hold_present(&h, 0, now)) r.held++;
            else r.shown_video++;
            break;
        case P_MESSAGE:
        case P_LOADER:
            if (load_hold_present(&h, 1, now)) r.held++;
            else r.shown_loader++;
            break;
        case P_CLEAR:
            if (load_hold_present(&h, 0, now)) r.held++;
            else r.shown_blank++;
            break;
        }
    }
    return r;
}

static void test_transitions(void) {
    static const int next_level[] = { P_UI_FRAME, P_GAME, P_MESSAGE, P_LOAD_START, P_CLEAR, P_CLEAR, P_CLEAR, P_LOADER, P_LOADER,
                                      P_LOADER, P_LOAD_END, P_CLEAR, P_CLEAR, P_CLEAR, P_UI_FRAME, P_GAME };
    static const int new_game[] = { P_VIDEO, P_VIDEO, P_LOAD_START, P_LOADER, P_LOADER, P_LOAD_END, P_CLEAR, P_CLEAR, P_CLEAR,
                                    P_UI_FRAME, P_GAME };
    static const int no_loader[] = { P_LOAD_START, P_CLEAR, P_CLEAR, P_CLEAR, P_CLEAR, P_CLEAR, P_CLEAR, P_LOAD_END, P_UI_FRAME, P_GAME };
    static const int credits[] = { P_UI_FRAME, P_VIDEO, P_VIDEO, P_VIDEO, P_UI_FRAME };
    hold_run_t r = run_hold(next_level, (int)(sizeof next_level / sizeof next_level[0]), 50);
    CHECK(r.shown_blank == 0 && r.held == 6 && r.shown_loader == 4 && r.shown_game == 2,
          "next level: no blank frame between the last game frame, the message, the loading screen and the new level "
          "(blank %d, held %d, loading %d)", r.shown_blank, r.held, r.shown_loader);
    r = run_hold(new_game, (int)(sizeof new_game / sizeof new_game[0]), 50);
    CHECK(r.shown_video == 2 && r.shown_blank == 0 && r.held == 3 && r.shown_loader == 2,
          "new game: the intro video is untouched, the loading screen stays until the first frame (video %d, blank %d)",
          r.shown_video, r.shown_blank);
    r = run_hold(no_loader, (int)(sizeof no_loader / sizeof no_loader[0]), 50);
    CHECK(r.held == LOAD_HOLD_MAX_PRESENTS && r.shown_blank == 6 - (int)LOAD_HOLD_MAX_PRESENTS,
          "presents the hold does not know are let through after its cap (held %d, shown %d)", r.held, r.shown_blank);
    r = run_hold(next_level, (int)(sizeof next_level / sizeof next_level[0]), LOAD_HOLD_MAX_MS);
    CHECK(r.shown_blank > 0, "a hold never outlasts its time cap");
    r = run_hold(credits, (int)(sizeof credits / sizeof credits[0]), 50);
    CHECK(r.held == 0 && r.shown_video == 3, "videos outside a level load are never held");
}

void loading_tests(int* checks, int* failures, const char* dump_dir) {
    g_checks = checks;
    g_failures = failures;
    synthetic_art();
    test_inventory();
    test_layout();
    test_diff();
    test_transitions();
    test_painter();
    test_retail_beach();
    test_hd_art();
    if (dump_dir) dump_screens(dump_dir);
}
