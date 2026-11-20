#ifndef TRESPASSER_AUDIO_MIXER_H
#define TRESPASSER_AUDIO_MIXER_H

#include <stddef.h>
#include <stdint.h>

/* A small deterministic software mixer for the game's sound buffers and the videos' PCM streams: PCM that plays, loops,
 * streams and is written through locked regions. No SDL, no Win32: the SDL backend pulls frames from it and tests call
 * it directly. */

typedef struct audio_mixer audio_mixer_t;
typedef struct audio_voice audio_voice_t;
typedef struct audio_stream audio_stream_t;

typedef struct {
    uint32_t rate;
    uint16_t channels; /* 1 or 2 */
    uint16_t bits;     /* 8 or 16 */
} audio_format_t;

/* Lock and unlock around state shared with the mixing thread; both may be NULL when only one thread uses the mixer. */
typedef struct {
    void* context;
    void (*lock)(void* context);
    void (*unlock)(void* context);
} audio_lock_ops_t;

enum { AUDIO_STATUS_PLAYING = 1, AUDIO_STATUS_LOOPING = 4 };

audio_mixer_t* audio_mixer_create(uint32_t output_rate, const audio_lock_ops_t* ops);
/* Frees the mixer and every voice still attached to it. */
void audio_mixer_destroy(audio_mixer_t* mixer);
uint32_t audio_mixer_rate(const audio_mixer_t* mixer);
void audio_mixer_set_rate(audio_mixer_t* mixer, uint32_t rate);
void audio_mixer_set_master_gain(audio_mixer_t* mixer, float gain);
unsigned audio_mixer_voice_count(const audio_mixer_t* mixer);
unsigned audio_mixer_stream_count(const audio_mixer_t* mixer);
/* Interleaved stereo float frames, overwritten with the mix of every playing voice, clipped to [-1, 1]. */
void audio_mixer_mix(audio_mixer_t* mixer, float* out, uint32_t frames);

/* A valid format only: 1 or 2 channels, 8 or 16 bits, a rate of at least 1 Hz. */
int audio_format_valid(const audio_format_t* format);
uint32_t audio_format_block(const audio_format_t* format);

/* Buffer memory is owned by the voice and stays valid until audio_voice_destroy, zero (silence for 16 bits) filled. A
 * duplicate shares the source's memory and has its own state. */
audio_voice_t* audio_voice_create(audio_mixer_t* mixer, const audio_format_t* format, uint32_t bytes);
/* The same over memory the caller owns (guest memory, so that Lock can return a guest address): `bytes` long, silence
 * filled by the call, handed to `release` when the last voice sharing it is gone. */
typedef struct {
    uint8_t* bytes;
    void (*release)(void* context);
    void* context;
} audio_memory_t;
audio_voice_t* audio_voice_create_in(audio_mixer_t* mixer, const audio_format_t* format, uint32_t bytes,
                                     const audio_memory_t* memory);
audio_voice_t* audio_voice_duplicate(audio_mixer_t* mixer, const audio_voice_t* source);
void audio_voice_destroy(audio_voice_t* voice);

uint8_t* audio_voice_data(audio_voice_t* voice);
uint32_t audio_voice_size(const audio_voice_t* voice);
audio_format_t audio_voice_format(const audio_voice_t* voice);

void audio_voice_play(audio_voice_t* voice, int looping);
void audio_voice_stop(audio_voice_t* voice);
unsigned audio_voice_status(audio_voice_t* voice);
/* Byte offsets; the write cursor is a short lead ahead of the play cursor while playing. */
void audio_voice_positions(audio_voice_t* voice, uint32_t* play, uint32_t* write);
int audio_voice_set_position(audio_voice_t* voice, uint32_t byte_offset);

/* The voice's own linear gain. */
void audio_voice_set_gain(audio_voice_t* voice, float gain);
/* Playback rate in Hz; the voice's own rate when 0. */
void audio_voice_set_frequency(audio_voice_t* voice, uint32_t hz);
uint32_t audio_voice_frequency(audio_voice_t* voice);
/* The per-channel gain of a 3D source, on top of the voice's gain. */
void audio_voice_set_spatial(audio_voice_t* voice, float left, float right);

/* The offset and length become one or two regions (the second when the range wraps). 0 when the
 * offset is outside the buffer or the length exceeds it. */
int audio_voice_lock(audio_voice_t* voice, uint32_t offset, uint32_t bytes, uint8_t** p1, uint32_t* n1, uint8_t** p2,
                     uint32_t* n2);

/* A PCM stream: frames pushed by one thread are played in order at the stream's own rate, once, and never looped. The
 * queue is a ring of `capacity_ms` allocated at open; pushing copies and never allocates. A stream starts paused. */
audio_stream_t* audio_stream_open(audio_mixer_t* mixer, const audio_format_t* format, uint32_t capacity_ms);
/* Whole frames only; returns the bytes taken, less than `bytes` when the queue is full. */
uint32_t audio_stream_push(audio_stream_t* stream, const void* pcm, uint32_t bytes);
uint32_t audio_stream_queued(audio_stream_t* stream);
/* Frames the output has taken from the stream since it opened: the stream's own clock. */
uint64_t audio_stream_played_frames(audio_stream_t* stream);
void audio_stream_set_gain(audio_stream_t* stream, float gain);
void audio_stream_pause(audio_stream_t* stream, int paused);
int audio_stream_paused(audio_stream_t* stream);
void audio_stream_flush(audio_stream_t* stream);
void audio_stream_close(audio_stream_t* stream);

#endif
