/*
 * Video subtitles: the timelines, the cue for a frame, the retail fingerprints, every language's text, the setting,
 * the layout over the picture at the usual aspect ratios, and the wiring into the players and renderers.
 * Headless: ninja -C <build dir> video_subtitle_tests && video_subtitle_tests.exe
 */
#include <windows.h>
#include "test_profile.h"

#include <math.h>
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

static uint64_t cue_hash(const jvs_timeline_t* t) {
    uint64_t h = 0xCBF29CE484222325ULL;
    char line[256];
    for (int k = 0; k < t->cue_count; k++) {
        int n = snprintf(line, sizeof line, "%u,%u,%s\n", t->cues[k].first, t->cues[k].last, t->cues[k].key);
        for (int i = 0; i < n; i++) h = (h ^ (uint8_t)line[i]) * 0x100000001B3ULL;
    }
    return h;
}

static void test_timelines(void) {
    const jvs_timeline_t *intro = named("tpassintro.smk"), *newgame = named("newgame.smk"), *win = named("win.smk");
    CHECK(jvs_timeline_count == 12, "three videos with dialogue, a timeline for each of the four releases");
    CHECK(intro && intro->cue_count == 16, "tpassintro: 16 cues");
    CHECK(newgame && newgame->cue_count == 9, "newgame: 9 cues");
    CHECK(win && win->cue_count == 7, "win: 7 cues");
    CHECK(intro && intro->frames == 1585 && newgame && newgame->frames == 1085 && win && win->frames == 606, "frame counts");
    CHECK(intro && cue_hash(intro) == 0xD1E323C897472FA1ULL, "tpassintro: the English cue table is unchanged");
    CHECK(newgame && cue_hash(newgame) == 0x0876A7365477C0E1ULL, "newgame: the English cue table is unchanged");
    CHECK(win && cue_hash(win) == 0xA47FF2A82FC36780ULL, "win: the English cue table is unchanged");
    CHECK(jvs_timeline_named("TPASSINTRO.SMK") == intro, "names are case-insensitive");
    CHECK(!jvs_timeline_named("credits.smk"), "credits.smk is music only: no timeline");
    CHECK(!jvs_timeline_named("mymod.smk") && !jvs_timeline_named("") && !jvs_timeline_named(NULL), "unknown names");
    CHECK(!jvs_timeline_named("tpassintro"), "the name with its extension");
    for (int i = 0; i < jvs_timeline_count; i++) {
        const jvs_timeline_t* t = &jvs_timelines[i];
        CHECK(t->fingerprint_count >= 1, "%s: at least one accepted file", t->name);
        for (int f = 0; f < t->fingerprint_count; f++) {
            const jvs_fingerprint_t* p = &t->fingerprints[f];
            CHECK(strlen(p->sha256) == 64 && p->size > JVS_FINGERPRINT_BYTES && p->release[0], "%s: fingerprint %d", t->name, f);
            for (int g = 0; g < f; g++)
                CHECK(p->size != t->fingerprints[g].size || strcmp(p->sha256, t->fingerprints[g].sha256),
                      "%s: fingerprint %d listed once", t->name, f);
        }
        for (int k = 0; k < t->cue_count; k++) {
            const jvs_cue_t* c = &t->cues[k];
            CHECK(c->first < c->last && c->last < t->frames, "%s %s cue %d inside the video", t->name, t->fingerprints[0].release, k);
            CHECK(k == 0 || c->first > t->cues[k - 1].last, "%s %s cue %d after the previous one", t->name, t->fingerprints[0].release, k);
            CHECK(!strncmp(c->key, "Video::", 7), "%s cue %d key", t->name, k);
        }
    }
}

static void test_selection(void) {
    const jvs_timeline_t *t = NULL, *intro = jvs_timeline_named("tpassintro.smk");
    jvs_session_t session;
    if (!intro) return;
    const jvs_fingerprint_t* p = &intro->fingerprints[0];
    CHECK(jvs_select(1, "tpassintro.smk", p->size, p->sha256, &t) == JVS_SELECTED && t == intro, "On: the retail intro has its timeline");
    CHECK(jvs_select(1, "TpassIntro.SMK", p->size, p->sha256, &t) == JVS_SELECTED && t == intro, "On: the name's case");
    CHECK(jvs_select(0, "tpassintro.smk", p->size, p->sha256, &t) == JVS_DISABLED && !t, "Off: none");
    CHECK(jvs_select(1, "tpassintro.smk", p->size, "00", &t) == JVS_NOT_RETAIL && !t, "the retail size with other bytes");
    CHECK(jvs_select(1, "tpassintro.smk", p->size + 1, p->sha256, &t) == JVS_NOT_RETAIL && !t, "the retail bytes with another size");
    CHECK(jvs_select(1, "tpassintro.smk", p->size, NULL, &t) == JVS_NOT_RETAIL && !t, "no hash");
    CHECK(jvs_select(1, "newgame.smk", p->size, p->sha256, &t) == JVS_NOT_RETAIL && !t, "another video's fingerprint under this name");
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

typedef struct {
    const char* release;
    const char* name;
    uint64_t size;
    const char* head_sha256;
    uint32_t frames;
    int supported; /* its speech windows are proven to be a timeline's */
} release_file_t;

/* tools/smk_inventory.py: the dialogue videos of the four installed releases. The Russian, Spanish and Brazilian
 * Portuguese files are dubbed: same picture and frame count as the English ones, other speech, a timeline of their own
 * (tools/smk_speech.py). */
static const release_file_t k_releases[] = {
    { "en-116", "tpassintro.smk", 29473692u, "8df7c0d6c0e809cf17c644b81c4e1ba40fde487227d35dfccb1e22323c1cc915", 1585u, 1 },
    { "en-116", "newgame.smk", 15502788u, "98a379a48847e603c54ea84ee95c872eafdadbbd49e9a4a8e7d75e6a218b7770", 1085u, 1 },
    { "en-116", "win.smk", 13227104u, "fbe95bb588dc4b639e4b7a3177b25623482dcbb1d6661db5780bf5346bc01ab9", 606u, 1 },
    { "ru-116", "tpassintro.smk", 27176048u, "e04b66166d33e113f71b2a71c25518a7e6b929c4a133bac14b82066e7df1d616", 1585u, 1 },
    { "ru-116", "newgame.smk", 13974292u, "d45e2314d561c18851bcd9e2c31df3a3c1ff4ec93fc86b57b7623c6cc6b32798", 1085u, 1 },
    { "ru-116", "win.smk", 12286452u, "c99528607bf25e2e2812c71ba2a857475a874003256c78c0a38a9acaf2eb18a8", 606u, 1 },
    { "es-117", "tpassintro.smk", 29403480u, "ca8d85c9aa12982193a0fecc76df64381e2124d383be074bb46f8760b0a80c2d", 1585u, 1 },
    { "es-117", "newgame.smk", 15510172u, "447bc9f34d11a785a850f7ebb5b4afe06f223decda2f6e6344114210a6dde92a", 1085u, 1 },
    { "es-117", "win.smk", 13247472u, "6a48b4c62a5a08fa6012d67e61effab53e941b8fbe3c03f04073744f4dc70449", 606u, 1 },
    { "ptbr-117", "tpassintro.smk", 29453916u, "1a013e0994f1e6f5b9d0ac755ce06329fb0d0b558d112ee0622823b607f575d7", 1585u, 1 },
    { "ptbr-117", "newgame.smk", 15496048u, "3c13a9c2e9495a55dbca840bd149cf051933417356f04093d282d261234787a0", 1085u, 1 },
    { "ptbr-117", "win.smk", 13274428u, "23075ee5cc85eaa46858aaa09cbee6bce2acac2db13d5ac73b63803832605e0e", 606u, 1 },
};
static const char* const k_release_ids[] = { "en-116", "ru-116", "es-117", "ptbr-117" };
#define RELEASE_COUNT ((int)(sizeof k_releases / sizeof k_releases[0]))

typedef struct {
    const char* release;
    const char* name;
    uint64_t hash;
} cue_pin_t;

/* A release's own timing, from tools/smk_speech.py: a change to a cue table is a change to this list. */
static const cue_pin_t k_cue_pins[] = {
    { "en-116", "tpassintro.smk", 0xD1E323C897472FA1ULL },
    { "ru-116", "tpassintro.smk", 0xEB14BC26EED16A89ULL },
    { "es-117", "tpassintro.smk", 0x9B9784BAD9218897ULL },
    { "ptbr-117", "tpassintro.smk", 0x96A33D7F2B5FE64DULL },
    { "en-116", "newgame.smk", 0x0876A7365477C0E1ULL },
    { "ru-116", "newgame.smk", 0xEADB41621EE79086ULL },
    { "es-117", "newgame.smk", 0xF49D3C46A0D05872ULL },
    { "ptbr-117", "newgame.smk", 0x6388F8146E08BE12ULL },
    { "en-116", "win.smk", 0xA47FF2A82FC36780ULL },
    { "ru-116", "win.smk", 0xBAE7A54530F07448ULL },
    { "es-117", "win.smk", 0x0EE7408AC867E3ACULL },
    { "ptbr-117", "win.smk", 0xD4EE52D013B0D678ULL },
};

static const jvs_timeline_t* release_timeline(const char* name, const char* release) {
    for (int i = 0; i < jvs_timeline_count; i++)
        if (!strcmp(jvs_timelines[i].name, name) && !strcmp(jvs_timelines[i].fingerprints[0].release, release)) return &jvs_timelines[i];
    return NULL;
}

/* Every dub has a timeline of its own, with the English cues' order and text and its own start and end frames. */
static void test_release_timelines(void) {
    static const struct {
        const char* name;
        int cues;
    } videos[] = { { "tpassintro.smk", 16 }, { "newgame.smk", 9 }, { "win.smk", 7 } };
    for (size_t v = 0; v < sizeof videos / sizeof videos[0]; v++) {
        const jvs_timeline_t* english = release_timeline(videos[v].name, "en-116");
        CHECK(english != NULL, "%s: the English timeline", videos[v].name);
        for (int r = 0; r < 4; r++) {
            const jvs_timeline_t* t = release_timeline(videos[v].name, k_release_ids[r]);
            int differs = 0, pinned = 0;
            CHECK(t != NULL && t->fingerprint_count == 1 && t->cue_count == videos[v].cues, "%s %s: %d cues", videos[v].name,
                  k_release_ids[r], videos[v].cues);
            if (!t || !english) continue;
            for (int k = 0; k < t->cue_count; k++) {
                const jvs_cue_t* c = &t->cues[k];
                differs |= c->first != english->cues[k].first || c->last != english->cues[k].last;
                CHECK(c->key == english->cues[k].key || !strcmp(c->key, english->cues[k].key), "%s %s cue %d: the same line as the English one",
                      videos[v].name, k_release_ids[r], k);
                CHECK(c->last - c->first + 1 >= 15, "%s %s cue %d: on screen long enough to read", videos[v].name, k_release_ids[r], k);
                CHECK(k == 0 || c->first == t->cues[k - 1].last + 1 || c->first > t->cues[k - 1].last + 1,
                      "%s %s cue %d: no overlap with the previous one", videos[v].name, k_release_ids[r], k);
            }
            CHECK(r == 0 || differs, "%s %s: its own timing, not the English one", videos[v].name, k_release_ids[r]);
            for (size_t p = 0; p < sizeof k_cue_pins / sizeof k_cue_pins[0]; p++)
                if (!strcmp(k_cue_pins[p].name, videos[v].name) && !strcmp(k_cue_pins[p].release, k_release_ids[r])) {
                    pinned = 1;
                    CHECK(cue_hash(t) == k_cue_pins[p].hash, "%s %s: the cue table is the analysed one", videos[v].name, k_release_ids[r]);
                }
            CHECK(pinned, "%s %s: pinned", videos[v].name, k_release_ids[r]);
            for (int o = 0; o < r; o++) {
                const jvs_timeline_t* other = release_timeline(videos[v].name, k_release_ids[o]);
                CHECK(!other || other == t || cue_hash(other) != cue_hash(t), "%s: %s and %s do not share a table by accident",
                      videos[v].name, k_release_ids[o], k_release_ids[r]);
            }
        }
    }
}

/* The cue shown on a frame comes from the file's own timeline; the words come from the chosen language alone. */
static void test_timing_variant_and_text_language(void) {
    static const char* const languages[] = { "en", "pt-BR", "es", "ru" };
    const release_file_t* ptbr_newgame = NULL;
    const jvs_timeline_t *selected = NULL, *english = release_timeline("newgame.smk", "en-116");
    for (int i = 0; i < RELEASE_COUNT; i++)
        if (!strcmp(k_releases[i].release, "ptbr-117") && !strcmp(k_releases[i].name, "newgame.smk")) ptbr_newgame = &k_releases[i];
    CHECK(ptbr_newgame && english, "the Brazilian newgame.smk and the English timeline are listed");
    if (!ptbr_newgame || !english) return;
    CHECK(jvs_select(1, "newgame.smk", ptbr_newgame->size, ptbr_newgame->head_sha256, &selected) == JVS_SELECTED && selected &&
              selected == release_timeline("newgame.smk", "ptbr-117"),
          "the Brazilian file selects the Brazilian timing");
    if (!selected) return;
    CHECK(jvs_cue_at(selected, 360) == 0 && jvs_cue_at(english, 360) == -1, "the Brazilian speech starts before the English cue");
    for (int l = 0; l < 4; l++) {
        size_t n = 0;
        char path[96];
        char* data;
        snprintf(path, sizeof path, "assets/translations/%s.interface.txt", languages[l]);
        data = read_file(path, &n);
        jpt_tr_reset();
        if (data) jpt_tr_load(data, n, !strcmp(languages[l], "ru") ? 1251 : 0);
        for (int k = 0; k < selected->cue_count; k++) {
            const char* shown = jpt_tr(selected->cues[k].key);
            CHECK(!strcmp(shown, jpt_tr(english->cues[k].key)), "%s: cue %d reads the same under either timing", languages[l], k);
            CHECK(l == 0 ? !strcmp(shown, jpt_tr_english(selected->cues[k].key)) : strcmp(shown, jpt_tr_english(selected->cues[k].key)) != 0,
                  "%s: cue %d text follows the language only", languages[l], k);
        }
        free(data);
    }
    jpt_tr_reset();
}

static const jvs_timeline_t* timeline_of(const release_file_t* r) {
    for (int i = 0; i < jvs_timeline_count; i++)
        if (!strcmp(jvs_timelines[i].name, r->name) && !strcmp(jvs_timelines[i].fingerprints[0].release, r->release))
            return &jvs_timelines[i];
    return NULL;
}

static void test_fingerprints(void) {
    const jvs_timeline_t* t = jvs_timeline_named("tpassintro.smk");
    const jvs_fingerprint_t* p;
    char upper[65];
    if (!t) return;
    p = &t->fingerprints[0];
    for (int i = 0; i < 64; i++) upper[i] = (char)(p->sha256[i] >= 'a' ? p->sha256[i] - 32 : p->sha256[i]);
    upper[64] = 0;
    CHECK(jvs_fingerprint_matches(t, p->size, p->sha256), "the retail file");
    CHECK(jvs_fingerprint_matches(t, p->size, upper), "hex case");
    CHECK(!jvs_fingerprint_matches(t, p->size + 1, p->sha256), "another size");
    CHECK(!jvs_fingerprint_matches(t, p->size, "00"), "another head");
    CHECK(!jvs_fingerprint_matches(NULL, p->size, p->sha256) && !jvs_fingerprint_matches(t, p->size, NULL), "missing");
    for (int i = 0; i < jvs_timeline_count; i++)
        for (int f = 0; f < jvs_timelines[i].fingerprint_count; f++) {
            const jvs_fingerprint_t* q = &jvs_timelines[i].fingerprints[f];
            int listed = 0;
            for (int r = 0; r < RELEASE_COUNT; r++)
                if (!strcmp(k_releases[r].name, jvs_timelines[i].name) && k_releases[r].size == q->size &&
                    !strcmp(k_releases[r].head_sha256, q->sha256) && !strcmp(k_releases[r].release, q->release))
                    listed = k_releases[r].supported;
            CHECK(listed, "%s: accepted file %s is a release file with proven timing", jvs_timelines[i].name, q->release);
        }
}

/* Every release's dialogue videos: accepted exactly when their timing is proven, never by name or size alone. */
static void test_releases(void) {
    const jvs_timeline_t* out = NULL;
    int accepted = 0;
    for (int i = 0; i < RELEASE_COUNT; i++) {
        const release_file_t* r = &k_releases[i];
        const jvs_timeline_t* named_t = jvs_timeline_named(r->name);
        jvs_choice_t c = jvs_select(1, r->name, r->size, r->head_sha256, &out);
        CHECK(named_t != NULL, "%s: %s has a timeline entry", r->release, r->name);
        CHECK(r->supported ? (c == JVS_SELECTED && out && out == timeline_of(r) && out->frames == r->frames)
                           : (c == JVS_NOT_RETAIL && !out),
              "%s %s: %s", r->release, r->name, r->supported ? "selected with its own timeline" : "refused");
        accepted += c == JVS_SELECTED;
        CHECK(jvs_select(1, r->name, r->size + 1, r->head_sha256, &out) == JVS_NOT_RETAIL && !out, "%s %s: a wrong size is refused",
              r->release, r->name);
        CHECK(jvs_select(1, r->name, r->size, "0000000000000000000000000000000000000000000000000000000000000000", &out) ==
                  JVS_NOT_RETAIL && !out,
              "%s %s: a wrong hash is refused", r->release, r->name);
        CHECK(jvs_select(0, r->name, r->size, r->head_sha256, &out) == JVS_DISABLED && !out, "%s %s: Off", r->release, r->name);
        /* the same name with another release's size and hash is that release's file: judged by its own fingerprint */
        for (int j = 0; j < RELEASE_COUNT; j++) {
            const release_file_t* o = &k_releases[j];
            if (j == i || strcmp(o->name, r->name)) continue;
            CHECK((jvs_select(1, r->name, o->size, o->head_sha256, &out) == JVS_SELECTED) == o->supported,
                  "%s: %s's file under the same name", r->name, o->release);
        }
    }
    CHECK(accepted == 12, "twelve accepted release files (%d)", accepted);
    CHECK(jvs_select(1, "credits.smk", 8243368u, "e66fdafb7547dde2bea8eec5e1b9808f56286f4963be26951bb58220f0fe756f", &out) ==
              JVS_NO_DIALOGUE && !out,
          "credits.smk (the same file in every release): no dialogue");
    CHECK(jvs_select(1, "newgame.smk", k_releases[1].size, "e04b66166d33e113f71b2a71c25518a7e6b929c4a133bac14b82066e7df1d616",
                     &out) == JVS_NOT_RETAIL,
          "a same-name file with a hash of another video is refused");
}

/* Several accepted files share one cue table; a file with its own timing has its own entry under the same name. */
static void test_shared_and_specific_tables(void) {
    static const jvs_cue_t cues_a[] = { { 10, 20, "Video::a" } };
    static const jvs_cue_t cues_b[] = { { 12, 24, "Video::a" } };
    static const jvs_fingerprint_t prints_a[] = { { "r1", 100000u, "aa" }, { "r2", 200000u, "bb" } };
    static const jvs_fingerprint_t prints_b[] = { { "r3", 300000u, "cc" } };
    static const jvs_timeline_t table[] = {
        { "x.smk", 100u, cues_a, 1, prints_a, 2 },
        { "x.smk", 100u, cues_b, 1, prints_b, 1 },
        { "y.smk", 100u, cues_a, 1, prints_a, 2 },
    };
    const jvs_timeline_t* t;
    CHECK((t = jvs_timeline_find(table, 3, "x.smk", 100000u, "aa")) == &table[0] && t->cues == cues_a, "first file of a shared table");
    CHECK((t = jvs_timeline_find(table, 3, "X.SMK", 200000u, "BB")) == &table[0] && t->cues == cues_a, "second file shares the same cue table");
    CHECK((t = jvs_timeline_find(table, 3, "x.smk", 300000u, "cc")) == &table[1] && t->cues == cues_b, "a file with its own timing");
    CHECK(!jvs_timeline_find(table, 3, "x.smk", 300000u, "aa") && !jvs_timeline_find(table, 3, "x.smk", 100000u, "cc"),
          "size and hash must belong to the same file");
    CHECK(!jvs_timeline_find(table, 3, "x.smk", 100001u, "aa") && !jvs_timeline_find(table, 3, "x.smk", 100000u, "ab"), "wrong size or hash");
    CHECK(!jvs_timeline_find(table, 3, "z.smk", 100000u, "aa") && !jvs_timeline_find(table, 3, NULL, 100000u, "aa"), "unknown name");
    CHECK(!jvs_timeline_find(table, 3, "x.smk", 100000u, NULL), "no hash");
    CHECK(jvs_timeline_find(table, 3, "y.smk", 200000u, "bb") == &table[2], "the name picks the entry too");
}

/* With the installed releases present (game/versions/<id>/data/menu), the constants match the real files. */
static void test_retail_files(void) {
    const jvs_timeline_t* out = NULL;
    int found = 0;
    for (int r = 0; r < RELEASE_COUNT; r++) {
        const release_file_t* f = &k_releases[r];
        char relative[160], sha[65];
        size_t size = 0;
        char* data;
        vl_smk_info_t info;
        const jvs_timeline_t* t = timeline_of(f);
        snprintf(relative, sizeof relative, "game/versions/%s/data/menu/%s", f->release, f->name);
        data = read_file(relative, &size);
        if (!data) continue;
        found++;
        jpt_sha256_buffer_hex(data, size < JVS_FINGERPRINT_BYTES ? size : JVS_FINGERPRINT_BYTES, sha);
        CHECK(size == f->size && !strcmp(sha, f->head_sha256), "%s %s: the fingerprint constants match the file", f->release, f->name);
        CHECK(jvs_fingerprint_matches(t, size, sha) == f->supported, "%s %s: accepted exactly when proven", f->release, f->name);
        CHECK(vl_smk_parse((const uint8_t*)data, (uint32_t)size, &info) && info.frames == f->frames && t && info.frames == t->frames,
              "%s %s: %u frames", f->release, f->name, f->frames);
        CHECK(info.frame_rate == -6666, "%s %s: 15 fps", f->release, f->name);
        free(data);
    }
    for (int v = 0; v < 4; v++) {
        char relative[160], sha[65];
        size_t size = 0;
        char* data;
        snprintf(relative, sizeof relative, "game/versions/%s/data/menu/credits.smk", k_release_ids[v]);
        data = read_file(relative, &size);
        if (!data) continue;
        jpt_sha256_buffer_hex(data, JVS_FINGERPRINT_BYTES, sha);
        CHECK(size == 8243368u && !strcmp(sha, "e66fdafb7547dde2bea8eec5e1b9808f56286f4963be26951bb58220f0fe756f"),
              "%s: credits.smk is the same file in every release", k_release_ids[v]);
        CHECK(jvs_select(1, "credits.smk", size, sha, &out) == JVS_NO_DIALOGUE && !out, "%s: credits.smk has no timeline",
              k_release_ids[v]);
        free(data);
    }
    if (!found) printf("note: game/versions/<id>/data/menu not present, the release files were not compared\n");
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

static int proportional_width(void* ctx, const char* utf8, int bytes) {
    (void)utf8;
    return bytes * *(int*)ctx * 11 / 20;
}

static void test_target_spaces(void) {
    static const target_t targets[] = {
        { "960x720 windowed", 960, 720 }, { "1920x1080 fullscreen", 1920, 1080 }, { "2560x1440 fullscreen", 2560, 1440 },
        { "1440x1080 letterboxed surface", 1440, 1080 }, { "3440x1440 ultrawide", 3440, 1440 },
    };
    const char* cue = "My name is John Parker Hammond. I was born on March 14, 1928.";
    int first_lines = 0;
    double first_scale = 0.0, first_margin = 0.0;
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        const target_t* t = &targets[i];
        vl_layout_t l = vl_layout(t->w, t->h, 640, 348);
        jvs_rect_t picture = { l.video.x, l.video.y, l.video.w, l.video.h }, at;
        jvs_line_t lines[JVS_MAX_LINES];
        int px = jvs_font_px(&picture), r = jvs_outline_px(px), count, block_w = 0, block_h, mx = t->w * 4 / 100;
        count = jvs_wrap(cue, jvs_max_line_width(&picture, t->w) - 2 * r, proportional_width, &px, lines);
        for (int k = 0; k < count; k++)
            if (lines[k].width > block_w) block_w = lines[k].width;
        block_w += 2 * (r + 2);
        block_h = count * px + 2 * (r + 2);
        at = jvs_place(t->w, t->h, &picture, block_w, block_h);
        CHECK(at.x >= mx && at.x + at.w <= t->w - mx && at.y >= 0 && at.y + at.h <= t->h,
              "%s: the cue lies inside its own target (%d..%d of %d)", t->name, at.x, at.x + at.w, t->w);
        CHECK(at.x >= picture.x && at.x + at.w <= picture.x + picture.w && at.y + at.h <= picture.y + picture.h,
              "%s: the cue lies inside the picture", t->name);
        {
            double scale = (double)px / picture.h, margin = (double)(picture.y + picture.h - at.y - at.h) / picture.h;
            if (!i) {
                first_lines = count;
                first_scale = scale;
                first_margin = margin;
            }
            CHECK(count == first_lines, "%s: %d lines like the windowed layout (%d)", t->name, count, first_lines);
            CHECK(fabs(scale - first_scale) < 0.004, "%s: the font keeps its share of the picture height", t->name);
            CHECK(fabs(margin - first_margin) < 0.02, "%s: the cue keeps its distance from the picture's bottom", t->name);
        }
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
    CHECK(before(video, "if (skip_intro_try(guest_name, mode != VL_PLAY_AUTO, V.play_lifted)) return;", "video_subtitles_begin(guest_name);"),
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
    CHECK(contains(video, "|| !vulkan_target_client(window, &rc))") && contains(video, "if (!vulkan_target_client(window, &rc)) {")
              && contains(video, "window_host_surface(window);"),
          "Vulkan: the picture and the subtitle are laid out in the surface the swapchain presents, not the game window");
    CHECK(contains("src/host/main.c", "video_subtitles_configure(cfg.video_subtitles, game_dir);"), "the setting");
    CHECK(contains("launcher/launcher.cpp",
                   "ui.Toggle(\"Video subtitles\", kVideoSubtitlesHelp, JPT_KEY_VIDEO_SUBTITLES, true, \"Off\", \"On\")"),
          "Gameplay > Video subtitles, On");
    CHECK(contains("launcher/localization/pt_br_ui_text.cpp", "{\"Video subtitles\", \"Legendas dos vídeos\"}"),
          "PT-BR option name");
    CHECK(contains("launcher/localization/pt_br_ui_text.cpp", "Exibe legendas no idioma do jogo"), "PT-BR help");
}

int main(void) {
    test_profile_activate();
    test_timelines();
    test_cue_at();
    test_selection();
    test_session();
    test_release_timelines();
    test_timing_variant_and_text_language();
    test_fingerprints();
    test_releases();
    test_shared_and_specific_tables();
    test_retail_files();
    test_languages();
    test_setting();
    test_wrap();
    test_layout();
    test_target_spaces();
    test_blend();
    test_outline();
    test_no_reference_material();
    test_wiring();
    printf("video_subtitle_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
