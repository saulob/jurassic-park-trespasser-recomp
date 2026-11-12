#ifndef TRESPASSER_HOST_POLICY_H
#define TRESPASSER_HOST_POLICY_H

/* Window-system-free decisions of the host, kept apart so tests can run them headless. */

#include <stdint.h>

typedef struct {
    int64_t frequency;
    int64_t window_start;
    int64_t last_present;
    uint32_t frames;
    uint64_t total_frames;
    double fps;
} fps_counter_t;

void fps_counter_init(fps_counter_t* c, int64_t frequency, int64_t now);
/* Returns 1 when a new rate is available (about once per second of presented frames). */
int fps_counter_present(fps_counter_t* c, int64_t now);
/* Returns 1 when presentation has stalled long enough that the shown rate must drop to 0. */
int fps_counter_idle(fps_counter_t* c, int64_t now);

typedef enum {
    INPUT_MENU,
    INPUT_GAMEPLAY_CAPTURED,
    INPUT_GAMEPLAY_RELEASED,
    INPUT_DIALOG,
    INPUT_MOVING_WINDOW,
    INPUT_FULLSCREEN_TRANSITION,
    INPUT_SHUTDOWN,
    INPUT_STATE_COUNT
} input_state_t;

/* What the game itself shows, read from its window stack. */
typedef enum { GUEST_UI_NONE, GUEST_UI_MENU, GUEST_UI_DIALOG, GUEST_UI_GAMEPLAY } guest_ui_t;

typedef struct {
    int shutdown;
    int transition;
    int size_move;
    int focused;
    int minimized;
    guest_ui_t ui;
} input_signals_t;

input_state_t input_state_from(const input_signals_t* s);
const char* input_state_name(input_state_t state);
/* Only focused gameplay takes relative Raw Input, hides the cursor and confines it to the picture. */
int input_captured(input_state_t state);
/* In gameplay the game reads a cursor it re-centres itself; elsewhere it follows the real one. */
int input_cursor_virtual(input_state_t state);
int input_hide_pointer(input_state_t state, int game_cursor_hidden);
/* A capture waits while a button is held outside the picture (the click that activated the window
 * on its title bar), so that drag is not pulled into the view. */
int input_should_clip(input_state_t state, int pointer_in_view, int button_held);

typedef enum { KEY_TO_GAME, KEY_DROP, KEY_HOST_FULLSCREEN, KEY_HOST_RENDERER, KEY_HOST_CLOSE } key_action_t;

/* msg is WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP; repeat is lParam bit 30. */
key_action_t key_policy(unsigned msg, unsigned vk, int repeat, int ctrl, int alt);
/* Esc, F1 and F12 open a window of the game over gameplay: the cursor is freed before it opens. */
int key_opens_game_window(unsigned msg, unsigned vk, int repeat);

/* CGameWnd opens its window on Esc/F1/F12 key-down, but CUIWnd::OnKey (0x0040AF50) cancels a window
 * on Esc key-up: the key-up of the press that opened a window must not reach that window. */
typedef struct {
    unsigned vk, depth;
    int armed;
} open_press_t;

void open_press_reset(open_press_t* p);
void open_press_keydown(open_press_t* p, unsigned vk, int repeat, int gameplay, unsigned depth);
/* 1 when this key-up belongs to the press that opened a window (the game's window stack grew). */
int open_press_keyup(open_press_t* p, unsigned vk, unsigned depth_now);

typedef enum {
    SHUTDOWN_NONE,
    SHUTDOWN_CLOSE_REQUESTED,
    SHUTDOWN_GUEST_QUIT_REQUESTED,
    SHUTDOWN_MODAL_CLOSING,
    SHUTDOWN_GAMEWND_CLOSING,
    SHUTDOWN_MAINSCREEN_CLOSING,
    SHUTDOWN_WINMAIN_RETURNING,
    SHUTDOWN_CRT_EXITING,
    SHUTDOWN_GUEST_STOPPED,
    SHUTDOWN_HOST_CLEANUP,
    SHUTDOWN_EXITED
} shutdown_phase_t;

typedef struct {
    shutdown_phase_t phase;
    const char* source;
    int host_requested;
    unsigned progress;
} shutdown_state_t;

void shutdown_init(shutdown_state_t* s);
/* 1 for the first close request; later ones (a second Alt+F4, the WM_CLOSE a close produces) are ignored. */
int shutdown_request(shutdown_state_t* s, const char* source);
void shutdown_quit_requested(shutdown_state_t* s);
/* Phases only move forward; 1 when this one is new. The game's own Exit starts at WinMainReturning. */
int shutdown_advance(shutdown_state_t* s, shutdown_phase_t phase, const char* source);
/* Any step of the game's way out counts as progress, even one that stays in the same phase. */
void shutdown_note_progress(shutdown_state_t* s);
void shutdown_exited(shutdown_state_t* s);
int shutdown_active(const shutdown_state_t* s);
/* The host leads the game out until its CRT takes over. */
int shutdown_host_driving(const shutdown_state_t* s);
int shutdown_guest_may_run(const shutdown_state_t* s);
int shutdown_renderer_allowed(const shutdown_state_t* s);
const char* shutdown_phase_name(shutdown_phase_t phase);
/* Milliseconds without progress after which the n-th stall report is due: 250, 1000, 2000, 4000... */
uint32_t shutdown_stall_threshold(unsigned reports);

/* F5. The game rebuilds its display in place only with the renderer its DirectDraw start-up chose:
 * a scene rendered with Direct3D keeps hardware render types on its polygons (sub_0053A680 never
 * re-types one) and those reach the Direct3D device the software fallback releases. So Direct3D to
 * software waits for the game's own DirectDraw restart at the next level load while a level is in
 * memory; everything else switches at a frame boundary with nothing locked or in a scene. */
typedef enum {
    RENDERER_ACTIVE,
    RENDERER_SWITCH_REQUESTED,
    RENDERER_DRAINING,
    RENDERER_REBUILDING,
    RENDERER_ACTIVE_NEW,
    RENDERER_SWITCH_FAILED,
    RENDERER_SWITCH_DEFERRED
} renderer_switch_t;

typedef enum { SWITCH_NOW, SWITCH_WAIT, SWITCH_DEFER, SWITCH_GIVE_UP } switch_decision_t;

#define RENDERER_SWITCH_ATTEMPTS 30

switch_decision_t renderer_switch_decide(int to_hardware, int scene_loaded, long locks, long dcs, long scenes,
                                         unsigned attempts);
const char* renderer_switch_name(renderer_switch_t state);

/* A renderer change waiting for the game's next DirectDraw creation. */
typedef struct {
    int value;
} pending_switch_t;

void pending_switch_set(pending_switch_t* p, int hardware);
void pending_switch_cancel(pending_switch_t* p);
/* -1 when nothing waits, else 0 (software) or 1 (hardware). take also clears it. */
int pending_switch_peek(const pending_switch_t* p);
int pending_switch_take(pending_switch_t* p);

/* The game's windows the host's quit path tells apart. */
typedef enum {
    GUEST_WND_NONE,
    GUEST_WND_GAME,
    GUEST_WND_INGAME_OPTIONS,
    GUEST_WND_QUIT,
    GUEST_WND_MAIN_MENU,
    GUEST_WND_VIDEO,
    GUEST_WND_LOADER,
    GUEST_WND_OTHER
} guest_wnd_t;

typedef enum { QUIT_WAIT, QUIT_END_WINDOW, QUIT_SKIP_VIDEO } quit_step_t;

typedef struct {
    quit_step_t step;
    int32_t code;
    int quit_to_windows;
    shutdown_phase_t phase;
} quit_action_t;

/* One step of the game's own way out for the window in front: the in-game Quit (CQuitWnd answer 1
 * ends gameplay with -1), the main menu's Exit (-1), Cancel (0) for any other window. */
quit_action_t quit_action_for(guest_wnd_t top, int at_video_pump);

typedef enum {
    READBACK_LOCK_READ,
    READBACK_LOCK_WRITE,
    READBACK_GETDC,
    READBACK_PIXEL_CALIBRATION,
    READBACK_BLT_SOURCE,
    READBACK_FRONT_BUFFER,
    READBACK_OTHER,
    READBACK_REASON_COUNT
} readback_reason_t;

typedef enum { CPU_ACCESS_LOCK, CPU_ACCESS_GETDC, CPU_ACCESS_BLT_SOURCE, CPU_ACCESS_BLT_DEST, CPU_ACCESS_FRONT, CPU_ACCESS_OTHER } cpu_access_t;

/* calibration_caller is the game function that reads test polygons back at start-up. */
readback_reason_t readback_classify(cpu_access_t access, int read_only, uint32_t caller, uint32_t calibration_caller);
const char* readback_reason_name(readback_reason_t reason);

/* Raster Locks that touch no pixel: CScreenRenderDWI::BeginFrame only hands the surface address to the
 * span rasterisers (which Lock again before drawing), CScreenRenderAuxD3D::FlushBatch locks to wait for
 * the hardware. */
#define LOCK_CALLER_BEGIN_FRAME 0x0055F1D0u
#define LOCK_CALLER_FLUSH_BATCH 0x00555AB0u

/* The gameplay text overlay's draws (0x0050EAF0, one line 0x0050E830): GetDC, then aliased TextOutA only (the host
 * makes their font non-antialiased), so nothing under the text is read. */
#define GETDC_CALLER_TEXT_OVERLAY 0x0050EAF0u

/* Whether a CPU access to a Direct3D render target needs the GPU's pixels in the CPU copy. The game
 * locks with DDLOCK_WAIT only, so write-only flags never come from it; callers decide instead. */
int target_access_reads(cpu_access_t access, uint32_t lock_flags, uint32_t caller);

/* The game's "unstable last time, Safe Mode?" prompt: WinMain sets registry "Safemode" to 1 at start
 * and to 0 at its end, and a 1 at start-up makes sub_0040D780 ask through the string-table message
 * box sub_0040E4D0 (type 0x23, title 1, text 11). By default the value reads as 0 and is never stored,
 * and that one message answers No. */
#define SAFEMODE_MSGBOX_HELPER 0x0040E4D0u
#define SAFEMODE_PROMPT_TYPE 0x23u
#define SAFEMODE_PROMPT_TITLE 1u
#define SAFEMODE_PROMPT_TEXT 11u
int safemode_value_virtual(int legacy);
/* "Hardware Water" read by the game's driver set-up (0x00506A50, string at 0x00660094) while it builds
 * the texture formats: 1 makes water a CRasterD3D alpha texture drawn blended (render type 3); 0 keeps
 * the software water texture. Answers 1 or 0, or -1 to leave the stored value / game default. */
int hardware_water_policy(int hardware_renderer, int value_stored, int software_water_requested);
int safemode_prompt_args(uint32_t func, uint32_t type, uint32_t title_id, uint32_t text_id);

/* Lifted calls push a dummy return address, so a caller is found in pcrecomp's indirect-call ring:
 * the newest call into one of the wrappers names the function that made it; 0 if none is left. */
uint32_t icall_ring_caller(const uint32_t* targets, const uint32_t* from, uint32_t next, unsigned size,
                           const uint32_t* wrappers, unsigned wrapper_count);

#endif
