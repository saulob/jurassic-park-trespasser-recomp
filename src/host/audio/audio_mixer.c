#include "audio_mixer.h"

#include <stdlib.h>
#include <string.h>

#define WRITE_LEAD_MS 10

typedef struct {
    uint8_t* bytes;
    uint32_t size;
    int refs;
} audio_block_t;

struct audio_voice {
    audio_mixer_t* mixer;
    audio_voice_t* next;
    audio_format_t format;
    audio_block_t* block;
    double position; /* in frames */
    uint32_t frequency;
    int playing, looping;
    float gain, spatial_left, spatial_right;
};

struct audio_mixer {
    uint32_t rate;
    audio_lock_ops_t ops;
    float master;
    audio_voice_t* voices;
    unsigned count;
};

static void lock(audio_mixer_t* m) {
    if (m->ops.lock) m->ops.lock(m->ops.context);
}

static void unlock(audio_mixer_t* m) {
    if (m->ops.unlock) m->ops.unlock(m->ops.context);
}

int audio_format_valid(const audio_format_t* f) {
    return f && f->rate >= 1 && (f->channels == 1 || f->channels == 2) && (f->bits == 8 || f->bits == 16);
}

uint32_t audio_format_block(const audio_format_t* f) { return (uint32_t)f->channels * (f->bits / 8u); }

audio_mixer_t* audio_mixer_create(uint32_t output_rate, const audio_lock_ops_t* ops) {
    audio_mixer_t* m;
    if (!output_rate) return NULL;
    m = (audio_mixer_t*)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->rate = output_rate;
    m->master = 1.0f;
    if (ops) m->ops = *ops;
    return m;
}

static void block_release(audio_block_t* b) {
    if (--b->refs > 0) return;
    free(b->bytes);
    free(b);
}

void audio_mixer_destroy(audio_mixer_t* m) {
    audio_voice_t* v;
    if (!m) return;
    for (v = m->voices; v;) {
        audio_voice_t* next = v->next;
        block_release(v->block);
        free(v);
        v = next;
    }
    free(m);
}

uint32_t audio_mixer_rate(const audio_mixer_t* m) { return m->rate; }

void audio_mixer_set_rate(audio_mixer_t* m, uint32_t rate) {
    if (!rate) return;
    lock(m);
    m->rate = rate;
    unlock(m);
}

void audio_mixer_set_master_gain(audio_mixer_t* m, float gain) {
    lock(m);
    m->master = gain < 0.0f ? 0.0f : gain;
    unlock(m);
}

unsigned audio_mixer_voice_count(const audio_mixer_t* m) { return m->count; }

static audio_voice_t* voice_new(audio_mixer_t* m, const audio_format_t* f, audio_block_t* block) {
    audio_voice_t* v = (audio_voice_t*)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->mixer = m;
    v->format = *f;
    v->block = block;
    v->gain = v->spatial_left = v->spatial_right = 1.0f;
    lock(m);
    v->next = m->voices;
    m->voices = v;
    m->count++;
    unlock(m);
    return v;
}

audio_voice_t* audio_voice_create(audio_mixer_t* m, const audio_format_t* f, uint32_t bytes) {
    audio_block_t* block;
    audio_voice_t* v;
    uint32_t align;
    if (!m || !audio_format_valid(f)) return NULL;
    align = audio_format_block(f);
    if (!bytes || bytes % align) return NULL;
    block = (audio_block_t*)calloc(1, sizeof *block);
    if (!block) return NULL;
    block->bytes = (uint8_t*)malloc(bytes);
    if (!block->bytes) {
        free(block);
        return NULL;
    }
    memset(block->bytes, f->bits == 8 ? 0x80 : 0, bytes);
    block->size = bytes;
    block->refs = 1;
    v = voice_new(m, f, block);
    if (!v) block_release(block);
    return v;
}

audio_voice_t* audio_voice_duplicate(audio_mixer_t* m, const audio_voice_t* source) {
    audio_voice_t* v;
    if (!m || !source) return NULL;
    lock(m);
    source->block->refs++;
    unlock(m);
    v = voice_new(m, &source->format, source->block);
    if (!v) block_release(source->block);
    else {
        v->frequency = source->frequency;
        v->gain = source->gain;
    }
    return v;
}

void audio_voice_destroy(audio_voice_t* voice) {
    audio_mixer_t* m;
    audio_voice_t** at;
    if (!voice) return;
    m = voice->mixer;
    lock(m);
    for (at = &m->voices; *at && *at != voice; at = &(*at)->next) {
    }
    if (*at) {
        *at = voice->next;
        m->count--;
    }
    unlock(m);
    block_release(voice->block);
    free(voice);
}

uint8_t* audio_voice_data(audio_voice_t* v) { return v->block->bytes; }
uint32_t audio_voice_size(const audio_voice_t* v) { return v->block->size; }
audio_format_t audio_voice_format(const audio_voice_t* v) { return v->format; }

void audio_voice_play(audio_voice_t* v, int looping) {
    lock(v->mixer);
    v->playing = 1;
    v->looping = looping != 0;
    unlock(v->mixer);
}

void audio_voice_stop(audio_voice_t* v) {
    lock(v->mixer);
    v->playing = 0;
    unlock(v->mixer);
}

unsigned audio_voice_status(audio_voice_t* v) {
    unsigned s;
    lock(v->mixer);
    s = v->playing ? (AUDIO_STATUS_PLAYING | (v->looping ? AUDIO_STATUS_LOOPING : 0u)) : 0u;
    unlock(v->mixer);
    return s;
}

static uint32_t effective_rate(const audio_voice_t* v) { return v->frequency ? v->frequency : v->format.rate; }

void audio_voice_positions(audio_voice_t* v, uint32_t* play, uint32_t* write) {
    uint32_t block = audio_format_block(&v->format), p, w;
    lock(v->mixer);
    p = (uint32_t)v->position * block;
    w = p;
    if (v->playing) {
        uint32_t lead = (uint32_t)((uint64_t)effective_rate(v) * WRITE_LEAD_MS / 1000u) * block;
        w = (p + lead) % v->block->size;
    }
    unlock(v->mixer);
    if (play) *play = p;
    if (write) *write = w;
}

int audio_voice_set_position(audio_voice_t* v, uint32_t byte_offset) {
    uint32_t block = audio_format_block(&v->format);
    if (byte_offset >= v->block->size) return 0;
    lock(v->mixer);
    v->position = byte_offset / block;
    unlock(v->mixer);
    return 1;
}

void audio_voice_set_gain(audio_voice_t* v, float gain) {
    lock(v->mixer);
    v->gain = gain;
    unlock(v->mixer);
}

void audio_voice_set_frequency(audio_voice_t* v, uint32_t hz) {
    lock(v->mixer);
    v->frequency = hz;
    unlock(v->mixer);
}

uint32_t audio_voice_frequency(audio_voice_t* v) { return effective_rate(v); }

void audio_voice_set_spatial(audio_voice_t* v, float left, float right) {
    lock(v->mixer);
    v->spatial_left = left;
    v->spatial_right = right;
    unlock(v->mixer);
}

int audio_voice_lock(audio_voice_t* v, uint32_t offset, uint32_t bytes, uint8_t** p1, uint32_t* n1, uint8_t** p2,
                     uint32_t* n2) {
    uint32_t size = v->block->size, first;
    if (offset >= size || bytes > size) return 0;
    first = bytes <= size - offset ? bytes : size - offset;
    *p1 = v->block->bytes + offset;
    *n1 = first;
    *p2 = first < bytes ? v->block->bytes : NULL;
    *n2 = bytes - first;
    return 1;
}

static float sample_at(const audio_voice_t* v, uint32_t frame, int channel) {
    const uint8_t* b = v->block->bytes;
    uint32_t index = frame * v->format.channels + (v->format.channels == 2 ? (uint32_t)channel : 0u);
    if (v->format.bits == 8) return ((float)b[index] - 128.0f) * (1.0f / 128.0f);
    return (float)(int16_t)(b[index * 2] | (b[index * 2 + 1] << 8)) * (1.0f / 32768.0f);
}

static void mix_voice(audio_voice_t* v, float master, uint32_t out_rate, float* out, uint32_t frames) {
    const uint32_t total = v->block->size / audio_format_block(&v->format);
    const double step = (double)effective_rate(v) / out_rate;
    const float gl = master * v->gain * v->spatial_left;
    const float gr = master * v->gain * v->spatial_right;
    double pos = v->position;
    for (uint32_t n = 0; n < frames; n++) {
        uint32_t i = (uint32_t)pos, j = i + 1;
        float l = sample_at(v, i, 0), r = sample_at(v, i, 1);
        double frac = pos - i;
        if (frac > 0.0) {
            float nl, nr;
            if (j >= total) j = v->looping ? 0 : i;
            nl = sample_at(v, j, 0);
            nr = sample_at(v, j, 1);
            l += (nl - l) * (float)frac;
            r += (nr - r) * (float)frac;
        }
        out[2 * n] += l * gl;
        out[2 * n + 1] += r * gr;
        pos += step;
        if (pos >= total) {
            if (v->looping) {
                while (pos >= total) pos -= total;
            } else {
                v->playing = 0;
                pos = 0.0;
                break;
            }
        }
    }
    v->position = pos;
}

void audio_mixer_mix(audio_mixer_t* m, float* out, uint32_t frames) {
    audio_voice_t* v;
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    lock(m);
    for (v = m->voices; v; v = v->next)
        if (v->playing) mix_voice(v, m->master, m->rate, out, frames);
    unlock(m);
    for (uint32_t i = 0; i < frames * 2; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
}
