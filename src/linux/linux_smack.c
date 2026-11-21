#include "linux_smack.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "guest_memory.h"
#include "jpt_fs.h"
#include "linux_log.h"
#include "linux_video_subtitles.h"
#include "smacker_compat.h"

typedef struct {
    int fd;
    int owned;
} file_source_t;

static int s_installed;

static void* guest_resolve(void* context, uint32_t address, uint32_t size) {
    void* p;
    (void)context;
    p = gm_ptr_checked(address, size, 1);
    return p ? p : gm_ptr_checked(address, size, 0);
}

static uint32_t guest_alloc(void* context, uint32_t size) {
    (void)context;
    return gm_heap_alloc(size, 1);
}

static void guest_release(void* context, uint32_t address) {
    (void)context;
    gm_heap_free(address);
}

static uint32_t read_at(void* context, uint64_t offset, void* out, uint32_t bytes) {
    const file_source_t* f = (const file_source_t*)context;
    ssize_t n = pread(f->fd, out, bytes, (off_t)offset);
    return n > 0 ? (uint32_t)n : 0;
}

static int make_source(int fd, int owned, smk_source_t* out) {
    file_source_t* f;
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (size < 0) {
        if (owned && fd >= 0) close(fd);
        return 0;
    }
    f = (file_source_t*)malloc(sizeof *f);
    if (!f) {
        if (owned) close(fd);
        return 0;
    }
    f->fd = fd, f->owned = owned;
    out->context = f, out->size = (uint64_t)size, out->read_at = read_at;
    return 1;
}

static int open_handle(void* context, uint32_t handle, smk_source_t* out) {
    (void)context;
    return make_source(hle_file_fd(handle), 0, out);
}

static int open_path(void* context, const char* path, smk_source_t* out) {
    char host[JPT_PATH_MAX];
    (void)context;
    if (!hle_vpath() || jpt_vpath_resolve(hle_vpath(), path, 0, host, sizeof host) != JPT_VP_OK) return 0;
    return make_source(open(host, O_RDONLY | O_CLOEXEC), 1, out);
}

static void close_source(void* context, smk_source_t* source) {
    file_source_t* f = (file_source_t*)source->context;
    (void)context;
    if (!f) return;
    if (f->owned) close(f->fd);
    free(f);
    source->context = NULL;
}

static void log_line(void* context, const char* line) {
    (void)context;
    lx_log("Linux SMACK", "%s", line);
}

static int handle_name(void* context, uint32_t handle, char* out, size_t capacity) {
    (void)context;
    return hle_file_name(handle, out, capacity);
}

static void video_opened(void* context, uint32_t video, const char* name, const smk_source_t* source, uint32_t frames) {
    (void)context, (void)frames;
    lx_vsub_opened(video, name, source);
}

static void video_frame(void* context, uint32_t video, uint32_t frame, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)context;
    lx_vsub_frame(video, frame, x, y, w, h);
}

static void video_closed(void* context, uint32_t video) {
    (void)context;
    lx_vsub_closed(video);
}

static uint64_t now_us(void* context) {
    struct timespec ts;
    (void)context;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int s_play_videos;

int lx_smack_can_decode(void) { return smk_ffmpeg_backend() != NULL; }
void lx_smack_set_play_videos(int play) { s_play_videos = play; }
int lx_smack_videos_enabled(void) { return s_play_videos && lx_smack_can_decode(); }

void hle_smack_init(void) {
    smk_host_t host = { { NULL, guest_resolve, guest_alloc, guest_release }, smk_ffmpeg_backend(), open_handle, open_path, close_source,
                        smk_dsound_mixer_for_device, now_us, NULL, log_line, handle_name, video_opened, video_frame, video_closed };
    smacker_hle_stats_reset();
    if (!host.backend) {
        lx_log("Linux SMACK", "no FFmpeg in this build (JPT_HAS_FFMPEG=0): Smacker videos are not decoded and the game skips them (NoVideo = 1); install libavformat-dev libavcodec-dev libavutil-dev and rebuild");
        return;
    }
    s_installed = smacker_hle_install(&host);
    if (!s_installed) {
        lx_log("Linux SMACK", "the Smacker HLE could not be installed");
        return;
    }
    {
        char libraries[512];
        smk_ffmpeg_describe(libraries, sizeof libraries);
        lx_log("Linux SMACK", "Smacker video decoded by FFmpeg: %s", libraries);
        if (strstr(libraries, "GPL") && !strstr(libraries, "LGPL"))
            lx_log("Linux SMACK", "this FFmpeg is not LGPL-only: fine for development, a release must ship an LGPL-only build");
    }
}

void hle_smack_shutdown(void) {
    if (s_installed) smacker_hle_shutdown();
    lx_vsub_reset();
    s_installed = 0;
}

#define SMK_WRAPPER(name, words, decorated)                           \
    static uint64_t sm_##name(const uint32_t* a, void* u) {           \
        (void)u;                                                      \
        return smacker_hle_call(SMKX_##name, a);                      \
    }
SMACK_EXPORTS(SMK_WRAPPER)
#undef SMK_WRAPPER

static const hle_entry_t k_table[] = {
#define SMK_ENTRY(name, words, decorated) { "smackw32.dll", decorated, sm_##name, words, HT_STDCALL },
    SMACK_EXPORTS(SMK_ENTRY)
#undef SMK_ENTRY
};

const hle_entry_t* hle_smack_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
