#include "dsound_compat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_log.h"
#include "audio_mixer.h"
#include "audio_policy.h"
#include "audio_spatial.h"
#include "audio_sync.h"

#define DEFAULT_RATE 22050u
#define SPEAKER_CONFIG_MAX 8
#define FACE_BYTES 8u
#define SLOT_BITS 14
#define SLOT_LIMIT (1u << SLOT_BITS)

static const ds_guid_t k_iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const ds_guid_t k_iid_direct_sound = { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_listener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_buffer = { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_iid_buffer_3d = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const ds_guid_t k_default_playback = { 0xDEF00000, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };
static const ds_guid_t k_default_voice = { 0xDEF00002, 0x9C6D, 0x47ED, { 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 } };
static const ds_guid_t k_guid_null = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

typedef struct ds_buffer ds_buffer_t;

typedef struct {
    uint32_t block; /* the guest face: [vtable][token] */
    uint32_t slot;
    uint32_t refs, coop, speakers;
    ds_buffer_t* primary;
    audio_listener_t listener, pending;
} ds_device_t;

/* Faces of one buffer object, in the guest block that holds them. */
enum { FACE_BUFFER, FACE_3D, FACE_LISTENER, FACE_COUNT };

struct ds_buffer {
    uint32_t block; /* three faces, FACE_BYTES apart */
    uint32_t slot;
    uint32_t refs;
    ds_device_t* device;
    ds_buffer_t* next;
    int primary, primary_playing;
    uint32_t caps;
    audio_format_t format;
    audio_voice_t* voice;
    uint32_t data; /* guest address of the samples */
    int32_t volume;
    uint32_t frequency;
    audio_source_t source, pending_source;
};

typedef enum { OBJECT_FREE, OBJECT_DEVICE, OBJECT_BUFFER } object_kind_t;

typedef struct {
    object_kind_t kind;
    uint16_t generation;
    void* object;
    uint32_t next_free; /* slot index + 1, 0 ends the list */
} slot_t;

typedef struct {
    uint32_t address;
} sample_block_t;

static struct {
    const audio_backend_t* backend;
    dsound_binding_t binding;
    audio_mixer_t* mixer;
    audio_mutex_t* mutex;
    int ready, down, announced;
    uint32_t vtable[DSI_COUNT];
    slot_t* slots;
    uint32_t slot_count, slot_capacity, free_slots;
    ds_buffer_t* buffers;
    dsound_compat_counts_t counts;
} S;

typedef uint32_t (*handler_t)(const uint32_t* args);

#define GUEST (&S.binding.guest)

static int read_guest(uint32_t address, void* out, uint32_t size) { return address && jpt_guest_read(GUEST, address, out, size); }
static int write_guest(uint32_t address, const void* data, uint32_t size) {
    return address && jpt_guest_write(GUEST, address, data, size);
}
static int write_guest32(uint32_t address, uint32_t value) { return write_guest(address, &value, sizeof value); }

static int guest_guid_is(uint32_t address, const ds_guid_t* guid) {
    ds_guid_t read;
    return read_guest(address, &read, sizeof read) && !memcmp(&read, guid, sizeof read);
}

/* ---- the object table ------------------------------------------------------------------------------------------- */

static uint32_t make_token(uint32_t slot, uint16_t generation, unsigned face) {
    return ((uint32_t)generation << 16) | (slot << 2) | face;
}

static int slot_add(object_kind_t kind, void* object, uint32_t* slot_out, uint16_t* generation_out) {
    uint32_t index;
    if (S.free_slots) {
        index = S.free_slots - 1;
        S.free_slots = S.slots[index].next_free;
    } else {
        if (S.slot_count >= SLOT_LIMIT) return 0;
        if (S.slot_count == S.slot_capacity) {
            uint32_t capacity = S.slot_capacity ? S.slot_capacity * 2 : 64;
            slot_t* grown = (slot_t*)realloc(S.slots, capacity * sizeof *grown);
            if (!grown) return 0;
            S.slots = grown;
            S.slot_capacity = capacity;
        }
        index = S.slot_count++;
        memset(&S.slots[index], 0, sizeof S.slots[index]);
    }
    S.slots[index].kind = kind;
    S.slots[index].object = object;
    S.slots[index].next_free = 0;
    if (++S.slots[index].generation == 0) S.slots[index].generation = 1;
    *slot_out = index;
    *generation_out = S.slots[index].generation;
    return 1;
}

static void slot_remove(uint32_t index) {
    S.slots[index].kind = OBJECT_FREE;
    S.slots[index].object = NULL;
    S.slots[index].next_free = S.free_slots;
    S.free_slots = index + 1;
}

/* Writes the faces of a new object into guest memory. */
static uint32_t make_faces(object_kind_t kind, void* object, const dsound_interface_t* interfaces, unsigned faces,
                           uint32_t* slot_out) {
    uint32_t block, slot;
    uint16_t generation;
    if (!slot_add(kind, object, &slot, &generation)) return 0;
    block = GUEST->alloc(GUEST->context, faces * FACE_BYTES);
    if (!block) {
        slot_remove(slot);
        return 0;
    }
    for (unsigned i = 0; i < faces; i++) {
        uint32_t words[2] = { S.vtable[interfaces[i]], make_token(slot, generation, i) };
        write_guest(block + i * FACE_BYTES, words, sizeof words);
    }
    *slot_out = slot;
    return block;
}

static void free_faces(uint32_t slot, uint32_t block) {
    slot_remove(slot);
    GUEST->release(GUEST->context, block);
}

/* The object a guest face address stands for. The token in the guest block names the table slot and generation, and
 * the slot's object must own exactly that address. */
static void* find_object(uint32_t address, object_kind_t kind, unsigned* face_out) {
    uint32_t words[2], slot, block, token;
    unsigned face;
    if (!read_guest(address, words, sizeof words)) return NULL;
    token = words[1];
    face = token & 3u;
    slot = (token >> 2) & (SLOT_LIMIT - 1);
    if (slot >= S.slot_count || S.slots[slot].kind != kind || S.slots[slot].generation != (token >> 16)) return NULL;
    block = kind == OBJECT_DEVICE ? ((ds_device_t*)S.slots[slot].object)->block : ((ds_buffer_t*)S.slots[slot].object)->block;
    if (address != block + face * FACE_BYTES || (kind == OBJECT_DEVICE && face != 0) || face >= FACE_COUNT) return NULL;
    if (face_out) *face_out = face;
    return S.slots[slot].object;
}

static ds_device_t* find_device(uint32_t address) { return (ds_device_t*)find_object(address, OBJECT_DEVICE, NULL); }
static ds_buffer_t* find_buffer(uint32_t address, unsigned* face) { return (ds_buffer_t*)find_object(address, OBJECT_BUFFER, face); }

static uint32_t buffer_face(const ds_buffer_t* b, unsigned face) { return b->block + face * FACE_BYTES; }

/* ---- install and output ----------------------------------------------------------------------------------------- */

static const struct {
    unsigned count;
    dsound_method_t first;
} k_interfaces[DSI_COUNT] = {
    { DSOUND_DEVICE_SLOTS, DSM_DeviceQueryInterface },
    { DSOUND_BUFFER_SLOTS, DSM_BufferQueryInterface },
    { DSOUND_BUFFER3D_SLOTS, DSM_Buffer3DQueryInterface },
    { DSOUND_LISTENER_SLOTS, DSM_ListenerQueryInterface },
};

static const char* const k_method_names[DSM_COUNT] = {
#define DSOUND_NAME_ENTRY(name, words) #name,
    DSOUND_ALL_METHODS(DSOUND_NAME_ENTRY)
#undef DSOUND_NAME_ENTRY
};

static const uint8_t k_method_words[DSM_COUNT] = {
#define DSOUND_WORDS_ENTRY(name, words) words,
    DSOUND_ALL_METHODS(DSOUND_WORDS_ENTRY)
#undef DSOUND_WORDS_ENTRY
};

static int build_vtables(void) {
    for (int i = 0; i < DSI_COUNT; i++) {
        uint32_t table = GUEST->alloc(GUEST->context, k_interfaces[i].count * 4u);
        if (!table) return 0;
        for (unsigned slot = 0; slot < k_interfaces[i].count; slot++) {
            uint32_t entry = S.binding.method_address(S.binding.context, (dsound_method_t)(k_interfaces[i].first + slot));
            if (!entry || !write_guest32(table + slot * 4u, entry)) return 0;
        }
        S.vtable[i] = table;
    }
    return 1;
}

int dsound_compat_install(const audio_backend_t* backend, const dsound_binding_t* binding) {
    if (!backend || !binding || !jpt_guest_ops_valid(&binding->guest) || !binding->method_address) return 0;
    if (S.mixer) return 0;
    if (!S.mutex) S.mutex = audio_mutex_create();
    if (!S.mutex) return 0;
    audio_mutex_lock(S.mutex);
    S.backend = backend;
    S.binding = *binding;
    S.ready = build_vtables();
    audio_mutex_unlock(S.mutex);
    return S.ready;
}

audio_mixer_t* dsound_compat_mixer(void) { return S.mixer; }

uint32_t dsound_compat_vtable(dsound_interface_t interface) { return interface < DSI_COUNT ? S.vtable[interface] : 0; }

void dsound_compat_counts(dsound_compat_counts_t* counts) {
    if (S.mutex) audio_mutex_lock(S.mutex);
    *counts = S.counts;
    if (S.mutex) audio_mutex_unlock(S.mutex);
}

static int output_open(void) {
    if (S.down) return 0;
    if (S.mixer) return 1;
    if (!S.backend) return 0;
    S.mixer = S.backend->open(DEFAULT_RATE);
    if (!S.mixer) {
        host_log("audio", "the SDL3 audio device could not be opened: DirectSound reports no driver");
        return 0;
    }
    if (!S.announced) {
        char line[128];
        audio_backend_line(line, sizeof line, audio_mixer_rate(S.mixer), S.backend->name());
        host_log("audio", "%s", line);
        host_log("audio", "DirectSound compatibility active");
        S.announced = 1;
    }
    return 1;
}

void dsound_compat_shutdown(void) {
    ds_buffer_t* b;
    if (!S.ready) return;
    audio_mutex_lock(S.mutex);
    S.down = 1;
    if (S.mixer) {
        S.backend->close();
        S.mixer = NULL;
    }
    for (b = S.buffers; b; b = b->next) b->voice = NULL;
    host_log_perf("audio", "buffers created %u, released %u, peak %u, still alive %u", S.counts.buffers_created,
                  S.counts.buffers_released, S.counts.buffers_peak, S.counts.buffers_live);
    audio_mutex_unlock(S.mutex);
}

int dsound_compat_is_device(uint32_t address) {
    int is_device;
    if (!S.ready) return 0;
    audio_mutex_lock(S.mutex);
    is_device = find_device(address) != NULL;
    audio_mutex_unlock(S.mutex);
    return is_device;
}

/* ---- objects ---------------------------------------------------------------------------------------------------- */

static void register_buffer(ds_buffer_t* b) {
    b->next = S.buffers;
    S.buffers = b;
    S.counts.buffers_created++;
    S.counts.buffers_live++;
    if (S.counts.buffers_live > S.counts.buffers_peak) S.counts.buffers_peak = S.counts.buffers_live;
}

static void unregister_buffer(ds_buffer_t* b) {
    ds_buffer_t** at;
    for (at = &S.buffers; *at && *at != b; at = &(*at)->next) {
    }
    if (*at) *at = b->next;
    S.counts.buffers_released++;
    S.counts.buffers_live--;
}

static void device_release_ref(ds_device_t* d) {
    if (--d->refs != 0) return;
    free_faces(d->slot, d->block);
    free(d);
}

static void refresh_spatial(ds_buffer_t* b) {
    float left, right;
    if (!b->voice || !(b->caps & DSBCAPS_CTRL3D)) return;
    audio_spatial_gains(&b->device->listener, &b->source, &left, &right);
    audio_voice_set_spatial(b->voice, left, right);
}

static void refresh_gain(ds_buffer_t* b) {
    if (b->voice) audio_voice_set_gain(b->voice, audio_volume_gain(b->volume));
}

static void sample_block_release(void* context) {
    sample_block_t* block = (sample_block_t*)context;
    GUEST->release(GUEST->context, block->address);
    free(block);
}

static ds_buffer_t* new_buffer(ds_device_t* d, uint32_t caps) {
    static const dsound_interface_t faces[FACE_COUNT] = { DSI_BUFFER, DSI_BUFFER3D, DSI_LISTENER };
    ds_buffer_t* b = (ds_buffer_t*)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->block = make_faces(OBJECT_BUFFER, b, faces, FACE_COUNT, &b->slot);
    if (!b->block) {
        free(b);
        return NULL;
    }
    b->refs = 1;
    b->device = d;
    b->caps = caps;
    audio_source_default(&b->source);
    b->pending_source = b->source;
    d->refs++;
    register_buffer(b);
    return b;
}

static uint32_t buffer_add_ref(ds_buffer_t* b) { return ++b->refs; }

static uint32_t buffer_release(ds_buffer_t* b) {
    if (--b->refs != 0) return b->refs;
    unregister_buffer(b);
    if (b->voice) audio_voice_destroy(b->voice);
    if (b->device->primary == b) b->device->primary = NULL;
    device_release_ref(b->device);
    free_faces(b->slot, b->block);
    free(b);
    return 0;
}

/* ---- IUnknown --------------------------------------------------------------------------------------------------- */

static uint32_t h_device_query_interface(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    if (!a[2] || !write_guest32(a[2], 0)) return DS_E_POINTER;
    if (!d) return DSERR_INVALIDPARAM;
    if (!guest_guid_is(a[1], &k_iid_unknown) && !guest_guid_is(a[1], &k_iid_direct_sound)) return DS_E_NOINTERFACE;
    d->refs++;
    write_guest32(a[2], d->block);
    return DS_OK;
}

static uint32_t h_device_add_ref(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    return d ? ++d->refs : 0;
}

static uint32_t h_device_release(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    uint32_t refs;
    if (!d) return 0;
    refs = d->refs - 1;
    device_release_ref(d);
    return refs;
}

static uint32_t h_buffer_query_interface(const uint32_t* a) {
    ds_buffer_t* b = find_buffer(a[0], NULL);
    int face = -1;
    if (!a[2] || !write_guest32(a[2], 0)) return DS_E_POINTER;
    if (!b) return DSERR_INVALIDPARAM;
    if (guest_guid_is(a[1], &k_iid_unknown) || guest_guid_is(a[1], &k_iid_buffer)) face = FACE_BUFFER;
    else if (guest_guid_is(a[1], &k_iid_buffer_3d) && !b->primary && (b->caps & DSBCAPS_CTRL3D)) face = FACE_3D;
    else if (guest_guid_is(a[1], &k_iid_listener) && b->primary && (b->caps & DSBCAPS_CTRL3D)) face = FACE_LISTENER;
    if (face < 0) return DS_E_NOINTERFACE;
    buffer_add_ref(b);
    write_guest32(a[2], buffer_face(b, (unsigned)face));
    return DS_OK;
}

static uint32_t h_buffer_add_ref(const uint32_t* a) {
    ds_buffer_t* b = find_buffer(a[0], NULL);
    return b ? buffer_add_ref(b) : 0;
}

static uint32_t h_buffer_release(const uint32_t* a) {
    ds_buffer_t* b = find_buffer(a[0], NULL);
    return b ? buffer_release(b) : 0;
}

/* ---- IDirectSound ----------------------------------------------------------------------------------------------- */

static int read_format(uint32_t address, ds_waveformatex_t* out) {
    memset(out, 0, sizeof *out);
    return read_guest(address, out, offsetof(ds_waveformatex_t, extra_size));
}

static int format_valid_for_buffer(uint32_t address, audio_format_t* out) {
    ds_waveformatex_t f;
    if (!read_format(address, &f) || f.format_tag != DS_WAVE_FORMAT_PCM) return 0;
    out->rate = f.samples_per_second;
    out->channels = f.channels;
    out->bits = f.bits_per_sample;
    return audio_format_valid(out) && f.samples_per_second >= DS_FREQUENCY_MIN && f.samples_per_second <= DS_FREQUENCY_MAX;
}

static uint32_t create_primary(ds_device_t* d, const ds_bufferdesc_t* desc, uint32_t* out) {
    if (desc->format_address || desc->buffer_bytes) return DSERR_INVALIDPARAM;
    if (d->primary) {
        buffer_add_ref(d->primary);
        *out = buffer_face(d->primary, FACE_BUFFER);
        return DS_OK;
    }
    d->primary = new_buffer(d, desc->flags);
    if (!d->primary) return DSERR_OUTOFMEMORY;
    d->primary->primary = 1;
    d->primary->format.rate = DEFAULT_RATE;
    d->primary->format.channels = 1;
    d->primary->format.bits = 8;
    *out = buffer_face(d->primary, FACE_BUFFER);
    return DS_OK;
}

static audio_voice_t* make_voice(const audio_format_t* format, uint32_t bytes, uint32_t* address) {
    sample_block_t* block = (sample_block_t*)calloc(1, sizeof *block);
    audio_memory_t memory;
    audio_voice_t* voice;
    if (!block) return NULL;
    block->address = GUEST->alloc(GUEST->context, bytes);
    memory.bytes = block->address ? (uint8_t*)GUEST->resolve(GUEST->context, block->address, bytes) : NULL;
    if (!memory.bytes) {
        if (block->address) GUEST->release(GUEST->context, block->address);
        free(block);
        return NULL;
    }
    memory.release = sample_block_release;
    memory.context = block;
    voice = audio_voice_create_in(S.mixer, format, bytes, &memory);
    if (!voice) {
        GUEST->release(GUEST->context, block->address);
        free(block);
        return NULL;
    }
    *address = block->address;
    return voice;
}

static uint32_t create_secondary(ds_device_t* d, const ds_bufferdesc_t* desc, uint32_t* out) {
    audio_format_t format;
    ds_buffer_t* b;
    if (!desc->format_address) return DSERR_INVALIDPARAM;
    if (!format_valid_for_buffer(desc->format_address, &format)) return DSERR_BADFORMAT;
    if (desc->buffer_bytes < DSBSIZE_MIN || desc->buffer_bytes > DSBSIZE_MAX || desc->buffer_bytes % audio_format_block(&format))
        return DSERR_INVALIDPARAM;
    if ((desc->flags & DSBCAPS_CTRL3D) && (format.channels != 1 || (desc->flags & DSBCAPS_CTRLPAN))) return DSERR_INVALIDPARAM;
    if (desc->flags & DSBCAPS_LOCHARDWARE) return DSERR_ALLOCATED;
    if (!S.mixer) return DSERR_NODRIVER;
    b = new_buffer(d, desc->flags);
    if (!b) return DSERR_OUTOFMEMORY;
    b->format = format;
    b->voice = make_voice(&format, desc->buffer_bytes, &b->data);
    if (!b->voice) {
        buffer_release(b);
        return DSERR_OUTOFMEMORY;
    }
    *out = buffer_face(b, FACE_BUFFER);
    return DS_OK;
}

static uint32_t h_device_create_sound_buffer(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    ds_bufferdesc_t desc;
    uint32_t size, face = 0, hr;
    if (!a[2]) return DSERR_INVALIDPARAM;
    if (!write_guest32(a[2], 0)) return DSERR_INVALIDPARAM;
    if (a[3]) return DS_CLASS_E_NOAGGREGATION;
    if (!a[1] || !read_guest(a[1], &size, sizeof size) || (size != DS_BUFFERDESC_V1_SIZE && size != DS_BUFFERDESC_V2_SIZE))
        return DSERR_INVALIDPARAM;
    memset(&desc, 0, sizeof desc);
    if (!read_guest(a[1], &desc, size)) return DSERR_INVALIDPARAM;
    if (S.down || !S.mixer) return DSERR_NODRIVER;
    if (!d) return DSERR_INVALIDPARAM;
    hr = (desc.flags & DSBCAPS_PRIMARYBUFFER) ? create_primary(d, &desc, &face) : create_secondary(d, &desc, &face);
    if (hr == DS_OK && !write_guest32(a[2], face)) {
        ds_buffer_t* b = find_buffer(face, NULL);
        if (b) buffer_release(b);
        return DSERR_INVALIDPARAM;
    }
    return hr;
}

static uint32_t h_device_get_caps(const uint32_t* a) {
    ds_caps_t caps;
    uint32_t size;
    if (!a[1] || !read_guest(a[1], &size, sizeof size) || size != sizeof caps) return DSERR_INVALIDPARAM;
    memset(&caps, 0, sizeof caps);
    caps.size = sizeof caps;
    caps.flags = DSCAPS_CONTINUOUSRATE | DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                 DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    caps.min_secondary_rate = DS_FREQUENCY_MIN;
    caps.max_secondary_rate = DS_FREQUENCY_MAX;
    caps.primary_buffers = 1;
    return write_guest(a[1], &caps, sizeof caps) ? DS_OK : DSERR_INVALIDPARAM;
}

static uint32_t h_device_duplicate_sound_buffer(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    ds_buffer_t *src, *b;
    unsigned face;
    if (!a[2] || !write_guest32(a[2], 0)) return DSERR_INVALIDPARAM;
    src = find_buffer(a[1], &face);
    if (!src || face != FACE_BUFFER || !d) return DSERR_INVALIDPARAM;
    if (src->primary) return DSERR_INVALIDPARAM;
    if (!src->voice || S.down || !S.mixer) return DSERR_BUFFERLOST;
    b = new_buffer(d, src->caps);
    if (!b) return DSERR_OUTOFMEMORY;
    b->format = src->format;
    b->volume = src->volume;
    b->frequency = src->frequency;
    b->source = src->source;
    b->pending_source = src->pending_source;
    b->data = src->data;
    b->voice = audio_voice_duplicate(S.mixer, src->voice);
    if (!b->voice) {
        buffer_release(b);
        return DSERR_OUTOFMEMORY;
    }
    audio_voice_set_frequency(b->voice, b->frequency);
    refresh_gain(b);
    refresh_spatial(b);
    if (!write_guest32(a[2], buffer_face(b, FACE_BUFFER))) {
        buffer_release(b);
        return DSERR_INVALIDPARAM;
    }
    return DS_OK;
}

static uint32_t h_device_set_cooperative_level(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    if (!d) return DSERR_INVALIDPARAM;
    if (a[2] < DSSCL_NORMAL || a[2] > DSSCL_WRITEPRIMARY) return DSERR_INVALIDPARAM;
    d->coop = a[2];
    return DS_OK;
}

static uint32_t h_device_compact(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    if (!d) return DSERR_INVALIDPARAM;
    return d->coop >= DSSCL_PRIORITY ? DS_OK : DSERR_PRIOLEVELNEEDED;
}

static uint32_t h_device_get_speaker_config(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    if (!a[1] || !d) return DSERR_INVALIDPARAM;
    return write_guest32(a[1], d->speakers) ? DS_OK : DSERR_INVALIDPARAM;
}

static uint32_t h_device_set_speaker_config(const uint32_t* a) {
    ds_device_t* d = find_device(a[0]);
    uint32_t speakers = a[1] & 0xFF;
    if (!d || speakers < 1 || speakers > SPEAKER_CONFIG_MAX) return DSERR_INVALIDPARAM;
    d->speakers = a[1];
    return DS_OK;
}

static uint32_t h_device_initialize(const uint32_t* a) {
    (void)a;
    return DSERR_ALREADYINITIALIZED;
}

static uint32_t h_create(const uint32_t* a) {
    ds_device_t* d;
    static const dsound_interface_t faces[1] = { DSI_DEVICE };
    if (!a[1] || !write_guest32(a[1], 0)) return DSERR_INVALIDPARAM;
    if (a[2]) return DS_CLASS_E_NOAGGREGATION;
    if (!S.ready) return DSERR_NODRIVER;
    if (a[0] && !guest_guid_is(a[0], &k_default_playback) && !guest_guid_is(a[0], &k_default_voice) &&
        !guest_guid_is(a[0], &k_guid_null))
        return DSERR_NODRIVER;
    if (!output_open()) return DSERR_NODRIVER;
    d = (ds_device_t*)calloc(1, sizeof *d);
    if (!d) return DSERR_OUTOFMEMORY;
    d->block = make_faces(OBJECT_DEVICE, d, faces, 1, &d->slot);
    if (!d->block) {
        free(d);
        return DSERR_OUTOFMEMORY;
    }
    d->refs = 1;
    d->coop = DSSCL_NORMAL;
    d->speakers = DSSPEAKER_STEREO;
    audio_listener_default(&d->listener);
    d->pending = d->listener;
    S.counts.devices_created++;
    if (!write_guest32(a[1], d->block)) {
        device_release_ref(d);
        return DSERR_INVALIDPARAM;
    }
    return DS_OK;
}

/* ---- IDirectSoundBuffer ----------------------------------------------------------------------------------------- */

#define BUFFER_OR_FAIL(address, result)                                                                                 \
    ds_buffer_t* b = find_buffer(address, NULL);                                                                        \
    if (!b) return (result)

static uint32_t h_buffer_get_current_position(const uint32_t* a) {
    uint32_t p = 0, w = 0;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (b->primary) return DSERR_INVALIDCALL;
    if (!b->voice) return DSERR_BUFFERLOST;
    audio_voice_positions(b->voice, &p, &w);
    if (a[1]) write_guest32(a[1], p);
    if (a[2]) write_guest32(a[2], w);
    return DS_OK;
}

static uint32_t h_buffer_get_volume(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!a[1]) return DSERR_INVALIDPARAM;
    if (!(b->caps & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    return write_guest32(a[1], (uint32_t)b->volume) ? DS_OK : DSERR_INVALIDPARAM;
}

static uint32_t h_buffer_get_status(const uint32_t* a) {
    uint32_t status;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!a[1]) return DSERR_INVALIDPARAM;
    if (b->primary) status = b->primary_playing ? (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING) : 0;
    else status = b->voice ? audio_voice_status(b->voice) : DSBSTATUS_BUFFERLOST;
    return write_guest32(a[1], status) ? DS_OK : DSERR_INVALIDPARAM;
}

static uint32_t h_buffer_lock(const uint32_t* a) {
    uint32_t offset = a[1], bytes = a[2], flags = a[7];
    uint8_t *r1 = NULL, *r2 = NULL;
    uint32_t l1 = 0, l2 = 0;
    const uint8_t* base;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (a[3]) write_guest32(a[3], 0);
    if (a[4]) write_guest32(a[4], 0);
    if (a[5]) write_guest32(a[5], 0);
    if (a[6]) write_guest32(a[6], 0);
    if (!a[3] || !a[4]) return DSERR_INVALIDPARAM;
    if (b->primary) return DSERR_INVALIDCALL;
    if (!b->voice) return DSERR_BUFFERLOST;
    if (flags & DSBLOCK_FROMWRITECURSOR) audio_voice_positions(b->voice, NULL, &offset);
    if (flags & DSBLOCK_ENTIREBUFFER) {
        offset = 0;
        bytes = audio_voice_size(b->voice);
    }
    if (!audio_voice_lock(b->voice, offset, bytes, &r1, &l1, &r2, &l2)) return DSERR_INVALIDPARAM;
    base = audio_voice_data(b->voice);
    write_guest32(a[3], b->data + (uint32_t)(r1 - base));
    write_guest32(a[4], l1);
    if (a[5] && r2) write_guest32(a[5], b->data + (uint32_t)(r2 - base));
    if (a[6]) write_guest32(a[6], l2);
    return DS_OK;
}

static uint32_t h_buffer_play(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (a[1]) return DSERR_INVALIDPARAM;
    if (b->primary) {
        if (!(a[3] & DSBPLAY_LOOPING)) return DSERR_INVALIDPARAM;
        b->primary_playing = 1;
        return DS_OK;
    }
    if (!b->voice) return DSERR_BUFFERLOST;
    audio_voice_play(b->voice, (a[3] & DSBPLAY_LOOPING) != 0);
    return DS_OK;
}

static uint32_t h_buffer_set_current_position(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (b->primary) return DSERR_INVALIDCALL;
    if (!b->voice) return DSERR_BUFFERLOST;
    return audio_voice_set_position(b->voice, a[1]) ? DS_OK : DSERR_INVALIDPARAM;
}

static uint32_t h_buffer_set_format(const uint32_t* a) {
    audio_format_t format;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!b->primary) return DSERR_INVALIDCALL;
    if (b->device->coop < DSSCL_PRIORITY) return DSERR_PRIOLEVELNEEDED;
    if (!format_valid_for_buffer(a[1], &format)) return DSERR_INVALIDPARAM;
    b->format = format;
    if (S.mixer && format.rate != audio_mixer_rate(S.mixer)) S.backend->set_rate(format.rate);
    return DS_OK;
}

static uint32_t h_buffer_set_volume(const uint32_t* a) {
    int32_t volume = (int32_t)a[1];
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!(b->caps & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (volume < DS_VOLUME_MIN || volume > DS_VOLUME_MAX) return DSERR_INVALIDPARAM;
    b->volume = volume;
    if (b->primary) {
        if (S.mixer) audio_mixer_set_master_gain(S.mixer, audio_volume_gain(volume));
    } else {
        refresh_gain(b);
    }
    return DS_OK;
}

static uint32_t h_buffer_set_frequency(const uint32_t* a) {
    int hz;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (b->primary) return DSERR_INVALIDCALL;
    if (!(b->caps & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    hz = audio_resolve_frequency(a[1], 0);
    if (hz < 0) return DSERR_INVALIDPARAM;
    b->frequency = (uint32_t)hz;
    if (b->voice) audio_voice_set_frequency(b->voice, (uint32_t)hz);
    return DS_OK;
}

static uint32_t h_buffer_stop(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (b->primary) b->primary_playing = 0;
    else if (b->voice) audio_voice_stop(b->voice);
    else return DSERR_BUFFERLOST;
    return DS_OK;
}

static uint32_t h_buffer_unlock(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (b->primary) return DSERR_INVALIDCALL;
    if (!a[1] && !a[3]) return DSERR_INVALIDPARAM;
    return b->voice ? DS_OK : DSERR_BUFFERLOST;
}

static uint32_t h_buffer_restore(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    return b->voice || b->primary ? DS_OK : DSERR_BUFFERLOST;
}

static uint32_t h_unsupported(const uint32_t* a) {
    (void)a;
    return DSERR_UNSUPPORTED;
}

/* ---- IDirectSound3DBuffer --------------------------------------------------------------------------------------- */

static int apply_valid(uint32_t apply) { return apply == DS3D_IMMEDIATE || apply == DS3D_DEFERRED; }

#define SOURCE_EDIT(b, apply, ...)                                                                                      \
    do {                                                                                                                \
        audio_source_t* s = &(b)->pending_source;                                                                       \
        __VA_ARGS__;                                                                                                    \
        if ((apply) == DS3D_IMMEDIATE) {                                                                                \
            s = &(b)->source;                                                                                           \
            __VA_ARGS__;                                                                                                \
            refresh_spatial(b);                                                                                         \
        }                                                                                                               \
    } while (0)

static uint32_t h_3d_set_position(const uint32_t* a) {
    float x = jpt_word_to_float(a[1]), y = jpt_word_to_float(a[2]), z = jpt_word_to_float(a[3]);
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[4])) return DSERR_INVALIDPARAM;
    SOURCE_EDIT(b, a[4], s->position.x = x; s->position.y = y; s->position.z = z);
    return DS_OK;
}

static uint32_t h_3d_set_min_distance(const uint32_t* a) {
    float distance = jpt_word_to_float(a[1]);
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[2]) || !(distance >= 0.0f)) return DSERR_INVALIDPARAM;
    SOURCE_EDIT(b, a[2], s->min_distance = distance);
    return DS_OK;
}

static uint32_t h_3d_set_cone_angles(const uint32_t* a) {
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[3]) || a[1] > 360 || a[2] > 360) return DSERR_INVALIDPARAM;
    SOURCE_EDIT(b, a[3], s->inside_angle = a[1]; s->outside_angle = a[2]);
    return DS_OK;
}

static uint32_t h_3d_set_cone_orientation(const uint32_t* a) {
    float x = jpt_word_to_float(a[1]), y = jpt_word_to_float(a[2]), z = jpt_word_to_float(a[3]);
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[4])) return DSERR_INVALIDPARAM;
    SOURCE_EDIT(b, a[4], s->cone_orientation.x = x; s->cone_orientation.y = y; s->cone_orientation.z = z);
    return DS_OK;
}

static uint32_t h_3d_set_cone_outside_volume(const uint32_t* a) {
    int32_t volume = (int32_t)a[1];
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[2]) || volume < DS_VOLUME_MIN || volume > DS_VOLUME_MAX) return DSERR_INVALIDPARAM;
    SOURCE_EDIT(b, a[2], s->outside_volume = volume);
    return DS_OK;
}

/* ---- IDirectSound3DListener ------------------------------------------------------------------------------------- */

static void refresh_listener(ds_device_t* d) {
    for (ds_buffer_t* b = S.buffers; b; b = b->next)
        if (b->device == d) refresh_spatial(b);
}

#define LISTENER_EDIT(d, apply, ...)                                                                                    \
    do {                                                                                                                \
        audio_listener_t* l = &(d)->pending;                                                                            \
        __VA_ARGS__;                                                                                                    \
        if ((apply) == DS3D_IMMEDIATE) {                                                                                \
            l = &(d)->listener;                                                                                         \
            __VA_ARGS__;                                                                                                \
            refresh_listener(d);                                                                                        \
        }                                                                                                               \
    } while (0)

static uint32_t h_listener_set_position(const uint32_t* a) {
    float x = jpt_word_to_float(a[1]), y = jpt_word_to_float(a[2]), z = jpt_word_to_float(a[3]);
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[4])) return DSERR_INVALIDPARAM;
    LISTENER_EDIT(b->device, a[4], l->position.x = x; l->position.y = y; l->position.z = z);
    return DS_OK;
}

static uint32_t h_listener_set_orientation(const uint32_t* a) {
    float fx = jpt_word_to_float(a[1]), fy = jpt_word_to_float(a[2]), fz = jpt_word_to_float(a[3]);
    float tx = jpt_word_to_float(a[4]), ty = jpt_word_to_float(a[5]), tz = jpt_word_to_float(a[6]);
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    if (!apply_valid(a[7])) return DSERR_INVALIDPARAM;
    LISTENER_EDIT(b->device, a[7], l->front.x = fx; l->front.y = fy; l->front.z = fz; l->top.x = tx; l->top.y = ty; l->top.z = tz);
    return DS_OK;
}

static uint32_t h_listener_commit(const uint32_t* a) {
    ds_device_t* d;
    BUFFER_OR_FAIL(a[0], DSERR_INVALIDPARAM);
    d = b->device;
    d->listener = d->pending;
    for (ds_buffer_t* each = S.buffers; each; each = each->next) {
        if (each->device != d || !(each->caps & DSBCAPS_CTRL3D)) continue;
        each->source = each->pending_source;
        refresh_spatial(each);
    }
    return DS_OK;
}

/* ---- dispatch --------------------------------------------------------------------------------------------------- */

static const handler_t k_handlers[DSM_COUNT] = {
    [DSM_Create] = h_create,
    [DSM_DeviceQueryInterface] = h_device_query_interface,
    [DSM_DeviceAddRef] = h_device_add_ref,
    [DSM_DeviceRelease] = h_device_release,
    [DSM_DeviceCreateSoundBuffer] = h_device_create_sound_buffer,
    [DSM_DeviceGetCaps] = h_device_get_caps,
    [DSM_DeviceDuplicateSoundBuffer] = h_device_duplicate_sound_buffer,
    [DSM_DeviceSetCooperativeLevel] = h_device_set_cooperative_level,
    [DSM_DeviceCompact] = h_device_compact,
    [DSM_DeviceGetSpeakerConfig] = h_device_get_speaker_config,
    [DSM_DeviceSetSpeakerConfig] = h_device_set_speaker_config,
    [DSM_DeviceInitialize] = h_device_initialize,
    [DSM_BufferQueryInterface] = h_buffer_query_interface,
    [DSM_BufferAddRef] = h_buffer_add_ref,
    [DSM_BufferRelease] = h_buffer_release,
    [DSM_BufferGetCaps] = h_unsupported,
    [DSM_BufferGetCurrentPosition] = h_buffer_get_current_position,
    [DSM_BufferGetFormat] = h_unsupported,
    [DSM_BufferGetVolume] = h_buffer_get_volume,
    [DSM_BufferGetPan] = h_unsupported,
    [DSM_BufferGetFrequency] = h_unsupported,
    [DSM_BufferGetStatus] = h_buffer_get_status,
    [DSM_BufferInitialize] = h_unsupported,
    [DSM_BufferLock] = h_buffer_lock,
    [DSM_BufferPlay] = h_buffer_play,
    [DSM_BufferSetCurrentPosition] = h_buffer_set_current_position,
    [DSM_BufferSetFormat] = h_buffer_set_format,
    [DSM_BufferSetVolume] = h_buffer_set_volume,
    [DSM_BufferSetPan] = h_unsupported,
    [DSM_BufferSetFrequency] = h_buffer_set_frequency,
    [DSM_BufferStop] = h_buffer_stop,
    [DSM_BufferUnlock] = h_buffer_unlock,
    [DSM_BufferRestore] = h_buffer_restore,
    [DSM_Buffer3DQueryInterface] = h_buffer_query_interface,
    [DSM_Buffer3DAddRef] = h_buffer_add_ref,
    [DSM_Buffer3DRelease] = h_buffer_release,
    [DSM_Buffer3DGetAllParameters] = h_unsupported,
    [DSM_Buffer3DGetConeAngles] = h_unsupported,
    [DSM_Buffer3DGetConeOrientation] = h_unsupported,
    [DSM_Buffer3DGetConeOutsideVolume] = h_unsupported,
    [DSM_Buffer3DGetMaxDistance] = h_unsupported,
    [DSM_Buffer3DGetMinDistance] = h_unsupported,
    [DSM_Buffer3DGetMode] = h_unsupported,
    [DSM_Buffer3DGetPosition] = h_unsupported,
    [DSM_Buffer3DGetVelocity] = h_unsupported,
    [DSM_Buffer3DSetAllParameters] = h_unsupported,
    [DSM_Buffer3DSetConeAngles] = h_3d_set_cone_angles,
    [DSM_Buffer3DSetConeOrientation] = h_3d_set_cone_orientation,
    [DSM_Buffer3DSetConeOutsideVolume] = h_3d_set_cone_outside_volume,
    [DSM_Buffer3DSetMaxDistance] = h_unsupported,
    [DSM_Buffer3DSetMinDistance] = h_3d_set_min_distance,
    [DSM_Buffer3DSetMode] = h_unsupported,
    [DSM_Buffer3DSetPosition] = h_3d_set_position,
    [DSM_Buffer3DSetVelocity] = h_unsupported,
    [DSM_ListenerQueryInterface] = h_buffer_query_interface,
    [DSM_ListenerAddRef] = h_buffer_add_ref,
    [DSM_ListenerRelease] = h_buffer_release,
    [DSM_ListenerGetAllParameters] = h_unsupported,
    [DSM_ListenerGetDistanceFactor] = h_unsupported,
    [DSM_ListenerGetDopplerFactor] = h_unsupported,
    [DSM_ListenerGetOrientation] = h_unsupported,
    [DSM_ListenerGetPosition] = h_unsupported,
    [DSM_ListenerGetRolloffFactor] = h_unsupported,
    [DSM_ListenerGetVelocity] = h_unsupported,
    [DSM_ListenerSetAllParameters] = h_unsupported,
    [DSM_ListenerSetDistanceFactor] = h_unsupported,
    [DSM_ListenerSetDopplerFactor] = h_unsupported,
    [DSM_ListenerSetOrientation] = h_listener_set_orientation,
    [DSM_ListenerSetPosition] = h_listener_set_position,
    [DSM_ListenerSetRolloffFactor] = h_unsupported,
    [DSM_ListenerSetVelocity] = h_unsupported,
    [DSM_ListenerCommitDeferredSettings] = h_listener_commit,
};

uint32_t dsound_compat_call(dsound_method_t method, const uint32_t* args) {
    uint32_t result;
    if ((unsigned)method >= DSM_COUNT || !k_handlers[method]) return DSERR_UNSUPPORTED;
    if (!S.mutex) return DSERR_NODRIVER;
    audio_mutex_lock(S.mutex);
    result = k_handlers[method](args);
    audio_mutex_unlock(S.mutex);
    return result;
}

uint32_t dsound_method_words(dsound_method_t method) { return (unsigned)method < DSM_COUNT ? k_method_words[method] : 0; }
const char* dsound_method_name(dsound_method_t method) { return (unsigned)method < DSM_COUNT ? k_method_names[method] : "?"; }

dsound_interface_t dsound_method_interface(dsound_method_t method) {
    for (int i = DSI_COUNT - 1; i >= 0; i--)
        if (method >= k_interfaces[i].first) return (dsound_interface_t)i;
    return DSI_DEVICE;
}

unsigned dsound_method_slot(dsound_method_t method) { return (unsigned)(method - k_interfaces[dsound_method_interface(method)].first); }

int dsound_compat_vtables_complete(void) {
    for (int i = 0; i < DSM_COUNT; i++)
        if (!k_handlers[i]) return 0;
    for (int i = 0; i < DSI_COUNT; i++)
        if (!S.vtable[i]) return 0;
    return 1;
}
