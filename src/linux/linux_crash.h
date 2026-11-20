/* A fatal signal in the Linux host: what the process was doing, written with write(2) from preallocated state before the default
 * action (the core dump) runs. The stage setters are single atomic stores, cheap enough for the present and import paths. */
#ifndef JPT_LINUX_CRASH_H
#define JPT_LINUX_CRASH_H

#include <stdint.h>

typedef enum {
    LX_PRESENT_NONE,
    LX_PRESENT_BEGIN,
    LX_PRESENT_TEXTURE_LOCK,
    LX_PRESENT_TEXTURE_UPLOAD,
    LX_PRESENT_RENDER_CLEAR,
    LX_PRESENT_RENDER_TEXTURE,
    LX_PRESENT_RENDER_PRESENT,
    LX_PRESENT_EVENT_PUMP,
    LX_PRESENT_FRAME_HOOK,
    LX_PRESENT_END
} lx_present_stage_t;

typedef enum {
    LX_AUDIO_NONE,
    LX_AUDIO_DEVICE_OPEN,
    LX_AUDIO_STREAM_THREAD,
    LX_AUDIO_GUEST_THREAD,
    LX_AUDIO_BUFFER_LOCK,
    LX_AUDIO_BUFFER_UNLOCK,
    LX_AUDIO_METHOD
} lx_audio_stage_t;

void lx_crash_install(void);
void lx_crash_present_stage(lx_present_stage_t stage);
void lx_crash_audio_stage(lx_audio_stage_t stage);
void lx_crash_import(uint32_t thunk_va);
/* The calling host thread runs guest thread id (0 for the main one) from entry. */
void lx_crash_thread_started(uint32_t id, uint32_t entry);
void lx_crash_thread_ended(void);

/* Writes the report a fatal signal would write, for tests; returns the number of report bytes written to fd. */
int lx_crash_report(int fd, int signo, void* fault_address, void* host_rip);

#endif
