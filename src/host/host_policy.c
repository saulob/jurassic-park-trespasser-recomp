#include "host_policy.h"

#include <string.h>

#define FPS_STALL_WINDOWS 2

enum { MSG_KEYDOWN = 0x100, MSG_KEYUP = 0x101, MSG_SYSKEYDOWN = 0x104, MSG_SYSKEYUP = 0x105 };
enum { VK_ESC = 0x1B, VK_F1_ = 0x70, VK_F4_ = 0x73, VK_F5_ = 0x74, VK_F11_ = 0x7A, VK_F12_ = 0x7B };

void fps_counter_init(fps_counter_t* c, int64_t frequency, int64_t now) {
    memset(c, 0, sizeof *c);
    c->frequency = frequency > 0 ? frequency : 1;
    c->window_start = now;
    c->last_present = now;
}

int fps_counter_present(fps_counter_t* c, int64_t now) {
    int64_t elapsed = now - c->window_start;
    c->frames++;
    c->total_frames++;
    c->last_present = now;
    if (elapsed < c->frequency) return 0;
    c->fps = (double)c->frames * (double)c->frequency / (double)elapsed;
    c->frames = 0;
    c->window_start = now;
    return 1;
}

int fps_counter_idle(fps_counter_t* c, int64_t now) {
    if (now - c->last_present < FPS_STALL_WINDOWS * c->frequency) return 0;
    c->frames = 0;
    c->window_start = now;
    if (c->fps == 0.0) return 0;
    c->fps = 0.0;
    return 1;
}

input_state_t input_state_from(const input_signals_t* s) {
    if (s->shutdown) return INPUT_SHUTDOWN;
    if (s->transition) return INPUT_FULLSCREEN_TRANSITION;
    if (s->size_move) return INPUT_MOVING_WINDOW;
    if (s->ui == GUEST_UI_DIALOG) return INPUT_DIALOG;
    if (s->ui == GUEST_UI_GAMEPLAY) return s->focused && !s->minimized ? INPUT_GAMEPLAY_CAPTURED : INPUT_GAMEPLAY_RELEASED;
    return INPUT_MENU;
}

const char* input_state_name(input_state_t state) {
    static const char* const names[INPUT_STATE_COUNT] = {
        "INPUT_MENU", "INPUT_GAMEPLAY_CAPTURED", "INPUT_GAMEPLAY_RELEASED", "INPUT_DIALOG",
        "INPUT_MOVING_WINDOW", "INPUT_FULLSCREEN_TRANSITION", "INPUT_SHUTDOWN",
    };
    return state >= 0 && state < INPUT_STATE_COUNT ? names[state] : "?";
}

int input_captured(input_state_t state) { return state == INPUT_GAMEPLAY_CAPTURED; }

int input_cursor_virtual(input_state_t state) {
    return state == INPUT_GAMEPLAY_CAPTURED || state == INPUT_GAMEPLAY_RELEASED || state == INPUT_MOVING_WINDOW ||
           state == INPUT_FULLSCREEN_TRANSITION || state == INPUT_SHUTDOWN;
}

int input_hide_pointer(input_state_t state, int game_cursor_hidden) {
    switch (state) {
    case INPUT_GAMEPLAY_CAPTURED: return 1;
    case INPUT_MENU:
    case INPUT_DIALOG: return game_cursor_hidden;
    default: return 0;
    }
}

int input_should_clip(input_state_t state, int pointer_in_view, int button_held) {
    return input_captured(state) && (pointer_in_view || !button_held);
}

key_action_t key_policy(unsigned msg, unsigned vk, int repeat, int ctrl, int alt) {
    int down = msg == MSG_KEYDOWN || msg == MSG_SYSKEYDOWN;
    if (msg != MSG_KEYDOWN && msg != MSG_KEYUP && msg != MSG_SYSKEYDOWN && msg != MSG_SYSKEYUP) return KEY_TO_GAME;
    switch (vk) {
    case VK_F11_:
        if (ctrl) return KEY_TO_GAME;
        return down && !repeat ? KEY_HOST_FULLSCREEN : KEY_DROP;
    case VK_F5_: return down && !repeat ? KEY_HOST_RENDERER : KEY_DROP;
    case VK_F4_:
        if (!alt) return KEY_TO_GAME;
        return down && !repeat ? KEY_HOST_CLOSE : KEY_DROP;
    case VK_ESC:
    case VK_F1_:
    case VK_F12_:
        /* Each opens a modal window of the game on key-down; auto-repeat would open it again. */
        return down && repeat ? KEY_DROP : KEY_TO_GAME;
    default: return KEY_TO_GAME;
    }
}

int key_opens_game_window(unsigned msg, unsigned vk, int repeat) {
    return msg == MSG_KEYDOWN && !repeat && (vk == VK_ESC || vk == VK_F1_ || vk == VK_F12_);
}

void open_press_reset(open_press_t* p) { memset(p, 0, sizeof *p); }

void open_press_keydown(open_press_t* p, unsigned vk, int repeat, int gameplay, unsigned depth) {
    if (repeat || (vk != VK_ESC && vk != VK_F1_ && vk != VK_F12_)) return;
    p->armed = gameplay != 0;
    p->vk = vk;
    p->depth = depth;
}

int open_press_keyup(open_press_t* p, unsigned vk, unsigned depth_now) {
    if (!p->armed || p->vk != vk) return 0;
    p->armed = 0;
    return depth_now > p->depth;
}

void shutdown_init(shutdown_state_t* s) { memset(s, 0, sizeof *s); }

int shutdown_request(shutdown_state_t* s, const char* source) {
    if (s->phase != SHUTDOWN_NONE) return 0;
    s->phase = SHUTDOWN_CLOSE_REQUESTED;
    s->source = source;
    s->host_requested = 1;
    return 1;
}

void shutdown_quit_requested(shutdown_state_t* s) { shutdown_advance(s, SHUTDOWN_GUEST_QUIT_REQUESTED, NULL); }

int shutdown_advance(shutdown_state_t* s, shutdown_phase_t phase, const char* source) {
    if (phase <= s->phase) return 0;
    if (s->phase == SHUTDOWN_NONE) s->source = source ? source : "the game's own menu";
    s->phase = phase;
    s->progress++;
    return 1;
}

void shutdown_note_progress(shutdown_state_t* s) { s->progress++; }

void shutdown_exited(shutdown_state_t* s) { shutdown_advance(s, SHUTDOWN_EXITED, NULL); }

int shutdown_active(const shutdown_state_t* s) { return s->phase != SHUTDOWN_NONE; }

int shutdown_host_driving(const shutdown_state_t* s) {
    return s->host_requested && s->phase >= SHUTDOWN_CLOSE_REQUESTED && s->phase < SHUTDOWN_CRT_EXITING;
}

int shutdown_guest_may_run(const shutdown_state_t* s) { return s->phase < SHUTDOWN_GUEST_STOPPED; }

int shutdown_renderer_allowed(const shutdown_state_t* s) { return s->phase < SHUTDOWN_HOST_CLEANUP; }

const char* shutdown_phase_name(shutdown_phase_t phase) {
    switch (phase) {
    case SHUTDOWN_NONE: return "Running";
    case SHUTDOWN_CLOSE_REQUESTED: return "CloseRequested";
    case SHUTDOWN_GUEST_QUIT_REQUESTED: return "GuestQuitRequested";
    case SHUTDOWN_MODAL_CLOSING: return "ModalClosing";
    case SHUTDOWN_GAMEWND_CLOSING: return "GameWndClosing";
    case SHUTDOWN_MAINSCREEN_CLOSING: return "MainScreenClosing";
    case SHUTDOWN_WINMAIN_RETURNING: return "WinMainReturning";
    case SHUTDOWN_CRT_EXITING: return "CRTExiting";
    case SHUTDOWN_GUEST_STOPPED: return "GuestStopped";
    case SHUTDOWN_HOST_CLEANUP: return "HostCleanup";
    case SHUTDOWN_EXITED: return "Exited";
    }
    return "?";
}

uint32_t shutdown_stall_threshold(unsigned reports) {
    if (!reports) return 250;
    return reports > 8 ? 1000u << 7 : 1000u << (reports - 1);
}

quit_action_t quit_action_for(guest_wnd_t top, int at_video_pump) {
    quit_action_t a = { QUIT_WAIT, 0, 0, SHUTDOWN_NONE };
    if (at_video_pump) {
        if (top == GUEST_WND_VIDEO) {
            a.step = QUIT_SKIP_VIDEO;
            a.phase = SHUTDOWN_MODAL_CLOSING;
        }
        return a;
    }
    switch (top) {
    case GUEST_WND_GAME:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.quit_to_windows = 1;
        a.phase = SHUTDOWN_GAMEWND_CLOSING;
        break;
    case GUEST_WND_MAIN_MENU:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.phase = SHUTDOWN_MAINSCREEN_CLOSING;
        break;
    case GUEST_WND_INGAME_OPTIONS:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    case GUEST_WND_QUIT:
        a.step = QUIT_END_WINDOW;
        a.code = 1;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    case GUEST_WND_OTHER:
        a.step = QUIT_END_WINDOW;
        a.code = 0;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    default: break;
    }
    return a;
}

readback_reason_t readback_classify(cpu_access_t access, int read_only, uint32_t caller, uint32_t calibration_caller) {
    if (calibration_caller && caller == calibration_caller) return READBACK_PIXEL_CALIBRATION;
    switch (access) {
    case CPU_ACCESS_LOCK: return read_only ? READBACK_LOCK_READ : READBACK_LOCK_WRITE;
    case CPU_ACCESS_GETDC: return READBACK_GETDC;
    case CPU_ACCESS_BLT_SOURCE: return READBACK_BLT_SOURCE;
    case CPU_ACCESS_FRONT: return READBACK_FRONT_BUFFER;
    default: return READBACK_OTHER;
    }
}

const char* readback_reason_name(readback_reason_t reason) {
    static const char* const names[READBACK_REASON_COUNT] = {
        "READBACK_LOCK_READ", "READBACK_LOCK_WRITE", "READBACK_GETDC", "READBACK_PIXEL_CALIBRATION",
        "READBACK_BLT_SOURCE", "READBACK_FRONT_BUFFER", "READBACK_OTHER",
    };
    return reason >= 0 && reason < READBACK_REASON_COUNT ? names[reason] : "?";
}

#define LOCK_FLAG_WRITEONLY 0x20u
#define LOCK_FLAG_DISCARDCONTENTS 0x2000u

int target_access_reads(cpu_access_t access, uint32_t lock_flags, uint32_t caller) {
    switch (access) {
    case CPU_ACCESS_BLT_DEST: return 0;
    case CPU_ACCESS_LOCK:
        if (lock_flags & (LOCK_FLAG_WRITEONLY | LOCK_FLAG_DISCARDCONTENTS)) return 0;
        return caller != LOCK_CALLER_BEGIN_FRAME && caller != LOCK_CALLER_FLUSH_BATCH;
    default: return 1;
    }
}

int safemode_value_virtual(int legacy) { return !legacy; }

int hardware_water_policy(int hardware_renderer, int value_stored, int software_water_requested) {
    if (!hardware_renderer || value_stored || software_water_requested) return -1;
    return 1;
}

int safemode_prompt_args(uint32_t func, uint32_t type, uint32_t title_id, uint32_t text_id) {
    return func == SAFEMODE_MSGBOX_HELPER && type == SAFEMODE_PROMPT_TYPE && title_id == SAFEMODE_PROMPT_TITLE &&
           text_id == SAFEMODE_PROMPT_TEXT;
}

uint32_t icall_ring_caller(const uint32_t* targets, const uint32_t* from, uint32_t next, unsigned size,
                           const uint32_t* wrappers, unsigned wrapper_count) {
    for (unsigned back = 1; back <= size && back <= next; back++) {
        unsigned k = (next - back) & (size - 1);
        for (unsigned w = 0; w < wrapper_count; w++)
            if (targets[k] == wrappers[w]) return from[k];
    }
    return 0;
}

switch_decision_t renderer_switch_decide(int to_hardware, int scene_loaded, long locks, long dcs, long scenes,
                                         unsigned attempts) {
    if (!to_hardware && scene_loaded) return SWITCH_DEFER;
    if (locks || dcs || scenes) return attempts + 1 >= RENDERER_SWITCH_ATTEMPTS ? SWITCH_GIVE_UP : SWITCH_WAIT;
    return SWITCH_NOW;
}

const char* renderer_switch_name(renderer_switch_t state) {
    static const char* const names[] = { "RENDERER_ACTIVE",      "RENDERER_SWITCH_REQUESTED", "RENDERER_DRAINING",
                                         "RENDERER_REBUILDING",  "RENDERER_ACTIVE_NEW",       "RENDERER_SWITCH_FAILED",
                                         "RENDERER_SWITCH_DEFERRED" };
    return state >= 0 && state <= RENDERER_SWITCH_DEFERRED ? names[state] : "?";
}

void pending_switch_set(pending_switch_t* p, int hardware) { p->value = hardware ? 2 : 1; }

void pending_switch_cancel(pending_switch_t* p) { p->value = 0; }

int pending_switch_peek(const pending_switch_t* p) { return p->value ? p->value - 1 : -1; }

int pending_switch_take(pending_switch_t* p) {
    int v = pending_switch_peek(p);
    p->value = 0;
    return v;
}
