/*
 * Gamepads through SDL3, always together with the keyboard and mouse. Gameplay gets the gamepad as the game's own
 * input: the right stick as mouse motion in the cursor CInput reads, then the buttons and the left stick into its
 * SInput (command bits, analog move) after the keyboard has written them, so every remapped key keeps working. Menus
 * are driven through the game's own pointer and clicks (gamepad_menu.c). SDL is started by platform.c; every SDL call
 * happens on the guest thread.
 */
#include "guest_va.h"
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cheats.h"
#include "gamepad.h"
#include "gamepad_ui.h"
#include "minimap.h"
#include "overlay_policy.h"
#include "pad_sdl.h"
#include "physics_watch.h"
#include "platform.h"
#include "tutorial_prompts.h"

#define CINPUT_READ_VA GVA(0x005F80B0)
#define SINPUT_IN_CINPUT 0x08u
#define SINPUT_ROTATE_X 0x04u
#define SINPUT_MOVE_X 0x0Cu
#define SINPUT_MOVE_Y 0x10u
#define SINPUT_STATE 0x14u
#define SINPUT_HIT 0x18u
#define SAMPLE_INTERVAL_SECONDS 0.001
#define SWITCH_LOG_LIMIT 16
#define INVERT_MOUSE_VA GVA(0x006603D8)
#define MOVE_COMMANDS (JPT_CMD_RUN | JPT_CMD_WALK | JPT_CMD_BACKUP | JPT_CMD_LEFT | JPT_CMD_RIGHT)
#define NEUTRAL_LOG_LIMIT 24
#define HAND_LOG_LIMIT 40

static struct {
    jpt_gamepad_settings_t cfg;
    int opened;
    SDL_Gamepad* pads[PAD_ROSTER_MAX];
    char names[PAD_ROSTER_MAX][64];
    pad_activity_t activity[PAD_ROSTER_MAX];
    pad_roster_t roster;
    pad_tracker_t tracker;
    pad_state_t state;
    int64_t sampled, frequency;
    recomp_func_t cinput_lifted;
    gamepad_ui_t ui;
    pad_play_t play;
    int64_t last_play;
    int neutral_logs, hand_logs;
    ov_commands_t commands;
    struct {
        int lt, mouse, guest, masked, look_wait, blocked;
    } hand;
    int frame_ready, frame_looked;
    pad_chord_t chord;
    int chord_fire;
    uint8_t triggers_held[2];
    int64_t last_ui;
    unsigned connections, disconnections, switch_logs;
    unsigned long play_frames;
} G;

static int64_t now_qpc(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double seconds_between(int64_t a, int64_t b) { return G.frequency ? (double)(b - a) / (double)G.frequency : 0.0; }

static float stick_deadzone(void) { return G.cfg.stick_deadzone / 100.0f; }

static float trigger_deadzone(void) { return G.cfg.trigger_deadzone / 100.0f; }

void gamepad_configure(const jpt_gamepad_settings_t* settings) {
    char binds[512];
    size_t used = 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    G.frequency = f.QuadPart;
    G.cfg = *settings;
    pad_roster_init(&G.roster);
    pad_tracker_init(&G.tracker);
    binds[0] = 0;
    for (int a = 0; a < JPT_ACT_COUNT && used < sizeof binds; a++) {
        int n = _snprintf(binds + used, sizeof binds - used, "%s%s=%s", a ? ", " : "", jpt_pad_actions[a].name,
                          jpt_pad_control_key(G.cfg.bind[a]));
        if (n < 0) break;
        used += (size_t)n;
    }
    binds[sizeof binds - 1] = 0;
    host_log("gamepad", "settings: glyphs %s, look %d%%/%d%%, invert Y %s, stick deadzone %d%%, trigger deadzone %d%%",
             jpt_glyph_style_keys[G.cfg.glyphs], G.cfg.look_x, G.cfg.look_y, G.cfg.invert_y ? "on" : "off",
             G.cfg.stick_deadzone, G.cfg.trigger_deadzone);
    host_log("gamepad", "bindings: %s; Start = pause menu", binds);
}

static void log_device(void) {
    int active = G.roster.active;
    if (G.switch_logs >= SWITCH_LOG_LIMIT) return;
    if (++G.switch_logs == SWITCH_LOG_LIMIT) {
        host_log("gamepad", "device change line limit reached");
        return;
    }
    if (G.tracker.device == PAD_DEVICE_GAMEPAD && active >= 0)
        host_log("gamepad", "active input device: %s controller (%s); glyph set selected: %s",
                 jpt_pad_family_name((jpt_pad_family_t)G.roster.slots[active].family), G.names[active],
                 jpt_pad_family_name(jpt_glyph_family((jpt_glyph_style_t)G.cfg.glyphs, 1,
                                                      (jpt_pad_family_t)G.roster.slots[active].family)));
    else
        host_log("gamepad", "active input device: keyboard and mouse; keycaps selected");
}

/* Z is a plain hold: a latch left on would never end. */
static void to_keyboard_mouse(int changed) {
    if (!changed) return;
    window_host_release_pad_cursor();
    gamepad_menu_forget_focus();
    pad_play_release(&G.play);
    log_device();
}

void gamepad_note_keyboard(void) { to_keyboard_mouse(pad_tracker_keyboard(&G.tracker)); }

void gamepad_note_mouse_button(void) { to_keyboard_mouse(pad_tracker_mouse_button(&G.tracker)); }

void gamepad_note_mouse_motion(int dx, int dy) {
    to_keyboard_mouse(pad_tracker_mouse_motion(&G.tracker, dx, dy, GetTickCount()));
}

static const char* slot_family(int slot) { return jpt_pad_family_name((jpt_pad_family_t)G.roster.slots[slot].family); }

static void open_pad(SDL_JoystickID id) {
    SDL_Gamepad* g;
    const char* name;
    jpt_pad_family_t family;
    pad_state_t st;
    int slot, rumble;
    if (pad_roster_find(&G.roster, (uint32_t)id) >= 0) return;
    g = SDL_OpenGamepad(id);
    if (!g) {
        host_log("gamepad", "a controller could not be opened: %s", SDL_GetError());
        return;
    }
    name = SDL_GetGamepadName(g);
    family = jpt_pad_family_of(pad_sdl_type(SDL_GetGamepadType(g)), SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), name);
    slot = pad_roster_add(&G.roster, (uint32_t)id, family);
    if (slot < 0) {
        SDL_CloseGamepad(g);
        return;
    }
    G.pads[slot] = g;
    _snprintf(G.names[slot], sizeof G.names[slot] - 1, "%s", name ? name : "unnamed controller");
    G.names[slot][sizeof G.names[slot] - 1] = 0;
    pad_activity_reset(&G.activity[slot]);
    pad_sdl_read(g, &st);
    pad_activity_update(&G.activity[slot], &st, stick_deadzone(), trigger_deadzone());
    rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    G.connections++;
    host_log("gamepad", "%s controller connected: %s (%04x:%04x%s)%s", jpt_pad_family_name(family), G.names[slot],
             SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), rumble ? ", rumble" : "",
             G.roster.active == slot ? ", in use" : "");
}

static void close_pad(SDL_JoystickID id) {
    int slot = pad_roster_find(&G.roster, (uint32_t)id), was_active, left;
    if (slot < 0) return;
    host_log("gamepad", "%s controller disconnected: %s", slot_family(slot), G.names[slot]);
    SDL_CloseGamepad(G.pads[slot]);
    G.pads[slot] = NULL;
    G.disconnections++;
    was_active = pad_roster_remove(&G.roster, (uint32_t)id);
    left = pad_roster_count(&G.roster);
    if (was_active && G.roster.active >= 0)
        host_log("gamepad", "now using the %s controller: %s", slot_family(G.roster.active), G.names[G.roster.active]);
    if (pad_tracker_pads_left(&G.tracker, left)) to_keyboard_mouse(1);
    memset(&G.state, 0, sizeof G.state);
}

static void open_connected(void) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) open_pad(ids[i]);
    SDL_free(ids);
}

static int pads_ready(void) {
    if (!platform_gamepads_ready()) return 0;
    if (!G.opened) {
        G.opened = 1;
        open_connected();
        if (!pad_roster_count(&G.roster)) host_log("gamepad", "no controller connected yet");
    }
    return 1;
}

void gamepad_device_added(uint32_t id) { open_pad((SDL_JoystickID)id); }

void gamepad_device_removed(uint32_t id) { close_pad((SDL_JoystickID)id); }

static void sample(int64_t now) {
    int focused;
    if (G.sampled && seconds_between(G.sampled, now) < SAMPLE_INTERVAL_SECONDS) return;
    G.sampled = now;
    memset(&G.state, 0, sizeof G.state);
    platform_pump();
    if (!pads_ready()) return;
    focused = window_host_input_focused();
    for (int slot = 0; slot < PAD_ROSTER_MAX; slot++) {
        pad_state_t st;
        if (!G.roster.slots[slot].used || !G.pads[slot]) continue;
        pad_sdl_read(G.pads[slot], &st);
        if (pad_activity_update(&G.activity[slot], &st, stick_deadzone(), trigger_deadzone()) && focused) {
            if (pad_roster_activity(&G.roster, slot))
                host_log("gamepad", "now using the %s controller: %s", slot_family(slot), G.names[slot]);
            if (pad_tracker_gamepad(&G.tracker)) log_device();
        }
        if (slot == G.roster.active && focused) G.state = st;
    }
    G.chord_fire |= pad_chord_update(&G.chord, MM_PAD_CHORD, G.state.buttons,
                                     (uint32_t)(seconds_between(0, now) * 1000.0), &G.state.buttons);
}

void gamepad_idle(void) { sample(now_qpc()); }

static uint32_t buttons_now(void) {
    return pad_buttons_with_trigger_hold(&G.state, trigger_deadzone(), G.triggers_held);
}

uint32_t gamepad_buttons(void) { return G.state.buttons; }

static float rdf(uint32_t va) { return *(float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

/* Hint goes through the game's own F1 (CGameWnd::OnKey opens CHintWnd) once overlay.c has closed the host's. */
/* Before CInput reads: this frame's pad, and the right stick as mouse motion in the cursor it is about to read. */
static void gameplay_before_read(void) {
    int64_t now = now_qpc();
    double dt = G.last_play ? seconds_between(G.last_play, now) : 0.0;
    pad_read_t kind;
    int dx, dy;
    sample(now);
    kind = pad_read_kind(!G.last_play, G.ui.left_gameplay, window_host_input_focused(), dt);
    G.ui.left_gameplay = 0;
    G.last_play = now;
    pad_play_before(&G.play, kind, &G.state, buttons_now(), &G.cfg, (float)dt, MEM8(INVERT_MOUSE_VA) != 0, &dx,
                    &dy);
    G.frame_looked = dx || dy;
    if (G.frame_looked) window_host_pad_relative(dx, dy);
    G.frame_ready = 1;
}

static void gameplay_after_read(uint32_t sinput) {
    pad_sinput_t in;
    if (!G.frame_ready) return;
    G.frame_ready = 0;
    in.move_x = rdf(sinput + SINPUT_MOVE_X);
    in.move_y = rdf(sinput + SINPUT_MOVE_Y);
    in.state = MEM32(sinput + SINPUT_STATE);
    in.hit = MEM32(sinput + SINPUT_HIT);
    if (pad_play_after(&G.play, &G.cfg, &in)) {
        wrf(sinput + SINPUT_MOVE_X, in.move_x);
        wrf(sinput + SINPUT_MOVE_Y, in.move_y);
        MEM32(sinput + SINPUT_STATE) = in.state;
        MEM32(sinput + SINPUT_HIT) = in.hit;
        G.play_frames++;
    } else if (G.frame_looked) {
        G.play_frames++;
    }
}

static void log_neutral(const char* why, uint32_t held) {
    char names[160];
    size_t used = 0;
    names[0] = 0;
    for (int b = 0; b < JPT_PAD_CONTROL_COUNT && used < sizeof names; b++) {
        int n;
        if (!(held & JPT_PAD_BIT(b))) continue;
        n = _snprintf(names + used, sizeof names - used, "%s%s", used ? " " : "", jpt_pad_control_key(b));
        if (n < 0) break;
        used += (size_t)n;
    }
    names[sizeof names - 1] = 0;
    if (G.neutral_logs >= NEUTRAL_LOG_LIMIT) return;
    if (++G.neutral_logs == NEUTRAL_LOG_LIMIT) host_log("input", "gameplay input neutral: line limit reached");
    else if (held)
        host_log("input", "gameplay input neutral (%s): suppressing %s until release", why, names);
}

/* An interface opened or closed over gameplay: nothing held across it reaches Anne until released and pressed again. */
static void gameplay_neutral(const char* why) {
    uint32_t held = buttons_now();
    pad_play_neutral(&G.play, held);
    log_neutral(why, held);
}

static void gameplay_mask(uint32_t sinput, int blocked, int resumed) {
    uint32_t state = MEM32(sinput + SINPUT_STATE), pass;
    ov_commands_block(&G.commands, blocked, state);
    if (resumed) ov_commands_resume(&G.commands, state);
    pass = ov_commands_pass(&G.commands, state);
    if (blocked || pass == state) return;
    MEM32(sinput + SINPUT_STATE) = pass;
    MEM32(sinput + SINPUT_HIT) &= pass;
    if (G.commands.masked & MOVE_COMMANDS) {
        wrf(sinput + SINPUT_MOVE_X, 0.0f);
        wrf(sinput + SINPUT_MOVE_Y, 0.0f);
    }
}

/* The read's result only (look, move, held commands, new presses): what CInput itself keeps is left alone. */
static void clear_result(uint32_t sinput) {
    wrf(sinput + SINPUT_ROTATE_X, 0.0f);
    wrf(sinput + SINPUT_ROTATE_X + 4, 0.0f);
    wrf(sinput + SINPUT_MOVE_X, 0.0f);
    wrf(sinput + SINPUT_MOVE_Y, 0.0f);
    MEM32(sinput + SINPUT_STATE) = 0;
    MEM32(sinput + SINPUT_HIT) = 0;
}

/* Move Hand as the game sees it, against the LT and mouse button behind it: one line per change. */
static void note_hand(int mouse, uint32_t sinput, int blocked) {
    int lt = (buttons_now() & JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER)) != 0;
    int guest = (MEM32(sinput + SINPUT_STATE) & JPT_CMD_HAND) != 0, masked = (G.commands.masked & JPT_CMD_HAND) != 0;
    int lt_waits = lt && (G.play.edges.blocked & JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER));
    if (lt == G.hand.lt && mouse == G.hand.mouse && guest == G.hand.guest && masked == G.hand.masked &&
        G.play.look_wait == G.hand.look_wait && blocked == G.hand.blocked)
        return;
    G.hand.lt = lt;
    G.hand.mouse = mouse;
    G.hand.guest = guest;
    G.hand.masked = masked;
    G.hand.look_wait = G.play.look_wait;
    G.hand.blocked = blocked;
    if (G.hand_logs >= HAND_LOG_LIMIT) return;
    if (++G.hand_logs == HAND_LOG_LIMIT) host_log("input-hand", "line limit reached");
    else
        host_log("input-hand", "LT %s%s, mouse hand button %s%s, Move Hand %s, right stick %s%s", lt ? "held" : "up",
                 lt_waits ? " (waits for its release)" : "", mouse ? "held" : "up",
                 masked ? " (waits for its release)" : "", guest ? "on" : "off",
                 G.play.look_wait ? "waits for its centre" : "live", blocked ? "; a host interface is up" : "");
}

static void watch(uint32_t sinput, int mouse, int blocked) {
    pw_input_t in;
    in.move_x = rdf(sinput + SINPUT_MOVE_X);
    in.move_y = rdf(sinput + SINPUT_MOVE_Y);
    in.state = MEM32(sinput + SINPUT_STATE);
    in.hit = MEM32(sinput + SINPUT_HIT);
    in.pad_buttons = buttons_now();
    in.lt = (in.pad_buttons & JPT_PAD_BIT(JPT_PAD_LEFT_TRIGGER)) != 0;
    in.mouse_hand = mouse;
    in.stick_x = G.state.rx;
    in.stick_y = G.state.ry;
    in.interface_up = blocked;
    physics_watch_read(&in);
}

/* CInput::tinReadDefaultControls(this): keys, mouse buttons and mouse look into this + 8, returned in eax. */
static void cinput_read(void) {
    uint32_t self = g_ecx, sinput = self + SINPUT_IN_CINPUT;
    int blocked, resumed, mouse;
    if (g_alias_entry) {
        G.cinput_lifted();
        return;
    }
    blocked = overlay_blocks_gameplay();
    resumed = G.ui.left_gameplay && !blocked;
    if (blocked != G.commands.blocked)
        gameplay_neutral(blocked ? "a host interface opened" : "a host interface closed");
    else if (resumed)
        gameplay_neutral("gameplay resumed after a window of the game");
    gameplay_before_read();
    G.cinput_lifted();
    mouse = (MEM32(sinput + SINPUT_STATE) & JPT_CMD_HAND) != 0;
    gameplay_after_read(sinput);
    gameplay_mask(sinput, blocked, resumed);
    if (blocked || cheats_take_input(sinput)) clear_result(sinput);
    note_hand(mouse, sinput, blocked);
    watch(sinput, mouse, blocked);
}

int gamepad_install(void) {
    if (!guest_override_install(CINPUT_READ_VA, cinput_read, &G.cinput_lifted)) {
        host_log("gamepad", "warning: the game's input read could not be hooked; gamepads work in menus only");
        return 0;
    }
    return 1;
}

static void toggle_minimap_from_pad(const char* source) { window_host_toggle_minimap(source); }

void gamepad_ui_loop(void) {
    gamepad_ui_in_t in;
    int64_t now = now_qpc();
    double dt = G.last_ui ? seconds_between(G.last_ui, now) : 0.0;
    G.last_ui = now;
    sample(now);
    in.pad = &G.state;
    in.cfg = &G.cfg;
    in.buttons = buttons_now();
    in.now_ms = GetTickCount();
    in.dt = (float)dt;
    in.device_gamepad = G.tracker.device == PAD_DEVICE_GAMEPAD;
    in.chord = G.chord_fire;
    G.chord_fire = 0;
    in.stick_deadzone = stick_deadzone();
    in.toggle_minimap = toggle_minimap_from_pad;
    gamepad_ui_step(&G.ui, &in);
}

void gamepad_overlay_changed(void) { gamepad_ui_overlay_changed(&G.ui, buttons_now()); }

void gamepad_video_loop(void) {
    sample(now_qpc());
    gamepad_ui_video(&G.ui, buttons_now());
}

int gamepad_menu_focus(void) {
    return G.tracker.device == PAD_DEVICE_GAMEPAD && G.ui.mode == GUI_MENU && !gamepad_menu_pointer_free();
}

void gamepad_prompts(gamepad_prompts_t* out) {
    int active = G.roster.active;
    jpt_pad_family_t family = active >= 0 ? (jpt_pad_family_t)G.roster.slots[active].family : JPT_FAMILY_XBOX;
    out->gamepad = G.tracker.device == PAD_DEVICE_GAMEPAD && active >= 0;
    out->family = jpt_glyph_family((jpt_glyph_style_t)G.cfg.glyphs, active >= 0, family);
    out->settings = &G.cfg;
}

int gamepad_rumble(float low, float high, unsigned ms) {
    int active = G.roster.active;
    uint16_t motor_low, motor_high;
    if (!platform_gamepads_ready() || active < 0 || !G.pads[active]) return 0;
    pad_rumble_motors(low, high, &motor_low, &motor_high);
    return SDL_RumbleGamepad(G.pads[active], motor_low, motor_high, ms);
}

void gamepad_summary(void) {
    host_log("gamepad", "summary: %u connected, %u disconnected, %u device changes, %lu gameplay frames with gamepad "
             "input%s", G.connections, G.disconnections, G.tracker.switches, G.play_frames,
             platform_gamepads_ready() ? "" : " (SDL gamepad support unavailable)");
}
