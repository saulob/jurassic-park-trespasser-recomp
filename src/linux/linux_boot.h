/* The headless bootstrap: lays the guest out in its window, binds the imports, and runs the lifted entry point on a guest
 * thread until something stops it. It opens no window, starts no audio and plays nothing; it stops at the first import with no
 * implementation, so the result says exactly what the guest needs next. */
#ifndef JPT_LINUX_BOOT_H
#define JPT_LINUX_BOOT_H

#include <stdint.h>

#include "jpt_custom_level.h"
#include "linux_window.h"

typedef enum {
    LX_BOOT_SETUP_FAILED,      /* the game folder, the guest image or the lift did not match */
    LX_BOOT_DATA_UNAVAILABLE,  /* no guest image to run */
    LX_BOOT_EXITED,            /* the guest ended the process */
    LX_BOOT_MISSING_IMPORT,    /* the guest called an import with no implementation */
    LX_BOOT_REACHED_USER32,    /* the guest called a user32 function: the window milestone */
    LX_BOOT_FATAL_ABI,         /* stack imbalance or an unreadable guest stack */
    LX_BOOT_BAD_DISPATCH,      /* a call to an address that is neither lifted nor a thunk */
    LX_BOOT_NOT_LIFTED,        /* control reached a function the lift left out */
    LX_BOOT_FRAME_PRESENTED,   /* the guest presented its first frame to the (virtual) window */
    LX_BOOT_IDLE               /* the guest polls for messages and nothing is left to deliver */
} lx_boot_status_t;

typedef struct {
    const char* game_dir;
    const char* version; /* default en-116 */
    const char* user_root;
    const char* saves_dir; /* the game's "Installed Directory"; default <user_root>/saves */
    const char* settings_file; /* trespasser.ini: the language and the video subtitles switch; NULL = the defaults */
    const jpt_custom_level_request_t* custom_level; /* the custom level to start; NULL or an empty scene for none */
    lx_window_backend_t* backend; /* the host window; NULL = the virtual backend (no window) */
    int unbounded;         /* run until the guest exits: no frame and no idle limit */
    int use_features;      /* the host features over the lifted game (src/host guest_features.h); the executable turns them on */
    int use_gamepad;       /* SDL3 gamepads drive the game's input (the executable); headless suites leave them off */
    int play_videos;       /* let the game play its Smacker videos (needs FFmpeg in the build); 0 answers NoVideo = 1 */
    unsigned frame_limit;  /* stop after this many presented frames; default 1 */
    void (*frame_hook)(unsigned frame, void* user); /* after each presented frame */
    void* hook_user;
    unsigned idle_limit;   /* stop after this many empty PeekMessage calls; default 200000, 0xFFFFFFFF never */
} lx_boot_options_t;

typedef struct {
    lx_boot_status_t status;
    char detail[200];       /* module!function of the missing import, or the reason */
    uint32_t entry, guest_eip, caller, exit_code;
    unsigned imports_total, imports_implemented, imports_reached;
    unsigned selfmod_entries, lift_hooks, overrides_listed, overrides_installed, overrides_required;
    int quit_reason, cleanup_complete;
    unsigned host_ended_guest, windows_ended_by_host;
    struct {
        unsigned files, stream_views, finds, ddraw_surfaces, audio_buffers, guest_threads, smack_objects;
    } at_stop; /* what the guest still held when it stopped, before the host released anything */
    unsigned modules, module_functions, module_imports, module_imports_implemented, dll_attach_count;
} lx_boot_result_t;

/* Every field set: the hooks and limits a caller does not name are off. */
void lx_boot_options_init(lx_boot_options_t* o, const char* game_dir, const char* version, const char* user_root, const char* saves_dir,
                          lx_window_backend_t* backend, int unbounded);
lx_boot_status_t lx_boot_headless(const lx_boot_options_t* options, lx_boot_result_t* result);
/* The process exit code of a finished run: 0 when the user asked to quit (whatever the game returned on its way out), else the guest's. */
int lx_boot_process_exit_code(const lx_boot_result_t* result);
const char* lx_boot_status_name(lx_boot_status_t status);

#endif
