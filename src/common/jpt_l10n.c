#include "jpt_l10n.h"

#include <stdlib.h>
#include <string.h>

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
const jpt_l10n_language_t jpt_l10n_languages[JPT_L10N_LANGUAGE_COUNT] = {
    { "en", "English", 1252 },  { "pt-BR", "Brazilian Portuguese", 1252 }, { "fr", "French", 1252 },
    { "de", "German", 1252 },   { "es", "Spanish", 1252 },                { "ru", "Russian", 1251 },
    { "it", "Italian", 1252 },
};

const jpt_l10n_credit_t jpt_l10n_credits[JPT_L10N_CREDIT_COUNT] = {
    { "pt-BR", "game text", "DreamWorks Interactive", "Nick3069", "official" },
    { "fr", "game text", "DreamWorks Interactive", "Nick3069", "official" },
    { "de", "game text", "DreamWorks Interactive", "Nick3069", "official" },
    { "es", "game text", "DreamWorks Interactive", "Nick3069", "official" },
    { "ru", "game text and subtitles", "cynodesmus", NULL, "community" },
    { "it", "subtitles", "KeyofBlueS", NULL, "community" },
};

int jpt_l10n_id_fixed(unsigned id) { return id == 10; }

const jpt_l10n_language_t* jpt_l10n_language(const char* code) {
    for (int i = 0; code && i < JPT_L10N_LANGUAGE_COUNT; i++)
        if (!strcmp(jpt_l10n_languages[i].code, code)) return &jpt_l10n_languages[i];
    return NULL;
}

const char* jpt_l10n_infer_language(const char* game_text, const char* subtitles) {
    const jpt_l10n_language_t* text = jpt_l10n_language(game_text);
    const jpt_l10n_language_t* subs = jpt_l10n_language(subtitles);
    if (text && strcmp(text->code, JPT_L10N_ENGLISH)) return text->code;
    if (subs && strcmp(subs->code, JPT_L10N_ENGLISH)) return subs->code;
    return JPT_L10N_ENGLISH;
}

/* ---- code pages */

static const uint16_t k_cp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D,
    0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x009D, 0x017E, 0x0178,
};

static const uint16_t k_cp1251[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C,
    0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A,
    0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9,
    0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

static uint32_t cp_decode(int codepage, unsigned char b) {
    if (b < 0x80) return b;
    if (codepage == 1251) return b >= 0xC0 ? 0x0410u + (b - 0xC0u) : k_cp1251[b - 0x80];
    return b >= 0xA0 ? b : k_cp1252[b - 0x80];
}

static int cp_encode(int codepage, uint32_t c) {
    if (c < 0x80) return (int)c;
    for (int b = 0x80; b <= 0xFF; b++)
        if (cp_decode(codepage, (unsigned char)b) == c) return b;
    return -1;
}

/* The unaccented letter of U+00C0..U+00FF; '?' where there is none. */
static const char k_latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUY?saaaaaaaceeeeiiiidnooooo?ouuuuy?y";

static int best_fit(int codepage, uint32_t c) {
    int b = cp_encode(codepage, c);
    if (b >= 0) return b;
    if (c >= 0xC0 && c <= 0xFF) return k_latin1_base[c - 0xC0];
    switch (c) {
    case 0x0152: return 'O';
    case 0x0153: return 'o';
    case 0x0160: return 'S';
    case 0x017D: return 'Z';
    case 0x0178: return 'Y';
    case 0x0161: return 's';
    case 0x017E: return 'z';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: case 0x00AB: case 0x00BB: return '"';
    case 0x2013: case 0x2014: return '-';
    default: return '?';
    }
}

static size_t utf8_put(uint32_t c, char* out) {
    if (c < 0x80) return out[0] = (char)c, 1;
    if (c < 0x800) return out[0] = (char)(0xC0 | (c >> 6)), out[1] = (char)(0x80 | (c & 0x3F)), 2;
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* One code point from s (n bytes left); 0 bytes used when the sequence is not UTF-8. */
static size_t utf8_get(const unsigned char* s, size_t n, uint32_t* c) {
    size_t len;
    uint32_t v;
    if (!n) return 0;
    if (s[0] < 0x80) return *c = s[0], 1;
    if ((s[0] & 0xE0) == 0xC0) len = 2, v = s[0] & 0x1Fu;
    else if ((s[0] & 0xF0) == 0xE0) len = 3, v = s[0] & 0x0Fu;
    else if ((s[0] & 0xF8) == 0xF0) len = 4, v = s[0] & 0x07u;
    else return 0;
    if (len > n) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *c = v;
    return len;
}

int jpt_utf8_valid(const char* s, size_t n) {
    uint32_t c;
    for (size_t i = 0, k; i < n; i += k)
        if (!(k = utf8_get((const unsigned char*)s + i, n - i, &c))) return 0;
    return 1;
}

size_t jpt_cp_to_utf8(int codepage, const char* in, size_t n, char* out, size_t cap) {
    size_t o = 0;
    char buf[4];
    for (size_t i = 0; i < n; i++) {
        size_t k = utf8_put(cp_decode(codepage, (unsigned char)in[i]), buf);
        if (o + k >= cap) return (size_t)-1;
        memcpy(out + o, buf, k);
        o += k;
    }
    if (!cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

size_t jpt_utf8_to_cp(int codepage, const char* in, char* out, size_t cap) {
    const unsigned char* s = (const unsigned char*)in;
    size_t n = strlen(in), o = 0;
    uint32_t c;
    for (size_t i = 0, k; i < n; i += k) {
        if (!(k = utf8_get(s + i, n - i, &c))) return (size_t)-1;
        if (o + 1 >= cap) return (size_t)-1;
        out[o++] = (char)best_fit(codepage, c);
    }
    if (!cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

int jpt_cp_charset(int codepage) { return codepage == 1251 ? 204 : 0; }

static int s_display_codepage;

void jpt_l10n_set_display_codepage(int codepage) { s_display_codepage = codepage; }
int jpt_l10n_display_codepage(void) { return s_display_codepage; }
int jpt_l10n_display_charset(int fallback) {
    return s_display_codepage ? jpt_cp_charset(s_display_codepage) : fallback;
}

static jpt_l10n_text_lookup_fn s_lookup;

void jpt_l10n_set_lookup(jpt_l10n_text_lookup_fn fn) { s_lookup = fn; }

const char* jpt_l10n_text_or(unsigned id, const char* english) {
    const char* text = s_lookup ? s_lookup(id) : NULL;
    return text ? text : english;
}

/* ---- GameStrings */

static int is_upper(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x0400 && c <= 0x042F);
}

static int is_lower(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7) || (c >= 0x0430 && c <= 0x045F);
}

/* "word.Next" -> "word. Next". out holds at least 2 * strlen(in) + 1 bytes. */
static int normalize_spacing(const char* in, char* out) {
    const unsigned char* s = (const unsigned char*)in;
    size_t n = strlen(in), o = 0;
    uint32_t prev = 0, c, next;
    int added = 0;
    for (size_t i = 0, k; i < n; i += k) {
        k = utf8_get(s + i, n - i, &c);
        if (!k) k = 1, c = s[i];
        memcpy(out + o, in + i, k);
        o += k;
        if ((c == '.' || c == '!' || c == '?') && is_lower(prev) && i + k < n &&
            utf8_get(s + i + k, n - i - k, &next) && is_upper(next)) {
            out[o++] = ' ';
            added++;
        }
        prev = c;
    }
    out[o] = 0;
    return added;
}

/* Key names (16000 + the key) are abbreviations such as "Verr.Maj", not sentences. */
static int is_key_name(unsigned long id) { return id >= 16000 && id < 17000; }

static const char* skip_blank(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* The quoted value at p (after the opening quote) into raw. Returns the byte after the closing quote, NULL when the
 * quote is not closed. */
static const char* unquote(const char* p, const char* end, char* raw, size_t* len) {
    size_t o = 0;
    while (p < end) {
        char ch = *p++;
        if (ch == '"') {
            if (p < end && *p == '"') {
                raw[o++] = '"';
                p++;
                continue;
            }
            *len = o;
            return p;
        }
        if (ch == '\\' && p < end) {
            char e = *p++;
            switch (e) {
            case 'r': raw[o++] = '\r'; break;
            case 'n': raw[o++] = '\n'; break;
            case 't': raw[o++] = '\t'; break;
            case '"': raw[o++] = '"'; break;
            case '\\': raw[o++] = '\\'; break;
            default: raw[o++] = '\\', raw[o++] = e; break;
            }
            continue;
        }
        raw[o++] = ch;
    }
    return NULL;
}

int jpt_gs_parse(const char* data, size_t n, int codepage, int normalize, jpt_text_fn fn, void* ctx,
                 jpt_gs_stats_t* stats) {
    const char *p = data, *end = data + n;
    unsigned char* seen = (unsigned char*)calloc(JPT_L10N_ID_LIMIT / 8, 1);
    jpt_gs_stats_t st;
    int utf8 = 0, line_no = 0;
    memset(&st, 0, sizeof st);
    if (!seen) return 0;
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) {
        utf8 = 1;
        p += 3;
    }
    while (p < end) {
        const char *line = p, *eol = memchr(p, '\n', (size_t)(end - p)), *q;
        size_t len = (size_t)((eol ? eol : end) - line), raw_n = 0;
        char *raw, *text, *spaced;
        unsigned long id = 0;
        int digits = 0, ok = 0;
        p = eol ? eol + 1 : end;
        line_no++;
        if (len && line[len - 1] == '\r') len--;
        q = skip_blank(line, line + len);
        if (q == line + len || *q == ';') continue;
        while (q < line + len && *q >= '0' && *q <= '9' && digits < 6) id = id * 10 + (unsigned long)(*q++ - '0'), digits++;
        raw = (char*)malloc(len + 1);
        text = (char*)malloc(len * 3 + 1);
        spaced = (char*)malloc(len * 6 + 2);
        if (!raw || !text || !spaced) {
            free(raw), free(text), free(spaced);
            break;
        }
        if (digits && id < JPT_L10N_ID_LIMIT) {
            q = skip_blank(q, line + len);
            if (q < line + len && *q == '=') {
                q = skip_blank(q + 1, line + len);
                if (q < line + len && *q == '"' && (q = unquote(q + 1, line + len, raw, &raw_n)) != NULL) {
                    q = skip_blank(q, line + len);
                    ok = q == line + len || *q == ';';
                }
            }
        }
        if (ok) {
            raw[raw_n] = 0;
            if (utf8) ok = jpt_utf8_valid(raw, raw_n) && (memcpy(text, raw, raw_n + 1), 1);
            else ok = jpt_cp_to_utf8(codepage, raw, raw_n, text, len * 3 + 1) != (size_t)-1;
        }
        if (!ok) {
            if (!st.invalid++) st.first_invalid_line = line_no;
        } else if (seen[id / 8] & (1u << (id % 8))) {
            st.duplicates++;
        } else if (text[0]) {
            seen[id / 8] |= (unsigned char)(1u << (id % 8));
            if (normalize && !is_key_name(id)) st.normalized += normalize_spacing(text, spaced);
            else strcpy(spaced, text);
            st.entries++;
            fn(ctx, (unsigned)id, spaced);
        }
        free(raw), free(text), free(spaced);
    }
    free(seen);
    if (stats) *stats = st;
    return st.entries > 0;
}

/* The conversion letters of a printf format, %% skipped. */
static size_t conversions(const char* f, char* out, size_t cap) {
    size_t o = 0;
    for (; *f; f++) {
        if (*f != '%') continue;
        if (f[1] == '%') {
            f++;
            continue;
        }
        f++;
        while (*f && strchr("-+ #0123456789.*hlLIw", *f)) f++;
        if (!*f) break;
        if (o + 1 < cap) out[o++] = *f == 'i' ? 'd' : *f;
    }
    out[o] = 0;
    return o;
}

int jpt_format_compatible(const char* english, const char* translated) {
    char a[64], b[64];
    conversions(english, a, sizeof a);
    conversions(translated, b, sizeof b);
    return !strcmp(a, b);
}

/* ---- installed files */

size_t jpt_l10n_escape(const char* text, char* out, size_t cap) {
    size_t o = 0;
    for (; *text; text++) {
        const char* e = *text == '\\' ? "\\\\" : *text == '\n' ? "\\n" : *text == '\r' ? "\\r" : *text == '\t' ? "\\t" : NULL;
        size_t k = e ? 2 : 1;
        if (o + k >= cap) return (size_t)-1;
        if (e) memcpy(out + o, e, 2);
        else out[o] = *text;
        o += k;
    }
    if (!cap) return (size_t)-1;
    out[o] = 0;
    return o;
}

static void unescape(char* s) {
    char* o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* The next line of data without its line break, copied into a malloc'd buffer; NULL at the end. */
static char* next_line(const char** p, const char* end) {
    const char *line = *p, *eol;
    size_t len;
    char* copy;
    if (*p >= end) return NULL;
    eol = memchr(line, '\n', (size_t)(end - line));
    len = (size_t)((eol ? eol : end) - line);
    *p = eol ? eol + 1 : end;
    if (len && line[len - 1] == '\r') len--;
    copy = (char*)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, line, len);
    copy[len] = 0;
    return copy;
}

static int has_header(const char** p, const char* end, const char* header) {
    char* first = next_line(p, end);
    int ok = first && !strcmp(first, header);
    free(first);
    return ok;
}

int jpt_l10n_read_strings(const char* data, size_t n, jpt_text_fn fn, void* ctx) {
    const char *p = data, *end = data + n;
    char* line;
    int count = 0;
    if (!has_header(&p, end, JPT_L10N_STRINGS_HEADER)) return -1;
    while ((line = next_line(&p, end)) != NULL) {
        char *tab = strchr(line, '\t'), *stop = NULL;
        unsigned long id = tab ? strtoul(line, &stop, 10) : 0;
        if (tab && stop == tab && tab > line && id < JPT_L10N_ID_LIMIT && tab[1]) {
            unescape(tab + 1);
            if (jpt_utf8_valid(tab + 1, strlen(tab + 1))) {
                fn(ctx, (unsigned)id, tab + 1);
                count++;
            }
        }
        free(line);
    }
    return count;
}

typedef struct {
    char pack[16];
    uint32_t sample;
    int count, open;
    uint32_t units[JPT_CAPTION_MAX_LINES];
    char* lines[JPT_CAPTION_MAX_LINES];
} sample_t;

static void flush_sample(sample_t* s, jpt_sample_fn fn, void* ctx, int* samples) {
    if (s->open && s->count) {
        fn(ctx, s->pack, s->sample, s->count, s->units, (const char* const*)s->lines);
        (*samples)++;
    }
    for (int i = 0; i < s->count; i++) free(s->lines[i]);
    s->count = s->open = 0;
}

int jpt_l10n_read_subtitles(const char* data, size_t n, jpt_sample_fn fn, void* ctx) {
    const char *p = data, *end = data + n;
    sample_t s;
    char* line;
    int samples = 0;
    memset(&s, 0, sizeof s);
    if (!has_header(&p, end, JPT_L10N_SUBTITLES_HEADER)) return -1;
    while ((line = next_line(&p, end)) != NULL) {
        char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL, *stop = NULL;
        if (line[0] == 'S' && a == line + 1 && b && b - a - 1 < (int)sizeof s.pack) {
            flush_sample(&s, fn, ctx, &samples);
            memcpy(s.pack, a + 1, (size_t)(b - a - 1));
            s.pack[b - a - 1] = 0;
            s.sample = (uint32_t)strtoul(b + 1, &stop, 16);
            s.open = *stop == 0 && b[1];
        } else if (line[0] == 'C' && a == line + 1 && b && s.open && s.count < JPT_CAPTION_MAX_LINES) {
            unsigned long units = strtoul(a + 1, &stop, 10);
            if (stop == b && units < 1000) {
                unescape(b + 1);
                if (jpt_utf8_valid(b + 1, strlen(b + 1)) && (s.lines[s.count] = _strdup(b + 1)) != NULL)
                    s.units[s.count++] = (uint32_t)units;
            }
        }
        free(line);
    }
    flush_sample(&s, fn, ctx, &samples);
    return samples;
}

/* ---- TPA captions */

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int jpt_caption_lines(const uint8_t* block, size_t n, jpt_caption_line_fn fn, void* ctx) {
    size_t p = 8;
    uint32_t count;
    if (n < 8 || rd32(block) != n) return -1;
    count = rd32(block + 4);
    if (!count || count > JPT_CAPTION_MAX_LINES) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* z;
        uint32_t units;
        if (p + 4 >= n) return -1;
        units = rd32(block + p);
        p += 4;
        z = (const uint8_t*)memchr(block + p, 0, n - p);
        if (!z) return -1;
        if (fn) fn(ctx, units, (const char*)block + p, (size_t)(z - (block + p)));
        p = (size_t)(z - block) + 1;
    }
    return (int)count;
}

uint64_t jpt_caption_fingerprint(const uint8_t* block, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 4; i < n; i++) h = (h ^ block[i]) * 0x100000001B3ull;
    return h;
}

size_t jpt_caption_build(int count, const uint32_t* units, const char* const* lines, uint8_t* out, size_t cap) {
    size_t p = 8;
    if (count <= 0 || count > JPT_CAPTION_MAX_LINES || cap < 8) return 0;
    for (int i = 0; i < count; i++) {
        size_t len = strlen(lines[i]) + 1;
        if (p + 4 + len > cap) return 0;
        wr32(out + p, units[i]);
        memcpy(out + p + 4, lines[i], len);
        p += 4 + len;
    }
    wr32(out, (uint32_t)p);
    wr32(out + 4, (uint32_t)count);
    return p;
}

typedef struct {
    uint32_t id, index;
} id_index_t;

static int by_id(const void* a, const void* b) {
    const id_index_t *x = (const id_index_t*)a, *y = (const id_index_t*)b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

int jpt_tpa_captions(jpt_read_fn read, void* rctx, uint64_t size, jpt_tpa_caption_fn fn, void* ctx,
                     jpt_tpa_stats_t* stats) {
    uint8_t head[JPT_TPA_HEADER], *dir, *later;
    id_index_t* order;
    uint32_t count;
    jpt_tpa_stats_t st;
    memset(&st, 0, sizeof st);
    if (stats) *stats = st;
    if (size < JPT_TPA_HEADER || !read(rctx, 0, head, sizeof head) || rd32(head) != JPT_TPA_MAGIC ||
        rd32(head + 8) != JPT_TPA_HEADER)
        return 0;
    count = rd32(head + 4);
    if (!count || count > JPT_TPA_MAX_SAMPLES || JPT_TPA_HEADER + (uint64_t)count * JPT_TPA_ENTRY > size) return 0;
    dir = (uint8_t*)malloc((size_t)count * JPT_TPA_ENTRY);
    order = (id_index_t*)malloc(count * sizeof *order);
    later = (uint8_t*)calloc(count, 1);
    if (!dir || !order || !later || !read(rctx, JPT_TPA_HEADER, dir, (size_t)count * JPT_TPA_ENTRY)) {
        free(dir), free(order), free(later);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) order[i].id = rd32(dir + i * JPT_TPA_ENTRY), order[i].index = i;
    qsort(order, count, sizeof *order, by_id);
    for (uint32_t i = 1; i < count; i++)
        if (order[i].id == order[i - 1].id) later[order[i].index] = 1, st.duplicates++;
    st.samples = (int)(count - (uint32_t)st.duplicates);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = dir + i * JPT_TPA_ENTRY;
        uint32_t offset = rd32(e + 4), header = rd32(e + 0x18 + 4), block_n;
        uint8_t* buf;
        if (later[i] || header <= JPT_TPA_DESCRIPTOR) continue;
        if (header > 0x10000u || offset + (uint64_t)header > size || !(buf = (uint8_t*)malloc(header))) {
            st.malformed++;
            continue;
        }
        if (!read(rctx, offset, buf, header) || memcmp(buf, e + 0x14, JPT_TPA_DESCRIPTOR) ||
            (block_n = rd32(buf + JPT_TPA_DESCRIPTOR)) > header - JPT_TPA_DESCRIPTOR ||
            jpt_caption_lines(buf + JPT_TPA_DESCRIPTOR, block_n, NULL, NULL) < 0) {
            st.malformed++;
        } else {
            st.captioned++;
            fn(ctx, rd32(e), buf + JPT_TPA_DESCRIPTOR, block_n);
        }
        free(buf);
    }
    free(dir), free(order), free(later);
    if (stats) *stats = st;
    return 1;
}

/* ---- menu labels and inline texts */

static const struct {
    const char* english;
    int id;
} k_ddf[] = {
    { "Quality", 10000 },          { "Brightness", 10001 },           { "Screen Size", 10002 },
    { "Video Driver Name", 10003 }, { "World Initialization", 10010 }, { "Level File Copy", 10011 },
    { "Walk Forward", 10020 },     { "Run Forward", 10021 },          { "Walk Backwards", 10022 },
    { "Sidestep Left", 10023 },    { "Sidestep Right", 10024 },       { "Jump", 10025 },
    { "Crouch", 10026 },           { "Throw", 10027 },                { "Use/Fire", 10028 },
    { "Rotate Wrist", 10029 },     { "Move Hand", 10030 },            { "Grab/Drop", 10031 },
    { "Stow/Retrieve", 10032 },    { "Replay", 10033 },               { "voice over", 10034 },
    { "Gore", 10035 },             { "Invert Mouse", 10036 },         { "Rotate Arm", 10037 },
    { "Play Sound Effects", 10050 }, { "Play Ambient Sounds", 10051 }, { "Play Voice Overs", 10052 },
    { "Play Background Music", 10053 }, { "Show Subtitles", 10054 }, { "3D Audio Accel. Enabled", 10055 },
};

int jpt_ddf_text_id(const char* english) {
    for (int i = 0; english && i < COUNT(k_ddf); i++)
        if (!strcmp(k_ddf[i].english, english)) return k_ddf[i].id;
    return 0;
}

static const char* find_nocase(const char* s, size_t n, const char* what) {
    size_t w = strlen(what);
    for (size_t i = 0; i + w <= n; i++) {
        size_t k = 0;
        while (k < w && (s[i + k] | 0x20) == (what[k] | 0x20)) k++;
        if (k == w) return s + i;
    }
    return NULL;
}

/* The replacement of one DDF line, or NULL to keep it. */
static const char* ddf_label(const char* line, size_t len, jpt_lookup_fn lookup, void* ctx, size_t* open,
                             size_t* close) {
    const char *q1 = memchr(line, '"', len), *q2, *semi, *text;
    char english[64];
    int id;
    if (!q1 || !(q2 = memchr(q1 + 1, '"', len - (size_t)(q1 + 1 - line)))) return NULL;
    semi = skip_blank(q2 + 1, line + len);
    if (semi == line + len || *semi != ';' || !find_nocase(semi, (size_t)(line + len - semi), "Initial Text") ||
        (size_t)(q2 - q1 - 1) >= sizeof english)
        return NULL;
    memcpy(english, q1 + 1, (size_t)(q2 - q1 - 1));
    english[q2 - q1 - 1] = 0;
    if (!(id = jpt_ddf_text_id(english)) || !(text = lookup(ctx, (unsigned)id)) || strpbrk(text, "\"\\\r\n"))
        return NULL;
    *open = (size_t)(q1 - line) + 1;
    *close = (size_t)(q2 - line);
    return text;
}

int jpt_ddf_translate(const char* in, size_t n, jpt_lookup_fn lookup, void* ctx, char** out, size_t* out_n) {
    const char *p = in, *end = in + n;
    size_t cap = n + 1, o = 0;
    char* buf = (char*)malloc(cap);
    int replaced = 0;
    if (!buf) return -1;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p)), *text;
        size_t len = (size_t)((eol ? eol + 1 : end) - p), body = len, open = 0, close = 0, need;
        while (body && (p[body - 1] == '\n' || p[body - 1] == '\r')) body--;
        text = ddf_label(p, body, lookup, ctx, &open, &close);
        need = text ? open + strlen(text) + (len - close) : len;
        if (o + need + 1 > cap) {
            char* grown;
            cap = (o + need + 1) * 2;
            if (!(grown = (char*)realloc(buf, cap))) {
                free(buf);
                return -1;
            }
            buf = grown;
        }
        if (text) {
            memcpy(buf + o, p, open);
            memcpy(buf + o + open, text, strlen(text));
            memcpy(buf + o + open + strlen(text), p + close, len - close);
            replaced++;
        } else {
            memcpy(buf + o, p, len);
        }
        o += need;
        p += len;
    }
    buf[o] = 0;
    *out = buf;
    *out_n = o;
    return replaced;
}

static const struct {
    const char* english;
    const char* key;
} k_tutorial[JPT_L10N_TUTORIAL_COUNT] = {
    { "Welcome to Trespasser.",
      "Tutorial::Welcome to Trespasser." },
    { "To look around, move your mouse.",
      "Tutorial::To look around, move <LOOK>." },
    { "To move forward, press and hold 'W'.",
      "Tutorial::To move forward, press and hold <RUN>." },
    { "Your adventure begins here.",
      "Tutorial::Your adventure begins here." },
    { "Press 'Q' to jump up.",
      "Tutorial::Press <JUMP> to jump up." },
    { "Press 'A' and 'D' to step left and right.",
      "Tutorial::Press <LEFT> and <RIGHT> to step left and right." },
    { "And press 'Z' to crouch.",
      "Tutorial::And press <CROUCH> to crouch." },
    { "You can use your hand to move and carry objects.",
      "Tutorial::You can use your hand to move and carry objects." },
    { "Try pressing and holding the left mouse button.",
      "Tutorial::Try pressing and holding <HAND>." },
    { "Now, moving the mouse moves your hand.",
      "Tutorial::Now, moving <LOOK> moves your hand." },
    { "Try picking up the two-by-four on the crate up ahead.",
      "Tutorial::Try picking up the two-by-four on the crate up ahead." },
    { "To pick up an object, move your hand near it....",
      "Tutorial::To pick up an object, move your hand near it...." },
    { "Then click the right mouse button.",
      "Tutorial::Then click <GRAB>." },
    { "Good! Hold the left mouse button and move the mouse to wield the two-by-four.",
      "Tutorial::Good! Hold <HAND> and move <LOOK> to wield the two-by-four." },
    { "Press the right button to drop it.",
      "Tutorial::Press <GRAB> to drop it." },
    { "Good.  Try picking up some other items.",
      "Tutorial::Good. Try picking up some other items." },
    { "You may have to crouch to reach them if they're on the ground.",
      "Tutorial::You may have to crouch to reach them if they're on the ground." },
    { "Feel free to practice running and jumping here, until you're ready to move on.",
      "Tutorial::Feel free to practice running and jumping here, until you're ready to move on." },
    { "Firearms can be an important survival tool.",
      "Tutorial::Firearms can be an important survival tool." },
    { "To fire a gun, pick it up, and press the space bar.",
      "Tutorial::To fire a gun, pick it up, and press <USE>." },
    { "You can aim the gun using the sights.",
      "Tutorial::You can aim the gun using the sights." },
    { "To adjust your wrist, press shift +  left mouse button, and move the mouse.",
      "Tutorial::To adjust your wrist, press <WRIST> + <HAND>, and move <LOOK>." },
    { "You can rotate your arm by holding control + the left mouse button, and moving the mouse..",
      "Tutorial::You can rotate your arm by holding <ARM> + <HAND>, and moving <LOOK>." },
    { "Press 'F' to throw an object you are holding.",
      "Tutorial::Press <THROW> to throw an object you are holding." },
    { "You will always throw toward the center of your view.",
      "Tutorial::You will always throw toward the center of your view." },
    { "Step up and try to nail a paint can!",
      "Tutorial::Step up and try to nail a paint can!" },
    { "To strike with an object, pick up the object and press the space bar.",
      "Tutorial::To strike with an object, pick up the object and press <USE>." },
    { "Try swinging the baseball bat.",
      "Tutorial::Try swinging the baseball bat." },
    { "You can always press F1 for a hint about what to do next.",
      "Tutorial::You can always press <HINT> for a hint about what to do next." },
};

/* The text with every run of blanks one space and none at the ends. */
static size_t squeeze(const char* in, char* out, size_t cap) {
    size_t o = 0;
    int space = 0;
    for (; *in; in++) {
        if (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') {
            space = o > 0;
            continue;
        }
        if (space && o + 1 < cap) out[o++] = ' ';
        space = 0;
        if (o + 1 < cap) out[o++] = *in;
    }
    out[o] = 0;
    return o;
}

const char* jpt_tutorial_key(const char* english) {
    char a[512], b[512];
    if (!english || squeeze(english, a, sizeof a) == 0) return NULL;
    for (int i = 0; i < JPT_L10N_TUTORIAL_COUNT; i++) {
        squeeze(k_tutorial[i].english, b, sizeof b);
        if (!strcmp(a, b)) return k_tutorial[i].key;
    }
    return NULL;
}

const char* jpt_tutorial_english(int index) {
    return index >= 0 && index < JPT_L10N_TUTORIAL_COUNT ? k_tutorial[index].english : NULL;
}

const char* jpt_tutorial_key_at(int index) {
    return index >= 0 && index < JPT_L10N_TUTORIAL_COUNT ? k_tutorial[index].key : NULL;
}

static int ddf_name_ok(const char* name) {
    size_t n = strlen(name);
    return n > 4 && n < 64 && !_stricmp(name + n - 4, ".ddf") && !strpbrk(name, "\\/:");
}

static size_t trimmed_length(const char* dir) {
    size_t n = strlen(dir);
    while (n && dir[n - 1] == '\\') n--;
    return n;
}

static int has_prefix(const char* path, const char* dir, size_t dir_len, const char* tail) {
    return dir_len && !_strnicmp(path, dir, dir_len) && path[dir_len] == '\\' &&
           !_strnicmp(path + dir_len + 1, tail, strlen(tail));
}

int jpt_menu_ddf_source(const char* game_dir, const char* user_dir, const char* path, char* source, size_t cap,
                        char* name, size_t name_cap) {
    const char* file = NULL;
    size_t g = game_dir ? trimmed_length(game_dir) : 0, u = user_dir ? trimmed_length(user_dir) : 0;
    if (!path || !g || !source || !name || strstr(path, "..") || strchr(path, '/')) return 0;
    if (!_strnicmp(path, "menu\\", 5)) file = path + 5;
    else if (!_strnicmp(path, "data\\menu\\", 10)) file = path + 10;
    else if (has_prefix(path, game_dir, g, "data\\menu\\")) file = path + g + 11;
    else if (has_prefix(path, user_dir, u, "menu\\")) file = path + u + 6;
    if (!file || !ddf_name_ok(file) || strlen(file) >= name_cap) return 0;
    if ((size_t)snprintf(source, cap, "%.*s\\data\\menu\\%s", (int)g, game_dir, file) >= cap) return 0;
    strcpy(name, file);
    return 1;
}
