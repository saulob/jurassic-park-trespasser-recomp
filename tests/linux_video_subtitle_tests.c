/* Subtitles over the videos on Linux, without a window and without the guest: the timelines the Windows host uses (their
 * coverage by video and language, the cue on a frame, the end and the skip), and the overlay itself on synthetic frames: its
 * place in the video rectangle, centring, lines, clipping, other frame sizes, the guest's surface left alone, and the retail
 * files of the installed game accepted by the same fingerprints. Retail checks report DATA UNAVAILABLE without the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_hash.h"
#include "jpt_l10n.h"
#include "jpt_tr.h"
#include "jpt_video_subtitles.h"
#include "l10n_assets.h"
#include "linux_video_subtitles.h"

static int g_checks, g_failures, g_unavailable;

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

/* ---- the timelines ---- */

static void test_coverage(void) {
    static const char* const videos[] = { "tpassintro.smk", "newgame.smk", "win.smk", "credits.smk" };
    static const int cues[] = { 16, 9, 7, 0 };
    for (size_t v = 0; v < sizeof videos / sizeof videos[0]; v++) {
        const jvs_timeline_t* t = jvs_timeline_named(videos[v]);
        int entries = 0;
        for (int i = 0; i < jvs_timeline_count; i++) entries += !strcmp(jvs_timelines[i].name, videos[v]);
        CHECK((t != NULL) == (cues[v] != 0), "%s: a timeline exists only for the videos with dialogue", videos[v]);
        if (!t) continue;
        CHECK(t->cue_count == cues[v], "%s has %d cues (%d)", videos[v], cues[v], t->cue_count);
        printf("  %s: %d timelines (one per release timing), %d cues\n", videos[v], entries, t->cue_count);
    }
}

static void test_languages(void) {
    static const char* const codes[] = { "en", "pt-BR", "fr", "de", "es", "ru", "it" };
    for (size_t l = 0; l < sizeof codes / sizeof codes[0]; l++) {
        const jpt_l10n_language_t* language = jpt_l10n_language(codes[l]);
        const char *data, *english_data;
        size_t size, english_size;
        int total = 0, translated = 0, loaded;
        CHECK(language != NULL, "%s is a language of the table", codes[l]);
        if (!language) continue;
        jpt_tr_reset();
        loaded = 0;
        if (strcmp(codes[l], JPT_L10N_ENGLISH)) {
            CHECK(l10n_asset(codes[l], JPT_TR_ASSET, &data, &size), "%s has an embedded interface file", codes[l]);
            loaded = l10n_asset(codes[l], JPT_TR_ASSET, &data, &size) ? jpt_tr_load(data, size, language->codepage) : 0;
            CHECK(loaded > 0, "%s: %d entries load", codes[l], loaded);
        } else {
            CHECK(l10n_asset(codes[l], JPT_TR_ASSET, &english_data, &english_size), "English has its interface file");
        }
        for (int t = 0; t < jvs_timeline_count; t++)
            for (int c = 0; c < jvs_timelines[t].cue_count; c++) {
                const char* key = jvs_timelines[t].cues[c].key;
                total++;
                translated += !strcmp(codes[l], JPT_L10N_ENGLISH) || jpt_tr_has(key);
                CHECK(jpt_tr(key)[0] != 0, "%s: cue %d of %s has text", codes[l], c, jvs_timelines[t].name);
            }
        CHECK(translated == total, "%s: every cue text of every video has a translation (%d of %d)", codes[l], translated, total);
        printf("  %-6s %d of %d cue texts translated\n", codes[l], translated, total);
    }
    jpt_tr_reset();
}

static void test_session(void) {
    const jvs_timeline_t* t = jvs_timeline_named("tpassintro.smk");
    jvs_session_t s;
    jvs_session_begin(&s, t);
    CHECK(jvs_session_cue(&s, 0) == 0 && jvs_session_cue(&s, 82) == 0, "the first cue shows on frames 0 to 82");
    CHECK(jvs_session_cue(&s, 85) == -1, "nothing shows between two cues");
    CHECK(jvs_session_cue(&s, 89) == 1 && jvs_session_cue(&s, 124) == 1, "the second cue takes over at 89");
    CHECK(jvs_session_cue(&s, 125) == 2, "and the third at 125");
    CHECK(jvs_session_cue(&s, 1584) == -1, "nothing after the last cue");
    CHECK(jvs_session_cue(&s, 5) == -1, "a frame number lower than one shown means the decoder wrapped: nothing shows");
    jvs_session_end(&s);
    CHECK(jvs_session_cue(&s, 10) == -1, "an ended session shows nothing");
    jvs_session_begin(&s, t);
    CHECK(jvs_session_cue(&s, 10) == 0, "a new playback starts clean");
}

static void test_outline(void) {
    uint32_t glyphs[49], out[49];
    uint16_t scratch[49];
    memset(glyphs, 0, sizeof glyphs);
    glyphs[3 * 7 + 3] = 0xFFu;
    jvs_outline(glyphs, 7, 7, 1, scratch, out);
    CHECK(out[3 * 7 + 3] == 0xFFFFFFFFu, "a solid glyph pixel is opaque white");
    CHECK(out[3 * 7 + 4] == 0xEB000000u, "next to it: the outline, 235 of 255 opaque (0x%08X)", out[3 * 7 + 4]);
    CHECK(out[2 * 7 + 4] == 0x9C000000u, "on the diagonal: a softer edge (0x%08X)", out[2 * 7 + 4]);
    CHECK(out[3 * 7 + 6] == 0u && out[0] == 0u, "beyond the outline: nothing");
}

/* ---- the overlay ---- */

typedef struct {
    int min_x, min_y, max_x, max_y, bright;
} bounds_t;

static uint16_t* make_frame(int w, int h) { return (uint16_t*)calloc((size_t)w * (size_t)h, 2); }

static bounds_t bright_bounds(const lx_frame_t* f, int from_y, int to_y) {
    bounds_t b = { 1 << 30, 1 << 30, -1, -1, 0 };
    for (int y = from_y; y < to_y && y < f->height; y++)
        for (int x = 0; x < f->width; x++) {
            uint32_t p = ((const uint32_t*)((const uint8_t*)f->pixels + (size_t)y * (size_t)f->pitch))[x];
            if ((p & 0xFF) > 200 && ((p >> 8) & 0xFF) > 200 && ((p >> 16) & 0xFF) > 200) {
                if (x < b.min_x) b.min_x = x;
                if (y < b.min_y) b.min_y = y;
                if (x > b.max_x) b.max_x = x;
                if (y > b.max_y) b.max_y = y;
                b.bright++;
            }
        }
    return b;
}

static const jvs_cue_t k_cues[] = {
    { 0, 9, "Video::Hello there" },
    { 12, 19, "Video::This line is long enough that it cannot stay on one line over a picture, so it breaks into more than one" },
    { 22, 29, "Video::Short" },
};
static const jvs_timeline_t k_timeline = { "synthetic.smk", 30u, k_cues, 3, NULL, 0 };

static int overlay(const lx_frame_t* in, lx_frame_t* out) { return lx_vsub_compose(in, out); }

static void test_overlay(void) {
    enum { W = 640, H = 480, PY = 66, PH = 348 };
    uint16_t* pixels = make_frame(W, H);
    lx_frame_t in, out;
    bounds_t b;
    jvs_rect_t picture = { 0, PY, W, PH };
    int px = jvs_font_px(&picture);
    jvs_rect_t expected;
    memset(&in, 0, sizeof in);
    in.pixels = pixels, in.width = W, in.height = H, in.pitch = W * 2, in.format = LX_PF_RGB565;
    lx_vsub_reset();
    lx_vsub_set_enabled(1);
    lx_vsub_begin_timeline(7, &k_timeline);

    CHECK(!overlay(&in, &out), "before the first frame nothing is drawn");
    lx_vsub_frame(7, 0, 0, PY, W, PH);
    CHECK(lx_vsub_cue() == 0 && overlay(&in, &out), "cue 0 is drawn on frame 0");
    CHECK(out.format == LX_PF_XRGB8888 && out.width == W && out.height == H && out.pixels != in.pixels, "into a copy of the frame, as XRGB8888");
    b = bright_bounds(&out, 0, H);
    CHECK(b.bright > 80, "white text is there (%d bright pixels)", b.bright);
    CHECK(b.min_y > PY + PH / 2 && b.max_y < PY + PH, "in the lower part of the picture (rows %d-%d of %d-%d)", b.min_y, b.max_y, PY, PY + PH);
    CHECK(abs((b.min_x + b.max_x) / 2 - W / 2) <= 3, "centred under the picture's middle (%d)", (b.min_x + b.max_x) / 2);
    {
        int single_rows = b.max_y - b.min_y + 1;
        CHECK(single_rows < px * 2, "one line: %d rows for a %d px face", single_rows, px);
        expected = jvs_place(W, H, &picture, 1, 1);
        CHECK(b.max_y <= expected.y + expected.h, "the text ends at or above the Windows layout's bottom line (%d, %d)", b.max_y, expected.y + expected.h);
        lx_vsub_frame(7, 12, 0, PY, W, PH);
        CHECK(lx_vsub_cue() == 1 && overlay(&in, &out), "cue 1 follows on frame 12");
        b = bright_bounds(&out, 0, H);
        CHECK(b.max_y - b.min_y + 1 > single_rows + px / 2, "a long text takes more lines (%d rows)", b.max_y - b.min_y + 1);
        CHECK(b.min_x >= W * 6 / 100 && b.max_x <= W - W * 6 / 100, "and stays inside the picture's width (%d-%d)", b.min_x, b.max_x);
    }
    CHECK(bright_bounds(&out, 0, PY - 1).bright == 0 && bright_bounds(&out, PY + PH + 1, H).bright == 0, "the letterbox outside the picture stays black");
    CHECK(pixels[(PY + PH - 10) * W + W / 2] == 0 && pixels[100 * W + 100] == 0, "the guest's own surface is untouched");

    lx_vsub_frame(7, 20, 0, PY, W, PH);
    CHECK(lx_vsub_cue() == -1 && !overlay(&in, &out), "between two cues nothing is drawn (frame 20)");
    lx_vsub_frame(7, 22, 0, PY, W, PH);
    CHECK(lx_vsub_cue() == 2 && overlay(&in, &out), "cue 2 on frame 22");

    in.width = 800, in.height = 600, in.pitch = 1600;
    free(pixels);
    pixels = make_frame(800, 600);
    in.pixels = pixels;
    lx_vsub_frame(7, 22, 80, 100, 640, 400);
    CHECK(overlay(&in, &out), "another frame size and video rectangle");
    b = bright_bounds(&out, 0, 600);
    CHECK(abs((b.min_x + b.max_x) / 2 - (80 + 320)) <= 3, "centred on the video rectangle, not the window (%d)", (b.min_x + b.max_x) / 2);
    CHECK(b.max_y < 100 + 400 && b.min_y > 100 + 200, "inside that rectangle (rows %d-%d)", b.min_y, b.max_y);

    lx_vsub_frame(7, 22, -200, 60, 1200, 300);
    CHECK(overlay(&in, &out), "a rectangle that goes past the frame still draws");
    b = bright_bounds(&out, 0, 600);
    CHECK(b.bright > 0 && b.min_x >= 0 && b.max_x < 800, "clipped to the frame (%d-%d)", b.min_x, b.max_x);

    lx_vsub_closed(9);
    lx_vsub_frame(7, 22, 80, 100, 640, 400);
    CHECK(lx_vsub_cue() == 2, "closing another video changes nothing");
    lx_vsub_closed(7);
    CHECK(lx_vsub_cue() == -1 && !overlay(&in, &out), "closing the video removes the subtitle at once");
    lx_vsub_frame(7, 22, 80, 100, 640, 400);
    CHECK(lx_vsub_cue() == -1 && !overlay(&in, &out), "a frame of a closed video brings nothing back");

    lx_vsub_begin_timeline(8, &k_timeline);
    lx_vsub_frame(8, 0, 0, PY, W, PH);
    CHECK(lx_vsub_cue() == 0, "the next video starts with its own cue");
    lx_vsub_reset();
    CHECK(lx_vsub_cue() == -1 && !overlay(&in, &out), "a reset (the HLE shutting down, a skip) leaves nothing on screen");
    free(pixels);
}

static void test_cyrillic(void) {
    static const jvs_cue_t cues[] = { { 0, 9, "Video::My name is John Parker Hammond. I was born on March 14, 1928." } };
    static const jvs_timeline_t timeline = { "ru.smk", 10u, cues, 1, NULL, 0 };
    const jpt_l10n_language_t* ru = jpt_l10n_language("ru");
    const char *data, *text;
    size_t size;
    uint16_t* pixels = make_frame(640, 480);
    lx_frame_t in, out;
    bounds_t b;
    jpt_tr_reset();
    CHECK(l10n_asset("ru", JPT_TR_ASSET, &data, &size) && jpt_tr_load(data, size, ru->codepage) > 0, "the Russian interface text loads");
    memset(&in, 0, sizeof in);
    in.pixels = pixels, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    lx_vsub_begin_timeline(1, &timeline);
    lx_vsub_frame(1, 0, 0, 66, 640, 348);
    text = lx_vsub_text(0);
    CHECK(text && (unsigned char)text[0] >= 0xD0, "the cue is Cyrillic UTF-8 (%s)", text ? text : "none");
    CHECK(lx_vsub_compose(&in, &out), "and draws");
    b = bright_bounds(&out, 0, 480);
    CHECK(b.bright > 200, "with glyphs for the letters (%d bright pixels)", b.bright);
    lx_vsub_reset();
    jpt_tr_reset();
    free(pixels);
}

/* ---- the retail files ---- */

typedef struct {
    FILE* file;
    uint64_t size;
} file_source_t;

static uint32_t read_file_at(void* context, uint64_t offset, void* out, uint32_t bytes) {
    file_source_t* f = (file_source_t*)context;
    if (fseek(f->file, (long)offset, SEEK_SET) != 0) return 0;
    return (uint32_t)fread(out, 1, bytes, f->file);
}

static void test_retail(void) {
    static const char* const folders[] = { "game/data/menu", "game/versions/en-116/data/menu", "game/versions/ptbr-117/data/menu",
                                           "game/versions/ru-116/data/menu", "game/versions/es-117/data/menu" };
    static const char* const videos[] = { "tpassintro.smk", "newgame.smk", "win.smk", "credits.smk" };
    int seen = 0;
    for (size_t d = 0; d < sizeof folders / sizeof folders[0]; d++)
        for (size_t v = 0; v < sizeof videos / sizeof videos[0]; v++) {
            char path[512];
            file_source_t f;
            smk_source_t source;
            const jvs_timeline_t* picked = NULL;
            uint8_t* head;
            uint32_t got;
            char sha[65];
            snprintf(path, sizeof path, "%s/%s/%s", JPT_SOURCE_DIR, folders[d], videos[v]);
            f.file = fopen(path, "rb");
            if (!f.file) continue;
            fseek(f.file, 0, SEEK_END);
            f.size = (uint64_t)ftell(f.file);
            seen++;
            source.context = &f, source.size = f.size, source.read_at = read_file_at;
            head = (uint8_t*)malloc(JVS_FINGERPRINT_BYTES);
            got = read_file_at(&f, 0, head, JVS_FINGERPRINT_BYTES);
            jpt_sha256_buffer_hex(head, got, sha);
            free(head);
            if (jvs_timeline_named(videos[v])) {
                CHECK(jvs_select(1, videos[v], f.size, sha, &picked) == JVS_SELECTED && picked->frames == jvs_timeline_named(videos[v])->frames,
                      "%s is a retail file the timelines accept", path + strlen(JPT_SOURCE_DIR) + 1);
                lx_vsub_opened(5, videos[v], &source);
                CHECK(lx_vsub_cue() == -1, "opening a video shows no cue before its first frame");
                lx_vsub_frame(5, 0, 0, 66, 640, 348);
                CHECK(lx_vsub_cue() == (picked->cues[0].first == 0 ? 0 : -1), "frame 0 of %s shows what the timeline says", videos[v]);
                lx_vsub_frame(5, picked->cues[0].first, 0, 66, 640, 348);
                CHECK(lx_vsub_cue() == 0, "the first cue of %s shows on its first frame (%d)", videos[v], (int)picked->cues[0].first);
                lx_vsub_closed(5);
            } else {
                CHECK(jvs_select(1, videos[v], f.size, sha, &picked) == JVS_NO_DIALOGUE, "%s has no dialogue and no timeline", videos[v]);
            }
            fclose(f.file);
        }
    if (!seen) {
        printf("linux_video_subtitle_tests: DATA UNAVAILABLE (no installed video under game/)\n");
        g_unavailable++;
    }
}

int main(void) {
    test_coverage();
    test_languages();
    test_session();
    test_outline();
    test_overlay();
    test_cyrillic();
    test_retail();
    printf("linux_video_subtitle_tests: %d checks, %d failures%s\n", g_checks, g_failures, g_unavailable ? " (retail checks DATA UNAVAILABLE)" : "");
    return g_failures ? 1 : 0;
}
