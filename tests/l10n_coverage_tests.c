/*
 * The completeness of every language's text, from the embedded files (and the game's own English when its files are in
 * game\): GameStrings and voice captions against the English sets, the interface table against its English key list,
 * placeholders and control tokens kept, nothing left in English, every character in the code page and in the fonts.
 * Prints the coverage per language; l10n_tests --inventory also lists the game's menu files and their labels.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "jpt_l10n.h"
#include "jpt_prompt_text.h"
#include "jpt_tr.h"
#include "l10n_assets.h"
#include "text_layout.h"

#ifdef JPT_SOURCE_DIR
#define GAME_DIR JPT_SOURCE_DIR "/game/"
#define SOURCE_DIR JPT_SOURCE_DIR "/"
#else
#define GAME_DIR "game/"
#define SOURCE_DIR ""
#endif

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

static const char* const k_languages[] = { "pt-BR", "fr", "de", "es", "ru", "it" };
#define LANGUAGES ((int)(sizeof k_languages / sizeof k_languages[0]))
#define MAX_IDS 65536
#define MAX_SAMPLES 2048
#define MAX_KEYS 512

/* ---- GameStrings */

typedef struct {
    unsigned char has[MAX_IDS];
    char* text[MAX_IDS];
    int count;
} id_set_t;

static void add_id(void* ctx, unsigned id, const char* utf8) {
    id_set_t* s = (id_set_t*)ctx;
    if (id >= MAX_IDS || s->has[id]) return;
    s->has[id] = 1;
    s->text[id] = _strdup(utf8);
    s->count++;
}

static void free_ids(id_set_t* s) {
    for (int i = 0; i < MAX_IDS; i++) free(s->text[i]);
    memset(s, 0, sizeof *s);
}

static int read_asset_strings(const char* language, const char* kind, id_set_t* s) {
    const char* data;
    size_t n;
    return l10n_asset(language, kind, &data, &n) ? jpt_l10n_read_strings(data, n, add_id, s) : 0;
}

/* The English text of the game's own string table, when trespass.exe is there. */
static int english_strings(id_set_t* s) {
    HMODULE exe = LoadLibraryExA(GAME_DIR "trespass.exe", NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!exe) return 0;
    for (unsigned id = 0; id < MAX_IDS; id++) {
        WCHAR wide[4096];
        char utf8[8192];
        int n = LoadStringW(exe, id, wide, 4096), m;
        if (n <= 0 || (m = WideCharToMultiByte(CP_UTF8, 0, wide, n, utf8, sizeof utf8 - 1, NULL, NULL)) <= 0) continue;
        utf8[m] = 0;
        add_id(s, id, utf8);
    }
    FreeLibrary(exe);
    return 1;
}

static int count_newlines(const char* s) {
    int n = 0;
    for (; *s; s++) n += *s == '\n';
    return n;
}

/* The keys: every ID the localized releases translate (the game's own table and the IDs its DDF labels and tutorial
 * texts use), less the ones never translated. */
static void test_gamestrings(void) {
    static id_set_t keys, english, release, port;
    int have_english = english_strings(&english), total;
    memset(&keys, 0, sizeof keys);
    read_asset_strings("pt-BR", JPT_L10N_STRINGS_ASSET, &keys);
    read_asset_strings("fr", JPT_L10N_STRINGS_ASSET, &keys);
    read_asset_strings("es", JPT_L10N_STRINGS_ASSET, &keys);
    total = keys.count;
    CHECK(total == 228, "GameStrings keys: %d", total);
    if (have_english) {
        int in_exe = 0;
        for (int id = 0; id < MAX_IDS; id++)
            if (english.has[id]) {
                in_exe++;
                CHECK(keys.has[id], "English string %d has no key", id);
            }
        printf("GameStrings: %d English strings in trespass.exe, %d keys in all (the releases' DDF and tutorial IDs)\n",
               in_exe, total);
    }
    for (int l = 0; l < LANGUAGES; l++) {
        int from_release = 0, from_port = 0, missing = 0;
        memset(&release, 0, sizeof release);
        memset(&port, 0, sizeof port);
        read_asset_strings(k_languages[l], JPT_L10N_STRINGS_ASSET, &release);
        read_asset_strings(k_languages[l], JPT_L10N_STRINGS_PORT_ASSET, &port);
        for (int id = 0; id < MAX_IDS; id++) {
            const char* english_text = have_english && english.has[id] ? english.text[id] : keys.text[id];
            if (!keys.has[id] || jpt_l10n_id_fixed((unsigned)id)) continue;
            if (release.has[id]) from_release++;
            else if (port.has[id]) from_port++;
            else {
                missing++;
                CHECK(0, "%s: GameString %d missing", k_languages[l], id);
            }
            if (port.has[id] && english_text)
                CHECK(jpt_format_compatible(english_text, port.text[id]), "%s: port string %d changes printf conversions",
                      k_languages[l], id);
        }
        for (int id = 0; id < MAX_IDS; id++)
            if (port.has[id])
                CHECK(keys.has[id], "%s: port string %d is not a key", k_languages[l], id);
        printf("GameStrings %-5s: %d of %d (%s %d, port %d), missing %d, never translated: 10 (save file name)\n",
               k_languages[l], from_release + from_port, total - 1,
               !strcmp(k_languages[l], "ru") || !strcmp(k_languages[l], "it") ? "community" : "official", from_release,
               from_port, missing);
        free_ids(&release);
        free_ids(&port);
    }
    free_ids(&keys);
    free_ids(&english);
}

/* ---- voice captions */

typedef struct {
    char pack[16];
    uint32_t sample;
    int lines;
    uint32_t units[JPT_CAPTION_MAX_LINES];
    char tokens[256];
} sample_t;

typedef struct {
    sample_t* items;
    int count;
} samples_t;

static void add_sample(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units,
                       const char* const* lines) {
    samples_t* s = (samples_t*)ctx;
    sample_t* it;
    char all[4096] = "";
    if (s->count >= MAX_SAMPLES) return;
    it = &s->items[s->count++];
    memset(it, 0, sizeof *it);
    snprintf(it->pack, sizeof it->pack, "%s", pack);
    it->sample = sample;
    it->lines = count;
    for (int i = 0; i < count; i++) {
        it->units[i] = units[i];
        strncat(all, lines[i], sizeof all - strlen(all) - 2);
        strcat(all, " ");
    }
    jpt_tr_tokens(all, it->tokens, sizeof it->tokens);
}

static const sample_t* find_sample(const samples_t* s, const char* pack, uint32_t sample) {
    for (int i = 0; i < s->count; i++)
        if (s->items[i].sample == sample && !strcmp(s->items[i].pack, pack)) return &s->items[i];
    return NULL;
}

static int read_asset_samples(const char* language, const char* kind, samples_t* s) {
    const char* data;
    size_t n;
    return l10n_asset(language, kind, &data, &n) ? jpt_l10n_read_subtitles(data, n, add_sample, s) : 0;
}

typedef struct {
    const char* pack;
    samples_t* out;
} english_scan_t;

static void collect_units(void* ctx, uint32_t units, const char* text, size_t n) {
    sample_t* s = (sample_t*)ctx;
    (void)text;
    (void)n;
    if (s->lines < JPT_CAPTION_MAX_LINES) s->units[s->lines++] = units;
}

static void on_english(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    english_scan_t* e = (english_scan_t*)ctx;
    sample_t* it;
    if (e->out->count >= MAX_SAMPLES) return;
    it = &e->out->items[e->out->count++];
    memset(it, 0, sizeof *it);
    snprintf(it->pack, sizeof it->pack, "%s", e->pack);
    it->sample = sample;
    jpt_caption_lines(block, n, collect_units, it);
}

static int file_read(void* ctx, uint64_t offset, void* buf, size_t n) {
    FILE* f = (FILE*)ctx;
    return !_fseeki64(f, (long long)offset, SEEK_SET) && fread(buf, 1, n, f) == n;
}

/* The game's English captioned samples (Stream.tpa and Effects.tpa), when its files are there. */
static int english_samples(samples_t* out) {
    static const char* const files[2][2] = { { "Stream.tpa", "stream" }, { "Effects.tpa", "effects" } };
    for (int i = 0; i < 2; i++) {
        char path[MAX_PATH];
        FILE* f;
        long long size;
        english_scan_t e;
        jpt_tpa_stats_t st;
        snprintf(path, sizeof path, GAME_DIR "data/%s", files[i][0]);
        if (!(f = fopen(path, "rb"))) return 0;
        _fseeki64(f, 0, SEEK_END);
        size = _ftelli64(f);
        e.pack = files[i][1];
        e.out = out;
        jpt_tpa_captions(file_read, f, (uint64_t)size, on_english, &e, &st);
        fclose(f);
    }
    return out->count > 0;
}

/* The corrected English captions (en.subtitles.txt) replace the retail lines and times of the samples they hold and add the
 * samples the restored audio brings. */
static void merge_canonical_captions(samples_t* english) {
    samples_t canonical = { 0 };
    canonical.items = (sample_t*)calloc(MAX_SAMPLES, sizeof(sample_t));
    read_asset_samples(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &canonical);
    for (int i = 0; i < canonical.count; i++) {
        const sample_t* c = &canonical.items[i];
        sample_t* e = (sample_t*)find_sample(english, c->pack, c->sample);
        if (e) {
            e->lines = c->lines;
            memcpy(e->units, c->units, sizeof e->units);
        } else if (english->count < MAX_SAMPLES) {
            english->items[english->count++] = *c;
        }
    }
    free(canonical.items);
}

static void test_subtitles(void) {
    samples_t english = { 0 }, port[LANGUAGES], release = { 0 };
    int have_english;
    english.items = (sample_t*)calloc(MAX_SAMPLES, sizeof(sample_t));
    release.items = (sample_t*)calloc(MAX_SAMPLES, sizeof(sample_t));
    for (int l = 0; l < LANGUAGES; l++) {
        port[l].items = (sample_t*)calloc(MAX_SAMPLES, sizeof(sample_t));
        port[l].count = 0;
        read_asset_samples(k_languages[l], JPT_L10N_SUBTITLES_PORT_ASSET, &port[l]);
    }
    have_english = english_samples(&english);
    if (have_english) merge_canonical_captions(&english);
    if (!have_english) {
        for (int i = 0; i < port[0].count; i++) english.items[english.count++] = port[0].items[i];
        printf("captions: the game's TPAs are not in game\\data; the Brazilian Portuguese set stands for English\n");
    }
    CHECK(english.count == 486, "English captioned samples: %d", english.count);
    for (int l = 0; l < LANGUAGES; l++) {
        int from_release = 0, from_port = 0, missing = 0, no_english = 0;
        release.count = 0;
        read_asset_samples(k_languages[l], JPT_L10N_SUBTITLES_ASSET, &release);
        for (int i = 0; i < english.count; i++) {
            const sample_t *e = &english.items[i], *r = find_sample(&release, e->pack, e->sample),
                           *p = find_sample(&port[l], e->pack, e->sample);
            if (r) from_release++;
            else if (p) {
                from_port++;
                CHECK(p->lines == e->lines && !memcmp(p->units, e->units, sizeof(uint32_t) * (size_t)e->lines),
                      "%s %s %08x: the English sample's lines and times", k_languages[l], e->pack, e->sample);
                if (strcmp(k_languages[l], "it"))
                    CHECK(!strcmp(p->tokens, find_sample(&port[0], e->pack, e->sample)->tokens),
                          "%s %s %08x: control tokens %s differ from the other languages'", k_languages[l], e->pack,
                          e->sample, p->tokens);
            } else {
                missing++;
                CHECK(0, "%s %s %08x: no caption", k_languages[l], e->pack, e->sample);
            }
        }
        for (int i = 0; i < release.count; i++)
            no_english += !find_sample(&english, release.items[i].pack, release.items[i].sample);
        for (int i = 0; i < port[l].count; i++) {
            const sample_t* p = &port[l].items[i];
            no_english += !find_sample(&english, p->pack, p->sample) && !find_sample(&release, p->pack, p->sample);
        }
        printf("captions %-5s: %d of %d samples (%s %d, port %d), missing %d, %d with no English caption\n",
               k_languages[l], from_release + from_port, english.count,
               !strcmp(k_languages[l], "ru") || !strcmp(k_languages[l], "it") ? "community" : "release", from_release,
               from_port, missing, no_english);
    }
    {
        int with_tokens = 0;
        for (int i = 0; i < port[0].count; i++) with_tokens += port[0].items[i].tokens[0] != 0;
        CHECK(with_tokens >= 10, "tutorial samples with control tokens: %d", with_tokens);
    }
    for (int l = 0; l < LANGUAGES; l++) free(port[l].items);
    free(english.items);
    free(release.items);
}

/* ---- the interface table */

typedef struct {
    int count;
    char* key[MAX_KEYS];
    char* text[MAX_KEYS];
    char* flags[MAX_KEYS];
} table_t;

static void add_entry(void* ctx, const char* key, const char* text, const char* flags) {
    table_t* t = (table_t*)ctx;
    if (t->count >= MAX_KEYS) return;
    t->key[t->count] = _strdup(key);
    t->text[t->count] = _strdup(text);
    t->flags[t->count] = _strdup(flags);
    t->count++;
}

static void free_table(table_t* t) {
    for (int i = 0; i < t->count; i++) free(t->key[i]), free(t->text[i]), free(t->flags[i]);
    memset(t, 0, sizeof *t);
}

static int read_table(const char* language, table_t* t) {
    const char* data;
    size_t n;
    memset(t, 0, sizeof *t);
    return l10n_asset(language, JPT_TR_ASSET, &data, &n) ? jpt_tr_parse(data, n, add_entry, t) : -1;
}

static const char* table_text(const table_t* t, const char* key) {
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->key[i], key)) return t->text[i];
    return NULL;
}

/* "keep=de,es" or "keep=*": the languages whose text may stay the English one. */
static int may_keep(const char* flags, const char* language) {
    const char* list = strstr(flags, JPT_TR_KEEP "=");
    size_t n = strlen(language);
    if (!list) return 0;
    list += strlen(JPT_TR_KEEP) + 1;
    if (*list == '*') return 1;
    for (const char* p = list; (p = strstr(p, language)) != NULL; p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

/* A whole word, outside <TOKENS>. */
static int has_word(const char* text, const char* word) {
    size_t n = strlen(word);
    for (const char* p = text; *p; p++) {
        if (*p == '<' && strchr(p, '>')) {
            p = strchr(p, '>');
            continue;
        }
        if (_strnicmp(p, word, n)) continue;
        if ((p == text || !isalpha((unsigned char)p[-1])) && !isalpha((unsigned char)p[n])) return 1;
    }
    return 0;
}

/* English the translations must not contain: the words of the menus, the controls card and the panels. */
static const char* const k_english_words[] = {
    "New Game", "Load", "Options", "Exit", "Move", "Look", "Jump", "Objectives", "Objective", "Hint", "Resume",
    "Restart", "Save", "Quit", "Cancel", "Delete", "Close", "Crouch", "Throw", "Grab", "Rotate", "Wrist", "Stow",
    "Show", "Teleport", "Level", "Health", "Ammo", "Speed", "Height", "Freeze", "Coordinates", "Compass", "the", "you",
    "and", "with", "your", "failed", "unavailable", "Settings",
};

/* Words a translation may still have: the legal lines' names, and words the language spells the same way. */
static int allowed_english(const char* language, const char* key, const char* word) {
    static const struct {
        const char *language, *word;
    } shared[] = { { "de", "Level" }, { "de", "Teleport" }, { "fr", "Options" }, { "pt-BR", "Move" } };
    if (!strncmp(key, "Footer::", 8)) return 1;
    for (size_t i = 0; i < sizeof shared / sizeof shared[0]; i++)
        if (!strcmp(word, shared[i].word) && !strcmp(language, shared[i].language)) return 1;
    return 0;
}

static void check_codepage(const char* language, int codepage, const char* what, const char* text) {
    char out[4096];
    int before = 0, after = 0;
    CHECK(jpt_utf8_valid(text, strlen(text)), "%s %s: not UTF-8", language, what);
    if (jpt_utf8_to_cp(codepage, text, out, sizeof out) == (size_t)-1) {
        CHECK(0, "%s %s: not convertible", language, what);
        return;
    }
    for (const char* p = text; *p; p++) before += *p == '?';
    for (const char* p = out; *p; p++) after += *p == '?';
    CHECK(before == after, "%s %s: characters the code page %d lacks: %s", language, what, codepage, text);
}

/* The game's text, the subtitles and the panels draw with Arial (the overlay, Dear ImGui's merged fallback). */
typedef struct {
    HDC dc;
    HFONT font;
} glyph_probe_t;

static int has_glyphs(glyph_probe_t* g, const char* utf8) {
    WCHAR wide[4096];
    WORD index[4096];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 4096) - 1;
    if (n <= 0) return 1;
    if (GetGlyphIndicesW(g->dc, wide, n, index, GGI_MARK_NONEXISTING_GLYPHS) != (DWORD)n) return 0;
    for (int i = 0; i < n; i++)
        if (wide[i] > L' ' && index[i] == 0xFFFF) return 0;
    return 1;
}

static void test_interface(void) {
    table_t english, t;
    glyph_probe_t g;
    int count = read_table("en", &english);
    CHECK(count > 150, "en.interface.txt: %d keys", count);
    g.dc = CreateCompatibleDC(NULL);
    g.font = CreateFontA(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Arial");
    SelectObject(g.dc, g.font);
    for (int i = 0; i < english.count; i++) {
        CHECK(!strcmp(english.text[i], jpt_tr_english(english.key[i])), "en %s: its text is not the key's", english.key[i]);
        for (const char* p = english.key[i]; *p; p++) CHECK((unsigned char)*p < 0x80 || !strncmp(english.key[i], "Footer::", 8),
                                                           "en %s: keys are ASCII", english.key[i]);
    }
    for (int l = 0; l < LANGUAGES; l++) {
        const char* code = k_languages[l];
        int cp = jpt_l10n_language(code)->codepage, kept = 0;
        CHECK(read_table(code, &t) == english.count, "%s: %d entries, English has %d", code, t.count, english.count);
        for (int i = 0; i < t.count; i++) CHECK(table_text(&english, t.key[i]) != NULL, "%s: %s is not a key", code, t.key[i]);
        for (int i = 0; i < english.count; i++) {
            const char *key = english.key[i], *en = english.text[i], *text = table_text(&t, key);
            char a[256], b[256];
            if (!text || !*text) {
                CHECK(0, "%s: %s missing", code, key);
                continue;
            }
            jpt_tr_tokens(en, a, sizeof a);
            jpt_tr_tokens(text, b, sizeof b);
            CHECK(!strcmp(a, b), "%s %s: tokens %s instead of %s", code, key, b, a);
            CHECK(jpt_format_compatible(en, text), "%s %s: printf conversions differ", code, key);
            CHECK(count_newlines(en) == count_newlines(text), "%s %s: line breaks differ", code, key);
            if (!strcmp(text, en)) {
                kept++;
                CHECK(may_keep(english.flags[i], code), "%s %s: still English", code, key);
            }
            for (size_t w = 0; w < sizeof k_english_words / sizeof k_english_words[0]; w++)
                if (strcmp(text, en) && has_word(text, k_english_words[w]) && !allowed_english(code, key, k_english_words[w]))
                    CHECK(0, "%s %s: English \"%s\" in \"%s\"", code, key, k_english_words[w], text);
            check_codepage(code, cp, key, text);
            CHECK(has_glyphs(&g, text), "%s %s: Arial lacks a character of \"%s\"", code, key, text);
        }
        printf("interface %-5s: %d of %d keys, %d the same as English by design (names, symbols, words the language "
               "shares)\n", code, t.count, english.count, kept);
        free_table(&t);
    }
    DeleteObject(g.font);
    DeleteDC(g.dc);
    free_table(&english);
}

/* Every line of game text and every caption: in the code page, in Arial, and the port's own without English. */
typedef struct {
    const char* language;
    int codepage, port, lines, english;
    glyph_probe_t* glyphs;
} text_scan_t;

static const char* const k_english_sentence[] = { "the", "you", "and", "I'm", "it's", "what", "this", "there" };

static void scan_line(text_scan_t* s, const char* what, const char* text) {
    s->lines++;
    check_codepage(s->language, s->codepage, what, text);
    CHECK(has_glyphs(s->glyphs, text), "%s %s: Arial lacks a character of \"%s\"", s->language, what, text);
    if (!s->port) return;
    for (size_t w = 0; w < sizeof k_english_sentence / sizeof k_english_sentence[0]; w++)
        if (has_word(text, k_english_sentence[w]) && !strstr(text, "The Lost World")) {
            s->english++;
            CHECK(0, "%s %s: English \"%s\" in \"%s\"", s->language, what, k_english_sentence[w], text);
            break;
        }
}

static void scan_string(void* ctx, unsigned id, const char* utf8) {
    char what[32];
    snprintf(what, sizeof what, "string %u", id);
    if (id < 16000 || id >= 17000) scan_line((text_scan_t*)ctx, what, utf8);
}

static void scan_sample(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units,
                        const char* const* lines) {
    char what[48];
    (void)units;
    snprintf(what, sizeof what, "%s %08x", pack, sample);
    for (int i = 0; i < count; i++) scan_line((text_scan_t*)ctx, what, lines[i]);
}

static void test_all_lines(void) {
    glyph_probe_t g;
    g.dc = CreateCompatibleDC(NULL);
    g.font = CreateFontA(-32, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Arial");
    SelectObject(g.dc, g.font);
    for (int l = 0; l < LANGUAGES; l++) {
        static const char* const kinds[4] = { JPT_L10N_STRINGS_ASSET, JPT_L10N_STRINGS_PORT_ASSET, JPT_L10N_SUBTITLES_ASSET,
                                              JPT_L10N_SUBTITLES_PORT_ASSET };
        text_scan_t s;
        memset(&s, 0, sizeof s);
        s.language = k_languages[l];
        s.codepage = jpt_l10n_language(k_languages[l])->codepage;
        s.glyphs = &g;
        for (int k = 0; k < 4; k++) {
            const char* data;
            size_t n;
            if (!l10n_asset(k_languages[l], kinds[k], &data, &n)) continue;
            s.port = k == 1 || k == 3;
            if (k < 2) jpt_l10n_read_strings(data, n, scan_string, &s);
            else jpt_l10n_read_subtitles(data, n, scan_sample, &s);
        }
        printf("text %-5s: %d lines of game text and captions checked, %d with English left\n", s.language, s.lines, s.english);
    }
    DeleteObject(g.font);
    DeleteDC(g.dc);
}

static const char* lookup_from_set(void* ctx, unsigned id) {
    const id_set_t* s = (const id_set_t*)ctx;
    return id < MAX_IDS ? s->text[id] : NULL;
}

/* ---- the beach tutorial and the loading labels: the paths the game really draws them through */

/* Microsoft's SZDD (compress.exe) LZ, the format of the game's .GRF files. Returns the length, 0 when it is not one. */
static size_t szdd_expand(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    uint8_t window[4096];
    size_t size, o = 0, i = 14;
    int pos = 4096 - 16;
    if (n < 14 || memcmp(in, "SZDD", 4)) return 0;
    size = (size_t)in[10] | (size_t)in[11] << 8 | (size_t)in[12] << 16 | (size_t)in[13] << 24;
    if (size > cap) return 0;
    memset(window, ' ', sizeof window);
    while (i < n && o < size) {
        unsigned control = in[i++];
        for (int bit = 0; bit < 8 && i < n && o < size; bit++) {
            if (control & (1u << bit)) {
                out[o++] = window[pos] = in[i++];
                pos = (pos + 1) & 4095;
            } else {
                unsigned lo = in[i], hi = in[i + 1], from = lo | ((hi & 0xF0u) << 4), len = (hi & 0x0Fu) + 3;
                i += 2;
                for (unsigned k = 0; k < len && o < size; k++) {
                    out[o++] = window[pos] = window[(from + k) & 4095];
                    pos = (pos + 1) & 4095;
                }
            }
        }
    }
    return o;
}

/* Sentences of a level's text: zero-terminated ASCII strings that start with a capital, hold a space and end like a
 * sentence (the engine's names and resource labels do not). */
static int is_sentence(const uint8_t* s, size_t n) {
    int spaces = 0;
    if (n < 15 || s[0] < 'A' || s[0] > 'Z') return 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < ' ' || s[i] > '~') return 0;
        spaces += s[i] == ' ';
    }
    return spaces >= 2 && (s[n - 1] == '.' || s[n - 1] == '!' || s[n - 1] == '?') && strncmp((const char*)s, "Terrain", 7);
}

static int tutorial_listed(const char* sentence) { return jpt_tutorial_key(sentence) != NULL; }

/* Every sentence in the game's own beach level is a line the port translates, and every line the port translates is in
 * the level: a tutorial line the table misses would be drawn in English. */
static void test_tutorial_against_game(void) {
    FILE* f = fopen(GAME_DIR "data/be.GRF", "rb");
    uint8_t *packed, *level;
    size_t n, size;
    int found = 0, covered[JPT_L10N_TUTORIAL_COUNT] = { 0 };
    if (!f) {
        printf("tutorial: game\\data\\be.GRF not found, the lines are checked against the table only\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    packed = (uint8_t*)malloc(n);
    level = (uint8_t*)malloc(16u << 20);
    if (!packed || !level || fread(packed, 1, n, f) != n) {
        fclose(f);
        free(packed);
        free(level);
        return;
    }
    fclose(f);
    size = szdd_expand(packed, n, level, 16u << 20);
    CHECK(size > 1000000, "be.GRF expands to %u bytes", (unsigned)size);
    for (size_t i = 1; i < size; i++) {
        size_t j = i;
        if (level[i - 1] != 0 || !level[i]) continue;
        while (j < size && level[j]) j++;
        if (j < size && is_sentence(level + i, j - i)) {
            const char* text = (const char*)level + i;
            found++;
            CHECK(tutorial_listed(text), "a sentence of the beach level has no translation key: \"%s\"", text);
            for (int t = 0; t < JPT_L10N_TUTORIAL_COUNT; t++) {
                const char* key = jpt_tutorial_key(text);
                covered[t] |= key && !strcmp(key, jpt_tutorial_key_at(t));
            }
        }
        i = j;
    }
    for (int t = 0; t < JPT_L10N_TUTORIAL_COUNT; t++) CHECK(covered[t], "tutorial line %d never occurs in the level", t);
    printf("tutorial: %d sentences in be.GRF, %d lines in the table\n", found, JPT_L10N_TUTORIAL_COUNT);
    free(packed);
    free(level);
}

static void test_tutorial(void) {
    table_t english, t;
    int count = read_table("en", &english);
    CHECK(count > 150, "en.interface.txt");
    for (int i = 0; i < JPT_L10N_TUTORIAL_COUNT; i++) {
        const char *raw = jpt_tutorial_english(i), *key = jpt_tutorial_key_at(i);
        char padded[256];
        CHECK(raw && key && !strncmp(key, "Tutorial::", 10) && !strcmp(jpt_tutorial_key(raw), key), "tutorial line %d", i);
        snprintf(padded, sizeof padded, "  %s\r\n", raw);
        CHECK(!strcmp(jpt_tutorial_key(padded) ? jpt_tutorial_key(padded) : "", key), "tutorial line %d ignores blanks around it", i);
        CHECK(table_text(&english, key) != NULL, "%s: not in en.interface.txt", key);
    }
    CHECK(!jpt_tutorial_key("Welcome to Trespasser") && !jpt_tutorial_key("") && !jpt_tutorial_key(NULL),
          "only the game's lines have keys");
    for (int l = 0; l < LANGUAGES; l++) {
        const char* code = k_languages[l];
        int same = 0;
        read_table(code, &t);
        for (int i = 0; i < JPT_L10N_TUTORIAL_COUNT; i++) {
            const char *key = jpt_tutorial_key_at(i), *en = table_text(&english, key), *text = table_text(&t, key);
            char a[256], b[256];
            CHECK(text && *text, "%s %s: missing", code, key);
            if (!text) continue;
            jpt_tr_tokens(en, a, sizeof a);
            jpt_tr_tokens(text, b, sizeof b);
            CHECK(!strcmp(a, b) || (!strcmp(code, "ru") && !strcmp(a, b)), "%s %s: tokens \"%s\" instead of \"%s\"", code, key, b, a);
            CHECK(!strstr(text, "'Q'") && !strstr(text, "'W'") && !strstr(text, "mouse button") && !strstr(text, " F1"),
                  "%s %s: a fixed key where a token goes: %s", code, key, text);
            for (const char* p = text; *p; p++)
                if (*p == '<') {
                    const char* close = strchr(p, '>');
                    CHECK(close && (p == text || p[-1] == ' ' || p[-1] == '(') && (close[1] == 0 || strchr(" .,;:!?)", close[1])),
                          "%s %s: a token must stand between blanks or punctuation: %s", code, key, text);
                    if (!close) break;
                    p = close;
                }
            same += !strcmp(text, en);
            CHECK(jpt_prompt_text_has_prompts(text) == (strchr(text, '<') != NULL),
                  "%s %s: the English phrase rules must not fire on a translation: %s", code, key, text);
        }
        CHECK(same == 0, "%s: %d tutorial lines still English", code, same);
        free_table(&t);
    }
    free_table(&english);
}

static uint32_t advance_of(void* ctx, unsigned char c) {
    (void)ctx;
    return c == JPT_PROMPT_MARK ? 28u : 8u;
}

/* The tutorial lines on the game's top text band: the overlay wraps by its own rule on a 640 px raster (a token is a
 * glyph about 3.5 letters wide), and a line must stay within three lines in every language. */
static void test_tutorial_fit(void) {
    const tl_view_t view = { 640, 480, 0, 0, 18 };
    table_t t;
    int widest = 0;
    for (int l = 0; l < LANGUAGES; l++) {
        const jpt_l10n_language_t* lang = jpt_l10n_language(k_languages[l]);
        read_table(k_languages[l], &t);
        for (int i = 0; i < JPT_L10N_TUTORIAL_COUNT; i++) {
            const char* text = table_text(&t, jpt_tutorial_key_at(i));
            char marked[1024], cp[1024];
            static uint8_t blob[8192];
            uint32_t lines;
            size_t o = 0;
            if (!text || jpt_utf8_to_cp(lang->codepage, text, cp, sizeof cp) == (size_t)-1) continue;
            for (const char* p = cp; *p && o + 1 < sizeof marked; p++) {
                if (*p == '<' && strchr(p, '>')) {
                    marked[o++] = JPT_PROMPT_MARK;
                    p = strchr(p, '>');
                } else {
                    marked[o++] = *p;
                }
            }
            marked[o] = 0;
            CHECK(tl_layout(marked, TL_ALIGN_CENTRE, &view, advance_of, NULL, blob, sizeof blob) > 0, "%s %d: layout", k_languages[l], i);
            memcpy(&lines, blob + 4, 4);
            if ((int)lines > widest) widest = (int)lines;
            CHECK(lines <= 3, "%s: tutorial line %d needs %u lines: %s", k_languages[l], i, lines, text);
        }
        free_table(&t);
    }
    printf("tutorial fit: the longest line takes %d lines of the 640 px band\n", widest);
}

static const char* const k_visible[] = { "Welcome to Trespasser.", "To look around, move <LOOK>." };

/* The four texts of the capture, in every language, and loader.ddf as the game opens it: its labels are replaced. */
static void test_captured_texts(void) {
    static char ddf[8192];
    FILE* f = fopen(GAME_DIR "data/menu/loader.ddf", "rb");
    size_t ddf_n = f ? fread(ddf, 1, sizeof ddf - 1, f) : 0;
    if (f) fclose(f);
    for (int l = 0; l < LANGUAGES; l++) {
        const char* code = k_languages[l];
        table_t t;
        id_set_t* strings = (id_set_t*)calloc(1, sizeof *strings);
        char* out = NULL;
        size_t out_n = 0;
        read_table(code, &t);
        for (size_t i = 0; i < sizeof k_visible / sizeof k_visible[0]; i++) {
            char key[128];
            const char* text;
            snprintf(key, sizeof key, "Tutorial::%s", k_visible[i]);
            text = table_text(&t, key);
            CHECK(text && strcmp(text, k_visible[i]), "%s: \"%s\" is still English", code, k_visible[i]);
        }
        read_asset_strings(code, JPT_L10N_STRINGS_ASSET, strings);
        read_asset_strings(code, JPT_L10N_STRINGS_PORT_ASSET, strings);
        CHECK(strings->text[10010] && strings->text[10011] && strings->text[10012], "%s: loader labels 10010-10012", code);
        for (unsigned id = 10010; id <= 10012 && strings->text[id]; id++)
            CHECK(strcmp(strings->text[id], id == 10010 ? "World Initialization" : "Level File Copy"), "%s: string %u is English", code, id);
        if (ddf_n) {
            ddf[ddf_n] = 0;
            CHECK(jpt_ddf_translate(ddf, ddf_n, lookup_from_set, strings, &out, &out_n) == 2, "%s: loader.ddf labels replaced", code);
            CHECK(out && !strstr(out, "\"World Initialization\" ; Initial Text") && !strstr(out, "\"Level File Copy\" ; Initial Text"),
                  "%s: loader.ddf still has an English label", code);
            free(out);
        }
        free_ids(strings);
        free(strings);
        free_table(&t);
    }
    if (!ddf_n) printf("loader.ddf: game files not found, the labels are checked in the string table only\n");
}

/* The sources that draw the game's text read it through the translation layer. A literal English text of the interface
 * table in one of these files, outside a jpt_tr call, is a path that bypasses it; the files that hold the translated
 * tables are exercised by menu_l10n_tests and the frontend tests instead. */
static char* read_source(const char* name, size_t* n) {
    char path[MAX_PATH];
    FILE* f;
    char* data = NULL;
    snprintf(path, sizeof path, SOURCE_DIR "%s", name);
    f = fopen(path, "rb");
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc(*n + 1);
    if (data && fread(data, 1, *n, f) == *n) data[*n] = 0;
    else {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static int contains_call(const char* source, const char* name, const char* needle) {
    const char* at = strstr(source, name);
    return at && (!needle || strstr(at, needle));
}

static void test_call_sites(void) {
    static const struct {
        const char* file;
        const char* function;
        const char* calls;
    } wired[] = {
        { "src/host/l10n.c", "static void text_action(void)", "tutorial_text(" },
        { "src/host/l10n.c", "static const char* tutorial_text(", "jpt_tutorial_key(" },
        { "src/host/l10n.c", "static const char* tutorial_text(", "jpt_tr_cp(" },
        { "src/host/loading_hd.c", "static void read_caption(", "l10n_label(" },
        { "src/host/frontend_hd.c", "static int add_textbox(", "l10n_label(" },
        { "src/host/frontend_hd.c", "frontend_hd_prompts(", "jpt_tr_cp(\"Select\")" },
        { "src/host/tutorial_prompts.c", "static int panel_pass(", "jpt_tr_cp(card->title)" },
        { "src/host/io_trace.c", "void shim_CreateFileA(void)", "l10n_menu_file(" },
        { "src/host/io_trace.c", "void shim_LoadLibraryA(void)", "host_unavailable_library(" },
    };
    static const char* const guarded[] = { "src/host/frontend_hd.c",  "src/host/frontend_assets.c", "src/host/frontend_layout.c",
                                           "src/host/loading_hd.c",   "src/host/loading_layout.c",  "src/host/tutorial_prompts.c",
                                           "src/host/overlay.c",      "src/host/video_test.c",      "src/host/video_test_panel.cpp",
                                           "src/host/objectives.c",   "src/host/cheats.c",          "src/host/gamepad_menu.c" };
    static const char* const probe_free[] = { "src/host/io_trace.c", "src/host/l10n.c" };
    table_t english;
    for (size_t i = 0; i < 2; i++) {
        size_t n;
        char* source = read_source(probe_free[i], &n);
        CHECK(source != NULL, "%s: cannot read", probe_free[i]);
        if (!source) continue;
        CHECK(!strstr(source, "opened with access"), "%s logs the guest's probe path", probe_free[i]);
        CHECK(!strstr(source, "saves\\\\menu"), "%s names saves\\menu", probe_free[i]);
        free(source);
    }
    read_table("en", &english);
    for (size_t i = 0; i < sizeof wired / sizeof wired[0]; i++) {
        size_t n;
        char* source = read_source(wired[i].file, &n);
        CHECK(source != NULL, "%s: cannot read", wired[i].file);
        if (!source) continue;
        CHECK(contains_call(source, wired[i].function, wired[i].calls), "%s: %s no longer calls %s", wired[i].file,
              wired[i].function, wired[i].calls);
        free(source);
    }
    for (size_t i = 0; i < sizeof guarded / sizeof guarded[0]; i++) {
        size_t n;
        char* source = read_source(guarded[i], &n);
        if (!source) {
            CHECK(0, "%s: cannot read", guarded[i]);
            continue;
        }
        for (int k = 0; k < english.count; k++) {
            char literal[512];
            const char* at = source;
            const char* text = jpt_tr_english(english.key[k]);
            if (strlen(text) < 6 || strchr(text, '\n') || strchr(text, '%')) continue;
            snprintf(literal, sizeof literal, "\"%s\"", text);
            while ((at = strstr(at, literal)) != NULL) {
                const char* line = at;
                const char* end = strchr(at, '\n');
                char row[1024];
                while (line > source && line[-1] != '\n') line--;
                snprintf(row, sizeof row, "%.*s", (int)((end ? end : at + strlen(at)) - line), line);
                CHECK(strstr(row, "jpt_tr") || strstr(row, "host_log") || strstr(row, "note(") || strstr(row, "refuse("),
                      "%s: English text drawn without jpt_tr: %s", guarded[i], row);
                at += strlen(literal);
            }
        }
        free(source);
    }
    free_table(&english);
}

/* The menus' pictures, the controls card and the F1 tutorial line exist in every language. */
static void test_named_labels(void) {
    static const struct {
        const char* code;
        const char* text;
    } k_subtitle_labels[] = { { "en", "Subtitles" }, { "pt-BR", "Legendas" }, { "es", "Subt\xC3\xAD" "tulos" },
                              { "ru", "\xD0\xA1\xD1\x83\xD0\xB1\xD1\x82\xD0\xB8\xD1\x82\xD1\x80\xD1\x8B" } };
    static const char* const menu[] = { "Menu::NEW GAME", "Menu::LOAD", "Menu::OPTIONS", "Menu::EXIT", "Menu::RESUME",
                                        "Menu::RESTART LEVEL", "Menu::SAVE", "Menu::QUIT", "Menu::CANCEL",
                                        "Menu::DESKTOP", "Menu::MAIN MENU", "Menu::YES", "Menu::NO", "Menu::OK",
                                        "Title::QUIT", "Title::SOUND",
                                        "Tutorial::You can always press <HINT> for a hint about what to do next." };
    for (int l = 0; l < LANGUAGES; l++) {
        const char* data;
        size_t n;
        const jpt_l10n_language_t* lang = jpt_l10n_language(k_languages[l]);
        CHECK(l10n_asset(k_languages[l], JPT_TR_ASSET, &data, &n) && jpt_tr_load(data, n, lang->codepage) > 150,
              "%s: interface loads", k_languages[l]);
        for (size_t i = 0; i < sizeof menu / sizeof menu[0]; i++) CHECK(jpt_tr_has(menu[i]), "%s: %s", k_languages[l], menu[i]);
        CHECK(jpt_tr_has("Audio::Show Subtitles"), "%s: the Audio menu's subtitle row", k_languages[l]);
        for (size_t i = 0; i < sizeof k_subtitle_labels / sizeof k_subtitle_labels[0]; i++)
            if (!strcmp(k_subtitle_labels[i].code, k_languages[l]))
                CHECK(!strcmp(jpt_tr("Audio::Show Subtitles"), k_subtitle_labels[i].text), "%s: subtitle row label \"%s\"",
                      k_languages[l], jpt_tr("Audio::Show Subtitles"));
        for (int pad = 0; pad < 2; pad++) {
            const jpt_card_t* card = jpt_prompt_card(pad);
            CHECK(jpt_tr_has(card->title) && jpt_tr_has(jpt_prompt_hint_card(pad)->title), "%s: card title", k_languages[l]);
            for (int r = 0; r < card->count; r++) {
                CHECK(jpt_tr_has(card->rows[r].action), "%s: card row %s", k_languages[l], card->rows[r].action);
                CHECK(strcmp(jpt_tr(card->rows[r].action), card->rows[r].action) || !strcmp(card->rows[r].action, "Menu") ||
                          (!strcmp(card->rows[r].action, "Cheats") && !strcmp(k_languages[l], "de")),
                      "%s: card row %s is English", k_languages[l], card->rows[r].action);
            }
        }
    }
    jpt_tr_reset();
    CHECK(!strcmp(jpt_tr("Menu::NEW GAME"), "NEW GAME") && !strcmp(jpt_tr("Move"), "Move") && !jpt_tr_has("Move"),
          "English: the key's own text");
}

/* ---- the game's menu files, for --inventory */

static void inventory(void) {
    static const char* const ddfs[] = { "mainscreen", "ingameopt", "quit", "options", "options2", "audio", "controls",
                                        "loadgame", "savegame", "yesno", "okdlg", "hint", "directload", "render",
                                        "newgame", "loader" };
    printf("menu inventory (%sdata\\menu):\n", GAME_DIR);
    for (size_t i = 0; i < sizeof ddfs / sizeof ddfs[0]; i++) {
        char path[MAX_PATH], line[512];
        FILE* f;
        int pictures = 0, labels = 0, translated = 0;
        snprintf(path, sizeof path, GAME_DIR "data/menu/%s.ddf", ddfs[i]);
        if (!(f = fopen(path, "rb"))) continue;
        while (fgets(line, sizeof line, f)) {
            char* q = strchr(line, '"');
            if (strstr(line, ".tga\"") || strstr(line, ".TGA\"")) pictures++;
            if (q && q[1] != '"' && strstr(line, "Initial Text")) {
                char english[128];
                char* e = strchr(q + 1, '"');
                if (!e || e - q - 1 <= 0 || (size_t)(e - q - 1) >= sizeof english) continue;
                memcpy(english, q + 1, (size_t)(e - q - 1));
                english[e - q - 1] = 0;
                labels++;
                translated += jpt_ddf_text_id(english) != 0;
            }
        }
        fclose(f);
        printf("  %-11s %2d pictures, %2d text labels (%d by GameStrings ID)\n", ddfs[i], pictures, labels, translated);
    }
}

void l10n_coverage_tests(int* checks, int* failures, int list_inventory) {
    g_checks = checks;
    g_failures = failures;
    test_gamestrings();
    test_subtitles();
    test_interface();
    test_all_lines();
    test_tutorial();
    test_tutorial_against_game();
    test_tutorial_fit();
    test_captured_texts();
    test_call_sites();
    test_named_labels();
    if (list_inventory) inventory();
}
