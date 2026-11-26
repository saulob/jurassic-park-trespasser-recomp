#include "jpt_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_alloc.h"
#include "jpt_compat.h"
#include "jpt_fs.h"
#include "jpt_szdd.h"

#define MAGIC 0x0ACEBABEu
#define FILE_VERSION 10u
#define FILE_HEADER 48u
#define SECTION_ENTRY 32u
#define SYMBOL_HEAD 12u
#define SYMBOL_MAX 256u
#define HEADER_BYTES_MIN (4u + JPT_SAVE_WORLDS * WORLD_FIELD)
#define WORLD_FIELD 256u
#define OLDEST_VERSION 7
#define AI_LIST 60u
#define PLAYER_JOINTS 72u
#define PLAYER_JOINT_BYTES 48u
#define JOINTS_MAX 32u
#define NEWEST_VERSION 22
#define FILE_LIMIT (64u << 20)
#define STAMP_MAGIC "JPTSAVE1"
#define STAMP_BYTES (8u + JPT_SAVE_PACKAGE_MAX + 4u)
#define STATE_MAGIC "JPTSTATE"
#define STATE_HEAD 12u
#define STATE_MAX 8192u
#define SCRIPTS_MAGIC "JPTSCRPT"
#define TEMP_NAME "~save.tmp"

static uint32_t s_buffer = JPT_SAVE_SECTION_BYTES_MAX;

void jpt_save_set_buffer(uint32_t bytes) { s_buffer = bytes; }

static uint32_t rd(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wr(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static const char* file_name(const char* path) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (!slash || (back && back > slash)) slash = back;
    return slash ? slash + 1 : path;
}

int jpt_save_is_name(const char* path) {
    const char* name = file_name(path);
    size_t digits = 0;
    if (_strnicmp(name, "SAVEGAME.", 9)) return 0;
    if (!_stricmp(name + 9, "QCK")) return 1;
    while (name[9 + digits] >= '0' && name[9 + digits] <= '9') digits++;
    return digits >= 3 && digits <= 5 && !name[9 + digits];
}

int jpt_save_is_temporary(const char* path) { return !_stricmp(file_name(path), "temp.scx"); }

static int wrong(char* problem, size_t cap, const char* text) {
    if (cap) snprintf(problem, cap, "%s", text);
    return 0;
}

typedef struct {
    const uint8_t* bytes;
    uint32_t sections;
    size_t symbols_at, end;
    uint32_t symbols;
} image_t;

/* The reader takes the sections one after another and the symbols after them; it never uses an offset. */
static int walk(const uint8_t* p, size_t size, image_t* f, char* problem, size_t cap) {
    size_t at;
    if (size < FILE_HEADER || rd(p) != MAGIC) return wrong(problem, cap, "it is not a scene file");
    if (rd(p + 32) != FILE_VERSION) return wrong(problem, cap, "its container has another version");
    f->bytes = p;
    f->sections = rd(p + 8);
    f->symbols = rd(p + 12);
    if (!f->sections || f->sections > (size - FILE_HEADER) / SECTION_ENTRY) return wrong(problem, cap, "its section table does not fit the file");
    at = FILE_HEADER + (size_t)f->sections * SECTION_ENTRY;
    for (uint32_t i = 0; i < f->sections; i++) {
        const uint32_t bytes = rd(p + FILE_HEADER + (size_t)i * SECTION_ENTRY + 12);
        if (bytes > s_buffer) return wrong(problem, cap, "a section is larger than the game's buffer");
        if (bytes > size - at) return wrong(problem, cap, "it is cut short");
        at += bytes;
    }
    f->symbols_at = at;
    if (f->symbols > (size - at) / SYMBOL_HEAD) return wrong(problem, cap, "it is cut short");
    for (uint32_t i = 0; i < f->symbols; i++) {
        uint32_t length;
        if (size - at < SYMBOL_HEAD) return wrong(problem, cap, "it is cut short");
        length = rd(p + at + 4);
        at += SYMBOL_HEAD;
        if (!length || length > SYMBOL_MAX || length > size - at || p[at + length - 1]) return wrong(problem, cap, "a name in it is damaged");
        at += length;
    }
    f->end = at;
    return 1;
}

static const char* symbol(const image_t* f, uint32_t key) {
    size_t at = f->symbols_at;
    for (uint32_t i = 0; i < f->symbols; i++) {
        if (rd(f->bytes + at) == key) return (const char*)f->bytes + at + SYMBOL_HEAD;
        at += SYMBOL_HEAD + rd(f->bytes + at + 4);
    }
    return "";
}

static const uint8_t* section(const image_t* f, const char* name, uint32_t* size) {
    size_t at = FILE_HEADER + (size_t)f->sections * SECTION_ENTRY;
    for (uint32_t i = 0; i < f->sections; i++) {
        const uint8_t* e = f->bytes + FILE_HEADER + (size_t)i * SECTION_ENTRY;
        if (!strcmp(symbol(f, rd(e)), name)) {
            *size = rd(e + 12);
            return f->bytes + at;
        }
        at += rd(e + 12);
    }
    return NULL;
}

int jpt_save_container(const void* bytes, size_t size, char* problem, size_t cap) {
    image_t f;
    return walk((const uint8_t*)bytes, size, &f, problem, cap);
}

/* Five words, ten handles, then the animals it lists. */
static int ai_fits(const uint8_t* p, uint32_t n) { return n >= AI_LIST + 4 && rd(p + AI_LIST) <= (n - AI_LIST - 4) / 4; }

/* The matrices of the player's joints are read into an array of its body's. */
static int player_fits(const uint8_t* p, uint32_t n) {
    return n >= PLAYER_JOINTS + 4 && rd(p + PLAYER_JOINTS) <= JOINTS_MAX && rd(p + PLAYER_JOINTS) <= (n - PLAYER_JOINTS - 4) / PLAYER_JOINT_BYTES;
}

static int counts_fit(const image_t* f, char* problem, size_t cap) {
    uint32_t n = 0;
    const uint8_t* p = section(f, "Header", &n);
    if (!p || n < HEADER_BYTES_MIN || rd(p) != NEWEST_VERSION) return 1;
    p = section(f, "A9AC56AE", &n);
    if (p && !ai_fits(p, n)) return wrong(problem, cap, "its animals do not fit their section");
    p = section(f, "9FB57F53", &n);
    if (p && !player_fits(p, n)) return wrong(problem, cap, "the player's joints do not fit their section");
    return 1;
}

int jpt_save_state(const void* bytes, size_t size, char* problem, size_t cap) {
    image_t f;
    return walk((const uint8_t*)bytes, size, &f, problem, cap) && counts_fit(&f, problem, cap);
}

static void read_stamp(const uint8_t* p, size_t size, jpt_save_info_t* out) {
    if (size < STAMP_BYTES || memcmp(p, STAMP_MAGIC, 8) || p[8 + JPT_SAVE_PACKAGE_MAX - 1]) return;
    out->stamped = 1;
    memcpy(out->package, p + 8, JPT_SAVE_PACKAGE_MAX);
    out->content = rd(p + 8 + JPT_SAVE_PACKAGE_MAX);
}

static int read_text(const uint8_t* p, uint32_t size, char out[JPT_SAVE_TEXT_MAX], size_t most) {
    const uint8_t* end = (const uint8_t*)memchr(p, 0, size);
    const char* name;
    if (!end) return 0;
    name = file_name((const char*)p);
    if (strlen(name) > most) return 0;
    snprintf(out, JPT_SAVE_TEXT_MAX, "%s", name);
    return 1;
}

int jpt_save_inspect(const void* bytes, size_t size, jpt_save_info_t* out) {
    char* problem = out->problem;
    const size_t cap = sizeof out->problem;
    const uint8_t *header, *part;
    uint32_t n = 0;
    image_t f;
    memset(out, 0, sizeof *out);
    if (!walk((const uint8_t*)bytes, size, &f, problem, cap) || !counts_fit(&f, problem, cap)) return 0;
    out->image_size = f.end;
    read_stamp(f.bytes + f.end, size - f.end, out);
    header = section(&f, "Header", &n);
    if (!header || n < HEADER_BYTES_MIN) return wrong(problem, cap, "it has no header");
    out->version = (int)rd(header);
    if (out->version < OLDEST_VERSION || out->version > NEWEST_VERSION) return wrong(problem, cap, "its version is not one of this game");
    for (int i = 0; i < JPT_SAVE_WORLDS; i++)
        if (!read_text(header + 4 + i * WORLD_FIELD, WORLD_FIELD, out->worlds[i], JPT_SAVE_TEXT_MAX - 1)) return wrong(problem, cap, "a world's name in it is damaged");
    part = section(&f, "SaveSCNName", &n);
    if (part && !read_text(part, n, out->scene, JPT_SAVE_LEVEL_NAME_MAX)) return wrong(problem, cap, "its level's name is damaged");
    part = section(&f, "SaveGameName", &n);
    if (part && (n > 64 || !memchr(part, 0, n))) return wrong(problem, cap, "its name is damaged");
    if (!section(&f, "Partitions", &n) || !section(&f, "Hierarchy", &n)) return wrong(problem, cap, "it has no objects");
    return 1;
}

int jpt_save_worlds(const void* bytes, size_t size, char worlds[JPT_SAVE_WORLDS][JPT_SAVE_TEXT_MAX]) {
    const uint8_t* header;
    uint32_t n = 0;
    image_t f;
    if (!walk((const uint8_t*)bytes, size, &f, NULL, 0)) return 0;
    header = section(&f, "Header", &n);
    if (!header || n < HEADER_BYTES_MIN) return 0;
    for (int i = 0; i < JPT_SAVE_WORLDS; i++)
        if (!read_text(header + 4 + i * WORLD_FIELD, WORLD_FIELD, worlds[i], JPT_SAVE_TEXT_MAX - 1)) return 0;
    return 1;
}

static uint8_t* read_whole(const char* path, size_t* size) {
    uint64_t length = 0;
    jpt_fs_file_t* file = jpt_fs_open_read(path, &length);
    uint8_t* bytes = file && length && length <= FILE_LIMIT ? (uint8_t*)jpt_alloc((size_t)length, 1) : NULL;
    if (bytes && !jpt_fs_read_at(file, 0, bytes, (size_t)length)) {
        free(bytes);
        bytes = NULL;
    }
    if (file) jpt_fs_close(file);
    *size = bytes ? (size_t)length : 0;
    return bytes;
}

int jpt_save_inspect_file(const char* path, jpt_save_info_t* out) {
    size_t size = 0;
    uint8_t* bytes = read_whole(path, &size);
    int ok;
    if (bytes && jpt_szdd_is(bytes, size)) {
        uint8_t* expanded = jpt_szdd_expand(bytes, size, &size);
        free(bytes);
        bytes = expanded;
    }
    if (!bytes) {
        memset(out, 0, sizeof *out);
        return wrong(out->problem, sizeof out->problem, "it cannot be read");
    }
    ok = jpt_save_inspect(bytes, size, out);
    free(bytes);
    return ok;
}

int jpt_save_belongs(const jpt_save_info_t* save, const char* package, uint32_t content, char* why, size_t cap) {
    if (!save->stamped) return 1;
    if (strcmp(save->package, package ? package : "")) {
        if (cap) snprintf(why, cap, "save incompatible with current custom level: it belongs to %s", save->package);
        return 0;
    }
    if (save->content != content) {
        if (cap) snprintf(why, cap, "save incompatible with current custom level: the files of %s changed since", save->package);
        return 0;
    }
    return 1;
}

int jpt_save_begin(const char* path, char* temp, size_t cap) {
    const int folder = (int)(file_name(path) - path);
    int n;
    if (!jpt_save_is_name(path) && !jpt_save_is_temporary(path)) return 0;
    n = snprintf(temp, cap, "%.*s" TEMP_NAME, folder, path);
    return n > 0 && (size_t)n < cap;
}

static jpt_save_state_fn s_write_state;
static jpt_save_scripts_fn s_write_scripts;

void jpt_save_set_state(jpt_save_state_fn write) { s_write_state = write; }
void jpt_save_set_scripts(jpt_save_scripts_fn write) { s_write_scripts = write; }

static int append_scripts(FILE* f) {
    uint8_t head[STATE_HEAD];
    size_t size = 0;
    uint8_t* bytes = s_write_scripts ? s_write_scripts(&size) : NULL;
    int ok = 1;
    if (bytes && size && size <= JPT_SAVE_SCRIPTS_MAX) {
        memcpy(head, SCRIPTS_MAGIC, 8);
        wr(head + 8, (uint32_t)size);
        ok = fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(bytes, 1, size, f) == size;
    }
    free(bytes);
    return ok;
}

static int append_state(FILE* f) {
    uint8_t bytes[STATE_HEAD + STATE_MAX];
    const size_t size = s_write_state ? s_write_state(bytes + STATE_HEAD, STATE_MAX) : 0;
    if (!size) return 1;
    memcpy(bytes, STATE_MAGIC, 8);
    wr(bytes + 8, (uint32_t)size);
    return fwrite(bytes, 1, STATE_HEAD + size, f) == STATE_HEAD + size;
}

size_t jpt_save_read_state(const char* path, uint8_t* out, size_t cap) {
    size_t size = 0, at, length = 0;
    uint8_t* bytes = read_whole(path, &size);
    jpt_save_info_t info;
    if (bytes && jpt_save_inspect(bytes, size, &info) && info.stamped && (at = info.image_size + STAMP_BYTES) + STATE_HEAD <= size && !memcmp(bytes + at, STATE_MAGIC, 8)) {
        length = rd(bytes + at + 8);
        if (length > cap || length > size - at - STATE_HEAD) length = 0;
        memcpy(out, bytes + at + STATE_HEAD, length);
    }
    free(bytes);
    return length;
}

uint8_t* jpt_save_read_scripts(const char* path, size_t* size, char* problem, size_t cap) {
    size_t total = 0, at;
    uint8_t *bytes = read_whole(path, &total), *out = NULL;
    jpt_save_info_t info;
    *size = 0;
    if (cap) problem[0] = 0;
    if (!bytes || !jpt_save_inspect(bytes, total, &info) || !info.stamped || (at = info.image_size + STAMP_BYTES) > total) {
        free(bytes);
        return NULL;
    }
    if (total - at >= STATE_HEAD && !memcmp(bytes + at, STATE_MAGIC, 8)) {
        const uint32_t length = rd(bytes + at + 8);
        at = length <= total - at - STATE_HEAD ? at + STATE_HEAD + length : total;
    }
    if (total - at >= 8 && !memcmp(bytes + at, SCRIPTS_MAGIC, 8)) {
        const uint32_t length = total - at >= STATE_HEAD ? rd(bytes + at + 8) : 0;
        if (total - at < STATE_HEAD || length > total - at - STATE_HEAD) snprintf(problem, cap, "its script block is truncated");
        else if (!length || length > JPT_SAVE_SCRIPTS_MAX) snprintf(problem, cap, "its script block has a size of %u bytes", length);
        else if ((out = (uint8_t*)malloc(length)) != NULL) {
            memcpy(out, bytes + at + STATE_HEAD, length);
            *size = length;
        }
    }
    free(bytes);
    return out;
}

static int append_stamp(const char* path, const char* package, uint32_t content) {
    uint8_t stamp[STAMP_BYTES];
    FILE* f = fopen(path, "ab");
    int ok;
    if (!f) return 0;
    memset(stamp, 0, sizeof stamp);
    memcpy(stamp, STAMP_MAGIC, 8);
    snprintf((char*)stamp + 8, JPT_SAVE_PACKAGE_MAX, "%s", package);
    wr(stamp + 8 + JPT_SAVE_PACKAGE_MAX, content);
    ok = fwrite(stamp, 1, sizeof stamp, f) == sizeof stamp && append_state(f) && append_scripts(f);
    return fclose(f) == 0 && ok;
}

int jpt_save_finish(const char* path, const char* temp, const char* package, jpt_save_content_fn content, char* problem, size_t cap) {
    jpt_save_info_t info;
    int ok = jpt_save_inspect_file(temp, &info);
    if (!ok) wrong(problem, cap, info.problem);
    if (ok && package && package[0] && !append_stamp(temp, package, content ? content(&info) : 0)) ok = wrong(problem, cap, "it could not be written whole");
    if (ok && !jpt_fs_replace(temp, path)) ok = wrong(problem, cap, "it could not be put in place of the previous one");
    if (!ok) jpt_fs_remove(temp);
    return ok;
}
