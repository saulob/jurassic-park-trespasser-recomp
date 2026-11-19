#ifndef TRESPASSER_SMACKER_BACKEND_H
#define TRESPASSER_SMACKER_BACKEND_H

#include <stdint.h>

/* The decoder behind the Smacker HLE: demuxing and decoding only. The HLE owns the guest ABI, the clock, the picture's
 * conversion and the audio's path into the mixer. A backend never touches guest memory. */

#define SMK_HEADER_BYTES 104u
#define SMK_TRACKS 7u
#define SMK_FLAG_RING 1u
#define SMK_FLAG_Y_INTERLACED 2u
#define SMK_FLAG_Y_DOUBLED 4u

typedef struct {
    uint32_t version;               /* 2 or 4: "SMK2" or "SMK4" */
    uint32_t width, height, frames; /* height as stored in the file */
    int32_t frame_rate;             /* > 0 ms per frame, < 0 tens of microseconds, 0 = 10 fps */
    uint32_t flags;
    struct {
        uint32_t rate; /* 0: the track is absent */
        uint8_t channels, bits;
    } track[SMK_TRACKS];
} smk_header_t;

/* Parses the 104-byte file header; 0 when it is not a Smacker file or has no picture. */
int smk_header_parse(const uint8_t* bytes, uint32_t size, smk_header_t* out);
uint32_t smk_frame_duration_us(int32_t frame_rate);
/* Rows the guest sees: Y-interlaced and Y-doubled pictures are shown at twice their stored height. */
uint32_t smk_shown_height(const smk_header_t* header);

/* Where the file bytes come from: positional reads, so one source feeds several decoders. */
typedef struct {
    void* context;
    uint64_t size;
    /* Bytes actually read: fewer than asked only at the end of the file, 0 on failure. */
    uint32_t (*read_at)(void* context, uint64_t offset, void* out, uint32_t bytes);
} smk_source_t;

enum { SMK_STREAM_VIDEO = 1, SMK_STREAM_AUDIO = 2 };

typedef struct smk_backend smk_backend_t;

typedef struct {
    const char* name;
    /* `streams` says what the instance decodes: the other kind is skipped without decoding. NULL on failure. */
    smk_backend_t* (*open)(const smk_source_t* source, const smk_header_t* header, unsigned streams);
    void (*close)(smk_backend_t* backend);
    /* The next picture in file order: width x height palette indexes and the 768 RGB bytes in force. 0 at the end. */
    int (*next_video)(smk_backend_t* backend, uint8_t* picture, uint8_t* palette);
    /* The next PCM of a track in file order, in the track format; 0 at its end. */
    uint32_t (*next_audio)(smk_backend_t* backend, uint32_t track, uint8_t* out, uint32_t capacity);
} smk_backend_ops_t;

/* A source over memory (tests and tools); the memory description must outlive the source. */
typedef struct {
    const uint8_t* bytes;
    uint32_t size;
} smk_memory_t;
smk_source_t smk_memory_source(const smk_memory_t* memory);

/* The FFmpeg backend, NULL in a build without FFmpeg (JPT_SMACKER_FFMPEG). */
const smk_backend_ops_t* smk_ffmpeg_backend(void);
/* The versions and licences of the libraries behind it; 0 and an empty string without FFmpeg. */
int smk_ffmpeg_describe(char* out, unsigned capacity);

#endif
