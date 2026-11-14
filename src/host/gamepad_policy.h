/* The gamepad's decisions without SDL or windows: stick and trigger shaping, which device the player used last, which
 * controller is in use, gameplay input for the game's CInput and focus movement in its menus. */
#ifndef GAMEPAD_POLICY_H
#define GAMEPAD_POLICY_H

#include <stdint.h>

#include "jpt_gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t buttons;      /* JPT_PAD_BIT(control) of the digital buttons */
    float lx, ly, rx, ry;  /* -1..1, up negative as SDL reports */
    float lt, rt;          /* 0..1 */
} pad_state_t;

/* A radial deadzone: inside it the stick is centred, outside its travel is rescaled to 0..1 in the same direction. */
void pad_stick_shape(float x, float y, float deadzone, float* out_x, float* out_y);
float pad_trigger_shape(float value, float deadzone);
/* The look curve on a shaped stick's distance: fine aim near the centre, full speed at the edge. */
float pad_look_curve(float m);
/* The buttons of a sample with the triggers added as buttons past their deadzone. */
uint32_t pad_buttons_with_triggers(const pad_state_t* s, float trigger_deadzone);
/* With hysteresis; held[2] (LT, RT) keeps the state between samples. */
#define PAD_TRIGGER_HOLD_MARGIN 0.08f
uint32_t pad_buttons_with_trigger_hold(const pad_state_t* s, float trigger_deadzone, uint8_t held[2]);

/* Only the start of meaningful input counts: a new press, a stick or trigger crossing its threshold. Held buttons,
 * resting drift and trigger noise never do, so they cannot take the prompts back from the mouse. */
typedef struct {
    uint32_t buttons;
    uint8_t stick_on[4];
    uint8_t trigger_on[2];
} pad_activity_t;

/* Thresholds past the deadzone, so drift that sits just outside it is never activity. */
#define PAD_ACTIVITY_STICK_MARGIN 0.20f
#define PAD_ACTIVITY_TRIGGER_MARGIN 0.15f
void pad_activity_reset(pad_activity_t* a);
int pad_activity_update(pad_activity_t* a, const pad_state_t* s, float stick_deadzone, float trigger_deadzone);

typedef enum { PAD_DEVICE_KEYBOARD_MOUSE, PAD_DEVICE_GAMEPAD } pad_device_t;

/* The last device with meaningful activity. The mouse counts after PAD_MOUSE_SWITCH_DISTANCE of motion inside
 * PAD_MOUSE_WINDOW_MS, so a nudged or jittering mouse does not switch. */
#define PAD_MOUSE_SWITCH_DISTANCE 24
#define PAD_MOUSE_WINDOW_MS 250u
typedef struct {
    pad_device_t device;
    int mouse_distance;
    uint32_t mouse_last_ms;
    int mouse_seen;
    unsigned switches;
} pad_tracker_t;

void pad_tracker_init(pad_tracker_t* t);
/* Each returns 1 when the device changed. */
int pad_tracker_keyboard(pad_tracker_t* t);
int pad_tracker_mouse_button(pad_tracker_t* t);
int pad_tracker_mouse_motion(pad_tracker_t* t, int dx, int dy, uint32_t now_ms);
int pad_tracker_gamepad(pad_tracker_t* t);
/* A controller went away: with none left the keyboard and mouse take over. */
int pad_tracker_pads_left(pad_tracker_t* t, int pads_left);

/* Connected controllers; the one with the latest activity is in use, the first one connected until then. */
#define PAD_ROSTER_MAX 8
typedef struct {
    uint32_t id;
    int family;
    uint32_t order;
    int used;
} pad_slot_t;

typedef struct {
    pad_slot_t slots[PAD_ROSTER_MAX];
    int active;
    uint32_t clock;
} pad_roster_t;

void pad_roster_init(pad_roster_t* r);
/* The slot of a new controller, -1 when the roster is full or it is already in it. */
int pad_roster_add(pad_roster_t* r, uint32_t id, int family);
/* 1 when the controller in use went away; another connected one, the last used, takes its place. */
int pad_roster_remove(pad_roster_t* r, uint32_t id);
int pad_roster_find(const pad_roster_t* r, uint32_t id);
/* 1 when this controller becomes the one in use. */
int pad_roster_activity(pad_roster_t* r, int slot);
int pad_roster_count(const pad_roster_t* r);

/* Gameplay: the game's CInput values (SInput at CInput + 8). */
typedef struct {
    float move_x, move_y; /* sidestep right, forward */
    uint32_t state, hit;  /* command bits held, and pressed since the last frame */
} pad_sinput_t;

/* The keyboard's limits (CInput::tinReadDefaultControls): run 1.0 forward, back 0.4, sidestep 0.6. */
#define PAD_MOVE_FORWARD 1.0f
#define PAD_MOVE_BACK 0.4f
#define PAD_MOVE_SIDE 0.6f
/* Full stick at 100%: radians per second. */
#define PAD_LOOK_YAW_RATE 2.6f
#define PAD_LOOK_PITCH_RATE 1.8f
#define PAD_MAX_FRAME_SECONDS 0.1f

typedef struct {
    float move_x, move_y, look_x, look_y; /* look: radians this frame, +x right, +y up */
    uint32_t held;                        /* command bits */
} pad_gameplay_t;

/* Each bound control gives its action's command bit and nothing else, as the key map does for a key or mouse button:
 * Rotate Wrist and Rotate Arm act only with Move Hand held, as Shift and Ctrl do with the left mouse button. */
void pad_gameplay_from(const pad_state_t* s, uint32_t buttons, const jpt_gamepad_settings_t* cfg, float dt, int crouch,
                       pad_gameplay_t* out);
void pad_merge_sinput(pad_sinput_t* in, const pad_gameplay_t* pad, uint32_t pad_hit);

/* CInput turns each count of mouse motion into this many radians (0x005F8130). */
#define PAD_RADIANS_PER_COUNT 0.005f

/* The right stick's look as mouse counts, moved into the cursor CInput reads, so the view, the hand, the wrist and the
 * arm get exactly what the mouse gives them. The fraction of a count is kept for the next frame; a centred stick
 * stops at once, as a mouse does. invert_mouse: the game's own InvertMouse, which it applies to these counts too. */
typedef struct {
    float rest_x, rest_y;
} pad_mouse_t;

void pad_mouse_counts(pad_mouse_t* m, float look_x, float look_y, int invert_mouse, int* dx, int* dy);

/* CPlayer crouches while the command is held and has no toggle: B is the hold, L3 a latch on the same command that a
 * B or Jump press clears, so releasing B always stands. */
typedef struct {
    uint8_t latched;
} pad_crouch_t;

int pad_crouch_update(pad_crouch_t* c, int hold_down, int toggle_pressed, int hold_pressed, int jump_pressed);
void pad_crouch_stand(pad_crouch_t* c);

/* The command bits of the actions bound to these controls (the crouch toggle left out). */
uint32_t pad_commands_for(const jpt_gamepad_settings_t* cfg, uint32_t buttons);

/* Presses since the last update. Buttons held when a mode starts (a menu closing, gameplay resuming) are ignored until
 * they are released, so the press that closed a menu does not also jump. */
typedef struct {
    uint32_t previous, blocked;
} pad_edges_t;

void pad_edges_reset(pad_edges_t* e, uint32_t held_now);
/* The held buttons that count; *pressed gets the ones pressed since the last update. */
uint32_t pad_edges_update(pad_edges_t* e, uint32_t held, uint32_t* pressed);
/* Gameplay reads again after a pause (a menu, a video, another window, a long frame): a button that counted at the last
 * read and is still held keeps counting with no new press, as a held key or mouse button does for CInput; one pressed
 * meanwhile waits for its release. */
void pad_edges_resume(pad_edges_t* e, uint32_t held_now);

/* What the previous gameplay read was to this one. */
typedef enum {
    PAD_READ_NEXT,    /* the last frame */
    PAD_READ_RESUME,  /* before a menu, a video or another window, less than PAD_READ_GAP_SECONDS ago */
    PAD_READ_RESTART  /* none yet, no focus, or PAD_READ_GAP_SECONDS or more: the crouch latch, the look's fraction
                         and the frame time start again as well */
} pad_read_t;

#define PAD_READ_GAP_SECONDS 0.25
pad_read_t pad_read_kind(int first, int left_gameplay, int focused, double since_last);

int pad_bound(const jpt_gamepad_settings_t* cfg, int action, uint32_t buttons);

/* The pad around one CInput read: its command bits, its move and the right stick as mouse counts. */
typedef struct {
    pad_edges_t edges;
    pad_crouch_t crouch;
    pad_mouse_t mouse;
    pad_gameplay_t frame;
    uint32_t pressed;
    int move_wait, look_wait; /* each stick waits for its own centre */
} pad_play_t;

void pad_play_release(pad_play_t* p);
/* An interface opened or closed over gameplay: every held button waits for its release, each stick for its centre,
 * the crouch latch and the look's fraction start again. */
void pad_play_neutral(pad_play_t* p, uint32_t held_now);

/* Before CInput reads: *dx, *dy are the counts to move the cursor CInput is about to read. */
void pad_play_before(pad_play_t* p, pad_read_t kind, const pad_state_t* s, uint32_t buttons,
                     const jpt_gamepad_settings_t* cfg, float dt, int invert_mouse, int* dx, int* dy);
/* After: the pad's command bits, presses and move added to what CInput wrote; 0 when it had none. */
int pad_play_after(const pad_play_t* p, const jpt_gamepad_settings_t* cfg, pad_sinput_t* in);

/* Menus: focus moves between the game's controls (canvas rectangles) like the arrow keys of a modern menu. */
typedef struct {
    int left, top, right, bottom;
} pad_rect_t;

typedef enum { PAD_NAV_NONE = -1, PAD_NAV_UP, PAD_NAV_DOWN, PAD_NAV_LEFT, PAD_NAV_RIGHT } pad_nav_dir_t;

/* The nearest target in a direction from a rectangle, -1 for none. */
int pad_nav_pick(const pad_rect_t* targets, int count, pad_rect_t from, pad_nav_dir_t dir);
/* The direction a stick or the D-pad points in, with hysteresis on the stick: previous is the last result. */
pad_nav_dir_t pad_nav_direction(uint32_t buttons, float x, float y, pad_nav_dir_t previous);

/* A held direction repeats after PAD_REPEAT_DELAY_MS, then every PAD_REPEAT_MS. */
#define PAD_REPEAT_DELAY_MS 380u
#define PAD_REPEAT_MS 110u
typedef struct {
    pad_nav_dir_t dir;
    uint32_t next;
} pad_repeat_t;

void pad_repeat_reset(pad_repeat_t* r);
/* The direction to act on now, PAD_NAV_NONE when nothing fires. */
pad_nav_dir_t pad_repeat_update(pad_repeat_t* r, pad_nav_dir_t held, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
