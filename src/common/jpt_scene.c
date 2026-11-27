#include "jpt_scene.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_alloc.h"
#include "jpt_compat.h"
#include "jpt_save.h"

#define MAGIC 0x0ACEBABEu
#define FILE_HEADER 0x30u
#define SECTION_ENTRY 32u
#define SECTIONS_MAX 65536u
#define SYMBOL_HEAD 12u
#define HEADER_BYTES 0x514u
#define HEADER_BRIEF 0x504u
#define WORLD_PATH_MAX 260u
#define RENDER_OWN_VERSION 3u
#define RENDER_CE_VERSION 4u
#define WATER_OWN_VERSION 1u
#define WATER_CE_VERSION 2u
#define WATER_WORDS 11u
#define WATER_DECAY 5u /* two decays, then two speeds */
#define WATER_SPEED 7u
#define MESH_WORDS 7u
#define AI_HEAD_WORDS 5u
#define AI_HANDLES 10u
#define PLAYER_TAIL 236u
#define PLAYER_PLACEMENT 28u
#define PLAYER_STOW_BYTE 7u
#define PLAYER_STOW_SECOND 0x80u
#define PLAYER_SECOND_AT 120u /* the second stowed object's placement, from the player's own part */

typedef struct {
    const uint8_t* bytes;
    size_t size;
} span_t;

typedef struct {
    uint8_t* bytes;
    size_t size, cap;
    int failed;
} out_t;

static uint32_t rd(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put(out_t* o, const void* bytes, size_t size) {
    if (o->failed || size > SIZE_MAX - o->size) return;
    if (o->size + size > o->cap) {
        size_t cap = o->cap * 2 > o->size + size ? o->cap * 2 : o->size + size;
        uint8_t* grown = (uint8_t*)jpt_alloc_again(o->bytes, cap, 1);
        if (!grown) {
            o->failed = 1;
            return;
        }
        o->bytes = grown;
        o->cap = cap;
    }
    if (size) memcpy(o->bytes + o->size, bytes, size);
    o->size += size;
}

static void put32(out_t* o, uint32_t v) {
    uint8_t p[4];
    wr(p, v);
    put(o, p, 4);
}

static void put_float(out_t* o, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put32(o, bits);
}

static float as_float(uint32_t bits) {
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static int refuse(jpt_scene_report_t* r, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->problem, sizeof r->problem, fmt, ap);
    va_end(ap);
    return 0;
}

static void note(jpt_scene_report_t* r, const char* fmt, ...) {
    va_list ap;
    if (r->note_count == JPT_SCENE_NOTES_MAX) return;
    va_start(ap, fmt);
    vsnprintf(r->notes[r->note_count++], sizeof r->notes[0], fmt, ap);
    va_end(ap);
}

static int version_before(const uint8_t* p, size_t end) {
    size_t start = end;
    uint32_t version;
    while (start > 4 && end - start <= WORLD_PATH_MAX && p[start - 1] >= ' ' && p[start - 1] < 127) start--;
    version = rd(p + start - 4);
    return start < end && version && version <= 0xFF ? (int)version : 0;
}

/* The number in front of the first world's name; the header lies anywhere in the file. */
int jpt_scene_version(const void* bytes, size_t size) {
    const uint8_t* p = (const uint8_t*)bytes;
    for (size_t end = 4; end + 4 <= size; end++) {
        int version;
        if (p[end] != '.' || _strnicmp((const char*)p + end, ".grf", 4)) continue;
        if ((version = version_before(p, end)) != 0) return version;
    }
    return 0;
}

/* Words of a section, taken one by one; past the end everything is 0 and ok turns 0. */
typedef struct {
    span_t s;
    size_t at;
    int ok;
} words_t;

static uint32_t take(words_t* w) {
    if (w->at + 4 > w->s.size) {
        w->ok = 0;
        return 0;
    }
    w->at += 4;
    return rd(w->s.bytes + w->at - 4);
}

static void copy_words(words_t* w, out_t* o, unsigned count) {
    while (count--) put32(o, take(w));
}

static void skip_words(words_t* w, unsigned count) {
    while (count--) take(w);
}

/* The format 2 of the water block keeps per-second values where the first keeps 16.16 ones per 1/256 s. */
static int water(words_t* w, out_t* o, jpt_scene_report_t* r) {
    uint32_t version = take(w), word[WATER_WORDS];
    if (version == WATER_OWN_VERSION) {
        put32(o, version);
        copy_words(w, o, WATER_WORDS);
        return 1;
    }
    if (version != WATER_CE_VERSION) return refuse(r, "render section: water block version %u", version);
    if (take(w)) note(r, "water: a CE setting without a counterpart is left out");
    for (unsigned i = 0; i < WATER_WORDS; i++) word[i] = take(w);
    for (unsigned i = WATER_DECAY; i < WATER_SPEED; i++) {
        float decay = as_float(word[i]);
        if (!(decay >= 0.0f)) return refuse(r, "render section: water decay %g", (double)decay);
        decay = decay > 0.0f ? (float)(65536.0 * pow((double)decay, 1.0 / 256.0)) : 0.0f;
        memcpy(&word[i], &decay, 4);
    }
    put32(o, WATER_OWN_VERSION);
    for (unsigned i = 0; i < WATER_WORDS; i++) {
        if (i == WATER_SPEED || i == WATER_SPEED + 1u) put_float(o, as_float(word[i]) * 65536.0f);
        else put32(o, word[i]);
    }
    return 1;
}

static int render(span_t s, out_t* o, jpt_scene_report_t* r) {
    words_t w = { s, 0, 1 };
    uint32_t version = take(&w), block, mask;
    unsigned swapped = 0;
    if (version <= RENDER_OWN_VERSION && w.ok) {
        put(o, s.bytes, s.size);
        return 1;
    }
    if (version != RENDER_CE_VERSION) return refuse(r, "render section version %u", version);
    put32(o, RENDER_OWN_VERSION);
    put32(o, block = take(&w));
    if (block == 1) copy_words(&w, o, 15);
    copy_words(&w, o, 6);
    put32(o, block = take(&w));
    if (block == 2) copy_words(&w, o, 10);
    else if (block == 1) copy_words(&w, o, 22);
    copy_words(&w, o, 2);
    skip_words(&w, 1);
    mask = take(&w);
    if (mask & ~3u) return refuse(r, "render section: unknown extension 0x%X", mask);
    if (mask) note(r, "colour tables of CE's fog or water (0x%X) are left out", mask);
    skip_words(&w, (mask & 1u ? 0x80u / 4u : 0u) + (mask & 2u ? 0x100u / 4u : 0u));
    copy_words(&w, o, 5);
    if (!water(&w, o, r)) return 0;
    copy_words(&w, o, 5);
    while (w.ok && w.at < s.size) {
        uint32_t extra;
        copy_words(&w, o, MESH_WORDS);
        extra = take(&w);
        if (extra > (s.size - w.at) / 8u) return refuse(r, "render section: %u texture swaps in an animated mesh", extra);
        skip_words(&w, extra * 2u);
        swapped += extra != 0;
    }
    if (!w.ok) return refuse(r, "the render section ends inside a value");
    if (swapped) note(r, "swapped textures of %u animated mesh(es) are left out", swapped);
    return 1;
}

static int ai(span_t s, int version, out_t* o, jpt_scene_report_t* r) {
    words_t w = { s, 0, 1 };
    uint32_t active, listed;
    if (version < 25) {
        put(o, s.bytes, s.size);
        return 1;
    }
    copy_words(&w, o, AI_HEAD_WORDS);
    active = take(&w);
    if (active > s.size / 4u) return refuse(r, "AI section: %u active animals", active);
    for (uint32_t i = 0; i < active || i < AI_HANDLES; i++) {
        uint32_t handle = i < active ? take(&w) : 0;
        if (i < AI_HANDLES) put32(o, handle);
    }
    if (active > AI_HANDLES) note(r, "%u of %u active animals start inactive", active - AI_HANDLES, active);
    put32(o, listed = take(&w));
    if (listed > s.size / 4u) return refuse(r, "AI section: %u listed animals", listed);
    copy_words(&w, o, listed);
    if (!w.ok || w.at != s.size) return refuse(r, "AI section: %u bytes, %u read", (unsigned)s.size, (unsigned)w.at);
    return 1;
}

/* Bytes of the common part of an object that moves, up to its own values. */
static size_t common_bytes(span_t s, int version) {
    size_t n = 36;
    if (version < JPT_SCENE_VERSION_SCRIPTS) return n;
    if (s.size < n + 4) return s.size;
    if (!(rd(s.bytes + n) & 1u)) return n + 4;
    if (s.size < n + 8 || rd(s.bytes + n + 4) > s.size - n - 8) return s.size;
    return n + 8 + rd(s.bytes + n + 4);
}

static int player(span_t s, int version, out_t* o, jpt_scene_report_t* r) {
    size_t at = common_bytes(s, version), joints_at = at + 36, own;
    uint32_t joints;
    if (s.size < joints_at + 4) return refuse(r, "the player section is %u bytes", (unsigned)s.size);
    joints = rd(s.bytes + joints_at);
    if (joints > s.size / 48u) return refuse(r, "player section: %u joints", joints);
    own = joints_at + 4 + (size_t)joints * 48u;
    if (own + PLAYER_TAIL > s.size) return refuse(r, "the player section ends inside a value");
    if ((s.bytes[own + PLAYER_STOW_BYTE] & ~1u) != PLAYER_STOW_SECOND) {
        put(o, s.bytes, s.size);
        return 1;
    }
    if (own + PLAYER_TAIL + PLAYER_PLACEMENT > s.size) return refuse(r, "the player section ends inside a value");
    {
        const uint8_t stow = s.bytes[own + PLAYER_STOW_BYTE] & 0x7F;
        const size_t second = own + PLAYER_SECOND_AT;
        put(o, s.bytes, own + PLAYER_STOW_BYTE);
        put(o, &stow, 1);
        put(o, s.bytes + own + PLAYER_STOW_BYTE + 1, second - own - PLAYER_STOW_BYTE - 1);
        put(o, s.bytes + second + PLAYER_PLACEMENT, s.size - second - PLAYER_PLACEMENT);
    }
    note(r, "the second stowed object's own placement is left out");
    return 1;
}

static void ce_only(const char* name, span_t s, jpt_scene_report_t* r) {
    if (!strcmp(name, "190DAD34") && s.size >= 4 && rd(s.bytes)) note(r, "timed CE actions under way (0x%X) are left out", rd(s.bytes));
    if (!strcmp(name, "4B642136") && s.size >= 12 && (rd(s.bytes + 4) != 0xFFFFFFFFu || (s.bytes[8] & 1))) note(r, "keys a level turned off (CE) stay on");
    if (!strcmp(name, "1C1CA6D4") && s.size > 16) note(r, "the state of CE scripts (%u bytes) is left out", (unsigned)s.size);
}

static int section(const char* name, span_t s, int version, out_t* o, jpt_scene_report_t* r) {
    if (!strcmp(name, "4C56C6CC")) return render(s, o, r);
    if (!strcmp(name, "A9AC56AE")) return ai(s, version, o, r);
    if (!strcmp(name, "9FB57F53")) return player(s, version, o, r);
    ce_only(name, s, r);
    if (!strcmp(name, "Header")) {
        const size_t start = o->size;
        if (s.size < HEADER_BYTES) return refuse(r, "the header section is %u bytes", (unsigned)s.size);
        put(o, s.bytes, s.size);
        if (o->failed) return 1;
        if (version < JPT_SCENE_VERSION_SCRIPTS) wr(o->bytes + start, JPT_SCENE_VERSION_OWN);
        wr(o->bytes + start + HEADER_BRIEF, s.bytes[HEADER_BRIEF] ? 1u : 0u);
        return 1;
    }
    put(o, s.bytes, s.size);
    return 1;
}

typedef struct {
    span_t file;
    uint32_t sections, symbols, symbols_at, symbols_bytes;
} image_t;

static int open_image(const uint8_t* p, size_t size, image_t* f, jpt_scene_report_t* r) {
    uint64_t total = 0;
    f->file.bytes = p;
    f->file.size = size;
    if (size < FILE_HEADER || rd(p) != MAGIC) return refuse(r, "not a scene file");
    f->sections = rd(p + 8);
    f->symbols = rd(p + 12);
    f->symbols_bytes = rd(p + 16);
    f->symbols_at = rd(p + 20);
    if (!f->sections || f->sections > SECTIONS_MAX || f->symbols > SECTIONS_MAX || FILE_HEADER + (size_t)f->sections * SECTION_ENTRY > size)
        return refuse(r, "%u sections do not fit the file", f->sections);
    if (f->symbols_at > size || f->symbols_bytes > size - f->symbols_at) return refuse(r, "the symbol table lies outside the file");
    for (uint32_t i = 0; i < f->sections; i++) {
        const uint8_t* e = p + FILE_HEADER + (size_t)i * SECTION_ENTRY;
        const uint32_t at = rd(e + 8), bytes = rd(e + 12);
        if (at > size || bytes > size - at) return refuse(r, "section %u lies outside the file", i);
        if ((total += bytes) > size) return refuse(r, "its sections overlap");
        if (bytes > JPT_SAVE_SECTION_BYTES_MAX) return refuse(r, "section %u is %u bytes", i, bytes);
    }
    return 1;
}

static int section_name(const image_t* f, uint32_t key, char out[16]) {
    size_t at = f->symbols_at;
    const size_t end = (size_t)f->symbols_at + f->symbols_bytes;
    out[0] = 0;
    for (uint32_t i = 0; i < f->symbols; i++) {
        uint32_t length;
        if (at + SYMBOL_HEAD > end) return 0;
        length = rd(f->file.bytes + at + 4);
        if (length > end - at - SYMBOL_HEAD) return 0;
        if (rd(f->file.bytes + at) == key) {
            snprintf(out, 16, "%.*s", (int)(length < 15 ? length : 15), (const char*)f->file.bytes + at + SYMBOL_HEAD);
            return 1;
        }
        at += SYMBOL_HEAD + length;
    }
    return 0;
}

static int is_header(const char* name, span_t s, int version) { return !strcmp(name, "Header") && s.size >= HEADER_BYTES && (int)rd(s.bytes) == version; }

static uint8_t* rebuild(const image_t* f, int version, size_t* out_size, jpt_scene_report_t* r) {
    const size_t table = (size_t)f->sections * SECTION_ENTRY;
    out_t o = { NULL, 0, 0, 0 };
    int header = 0;
    put(&o, f->file.bytes, FILE_HEADER + table);
    for (uint32_t i = 0; i < f->sections && !o.failed; i++) {
        const uint8_t* e = f->file.bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY;
        const span_t s = { f->file.bytes + rd(e + 8), rd(e + 12) };
        const size_t start = o.size;
        char name[16];
        if (!section_name(f, rd(e), name)) {
            refuse(r, "section %u has no name", i);
            break;
        }
        header += is_header(name, s, version);
        if (!section(name, s, version, &o, r)) break;
        if (o.failed) break;
        wr(o.bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY + 8, (uint32_t)start);
        wr(o.bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY + 12, (uint32_t)(o.size - start));
    }
    if (!r->problem[0] && header != 1) refuse(r, "its header section is not version %d's", version);
    if (!r->problem[0] && o.failed) refuse(r, "out of memory");
    if (!r->problem[0]) {
        const size_t symbols_at = o.size;
        put(&o, f->file.bytes + f->symbols_at, f->symbols_bytes);
        if (o.failed || o.size > UINT32_MAX) refuse(r, "out of memory");
        else wr(o.bytes + 20, (uint32_t)symbols_at), wr(o.bytes + 4, (uint32_t)o.size);
    }
    if (r->problem[0]) {
        free(o.bytes);
        return NULL;
    }
    *out_size = o.size;
    return o.bytes;
}

uint8_t* jpt_scene_adapt(const void* bytes, size_t size, size_t* out_size, jpt_scene_report_t* report) {
    image_t f;
    jpt_scene_report_t own;
    uint8_t* adapted;
    if (!report) report = &own;
    memset(report, 0, sizeof *report);
    memset(&f, 0, sizeof f);
    *out_size = 0;
    report->version = jpt_scene_version(bytes, size);
    if (!report->version) return refuse(report, "its scene version could not be read"), NULL;
    if (report->version <= JPT_SCENE_VERSION_OWN) {
        jpt_save_state(bytes, size, report->problem, sizeof report->problem);
        return NULL;
    }
    if (report->version > JPT_SCENE_VERSION_MAX) return refuse(report, "scene version %d is newer than %d", report->version, JPT_SCENE_VERSION_MAX), NULL;
    if (!open_image((const uint8_t*)bytes, size, &f, report)) return NULL;
    note(report, "version %d: states of triggers with CE or ATX actions are read as far as the 1998 actions go", report->version);
    adapted = rebuild(&f, report->version, out_size, report);
    if (adapted && !jpt_save_state(adapted, *out_size, report->problem, sizeof report->problem)) {
        free(adapted);
        return NULL;
    }
    return adapted;
}

int jpt_scene_readable(const void* bytes, size_t size, jpt_scene_report_t* report) {
    jpt_scene_report_t own;
    size_t out = 0;
    if (!report) report = &own;
    free(jpt_scene_adapt(bytes, size, &out, report));
    return !report->problem[0];
}
