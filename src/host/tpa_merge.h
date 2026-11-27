/* One TPA made of several, as the game reads it: the samples of the first file, with those of the others added after them. A
 * sample of a later file stands for the one of the same ID before it. Nothing is written; every file is only read. */
#ifndef TPA_MERGE_H
#define TPA_MERGE_H

#include "stream_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    stream_read_fn read;
    void* ctx;
    uint64_t size;
} tpa_source_t;

typedef struct {
    int added, replaced, collisions;
    int refused; /* files that are not a TPA, or samples that lie outside their file */
} tpa_merge_stats_t;

typedef struct tpa_merge tpa_merge_t;

/* NULL when the first file is not a TPA. The sources' contexts must live as long as the result. */
tpa_merge_t* tpa_merge_open(const tpa_source_t* sources, int count, tpa_merge_stats_t* stats);
void tpa_merge_close(tpa_merge_t* merge);
uint64_t tpa_merge_size(const tpa_merge_t* merge);
/* As stream_pack_read. */
long tpa_merge_read(const tpa_merge_t* merge, uint64_t offset, void* buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
