#include "smacker_compat.h"

#include <stdlib.h>
#include <string.h>

#define PATH_CAPACITY 520u
#define STREAM_CAPACITY_MS 1500u
#define PENDING_BYTES 65536u
#define AUDIO_STALL_US 400000u
#define TRACK_FLAG(track) (0x2000u << (track))

typedef struct smk_object smk_object_t;

struct smk_object {
    smk_object_t* next;
    uint32_t address; /* the Smack structure in guest memory: the handle the game holds */
    smk_header_t header;
    uint32_t shown_height, rows_per_stored_row;
    smk_source_t source;
    smk_backend_t *video, *audio;
    uint8_t *picture, *previous;
    uint8_t palette[768];
    uint16_t palette_words[256];
    int have_picture, palette_changed, video_ended;
    uint32_t decoded, frame;

    int destination_set, destination_dirty;
    int32_t left, top;
    uint32_t pitch, destination_height, destination_address, format;

    int rect_ready;
    int32_t rect_x, rect_y, rect_w, rect_h;

    int armed, sound_on, running, drained;
    uint64_t armed_at, next_due, media, drain_media, drain_wall;
    audio_mixer_t* mixer;
    int clock_track;
    struct {
        audio_stream_t* stream;
        uint8_t* pending;
        uint32_t pending_bytes, pending_offset, block;
        uint32_t rate;
        int ended;
    } track[SMK_TRACKS];
};

static struct {
    smk_host_t host;
    int ready;
    uint32_t device;
    smk_object_t* objects;
    unsigned count;
} S;

static const struct {
    const char* name;
    uint8_t words;
} k_exports[SMKX_COUNT] = {
#define SMACK_TABLE_ENTRY(name, words, decorated) { decorated, words },
    SMACK_EXPORTS(SMACK_TABLE_ENTRY)
#undef SMACK_TABLE_ENTRY
};

#define GUEST (&S.host.guest)

static void put32(const smk_object_t* o, uint32_t offset, uint32_t value) { jpt_guest_write32(GUEST, o->address + offset, value); }

/* ---- audio ------------------------------------------------------------------------------------------------------ */

static int track_active(const smk_object_t* o, unsigned t) { return o->track[t].stream != NULL; }

static void audio_close(smk_object_t* o) {
    for (unsigned t = 0; t < SMK_TRACKS; t++) {
        if (o->track[t].stream) audio_stream_close(o->track[t].stream);
        free(o->track[t].pending);
        memset(&o->track[t], 0, sizeof o->track[t]);
    }
    if (o->audio) S.host.backend->close(o->audio);
    o->audio = NULL;
    o->clock_track = -1;
}

static void audio_open(smk_object_t* o, uint32_t flags) {
    o->clock_track = -1;
    if (!(flags & SMK_OPEN_TRACKS) || !S.host.mixer_for_device) return;
    o->mixer = S.host.mixer_for_device(S.host.context, S.device);
    if (!o->mixer) return;
    o->audio = S.host.backend->open(&o->source, &o->header, SMK_STREAM_AUDIO);
    if (!o->audio) return;
    for (unsigned t = 0; t < SMK_TRACKS; t++) {
        audio_format_t format;
        if (!o->header.track[t].rate || !(flags & TRACK_FLAG(t))) continue;
        format.rate = o->header.track[t].rate;
        format.channels = o->header.track[t].channels;
        format.bits = o->header.track[t].bits;
        o->track[t].stream = audio_stream_open(o->mixer, &format, STREAM_CAPACITY_MS);
        if (!o->track[t].stream) continue;
        o->track[t].pending = (uint8_t*)malloc(PENDING_BYTES);
        o->track[t].block = audio_format_block(&format);
        o->track[t].rate = format.rate;
        if (!o->track[t].pending) {
            audio_stream_close(o->track[t].stream);
            o->track[t].stream = NULL;
            continue;
        }
        if (o->clock_track < 0) o->clock_track = (int)t;
    }
    if (o->clock_track < 0) {
        S.host.backend->close(o->audio);
        o->audio = NULL;
    }
}

static uint32_t lead_bytes(const smk_object_t* o, unsigned t) {
    return (uint32_t)((uint64_t)o->track[t].rate * SMK_AUDIO_LEAD_MS / 1000u) * o->track[t].block;
}

/* Keeps each track's queue about SMK_AUDIO_LEAD_MS ahead of what the output has played. */
static void audio_pump(smk_object_t* o) {
    if (!o->audio || !o->sound_on) return;
    for (unsigned t = 0; t < SMK_TRACKS; t++) {
        if (!track_active(o, t)) continue;
        while (audio_stream_queued(o->track[t].stream) < lead_bytes(o, t)) {
            uint32_t taken;
            if (o->track[t].pending_offset >= o->track[t].pending_bytes) {
                if (o->track[t].ended) break;
                o->track[t].pending_offset = 0;
                o->track[t].pending_bytes = S.host.backend->next_audio(o->audio, t, o->track[t].pending, PENDING_BYTES);
                if (!o->track[t].pending_bytes) {
                    o->track[t].ended = 1;
                    break;
                }
            }
            taken = audio_stream_push(o->track[t].stream, o->track[t].pending + o->track[t].pending_offset,
                                      o->track[t].pending_bytes - o->track[t].pending_offset);
            o->track[t].pending_offset += taken;
            if (!taken) break;
        }
    }
}

static int audio_drained(const smk_object_t* o) {
    const unsigned t = (unsigned)o->clock_track;
    return o->track[t].ended && o->track[t].pending_offset >= o->track[t].pending_bytes &&
           audio_stream_queued(o->track[t].stream) == 0;
}

static int audio_clock_in_use(const smk_object_t* o) { return o->audio && o->sound_on && o->clock_track >= 0; }

/* Microseconds of the video's own time. The output's playback position is the clock while it plays, a wall clock where
 * it does not (no sound, a stalled device, the audio over). It never runs backwards. */
static uint64_t media_time(smk_object_t* o, uint64_t now) {
    uint64_t wall = now - o->armed_at, candidate = wall;
    if (audio_clock_in_use(o)) {
        const unsigned t = (unsigned)o->clock_track;
        uint64_t audio = audio_stream_played_frames(o->track[t].stream) * 1000000u / o->track[t].rate;
        if (o->drained || audio_drained(o)) {
            if (!o->drained) {
                o->drained = 1;
                o->drain_media = audio;
                o->drain_wall = now;
            }
            candidate = o->drain_media + (now - o->drain_wall);
        } else if (wall <= audio + AUDIO_STALL_US) {
            candidate = audio;
        }
    }
    if (candidate > o->media) o->media = candidate;
    return o->media;
}

static void audio_start(smk_object_t* o) {
    audio_pump(o);
    for (unsigned t = 0; t < SMK_TRACKS; t++)
        if (track_active(o, t) && o->sound_on) audio_stream_pause(o->track[t].stream, 0);
    o->running = 1;
}

/* ---- objects ---------------------------------------------------------------------------------------------------- */

static smk_object_t* find_object(uint32_t address) {
    for (smk_object_t* o = S.objects; o; o = o->next)
        if (o->address == address) return o;
    return NULL;
}

static void object_destroy(smk_object_t* o) {
    smk_object_t** at;
    audio_close(o);
    if (o->video) S.host.backend->close(o->video);
    if (S.host.close_source) S.host.close_source(S.host.context, &o->source);
    for (at = &S.objects; *at && *at != o; at = &(*at)->next) {
    }
    if (*at) {
        *at = o->next;
        S.count--;
    }
    if (o->address) GUEST->release(GUEST->context, o->address);
    free(o->picture);
    free(o->previous);
    free(o);
}

static void write_public_fields(const smk_object_t* o) {
    put32(o, SMK_OFF_VERSION, 0x32 << 24 | 'K' << 16 | 'M' << 8 | 'S');
    put32(o, SMK_OFF_WIDTH, o->header.width);
    put32(o, SMK_OFF_HEIGHT, o->shown_height);
    put32(o, SMK_OFF_FRAMES, o->header.frames);
    put32(o, SMK_OFF_RATE, (uint32_t)o->header.frame_rate);
    put32(o, SMK_OFF_FLAGS, o->header.flags);
    put32(o, SMK_OFF_FRAME_NUMBER, 0);
}

static uint32_t smk_open(const uint32_t* a) {
    smk_source_t source;
    uint8_t head[SMK_HEADER_BYTES];
    smk_header_t header;
    smk_object_t* o;
    uint32_t flags = a[1];
    int opened;
    if (!S.ready) return 0;
    if (flags & SMK_OPEN_FILEHANDLE) {
        opened = S.host.open_handle && S.host.open_handle(S.host.context, a[0], &source);
    } else {
        char path[PATH_CAPACITY];
        opened = S.host.open_path && jpt_guest_read_string(GUEST, a[0], path, sizeof path) &&
                 S.host.open_path(S.host.context, path, &source);
    }
    if (!opened) return 0;
    if (source.read_at(source.context, 0, head, sizeof head) != sizeof head || !smk_header_parse(head, sizeof head, &header)) {
        if (S.host.close_source) S.host.close_source(S.host.context, &source);
        return 0;
    }
    o = (smk_object_t*)calloc(1, sizeof *o);
    if (!o) {
        if (S.host.close_source) S.host.close_source(S.host.context, &source);
        return 0;
    }
    o->source = source;
    o->header = header;
    o->shown_height = smk_shown_height(&header);
    o->rows_per_stored_row = (header.flags & (SMK_FLAG_Y_INTERLACED | SMK_FLAG_Y_DOUBLED)) ? 2u : 1u;
    o->clock_track = -1;
    o->sound_on = 1;
    o->picture = (uint8_t*)calloc(header.width, header.height);
    o->previous = (uint8_t*)calloc(header.width, header.height);
    o->video = o->picture && o->previous ? S.host.backend->open(&o->source, &o->header, SMK_STREAM_VIDEO) : NULL;
    o->address = o->video ? GUEST->alloc(GUEST->context, SMK_STRUCT_BYTES) : 0;
    if (!o->address) {
        object_destroy(o);
        return 0;
    }
    audio_open(o, flags);
    write_public_fields(o);
    o->next = S.objects;
    S.objects = o;
    S.count++;
    return o->address;
}

static uint32_t smk_close(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    if (o) object_destroy(o);
    return 0;
}

static uint32_t smk_sound_on_off(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    if (!o) return 0;
    o->sound_on = a[1] != 0;
    for (unsigned t = 0; t < SMK_TRACKS; t++) {
        if (!track_active(o, t)) continue;
        if (!o->sound_on) {
            audio_stream_flush(o->track[t].stream);
            audio_stream_pause(o->track[t].stream, 1);
        } else if (o->running) {
            audio_stream_pause(o->track[t].stream, 0);
        }
    }
    return 0;
}

static uint32_t smk_sound_use_direct_sound(const uint32_t* a) {
    S.device = a[0];
    return 0;
}

static uint32_t smk_to_buffer(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    if (!o) return 0;
    o->left = (int32_t)a[1];
    o->top = (int32_t)a[2];
    o->pitch = a[3];
    o->destination_height = a[4];
    o->destination_address = a[5];
    o->format = a[6] & SMK_BUFFER_FORMAT_MASK;
    o->destination_set = a[5] != 0;
    o->destination_dirty = 1;
    return 0;
}

/* ---- frames ----------------------------------------------------------------------------------------------------- */

static uint16_t pack_color(uint32_t format, const uint8_t* rgb) {
    if (format == SMK_BUFFER_555) return (uint16_t)((rgb[0] >> 3) << 10 | (rgb[1] >> 3) << 5 | rgb[2] >> 3);
    return (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
}

#define BLOCK 4u

/* The original converts and writes a 4x4 block of the destination only when a pixel of the block changed; a block that
 * did not change keeps the colours it was written with, even when the palette moved on since (fades leave such blocks
 * behind). The output of the original is reproduced exactly this way. */
static int block_changed(const smk_object_t* o, uint32_t bx, uint32_t by) {
    const uint32_t w = o->header.width, h = o->header.height;
    const uint32_t columns = bx + BLOCK < w ? BLOCK : w - bx;
    for (uint32_t y = by; y < by + BLOCK && y < h; y++)
        if (memcmp(o->picture + (size_t)y * w + bx, o->previous + (size_t)y * w + bx, columns)) return 1;
    return 0;
}

static void write_block(const smk_object_t* o, uint8_t* base, uint32_t bx, uint32_t by) {
    const uint32_t bytes_per_pixel = o->format ? 2u : 1u, w = o->header.width, h = o->header.height;
    const int32_t columns = (int32_t)(o->pitch / bytes_per_pixel);
    const uint32_t copies = (o->header.flags & SMK_FLAG_Y_DOUBLED) ? 2u : 1u;
    for (uint32_t r = by; r < by + BLOCK && r < h; r++) {
        for (uint32_t copy = 0; copy < copies; copy++) {
            const int32_t y = o->top + (int32_t)(r * o->rows_per_stored_row + copy);
            int32_t dest_x = o->left + (int32_t)bx, n = (int32_t)(bx + BLOCK < w ? BLOCK : w - bx), skip = 0;
            const uint8_t* src = o->picture + (size_t)r * w + bx;
            uint8_t* dst;
            if (y < 0 || (uint32_t)y >= o->destination_height) continue;
            if (dest_x < 0) {
                skip = -dest_x;
                dest_x = 0;
            }
            n -= skip;
            if (dest_x + n > columns) n = columns - dest_x;
            if (n <= 0) continue;
            src += skip;
            dst = base + (size_t)y * o->pitch + (size_t)dest_x * bytes_per_pixel;
            if (!o->format) {
                memcpy(dst, src, (size_t)n);
            } else {
                for (int32_t i = 0; i < n; i++) {
                    const uint16_t v = o->palette_words[src[i]];
                    dst[2 * i] = (uint8_t)(v & 0xFF);
                    dst[2 * i + 1] = (uint8_t)(v >> 8);
                }
            }
        }
    }
}

/* Writes what changed (everything when `whole`) and returns the bounding box of the blocks written, in picture pixels. */
static int blit(smk_object_t* o, int whole, int* x0, int* y0, int* x1, int* y1) {
    const uint32_t bytes_per_pixel = o->format ? 2u : 1u;
    uint8_t* base;
    uint64_t size = (uint64_t)o->pitch * o->destination_height;
    int any = 0;
    *x0 = *y0 = 0x7FFFFFFF;
    *x1 = *y1 = -1;
    if (!o->destination_set || !o->pitch || !o->destination_height || size > 0x40000000u) return 0;
    base = (uint8_t*)GUEST->resolve(GUEST->context, o->destination_address, (uint32_t)size);
    if (!base || o->pitch < bytes_per_pixel) return 0;
    if (o->format)
        for (unsigned i = 0; i < 256; i++) o->palette_words[i] = pack_color(o->format, o->palette + i * 3u);
    for (uint32_t by = 0; by < o->header.height; by += BLOCK)
        for (uint32_t bx = 0; bx < o->header.width; bx += BLOCK) {
            int right, bottom;
            if (!whole && !block_changed(o, bx, by)) continue;
            write_block(o, base, bx, by);
            right = (int)(bx + BLOCK < o->header.width ? bx + BLOCK : o->header.width) - 1;
            bottom = (int)(by + BLOCK < o->header.height ? by + BLOCK : o->header.height) - 1;
            if ((int)bx < *x0) *x0 = (int)bx;
            if ((int)by < *y0) *y0 = (int)by;
            if (right > *x1) *x1 = right;
            if (bottom > *y1) *y1 = bottom;
            any = 1;
        }
    return any;
}

static void set_rect(smk_object_t* o, int x0, int y0, int x1, int y1) {
    int32_t columns = (int32_t)(o->pitch / (o->format ? 2u : 1u));
    int32_t x = o->left + x0, y = o->top + (int32_t)((uint32_t)y0 * o->rows_per_stored_row);
    int32_t right = o->left + x1 + 1, bottom = o->top + (int32_t)((uint32_t)(y1 + 1) * o->rows_per_stored_row);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > columns) right = columns;
    if (bottom > (int32_t)o->destination_height) bottom = (int32_t)o->destination_height;
    o->rect_ready = right > x && bottom > y;
    o->rect_x = x;
    o->rect_y = y;
    o->rect_w = right - x;
    o->rect_h = bottom - y;
}

static uint32_t smk_do_frame(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    int whole, x0, y0, x1, y1;
    if (!o) return 0;
    audio_pump(o);
    o->rect_ready = 0;
    o->palette_changed = 0;
    whole = !o->have_picture || o->destination_dirty;
    while (o->decoded <= o->frame && !o->video_ended) {
        uint8_t palette[768];
        memcpy(o->previous, o->picture, (size_t)o->header.width * o->header.height);
        if (!S.host.backend->next_video(o->video, o->picture, palette)) {
            o->video_ended = 1;
            break;
        }
        if (!o->have_picture || memcmp(palette, o->palette, sizeof palette)) {
            memcpy(o->palette, palette, sizeof palette);
            o->palette_changed = 1;
        }
        if (o->decoded != o->frame) whole = 1;
        o->decoded++;
        o->have_picture = 1;
    }
    if (!o->have_picture) return 0;
    put32(o, SMK_OFF_NEW_PALETTE, o->palette_changed != 0);
    if (o->palette_changed) jpt_guest_write(GUEST, o->address + SMK_OFF_PALETTE, o->palette, sizeof o->palette);
    if (blit(o, whole, &x0, &y0, &x1, &y1)) set_rect(o, x0, y0, x1, y1);
    o->destination_dirty = 0;
    return 0;
}

static uint32_t smk_to_buffer_rect(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    if (!o || !o->rect_ready) return 0;
    o->rect_ready = 0;
    put32(o, SMK_OFF_RECT_X, (uint32_t)o->rect_x);
    put32(o, SMK_OFF_RECT_Y, (uint32_t)o->rect_y);
    put32(o, SMK_OFF_RECT_W, (uint32_t)o->rect_w);
    put32(o, SMK_OFF_RECT_H, (uint32_t)o->rect_h);
    return 1;
}

static uint32_t smk_next_frame(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    if (!o) return 0;
    if (o->frame + 1 < o->header.frames) o->frame++;
    put32(o, SMK_OFF_FRAME_NUMBER, o->frame);
    return 0;
}

/* 0 when the next frame is due, 1 while it is not. The first call starts the clock and waits one frame time, as the
 * original does; a frame time later than the next one by more than a frame restarts the schedule unless the output's
 * own position is the clock (then the picture catches up with it). */
static uint32_t smk_wait(const uint32_t* a) {
    smk_object_t* o = find_object(a[0]);
    const uint64_t duration = o ? smk_frame_duration_us(o->header.frame_rate) : 0;
    uint64_t now, t;
    if (!o) return 0;
    now = S.host.now_us ? S.host.now_us(S.host.context) : 0;
    audio_pump(o);
    if (!o->armed) {
        o->armed = 1;
        o->armed_at = now;
        o->next_due = duration;
        audio_start(o);
        return 1;
    }
    t = media_time(o, now);
    if (t < o->next_due) return 1;
    o->next_due += duration;
    if (!audio_clock_in_use(o) && t > o->next_due + duration) o->next_due = t + duration;
    return 0;
}

/* ---- the buffer API: never used while the primary surface has memory, which the host always gives ---------------- */

static uint32_t smk_buffer_unavailable(const uint32_t* a) {
    (void)a;
    return 0;
}

/* ---- the entry points ------------------------------------------------------------------------------------------- */

typedef uint32_t (*smk_handler_t)(const uint32_t* args);

static const smk_handler_t k_handlers[SMKX_COUNT] = {
    [SMKX_SmackOpen] = smk_open,
    [SMKX_SmackClose] = smk_close,
    [SMKX_SmackSoundOnOff] = smk_sound_on_off,
    [SMKX_SmackWait] = smk_wait,
    [SMKX_SmackBufferOpen] = smk_buffer_unavailable,
    [SMKX_SmackNextFrame] = smk_next_frame,
    [SMKX_SmackColorRemapWithTrans] = smk_buffer_unavailable,
    [SMKX_SmackDoFrame] = smk_do_frame,
    [SMKX_SmackToBufferRect] = smk_to_buffer_rect,
    [SMKX_SmackSoundUseDirectSound] = smk_sound_use_direct_sound,
    [SMKX_SmackToBuffer] = smk_to_buffer,
    [SMKX_SmackBufferNewPalette] = smk_buffer_unavailable,
    [SMKX_SmackBufferBlit] = smk_buffer_unavailable,
    [SMKX_SmackBufferClose] = smk_buffer_unavailable,
};

int smacker_hle_install(const smk_host_t* host) {
    if (!host || !jpt_guest_ops_valid(&host->guest) || !host->backend || !host->backend->open || !host->backend->close ||
        !host->backend->next_video || !host->backend->next_audio)
        return 0;
    smacker_hle_shutdown();
    S.host = *host;
    S.ready = 1;
    return 1;
}

void smacker_hle_shutdown(void) {
    while (S.objects) object_destroy(S.objects);
    memset(&S, 0, sizeof S);
}

uint32_t smacker_hle_call(smk_export_t export_id, const uint32_t* args) {
    if ((unsigned)export_id >= SMKX_COUNT || !k_handlers[export_id]) return 0;
    return k_handlers[export_id](args);
}

const char* smacker_export_name(smk_export_t export_id) { return (unsigned)export_id < SMKX_COUNT ? k_exports[export_id].name : ""; }
uint32_t smacker_export_words(smk_export_t export_id) { return (unsigned)export_id < SMKX_COUNT ? k_exports[export_id].words : 0; }
unsigned smacker_open_count(void) { return S.count; }

int smacker_find_export(const char* name, smk_export_t* out) {
    if (!name || !*name) return 0;
    for (int i = 0; i < SMKX_COUNT; i++) {
        const char* decorated = k_exports[i].name;
        size_t plain = strcspn(decorated + 1, "@");
        if (!strcmp(name, decorated) || (strlen(name) == plain && !strncmp(name, decorated + 1, plain))) {
            if (out) *out = (smk_export_t)i;
            return 1;
        }
    }
    return 0;
}

int smacker_is_library(const char* name) {
    static const char target[] = "smackw32.dll";
    const char* base = name;
    if (!name) return 0;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    for (size_t i = 0; i < sizeof target; i++) {
        char c = base[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != target[i]) return 0;
        if (!c) return 1;
    }
    return 0;
}

smk_binding_t smk_binding_for_platform(jpt_platform_t platform) {
    return platform == JPT_PLATFORM_WINDOWS ? SMK_BINDING_ORIGINAL_DLL : SMK_BINDING_HLE;
}

int smk_binding_needs_dll(smk_binding_t binding) { return binding == SMK_BINDING_ORIGINAL_DLL; }
