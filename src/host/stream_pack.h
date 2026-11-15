/* A virtual view of the player's retail Stream.tpa: the same directory and samples, with the corrected English captions and
 * the port's restored audio records applied. Nothing is written to disk; the retail file is only read. */
#ifndef STREAM_PACK_H
#define STREAM_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*stream_read_fn)(void* ctx, uint64_t offset, void* buf, size_t n); /* 1 when all n bytes were read */

typedef struct {
    uint32_t id;
    const uint8_t* record;  /* descriptor, caption block and payload, as a TPA stores a sample */
    size_t size;
    const char* replaces;   /* SHA-256 of the retail payload this record replaces; NULL or "-" for a sample retail lacks */
} stream_audio_t;

typedef struct {
    uint32_t id;
    const uint8_t* block;   /* a caption block: u32 length, u32 count, then count times { u32 units, text } */
    size_t size;
} stream_caption_t;

typedef struct {
    int captions_applied;
    int audio_added;
    int audio_replaced;
    int audio_kept;         /* the retail file already has the sample, or its audio is not the one the record replaces */
} stream_pack_stats_t;

typedef struct stream_pack stream_pack_t;

/* NULL when the file is not a TPA. `size` is the real file's size. */
stream_pack_t* stream_pack_open(stream_read_fn read, void* ctx, uint64_t size, const stream_audio_t* audio, int audio_count,
                                const stream_caption_t* captions, int caption_count, stream_pack_stats_t* stats);
void stream_pack_close(stream_pack_t* pack);
uint64_t stream_pack_size(const stream_pack_t* pack);
/* Copies up to n bytes at `offset`; returns the count (0 at the end of the file) or -1 when the real file cannot be read. */
long stream_pack_read(const stream_pack_t* pack, uint64_t offset, void* buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
