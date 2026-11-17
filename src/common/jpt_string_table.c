#include "jpt_string_table.h"

#include <string.h>

#define HEADER 12u

static uint32_t get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int jpt_string_table_read(const uint8_t* data, size_t size, jpt_string_table_fn fn, void* user) {
    uint32_t count;
    size_t at = HEADER;
    if (!data || size < HEADER || get32(data) != JPT_STRING_TABLE_MAGIC || get32(data + 4) != JPT_STRING_TABLE_VERSION) return -1;
    count = get32(data + 8);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id, length;
        if (size - at < 8) return -1;
        id = get32(data + at);
        length = get32(data + at + 4);
        at += 8;
        if (length > size - at) return -1;
        fn(user, id, (const char*)data + at, length);
        at += length;
    }
    return (int)count;
}

size_t jpt_string_table_size(uint32_t count, size_t text_bytes) { return HEADER + 8u * (size_t)count + text_bytes; }

size_t jpt_string_table_write(uint8_t* out, size_t cap, const uint32_t* ids, const char* const* texts,
                              const uint32_t* lengths, uint32_t count) {
    size_t total = 0, at = HEADER;
    for (uint32_t i = 0; i < count; i++) total += lengths[i];
    if (cap < jpt_string_table_size(count, total)) return 0;
    put32(out, JPT_STRING_TABLE_MAGIC);
    put32(out + 4, JPT_STRING_TABLE_VERSION);
    put32(out + 8, count);
    for (uint32_t i = 0; i < count; i++) {
        put32(out + at, ids[i]);
        put32(out + at + 4, lengths[i]);
        memcpy(out + at + 8, texts[i], lengths[i]);
        at += 8u + lengths[i];
    }
    return at;
}
