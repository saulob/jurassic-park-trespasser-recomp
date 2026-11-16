/*
 * Video subtitles: the timelines, the cue for a frame, the retail fingerprints, every language's text, the setting,
 * the layout over the picture at the usual aspect ratios, and the wiring into the players and renderers.
 * Headless: ninja -C <build dir> video_subtitle_tests && video_subtitle_tests.exe
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_config.h"
#include "jpt_hash.h"
#include "jpt_tr.h"
#include "jpt_video_subtitles.h"
#include "video_layout.h"

static int g_checks, g_failures;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static const char* const k_languages[] = { "en", "pt-BR", "fr", "de", "es", "ru", "it" };

static char* read_file(const char* relative, size_t* size) {
    char path[MAX_PATH * 2];
    FILE* f;
    char* data;
    long n;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) data[n] = 0;
    if (size) *size = (size_t)n;
    return data;
}

static const jvs_timeline_t* named(const char* name) {
    const jvs_timeline_t* t = jvs_timeline_named(name);
    CHECK(t != NULL, "%s has a timeline", name);
    return t;
}

static void test_timelines(void) {
    const jvs_timeline_t *intro = named("tpassintro.smk"), *newgame = named("newgame.smk"), *win = named("win.smk");
    CHECK(jvs_timeline_count == 3, "three videos have dialogue");
    CHECK(intro && intro->cue_count == 16, "tpassintro: 16 cues");
    CHECK(newgame && newgame->cue_count == 9, "newgame: 9 cues");
    CHECK(win && win->cue_count == 7, "win: 7 cues");
    CHECK(jvs_timeline_named("TPASSINTRO.SMK") == intro, "names are case-insensitive");
    CHECK(!jvs_timeline_named("credits.smk"), "credits.smk is music only: no timeline");
    CHECK(!jvs_timeline_named("mymod.smk") && !jvs_timeline_named("") && !jvs_timeline_named(NULL), "unknown names");
    CHECK(!jvs_timeline_named("tpassintro"), "the name with its extension");
    for (int i = 0; i < jvs_timeline_count; i++) {
        const jvs_timeline_t* t = &jvs_timelines[i];
        CHECK(strlen(t->sha256) == 64, "%s: SHA-256 hex", t->name);
        for (int k = 0; k < t->cue_count; k++) {
            const jvs_cue_t* c = &t->cues[k];
            CHECK(c->first <= c->last && c->last < t->frames, "%s cue %d inside the video", t->name, k);
            CHECK(k == 0 || c->first > t->cues[k - 1].last, "%s cue %d after the previous one", t->name, k);
            CHECK(!strncmp(c->key, "Video::", 7), "%s cue %d key", t->name, k);
        }
    }
}

static void test_selection(void) {
    const jvs_timeline_t *t = NULL, *intro = jvs_timeline_named("tpassintro.smk");
    jvs_session_t session;
    if (!intro) return;
    CHECK(jvs_select(1, "tpassintro.smk", intro->size, intro->sha256, &t) == JVS_SELECTED && t == intro, "On: the retail intro has its timeline");
    CHECK(jvs_select(0, "tpassintro.smk", intro->size, intro->sha256, &t) == JVS_DISABLED && !t, "Off: none");
    CHECK(jvs_select(1, "credits.smk", 8243368u, "00", &t) == JVS_NO_DIALOGUE && !t, "credits: no cues");
    CHECK(jvs_select(1, "newgame.smk", 123u, "00", &t) == JVS_NOT_RETAIL && !t, "a modified video: none");
    CHECK(jvs_select(1, "mymod.smk", 1u, "00", &t) == JVS_NO_DIALOGUE, "an unknown name: none");
    jvs_session_begin(&session, intro);
    CHECK(jvs_session_cue(&session, 10) == 0, "a cue shows");
    jvs_session_end(&session);
    CHECK(jvs_session_cue(&session, 10) == -1, "a skipped or ended video leaves none");
}

static void test_cue_at(void) {
    const jvs_timeline_t* t = jvs_timeline_named("newgame.smk");
    if (!t) return;
    CHECK(jvs_cue_at(t, -1) == -1, "negative frame");
    CHECK(jvs_cue_at(t, 0) == -1 && jvs_cue_at(t, 367) == -1, "before the first cue");
    CHECK(jvs_cue_at(t, 368) == 0, "exactly at the start");
    CHECK(jvs_cue_at(t, 380) == 0, "during");
    CHECK(jvs_cue_at(t, 399) == 0, "exactly at the end");
    CHECK(jvs_cue_at(t, 400) == 1, "the next cue on the following frame");
    CHECK(jvs_cue_at(t, 434) == -1, "the gap between cues");
    CHECK(jvs_cue_at(t, 435) == 2, "after the gap");
    CHECK(jvs_cue_at(t, 730) == 8 && jvs_cue_at(t, 731) == -1, "the last cue and after it");
    CHECK(jvs_cue_at(t, 1084) == -1 && jvs_cue_at(t, 5000) == -1, "the video's end");
    CHECK(jvs_cue_at(NULL, 380) == -1, "no timeline");
    CHECK(jvs_cue_at(jvs_timeline_named("tpassintro.smk"), 0) == 0, "the intro speaks from frame 0");
    CHECK((int)(jvs_frame_ms(368, 66.66) + 0.5) == 24531, "frame 368 at 66.66 ms per frame");
}

static void test_session(void) {
    const jvs_timeline_t* t = jvs_timeline_named("win.smk");
    jvs_session_t s;
    jvs_session_begin(&s, t);
    CHECK(jvs_session_cue(&s, 0) == -1, "silence first");
    CHECK(jvs_session_cue(&s, 278) == 0 && jvs_session_cue(&s, 278) == 0, "the same frame presented twice");
    CHECK(jvs_session_cue(&s, 300) == 0, "a paused video keeps its frame and its cue");
    CHECK(jvs_session_cue(&s, 550) == 6, "the last cue");
    CHECK(jvs_session_cue(&s, 551) == -1, "after the last cue");
    CHECK(jvs_session_cue(&s, 0) == -1 && s.ended, "the decoder wrapped after the last frame: ended");
    CHECK(jvs_session_cue(&s, 300) == -1, "nothing after the end");
    jvs_session_end(&s);
    CHECK(jvs_session_cue(&s, 300) == -1, "video ended");
    jvs_session_begin(&s, t);
    CHECK(jvs_session_cue(&s, 300) == 0, "a new playback starts again");
    jvs_session_end(&s);
    CHECK(jvs_session_cue(&s, 320) == -1, "skipped or failed playback: no cue later");
    jvs_session_begin(&s, NULL);
    CHECK(!s.playing && jvs_session_cue(&s, 300) == -1, "no timeline, no cue");
}

static void test_fingerprints(void) {
    const jvs_timeline_t* t = jvs_timeline_named("tpassintro.smk");
    static const char* const dubbed[][2] = {
        { "tpassintro.smk", "1a013e0994f1e6f5" }, { "newgame.smk", "3c13a9c2e9495a55" }, { "win.smk", "23075ee5cc85eaa4" },
    };
    char upper[65];
    if (!t) return;
    for (int i = 0; i < 64; i++) upper[i] = (char)(t->sha256[i] >= 'a' ? t->sha256[i] - 32 : t->sha256[i]);
    upper[64] = 0;
    CHECK(jvs_fingerprint_matches(t, t->size, t->sha256), "the retail file");
    CHECK(jvs_fingerprint_matches(t, t->size, upper), "hex case");
    CHECK(!jvs_fingerprint_matches(t, t->size + 1, t->sha256), "another size");
    CHECK(!jvs_fingerprint_matches(t, t->size, "00"), "another head");
    CHECK(!jvs_fingerprint_matches(NULL, t->size, t->sha256) && !jvs_fingerprint_matches(t, t->size, NULL), "missing");
    for (int i = 0; i < 3; i++) {
        const jvs_timeline_t* r = jvs_timeline_named(dubbed[i][0]);
        CHECK(r && strncmp(r->sha256, dubbed[i][1], 16), "the dubbed %s of the official translations is not the retail one",
              dubbed[i][0]);
    }
}

/* With game/data present, the timelines match the player's files and the SMK headers. */
static void test_retail_files(void) {
    int found = 0;
    for (int i = 0; i < jvs_timeline_count; i++) {
        const jvs_timeline_t* t = &jvs_timelines[i];
        char relative[128], sha[65];
        size_t size = 0;
        char* data;
        vl_smk_info_t info;
        snprintf(relative, sizeof relative, "game/data/menu/%s", t->name);
        data = read_file(relative, &size);
        if (!data) continue;
        found++;
        jpt_sha256_buffer_hex(data, size < JVS_FINGERPRINT_BYTES ? size : JVS_FINGERPRINT_BYTES, sha);
        CHECK(jvs_fingerprint_matches(t, size, sha), "%s: retail fingerprint", t->name);
        CHECK(vl_smk_parse((const uint8_t*)data, (uint32_t)size, &info) && info.frames == t->frames,
              "%s: %u frames", t->name, t->frames);
        CHECK(info.frame_rate == -6666, "%s: 15 fps", t->name);
        free(data);
    }
    if (!found) printf("note: game/data/menu not present, the retail files were not compared\n");
}

static void test_languages(void) {
    for (int l = 0; l < 7; l++) {
        size_t n = 0;
        char path[96];
        char* data;
        int missing = 0, english = 0;
        snprintf(path, sizeof path, "assets/translations/%s.interface.txt", k_languages[l]);
        data = read_file(path, &n);
        CHECK(data != NULL, "%s", path);
        if (!data) continue;
        jpt_tr_reset();
        CHECK(jpt_tr_load(data, n, 0) > 0, "%s loads", path);
        for (int i = 0; i < jvs_timeline_count; i++)
            for (int k = 0; k < jvs_timelines[i].cue_count; k++) {
                const char* key = jvs_timelines[i].cues[k].key;
                const char* text = jpt_tr(key);
                if (!jpt_tr_has(key)) {
                    missing++;
                    printf("  %s lacks %s\n", k_languages[l], key);
                }
                if (l == 0) CHECK(!strcmp(text, jpt_tr_english(key)), "en: %s", key);
                else if (!strcmp(text, jpt_tr_english(key))) english++;
                CHECK(text[0] && !strstr(text, "Video::"), "%s: %s has text", k_languages[l], key);
            }
        CHECK(!missing, "%s: every cue translated", k_languages[l]);
        CHECK(!english, "%s: no cue left in English (%d)", k_languages[l], english);
        free(data);
    }
    {
        size_t n = 0;
        char* it = read_file("assets/translations/it.interface.txt", &n);
        jpt_tr_reset();
        if (it) jpt_tr_load(it, n, 0);
        CHECK(!strcmp(jpt_tr("Video::Anne? Anne, are you there?"), "Anne? Anne, ci sei?"), "Italian: the same cue");
        CHECK(!strcmp(jpt_tr("Video::How it began..."), "Come iniziò?"), "Italian: the video's own wording");
        CHECK(!strcmp(jpt_tr("Video::A line nobody wrote."), "A line nobody wrote."), "unknown key: its English text");
        free(it);
    }
    {
        size_t n = 0;
        char* ru = read_file("assets/translations/ru.interface.txt", &n);
        jpt_tr_reset();
        if (ru) jpt_tr_load(ru, n, 1251);
        CHECK(!strcmp(jpt_tr("Video::It's Jill, pick up the phone!"), "Это Джилл, возьми трубку!"), "Russian UTF-8");
        CHECK((unsigned char)jpt_tr_cp("Video::It's Jill, pick up the phone!")[0] == 0xDD, "Russian in code page 1251");
        free(ru);
    }
    {
        size_t n = 0;
        char* pt = read_file("assets/translations/pt-BR.interface.txt", &n);
        jpt_tr_reset();
        if (pt) jpt_tr_load(pt, n, 0);
        CHECK(!strcmp(jpt_tr("Video::Site B was not to be a theme park, but a research station."),
                      "O Site B não seria um parque temático, mas uma estação de pesquisa."),
              "PT-BR reuses the caption translation of the same line");
        free(pt);
    }
    jpt_tr_reset();
    CHECK(!strcmp(jpt_tr("Video::Anne? Anne, are you there?"), "Anne? Anne, are you there?"), "reset: English");
}

static void test_setting(void) {
    jpt_config_t c;
    char path[MAX_PATH], dir[MAX_PATH];
    FILE* f;
    jpt_config_defaults(&c);
    CHECK(c.video_subtitles == 1, "On by default");
    CHECK(!strcmp(jpt_default_value(JPT_KEY_VIDEO_SUBTITLES), "1"), "Reset all settings gives On");
    CHECK(jpt_config_apply(&c, JPT_KEY_VIDEO_SUBTITLES, "off") && !c.video_subtitles, "explicit Off");
    CHECK(jpt_config_apply(&c, JPT_KEY_VIDEO_SUBTITLES, "1") && c.video_subtitles, "explicit On");
    CHECK(!jpt_config_apply(&c, JPT_KEY_VIDEO_SUBTITLES, "maybe") && c.video_subtitles, "invalid value ignored");
    GetTempPathA(sizeof dir, dir);
    snprintf(path, sizeof path, "%sjpt_video_subtitles_%lu.ini", dir, GetCurrentProcessId());
    if ((f = fopen(path, "w"))) {
        fputs("settings_version = 3\nskip_intro = 1\n", f);
        fclose(f);
    }
    CHECK(jpt_config_load(path, &c) && c.video_subtitles && c.skip_intro, "an older file without the key gets On");
    if ((f = fopen(path, "w"))) {
        fputs("settings_version = 3\nvideo_subtitles = 0\n", f);
        fclose(f);
    }
    CHECK(jpt_config_load(path, &c) && !c.video_subtitles, "a saved Off is kept");
    if ((f = fopen(path, "w"))) {
        fputs("video_subtitles = 1\n", f);
        fclose(f);
    }
    CHECK(jpt_config_load(path, &c) && c.video_subtitles, "a saved On is kept");
    DeleteFileA(path);
}

static int fixed_width(void* ctx, const char* utf8, int bytes) {
    int chars = 0;
    (void)ctx;
    for (int i = 0; i < bytes; i++) chars += ((unsigned char)utf8[i] & 0xC0) != 0x80;
    return chars * 10;
}

static void test_wrap(void) {
    jvs_line_t lines[JVS_MAX_LINES];
    const char* longest = "It was an unprecedented accomplishment, the pinnacle of 20th century science,";
    int n = jvs_wrap("Come iniziò?", 500, fixed_width, NULL, lines);
    CHECK(n == 1 && lines[0].start == 0 && lines[0].length == (int)strlen("Come iniziò?") && lines[0].width == 120,
          "one line when it fits (UTF-8 counted by character)");
    n = jvs_wrap(longest, 500, fixed_width, NULL, lines);
    CHECK(n == 2, "two lines when one does not fit");
    if (n == 2) {
        int a = lines[0].width, b = lines[1].width;
        CHECK(a <= 500 && b <= 500 && abs(a - b) <= 100, "balanced lines (%d, %d)", a, b);
        CHECK(longest[lines[0].start + lines[0].length] == ' ' && longest[lines[1].start] != ' ', "split at a space");
    }
    n = jvs_wrap("Mon Dieu, ne me dis pas que ta mère était sérieuse\xC2\xA0? Tu es au Costa Rica\xC2\xA0?", 280,
                 fixed_width, NULL, lines);
    for (int i = 0; i < n; i++) {
        const char* text = "Mon Dieu, ne me dis pas que ta mère était sérieuse\xC2\xA0? Tu es au Costa Rica\xC2\xA0?";
        CHECK(text[lines[i].start] != '?', "a French question mark stays with its word");
        CHECK(lines[i].width <= 280, "line %d fits", i);
    }
    CHECK(n >= 3 && n <= JVS_MAX_LINES, "more lines only when two do not fit (%d)", n);
    n = jvs_wrap("Supercalifragilistic", 50, fixed_width, NULL, lines);
    CHECK(n == 1, "a single word never breaks inside");
    CHECK(jvs_wrap("", 500, fixed_width, NULL, lines) == 0, "empty");
}

typedef struct {
    const char* name;
    int w, h;
} target_t;

static void test_layout(void) {
    static const target_t targets[] = {
        { "4:3", 1440, 1080 }, { "16:9", 1920, 1080 }, { "16:10", 1920, 1200 }, { "21:9", 3440, 1440 },
        { "32:9", 5120, 1440 }, { "4K", 3840, 2160 }, { "software 640x480", 640, 480 },
    };
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        const target_t* t = &targets[i];
        vl_layout_t l = vl_layout(t->w, t->h, 640, 348);
        jvs_rect_t picture = { l.video.x, l.video.y, l.video.w, l.video.h }, at;
        int px = jvs_font_px(&picture), max_w = jvs_max_line_width(&picture, t->w);
        int block_w = max_w, block_h = 2 * px + 2 * jvs_outline_px(px) + 4;
        int mx = t->w * 4 / 100, my = t->h * 4 / 100;
        if (strstr(t->name, "software")) {
            picture.x = 0;
            picture.y = 66;
            picture.w = 640;
            picture.h = 348;
            px = jvs_font_px(&picture);
            max_w = jvs_max_line_width(&picture, t->w);
            block_w = max_w;
            block_h = 2 * px + 2 * jvs_outline_px(px) + 4;
        }
        at = jvs_place(t->w, t->h, &picture, block_w, block_h);
        CHECK(px >= 12 && px <= picture.h / 10, "%s: font %d px for a %d px picture", t->name, px, picture.h);
        CHECK(max_w <= picture.w && max_w <= t->w - 2 * mx, "%s: lines narrower than the picture and the safe area",
              t->name);
        CHECK(at.x >= mx && at.x + at.w <= t->w - mx, "%s: inside the safe area horizontally", t->name);
        CHECK(at.y >= my && at.y + at.h <= t->h - my, "%s: inside the safe area vertically", t->name);
        CHECK(at.x >= picture.x && at.x + at.w <= picture.x + picture.w, "%s: over the picture, not the bars", t->name);
        CHECK(abs((at.x + at.w / 2) - (picture.x + picture.w / 2)) <= 1, "%s: centred on the picture", t->name);
        CHECK(at.y + at.h <= picture.y + picture.h && at.y > picture.y + picture.h / 2, "%s: in the picture's lower half",
              t->name);
    }
    {
        jvs_rect_t picture = { 100, 0, 640, 348 };
        jvs_rect_t at = jvs_place(800, 348, &picture, 200, 40);
        CHECK(at.y + at.h <= 348 - 348 * 4 / 100, "a picture filling the target keeps the bottom margin");
        CHECK(at.x == 100 + 220, "centred on a picture that is off-centre");
    }
    {
        jvs_rect_t tiny = { 0, 0, 320, 100 };
        CHECK(jvs_font_px(&tiny) == 12, "a minimum size");
        CHECK(jvs_outline_px(12) == 1 && jvs_outline_px(104) == 7, "outline follows the size");
    }
}

static void test_blend(void) {
    uint32_t target[4] = { 0xFF000000u, 0xFF808080u, 0xFF0000FFu, 0xFFFFFFFFu };
    const uint32_t image[2] = { 0xFFFFFFFFu, 0x80000000u };
    jvs_blend(target, 2, 2, 2, image, 2, 1, 0, 0);
    CHECK(target[0] == 0xFFFFFFFFu, "opaque white over black");
    CHECK(target[1] == 0xFF404040u, "half-opaque black over grey (0x%08X)", target[1]);
    CHECK(target[2] == 0xFF0000FFu && target[3] == 0xFFFFFFFFu, "rows below untouched");
    jvs_blend(target, 2, 2, 2, image, 2, 1, 1, 1);
    CHECK(target[3] == 0xFFFFFFFFu && target[2] == 0xFF0000FFu, "clipped at the right edge");
    jvs_blend(target, 2, 2, 2, image, 2, 1, -5, -5);
    CHECK(target[0] == 0xFFFFFFFFu, "fully outside: nothing");
}

static int contains(const char* relative, const char* needle) {
    char* text = read_file(relative, NULL);
    int found = text && strstr(text, needle) != NULL;
    free(text);
    return found;
}

static int before(const char* relative, const char* first, const char* second) {
    char* text = read_file(relative, NULL);
    const char* a = text ? strstr(text, first) : NULL;
    const char* b = a ? strstr(a, second) : NULL;
    free(text);
    return a && b;
}

static void scan_tree(const char* relative, int* hits) {
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s/%s/*", JPT_SOURCE_DIR, relative);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char child[MAX_PATH];
        if (fd.cFileName[0] == '.') continue;
        snprintf(child, sizeof child, "%s/%s", relative, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(fd.cFileName, "gen")) scan_tree(child, hits);
        } else {
            const char* ext = strrchr(fd.cFileName, '.');
            if (ext && (!_stricmp(ext, ".smk") || !_stricmp(ext, ".mp4"))) {
                (*hits)++;
                printf("  a video in the sources: %s\n", child);
            }
            if (ext && (!_stricmp(ext, ".c") || !_stricmp(ext, ".h") || !_stricmp(ext, ".cpp") || !_stricmp(ext, ".txt") ||
                        !_stricmp(ext, ".cmake") || !_stricmp(ext, ".ps1"))) {
                char* text = read_file(child, NULL);
                if (text && (strstr(text, "Decomp_Downloads") || strstr(text, "original_cutscenes_upscaled"))) {
                    (*hits)++;
                    printf("  a local path in %s\n", child);
                }
                free(text);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void test_no_reference_material(void) {
    int hits = 0;
    scan_tree("src", &hits);
    scan_tree("assets", &hits);
    scan_tree("launcher", &hits);
    scan_tree("tools", &hits);
    CHECK(!hits, "no reference video or local reference path in the sources (%d)", hits);
    CHECK(!contains("CMakeLists.txt", "Decomp_Downloads"), "the build names no local reference folder");
}

static void test_wiring(void) {
    const char* video = "src/host/video_hd.c";
    CHECK(before(video, "skip_opening(intro);\n        return;", "video_subtitles_begin(guest_name);"),
          "Skip Intro returns before any subtitle session");
    CHECK(before(video, "video_subtitles_begin(guest_name);\n    V.play_lifted();\n    video_subtitles_end();",
                 "vl_session_end(&V.session);"),
          "the session spans the game's own Play and ends with it");
    CHECK(contains(video, "V.player = g_ecx;\n        video_subtitles_begin(guest_name);\n        V.play_lifted();\n"
                          "        video_subtitles_end();"),
          "the Video Test's ORIGINAL playback too");
    CHECK(before(video, "video_gpu_draw(V.gpu, &f, &layout", "d3dr_draw_video_subtitle(V.rtv, V.chain_w, V.chain_h"),
          "HD video surface: drawn after the frame");
    CHECK(before(video, "d3dr_draw_video_subtitle(V.rtv", "IDXGISwapChain1_Present(V.chain"),
          "HD video surface: drawn before its Present");
    CHECK(contains("src/host/ddraw/d3d11_renderer.c", "draw_subtitle(width, height, R.subtitle.pending);"),
          "Direct3D 11 with the game's own presentation");
    CHECK(before("src/host/ddraw/d3d11_renderer.c", "R.subtitle.pending = video_subtitles_for_screen(",
                 "R.subtitle.pending = NULL;"),
          "the pending image lives for one present only");
    CHECK(before("src/host/ddraw/d3d11_present.c", "video_subtitles_for_screen((int)width, (int)height",
                 "ID3D11DeviceContext_Unmap(s_p.context, (ID3D11Resource*)staging, 0);"),
          "Software: blended into the presented picture");
    CHECK(contains("src/host/main.c", "video_subtitles_configure(cfg.video_subtitles, game_dir);"), "the setting");
    CHECK(contains("launcher/launcher.cpp",
                   "ui.Toggle(\"Video subtitles\", kVideoSubtitlesHelp, JPT_KEY_VIDEO_SUBTITLES, true, \"Off\", \"On\")"),
          "Gameplay > Video subtitles, On");
    CHECK(contains("launcher/localization/pt_br_ui_text.cpp", "{\"Video subtitles\", \"Legendas dos vídeos\"}"),
          "PT-BR option name");
    CHECK(contains("launcher/localization/pt_br_ui_text.cpp", "Exibe legendas no idioma do jogo"), "PT-BR help");
}

int main(void) {
    test_timelines();
    test_cue_at();
    test_selection();
    test_session();
    test_fingerprints();
    test_retail_files();
    test_languages();
    test_setting();
    test_wrap();
    test_layout();
    test_blend();
    test_no_reference_material();
    test_wiring();
    printf("video_subtitle_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
