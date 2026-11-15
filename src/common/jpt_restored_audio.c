#include "jpt_restored_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_text(char* out, size_t cap, const char* value, size_t n) {
    if (n >= cap) return 0;
    memcpy(out, value, n);
    out[n] = 0;
    return 1;
}

static int set_field(jpt_restored_entry_t* e, const char* key, size_t key_n, const char* value, size_t n) {
    char number[32];
#define KEY(k) (key_n == sizeof k - 1 && !memcmp(key, k, key_n))
    if (KEY("name")) return copy_text(e->name, sizeof e->name, value, n);
    if (KEY("file")) return copy_text(e->file, sizeof e->file, value, n);
    if (KEY("sha256")) return copy_text(e->sha256, sizeof e->sha256, value, n);
    if (KEY("codec")) return copy_text(e->codec, sizeof e->codec, value, n);
    if (KEY("replaces")) return copy_text(e->replaces, sizeof e->replaces, value, n);
    if (n >= sizeof number) return 0;
    memcpy(number, value, n);
    number[n] = 0;
    if (KEY("record")) return e->record_size = strtoul(number, NULL, 10), 1;
    if (KEY("payload")) return e->payload_size = strtoul(number, NULL, 10), 1;
    if (KEY("format")) return e->format = strtoul(number, NULL, 0), 1;
    if (KEY("channels")) return e->channels = atoi(number), 1;
    if (KEY("bits")) return e->bits = atoi(number), 1;
    if (KEY("rate")) return e->rate = strtoul(number, NULL, 10), 1;
    if (KEY("duration_ms")) return e->duration_ms = strtoul(number, NULL, 10), 1;
#undef KEY
    return 0;
}

static int parse_line(const char* line, size_t n, jpt_restored_entry_t* e) {
    const char* end = line + n;
    const char* tab = memchr(line, '\t', n);
    char id[16];
    memset(e, 0, sizeof *e);
    if (!tab || tab - line != 6 || memcmp(line, "sample", 6)) return 0;
    line = tab + 1;
    tab = memchr(line, '\t', (size_t)(end - line));
    if (!tab || tab - line != 8) return 0;
    memcpy(id, line, 8);
    id[8] = 0;
    e->id = (uint32_t)strtoul(id, NULL, 16);
    line = tab + 1;
    while (line < end) {
        const char *next = memchr(line, '\t', (size_t)(end - line)), *eq;
        size_t len = next ? (size_t)(next - line) : (size_t)(end - line);
        eq = memchr(line, '=', len);
        if (!eq || !set_field(e, line, (size_t)(eq - line), eq + 1, len - (size_t)(eq - line) - 1)) return 0;
        line = next ? next + 1 : end;
    }
    return e->name[0] && e->file[0] && e->record_size && e->payload_size && strlen(e->sha256) == 64 && e->replaces[0];
}

int jpt_restored_parse(const char* data, size_t n, jpt_restored_entry_t* out, int cap) {
    const char* end = data + n;
    size_t header = strlen(JPT_RESTORED_HEADER);
    int count = 0;
    if (n < header || memcmp(data, JPT_RESTORED_HEADER, header)) return -1;
    data += header;
    while (data < end) {
        const char* eol = memchr(data, '\n', (size_t)(end - data));
        size_t len = eol ? (size_t)(eol - data) : (size_t)(end - data);
        if (len && data[len - 1] == '\r') len--;
        if (len) {
            if (count == cap || !parse_line(data, len, &out[count])) return -1;
            count++;
        }
        data += len + (eol ? (size_t)(eol - data) - len + 1 : 0);
    }
    return count;
}
