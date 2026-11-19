#include <string.h>

#include "smacker_backend.h"

#define HEADER_RATE_TRACK0 0x48u
#define TRACK_PRESENT 0x40000000u
#define TRACK_16_BIT 0x20000000u
#define TRACK_STEREO 0x10000000u
#define TRACK_RATE_MASK 0x00FFFFFFu

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int smk_header_parse(const uint8_t* bytes, uint32_t size, smk_header_t* out) {
    memset(out, 0, sizeof *out);
    if (!bytes || size < SMK_HEADER_BYTES || memcmp(bytes, "SMK", 3) || (bytes[3] != '2' && bytes[3] != '4')) return 0;
    out->version = (uint32_t)(bytes[3] - '0');
    out->width = le32(bytes + 0x04);
    out->height = le32(bytes + 0x08);
    out->frames = le32(bytes + 0x0C);
    out->frame_rate = (int32_t)le32(bytes + 0x10);
    out->flags = le32(bytes + 0x14);
    for (uint32_t t = 0; t < SMK_TRACKS; t++) {
        uint32_t field = le32(bytes + HEADER_RATE_TRACK0 + t * 4u);
        if (!(field & TRACK_PRESENT)) continue;
        out->track[t].rate = field & TRACK_RATE_MASK;
        out->track[t].bits = (field & TRACK_16_BIT) ? 16 : 8;
        out->track[t].channels = (field & TRACK_STEREO) ? 2 : 1;
    }
    return out->width > 0 && out->height > 0 && out->frames > 0 && out->width <= 4096 && out->height <= 4096;
}

uint32_t smk_frame_duration_us(int32_t frame_rate) {
    if (frame_rate > 0) return (uint32_t)frame_rate * 1000u;
    if (frame_rate < 0) return (uint32_t)(-(int64_t)frame_rate) * 10u;
    return 100000u;
}

uint32_t smk_shown_height(const smk_header_t* header) {
    return (header->flags & (SMK_FLAG_Y_INTERLACED | SMK_FLAG_Y_DOUBLED)) ? header->height * 2u : header->height;
}

static uint32_t memory_read(void* context, uint64_t offset, void* out, uint32_t bytes) {
    const smk_memory_t* m = (const smk_memory_t*)context;
    if (offset >= m->size) return 0;
    if (bytes > m->size - offset) bytes = (uint32_t)(m->size - offset);
    memcpy(out, m->bytes + offset, bytes);
    return bytes;
}

smk_source_t smk_memory_source(const smk_memory_t* memory) {
    smk_source_t source;
    source.context = (void*)memory;
    source.size = memory->size;
    source.read_at = memory_read;
    return source;
}
