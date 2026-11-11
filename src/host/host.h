#ifndef TRESPASSER_HOST_H
#define TRESPASSER_HOST_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "native32.h"
#include "host_policy.h"

/* Emitted by tools/run_lift.py into recomp_dispatch.c. */
extern const uint32_t trespasser_entry_va;
extern const uint32_t trespasser_image_base;
extern const uint32_t trespasser_text_start, trespasser_text_end;
extern const uint32_t trespasser_selfmod_start, trespasser_selfmod_end;
extern const char trespasser_expected_sha256[];

/* A shim is entered with g_esp at [return slot][arg0][arg1]... */
#define GUEST_ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define GUEST_PTR(n) ((void*)(uintptr_t)GUEST_ARG(n))
#define GUEST_RETURN_STDCALL(nargs) (g_esp += 4 + 4 * (nargs))

int host_log_open(const char* dir, const char* file);
const char* host_log_path(void);
void host_log(const char* tag, const char* fmt, ...);
void host_log_raw(const char* text, unsigned len);

int host_sha256_file(const char* path, char hex[65]);

void guest_env_init(const char* game_dir, const char* guest_args);
const char* guest_env_exe_path(void);
int guest_env_apply_command_line(void);
void shim_GetModuleHandleA(void);
void shim_GetModuleFileNameA(void);
void shim_MessageBoxA(void);
void shim_DialogBoxParamA(void);
void shim_OutputDebugStringA(void);

/* The store lives in user_dir. Installed Directory (saves, level swap files) is user_dir, Data Drive is game_dir. */
void guest_registry_init(const char* game_dir, const char* user_dir);
void registry_show_ddraw_cert_dialog(void);
void registry_software_water(void);
void shim_RegOpenKeyExA(void);
void shim_RegCreateKeyA(void);
void shim_RegQueryValueExA(void);
void shim_RegSetValueExA(void);
void shim_RegDeleteValueA(void);
void shim_RegCloseKey(void);

int io_trace_init(void);
void io_trace_summary(void);
void io_trace_user_dir(const char* dir);
void io_trace_release_swap_files(void);
void shim_CreateFileA(void);
void shim_CloseHandle(void);
void shim_GetFileAttributesA(void);
void shim_FindFirstFileA(void);
void shim_LZOpenFileA(void);
void shim_LoadLibraryA(void);
void shim_fopen(void);
void shim_open(void);

void milestones_init(void);
void milestones_observe(uint32_t target);

void ddraw_trace_init(void);
recomp_func_t ddraw_trace_lookup(uint32_t va);
int ddraw_trace_name(uint32_t va, char* out, int cap);
int ddraw_trace_last_call(char* out, int cap);
const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap);

int ddraw_compat_init(int enabled);
int ddraw_compat_active(void);
recomp_func_t ddraw_compat_lookup(uint32_t va);
int ddraw_compat_last_call(char* out, int cap);

int selfmod_init(void);
recomp_func_t selfmod_lookup(uint32_t va);
unsigned long selfmod_entries(void);

void diagnostics_install(unsigned watchdog_seconds);
/* Reports the machine state and ends the run with HOST_EXIT_SHUTDOWN_TIMEOUT. */
void diagnostics_shutdown_timeout(const char* detail);
/* After the exit code is logged: other DLLs' first-chance exceptions while unloading are theirs. */
void diagnostics_process_ending(void);

/* window_host.c: the game window, its presentation surface, the cursor and closing. */
void window_host_init(void);
/* trespasser.ini's Display settings; F11 writes display_mode back to settings_path. */
void window_host_configure(int borderless, int monitor, int window_w, int window_h, int stretch, int show_fps,
                           const char* settings_path);
int window_host_monitor_size(int monitor, int* width, int* height);
void window_host_set_display(HWND window, int width, int height);
void window_host_clear_display(HWND window);
HWND window_host_surface(HWND window);
void window_host_frame_presented(void);
void window_host_release_cursor(void);
void window_host_view_size(int* width, int* height);
void window_host_input_summary(void);
const char* window_host_close_origin(void);
/* Presentation stops once the host is closing the game; the game's frames still run. */
int window_host_frames_allowed(void);
/* exit_policy.c drives the last phases: the CRT's exit, its atexit handlers done, the host's cleanup. */
void window_host_crt_exiting(void);
void window_host_guest_stopped(void);
void window_host_begin_cleanup(void);
void window_host_exited(void);
void present_release(void);
void shim_CreateWindowExA(void);
void shim_DestroyWindow(void);
void shim_PostQuitMessage(void);
void shim_SetWindowLongA(void);
void shim_SetWindowPos(void);
void shim_MoveWindow(void);
void shim_SetWindowTextA(void);
void shim_ShowWindow(void);
void shim_GetWindowRect(void);
void shim_GetClientRect(void);
void shim_ClientToScreen(void);
void shim_GetCursorPos(void);
void shim_SetCursorPos(void);
void shim_ClipCursor(void);
void shim_ShowCursor(void);
void shim_SetCapture(void);
void shim_ReleaseCapture(void);
void shim_EnableMenuItem(void);
void shim_PeekMessageA(void);
void shim_IsIconic(void);
void shim_GetForegroundWindow(void);

/* guest_ui.c: the game's own window stack. */
typedef struct {
    guest_ui_t ui;
    guest_wnd_t kind;
    uint32_t top, top_vtable, manager;
    int stopped;
    unsigned depth;
} guest_ui_view_t;

int guest_ui_probe(guest_ui_view_t* out);
const char* guest_ui_class_name(uint32_t vtable);
int guest_display_built(void);
int guest_read32(uint32_t va, uint32_t* out);
/* The game's own "app active" flag: its message pump returns to its loops only while it is set. */
int guest_ui_active(const guest_ui_view_t* v);
/* The game's EndModal(code) on the window in front; quit_to_windows adds the flags its in-game Quit
 * sets. 1 done, -1 the window is already ending, 0 not possible. */
int guest_ui_end_window(const guest_ui_view_t* v, int32_t code, int quit_to_windows);
/* What the game's DirectDraw start-up sets up for Direct3D (its IDirectDraw4 and driver object),
 * redone before an F5 rebuild into D3D11. */
int guest_restore_direct3d(void);
/* Makes the game's next level load restart DirectDraw (its own renderer change point). */
int guest_request_display_restart(void);

/* d3d11_renderer.c: --renderer, --render-resolution, --texture-filter, --msaa */
/* box 0x0: the original compatibility size; otherwise each target is the largest of its surface's shape inside it. */
void d3dr_configure(int hardware, unsigned box_w, unsigned box_h, int filter, unsigned anisotropy, unsigned msaa);
/* d3d11_present.c: Off never waits for the vertical blank; On keeps the game's own Flip request. */
void present_set_vsync(int on);
int d3dr_startup(void);
int d3dr_hardware(void);
int d3dr_set_hardware(int hardware);
const char* d3dr_name(void);
/* Direct3D devices the game has created so far. */
unsigned d3dr_device_count(void);
/* --generate-mips: mip chains the game's textures do not have, for trilinear/anisotropic filtering. */
void d3dr_set_generate_mips(int on);
/* Applied when the game next creates DirectDraw; -1 when nothing waits. */
void d3dr_defer_switch(int hardware);
void d3dr_defer_switch_cancel(void);
int d3dr_pending_switch(void);
unsigned d3dr_textures_alive(void);
/* --debug-d3d-output texture|vertex-color|depth: what the pixel shader returns for every draw. */
int d3dr_set_debug_output(const char* mode);
void d3dr_summary(void);
/* guest_overrides.c: host equivalents of hot lifted functions, byte-exact with them (tests/water_tests.c). */
void guest_overrides_install(void);
/* Points the lifted function's incremental-link thunk at replacement; *body keeps the lifted code. */
int guest_override_redirect(uint32_t va, recomp_func_t replacement, recomp_func_t* body);
/* widescreen.c: gameplay at the aspect's own mode with a Hor+ main camera; 0 for 4:3 or when a site is missing. */
int widescreen_install(int aspect);
int widescreen_active(void);
/* dd_device.c: one more 16 bpp mode, listed whatever the desktop size. */
void dd_set_extra_mode(DWORD width, DWORD height);
/* registry.c: "Width"/"Height" answer this gameplay mode whatever is stored (the launcher's Aspect Ratio decides). */
void registry_gameplay_size(int width, int height);

/* exit_policy.c */
int host_exit_code_for_guest_return(int code);
void shim_exit(void);
void shim__exit(void);
void shim_ExitProcess(void);
void shim_CreateThread(void);
/* Guest threads (CreateThread from lifted code) still running, the main guest thread included. */
unsigned guest_threads_alive(void);
/* "[shutdown] guest threads <when>: N" and one line per worker still running. */
void guest_threads_log(const char* when);

/* registry.c / guest_env.c: the legacy "unstable last time, Safe Mode?" prompt. */
void registry_legacy_safemode_dialog(void);
int registry_legacy_safemode_enabled(void);

enum {
    HOST_EXIT_SETUP = 1,
    HOST_EXIT_NOT_LIFTED = 2,
    HOST_EXIT_FAULT = 3,
    HOST_EXIT_WATCHDOG = 4,
    HOST_EXIT_UNRESOLVED = 5,
    HOST_EXIT_GAME_FAILED = 6,
    HOST_EXIT_GUEST_ERROR = 7,
    HOST_EXIT_SHUTDOWN_TIMEOUT = 8
};

#endif
