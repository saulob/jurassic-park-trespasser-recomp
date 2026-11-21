/* Stream.tpa as the game reads it, independent of the platform: one virtual file object per handle the game opens (its own
 * position and lifetime), all of them over the same catalog of corrected captions and restored audio (stream_pack.h).
 * The platform keeps the handle table and the lock; this layer has none. */
#ifndef STREAM_VFILE_H
#define STREAM_VFILE_H

#include <stdint.h>

#include "stream_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*stream_log_fn)(const char* line);
typedef struct stream_vfile stream_vfile_t;

/* Loads the embedded restored audio and the English captions. Idempotent; call it once at start-up, before other threads open files. */
void stream_catalog_init(stream_log_fn log);

/* NULL when the file is not a TPA or STREAM_PACK_MAX_VIEWS views are open. `real_size` is the real file's size; `read` reads the real file. */
stream_vfile_t* stream_vfile_open(stream_read_fn read, void* context, uint64_t real_size);
void stream_vfile_close(stream_vfile_t* file);

/* 1 and the byte count (0 at the end); 0 when the real file cannot be read. The position advances by the count. */
int stream_vfile_read(stream_vfile_t* file, void* buffer, uint32_t wanted, uint32_t* got);
/* method: 0 begin, 1 current, 2 end. 0 for a bad method or a negative target, which leave the position alone. */
int stream_vfile_seek(stream_vfile_t* file, int64_t distance, uint32_t method, uint64_t* position);
uint64_t stream_vfile_size(const stream_vfile_t* file);
uint64_t stream_vfile_position(const stream_vfile_t* file);

/* A pack over the real file with the same catalog, for a reader that is not a game handle (the caption scan); free with stream_pack_close. */
stream_pack_t* stream_catalog_open_pack(stream_read_fn read, void* context, uint64_t real_size);

/* Views open now (leak checks) and the number ever opened. */
unsigned stream_vfile_live(void);
unsigned stream_vfile_opened_total(void);
int stream_catalog_audio_count(void);
int stream_catalog_caption_count(void);

#ifdef __cplusplus
}
#endif

#endif
