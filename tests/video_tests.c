/* Headless checks of the HD video presentation: the retail inventory (the four files and the five calls that play
 * them), what the original decoder writes for every frame (SMACKW32.DLL with no audio track and no window), the layout
 * on every screen shape, the session's routing, and the GPU passes on a WARP device (no window). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "test_profile.h"

#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/video_gpu.h"
#include "guest_va.h"
#include "video_layout.h"
#include "video_test_panel.h"
#include "video_test_state.h"

static int g_checks, g_failures;

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

#define SCREEN_W 640
#define SCREEN_H 480
#define PLAYER_VA 0x00418AE0u

typedef struct {
    const char* name;
    uint32_t string_va, push_va, call_va;
} call_site_t;

typedef struct {
    const char* file;
    uint32_t stored_rows, frames, flags;
} retail_video_t;

static const retail_video_t k_videos[] = {
    { "tpassintro", 348, 1585, 0 },
    { "newgame", 172, 1085, VL_SMK_Y_INTERLACED },
    { "credits", 348, 1303, 0 },
    { "win", 348, 606, 0 },
};

static const call_site_t k_calls[] = {
    { "menu\\tpassintro", 0x00660B88u, 0x0040F2B3u, 0x0040F2BCu },
    { "menu\\win", 0x00660BA8u, 0x0040F437u, 0x0040F440u },
    { "menu\\credits", 0x00660B98u, 0x0040F45Eu, 0x0040F46Au },
    { "menu\\newgame", 0x00661298u, 0x0041567Eu, 0x00415687u },
    { "menu\\credits", 0x00660B98u, 0x004163F1u, 0x004163FAu },
};

typedef struct {
    const char* aspect;
    int w, h;
} screen_t;

static const screen_t k_screens[] = {
    { "16:9", 1280, 720 },  { "16:9", 1920, 1080 }, { "16:9", 2560, 1440 },  { "4:3", 2560, 1920 },
    { "21:9", 3440, 1440 }, { "16:9", 3840, 2160 }, { "32:9", 5120, 1440 },  { "32:9", 7680, 2160 },
    { "16:10", 1920, 1200 }, { "16:10", 2560, 1600 }, { "4:3", 1024, 768 },  { "21:9", 2560, 1080 },
};
#define SCREEN_COUNT (int)(sizeof k_screens / sizeof k_screens[0])

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

static int rgb_close(uint32_t a, uint32_t b, int tolerance) {
    for (int s = 0; s < 24; s += 8)
        if (abs((int)((a >> s) & 255) - (int)((b >> s) & 255)) > tolerance) return 0;
    return 1;
}

static uint32_t expand565(uint16_t c) {
    uint32_t r = ((c >> 11) & 31) * 255u, g = ((c >> 5) & 63) * 255u, b = (c & 31) * 255u;
    return 0xFF000000u | ((r + 15) / 31) << 16 | ((g + 31) / 63) << 8 | (b + 15) / 31;
}

static double luma(uint32_t c) { return 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255); }

static const char* g_dump;

static void dump_ppm(const char* name, const uint32_t* p, int w, int h) {
    char path[MAX_PATH];
    FILE* f;
    if (!g_dump || !p) return;
    snprintf(path, sizeof path, "%s/%s.ppm", g_dump, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(p[i] >> 16), (uint8_t)(p[i] >> 8), (uint8_t)p[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---- pure logic ---- */

static void test_header_parse(void) {
    uint8_t h[VL_SMK_HEADER_BYTES];
    vl_smk_info_t info;
    memset(h, 0, sizeof h);
    memcpy(h, "SMK2", 4);
    h[4] = 0x80, h[5] = 0x02;
    h[8] = 0xAC;
    h[12] = 0x3D, h[13] = 0x04;
    h[16] = 0xF6, h[17] = 0xE5, h[18] = 0xFF, h[19] = 0xFF;
    h[20] = 2;
    h[0x48] = 0x22, h[0x49] = 0x56, h[0x4B] = 0xF0;
    CHECK(vl_smk_parse(h, sizeof h, &info), "a valid header parses");
    CHECK(info.width == 640 && info.height == 172 && info.frames == 1085 && info.flags == 2, "fields %ux%u %u %u",
          info.width, info.height, info.frames, info.flags);
    CHECK(fabs(vl_smk_ms_per_frame(info.frame_rate) - 66.66) < 1e-9, "-6666: 66.66 ms per frame");
    CHECK(info.audio_rate == 22050 && info.audio_bits == 16 && info.audio_channels == 2 && info.audio_compressed,
          "audio track 0: %u Hz %u bit %u ch", info.audio_rate, info.audio_bits, info.audio_channels);
    CHECK(vl_smk_shown_rows(172, 2) == 344 && vl_smk_shown_rows(348, 0) == 348 && vl_smk_shown_rows(100, 4) == 200,
          "shown rows follow the Y flags");
    CHECK(vl_smk_ms_per_frame(50) == 50.0 && vl_smk_ms_per_frame(0) == 100.0, "positive rates are ms, 0 is 10 fps");
    h[3] = 'X';
    CHECK(!vl_smk_parse(h, sizeof h, &info), "a wrong signature is refused");
    CHECK(!vl_smk_parse(h, 20, &info), "a short header is refused");
}

static void test_policy(void) {
    vl_policy_t p = vl_policy_for(0, 348);
    CHECK(p.rows == VL_ROWS_PROGRESSIVE && p.stored_rows == 348 && p.shown_rows == 348, "progressive rows");
    p = vl_policy_for(VL_SMK_Y_INTERLACED, 344);
    CHECK(p.rows == VL_ROWS_REBUILD && p.stored_rows == 172 && p.shown_rows == 344 && p.parity == 0,
          "Y-interlaced: half the rows, on the first of each pair");
    p = vl_policy_for(VL_SMK_Y_DOUBLED, 200);
    CHECK(p.rows == VL_ROWS_REBUILD && p.stored_rows == 100, "Y-doubled: every second row is a repeat");
    p = vl_policy_for(VL_SMK_Y_INTERLACED, 1);
    CHECK(p.rows == VL_ROWS_PROGRESSIVE, "a single row is never rebuilt");
}

static void test_layout(void) {
    for (int i = 0; i < SCREEN_COUNT; i++) {
        const screen_t* s = &k_screens[i];
        for (int v = 0; v < 2; v++) {
            int ch = v ? 344 : 348;
            vl_layout_t l = vl_layout(s->w, s->h, 640, ch);
            double want = 640.0 / ch, got = (double)l.video.w / l.video.h;
            int wider = (double)s->w / s->h > want;
            CHECK(fabs(got / want - 1.0) < 0.002, "%s %dx%d: picture %dx%d keeps 640:%d (%.4f vs %.4f)", s->aspect, s->w,
                  s->h, l.video.w, l.video.h, ch, got, want);
            CHECK(l.video.x >= 0 && l.video.y >= 0 && l.video.x + l.video.w <= s->w && l.video.y + l.video.h <= s->h,
                  "%s %dx%d: the picture is inside the output", s->aspect, s->w, s->h);
            CHECK(abs(l.video.x * 2 + l.video.w - s->w) <= 1 && abs(l.video.y * 2 + l.video.h - s->h) <= 1,
                  "%s %dx%d: centred", s->aspect, s->w, s->h);
            CHECK(wider ? (l.video.h == s->h && l.side > 0 && l.bars == 0) : (l.video.w == s->w && l.side == 0),
                  "%s %dx%d: %s", s->aspect, s->w, s->h, wider ? "fills the height, sides extended" : "fills the width");
        }
    }
    {
        vl_layout_t a = vl_layout(2560, 1920, 640, 348), b = vl_layout(1920, 1080, 640, 348);
        vl_layout_t c = vl_layout(5120, 1440, 640, 348), d = vl_layout(3440, 1440, 640, 348);
        CHECK(a.video.w == 2560 && a.video.h == 1392 && a.bars == 264, "4:3 2560x1920: 2560x1392, bars %d", a.bars);
        CHECK(b.video.w == 1920 && b.video.h == 1044 && b.bars == 18, "16:9 1080p: 1920x1044, bars %d", b.bars);
        CHECK(c.video.h == 1440 && c.video.w == 2648 && c.side == 1236, "32:9: 2648x1440, sides %d", c.side);
        CHECK(d.video.h == 1440 && d.video.w == 2648 && d.side == 396, "21:9: 2648x1440, sides %d", d.side);
    }
    {
        vl_layout_t z = vl_layout(0, 1080, 640, 348);
        CHECK(z.video.w == 0 && z.video.h == 0, "an empty output has no picture");
    }
}

static void test_output_size(void) {
    int w, h;
    vl_output_size(5120, 1440, 0, 0, &w, &h);
    CHECK(w == 5120 && h == 1440, "no box: the client");
    vl_output_size(5120, 1440, 7680, 2160, &w, &h);
    CHECK(w == 5120 && h == 1440, "a box larger than the client: the client (%dx%d)", w, h);
    vl_output_size(5120, 1440, 3840, 2160, &w, &h);
    CHECK(w == 3840 && h == 1080, "a 16:9 box for a 32:9 client: 32:9 inside it (%dx%d)", w, h);
    vl_output_size(2560, 1440, 1280, 720, &w, &h);
    CHECK(w == 1280 && h == 720, "a smaller box: the box (%dx%d)", w, h);
    vl_output_size(960, 720, 7680, 2160, &w, &h);
    CHECK(w == 960 && h == 720, "a 4:3 window: its own size (%dx%d)", w, h);
    vl_output_size(1920, 1080, 2560, 1920, &w, &h);
    CHECK(w == 1920 && h == 1080, "the client's shape always (%dx%d)", w, h);
    vl_output_size(0, 0, 1920, 1080, &w, &h);
    CHECK(w == 0 && h == 0, "no client: nothing");
}

static void test_names(void) {
    char out[64];
    vl_display_name("menu\\newgame", out, sizeof out);
    CHECK(!strcmp(out, "newgame.smk"), "menu\\newgame -> %s", out);
    vl_display_name("tpassintro", out, sizeof out);
    CHECK(!strcmp(out, "tpassintro.smk"), "no folder -> %s", out);
    vl_display_name("a/b/win.smk", out, sizeof out);
    CHECK(!strcmp(out, "win.smk"), "keeps an extension -> %s", out);
    vl_display_name("menu\\averyveryverylongname", out, 12);
    CHECK(strlen(out) < 12 && strstr(out, ".smk"), "cut to fit -> %s", out);
    vl_display_name(NULL, out, sizeof out);
    CHECK(out[0] == 0, "no name");
}

static vl_palette_map_t g_map;

static void test_palette_map(void) {
    uint8_t pal[768];
    int exact = 1, bounded = 1;
    for (int i = 0; i < 256; i++) {
        pal[i * 3] = (uint8_t)i;
        pal[i * 3 + 1] = (uint8_t)(255 - i);
        pal[i * 3 + 2] = (uint8_t)((i * 7) & 255);
    }
    memset(&g_map, 0, sizeof g_map);
    CHECK(vl_palette_map_update(&g_map, pal) == 1, "a first palette builds the map");
    CHECK(vl_palette_map_update(&g_map, pal) == 0, "the same palette changes nothing");
    for (int i = 0; i < 256; i++) {
        uint16_t code = vl_rgb565(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
        uint32_t m = g_map.map[code], want = 0xFF000000u | (uint32_t)pal[i * 3] << 16 | (uint32_t)pal[i * 3 + 1] << 8 |
                                            pal[i * 3 + 2];
        if (!m) exact = 0;
        if (m != want && !rgb_close(m, want, 7)) bounded = 0;
    }
    CHECK(exact, "every palette entry's code maps to a palette colour");
    CHECK(bounded, "a shared code maps within one RGB565 step of each colour sharing it");
    CHECK(g_map.count > 0 && g_map.count <= 256, "codes kept: %d", g_map.count);
    pal[0] = 200;
    CHECK(vl_palette_map_update(&g_map, pal) == 1, "a changed entry rebuilds the map");
    CHECK(g_map.map[vl_rgb565(0, 255, 0)] == 0xFF01FE07u, "a code entry 0 left maps to the entry still sharing it");
    CHECK(g_map.map[vl_rgb565(0, 0, 255)] == 0, "a code no entry has maps to nothing");
    CHECK(g_map.map[vl_rgb565(200, 255, 0)] == 0xFFC8FF00u, "the new colour maps");
}

static void test_session(void) {
    vl_session_t s;
    memset(&s, 0, sizeof s);
    CHECK(vl_session_route(&s, 1, 1, 0, 0) == VL_SHOW_CLASSIC, "no player: the game's own present");
    vl_session_begin(&s);
    CHECK(vl_session_route(&s, 1, 0, 0, 0) == VL_SHOW_CLASSIC, "before the raster exists: the game's own present");
    CHECK(vl_session_route(&s, 1, 1, 1, 0) == VL_SHOW_BLACK, "the player's three clears: black on the video surface");
    CHECK(vl_session_route(&s, 1, 1, 0, 0) == VL_SHOW_FRAME, "a decoded frame");
    CHECK(vl_session_route(&s, 0, 1, 0, 0) == VL_SHOW_CLASSIC, "software renderer: the game's own playback");
    CHECK(vl_session_route(&s, 1, 1, 0, 1) == VL_SHOW_CLASSIC, "the decoder's window blit: the game's own playback");
    vl_session_shown(&s, VL_SHOW_BLACK, 0, 0.5);
    CHECK(s.overlay && !s.started && s.frames == 0, "a black clear puts the surface up without counting a frame");
    CHECK(!vl_session_main_presented(&s), "a present of the game's own surface while playing keeps the video up");
    for (int f = 0; f < 10; f++) vl_session_shown(&s, VL_SHOW_FRAME, f == 9 ? 11 : f, 1.0 + f);
    CHECK(s.frames == 10 && s.dropped == 2 && s.last_frame == 11 && s.max_ms == 10.0, "frames %u dropped %u max %.1f",
          s.frames, s.dropped, s.max_ms);
    vl_session_end(&s);
    CHECK(!s.playing && s.overlay, "the last frame stays up after the player returns (a skip ends it the same way)");
    CHECK(vl_session_route(&s, 1, 1, 0, 0) == VL_SHOW_CLASSIC, "after the player: the game's own presents");
    CHECK(vl_session_main_presented(&s) && !s.overlay, "the next picture of the game's own takes over");
    CHECK(!vl_session_main_presented(&s), "and only once");

    vl_session_begin(&s);
    vl_session_shown(&s, VL_SHOW_FRAME, 0, 1.0);
    vl_session_failed(&s);
    CHECK(vl_session_route(&s, 1, 1, 0, 0) == VL_SHOW_CLASSIC, "a failure: the game's own playback");
    CHECK(vl_session_main_presented(&s), "and its first present hides the video surface");
    vl_session_end(&s);

    s.overlay = 1;
    vl_session_begin(&s);
    CHECK(s.overlay && s.playing && !s.failed && s.frames == 0, "a second video (credits after win) keeps the surface");
    CHECK(vl_session_route(&s, 1, 0, 0, 0) == VL_SHOW_CLASSIC && !vl_session_main_presented(&s),
          "its probe present stays under the surface");
    vl_session_end(&s);
    vl_session_main_presented(&s);

    vl_session_begin(&s);
    CHECK(!vl_session_stray_line(&s, VL_STRAY_LINE_MIN - 1) && !s.stray_line,
          "picture content reaching the bottom edge is not a stray line");
    CHECK(vl_session_stray_line(&s, 363) && s.stray_line, "credits' first card marks its last row as a stray line");
    CHECK(!vl_session_stray_line(&s, 400) && !vl_session_stray_line(&s, 4) && s.stray_line,
          "once, and it holds for the faded cards whose line is down to 4 pixels");
    vl_session_end(&s);
    vl_session_begin(&s);
    CHECK(!s.stray_line, "the next video starts without it");
    vl_session_end(&s);
}

static void test_lone_pixels(void) {
    static uint16_t pic[8][16];
    memset(pic, 0, sizeof pic);
    for (int x = 4; x < 12; x++) pic[7][x] = vl_rgb565(32, 32, 32);
    CHECK(vl_lone_bottom_pixels((const uint8_t*)pic, sizeof pic[0], 16, 8) == 8, "a grey line over black: 8 lone");
    pic[5][6] = vl_rgb565(200, 200, 200);
    pic[3][9] = vl_rgb565(200, 200, 200);
    CHECK(vl_lone_bottom_pixels((const uint8_t*)pic, sizeof pic[0], 16, 8) == 7,
          "a pixel with anything in its block above is not lone; above the block does not count");
    pic[7][0] = vl_rgb565(0, 4, 0);
    CHECK(vl_lone_bottom_pixels((const uint8_t*)pic, sizeof pic[0], 16, 8) == 8, "any non-black code is lit");
    CHECK(vl_lone_bottom_pixels((const uint8_t*)pic, sizeof pic[0], 16, 3) == 0 &&
          vl_lone_bottom_pixels(NULL, 32, 16, 8) == 0, "too short or no picture: none");
    CHECK(vl_lone_bottom_pixels((const uint8_t*)pic, sizeof pic[0], 16, 7) == 0, "only the last row is looked at");
}

static void test_video_test_state(void) {
    vt_state_t s;
    vl_session_t session;
    vl_play_mode_t forced = VL_PLAY_HD, mode = VL_PLAY_AUTO;
    int video = -1;
    memset(&s, 0, sizeof s);
    for (int other = 0; other < 3; other++)
        CHECK(!vt_f5(&s, 0) && !s.open, "F5 in %s does nothing",
              other == 0 ? "gameplay" : other == 1 ? "Options" : "another menu");
    CHECK(vt_f5(&s, 1) && s.open && vt_visible(&s), "F5 in the main menu opens the panel");
    CHECK(vt_f5(&s, 1) && !s.open && !vt_visible(&s), "F5 again closes it");
    CHECK(!vt_request(&s, 0, VL_PLAY_HD), "a closed panel asks for nothing");
    vt_f5(&s, 1);
    CHECK(!vt_request(&s, 0, VL_PLAY_AUTO) && !vt_request(&s, VT_VIDEO_COUNT, VL_PLAY_HD) && !vt_request(&s, -1, VL_PLAY_HD),
          "only Original or HD of the four rows");
    CHECK(vt_request(&s, 3, VL_PLAY_ORIGINAL) && s.pending, "win.smk Original waits for the menu's frame");
    CHECK(!vt_request(&s, 0, VL_PLAY_HD), "a second request while one waits is refused");
    CHECK(!vt_start(&s, 0, &video, &mode) && s.pending, "it waits while another window is in front");
    CHECK(vt_start(&s, 1, &video, &mode) && video == 3 && mode == VL_PLAY_ORIGINAL && s.playing && !s.pending,
          "it starts at the main menu's frame");
    CHECK(!vt_start(&s, 1, &video, &mode), "once");
    CHECK(!vt_visible(&s) && s.open, "the panel hides while its video plays and stays open for after");
    CHECK(!vt_request(&s, 0, VL_PLAY_HD), "no second playback while one runs");
    CHECK(!vt_f5(&s, 0) && s.open, "F5 during the video is not the panel's");
    vt_finished(&s);
    CHECK(vt_visible(&s), "the panel is back after the video");
    CHECK(vt_request(&s, 1, VL_PLAY_HD) && vt_f5(&s, 1) && !s.pending && !vt_start(&s, 1, &video, &mode),
          "closing the panel drops a waiting request");
    CHECK(!vt_f5(&s, 0) && !s.open, "a closed panel leaves F5 outside the main menu alone");

    CHECK(vl_play_mode_take(&forced) == VL_PLAY_HD && forced == VL_PLAY_AUTO, "the player takes the forced mode once");
    CHECK(vl_play_mode_take(&forced) == VL_PLAY_AUTO, "the next video is AUTO again, whether the forced one ended, was "
                                                      "skipped or failed");
    CHECK(!strcmp(vl_play_mode_name(VL_PLAY_ORIGINAL), "ORIGINAL") && !strcmp(vl_play_mode_name(VL_PLAY_HD), "HD"),
          "mode names for the log");
    CHECK(!vl_play_uses_session(VL_PLAY_ORIGINAL) && vl_play_uses_session(VL_PLAY_HD) && vl_play_uses_session(VL_PLAY_AUTO),
          "ORIGINAL plays without an HD session");
    memset(&session, 0, sizeof session);
    CHECK(vl_session_route(&session, 1, 1, 0, 0) == VL_SHOW_CLASSIC, "without a session every present is the game's own");
    vl_session_begin(&session);
    CHECK(vl_session_route(&session, 1, 1, 0, 0) == VL_SHOW_FRAME, "with one, HD draws every frame");
    vl_session_end(&session);
}

/* ---- retail files ---- */

typedef struct {
    uint32_t va, raw, size;
} section_t;

static uint8_t* g_exe;
static size_t g_exe_size;
static section_t g_sections[16];
static int g_section_count;

static const uint8_t* exe_at(uint32_t va, uint32_t bytes) {
    for (int i = 0; i < g_section_count; i++) {
        uint32_t rva = va - 0x00400000u;
        if (rva >= g_sections[i].va && rva + bytes <= g_sections[i].va + g_sections[i].size &&
            g_sections[i].raw + (rva - g_sections[i].va) + bytes <= g_exe_size)
            return g_exe + g_sections[i].raw + (rva - g_sections[i].va);
    }
    return NULL;
}

static int load_exe(const char* path) {
    uint32_t pe;
    int count;
    g_exe = read_file(path, &g_exe_size);
    if (!g_exe || g_exe_size < 0x400) return 0;
    pe = *(const uint32_t*)(g_exe + 0x3C);
    count = *(const uint16_t*)(g_exe + pe + 6);
    for (int i = 0; i < count && i < 16; i++) {
        const uint8_t* s = g_exe + pe + 24 + *(const uint16_t*)(g_exe + pe + 20) + i * 40;
        g_sections[i].va = *(const uint32_t*)(s + 12);
        g_sections[i].size = *(const uint32_t*)(s + 16);
        g_sections[i].raw = *(const uint32_t*)(s + 20);
        g_section_count = i + 1;
    }
    return 1;
}

static void test_inventory(const char* root) {
    char path[MAX_PATH];
    int all_calls = 0;
    for (int i = 0; i < 4; i++) {
        const retail_video_t* v = &k_videos[i];
        size_t size = 0;
        uint8_t* data;
        vl_smk_info_t info;
        snprintf(path, sizeof path, "%s/game/data/menu/%s.smk", root, v->file);
        data = read_file(path, &size);
        if (!data) {
            printf("video_tests: %s not installed, retail inventory skipped\n", path);
            return;
        }
        CHECK(vl_smk_parse(data, (uint32_t)size, &info), "%s.smk parses", v->file);
        CHECK(info.width == 640 && info.height == v->stored_rows && info.frames == v->frames && info.flags == v->flags,
              "%s.smk: %ux%u, %u frames, flags %u", v->file, info.width, info.height, info.frames, info.flags);
        CHECK(info.frame_rate == -6666, "%s.smk: 66.66 ms per frame", v->file);
        CHECK(info.audio_rate == 22050 && info.audio_bits == 16 && info.audio_channels == 2,
              "%s.smk: 22050 Hz 16-bit stereo", v->file);
        printf("  %-11s %s  %ux%u shown %ux%u  %.3g fps  %u frames  %.1f s  Smacker 2, 8-bit palette, %s  audio %u Hz "
               "%u-bit %s%s  %zu bytes\n", v->file, info.flags & VL_SMK_Y_INTERLACED ? "Y-interlaced" : "progressive ",
               info.width, info.height, info.width, vl_smk_shown_rows(info.height, info.flags),
               1000.0 / vl_smk_ms_per_frame(info.frame_rate), info.frames,
               info.frames * vl_smk_ms_per_frame(info.frame_rate) / 1000.0, "SMK2", info.audio_rate, info.audio_bits,
               info.audio_channels == 2 ? "stereo" : "mono", info.audio_compressed ? " (compressed)" : "", size);
        free(data);
    }
    snprintf(path, sizeof path, "%s/game/trespass.exe", root);
    if (!load_exe(path)) {
        printf("video_tests: %s not installed, call sites skipped\n", path);
        return;
    }
    for (int i = 0; i < (int)(sizeof k_calls / sizeof k_calls[0]); i++) {
        const call_site_t* c = &k_calls[i];
        const uint8_t* text = exe_at(c->string_va, (uint32_t)strlen(c->name) + 1);
        const uint8_t* push = exe_at(c->push_va, 5);
        const uint8_t* call = exe_at(c->call_va, 5);
        CHECK(text && !strcmp((const char*)text, c->name), "the string at 0x%08X is %s", c->string_va, c->name);
        CHECK(push && push[0] == 0x68 && *(const uint32_t*)(push + 1) == c->string_va, "0x%08X pushes %s", c->push_va,
              c->name);
        CHECK(call && call[0] == 0xE8 && c->call_va + 5 + *(const int32_t*)(call + 1) == PLAYER_VA,
              "0x%08X calls CVideoWnd::Play", c->call_va);
    }
    for (int i = 0; i < g_section_count; i++) {
        if (g_sections[i].va != 0x1000u) continue;
        for (uint32_t o = 0; o + 5 <= g_sections[i].size; o++) {
            const uint8_t* p = g_exe + g_sections[i].raw + o;
            uint32_t va = 0x00401000u + o;
            if (p[0] == 0xE8 && va + 5 + *(const int32_t*)(p + 1) == PLAYER_VA) all_calls++;
        }
    }
    CHECK(all_calls == 5, "CVideoWnd::Play has exactly five callers (%d)", all_calls);
    {
        const uint8_t* push = exe_at(0x00418B70u, 5);
        CHECK(push && push[0] == 0xE8 && 0x00418B75u + *(const int32_t*)(push + 1) == 0x00417110u,
              "Play puts its window on the game's stack before its first frame: F5 never finds the main menu alone in "
              "front during a video");
    }
    for (int i = 0; i < VT_VIDEO_COUNT; i++) {
        char want[32];
        const uint8_t* text = exe_at(GVA_AT(vt_videos[i].name_id), 16);
        snprintf(want, sizeof want, "menu\\%.*s", (int)(strlen(vt_videos[i].file) - 4), vt_videos[i].file);
        CHECK(text && !strcmp((const char*)text, want), "Video Test row %d plays the game's own string %s", i, want);
    }
    free(g_exe);
    g_exe = NULL;
}

/* ---- the original decoder ---- */

typedef void*(__stdcall* smack_open_fn)(const char*, uint32_t, uint32_t);
typedef void(__stdcall* smack_close_fn)(void*);
typedef uint32_t(__stdcall* smack_do_fn)(void*);
typedef void(__stdcall* smack_next_fn)(void*);
typedef void(__stdcall* smack_to_buffer_fn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void*, uint32_t);
typedef uint32_t(__stdcall* smack_rect_fn)(void*, uint32_t);

typedef struct {
    uint16_t screen[SCREEN_H][SCREEN_W];
    uint8_t palette[768];
    uint32_t shown, flags, frame;
    int loaded;
} decoded_t;

#define CAPTURES 4

typedef struct {
    const char* file;
    int captures;
    uint32_t capture[CAPTURES];
    decoded_t* out;
    uint32_t frames;
    int odd_black, even_black, seen;
    int lone_max, stray_first, stray_frames;
    long lit_after, lone_after;
} scan_t;

static decoded_t g_newgame[1], g_intro[CAPTURES], g_win[1], g_credits[3];

static scan_t g_scans[] = {
    { "newgame", 1, { 540 }, g_newgame },
    { "tpassintro", 4, { 150, 300, 600, 792 }, g_intro },
    { "win", 1, { 303 }, g_win },
    { "credits", 3, { 100, 500, 651 }, g_credits },
};
#define SCAN_COUNT (int)(sizeof g_scans / sizeof g_scans[0])

/* As CVideoWnd::Play decodes: RGB565 into a cleared 640x480 raster, the picture centred; no track is opened. Every
 * frame is decoded; the listed ones are kept. */
static int decode(HMODULE dll, const char* root, scan_t* v) {
    smack_open_fn open = (smack_open_fn)GetProcAddress(dll, "_SmackOpen@12");
    smack_close_fn close = (smack_close_fn)GetProcAddress(dll, "_SmackClose@4");
    smack_do_fn decode_frame = (smack_do_fn)GetProcAddress(dll, "_SmackDoFrame@4");
    smack_next_fn next = (smack_next_fn)GetProcAddress(dll, "_SmackNextFrame@4");
    smack_to_buffer_fn to_buffer = (smack_to_buffer_fn)GetProcAddress(dll, "_SmackToBuffer@28");
    smack_rect_fn rect = (smack_rect_fn)GetProcAddress(dll, "_SmackToBufferRect@8");
    static uint16_t screen[SCREEN_H][SCREEN_W];
    char path[MAX_PATH];
    uint8_t* smk;
    uint32_t top, shown, flags;
    int kept = 0;
    if (!open || !close || !decode_frame || !next || !to_buffer || !rect) return 0;
    snprintf(path, sizeof path, "%s/game/data/menu/%s.smk", root, v->file);
    smk = (uint8_t*)open(path, 0, 0xFFFFFFFFu);
    if (!smk) return 0;
    memset(screen, 0, sizeof screen);
    shown = *(uint32_t*)(smk + 8);
    flags = *(uint32_t*)(smk + 0x14);
    v->frames = *(uint32_t*)(smk + 0x0C);
    v->stray_first = -1;
    top = SCREEN_H / 2 - shown / 2;
    to_buffer(smk, 0, top, SCREEN_W * 2, SCREEN_H, screen, 0xC0000000u);
    for (uint32_t f = 0; f < v->frames; f++) {
        int odd = 1, even = 1;
        decode_frame(smk);
        while (rect(smk, 1)) {}
        for (uint32_t y = 0; y < shown; y++)
            for (int x = 0; x < SCREEN_W; x++)
                if (screen[top + y][x]) {
                    if (y & 1) odd = 0;
                    else even = 0;
                    break;
                }
        if (!even) {
            v->odd_black += odd;
            v->even_black += even;
            v->seen++;
        }
        if (vl_policy_for(flags, shown).rows == VL_ROWS_PROGRESSIVE) {
            int lone = vl_lone_bottom_pixels((const uint8_t*)screen[top], SCREEN_W * 2, SCREEN_W, (int)shown);
            if (lone > v->lone_max) v->lone_max = lone;
            if (lone >= VL_STRAY_LINE_MIN && v->stray_first < 0) v->stray_first = (int)f;
            if (lone >= VL_STRAY_LINE_MIN) v->stray_frames++;
            if (v->stray_first >= 0) {
                for (int x = 0; x < SCREEN_W; x++) v->lit_after += screen[top + shown - 1][x] != 0;
                v->lone_after += lone;
            }
        }
        if (kept < v->captures && f == v->capture[kept]) {
            decoded_t* d = &v->out[kept++];
            memcpy(d->screen, screen, sizeof screen);
            memcpy(d->palette, smk + 0x6C, sizeof d->palette);
            d->shown = shown;
            d->flags = flags;
            d->frame = f;
            d->loaded = 1;
        }
        if (f + 1 < v->frames) next(smk);
    }
    close(smk);
    return kept == v->captures;
}

static void test_decoder(const char* root, HMODULE dll) {
    for (int i = 0; i < SCAN_COUNT; i++) {
        scan_t* v = &g_scans[i];
        if (!decode(dll, root, v)) {
            CHECK(0, "%s.smk decodes through SMACKW32.DLL", v->file);
            continue;
        }
        if (v->out[0].flags & VL_SMK_Y_INTERLACED) {
            CHECK(v->out[0].shown == 344, "the decoder shows newgame at %u rows (172 stored)", v->out[0].shown);
            CHECK(v->seen > 100 && v->odd_black == v->seen,
                  "newgame: every second row of the picture stays black in every frame (%d of %d)", v->odd_black,
                  v->seen);
            continue;
        }
        CHECK(v->out[0].shown == 348, "%s: 348 rows", v->file);
        CHECK(v->odd_black == 0, "%s: no frame with the odd rows black (%d of %d)", v->file, v->odd_black, v->seen);
        printf("  %-10s last row: at most %d lone pixels in a frame; stray line %s", v->file, v->lone_max,
               v->stray_first < 0 ? "never\n" : "");
        if (v->stray_first >= 0)
            printf("from frame %d (%d frames >= %d), %ld of %ld lit pixels lone after it\n", v->stray_first,
                   v->stray_frames, VL_STRAY_LINE_MIN, v->lone_after, v->lit_after);
        if (!strcmp(v->file, "credits")) {
            CHECK(v->stray_first >= 0 && v->stray_first <= 20 && v->lone_max >= 360,
                  "credits: its last row is a stray line from its first card (frame %d, %d pixels)", v->stray_first,
                  v->lone_max);
            CHECK(v->lit_after > 100000 && v->lone_after == v->lit_after,
                  "credits: after that, every lit pixel of the last row stands alone over black (%ld of %ld)",
                  v->lone_after, v->lit_after);
        } else {
            CHECK(v->stray_first < 0 && v->lone_max * 3 < VL_STRAY_LINE_MIN,
                  "%s: never taken for a stray line (at most %d lone pixels)", v->file, v->lone_max);
        }
    }
}

/* ---- GPU passes on WARP ---- */

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;

static int warp_open(void) {
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    return SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, 3, D3D11_SDK_VERSION, &g_dev, NULL,
                                       &g_ctx));
}

/* The frame drawn on an out_w x out_h target, read back as 0xAARRGGBB; NULL on a failure. */
static uint32_t* draw(video_gpu_t* g, const video_gpu_frame_t* f, int out_w, int out_h, int content_w,
                      int content_h) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    vl_layout_t l = vl_layout(out_w, out_h, content_w, content_h);
    uint32_t* out = NULL;
    memset(&d, 0, sizeof d);
    d.Width = (UINT)out_w;
    d.Height = (UINT)out_h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)tex, NULL, &rtv)))
        goto done;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &staging))) goto done;
    if (!video_gpu_draw(g, f, &l, out_w, out_h, rtv)) {
        printf("video_gpu_draw: %s\n", video_gpu_error());
        goto done;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) goto done;
    out = (uint32_t*)malloc((size_t)out_w * out_h * 4);
    if (out)
        for (int y = 0; y < out_h; y++) memcpy(out + (size_t)y * out_w, (uint8_t*)m.pData + (size_t)y * m.RowPitch, out_w * 4);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)staging, 0);
done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (tex) ID3D11Texture2D_Release(tex);
    return out;
}

static video_gpu_frame_t frame_of(const uint16_t* rows, int width, int stride_px, vl_policy_t policy) {
    video_gpu_frame_t f;
    memset(&f, 0, sizeof f);
    f.rows = (const uint8_t*)rows;
    f.stride = (long)stride_px * 2;
    f.width = width;
    f.policy = policy;
    f.rgb565 = 1;
    return f;
}

#define TW 64
#define TH 48

static uint32_t g_lcg = 12345;
static uint32_t next_random(void) { return g_lcg = g_lcg * 1103515245u + 12345u; }

/* A decoded frame as video_hd.c hands it over, with the stages chosen. */
static uint32_t* draw_decoded(video_gpu_t* g, const decoded_t* d, int smooth, int deblock, int stray, int out_w,
                              int out_h) {
    vl_policy_t policy = vl_policy_for(d->flags, d->shown);
    uint32_t top = SCREEN_H / 2 - d->shown / 2;
    int rebuild = policy.rows == VL_ROWS_REBUILD;
    video_gpu_frame_t f = frame_of(&d->screen[top][0], SCREEN_W, rebuild ? SCREEN_W * 2 : SCREEN_W, policy);
    memset(&g_map, 0, sizeof g_map);
    vl_palette_map_update(&g_map, d->palette);
    f.palette_map = g_map.map;
    f.palette_changed = 1;
    f.smooth = smooth;
    f.deblock = deblock;
    f.stray_bottom_line = stray;
    return draw(g, &f, out_w, out_h, SCREEN_W, (int)d->shown);
}

/* Flat 4x4 blocks in a staircase of small steps along x and y: the edges spread into ramps, the mean stays. */
static void test_gpu_deblock_ramp(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    video_gpu_frame_t f;
    uint32_t *plain, *out;
    double edge_before = 0.0, edge_after = 0.0, mean_before = 0.0, mean_after = 0.0, worst = 0.0, worst_before = 0.0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int v = 40 + 6 * (x / 4) + 4 * (y / 4);
            pic[y][x] = vl_rgb565((uint8_t)v, (uint8_t)v, (uint8_t)v);
        }
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    plain = draw(g, &f, TW, TH, TW, TH);
    f.deblock = 1;
    out = draw(g, &f, TW, TH, TW, TH);
    if (!plain || !out) {
        CHECK(0, "a staircase draws");
        free(plain);
        free(out);
        return;
    }
    for (int y = 4; y < TH - 4; y++)
        for (int x = 4; x < TW - 5; x++) {
            double b = fabs(luma(plain[y * TW + x + 1]) - luma(plain[y * TW + x]));
            double a = fabs(luma(out[y * TW + x + 1]) - luma(out[y * TW + x]));
            if ((x & 3) == 3) {
                edge_before += b;
                edge_after += a;
            }
            worst = fmax(worst, a);
            worst_before = fmax(worst_before, b);
        }
    for (int i = 0; i < TW * TH; i++) {
        mean_before += luma(plain[i]);
        mean_after += luma(out[i]);
    }
    CHECK(edge_after < edge_before * 0.35, "a staircase of 6-level steps: block edges %.1f -> %.1f", edge_before,
          edge_after);
    CHECK(worst <= worst_before * 0.5, "every step of the ramp is at most half the staircase's (%.2f vs %.2f)", worst,
          worst_before);
    CHECK(fabs(mean_before - mean_after) / (TW * TH) < 0.3, "the mean level stays (%.2f -> %.2f)",
          mean_before / (TW * TH), mean_after / (TW * TH));
    free(plain);
    free(out);
}

static int max_change(const uint32_t* a, const uint32_t* b, int n) {
    int worst = 0;
    for (int i = 0; i < n; i++)
        for (int s = 0; s < 24; s += 8) {
            int d = abs((int)((a[i] >> s) & 255) - (int)((b[i] >> s) & 255));
            if (d > worst) worst = d;
        }
    return worst;
}

/* What deblocking must leave alone: a real edge on a block edge, texture, a lone star, text-like strokes. */
static void test_gpu_deblock_keeps(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    static const char* const names[] = { "a 160-level edge on a block edge", "a fine 50-level texture",
                                         "a star in a flat dark sky", "thin bright strokes on black" };
    for (int k = 0; k < 4; k++) {
        video_gpu_frame_t f;
        uint32_t *plain, *out;
        for (int y = 0; y < TH; y++)
            for (int x = 0; x < TW; x++) {
                int v = k == 0   ? (x < 32 ? 40 : 200)
                        : k == 1 ? 100 + ((x + y) & 1 ? 25 : -25) + 2 * (x / 8)
                        : k == 2 ? (x == 13 && y == 21 ? 230 : 18)
                                 : ((x % 7 == 3 || y % 9 == 4) && x > 8 && x < 56 ? 220 : 0);
                pic[y][x] = vl_rgb565((uint8_t)v, (uint8_t)v, (uint8_t)(v + (k == 2 ? 20 : 0)));
            }
        f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
        plain = draw(g, &f, TW, TH, TW, TH);
        f.deblock = 1;
        out = draw(g, &f, TW, TH, TW, TH);
        CHECK(plain && out && max_change(plain, out, TW * TH) <= 1, "%s: unchanged (%d)", names[k],
              plain && out ? max_change(plain, out, TW * TH) : -1);
        free(plain);
        free(out);
    }
}

/* The last row's stray line goes; picture content reaching the bottom edge stays, and nothing changes without the flag
 * or on a half-height file. */
static void test_gpu_stray_line(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    video_gpu_frame_t f;
    uint32_t *off, *on;
    int line_gone = 1, content_kept = 1, rest_same = 1;
    memset(pic, 0, sizeof pic);
    for (int x = 20; x < TW; x++) pic[TH - 1][x] = vl_rgb565(32, 32, 32);
    for (int y = TH - 6; y < TH; y++)
        for (int x = 4; x < 9; x++) pic[y][x] = vl_rgb565(180, 120, 60);
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    off = draw(g, &f, TW, TH, TW, TH);
    f.stray_bottom_line = 1;
    on = draw(g, &f, TW, TH, TW, TH);
    if (!off || !on) {
        CHECK(0, "a stray-line frame draws");
        free(off);
        free(on);
        return;
    }
    for (int x = 0; x < TW; x++) {
        if (x >= 20 && (on[(TH - 1) * TW + x] & 0xFFFFFFu)) line_gone = 0;
        if (x >= 4 && x < 9 && !rgb_close(on[(TH - 1) * TW + x], off[(TH - 1) * TW + x], 0)) content_kept = 0;
    }
    if (memcmp(off, on, (size_t)TW * (TH - 1) * 4)) rest_same = 0;
    CHECK(off[(TH - 1) * TW + 40] & 0xFFFFFFu, "without the flag the line is drawn as decoded");
    CHECK(line_gone, "with it, the line's pixels take the row above (black)");
    CHECK(content_kept, "a picture reaching the bottom edge keeps its last row");
    CHECK(rest_same, "every other row is untouched");
    free(off);
    free(on);
}

static void test_gpu_identity(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    video_gpu_frame_t f;
    uint32_t* out;
    int exact = 1;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) pic[y][x] = (uint16_t)(next_random() >> 8);
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    out = draw(g, &f, TW, TH, TW, TH);
    CHECK(out != NULL, "a progressive frame draws");
    if (!out) return;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++)
            if (!rgb_close(out[y * TW + x], expand565(pic[y][x]), 1) || (out[y * TW + x] >> 24) != 255) exact = 0;
    CHECK(exact, "progressive at 1:1: every pixel is the decoder's own colour, opaque");
    free(out);
}

static void test_gpu_palette(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    uint8_t pal[768];
    video_gpu_frame_t f;
    uint32_t* out;
    int exact = 1;
    for (int i = 0; i < 256; i++) {
        pal[i * 3] = (uint8_t)(i | 3);
        pal[i * 3 + 1] = (uint8_t)((i * 3) | 1);
        pal[i * 3 + 2] = (uint8_t)(255 - i);
    }
    memset(&g_map, 0, sizeof g_map);
    vl_palette_map_update(&g_map, pal);
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            int i = (x * 4 + y) & 255;
            pic[y][x] = vl_rgb565(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
        }
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    f.palette_map = g_map.map;
    f.palette_changed = 1;
    out = draw(g, &f, TW, TH, TW, TH);
    if (!out) {
        CHECK(0, "a palette frame draws");
        return;
    }
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            uint32_t want = g_map.map[pic[y][x]];
            if (!rgb_close(out[y * TW + x], want, 1)) exact = 0;
        }
    CHECK(exact, "RGB565 codes come back as the palette's 8-bit colours");
    free(out);
}

/* The decoder's screen for a Y-interlaced file: picture on the rows parity + 2k, black between. */
static void test_gpu_rebuild(video_gpu_t* g, int parity) {
    static uint16_t screen[TH][TW];
    static uint16_t stored[TH / 2][TW];
    vl_policy_t policy = vl_policy_for(VL_SMK_Y_INTERLACED, TH);
    video_gpu_frame_t f;
    uint32_t* out;
    int black_rows = 0, kept = 1, bounded = 1;
    memset(screen, 0, sizeof screen);
    for (int k = 0; k < TH / 2; k++)
        for (int x = 0; x < TW; x++) {
            int v = 40 + k * 7 + (x > 2 * k + 8 ? 120 : 0);
            stored[k][x] = vl_rgb565((uint8_t)v, (uint8_t)(v / 2 + 20), (uint8_t)(255 - v));
            screen[parity + 2 * k < TH ? parity + 2 * k : TH - 1][x] = stored[k][x];
        }
    policy.parity = parity;
    f = frame_of(&screen[parity][0], TW, TW * 2, policy);
    out = draw(g, &f, TW, TH, TW, TH);
    if (!out) {
        CHECK(0, "a Y-interlaced frame draws");
        return;
    }
    for (int y = 0; y < TH; y++) {
        int r = y - parity, black = 1;
        for (int x = 0; x < TW; x++)
            if (out[y * TW + x] & 0xFFFFFFu) black = 0;
        black_rows += black;
        for (int x = 0; x < TW; x++) {
            uint32_t c = out[y * TW + x];
            if (r >= 0 && !(r & 1)) {
                if (!rgb_close(c, expand565(stored[r >> 1][x]), 1)) kept = 0;
            } else {
                int a = r < 0 ? 0 : r >> 1, b = r < 0 ? 0 : (r >> 1) + 1 < TH / 2 ? (r >> 1) + 1 : TH / 2 - 1;
                double lo = 1e9, hi = -1e9, l = luma(c);
                for (int dx = -2; dx <= 2; dx++) {
                    int xx = x + dx < 0 ? 0 : x + dx >= TW ? TW - 1 : x + dx;
                    double la = luma(expand565(stored[a][xx])), lb = luma(expand565(stored[b][xx]));
                    lo = fmin(lo, fmin(la, lb));
                    hi = fmax(hi, fmax(la, lb));
                }
                if (l < lo - 1.0 || l > hi + 1.0) bounded = 0;
            }
        }
    }
    CHECK(black_rows == 0, "parity %d: no black row left (%d)", parity, black_rows);
    CHECK(kept, "parity %d: the stored rows are shown unchanged on their own rows", parity);
    CHECK(bounded, "parity %d: rebuilt rows stay within their neighbours (no overshoot)", parity);
    free(out);
}

/* A thin diagonal against the plain average of the rows above and below: the edge-directed rows follow it. */
static void test_gpu_diagonal(video_gpu_t* g) {
    static uint16_t screen[TH][TW];
    static uint8_t ideal[TH][TW];
    video_gpu_frame_t f;
    uint32_t* out;
    double err_rebuilt = 0.0, err_average = 0.0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) ideal[y][x] = (uint8_t)(x >= y + 8 && x < y + 11 ? 255 : 16);
    memset(screen, 0, sizeof screen);
    for (int k = 0; k < TH / 2; k++)
        for (int x = 0; x < TW; x++) {
            uint8_t v = ideal[2 * k][x];
            screen[2 * k][x] = vl_rgb565(v, v, v);
        }
    f = frame_of(&screen[0][0], TW, TW * 2, vl_policy_for(VL_SMK_Y_INTERLACED, TH));
    out = draw(g, &f, TW, TH, TW, TH);
    if (!out) {
        CHECK(0, "a diagonal frame draws");
        return;
    }
    for (int k = 0; k + 1 < TH / 2; k++)
        for (int x = 4; x < TW - 4; x++) {
            int y = 2 * k + 1;
            double avg = (luma(expand565(screen[2 * k][x])) + luma(expand565(screen[2 * k + 2][x]))) / 2.0;
            err_rebuilt += fabs(luma(out[y * TW + x]) - ideal[y][x]);
            err_average += fabs(avg - ideal[y][x]);
        }
    CHECK(err_rebuilt < err_average * 0.5, "edge-directed rows follow a diagonal: error %.0f vs %.0f averaged",
          err_rebuilt, err_average);
    free(out);
}

static void test_gpu_upscale(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    video_gpu_frame_t f;
    uint32_t* out;
    int ow = TW * 4, oh = TH * 4, overshoot = 0, widest = 0;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) pic[y][x] = x < TW / 2 ? vl_rgb565(40, 40, 40) : vl_rgb565(220, 220, 220);
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    out = draw(g, &f, ow, oh, TW, TH);
    if (!out) {
        CHECK(0, "a 4x upscale draws");
        return;
    }
    for (int y = 0; y < oh; y++) {
        int first = -1, last = -1;
        double prev = -1.0;
        for (int x = 0; x < ow; x++) {
            double l = luma(out[y * ow + x]);
            if (l < luma(expand565(pic[0][0])) - 0.5 || l > luma(expand565(pic[0][TW - 1])) + 0.5) overshoot++;
            if (l < prev - 0.5) overshoot++;
            if (l > luma(expand565(pic[0][0])) + 2.0 && l < luma(expand565(pic[0][TW - 1])) - 2.0) {
                if (first < 0) first = x;
                last = x;
            }
            prev = l;
        }
        if (last - first + 1 > widest) widest = last - first + 1;
    }
    CHECK(overshoot == 0, "Catmull-Rom with the 2x2 clamp: a step stays monotonic, no ringing (%d)", overshoot);
    CHECK(widest > 0 && widest <= 8, "a step spans at most two source pixels at 4x (%d px)", widest);
    free(out);
}

/* A bright picture on every screen shape: bars black, the picture where the layout puts it, sides dim and fading. */
static void test_gpu_shapes(video_gpu_t* g) {
    static const int sizes[][2] = { { 640, 480 }, { 1280, 720 }, { 1280, 800 }, { 1720, 720 }, { 2560, 720 },
                                    { 1280, 960 } };
    static uint16_t pic[87][160];
    for (int y = 0; y < 87; y++)
        for (int x = 0; x < 160; x++) pic[y][x] = vl_rgb565((uint8_t)(150 + x / 4), 180, (uint8_t)(100 + y));
    for (int s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++) {
        int w = sizes[s][0], h = sizes[s][1], bars_black = 1, inside_lit = 1, sides_ok = 1;
        vl_layout_t l = vl_layout(w, h, 160, 87);
        video_gpu_frame_t f = frame_of(&pic[0][0], 160, 160, vl_policy_for(0, 87));
        uint32_t* out = draw(g, &f, w, h, 160, 87);
        if (!out) {
            CHECK(0, "%dx%d draws", w, h);
            continue;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t c = out[(size_t)y * w + x] & 0xFFFFFFu;
                int in_rows = y >= l.video.y && y < l.video.y + l.video.h;
                int in_cols = x >= l.video.x && x < l.video.x + l.video.w;
                if (!in_rows && c) bars_black = 0;
                if (in_rows && in_cols && luma(c) < 100.0) inside_lit = 0;
                if (in_rows && !in_cols && (luma(c) > 120.0 || luma(c) < 4.0)) sides_ok = 0;
            }
        CHECK(bars_black, "%dx%d: the bars are black", w, h);
        CHECK(inside_lit, "%dx%d: the picture fills its rectangle", w, h);
        CHECK(sides_ok, "%dx%d: the sides are a dim extension of the picture", w, h);
        if (l.side > 8) {
            int y = l.video.y + l.video.h / 2;
            double near_edge = luma(out[(size_t)y * w + l.video.x - 2]), far_edge = luma(out[(size_t)y * w + 1]);
            CHECK(near_edge > far_edge + 5.0, "%dx%d: the extension fades outwards (%.0f -> %.0f)", w, h, near_edge,
                  far_edge);
        }
        free(out);
    }
}

static void test_gpu_determinism(video_gpu_t* g) {
    static uint16_t pic[TH][TW];
    video_gpu_frame_t f;
    uint32_t *a, *b;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) pic[y][x] = (uint16_t)(next_random() >> 9);
    f = frame_of(&pic[0][0], TW, TW, vl_policy_for(0, TH));
    f.smooth = 1;
    f.deblock = 1;
    a = draw(g, &f, 400, 120, TW, TH);
    b = draw(g, &f, 400, 120, TW, TH);
    CHECK(a && b && !memcmp(a, b, 400 * 120 * 4), "the same frame draws the same pixels");
    free(a);
    free(b);
}

static void test_gpu_refusals(video_gpu_t* g) {
    static uint16_t pic[4][4];
    video_gpu_frame_t f = frame_of(&pic[0][0], 4, 4, vl_policy_for(0, 4));
    vl_layout_t l = vl_layout(16, 16, 4, 4);
    f.width = 5000;
    CHECK(!video_gpu_draw(g, &f, &l, 16, 16, NULL), "a frame wider than any texture is refused");
    f.width = 4;
    f.rows = NULL;
    CHECK(!video_gpu_draw(g, &f, &l, 16, 16, NULL), "no rows: refused");
}

/* ---- real frames ---- */

static double checker_energy(const uint32_t* p, int w, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    long n = 0;
    for (int y = y0; y + 1 < y1; y += 2)
        for (int x = x0; x + 1 < x1; x += 2) {
            double a = luma(p[y * w + x]), b = luma(p[y * w + x + 1]), c = luma(p[(y + 1) * w + x]),
                   d = luma(p[(y + 1) * w + x + 1]);
            sum += fabs(a - b - c + d) / 4.0;
            n++;
        }
    return n ? sum / n : 0.0;
}

static double row_alternation(const uint32_t* p, int w, int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    long n = 0;
    for (int y = y0; y + 1 < y1; y += 2)
        for (int x = x0; x + 1 < x1; x += 2) {
            double a = luma(p[y * w + x]), b = luma(p[y * w + x + 1]), c = luma(p[(y + 1) * w + x]),
                   d = luma(p[(y + 1) * w + x + 1]);
            sum += fabs(a + b - c - d) / 4.0;
            n++;
        }
    return n ? sum / n : 0.0;
}

/* Gradient energy kept on strong edges (> 80 levels) and on mid-contrast texture (30-80), and the mean change. */
static void edge_kept(const uint32_t* before, const uint32_t* after, int w, int h, double* kept, double* texture,
                      double* change) {
    double sb = 0.0, sa = 0.0, tb = 0.0, ta = 0.0, diff = 0.0;
    for (int y = 1; y + 1 < h; y++)
        for (int x = 1; x + 1 < w; x++) {
            double gb = fabs(luma(before[y * w + x + 1]) - luma(before[y * w + x - 1])) +
                        fabs(luma(before[(y + 1) * w + x]) - luma(before[(y - 1) * w + x]));
            double ga = fabs(luma(after[y * w + x + 1]) - luma(after[y * w + x - 1])) +
                        fabs(luma(after[(y + 1) * w + x]) - luma(after[(y - 1) * w + x]));
            if (gb > 80.0) {
                sb += gb;
                sa += ga;
            } else if (gb > 30.0) {
                tb += gb;
                ta += ga;
            }
            diff += fabs(luma(after[y * w + x]) - luma(before[y * w + x]));
        }
    *kept = sb > 0.0 ? sa / sb : 1.0;
    *texture = tb > 0.0 ? ta / tb : 1.0;
    *change = diff / ((double)(w - 2) * (h - 2));
}

static void dump_decoded(const char* name, const decoded_t* d) {
    uint32_t* p;
    uint32_t top = SCREEN_H / 2 - d->shown / 2;
    if (!g_dump) return;
    p = (uint32_t*)malloc((size_t)SCREEN_W * d->shown * 4);
    if (!p) return;
    for (uint32_t y = 0; y < d->shown; y++)
        for (int x = 0; x < SCREEN_W; x++) p[y * SCREEN_W + x] = expand565(d->screen[top + y][x]);
    dump_ppm(name, p, SCREEN_W, (int)d->shown);
    free(p);
}

static void dump_hd(const char* name, video_gpu_t* g, const decoded_t* d, int stray) {
    static const int sizes[][2] = { { 1920, 1080 }, { 2560, 1920 }, { 5120, 1440 } };
    char file[64];
    if (!g_dump) return;
    for (int i = 0; i < 3; i++) {
        uint32_t* out = draw_decoded(g, d, 1, 1, stray, sizes[i][0], sizes[i][1]);
        snprintf(file, sizeof file, "%s_hd_%dx%d", name, sizes[i][0], sizes[i][1]);
        dump_ppm(file, out, sizes[i][0], sizes[i][1]);
        free(out);
    }
}

/* Mean luma step across the codec's 4-pixel block edges over the mean step inside blocks, along x (axis 0) or y (1). */
static double block_ratio(const uint32_t* p, int w, int h, int axis) {
    double edge = 0.0, inner = 0.0;
    long ne = 0, ni = 0;
    for (int y = 0; y + axis < h; y++)
        for (int x = 0; x + 1 - axis < w; x++) {
            double d = fabs(luma(p[y * w + x]) - luma(p[(y + axis) * w + x + 1 - axis]));
            if (((axis ? y : x) & 3) == 3) {
                edge += d;
                ne++;
            } else {
                inner += d;
                ni++;
            }
        }
    return ne && ni && inner > 0.0 ? (edge / ne) / (inner / ni) : 1.0;
}

static int flat_block(const uint32_t* p, int w, int x0, int y0) {
    uint32_t c = p[y0 * w + x0] & 0xFFFFFFu;
    for (int y = y0; y < y0 + 4; y++)
        for (int x = x0; x < x0 + 4; x++)
            if ((p[y * w + x] & 0xFFFFFFu) != c) return 0;
    return 1;
}

/* Smooth areas: the mean luma step across the edges between two one-colour blocks of ref that differ by less than 32
 * levels, in ref and in p. */
static void flat_steps(const uint32_t* ref, const uint32_t* p, int w, int h, double* before, double* after) {
    double sb = 0.0, sa = 0.0;
    long n = 0;
    for (int by = 0; by + 4 <= h; by += 4)
        for (int bx = 0; bx + 4 <= w; bx += 4)
            for (int axis = 0; axis < 2; axis++) {
                int px = axis ? bx : bx - 4, py = axis ? by - 4 : by;
                double step;
                if (px < 0 || py < 0 || !flat_block(ref, w, bx, by) || !flat_block(ref, w, px, py)) continue;
                step = fabs(luma(ref[by * w + bx]) - luma(ref[py * w + px]));
                if (step == 0.0 || step >= 32.0) continue;
                for (int k = 0; k < 4; k++) {
                    int x = axis ? bx + k : bx, y = axis ? by : by + k;
                    int x0 = axis ? x : x - 1, y0 = axis ? y - 1 : y;
                    sb += fabs(luma(ref[y * w + x]) - luma(ref[y0 * w + x0]));
                    sa += fabs(luma(p[y * w + x]) - luma(p[y0 * w + x0]));
                    n++;
                }
            }
    *before = n ? sb / n : 0.0;
    *after = n ? sa / n : 0.0;
}

/* Pixels brighter than their eight neighbours by 24 levels or more (stars, specks): how many of ref's are within two
 * levels in p. */
static void stars_kept(const uint32_t* ref, const uint32_t* p, int w, int h, int* total, int* kept) {
    *total = *kept = 0;
    for (int y = 1; y + 1 < h; y++)
        for (int x = 1; x + 1 < w; x++) {
            double c = luma(ref[y * w + x]), around = 0.0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx || dy) around = fmax(around, luma(ref[(y + dy) * w + x + dx]));
            if (c < around + 24.0) continue;
            (*total)++;
            if (fabs(luma(p[y * w + x]) - c) <= 2.0) (*kept)++;
        }
}

static void test_real_newgame(video_gpu_t* g) {
    decoded_t* d = &g_newgame[0];
    uint32_t top, *out, *deblocked;
    int black_before = 0, black_after = 0, black_stored = 0;
    double cross = 0.0, same = 0.0;
    if (!d->loaded) return;
    top = SCREEN_H / 2 - d->shown / 2;
    out = draw_decoded(g, d, 0, 0, 0, SCREEN_W, (int)d->shown);
    deblocked = draw_decoded(g, d, 1, 1, 1, SCREEN_W, (int)d->shown);
    if (!out || !deblocked) {
        CHECK(0, "newgame frame draws");
        free(out);
        free(deblocked);
        return;
    }
    for (uint32_t y = 0; y < d->shown; y++) {
        int was = 1, now = 1;
        for (int x = 0; x < SCREEN_W; x++) {
            if (d->screen[top + y][x]) was = 0;
            if (out[y * SCREEN_W + x] & 0xFFFFFFu) now = 0;
        }
        black_before += was;
        black_after += now;
        if (!(y & 1)) black_stored += was;
    }
    for (uint32_t y = 0; y + 2 < d->shown; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            cross += fabs(luma(out[y * SCREEN_W + x]) - luma(out[(y + 1) * SCREEN_W + x]));
            same += fabs(luma(out[y * SCREEN_W + x]) - luma(out[(y + 2) * SCREEN_W + x]));
        }
    CHECK(black_before >= 172 && black_after <= 2 * black_stored,
          "newgame frame 540: black rows %d decoded (%d of them stored black), %d presented", black_before,
          black_stored, black_after);
    CHECK(same > 0.0 && cross / same < 0.9, "newgame frame 540: no residual combing (adjacent/two-apart %.2f)",
          same > 0.0 ? cross / same : 0.0);
    free(deblocked);
    deblocked = draw_decoded(g, d, 0, 1, 1, SCREEN_W, (int)d->shown);
    CHECK(deblocked && !memcmp(out, deblocked, (size_t)SCREEN_W * d->shown * 4),
          "newgame frame 540: the row rebuild is bit-identical with deblocking and the stray-line rule asked for");
    printf("  newgame frame 540: %d black rows decoded -> %d after the row rebuild; adjacent/two-apart row "
           "difference %.2f; deblocking and the stray-line rule leave it bit-identical\n", black_before, black_after,
           same > 0.0 ? cross / same : 0.0);
    dump_decoded("newgame_540_decoded", d);
    dump_ppm("newgame_540_rebuilt", out, SCREEN_W, (int)d->shown);
    dump_hd("newgame_540", g, d, 0);
    free(out);
    free(deblocked);
}

typedef struct {
    double err565, errmap, checker[2], rows[2], kept, texture, change;
    double ratio_x[2], ratio_y[2], flat[2], kept_db, texture_db, change_db;
    int stars, stars_kept, text_change;
} frame_metrics_t;

/* plain: the palette's colours; smooth: the previous pipeline (dither smoothing); final: deblocking then smoothing.
 * Stars, edges and text are compared between smooth and final, without the last row (credits' stray line). */
static int measure(video_gpu_t* g, const decoded_t* r, int stray, frame_metrics_t* m) {
    uint32_t top = SCREEN_H / 2 - r->shown / 2;
    int w = SCREEN_W, h = (int)r->shown, ok;
    video_gpu_frame_t f = frame_of(&r->screen[top][0], SCREEN_W, SCREEN_W, vl_policy_for(r->flags, r->shown));
    uint32_t *quantised = draw(g, &f, w, h, w, h);
    uint32_t *plain = draw_decoded(g, r, 0, 0, 0, w, h), *smooth = draw_decoded(g, r, 1, 0, 0, w, h);
    uint32_t *final = draw_decoded(g, r, 1, 1, stray, w, h);
    double unused;
    memset(m, 0, sizeof *m);
    ok = quantised && plain && smooth && final;
    if (ok) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t want = g_map.map[r->screen[top + y][x]];
                if (!want) continue;
                for (int s = 0; s < 24; s += 8) {
                    m->err565 += abs((int)((quantised[y * w + x] >> s) & 255) - (int)((want >> s) & 255));
                    m->errmap += abs((int)((plain[y * w + x] >> s) & 255) - (int)((want >> s) & 255));
                }
            }
        m->err565 /= w * h * 3.0;
        m->errmap /= w * h * 3.0;
        m->checker[0] = checker_energy(plain, w, 0, 0, w, h);
        m->checker[1] = checker_energy(smooth, w, 0, 0, w, h);
        m->rows[0] = row_alternation(plain, w, 0, 0, w, h);
        m->rows[1] = row_alternation(smooth, w, 0, 0, w, h);
        edge_kept(plain, smooth, w, h, &m->kept, &m->texture, &m->change);
        m->ratio_x[0] = block_ratio(smooth, w, h - 1, 0);
        m->ratio_x[1] = block_ratio(final, w, h - 1, 0);
        m->ratio_y[0] = block_ratio(smooth, w, h - 1, 1);
        m->ratio_y[1] = block_ratio(final, w, h - 1, 1);
        flat_steps(plain, smooth, w, h - 1, &unused, &m->flat[0]);
        flat_steps(plain, final, w, h - 1, &unused, &m->flat[1]);
        edge_kept(smooth, final, w, h - 1, &m->kept_db, &m->texture_db, &m->change_db);
        stars_kept(smooth, final, w, h - 1, &m->stars, &m->stars_kept);
        for (int i = 0; i < w * (h - 1); i++)
            if (luma(smooth[i]) > 96.0 && max_change(&smooth[i], &final[i], 1) > m->text_change)
                m->text_change = max_change(&smooth[i], &final[i], 1);
    }
    free(quantised);
    free(plain);
    free(smooth);
    free(final);
    return ok;
}

static void test_real_frames(video_gpu_t* g) {
    double rx[2] = { 0.0, 0.0 }, ry[2] = { 0.0, 0.0 }, flat[2] = { 0.0, 0.0 }, kept = 1.0, texture = 1.0;
    int stars = 0, stars_kept = 0, intro_frames = 0;
    test_real_newgame(g);
    for (int i = 1; i < SCAN_COUNT; i++) {
        scan_t* v = &g_scans[i];
        int credits = !strcmp(v->file, "credits"), intro = !strcmp(v->file, "tpassintro");
        for (int k = 0; k < v->captures; k++) {
            decoded_t* r = &v->out[k];
            frame_metrics_t m;
            char name[64];
            if (!r->loaded) continue;
            if (!measure(g, r, credits, &m)) {
                CHECK(0, "%s %u draws", v->file, r->frame);
                continue;
            }
            printf("  %-10s %4u  palette error %.2f -> %.2f; dither checkerboard %.2f -> %.2f, row alternation %.2f -> "
                   "%.2f; block edge/inner x %.2f -> %.2f, y %.2f -> %.2f; smooth-area block step %.2f -> %.2f; strong "
                   "edges kept %.1f%%, texture kept %.1f%%, mean change %.2f; stars %d/%d\n",
                   v->file, r->frame, m.err565, m.errmap, m.checker[0], m.checker[1], m.rows[0], m.rows[1],
                   m.ratio_x[0], m.ratio_x[1], m.ratio_y[0], m.ratio_y[1], m.flat[0], m.flat[1], m.kept_db * 100.0,
                   m.texture_db * 100.0, m.change_db, m.stars_kept, m.stars);
            CHECK(m.errmap < m.err565 * 0.25 + 0.01, "%s %u: palette colours restored (error %.2f vs %.2f)", v->file,
                  r->frame, m.errmap, m.err565);
            CHECK(m.checker[1] <= m.checker[0] + 0.01 && m.rows[1] <= m.rows[0] + 0.01,
                  "%s %u: dither smoothing never adds a pattern", v->file, r->frame);
            CHECK(m.kept > 0.95 && m.texture > 0.8 && m.change < 2.0, "%s %u: dither smoothing keeps the picture",
                  v->file, r->frame);
            CHECK(m.ratio_x[1] <= m.ratio_x[0] + 0.01 && m.ratio_y[1] <= m.ratio_y[0] + 0.01,
                  "%s %u: deblocking never adds block edges (x %.2f -> %.2f, y %.2f -> %.2f)", v->file, r->frame,
                  m.ratio_x[0], m.ratio_x[1], m.ratio_y[0], m.ratio_y[1]);
            CHECK(m.kept_db > 0.97 && m.texture_db > 0.9 && m.change_db < 1.5,
                  "%s %u: deblocking keeps edges (%.3f) and texture (%.3f), mean change %.2f", v->file, r->frame,
                  m.kept_db, m.texture_db, m.change_db);
            CHECK(m.stars_kept == m.stars, "%s %u: every star and speck kept (%d of %d)", v->file, r->frame,
                  m.stars_kept, m.stars);
            if (credits)
                CHECK(m.text_change <= 2, "%s %u: its text unchanged (%d levels at most)", v->file, r->frame,
                      m.text_change);
            if (intro) {
                rx[0] += m.ratio_x[0], rx[1] += m.ratio_x[1], ry[0] += m.ratio_y[0], ry[1] += m.ratio_y[1];
                flat[0] += m.flat[0], flat[1] += m.flat[1];
                kept = fmin(kept, m.kept_db), texture = fmin(texture, m.texture_db);
                stars += m.stars, stars_kept += m.stars_kept;
                intro_frames++;
            }
            if (!strcmp(v->file, "win"))
                CHECK(m.checker[1] < m.checker[0] * 0.7 && m.rows[1] < m.rows[0] * 0.7,
                      "win: its ordered dither is reduced (%.2f -> %.2f, rows %.2f -> %.2f)", m.checker[0],
                      m.checker[1], m.rows[0], m.rows[1]);
            snprintf(name, sizeof name, "%s_%u_decoded", v->file, r->frame);
            dump_decoded(name, r);
            snprintf(name, sizeof name, "%s_%u", v->file, r->frame);
            dump_hd(name, g, r, credits);
        }
    }
    if (intro_frames) {
        double n = intro_frames;
        printf("  tpassintro, %d frames: block edge/inner x %.2f -> %.2f, y %.2f -> %.2f; smooth-area block step %.2f "
               "-> %.2f; strong edges kept >= %.1f%%, texture kept >= %.1f%%; stars %d/%d\n", intro_frames, rx[0] / n,
               rx[1] / n, ry[0] / n, ry[1] / n, flat[0] / n, flat[1] / n, kept * 100.0, texture * 100.0, stars_kept,
               stars);
        CHECK(rx[1] < rx[0] * 0.8 && ry[1] < ry[0] * 0.8, "tpassintro: block edges reduced (x %.2f -> %.2f, y %.2f -> "
              "%.2f)", rx[0] / n, rx[1] / n, ry[0] / n, ry[1] / n);
        CHECK(flat[1] < flat[0] * 0.5, "tpassintro: steps between smooth blocks halved at least (%.2f -> %.2f)",
              flat[0] / n, flat[1] / n);
    }
}

/* The stray line of credits' last row: in the decoded frame, drawn without the rule, gone with it, and no line in HD. */
static void test_credits_line(video_gpu_t* g) {
    for (int k = 0; k < 2; k++) {
        decoded_t* r = &g_credits[k];
        uint32_t top = SCREEN_H / 2 - r->shown / 2, *off, *on, *hd;
        int h = (int)r->shown, decoded_lit = 0, off_lit = 0, on_lit = 0, above_lit = 0, hd_lit = 0, last_text = -1;
        vl_layout_t l = vl_layout(1920, 1080, SCREEN_W, h);
        if (!r->loaded) continue;
        for (int x = 0; x < SCREEN_W; x++) decoded_lit += r->screen[top + h - 1][x] != 0;
        for (int y = 0; y < h - 4; y++)
            for (int x = 0; x < SCREEN_W; x++)
                if (r->screen[top + y][x]) last_text = y;
        off = draw_decoded(g, r, 1, 1, 0, SCREEN_W, h);
        on = draw_decoded(g, r, 1, 1, 1, SCREEN_W, h);
        hd = draw_decoded(g, r, 1, 1, 1, 1920, 1080);
        if (!off || !on || !hd) {
            CHECK(0, "credits %u draws", r->frame);
        } else {
            for (int x = 0; x < SCREEN_W; x++) {
                off_lit += luma(off[(h - 1) * SCREEN_W + x]) > 1.0;
                on_lit += luma(on[(h - 1) * SCREEN_W + x]) > 1.0;
                above_lit += luma(on[(h - 2) * SCREEN_W + x]) > 1.0;
            }
            for (int y = l.video.y + (last_text + 8) * l.video.h / h; y < 1080; y++)
                for (int x = 0; x < 1920; x++) hd_lit += luma(hd[(size_t)y * 1920 + x]) > 1.0;
            printf("  credits %4u: decoded row %d lit at %d pixels (last picture row above it: %d); drawn %d -> %d with "
                   "the rule; HD 1920x1080 below the last text row: %d lit pixels\n", r->frame, h - 1, decoded_lit,
                   last_text, off_lit, on_lit, hd_lit);
            CHECK(decoded_lit >= 300 && last_text < h - 40, "credits %u: the decoded frame carries the line (%d pixels) "
                  "far below its picture (row %d)", r->frame, decoded_lit, last_text);
            CHECK(off_lit >= 300, "credits %u: without the rule it is drawn (%d)", r->frame, off_lit);
            CHECK(on_lit == 0 && above_lit == 0, "credits %u: with it, no line (%d lit pixels)", r->frame, on_lit);
            CHECK(hd_lit == 0, "credits %u: nothing below the text in HD (%d lit pixels)", r->frame, hd_lit);
        }
        free(off);
        free(on);
        free(hd);
    }
}

/* WARP rasterises on the CPU: an upper bound of the passes' cost, with and without deblocking. */
static void time_passes(video_gpu_t* g) {
    LARGE_INTEGER freq, t0, t1;
    double ms[2];
    if (!g_intro[0].loaded) return;
    QueryPerformanceFrequency(&freq);
    for (int deblock = 0; deblock < 2; deblock++) {
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 4; i++) free(draw_decoded(g, &g_intro[0], 1, deblock, 0, 2560, 1440));
        QueryPerformanceCounter(&t1);
        ms[deblock] = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart / 4.0;
    }
    printf("  WARP (CPU) 2560x1440 frame incl. readback: %.1f ms without deblocking, %.1f ms with\n", ms[0], ms[1]);
}

/* The Video Test panel on WARP: it draws over the frame, and nothing is clicked without input. */
static void test_panel(void) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11RenderTargetView* rtv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    vt_panel_row_t rows[VT_VIDEO_COUNT];
    vt_panel_frame_t f;
    int video = -1, mode = VL_PLAY_AUTO, clicked = 0, lit = 0;
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    memset(&d, 0, sizeof d);
    d.Width = 1280;
    d.Height = 960;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)tex, NULL, &rtv))) {
        CHECK(0, "panel target");
        goto done;
    }
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &staging))) goto done;
    memset(rows, 0, sizeof rows);
    for (int i = 0; i < VT_VIDEO_COUNT; i++) {
        rows[i].file = vt_videos[i].file;
        strcpy(rows[i].size, "640x348");
        strcpy(rows[i].length, "40.4 s");
    }
    f.count = VT_VIDEO_COUNT;
    f.rows = rows;
    f.hd_available = 1;
    f.view_w = 960.0f;
    f.view_h = 720.0f;
    f.dpi_scale = 1.0f;
    for (int frame = 0; frame < 3; frame++) {
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, black);
        clicked |= vt_panel_draw(g_dev, g_ctx, rtv, 1280, 960, &f, &video, &mode);
    }
    CHECK(!clicked && video == -1, "no button fires without a click");
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)staging, (ID3D11Resource*)tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < 960; y += 4)
            for (int x = 0; x < 1280; x += 4)
                if (((const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x] & 0xFFFFFFu) lit++;
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)staging, 0);
    }
    CHECK(lit > 500, "the panel is drawn over the frame, scaled to the back buffer (%d samples)", lit);
    vt_panel_release();
    vt_panel_mouse_move(10.0f, 10.0f);
    vt_panel_mouse_button(0, 1);
    CHECK(1, "input without a panel is ignored");
done:
    if (staging) ID3D11Texture2D_Release(staging);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (tex) ID3D11Texture2D_Release(tex);
}

static void test_gpu(void) {
    video_gpu_t* g = NULL;
    ULONG refs_before, refs_after;
    if (!warp_open()) {
        printf("video_tests: no WARP device, GPU passes skipped\n");
        return;
    }
    ID3D11Device_AddRef(g_dev);
    refs_before = ID3D11Device_Release(g_dev);
    {
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        CHECK(video_gpu_create(g_dev, g_ctx, &g) && g, "the passes compile and their objects are made (%s)",
              video_gpu_error());
        QueryPerformanceCounter(&t1);
        printf("  passes compiled and created in %.1f ms\n", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
    }
    if (g) {
        test_gpu_identity(g);
        test_gpu_palette(g);
        test_gpu_rebuild(g, 0);
        test_gpu_rebuild(g, 1);
        test_gpu_diagonal(g);
        test_gpu_upscale(g);
        test_gpu_shapes(g);
        test_gpu_determinism(g);
        test_gpu_refusals(g);
        test_gpu_deblock_ramp(g);
        test_gpu_deblock_keeps(g);
        test_gpu_stray_line(g);
        test_real_frames(g);
        test_credits_line(g);
        time_passes(g);
        video_gpu_destroy(g);
    }
    CHECK(video_gpu_live_objects() == 0, "every object is released (%ld left)", video_gpu_live_objects());
    ID3D11DeviceContext_ClearState(g_ctx);
    ID3D11DeviceContext_Flush(g_ctx);
    ID3D11Device_AddRef(g_dev);
    refs_after = ID3D11Device_Release(g_dev);
    CHECK(refs_after == refs_before, "the device holds no reference for the passes (%lu vs %lu)", refs_after, refs_before);
    for (int round = 0; round < 3; round++) {
        CHECK(video_gpu_create(g_dev, g_ctx, &g), "made again (%d)", round);
        video_gpu_destroy(g);
    }
    CHECK(video_gpu_live_objects() == 0, "create and destroy leave nothing (%ld)", video_gpu_live_objects());
    test_panel();
    ID3D11DeviceContext_Release(g_ctx);
    ID3D11Device_Release(g_dev);
}

int main(int argc, char** argv) {
    test_profile_activate();
    const char* root = JPT_SOURCE_DIR;
    char path[MAX_PATH];
    HMODULE dll;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--dump")) g_dump = argv[i + 1];
    test_header_parse();
    test_policy();
    test_layout();
    test_output_size();
    test_names();
    test_palette_map();
    test_session();
    test_lone_pixels();
    test_video_test_state();
    printf("retail videos:\n");
    test_inventory(root);
    snprintf(path, sizeof path, "%s/game/SMACKW32.DLL", root);
    dll = LoadLibraryA(path);
    if (dll) {
        test_decoder(root, dll);
    } else {
        printf("video_tests: %s not installed, decoder checks skipped\n", path);
    }
    test_gpu();
    printf("video_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
