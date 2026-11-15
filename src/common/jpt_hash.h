#ifndef JPT_HASH_H
#define JPT_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
} jpt_sha256_t;

void jpt_sha256_init(jpt_sha256_t* ctx);
void jpt_sha256_update(jpt_sha256_t* ctx, const void* data, size_t n);
/* 64 lowercase hex digits and a NUL. */
void jpt_sha256_hex(jpt_sha256_t* ctx, char out[65]);
void jpt_sha256_buffer_hex(const void* data, size_t n, char out[65]);

/* The game's sample ID: the CRC-32 of the sample name in lower case. */
uint32_t jpt_sample_id(const char* name);

#ifdef __cplusplus
}
#endif

#endif
