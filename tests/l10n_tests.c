/*
 * Headless checks of the text translations: the language choice and what it gives, the settings and their migration, the
 * embedded text itself, GameStrings parsing, code pages, TPA caption blocks, DDF labels and the overlay's word wrap for
 * translated text. The fixtures are made up here.
 * Build: CMake target l10n_tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "jpt_config.h"
#include "jpt_l10n.h"
#include "l10n_assets.h"
#include "text_layout.h"

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

#define MAX_TEXT 64

typedef struct {
    int count;
    unsigned id[MAX_TEXT];
    char text[MAX_TEXT][256];
} texts_t;

static void collect(void* ctx, unsigned id, const char* utf8) {
    texts_t* t = (texts_t*)ctx;
    if (t->count == MAX_TEXT) return;
    t->id[t->count] = id;
    snprintf(t->text[t->count++], 256, "%s", utf8);
}

static const char* text_of(const texts_t* t, unsigned id) {
    for (int i = 0; i < t->count; i++)
        if (t->id[i] == id) return t->text[i];
    return NULL;
}

static void test_gamestrings(void) {
    /* 0xE7 0xE3 = c-cedilla, a-tilde in 1252; the second line repeats ID 2 */
    static const char file[] = "; a comment\r\n"
                               "\r\n"
                               "1=\"Ol\xE1 mundo\"\r\n"
                               "2=\"A\\r\\nB \\\"quoted\\\" back\\\\slash\"\r\n"
                               "2=\"duplicate\"\r\n"
                               "  3 = \"spaced\" ; trailing comment\r\n"
                               "4=\"say \"\"hi\"\"\"\r\n"
                               "five=\"not an id\"\r\n"
                               "6=\"unterminated\r\n"
                               "7=\"\"\r\n"
                               "8=\"Fim.Come\xE7o!Ent\xE3o\"\n"
                               "9=\"Readme.txt names a file.\"";
    static const char glued[] = "8=\"Fim.Come\xE7o\"";
    static const char key[] = "16012=\"Verr.Maj\"";
    static const char bom[] ="\xEF\xBB\xBF" "10=\"\xC3\xBC\"";
    texts_t t;
    jpt_gs_stats_t st;
    memset(&t, 0, sizeof t);
    CHECK(jpt_gs_parse(file, sizeof file - 1, 1252, 1, collect, &t, &st));
    CHECK(st.entries == 6 && st.duplicates == 1 && st.invalid == 2 && st.first_invalid_line == 8);
    CHECK(text_of(&t, 1) && !strcmp(text_of(&t, 1), "Ol\xC3\xA1 mundo"));
    CHECK(text_of(&t, 2) && !strcmp(text_of(&t, 2), "A\r\nB \"quoted\" back\\slash"));
    CHECK(text_of(&t, 3) && !strcmp(text_of(&t, 3), "spaced"));
    CHECK(text_of(&t, 4) && !strcmp(text_of(&t, 4), "say \"hi\""));
    CHECK(!text_of(&t, 7));
    CHECK(text_of(&t, 8) && !strcmp(text_of(&t, 8), "Fim. Come\xC3\xA7o! Ent\xC3\xA3o"));
    CHECK(text_of(&t, 9) && !strcmp(text_of(&t, 9), "Readme.txt names a file."));
    CHECK(st.normalized == 2);

    memset(&t, 0, sizeof t);
    CHECK(jpt_gs_parse(glued, sizeof glued - 1, 1252, 0, collect, &t, &st) && !strcmp(t.text[0], "Fim.Come\xC3\xA7o"));
    memset(&t, 0, sizeof t);
    CHECK(jpt_gs_parse(key, sizeof key - 1, 1252, 1, collect, &t, &st) && !strcmp(t.text[0], "Verr.Maj") &&
          st.normalized == 0);
    memset(&t, 0, sizeof t);
    CHECK(!jpt_gs_parse("; only comments\r\n\r\n", 19, 1252, 1, collect, &t, &st) && t.count == 0);
    memset(&t, 0, sizeof t);
    CHECK(jpt_gs_parse(bom, sizeof bom - 1, 1251, 0, collect, &t, &st) &&
          !strcmp(t.text[0], "\xC3\xBC"));
    memset(&t, 0, sizeof t);
    CHECK(!jpt_gs_parse("70000=\"too big\"", 15, 1252, 0, collect, &t, &st) && st.invalid == 1);
}

static void test_cyrillic(void) {
    /* "Privet." in 1251: 0xCF 0xF0 0xE8 0xE2 0xE5 0xF2, then ya (0xFF) and yo (0xB8) */
    static const char file[] = "12=\"\xCF\xF0\xE8\xE2\xE5\xF2.\xDF\xFF \xB8\"";
    texts_t t;
    jpt_gs_stats_t st;
    char back[64];
    memset(&t, 0, sizeof t);
    CHECK(jpt_gs_parse(file, sizeof file - 1, 1251, 1, collect, &t, &st) && st.normalized == 1);
    CHECK(!strcmp(t.text[0], "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82. \xD0\xAF\xD1\x8F \xD1\x91"));
    CHECK(jpt_utf8_to_cp(1251, t.text[0], back, sizeof back) != (size_t)-1);
    CHECK(!strcmp(back, "\xCF\xF0\xE8\xE2\xE5\xF2. \xDF\xFF \xB8"));
    /* Cyrillic has no 1252 form */
    CHECK(jpt_utf8_to_cp(1252, "\xD0\xAF", back, sizeof back) == 1 && !strcmp(back, "?"));
    CHECK(jpt_cp_charset(1251) == 204 && jpt_cp_charset(1252) == 0);
}

static void test_latin(void) {
    char out[64];
    /* German sharp s and u-umlaut, Spanish n-tilde and inverted question mark, Italian e-grave */
    static const char german[] = "\xDF\xFC\xF1\xBF\xE8\x80";
    CHECK(jpt_cp_to_utf8(1252, german, 6, out, sizeof out) != (size_t)-1);
    CHECK(!strcmp(out, "\xC3\x9F\xC3\xBC\xC3\xB1\xC2\xBF\xC3\xA8\xE2\x82\xAC"));
    CHECK(jpt_utf8_to_cp(1252, out, out, sizeof out) == 6 && !memcmp(out, german, 6));
    /* accents a Cyrillic code page lacks are dropped, not replaced by garbage */
    CHECK(jpt_utf8_to_cp(1251, "Ol\xC3\xA1 \xC3\xA7\xC3\xA3o \xC3\x9F", out, sizeof out) != (size_t)-1);
    CHECK(!strcmp(out, "Ola cao s"));
    CHECK(jpt_utf8_to_cp(1252, "\xC3", out, sizeof out) == (size_t)-1);
    CHECK(jpt_cp_to_utf8(1252, "abc", 3, out, 3) == (size_t)-1);
    CHECK(jpt_utf8_valid("\xD0\xAF", 2) && !jpt_utf8_valid("\xC0\xAF", 2) && !jpt_utf8_valid("\xED\xA0\x80", 3));
}

static void test_formats(void) {
    CHECK(jpt_format_compatible("Replace\r\n\"%s\"?", "Substituir \"%s\"?"));
    CHECK(jpt_format_compatible("needs %i MB", "precisa de %d MB"));
    CHECK(jpt_format_compatible("100%% sure", "sem formato"));
    CHECK(!jpt_format_compatible("Replace \"%s\"?", "Substituir?"));
    CHECK(!jpt_format_compatible("%s on %s", "%s"));
    CHECK(!jpt_format_compatible("plain", "com %s a mais"));
}

typedef struct {
    int samples, lines;
    char pack[16];
    uint32_t sample, units[4];
    char first[64];
} subs_t;

static void collect_sample(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units,
                           const char* const* lines) {
    subs_t* s = (subs_t*)ctx;
    if (!s->samples++) {
        snprintf(s->pack, sizeof s->pack, "%s", pack);
        s->sample = sample;
        for (int i = 0; i < count && i < 4; i++) s->units[i] = units[i];
        snprintf(s->first, sizeof s->first, "%s", lines[0]);
    }
    s->lines += count;
}

static void test_installed_files(void) {
    char escaped[64];
    texts_t t;
    subs_t s;
    static const char strings[] = JPT_L10N_STRINGS_HEADER "\r\n"
                                  "18001\tLinha\\num\\tdois\\\\\n"
                                  "bad line\n"
                                  "70000\ttoo big\n"
                                  "18002\t\xD0\xAF\n";
    static const char subtitles[] = JPT_L10N_SUBTITLES_HEADER "\n"
                                    "S\tstream\t2fe21360\n"
                                    "C\t12\tPrima\\nriga\n"
                                    "C\t9\tSeconda\n"
                                    "C\tx\tbad units\n"
                                    "S\teffects\tzz\n"
                                    "C\t4\tignored: bad sample\n"
                                    "S\teffects\t0000abcd\n"
                                    "C\t0\tzero\n";
    CHECK(jpt_l10n_escape("a\\b\nc\td\r", escaped, sizeof escaped) == 12 && !strcmp(escaped, "a\\\\b\\nc\\td\\r"));
    CHECK(jpt_l10n_escape("abcdef", escaped, 4) == (size_t)-1);
    memset(&t, 0, sizeof t);
    CHECK(jpt_l10n_read_strings(strings, sizeof strings - 1, collect, &t) == 2);
    CHECK(text_of(&t, 18001) && !strcmp(text_of(&t, 18001), "Linha\num\tdois\\"));
    CHECK(text_of(&t, 18002) && !strcmp(text_of(&t, 18002), "\xD0\xAF"));
    CHECK(jpt_l10n_read_strings("18001\tno header", 15, collect, &t) == -1);
    memset(&s, 0, sizeof s);
    CHECK(jpt_l10n_read_subtitles(subtitles, sizeof subtitles - 1, collect_sample, &s) == 2);
    CHECK(s.lines == 3 && !strcmp(s.pack, "stream") && s.sample == 0x2fe21360u);
    CHECK(s.units[0] == 12 && s.units[1] == 9 && !strcmp(s.first, "Prima\nriga"));
}

typedef struct {
    int count;
    uint32_t units[4];
    char text[4][32];
} lines_t;

static void collect_line(void* ctx, uint32_t units, const char* text, size_t n) {
    lines_t* l = (lines_t*)ctx;
    if (l->count == 4) return;
    l->units[l->count] = units;
    snprintf(l->text[l->count++], 32, "%.*s", (int)n, text);
}

static void test_caption_blocks(void) {
    const char* lines[] = { "One line.", "\xC8 un'altra." };
    uint32_t units[] = { 12, 9 };
    uint8_t block[128], other[128];
    lines_t l;
    size_t n = jpt_caption_build(2, units, lines, block, sizeof block);
    CHECK(n == 8 + 4 + 10 + 4 + 12);
    memset(&l, 0, sizeof l);
    CHECK(jpt_caption_lines(block, n, collect_line, &l) == 2);
    CHECK(l.units[0] == 12 && l.units[1] == 9 && !strcmp(l.text[0], "One line.") && !strcmp(l.text[1], lines[1]));
    CHECK(jpt_caption_lines(block, n - 1, NULL, NULL) < 0);
    CHECK(jpt_caption_build(2, units, lines, other, 20) == 0);
    memcpy(other, block, n);
    CHECK(jpt_caption_fingerprint(block, n) == jpt_caption_fingerprint(other, n));
    other[8] = 13; /* the same text with another timing is another block */
    CHECK(jpt_caption_fingerprint(block, n) != jpt_caption_fingerprint(other, n));
    block[n - 1] = 'x'; /* no NUL */
    CHECK(jpt_caption_lines(block, n, NULL, NULL) < 0);
    CHECK(JPT_CAPTION_UNIT_MS == 200);
}

/* A TPA made here: three samples, the second captioned, the third a repeat of the first's ID. */
typedef struct {
    uint8_t bytes[1024];
    size_t size;
} memfile_t;

static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

static int mem_read(void* ctx, uint64_t offset, void* buf, size_t n) {
    memfile_t* f = (memfile_t*)ctx;
    if (offset + n > f->size) return 0;
    memcpy(buf, f->bytes + offset, n);
    return 1;
}

static void make_tpa(memfile_t* f, const uint8_t* caption, size_t caption_n) {
    uint32_t data = JPT_TPA_HEADER + 3 * JPT_TPA_ENTRY, header = JPT_TPA_DESCRIPTOR + (uint32_t)caption_n + 4;
    memset(f, 0, sizeof *f);
    put32(f->bytes, JPT_TPA_MAGIC);
    put32(f->bytes + 4, 3);
    put32(f->bytes + 8, JPT_TPA_HEADER);
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t* e = f->bytes + JPT_TPA_HEADER + i * JPT_TPA_ENTRY;
        put32(e, i == 1 ? 0x2FE21360u : 0x11111111u);
        put32(e + 4, data);
        memcpy(e + 0x14, "WBOR", 4);
        put32(e + 0x18, 0x6E);
        put32(e + 0x1C, i == 1 ? header : JPT_TPA_DESCRIPTOR);
    }
    memcpy(f->bytes + data, f->bytes + JPT_TPA_HEADER + JPT_TPA_ENTRY + 0x14, JPT_TPA_DESCRIPTOR);
    memcpy(f->bytes + data + JPT_TPA_DESCRIPTOR, caption, caption_n);
    f->size = data + header + 16;
}

typedef struct {
    int count;
    uint32_t sample;
    size_t n;
} tpa_seen_t;

static void on_caption(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    tpa_seen_t* s = (tpa_seen_t*)ctx;
    (void)block;
    s->count++;
    s->sample = sample;
    s->n = n;
}

static void test_tpa(void) {
    const char* lines[] = { "Welcome." };
    uint32_t units[] = { 10 };
    uint8_t block[64];
    size_t n = jpt_caption_build(1, units, lines, block, sizeof block);
    memfile_t f;
    tpa_seen_t seen;
    jpt_tpa_stats_t st;
    make_tpa(&f, block, n);
    memset(&seen, 0, sizeof seen);
    CHECK(jpt_tpa_captions(mem_read, &f, f.size, on_caption, &seen, &st));
    CHECK(seen.count == 1 && seen.sample == 0x2FE21360u && seen.n == n);
    CHECK(st.samples == 2 && st.captioned == 1 && st.duplicates == 1 && st.malformed == 0);
    f.bytes[JPT_TPA_HEADER + 3 * JPT_TPA_ENTRY] = 'X'; /* the data no longer repeats the descriptor */
    memset(&seen, 0, sizeof seen);
    CHECK(jpt_tpa_captions(mem_read, &f, f.size, on_caption, &seen, &st) && seen.count == 0 && st.malformed == 1);
    put32(f.bytes, 0x151);
    CHECK(!jpt_tpa_captions(mem_read, &f, f.size, on_caption, &seen, &st));
    make_tpa(&f, block, n);
    put32(f.bytes + 4, 100000);
    CHECK(!jpt_tpa_captions(mem_read, &f, f.size, on_caption, &seen, &st));
}

static const char* menu_lookup(void* ctx, unsigned id) {
    (void)ctx;
    return id == 10054 ? "Legendas" : id == 10025 ? "Pular \"x\"" : NULL;
}

static void test_ddf(void) {
    static const char ddf[] = "UICTRL \"checkbox\" 1 1 1020 80\r\n"
                              "        \"Show Subtitles\" ; Initial Text\r\n"
                              "        \"Jump\" ; Initial Text\r\n"
                              "        \"Crouch\" ; Initial Text\r\n"
                              "        \"menu\\\\check_0.tga\"\r\n"
                              "        \"Show Subtitles\" ; Other";
    char* out = NULL;
    size_t n = 0;
    CHECK(jpt_ddf_text_id("Show Subtitles") == 10054 && jpt_ddf_text_id("Quality") == 10000 &&
          jpt_ddf_text_id("Rotate Arm") == 10037 && !jpt_ddf_text_id("Unknown"));
    CHECK(jpt_ddf_translate(ddf, sizeof ddf - 1, menu_lookup, NULL, &out, &n) == 1);
    CHECK(out && strstr(out, "        \"Legendas\" ; Initial Text\r\n") && strstr(out, "\"Jump\" ; Initial Text") &&
          strstr(out, "\"Crouch\" ; Initial Text") && strstr(out, "\"Show Subtitles\" ; Other"));
    CHECK(out && n == strlen(out) && n == sizeof ddf - 1 - strlen("Show Subtitles") + strlen("Legendas"));
    free(out);
    CHECK(!strcmp(jpt_tutorial_key("You can always press F1 for a hint about what to do next."),
                  "Tutorial::You can always press <HINT> for a hint about what to do next."));
    CHECK(!jpt_tutorial_key("Something else."));
}

static void test_language_mapping(void) {
    static const char* const codes[] = { "en", "pt-BR", "fr", "de", "es", "ru", "it" };
    CHECK(JPT_L10N_LANGUAGE_COUNT == (int)(sizeof codes / sizeof codes[0]));
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) CHECK(jpt_l10n_language(codes[i]) == &jpt_l10n_languages[i]);
    CHECK(!jpt_l10n_language("xx") && !jpt_l10n_language(NULL) && !jpt_l10n_language(""));
    CHECK(jpt_l10n_id_fixed(10) && !jpt_l10n_id_fixed(5) && !jpt_l10n_id_fixed(18001));
    {
        static const char* const names[] = { "English", "Brazilian Portuguese", "French", "German", "Spanish", "Russian", "Italian" };
        for (int i = 0; i < JPT_L10N_LANGUAGE_COUNT; i++) CHECK(!strcmp(jpt_l10n_languages[i].name, names[i]));
    }
    {
        static const char* const listed[] = { "English", "Brazilian Portuguese", "French", "German", "Italian", "Russian", "Spanish" };
        static const char* const listed_codes[] = { "en", "pt-BR", "fr", "de", "it", "ru", "es" };
        for (int i = 0; i < JPT_L10N_LANGUAGE_COUNT; i++) {
            const jpt_l10n_language_t* l = jpt_l10n_language_listed(i);
            CHECK(l && !strcmp(l->name, listed[i]) && !strcmp(l->code, listed_codes[i]) && l == jpt_l10n_language(listed_codes[i]),
                  "launcher position %d is %s, the same entry as its code's", i, listed[i]);
            CHECK(i < 2 || strcmp(jpt_l10n_language_listed(i - 1)->name, l->name) < 0, "%s follows its neighbour alphabetically", listed[i]);
        }
        CHECK(!jpt_l10n_language_listed(-1) && !jpt_l10n_language_listed(JPT_L10N_LANGUAGE_COUNT), "positions outside the list");
        CHECK(!strcmp(jpt_l10n_language_listed(0)->code, JPT_L10N_ENGLISH), "English is first");
    }
    CHECK(jpt_l10n_language("ru")->codepage == 1251 && jpt_l10n_language("de")->codepage == 1252);
    CHECK(JPT_L10N_CREDIT_COUNT == 7 && !strcmp(jpt_l10n_credits[0].language, "pt-BR") &&
          !strcmp(jpt_l10n_credits[0].converter, "Nick3069") && !strcmp(jpt_l10n_credits[4].author, "cynodesmus") &&
          !strcmp(jpt_l10n_credits[5].author, "KeyofBlueS") && !strcmp(JPT_L10N_PRESERVATION, "TresCom community"));
    for (int i = 0; i < JPT_L10N_CREDIT_COUNT; i++)
        CHECK(jpt_l10n_language(jpt_l10n_credits[i].language) != NULL &&
              (!strcmp(jpt_l10n_credits[i].source, "official") || !strcmp(jpt_l10n_credits[i].source, "community")));
    CHECK(!strcmp(jpt_l10n_credits[0].source, "official") && !strcmp(jpt_l10n_credits[4].source, "community"));
    CHECK(!strcmp(JPT_L10N_PORT_COMPLETION, "Trespasser Recompiled") && strstr(JPT_L10N_PORT_CONTENT, "subtitles"));
}

static void test_inferred_language(void) {
    CHECK(!strcmp(jpt_l10n_infer_language("pt-BR", "en"), "pt-BR"));
    CHECK(!strcmp(jpt_l10n_infer_language("ru", "ru"), "ru"));
    CHECK(!strcmp(jpt_l10n_infer_language("en", "it"), "it"));
    CHECK(!strcmp(jpt_l10n_infer_language("fr", ""), "fr"));
    CHECK(!strcmp(jpt_l10n_infer_language("fr", "it"), "fr"));
    CHECK(!strcmp(jpt_l10n_infer_language("it", "en"), "it"));
    CHECK(!strcmp(jpt_l10n_infer_language("en", "pt-BR"), "pt-BR"));
    CHECK(!strcmp(jpt_l10n_infer_language("", ""), "en") && !strcmp(jpt_l10n_infer_language(NULL, NULL), "en"));
}

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int has_line(const char* path, const char* key) {
    char line[256], k[128], v[128];
    FILE* f = fopen(path, "r");
    int found = 0;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        found |= jpt_ini_parse_line(line, 0, k, sizeof k, v, sizeof v) && !strcmp(k, key);
    }
    if (f) fclose(f);
    return found;
}

static void test_config(void) {
    static const struct {
        const char* file;
        const char* language;
    } cases[] = {
        { "game_text_language = pt-BR\n", "pt-BR" },
        { "game_text_language = ru\nsubtitle_language = ru\n", "ru" },
        { "game_text_language = en\nsubtitle_language = it\n", "it" },
        { "game_text_language = fr\n", "fr" },
        { "subtitle_language = it\ngame_text_language = it\n", "it" },
        { "game_text_language = it\nsubtitle_language = en\n", "it" },
        { "game_text_language = klingon\n", "en" },
        { "gore = 2\nvsync = 0\n", "en" },
        { "language = de\ngame_text_language = pt-BR\n", "de" },
        { "language = es\n", "es" },
        { "language = nonsense\ngame_text_language = fr\n", "fr" },
        { "vsync = 1\n", "en" },
    };
    char dir[MAX_PATH], path[MAX_PATH];
    jpt_config_t c;
    GetTempPathA(MAX_PATH, dir);
    snprintf(path, sizeof path, "%sjpt_l10n_config_%lu.ini", dir, GetCurrentProcessId());
    jpt_config_defaults(&c);
    CHECK(!strcmp(c.language, "en"));
    CHECK(jpt_config_apply(&c, JPT_KEY_LANGUAGE, "pt-BR") && !strcmp(c.language, "pt-BR"));
    CHECK(!jpt_config_apply(&c, JPT_KEY_LANGUAGE, "xx") && !strcmp(c.language, "pt-BR"));
    CHECK(jpt_config_valid(JPT_KEY_LANGUAGE, "it") && !jpt_config_valid(JPT_KEY_LANGUAGE, ""));
    CHECK(!c.show_resolution_title && !strcmp(jpt_default_value(JPT_KEY_SHOW_RESOLUTION_TITLE), "0"));
    CHECK(jpt_config_apply(&c, JPT_KEY_SHOW_RESOLUTION_TITLE, "on") && c.show_resolution_title);
    CHECK(!jpt_config_apply(&c, JPT_KEY_SHOW_RESOLUTION_TITLE, "maybe") && c.show_resolution_title);
    CHECK(!jpt_default_value("gore") && !jpt_default_value("Gore"));
    CHECK(!strcmp(jpt_default_value(JPT_KEY_LANGUAGE), "en") && !jpt_default_value(JPT_KEY_LEGACY_GAME_TEXT_LANGUAGE) &&
          !jpt_default_value(JPT_KEY_LEGACY_SUBTITLE_LANGUAGE));
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        write_file(path, cases[i].file);
        CHECK(jpt_config_load(path, &c) && !strcmp(c.language, cases[i].language));
    }
    write_file(path, "game_text_language = pt-BR\nvsync = 0\n");
    CHECK(jpt_config_migrate_language(path) == 1 && !has_line(path, "game_text_language") &&
          !has_line(path, "subtitle_language") && has_line(path, "language") && has_line(path, "vsync"));
    CHECK(jpt_config_load(path, &c) && !strcmp(c.language, "pt-BR") && !c.vsync);
    CHECK(jpt_config_migrate_language(path) == 0);
    write_file(path, "game_text_language = en\nsubtitle_language = it\n");
    CHECK(jpt_config_migrate_language(path) == 1 && jpt_config_load(path, &c) && !strcmp(c.language, "it"));
    write_file(path, "language = fr\ngame_text_language = ru\nsubtitle_language = ru\n");
    CHECK(jpt_config_migrate_language(path) == 1 && !has_line(path, "game_text_language") &&
          !has_line(path, "subtitle_language") && jpt_config_load(path, &c) && !strcmp(c.language, "fr"));
    write_file(path, "vsync = 1\n");
    CHECK(jpt_config_migrate_language(path) == 0 && !has_line(path, "language"));
    write_file(path, "gore = 2\nvsync = 0\n");
    CHECK(jpt_config_migrate_language(path) == 0 && has_line(path, "gore") && jpt_config_load(path, &c) && !c.vsync);
    DeleteFileA(path);
    CHECK(jpt_config_migrate_language(path) == 0 && jpt_config_load(path, &c) == 0 && !strcmp(c.language, "en"));
    jpt_l10n_set_display_codepage(0);
    CHECK(jpt_l10n_display_charset(7) == 7);
    jpt_l10n_set_display_codepage(1251);
    CHECK(jpt_l10n_display_charset(0) == 204);
    jpt_l10n_set_display_codepage(0);
    CHECK(!strcmp(jpt_l10n_text_or(10033, "Replay"), "Replay"));
}

typedef struct {
    int count, empty, invalid_utf8, lost_in_codepage, bad_ids;
    int codepage;
    unsigned id[512];
} embedded_strings_t;

static void check_embedded_string(void* ctx, unsigned id, const char* utf8) {
    embedded_strings_t* s = (embedded_strings_t*)ctx;
    char back[2048];
    size_t question = 0, kept = 0;
    for (const char* p = utf8; *p; p++) question += *p == '?';
    if (s->count < 512) s->id[s->count] = id;
    s->count++;
    s->empty += !utf8[0];
    s->invalid_utf8 += !jpt_utf8_valid(utf8, strlen(utf8));
    s->bad_ids += id >= JPT_L10N_ID_LIMIT;
    if (jpt_utf8_to_cp(s->codepage, utf8, back, sizeof back) == (size_t)-1) s->lost_in_codepage++;
    else {
        for (const char* p = back; *p; p++) kept += *p == '?';
        s->lost_in_codepage += kept != question;
    }
}

static int has_id(const embedded_strings_t* s, unsigned id) {
    for (int i = 0; i < s->count && i < 512; i++)
        if (s->id[i] == id) return 1;
    return 0;
}

typedef struct {
    int samples, lines, bad, stream, effects;
    int codepage, lost;
} embedded_subtitles_t;

static void check_embedded_sample(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units,
                                  const char* const* lines) {
    embedded_subtitles_t* s = (embedded_subtitles_t*)ctx;
    char back[512];
    s->samples++;
    s->lines += count;
    s->stream += !strcmp(pack, "stream");
    s->effects += !strcmp(pack, "effects");
    s->bad += !sample || count < 1 || count > JPT_CAPTION_MAX_LINES;
    for (int i = 0; i < count; i++) {
        size_t question = 0, kept = 0;
        for (const char* p = lines[i]; *p; p++) question += *p == '?';
        s->bad += units[i] > 60 || !jpt_utf8_valid(lines[i], strlen(lines[i]));
        if (jpt_utf8_to_cp(s->codepage, lines[i], back, sizeof back) == (size_t)-1) s->lost++;
        else {
            for (const char* p = back; *p; p++) kept += *p == '?';
            s->lost += kept != question;
        }
    }
}

static void test_embedded_text(void) {
    static const struct {
        const char* language;
        int min_strings;
    } text[] = { { "pt-BR", 228 }, { "fr", 228 }, { "de", 218 }, { "es", 228 }, { "ru", 217 }, { "it", 227 } };
    static const struct {
        const char* language;
        int min_samples, min_lines, effects;
    } subtitles[] = { { "ru", 485, 985, 1 }, { "it", 475, 1077, 0 } };
    const char* data;
    size_t n;
    for (size_t i = 0; i < sizeof text / sizeof text[0]; i++) {
        embedded_strings_t s;
        memset(&s, 0, sizeof s);
        s.codepage = jpt_l10n_language(text[i].language)->codepage;
        CHECK(l10n_asset(text[i].language, strcmp(text[i].language, "it") ? JPT_L10N_STRINGS_ASSET : JPT_L10N_STRINGS_PORT_ASSET,
                         &data, &n) && n > 4000);
        CHECK(jpt_l10n_read_strings(data, n, check_embedded_string, &s) == text[i].min_strings);
        CHECK(s.count == text[i].min_strings && !s.empty && !s.invalid_utf8 && !s.bad_ids && !s.lost_in_codepage);
        CHECK(has_id(&s, 2) && has_id(&s, 10054) && has_id(&s, 16049) && has_id(&s, 18300) && has_id(&s, 19290));
        CHECK(strcmp(text[i].language, "it") || !has_id(&s, 10));
        CHECK(!memchr(data, '\r', n));
    }
    for (size_t i = 0; i < sizeof subtitles / sizeof subtitles[0]; i++) {
        embedded_subtitles_t s;
        memset(&s, 0, sizeof s);
        s.codepage = jpt_l10n_language(subtitles[i].language)->codepage;
        CHECK(l10n_asset(subtitles[i].language, JPT_L10N_SUBTITLES_ASSET, &data, &n));
        CHECK(jpt_l10n_read_subtitles(data, n, check_embedded_sample, &s) == subtitles[i].min_samples);
        CHECK(s.samples == subtitles[i].min_samples && s.lines == subtitles[i].min_lines && !s.bad && !s.lost);
        CHECK(s.stream > 400 && (s.effects > 0) == subtitles[i].effects);
    }
    {
        static const char* const port[] = { "pt-BR", "fr", "de", "es" };
        for (size_t i = 0; i < sizeof port / sizeof port[0]; i++) {
            embedded_subtitles_t s;
            memset(&s, 0, sizeof s);
            s.codepage = 1252;
            CHECK(!l10n_asset(port[i], JPT_L10N_SUBTITLES_ASSET, &data, &n));
            CHECK(l10n_asset(port[i], JPT_L10N_SUBTITLES_PORT_ASSET, &data, &n));
            CHECK(jpt_l10n_read_subtitles(data, n, check_embedded_sample, &s) == 486 && s.effects == 76 && !s.bad && !s.lost);
        }
    }
    CHECK(!l10n_asset("it", JPT_L10N_STRINGS_ASSET, &data, &n) && !l10n_asset("en", JPT_L10N_STRINGS_ASSET, &data, &n));
    CHECK(l10n_asset("en", JPT_L10N_SUBTITLES_ASSET, &data, &n) && !l10n_asset("xx", JPT_L10N_STRINGS_ASSET, &data, &n));
    CHECK(l10n_asset("ru", JPT_L10N_SUBTITLES_PORT_ASSET, &data, &n) && l10n_asset("it", JPT_L10N_SUBTITLES_PORT_ASSET, &data, &n));
}

typedef struct {
    char text[4][512];
    int found[4];
} formats_t;

static const unsigned k_format_ids[4] = { 6, 7, 22, 33 };

static void collect_formats(void* ctx, unsigned id, const char* utf8) {
    formats_t* f = (formats_t*)ctx;
    for (int i = 0; i < 4; i++)
        if (id == k_format_ids[i] && strlen(utf8) < sizeof f->text[i]) {
            strcpy(f->text[i], utf8);
            f->found[i] = 1;
        }
}

/* The strings the game formats keep the English printf conversions (strings 6, 7, 22 and 33). */
static void test_embedded_formats(void) {
    static const char* const english[4] = { "Replace \"%s\"?", "Delete game \"%s\"?", "Insert the CD in %s.",
                                            "Trespasser requires %i MB of free disk space." };
    static const char* const languages[] = { "pt-BR", "fr", "de", "es", "ru", "it" };
    for (size_t l = 0; l < sizeof languages / sizeof languages[0]; l++) {
        const char* data;
        size_t n;
        formats_t f;
        memset(&f, 0, sizeof f);
        CHECK(l10n_asset(languages[l], strcmp(languages[l], "it") ? JPT_L10N_STRINGS_ASSET : JPT_L10N_STRINGS_PORT_ASSET, &data, &n));
        CHECK(jpt_l10n_read_strings(data, n, collect_formats, &f) > 200);
        for (int i = 0; i < 4; i++)
            if (f.found[i]) CHECK(jpt_format_compatible(english[i], f.text[i]));
    }
}

/* Advances: 10 for every byte, 30 for 0xE9 and 0xFF, 7 for anything below 0x20. */
static uint32_t widths(void* ctx, unsigned char c) {
    (void)ctx;
    return c == 0xE9 || c == 0xFF ? 30u : c < 0x20 ? 7u : 10u;
}

typedef struct {
    int32_t x, y;
    uint32_t count;
    const char* text;
} line_t;

static int read_lines(const uint8_t* out, line_t* lines, int max) {
    uint32_t n, at = 8;
    memcpy(&n, out + 4, 4);
    for (uint32_t i = 0; i < n && (int)i < max; i++) {
        memcpy(&lines[i].x, out + at, 4);
        memcpy(&lines[i].y, out + at + 4, 4);
        memcpy(&lines[i].count, out + at + 8, 4);
        lines[i].text = (const char*)out + at + TL_RECORD;
        at += TL_RECORD + (uint32_t)strlen(lines[i].text) + 1;
    }
    return (int)n;
}

static void test_layout(void) {
    tl_view_t v = { 116, 400, 5, 20, 18 }; /* lines up to 100 wide */
    uint8_t out[1024];
    line_t l[8];
    uint32_t size;
    memset(out, 0xCC, sizeof out);
    CHECK(tl_layout("hello world again", TL_ALIGN_CENTRE, &v, widths, NULL, out, sizeof out) > 0);
    CHECK(read_lines(out, l, 8) == 3);
    CHECK(!strcmp(l[0].text, "hello ") && l[0].count == 6 && l[0].x == (100 - 60) / 2 + 5 + 8 && l[0].y == 20 + 8);
    CHECK(!strcmp(l[1].text, "world ") && !strcmp(l[2].text, "again") && l[2].y == 20 + 8 + 2 * 18);
    memcpy(&size, out, 4);
    CHECK(size > 8);

    /* Accented and 0xFF letters take their own, unsigned widths: the second word no longer fits 130. */
    v.width = 146;
    memset(out, 0, sizeof out);
    CHECK(tl_layout("\xE9\xE9\xE9 \xFF\xFF\xFF", TL_ALIGN_LEFT, &v, widths, NULL, out, sizeof out) > 0);
    CHECK(read_lines(out, l, 8) == 2 && !strcmp(l[0].text, "\xE9\xE9\xE9 ") && !strcmp(l[1].text, "\xFF\xFF\xFF"));
    CHECK(l[0].x == 13 && l[1].y == 20 + 8 + 18);

    /* CR ends a line; right and bottom alignment. */
    v.width = 116;
    memset(out, 0, sizeof out);
    CHECK(tl_layout("ab\rcd", TL_ALIGN_RIGHT | TL_ALIGN_BOTTOM, &v, widths, NULL, out, sizeof out) > 0);
    CHECK(read_lines(out, l, 8) == 2 && !strcmp(l[0].text, "ab") && !strcmp(l[1].text, "cd"));
    CHECK(l[0].x == 5 + 116 - 20 - 8 && l[0].y == 20 + 8 + (400 - (8 + 36) - 8));

    /* A word longer than the line is cut where it stops fitting, as retail does. */
    memset(out, 0, sizeof out);
    CHECK(tl_layout("abcdefghijklmno", TL_ALIGN_LEFT, &v, widths, NULL, out, sizeof out) > 0);
    CHECK(read_lines(out, l, 8) == 2 && l[0].count == 9 && !strcmp(l[1].text, "jklmno"));

    /* No alignment flag: x is left as it was. */
    memset(out, 0x5A, sizeof out);
    CHECK(tl_layout("x", 0, &v, widths, NULL, out, sizeof out) > 0 && read_lines(out, l, 8) == 1);
    CHECK(l[0].x == 0x5A5A5A5A);

    /* A glyph wider than the line ends instead of looping. */
    v.width = 30;
    memset(out, 0, sizeof out);
    CHECK(tl_layout("\xE9\xE9", TL_ALIGN_LEFT, &v, widths, NULL, out, sizeof out) > 0 && read_lines(out, l, 8) == 2);
    CHECK(tl_layout("", TL_ALIGN_LEFT, &v, widths, NULL, out, sizeof out) == 8 && read_lines(out, l, 8) == 0);
    CHECK(tl_layout("abc", TL_ALIGN_LEFT, &v, widths, NULL, out, 10) == 0);
}

void l10n_coverage_tests(int* checks, int* failures, int list_inventory);

static int menu_source(const char* user, const char* path, char* src, char* name) {
    return jpt_menu_ddf_source("C:\\port\\game\\", user, path, src, 300, name, 64);
}

static void test_menu_ddf_source(void) {
    static const char* const names[] = {"mainscreen", "options", "controls", "loader", "gamewnd", "hint",
                                        "ingameopt", "options2", "loadgame", "yesno", "quit"};
    const char* user = "C:\\port\\user_data\\saves\\";
    char in[300], want[300], src[300], name[64];
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        snprintf(want, sizeof want, "C:\\port\\game\\data\\menu\\%s.ddf", names[i]);
        snprintf(in, sizeof in, "menu\\%s.ddf", names[i]);
        CHECK(menu_source(user, in, src, name) && !strcmp(src, want));
        snprintf(in, sizeof in, "C:\\port\\user_data\\saves\\menu\\%s.ddf", names[i]);
        CHECK(menu_source(user, in, src, name) && !strcmp(src, want) && !strstr(src, "saves"));
        snprintf(in, sizeof in, "C:\\port\\game\\data\\menu\\%s.ddf", names[i]);
        CHECK(menu_source(user, in, src, name) && !strcmp(src, want));
        CHECK(!strncmp(name, names[i], strlen(names[i])));
    }
    CHECK(menu_source(user, "MENU\\Controls.DDF", src, name) && !strcmp(src, "C:\\port\\game\\data\\menu\\Controls.DDF") &&
          !strcmp(name, "Controls.DDF"));
    CHECK(menu_source(user, "c:\\PORT\\GAME\\DATA\\MENU\\x.ddf", src, name) && !strstr(src, "data\\data") &&
          !strstr(src, "game\\game"));
    {
        static const char* const probed[] = {"mainscreen", "options"};
        for (int i = 0; i < 2; i++) {
            snprintf(in, sizeof in, "C:\\port\\user_data\\saves\\menu\\%s.ddf", probed[i]);
            snprintf(want, sizeof want, "C:\\port\\game\\data\\menu\\%s.ddf", probed[i]);
            CHECK(menu_source(user, in, src, name) && !strcmp(src, want) && !strstr(src, "user_data"));
        }
    }
    CHECK(!menu_source(user, "menu\\..\\..\\x.ddf", src, name));
    CHECK(!menu_source(user, "C:\\port\\game\\data\\menu\\..\\x.ddf", src, name));
    CHECK(!menu_source(user, "\\\\server\\share\\menu\\x.ddf", src, name));
    CHECK(!menu_source(user, "D:\\other\\data\\menu\\x.ddf", src, name));
    CHECK(!menu_source(user, "C:\\port\\game\\data\\menu\\sub\\x.ddf", src, name));
    CHECK(!menu_source(user, "C:\\port\\user_data\\saves\\SAVEGAME.0", src, name));
    CHECK(!menu_source(user, "C:\\port\\user_data\\saves\\level.swp", src, name));
    CHECK(!menu_source(user, "menu\\x.txt", src, name));
    CHECK(!menu_source(NULL, "C:\\port\\user_data\\saves\\menu\\x.ddf", src, name));
}

int main(int argc, char** argv) {
    int inventory = argc > 1 && !strcmp(argv[1], "--inventory");
    test_gamestrings();
    test_cyrillic();
    test_latin();
    test_formats();
    test_installed_files();
    test_caption_blocks();
    test_tpa();
    test_ddf();
    test_menu_ddf_source();
    test_language_mapping();
    test_inferred_language();
    test_config();
    test_embedded_text();
    test_embedded_formats();
    test_layout();
    l10n_coverage_tests(&s_checks, &s_failures, inventory);
    printf("l10n_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
