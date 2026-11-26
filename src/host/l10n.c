/*
 * The embedded translations. String IDs go through the LoadStringA import (hints are 18000 + the hint, 0x0040E820);
 * voice captions are swapped where 0x004395A0 hands a sample's caption block to the text overlay, the block being
 * recognised by its English captions (read from the game's own TPAs); menu labels come from translated copies of the
 * DDF files; the port's own text comes from the interface table (jpt_tr.h). A release's text is loaded first and the
 * port's completion files only add what it lacks (captions: they replace the release's lines with the corrected timing).
 * Audio is never translated: the English voices keep playing, and stream_audio.c serves the TPA they come from.
 */
#include "guest_va.h"
#include "l10n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_menu_panel.h"
#include "jpt_l10n.h"
#include "jpt_string_table.h"
#include "jpt_tr.h"
#include "jpt_user_data.h"
#include "l10n_assets.h"
#include "level_actions.h"
#ifdef _WIN32
#include "stream_audio.h"
#else
#include "stream_vfile.h"
#endif

#ifdef _WIN32
#define SEP "\\"
#define ATOMIC_SET(p, v) InterlockedExchange((volatile LONG*)(p), (v))
#define ATOMIC_GET(p) InterlockedCompareExchange((volatile LONG*)(p), 0, 0)
#else
#define MAX_PATH 512
#define LF_FACESIZE 32
#define SEP "/"
#define ATOMIC_SET(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define ATOMIC_GET(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#endif

#define CAPTIONS_VA GVA(0x004395A0)
#define TEXT_ACTION_VA GVA(0x00472B60)
#define CAPTION_STATE 0x20u        /* the sample's caption state: shown flag at +0, block at +4 */
#define TEXT_ACTION_TEXT 0x14u
#define TEXT_ACTION_ID 0x18u
#define STRING_PROBE 2048
#define FP_SLOTS 4096u
#define MENU_FILES 32
#define LOG_LIMIT 6
#define MENU_CACHE "menu-text"

typedef struct {
    unsigned id;
    char* utf8;
    char* display;
    int checked; /* 0 not yet, 1 usable, -1 its printf conversions differ from the English text's */
} entry_t;

typedef struct {
    char pack[16];
    uint32_t sample;
    uint8_t* block;
    size_t size;
} caption_t;

typedef struct {
    char name[64];
    int translated;
} menu_file_t;

static struct {
    int codepage;
    char text_code[JPT_L10N_CODE_MAX], subtitle_code[JPT_L10N_CODE_MAX];
    int release_language;
    int release_strings, port_strings, release_samples, port_samples, interface_count;
    char cache_dir[MAX_PATH], game_dir[MAX_PATH], game_host[MAX_PATH], user_dir[MAX_PATH];
    entry_t* strings;
    int string_count, string_cap;
    caption_t* captions;
    int caption_count, caption_cap;
    uint64_t fp_key[FP_SLOTS];
    int fp_value[FP_SLOTS];
    volatile int captions_ready;
    int hits_logged, misses_logged, truncations_logged, text_hits_logged, text_misses_logged, unreadable_logged;
    menu_file_t menus[MENU_FILES];
    int menu_count;
    char release_face[LF_FACESIZE];
} L;

static recomp_func_t s_captions_lifted, s_text_action_lifted;

static char* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    long size;
    char* data = NULL;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) &&
        (data = (char*)malloc((size_t)size + 1)) != NULL) {
        *n = fread(data, 1, (size_t)size, f);
        data[*n] = 0;
    }
    fclose(f);
    return data;
}

static char* to_display(const char* utf8) {
    size_t cap = strlen(utf8) + 1;
    char* out = (char*)malloc(cap);
    if (out && jpt_utf8_to_cp(L.codepage, utf8, out, cap) == (size_t)-1) {
        free(out);
        out = NULL;
    }
    return out;
}

static entry_t* find_entry(unsigned id);

static int ascii_only(const char* s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

/* ctx is non-NULL for the port's completion: an ID the release already has keeps the release's text, unless the
 * release left it in English inside a Cyrillic translation. */
static void add_string(void* ctx, unsigned id, const char* utf8) {
    entry_t* e = ctx ? find_entry(id) : NULL;
    if (jpt_l10n_id_fixed(id)) return;
    if (e) {
        char* copy;
        if (L.codepage != 1251 || !ascii_only(e->utf8) || ascii_only(utf8) || !(copy = _strdup(utf8))) return;
        free(e->utf8);
        e->utf8 = copy;
        return;
    }
    if (L.string_count == L.string_cap) {
        int cap = L.string_cap ? L.string_cap * 2 : 256;
        entry_t* grown = (entry_t*)realloc(L.strings, (size_t)cap * sizeof *grown);
        if (!grown) return;
        L.strings = grown;
        L.string_cap = cap;
    }
    L.strings[L.string_count].id = id;
    L.strings[L.string_count].utf8 = _strdup(utf8);
    L.strings[L.string_count].display = NULL;
    L.strings[L.string_count].checked = 0;
    if (L.strings[L.string_count].utf8) L.string_count++;
}

static int by_id(const void* a, const void* b) {
    unsigned x = ((const entry_t*)a)->id, y = ((const entry_t*)b)->id;
    return x < y ? -1 : x > y;
}

static entry_t* find_entry(unsigned id) {
    entry_t key;
    key.id = id;
    return L.release_strings ? (entry_t*)bsearch(&key, L.strings, (size_t)L.release_strings, sizeof key, by_id) : NULL;
}

static int find_caption(const char* pack, uint32_t sample);

/* ctx is non-NULL for the port's completion: a sample the release already captions takes the completion's lines, which
 * carry the corrected timing. */
static void add_caption(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units,
                        const char* const* lines) {
    char* converted[JPT_CAPTION_MAX_LINES] = { NULL };
    uint8_t block[0x2000];
    size_t size;
    int ok = 1, existing = ctx ? find_caption(pack, sample) : -1;
    for (int i = 0; i < count; i++) ok = (converted[i] = ok ? to_display(lines[i]) : NULL) != NULL && ok;
    size = ok ? jpt_caption_build(count, units, (const char* const*)converted, block, sizeof block) : 0;
    for (int i = 0; i < count; i++) free(converted[i]);
    if (!size) return;
    if (existing >= 0) {
        uint8_t* replaced = (uint8_t*)malloc(size);
        if (!replaced) return;
        memcpy(replaced, block, size);
        free(L.captions[existing].block);
        L.captions[existing].block = replaced;
        L.captions[existing].size = size;
        return;
    }
    if (L.caption_count == L.caption_cap) {
        int cap = L.caption_cap ? L.caption_cap * 2 : 256;
        caption_t* grown = (caption_t*)realloc(L.captions, (size_t)cap * sizeof *grown);
        if (!grown) return;
        L.captions = grown;
        L.caption_cap = cap;
    }
    if (!(L.captions[L.caption_count].block = (uint8_t*)malloc(size))) return;
    memcpy(L.captions[L.caption_count].block, block, size);
    L.captions[L.caption_count].size = size;
    L.captions[L.caption_count].sample = sample;
    snprintf(L.captions[L.caption_count].pack, sizeof L.captions[0].pack, "%s", pack);
    L.caption_count++;
}

static int find_caption(const char* pack, uint32_t sample) {
    for (int i = 0; i < L.caption_count; i++)
        if (L.captions[i].sample == sample && !strcmp(L.captions[i].pack, pack)) return i;
    return -1;
}

static int fp_slot(uint64_t fp) {
    uint32_t at = (uint32_t)(fp ^ (fp >> 32)) & (FP_SLOTS - 1);
    for (uint32_t probe = 0; probe < FP_SLOTS; probe++, at = (at + 1) & (FP_SLOTS - 1))
        if (L.fp_value[at] < 0 || L.fp_key[at] == fp) return (int)at;
    return -1;
}

typedef struct {
    const char* pack;
    int matched, english_only;
} scan_t;

static void on_english_caption(void* ctx, uint32_t sample, const uint8_t* block, size_t n) {
    scan_t* s = (scan_t*)ctx;
    int index = find_caption(s->pack, sample), slot;
    uint64_t fp = jpt_caption_fingerprint(block, n);
    if (index < 0) {
        s->english_only++;
        return;
    }
    if ((slot = fp_slot(fp)) < 0 || L.fp_value[slot] >= 0) return;
    L.fp_key[slot] = fp;
    L.fp_value[slot] = index;
    s->matched++;
}

#ifdef _WIN32
static int read_handle(void* ctx, uint64_t offset, void* buf, size_t n) {
    LARGE_INTEGER at;
    DWORD got = 0;
    at.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx((HANDLE)ctx, at, NULL, FILE_BEGIN) && ReadFile((HANDLE)ctx, buf, (DWORD)n, &got, NULL) &&
           got == n;
}

#endif

static int read_pack(void* ctx, uint64_t offset, void* buf, size_t n) {
    return stream_pack_read((const stream_pack_t*)ctx, offset, buf, n) == (long)n;
}

#ifdef _WIN32
/* The game looks for <game>\X before <game>\data\X. */
static HANDLE open_game_file(const char* name, char* path, size_t cap) {
    static const char* const dirs[] = { "", "data\\" };
    for (int i = 0; i < 2; i++) {
        HANDLE h;
        snprintf(path, cap, "%s\\%s%s", L.game_dir, dirs[i], name);
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return INVALID_HANDLE_VALUE;
}

static DWORD WINAPI scan_english_captions(LPVOID unused) {
    static const char* const files[][2] = {
        { "Stream.tpa", "stream" }, { "Effects.tpa", "effects" }, { "Ambient.tpa", "ambient" }, { "Menu.tpa", "menu" },
    };
    scan_t s;
    (void)unused;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 4; i++) {
        char path[MAX_PATH];
        LARGE_INTEGER size;
        jpt_tpa_stats_t st;
        stream_pack_t* view;
        HANDLE h = open_game_file(files[i][0], path, sizeof path);
        if (h == INVALID_HANDLE_VALUE) continue;
        s.pack = files[i][1];
        view = i == 0 && GetFileSizeEx(h, &size) ? stream_audio_open_view(h, (uint64_t)size.QuadPart) : NULL;
        if (!GetFileSizeEx(h, &size) ||
            !(view ? jpt_tpa_captions(read_pack, view, stream_pack_size(view), on_english_caption, &s, &st)
                   : jpt_tpa_captions(read_handle, h, (uint64_t)size.QuadPart, on_english_caption, &s, &st)))
            host_log("l10n", "%s is not a readable TPA: its captions stay English", path);
        else if (st.captioned)
            host_log("l10n", "%s: %d captioned samples of %d%s", path, st.captioned, st.samples,
                     st.malformed ? " (some malformed)" : "");
        stream_pack_close(view);
        CloseHandle(h);
    }
    host_log("l10n", "subtitles: %d English caption blocks translated, %d without a translation (English kept)",
             s.matched, s.english_only);
    ATOMIC_SET(&L.captions_ready, 1);
    return 0;
}
#else
static int read_stdio(void* ctx, uint64_t offset, void* buf, size_t n) {
    FILE* f = (FILE*)ctx;
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* The game looks for <game>/X before <game>/data/X. */
static FILE* open_game_file(const char* name, char* path, size_t cap) {
    static const char* const dirs[] = { "", "data/" };
    for (int i = 0; i < 2; i++) {
        FILE* f;
        snprintf(path, cap, "%s/%s%s", L.game_host, dirs[i], name);
        f = fopen(path, "rb");
        if (f) return f;
    }
    return NULL;
}

/* Windows scans on a thread so the start-up is not held; here the scan is the start-up's own (a few reads of each TPA's directory). */
static void scan_english_captions(void) {
    static const char* const files[][2] = {
        { "Stream.tpa", "stream" }, { "Effects.tpa", "effects" }, { "Ambient.tpa", "ambient" }, { "Menu.tpa", "menu" },
    };
    scan_t s;
    memset(&s, 0, sizeof s);
    stream_catalog_init(NULL);
    for (int i = 0; i < 4; i++) {
        char path[2 * MAX_PATH];
        jpt_tpa_stats_t st;
        long size;
        stream_pack_t* view;
        FILE* f = open_game_file(files[i][0], path, sizeof path);
        if (!f) continue;
        s.pack = files[i][1];
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        view = i == 0 && size > 0 ? stream_catalog_open_pack(read_stdio, f, (uint64_t)size) : NULL;
        if (size <= 0 ||
            !(view ? jpt_tpa_captions(read_pack, view, stream_pack_size(view), on_english_caption, &s, &st)
                   : jpt_tpa_captions(read_stdio, f, (uint64_t)size, on_english_caption, &s, &st)))
            host_log("l10n", "%s is not a readable TPA: its captions stay English", path);
        else if (st.captioned)
            host_log("l10n", "%s: %d captioned samples of %d%s", path, st.captioned, st.samples,
                     st.malformed ? " (some malformed)" : "");
        stream_pack_close(view);
        fclose(f);
    }
    host_log("l10n", "subtitles: %d English caption blocks translated, %d without a translation (English kept)",
             s.matched, s.english_only);
    ATOMIC_SET(&L.captions_ready, 1);
}
#endif

static void load_strings(int release_text_loaded) {
    static int port = 1;
    const char* data;
    size_t n;
    if (!release_text_loaded && l10n_asset(L.text_code, JPT_L10N_STRINGS_ASSET, &data, &n))
        jpt_l10n_read_strings(data, n, add_string, NULL);
    qsort(L.strings, (size_t)L.string_count, sizeof *L.strings, by_id);
    L.release_strings = L.string_count;
    if (l10n_asset(L.text_code, JPT_L10N_STRINGS_PORT_ASSET, &data, &n)) jpt_l10n_read_strings(data, n, add_string, &port);
    L.port_strings = L.string_count - L.release_strings;
    qsort(L.strings, (size_t)L.string_count, sizeof *L.strings, by_id);
    L.release_strings = L.string_count;
    if (!L.string_count) host_log("l10n", "no game text for %s: English is used", L.text_code);
}

static void load_subtitles(void) {
    static int port = 1;
    const char* data;
    size_t n;
    if (l10n_asset(L.subtitle_code, JPT_L10N_SUBTITLES_ASSET, &data, &n)) jpt_l10n_read_subtitles(data, n, add_caption, NULL);
    L.release_samples = L.caption_count;
    if (l10n_asset(L.subtitle_code, JPT_L10N_SUBTITLES_PORT_ASSET, &data, &n))
        jpt_l10n_read_subtitles(data, n, add_caption, &port);
    L.port_samples = L.caption_count - L.release_samples;
    if (!L.caption_count) host_log("l10n", "no subtitles for %s: English is used", L.subtitle_code);
}

static void load_interface(void) {
    const char* data;
    size_t n;
    if (l10n_asset(L.text_code, JPT_TR_ASSET, &data, &n)) L.interface_count = jpt_tr_load(data, n, L.codepage);
    if (L.interface_count <= 0) host_log("l10n", "no interface text for %s: English is used", L.text_code);
}

typedef struct {
    int codepage, count;
} release_load_t;

static void add_release_string(void* user, uint32_t id, const char* bytes, uint32_t length) {
    release_load_t* load = (release_load_t*)user;
    char utf8[STRING_PROBE * 3];
    if (!length || length >= STRING_PROBE) return;
    if (jpt_cp_to_utf8(load->codepage, bytes, length, utf8, sizeof utf8) == (size_t)-1) return;
    add_string(NULL, id, utf8);
    load->count++;
}

static int load_release_strings(const char* game_dir, const char* relative, int codepage) {
    char path[MAX_PATH];
    release_load_t load = { codepage, 0 };
    uint8_t* data;
    long size;
    FILE* f;
    if (snprintf(path, sizeof path, "%s" SEP "%s", game_dir, relative) >= (int)sizeof path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 0 ? (uint8_t*)malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) == (size_t)size && jpt_string_table_read(data, (size_t)size, add_release_string, &load) < 0)
        load.count = 0;
    free(data);
    fclose(f);
    qsort(L.strings, (size_t)L.string_count, sizeof *L.strings, by_id);
    return load.count;
}

#ifdef _WIN32
static void use_unicode_fallback_font(void) {
    char font[MAX_PATH];
    UINT n = GetWindowsDirectoryA(font, MAX_PATH);
    if (!n || n >= MAX_PATH - 20) return;
    strcat(font, "\\Fonts\\arial.ttf");
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) dmp_set_fallback_font(font);
}

static void use_release_font(const char* game_dir, const jpt_game_version_t* version) {
    char font[MAX_PATH];
    if (!version->font_file) return;
    snprintf(font, sizeof font, "%s\\%s", game_dir, version->font_file);
    if (AddFontResourceExA(font, FR_PRIVATE, NULL) > 0) snprintf(L.release_face, sizeof L.release_face, "%s", version->font_face);
    host_log("l10n", "the release's font %s %s", version->font_face,
             L.release_face[0] ? "replaces the game's Arial" : "could not be loaded: Arial is kept");
}
#endif

/* The text a release shipped is the game's own in its language: only what it lacks comes from the port. A release whose
 * strings the guest already holds (Spanish, Brazilian) needs no game text from the port at all. */
void l10n_init(const char* cache_dir, const char* game_dir, const char* game_host_dir, const char* language, const jpt_game_version_t* version) {
    const jpt_l10n_language_t* chosen = jpt_l10n_language(language);
    int own_language, release_text = 0;
    memset(&L, 0, sizeof L);
    for (uint32_t i = 0; i < FP_SLOTS; i++) L.fp_value[i] = -1;
    snprintf(L.cache_dir, sizeof L.cache_dir, "%s", cache_dir);
    snprintf(L.game_dir, sizeof L.game_dir, "%s", game_dir);
    snprintf(L.game_host, sizeof L.game_host, "%s", game_host_dir ? game_host_dir : game_dir);
    snprintf(L.text_code, sizeof L.text_code, "%s", chosen ? chosen->code : JPT_L10N_ENGLISH);
    snprintf(L.subtitle_code, sizeof L.subtitle_code, "%s", L.text_code);
    L.codepage = chosen ? chosen->codepage : 1252;
    own_language = version && !strcmp(L.text_code, version->language_code);
    L.release_language = own_language;
#ifdef _WIN32
    use_unicode_fallback_font();
#endif
    jpt_tr_reset();
    if (strcmp(L.text_code, JPT_L10N_ENGLISH)) {
        if (own_language && version->text_strings) {
            L.release_strings = load_release_strings(L.game_host, version->text_strings, version->text_codepage);
            release_text = L.release_strings > 0;
            host_log("l10n", "game text from the %s release's %s: %d strings in code page %d", version->language,
                     version->text_strings, L.release_strings, version->text_codepage);
#ifdef _WIN32
            use_release_font(game_dir, version);
#endif
        }
        if (!own_language || version->text_strings) load_strings(release_text);
        load_subtitles();
        load_interface();
    }
    if (!L.string_count && !L.caption_count && L.interface_count <= 0) L.codepage = 0;
    jpt_l10n_set_display_codepage(L.codepage);
    if (L.string_count) jpt_l10n_set_lookup(l10n_text);
    if (!L.codepage) {
        host_log("l10n", "language %s: game text, menus and subtitles English (the game's own)", language);
        return;
    }
    host_log("l10n", "language %s: game text %d strings (%d release, %d port), subtitles %d samples (%d release, %d port), "
                     "interface %d strings, drawn in code page %d; voices stay English", language, L.string_count,
             L.string_count - L.port_strings, L.port_strings, L.caption_count, L.release_samples, L.port_samples,
             L.interface_count, L.codepage);
    if (L.caption_count) {
#ifdef _WIN32
        HANDLE thread = CreateThread(NULL, 0, scan_english_captions, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        else host_log("l10n", "cannot read the English captions: subtitles stay English");
#else
        scan_english_captions();
#endif
    }
}

int l10n_active(void) { return L.codepage != 0; }

int l10n_release_language(void) { return L.release_language; }

const char* l10n_text(unsigned id) {
    entry_t* e = find_entry(id);
    if (!e || e->checked < 0) return NULL;
    if (!e->display) e->display = to_display(e->utf8);
    return e->display;
}

const char* l10n_text_utf8(unsigned id) {
    entry_t* e = find_entry(id);
    return e && e->checked >= 0 ? e->utf8 : NULL;
}

/* A translation is used only when its printf conversions match the English string's. */
#ifdef _WIN32
static const char* checked_text(HINSTANCE instance, unsigned id) {
    entry_t* e = find_entry(id);
    if (!e || !l10n_text(id)) return NULL;
    if (!e->checked) {
        char english[STRING_PROBE];
        if (LoadStringA(instance, id, english, sizeof english) <= 0) english[0] = 0;
        e->checked = jpt_format_compatible(english, e->display) ? 1 : -1;
        if (e->checked < 0) host_log("l10n", "string %u: its format conversions differ from English: English kept", id);
    }
    return e->checked > 0 ? e->display : NULL;
}
#else
static const char* checked_text(unsigned id) {
    entry_t* e = find_entry(id);
    if (!e || !l10n_text(id)) return NULL;
    if (!e->checked) {
        char english[STRING_PROBE];
        if (!guest_load_string(id, english, sizeof english)) english[0] = 0;
        e->checked = jpt_format_compatible(english, e->display) ? 1 : -1;
        if (e->checked < 0) host_log("l10n", "string %u: its format conversions differ from English: English kept", id);
    }
    return e->checked > 0 ? e->display : NULL;
}

/* LoadStringA's answer when the language has the string: its length, or -1 to leave the game's own. */
int l10n_load_string(unsigned id, char* buffer, int cap) {
    char level[512];
    const char* text = buffer && cap > 0 ? level_actions_hint(id, level, sizeof level) ? level : checked_text(id) : NULL;
    size_t n;
    if (!text) return -1;
    n = strlen(text);
    if (n >= (size_t)cap) {
        if (L.truncations_logged++ < LOG_LIMIT) host_log("l10n", "string %u is %u bytes, the game's buffer %d: cut", id, (unsigned)n, cap);
        n = (size_t)cap - 1;
    }
    memcpy(buffer, text, n);
    buffer[n] = 0;
    return (int)n;
}
#endif

#ifdef _WIN32
void shim_LoadStringA(void) {
    HINSTANCE instance = (HINSTANCE)GUEST_PTR(0);
    UINT id = GUEST_ARG(1);
    char* buffer = (char*)GUEST_PTR(2);
    int cap = (int)GUEST_ARG(3);
    char level[512];
    const char* text = buffer && cap > 0 ? level_actions_hint(id, level, sizeof level) ? level : checked_text(instance, id) : NULL;
    if (text) {
        size_t n = strlen(text);
        if (n >= (size_t)cap) {
            if (L.truncations_logged++ < LOG_LIMIT)
                host_log("l10n", "string %u is %u bytes, the game's buffer %d: cut", id, (unsigned)n, cap);
            n = (size_t)cap - 1;
        }
        memcpy(buffer, text, n);
        buffer[n] = 0;
        g_eax = (uint32_t)n;
    } else {
        g_eax = (uint32_t)LoadStringA(instance, id, buffer, cap);
    }
    GUEST_RETURN_STDCALL(4);
}

/* The release's font replaces the game's only face, as the release's own executable does. */
static const char* face_for(const char* requested) {
    return L.release_face[0] && requested && !_stricmp(requested, "Arial") ? L.release_face : requested;
}

static DWORD charset_for(DWORD requested) {
    return L.codepage && (requested == ANSI_CHARSET || requested == DEFAULT_CHARSET)
               ? (DWORD)jpt_cp_charset(L.codepage)
               : requested;
}

void shim_CreateFontA(void) {
    g_eax = (uint32_t)(uintptr_t)CreateFontA((int)GUEST_ARG(0), (int)GUEST_ARG(1), (int)GUEST_ARG(2),
                                             (int)GUEST_ARG(3), (int)GUEST_ARG(4), GUEST_ARG(5), GUEST_ARG(6),
                                             GUEST_ARG(7), charset_for(GUEST_ARG(8)), GUEST_ARG(9), GUEST_ARG(10),
                                             GUEST_ARG(11), GUEST_ARG(12), face_for((LPCSTR)GUEST_PTR(13)));
    GUEST_RETURN_STDCALL(14);
}

void shim_CreateFontIndirectA(void) {
    const LOGFONTA* in = (const LOGFONTA*)GUEST_PTR(0);
    LOGFONTA lf;
    if (!in) {
        g_eax = 0;
    } else {
        lf = *in;
        lf.lfCharSet = (BYTE)charset_for(lf.lfCharSet);
        if (face_for(lf.lfFaceName) != lf.lfFaceName) snprintf(lf.lfFaceName, sizeof lf.lfFaceName, "%s", L.release_face);
        g_eax = (uint32_t)(uintptr_t)CreateFontIndirectA(&lf);
    }
    GUEST_RETURN_STDCALL(1);
}

#endif

/* The block's length as 0x004395A0 walks it (count at +4, then time and text per caption); 0 when implausible. */
static uint32_t block_extent(uint32_t block) {
    uint32_t count = MEM32(block + 4), at = 8;
    if (!count || count > JPT_CAPTION_MAX_LINES) return 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t len = strnlen((const char*)ADDR(block + at + 4), 0x400);
        if (len == 0x400) return 0;
        at += 4 + (uint32_t)len + 1;
    }
    return at;
}

static const caption_t* translated_block(uint32_t block) {
    uint32_t n;
    int slot;
    if (!ATOMIC_GET(&L.captions_ready)) return NULL;
    if (!(n = block_extent(block))) {
        if (L.unreadable_logged++ < LOG_LIMIT) host_log("l10n", "a sample's caption block is not readable: its captions are not shown");
        return NULL;
    }
    slot = fp_slot(jpt_caption_fingerprint((const uint8_t*)ADDR(block), n));
    if (slot >= 0 && L.fp_value[slot] >= 0) {
        if (L.hits_logged++ < LOG_LIMIT) host_log("l10n", "subtitle shown translated (%s)", L.subtitle_code);
        return &L.captions[L.fp_value[slot]];
    }
    if (L.misses_logged++ < LOG_LIMIT) host_log("l10n", "subtitle without a translation: English shown");
    return NULL;
}

/* A sample's captions handed to the overlay (one argument: the playing sample). */
static void captions(void) {
    uint32_t sample = MEM32(g_esp + 4), state = sample ? MEM32(sample + CAPTION_STATE) : 0;
    uint32_t block = state ? MEM32(state + 4) : 0;
    const caption_t* translated = !g_alias_entry && block && !MEM8(state) ? translated_block(block) : NULL;
    if (!translated) {
        s_captions_lifted();
        return;
    }
    MEM32(state + 4) = guest_keep(translated->block, (uint32_t)translated->size);
    s_captions_lifted();
    MEM32(state + 4) = block;
}

/* The translation of a TEXT action's own English text (the beach tutorial's lines), in the display code page. */
static const char* tutorial_text(const char* english) {
    const char* key = jpt_tutorial_key(english);
    if (key && jpt_tr_has(key)) {
        if (L.text_hits_logged++ < LOG_LIMIT) host_log("l10n", "tutorial text translated (%s): \"%s\"", L.text_code, english);
        return jpt_tr_cp(key);
    }
    if (L.text_misses_logged++ < LOG_LIMIT)
        host_log("l10n", "level text without a translation, English shown: \"%s\"", english);
    return NULL;
}

/* A level TEXT action (this in ecx): its string ID at +0x18, else its own English text at +0x14. */
static void text_action(void) {
    uint32_t self = g_ecx, inline_text = self ? MEM32(self + TEXT_ACTION_TEXT) : 0;
    const char* translated = NULL;
    if (!g_alias_entry && inline_text && !MEM32(self + TEXT_ACTION_ID)) translated = tutorial_text((const char*)ADDR(inline_text));
    if (!translated) {
        s_text_action_lifted();
        return;
    }
    MEM32(self + TEXT_ACTION_TEXT) = guest_keep(translated, (uint32_t)strlen(translated) + 1u);
    s_text_action_lifted();
    MEM32(self + TEXT_ACTION_TEXT) = inline_text;
}

void l10n_install(void) {
    if (L.caption_count && !guest_override_install(CAPTIONS_VA, captions, &s_captions_lifted))
        host_log("l10n", "the caption override is missing: subtitles stay English");
    if ((L.string_count || L.interface_count > 0) && !guest_override_install(TEXT_ACTION_VA, text_action, &s_text_action_lifted))
        host_log("l10n", "the TEXT action override is missing: level texts stay English");
}

static const char* ddf_lookup(void* ctx, unsigned id) {
    (void)ctx;
    return l10n_text(id);
}

const char* l10n_label(const char* english) {
    int id = jpt_ddf_text_id(english);
    return id ? l10n_text((unsigned)id) : NULL;
}

static int translate_menu_file(const char* source, const char* name, char* out, size_t cap) {
    char dir[2 * MAX_PATH], *data, *translated = NULL;
    size_t n = 0, translated_n = 0;
    int replaced;
    FILE* f;
    snprintf(dir, sizeof dir, "%s" SEP MENU_CACHE SEP "%s", L.cache_dir, L.text_code);
    if (snprintf(out, cap, "%s" SEP "%s", dir, name) >= (int)cap) return 0;
    if (!(data = read_all(source, &n))) {
        host_log("l10n", "menu\\%s: cannot read %s", name, source);
        return 0;
    }
    replaced = jpt_ddf_translate(data, n, ddf_lookup, NULL, &translated, &translated_n);
    free(data);
    if (replaced <= 0) {
        free(translated);
        return 0;
    }
    if (!jpt_make_dirs(dir) || !(f = fopen(out, "wb"))) {
        host_log("l10n", "menu\\%s: cannot write %s: its labels stay English", name, out);
        free(translated);
        return 0;
    }
    replaced = fwrite(translated, 1, translated_n, f) == translated_n && !fclose(f) ? replaced : 0;
    free(translated);
    if (replaced) host_log("l10n", "menu\\%s: %d labels translated (%s)", name, replaced, out);
    return replaced > 0;
}

void l10n_set_user_folder(const char* folder) { snprintf(L.user_dir, sizeof L.user_dir, "%s", folder ? folder : ""); }

int l10n_menu_file(const char* path, char* out, size_t cap) {
    char source[MAX_PATH], name[64];
    menu_file_t* m = NULL;
    char host_source[MAX_PATH];
    if (!L.string_count || !jpt_menu_ddf_source(L.game_dir, L.user_dir, path, source, sizeof source, name, sizeof name) ||
        !guest_host_path(source, host_source, sizeof host_source))
        return 0;
    for (int i = 0; i < L.menu_count; i++)
        if (!_stricmp(L.menus[i].name, name)) m = &L.menus[i];
    if (!m) {
        if (L.menu_count == MENU_FILES) return 0;
        m = &L.menus[L.menu_count++];
        snprintf(m->name, sizeof m->name, "%s", name);
        m->translated = translate_menu_file(host_source, name, out, cap);
        return m->translated;
    }
    return m->translated && snprintf(out, cap, "%s" SEP MENU_CACHE SEP "%s" SEP "%s", L.cache_dir, L.text_code, name) <
                                (int)cap;
}

#ifdef _WIN32
int l10n_message_box(HWND owner, const char* text, const char* caption, UINT type) {
    WCHAR wide_text[4096], wide_caption[512];
    UINT cp = (UINT)L.codepage;
    if (!MultiByteToWideChar(cp, 0, text ? text : "", -1, wide_text, 4096)) wide_text[0] = 0;
    if (!MultiByteToWideChar(cp, 0, caption ? caption : "", -1, wide_caption, 512)) wide_caption[0] = 0;
    return MessageBoxW(owner, wide_text, caption ? wide_caption : NULL, type);
}
#endif
