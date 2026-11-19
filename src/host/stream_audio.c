#include "stream_audio.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_l10n.h"
#include "jpt_restored_audio.h"
#include "l10n_assets.h"

typedef struct {
    HANDLE handle;
    stream_pack_t* pack;
    uint64_t position;
} virtual_file_t;

static struct {
    int ready;
    CRITICAL_SECTION lock;
    jpt_restored_entry_t entries[JPT_RESTORED_MAX];
    stream_audio_t audio[JPT_RESTORED_MAX];
    int audio_count;
    stream_caption_t* captions;
    uint8_t** caption_blocks;
    int caption_count, caption_cap;
    virtual_file_t files[STREAM_PACK_MAX_VIEWS];
    int stats_logged;
} S;

static int read_handle(void* ctx, uint64_t offset, void* buf, size_t n) {
    OVERLAPPED at;
    DWORD got = 0;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    return ReadFile((HANDLE)ctx, buf, (DWORD)n, &got, &at) && got == n;
}

static void add_caption(void* ctx, const char* pack, uint32_t sample, int count, const uint32_t* units, const char* const* lines) {
    char converted[JPT_CAPTION_MAX_LINES][512];
    const char* pointers[JPT_CAPTION_MAX_LINES];
    uint8_t block[0x2000], *copy;
    size_t size;
    (void)ctx;
    if (strcmp(pack, "stream") || count <= 0 || count > JPT_CAPTION_MAX_LINES) return;
    for (int i = 0; i < count; i++) {
        if (jpt_utf8_to_cp(1252, lines[i], converted[i], sizeof converted[i]) == (size_t)-1) return;
        pointers[i] = converted[i];
    }
    if (!(size = jpt_caption_build(count, units, pointers, block, sizeof block))) return;
    if (S.caption_count == S.caption_cap) {
        int cap = S.caption_cap ? S.caption_cap * 2 : 512;
        stream_caption_t* grown = (stream_caption_t*)realloc(S.captions, (size_t)cap * sizeof *grown);
        uint8_t** blocks = (uint8_t**)realloc(S.caption_blocks, (size_t)cap * sizeof *blocks);
        if (grown) S.captions = grown;
        if (blocks) S.caption_blocks = blocks;
        if (!grown || !blocks) return;
        S.caption_cap = cap;
    }
    if (!(copy = (uint8_t*)malloc(size))) return;
    memcpy(copy, block, size);
    S.caption_blocks[S.caption_count] = copy;
    S.captions[S.caption_count].id = sample;
    S.captions[S.caption_count].block = copy;
    S.captions[S.caption_count].size = size;
    S.caption_count++;
}

void stream_audio_init(void) {
    const uint8_t* manifest;
    size_t manifest_size;
    int count;
    const char* english;
    size_t english_size;
    if (S.ready) return;
    InitializeCriticalSection(&S.lock);
    S.ready = 1;
    if (restored_audio_asset(JPT_RESTORED_MANIFEST, &manifest, &manifest_size)) {
        count = jpt_restored_parse((const char*)manifest, manifest_size, S.entries, JPT_RESTORED_MAX);
        for (int i = 0; i < count; i++) {
            const uint8_t* record;
            size_t record_size;
            if (!restored_audio_asset(S.entries[i].file, &record, &record_size) || record_size != S.entries[i].record_size) {
                host_log("audio", "restored sample %s: its record is missing or the wrong size", S.entries[i].name);
                continue;
            }
            S.audio[S.audio_count].id = S.entries[i].id;
            S.audio[S.audio_count].record = record;
            S.audio[S.audio_count].size = record_size;
            S.audio[S.audio_count].replaces = S.entries[i].replaces;
            S.audio_count++;
        }
        if (count < 0) host_log("audio", "the restored audio manifest is malformed: nothing is restored");
    }
    if (l10n_asset(JPT_L10N_ENGLISH, JPT_L10N_SUBTITLES_ASSET, &english, &english_size))
        jpt_l10n_read_subtitles(english, english_size, add_caption, NULL);
}

stream_pack_t* stream_audio_open_view(HANDLE file, uint64_t size) {
    stream_pack_stats_t stats;
    stream_pack_t* pack;
    stream_audio_init();
    pack = stream_pack_open(read_handle, file, size, S.audio, S.audio_count, S.captions, S.caption_count, &stats);
    return pack;
}

void stream_audio_attach(HANDLE file, const char* path, DWORD access, DWORD flags) {
    LARGE_INTEGER size;
    stream_pack_stats_t stats;
    stream_pack_t* pack;
    int log_stats;
    if (file == INVALID_HANDLE_VALUE || !stream_pack_serves_open(path, access, flags)) return;
    if (!GetFileSizeEx(file, &size)) return;
    stream_audio_init();
    pack = stream_pack_open(read_handle, file, (uint64_t)size.QuadPart, S.audio, S.audio_count, S.captions, S.caption_count, &stats);
    if (!pack) {
        host_log("audio", "%s is not a readable TPA: it plays as it is", path);
        return;
    }
    EnterCriticalSection(&S.lock);
    for (int i = 0; i < STREAM_PACK_MAX_VIEWS; i++) {
        if (S.files[i].handle) continue;
        S.files[i].handle = file;
        S.files[i].pack = pack;
        S.files[i].position = 0;
        pack = NULL;
        break;
    }
    log_stats = !pack && !S.stats_logged;
    if (log_stats) S.stats_logged = 1;
    LeaveCriticalSection(&S.lock);
    if (pack) {
        stream_pack_close(pack);
        host_log("audio", "Stream.tpa: more than %d handles open, this one reads the player's file as it is", STREAM_PACK_MAX_VIEWS);
        return;
    }
    if (log_stats)
        host_log("audio", "Stream.tpa: %d captions corrected, %d restored samples added, %d replaced, %d left as in the player's file",
                 stats.captions_applied, stats.audio_added, stats.audio_replaced, stats.audio_kept);
}

void stream_audio_detach(HANDLE file) {
    stream_pack_t* pack = NULL;
    if (!S.ready) return;
    EnterCriticalSection(&S.lock);
    for (int i = 0; i < STREAM_PACK_MAX_VIEWS; i++) {
        if (S.files[i].handle != file) continue;
        pack = S.files[i].pack;
        memset(&S.files[i], 0, sizeof S.files[i]);
    }
    LeaveCriticalSection(&S.lock);
    stream_pack_close(pack);
}

static virtual_file_t* find_file(HANDLE file) {
    for (int i = 0; i < STREAM_PACK_MAX_VIEWS; i++)
        if (S.files[i].handle == file) return &S.files[i];
    return NULL;
}

void shim_ReadFile(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    void* buffer = GUEST_PTR(1);
    DWORD wanted = GUEST_ARG(2);
    DWORD* read = (DWORD*)GUEST_PTR(3);
    OVERLAPPED* overlapped = (OVERLAPPED*)GUEST_PTR(4);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = (uint32_t)ReadFile(file, buffer, wanted, read, overlapped);
        GUEST_RETURN_STDCALL(5);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = overlapped ? NULL : find_file(file);
    if (v) {
        long got = stream_pack_read(v->pack, v->position, buffer, wanted);
        if (got >= 0) v->position += (uint64_t)got;
        LeaveCriticalSection(&S.lock);
        if (read) *read = got < 0 ? 0 : (DWORD)got;
        if (got < 0) SetLastError(ERROR_READ_FAULT);
        g_eax = got >= 0;
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = (uint32_t)ReadFile(file, buffer, wanted, read, overlapped);
    }
    GUEST_RETURN_STDCALL(5);
}

void shim_SetFilePointer(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    LONG low = (LONG)GUEST_ARG(1);
    LONG* high = (LONG*)GUEST_PTR(2);
    DWORD method = GUEST_ARG(3);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = SetFilePointer(file, low, high, method);
        GUEST_RETURN_STDCALL(4);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = find_file(file);
    if (v) {
        int64_t distance = high ? ((int64_t)*high << 32) | (uint32_t)low : (int64_t)low, base = 0, target;
        if (method == FILE_CURRENT) base = (int64_t)v->position;
        else if (method == FILE_END) base = (int64_t)stream_pack_size(v->pack);
        target = base + distance;
        if (method > FILE_END || target < 0) {
            LeaveCriticalSection(&S.lock);
            SetLastError(ERROR_NEGATIVE_SEEK);
            g_eax = INVALID_SET_FILE_POINTER;
        } else {
            v->position = (uint64_t)target;
            LeaveCriticalSection(&S.lock);
            if (high) *high = (LONG)(target >> 32);
            SetLastError(NO_ERROR);
            g_eax = (uint32_t)target;
        }
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = SetFilePointer(file, low, high, method);
    }
    GUEST_RETURN_STDCALL(4);
}

void shim_GetFileSize(void) {
    HANDLE file = (HANDLE)GUEST_PTR(0);
    DWORD* high = (DWORD*)GUEST_PTR(1);
    virtual_file_t* v;
    if (!S.ready) {
        g_eax = GetFileSize(file, high);
        GUEST_RETURN_STDCALL(2);
        return;
    }
    EnterCriticalSection(&S.lock);
    v = find_file(file);
    if (v) {
        uint64_t size = stream_pack_size(v->pack);
        LeaveCriticalSection(&S.lock);
        if (high) *high = (DWORD)(size >> 32);
        SetLastError(NO_ERROR);
        g_eax = (uint32_t)size;
    } else {
        LeaveCriticalSection(&S.lock);
        g_eax = GetFileSize(file, high);
    }
    GUEST_RETURN_STDCALL(2);
}
