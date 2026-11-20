#include "jpt_tr.h"
#include "jpt_compat.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_l10n.h"

typedef struct {
    char* key;
    char* utf8;
    char* display;
} tr_entry_t;

static struct {
    tr_entry_t* entries;
    int count, cap;
    int codepage;
} T;

const char* jpt_tr_english(const char* key) {
    const char* mark = key ? strstr(key, "::") : NULL;
    return mark ? mark + 2 : key ? key : "";
}

static int by_key(const void* a, const void* b) { return strcmp(((const tr_entry_t*)a)->key, ((const tr_entry_t*)b)->key); }

static const tr_entry_t* find(const char* key) {
    tr_entry_t probe;
    if (!key || !T.count) return NULL;
    probe.key = (char*)key;
    return (const tr_entry_t*)bsearch(&probe, T.entries, (size_t)T.count, sizeof probe, by_key);
}

const char* jpt_tr(const char* key) {
    const tr_entry_t* e = find(key);
    return e ? e->utf8 : jpt_tr_english(key);
}

const char* jpt_tr_cp(const char* key) {
    const tr_entry_t* e = find(key);
    return e && e->display ? e->display : jpt_tr_english(key);
}

int jpt_tr_has(const char* key) { return find(key) != NULL; }

void jpt_tr_reset(void) {
    for (int i = 0; i < T.count; i++) {
        free(T.entries[i].key);
        free(T.entries[i].utf8);
        free(T.entries[i].display);
    }
    free(T.entries);
    memset(&T, 0, sizeof T);
}

static char* unescaped(const char* s, size_t n) {
    char* out = (char*)malloc(n + 1);
    size_t o = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            char e = s[++i];
            out[o++] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

int jpt_tr_parse(const char* data, size_t n, jpt_tr_entry_fn fn, void* ctx) {
    const char *p = data, *end = data + n;
    size_t header = strlen(JPT_TR_HEADER);
    int count = 0;
    if (n >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    if ((size_t)(end - p) < header || memcmp(p, JPT_TR_HEADER, header) || (p + header < end && p[header] != '\n' && p[header] != '\r'))
        return -1;
    while (p < end && *p != '\n') p++;
    while (p < end) {
        const char *line = ++p, *eol, *tab1, *tab2;
        size_t len;
        char *key, *text, *flags;
        if (p >= end) break;
        eol = (const char*)memchr(p, '\n', (size_t)(end - p));
        len = (size_t)((eol ? eol : end) - line);
        p = eol ? eol : end;
        if (len && line[len - 1] == '\r') len--;
        if (!len || line[0] == '#') continue;
        tab1 = (const char*)memchr(line, '\t', len);
        if (!tab1 || tab1 == line) continue;
        tab2 = (const char*)memchr(tab1 + 1, '\t', len - (size_t)(tab1 + 1 - line));
        key = unescaped(line, (size_t)(tab1 - line));
        text = unescaped(tab1 + 1, (size_t)((tab2 ? tab2 : line + len) - tab1 - 1));
        flags = tab2 ? unescaped(tab2 + 1, len - (size_t)(tab2 + 1 - line)) : NULL;
        if (key && text && text[0] && jpt_utf8_valid(key, strlen(key)) && jpt_utf8_valid(text, strlen(text))) {
            fn(ctx, key, text, flags ? flags : "");
            count++;
        }
        free(key);
        free(text);
        free(flags);
    }
    return count;
}

static char* to_codepage(const char* utf8, int codepage) {
    size_t cap = strlen(utf8) + 1;
    char* out = (char*)malloc(cap);
    if (out && jpt_utf8_to_cp(codepage, utf8, out, cap) == (size_t)-1) {
        free(out);
        out = NULL;
    }
    return out;
}

static void add(void* ctx, const char* key, const char* text, const char* flags) {
    tr_entry_t* e;
    (void)ctx;
    (void)flags;
    if (T.count == T.cap) {
        int cap = T.cap ? T.cap * 2 : 256;
        tr_entry_t* grown = (tr_entry_t*)realloc(T.entries, (size_t)cap * sizeof *grown);
        if (!grown) return;
        T.entries = grown;
        T.cap = cap;
    }
    e = &T.entries[T.count];
    e->key = _strdup(key);
    e->utf8 = _strdup(text);
    e->display = T.codepage ? to_codepage(text, T.codepage) : NULL;
    if (!e->key || !e->utf8) {
        free(e->key);
        free(e->utf8);
        free(e->display);
        return;
    }
    T.count++;
}

int jpt_tr_load(const char* data, size_t n, int codepage) {
    int count;
    jpt_tr_reset();
    T.codepage = codepage;
    count = jpt_tr_parse(data, n, add, NULL);
    if (count < 0) return -1;
    qsort(T.entries, (size_t)T.count, sizeof *T.entries, by_key);
    for (int i = 1; i < T.count; i++)
        if (!strcmp(T.entries[i].key, T.entries[i - 1].key)) {
            free(T.entries[i].key);
            free(T.entries[i].utf8);
            free(T.entries[i].display);
            memmove(&T.entries[i], &T.entries[i + 1], (size_t)(T.count - i - 1) * sizeof *T.entries);
            T.count--;
            i--;
        }
    return T.count;
}

void jpt_tr_tokens(const char* text, char* out, size_t cap) {
    size_t o = 0;
    if (!cap) return;
    out[0] = 0;
    for (const char* p = text; p && *p; p++) {
        const char* close;
        size_t len;
        if (*p != '<' || !(close = strchr(p, '>'))) continue;
        len = (size_t)(close - p) + 1;
        for (size_t i = 1; i + 1 < len; i++)
            if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9') || p[i] == '_')) len = 0;
        if (len < 3 || o + len + 2 > cap) continue;
        if (o) out[o++] = ' ';
        memcpy(out + o, p, len);
        o += len;
        out[o] = 0;
        p = close;
    }
}
