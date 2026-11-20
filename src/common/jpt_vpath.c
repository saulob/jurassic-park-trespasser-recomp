#include "jpt_vpath.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"

#define MAX_DRIVES 8
#define MAX_DIRS 128
#define MAX_ELEMENTS 64
#define MAX_NAME 255

typedef struct {
    char* host_dir;
    char** names;
    int count;
} dir_cache_t;

struct jpt_vpath {
    char drive[MAX_DRIVES];
    char* host_root[MAX_DRIVES];
    int ndrives;
    char cwd[JPT_PATH_MAX];
    dir_cache_t dirs[MAX_DIRS];
    int next_slot;
    unsigned listings, hits;
};

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (fold((unsigned char)*a) != fold((unsigned char)*b)) return 0;
    return *a == *b;
}

jpt_vpath_t* jpt_vpath_new(void) { return (jpt_vpath_t*)calloc(1, sizeof(jpt_vpath_t)); }

static void free_names(dir_cache_t* d) {
    for (int i = 0; i < d->count; i++) free(d->names[i]);
    free(d->names);
    d->names = NULL;
    d->count = 0;
}

static void free_dir(dir_cache_t* d) {
    free_names(d);
    free(d->host_dir);
    d->host_dir = NULL;
}

void jpt_vpath_free(jpt_vpath_t* v) {
    if (!v) return;
    for (int i = 0; i < MAX_DIRS; i++) free_dir(&v->dirs[i]);
    for (int i = 0; i < v->ndrives; i++) free(v->host_root[i]);
    free(v);
}

static char drive_letter(char c) { return (char)((c >= 'a' && c <= 'z') ? c - 32 : c); }

static int drive_index(const jpt_vpath_t* v, char drive) {
    for (int i = 0; i < v->ndrives; i++)
        if (v->drive[i] == drive_letter(drive)) return i;
    return -1;
}

int jpt_vpath_mount(jpt_vpath_t* v, char drive, const char* host_dir) {
    size_t n = strlen(host_dir);
    char* copy;
    drive = drive_letter(drive);
    if (drive < 'A' || drive > 'Z' || !n || drive_index(v, drive) >= 0 || v->ndrives == MAX_DRIVES) return 0;
    copy = (char*)malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, host_dir, n + 1);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = 0;
    v->drive[v->ndrives] = drive;
    v->host_root[v->ndrives++] = copy;
    if (!v->cwd[0]) snprintf(v->cwd, sizeof v->cwd, "%c:\\", drive);
    return 1;
}

const char* jpt_vpath_cwd(const jpt_vpath_t* v) { return v->cwd; }

static int is_sep(char c) { return c == '\\' || c == '/'; }

typedef struct {
    char drive;
    int absolute;
    char* elem[MAX_ELEMENTS];
    int count;
    char storage[JPT_PATH_MAX + MAX_ELEMENTS];
} split_t;

static int split_path(const char* path, split_t* s) {
    size_t n = strlen(path);
    char* out = s->storage;
    const char* p = path;
    memset(s, 0, sizeof *s);
    if (!n || n >= JPT_PATH_MAX) return JPT_VP_BAD_PATH;
    for (const char* q = path; *q; q++)
        if ((unsigned char)*q < 32) return JPT_VP_BAD_PATH;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        s->drive = drive_letter(p[0]);
        p += 2;
        if (is_sep(*p)) s->absolute = 1;
    } else if (is_sep(*p)) {
        s->absolute = 1;
    }
    while (*p) {
        char* start;
        while (is_sep(*p)) p++;
        if (!*p) break;
        if (s->count == MAX_ELEMENTS) return JPT_VP_BAD_PATH;
        start = out;
        while (*p && !is_sep(*p)) *out++ = *p++;
        *out++ = 0;
        if (out - start - 1 > MAX_NAME) return JPT_VP_BAD_PATH;
        s->elem[s->count++] = start;
    }
    return JPT_VP_OK;
}

/* The guest's absolute path as a drive and normalised elements, "." and ".." applied; ".." past the drive's root fails. */
static int normalise(const jpt_vpath_t* v, const char* path, char* drive, char names[][MAX_NAME + 1], int* count) {
    split_t s, base;
    int n = 0, r;
    r = split_path(path, &s);
    if (r) return r;
    *drive = s.drive;
    if (!s.drive || !s.absolute) {
        r = split_path(v->cwd, &base);
        if (r) return r;
        if (!s.drive) *drive = base.drive;
        if (!s.absolute && (!s.drive || s.drive == base.drive))
            for (int i = 0; i < base.count; i++) strcpy(names[n++], base.elem[i]);
    }
    for (int i = 0; i < s.count; i++) {
        if (!strcmp(s.elem[i], ".")) continue;
        if (!strcmp(s.elem[i], "..")) {
            if (!n) return JPT_VP_ESCAPE;
            n--;
            continue;
        }
        if (n == MAX_ELEMENTS) return JPT_VP_BAD_PATH;
        strcpy(names[n++], s.elem[i]);
    }
    *count = n;
    return JPT_VP_OK;
}

int jpt_vpath_chdir(jpt_vpath_t* v, const char* guest_dir) {
    char drive, names[MAX_ELEMENTS][MAX_NAME + 1], text[JPT_PATH_MAX];
    int n, r;
    size_t len;
    r = normalise(v, guest_dir, &drive, names, &n);
    if (r || drive_index(v, drive) < 0) return 0;
    len = (size_t)snprintf(text, sizeof text, "%c:\\", drive);
    for (int i = 0; i < n; i++) {
        int wrote = snprintf(text + len, sizeof text - len, "%s\\", names[i]);
        if (wrote < 0 || len + (size_t)wrote >= sizeof text) return 0;
        len += (size_t)wrote;
    }
    strcpy(v->cwd, text);
    return 1;
}

static int dir_slot(const jpt_vpath_t* v, const char* host_dir) {
    for (int i = 0; i < MAX_DIRS; i++)
        if (v->dirs[i].host_dir && !strcmp(v->dirs[i].host_dir, host_dir)) return i;
    return -1;
}

typedef struct {
    char** names;
    int count, cap;
} collect_t;

static void collect(const char* name, int is_dir, void* user) {
    collect_t* c = (collect_t*)user;
    char* copy;
    (void)is_dir;
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        char** grown = (char**)realloc(c->names, (size_t)cap * sizeof *grown);
        if (!grown) return;
        c->names = grown;
        c->cap = cap;
    }
    copy = (char*)malloc(strlen(name) + 1);
    if (!copy) return;
    strcpy(copy, name);
    c->names[c->count++] = copy;
}

static int list_dir(jpt_vpath_t* v, const char* host_dir) {
    collect_t c = { NULL, 0, 0 };
    int slot = dir_slot(v, host_dir);
    if (!jpt_fs_list(host_dir, collect, &c)) {
        if (slot >= 0) free_dir(&v->dirs[slot]);
        return -1;
    }
    if (slot < 0) {
        slot = v->next_slot;
        v->next_slot = (v->next_slot + 1) % MAX_DIRS;
        free_dir(&v->dirs[slot]);
        v->dirs[slot].host_dir = (char*)malloc(strlen(host_dir) + 1);
        if (!v->dirs[slot].host_dir) {
            for (int i = 0; i < c.count; i++) free(c.names[i]);
            free(c.names);
            return -1;
        }
        strcpy(v->dirs[slot].host_dir, host_dir);
    } else {
        free_names(&v->dirs[slot]);
    }
    v->dirs[slot].names = c.names;
    v->dirs[slot].count = c.count;
    v->listings++;
    return slot;
}

static const char* find_in(const dir_cache_t* d, const char* name) {
    const char* folded = NULL;
    for (int i = 0; i < d->count; i++) {
        if (!strcmp(d->names[i], name)) return d->names[i];
        if (!folded && same_name(d->names[i], name)) folded = d->names[i];
    }
    return folded;
}

/* The entry of host_dir that matches name under case folding; the listing is refreshed once when the cache misses. */
static const char* lookup(jpt_vpath_t* v, const char* host_dir, const char* name) {
    int slot = dir_slot(v, host_dir);
    if (slot >= 0) {
        const char* hit = find_in(&v->dirs[slot], name);
        if (hit) {
            v->hits++;
            return hit;
        }
    }
    slot = list_dir(v, host_dir);
    return slot < 0 ? NULL : find_in(&v->dirs[slot], name);
}

void jpt_vpath_invalidate(jpt_vpath_t* v, const char* host_dir) {
    int slot = dir_slot(v, host_dir);
    if (slot >= 0) free_dir(&v->dirs[slot]);
}

void jpt_vpath_stats(const jpt_vpath_t* v, unsigned* listings, unsigned* cache_hits) {
    if (listings) *listings = v->listings;
    if (cache_hits) *cache_hits = v->hits;
}

int jpt_vpath_resolve(jpt_vpath_t* v, const char* guest_path, int create, char* host_path, size_t cap) {
    char drive, names[MAX_ELEMENTS][MAX_NAME + 1], host[JPT_PATH_MAX];
    int n = 0, r, di;
    size_t len;
    if (!guest_path || !host_path || !cap) return JPT_VP_BAD_PATH;
    r = normalise(v, guest_path, &drive, names, &n);
    if (r) return r;
    di = drive_index(v, drive);
    if (di < 0) return JPT_VP_BAD_PATH;
    len = strlen(v->host_root[di]);
    if (len >= sizeof host) return JPT_VP_NO_SPACE;
    memcpy(host, v->host_root[di], len + 1);
    for (int i = 0; i < n; i++) {
        const char* use = lookup(v, host, names[i]);
        if (!use) {
            if (!(create && i == n - 1)) return JPT_VP_NOT_FOUND;
            use = names[i];
        }
        if (len + 1 + strlen(use) >= sizeof host) return JPT_VP_NO_SPACE;
        if (len != 1 || host[0] != '/') host[len++] = '/';
        strcpy(host + len, use);
        len += strlen(use);
    }
    if (len >= cap) return JPT_VP_NO_SPACE;
    memcpy(host_path, host, len + 1);
    return JPT_VP_OK;
}
