/*
 * Gamepads through SDL3, always together with the keyboard and mouse. Gameplay gets the gamepad as the game's own
 * input: the right stick as mouse motion in the cursor CInput reads, then the buttons and the left stick into its
 * SInput (command bits, analog move) after the keyboard has written them, so every remapped key keeps working. Menus
 * are driven through the game's own pointer and clicks (gamepad_menu.c). Every SDL call happens on the guest thread.
 */
#include "host.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cheats.h"
#include "gamepad.h"
#include "tutorial_prompts.h"

#define CINPUT_READ_VA 0x005F80B0u
#define SINPUT_IN_CINPUT 0x08u
#define SINPUT_ROTATE_X 0x04u
#define SINPUT_MOVE_X 0x0Cu
#define SINPUT_MOVE_Y 0x10u
#define SINPUT_STATE 0x14u
#define SINPUT_HIT 0x18u
#define SAMPLE_INTERVAL_SECONDS 0.001
#define SWITCH_LOG_LIMIT 16
#define INVERT_MOUSE_VA 0x006603D8u

typedef enum { MODE_NONE, MODE_GAMEPLAY, MODE_MENU, MODE_VIDEO } ui_mode_t;

static struct {
    jpt_gamepad_settings_t cfg;
    int sdl;  /* 0 not started, 1 ready, -1 unavailable */
    SDL_Gamepad* pads[PAD_ROSTER_MAX];
    char names[PAD_ROSTER_MAX][64];
    pad_activity_t activity[PAD_ROSTER_MAX];
    pad_roster_t roster;
    pad_tracker_t tracker;
    pad_state_t state;
    int64_t sampled, frequency;
    recomp_func_t cinput_lifted;
    pad_edges_t ui_edges;
    pad_play_t play;
    int64_t last_play;
    int left_gameplay, card_request;
    int frame_ready, frame_looked;
    int hint_shown;
    uint8_t triggers_held[2];
    ui_mode_t ui_mode;
    uint32_t ui_top;
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

static jpt_pad_type_t pad_type(SDL_GamepadType t) {
    return t > SDL_GAMEPAD_TYPE_UNKNOWN && (int)t < (int)JPT_PAD_TYPE_COUNT ? (jpt_pad_type_t)t : JPT_PAD_TYPE_UNKNOWN;
}

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

static void read_pad(SDL_Gamepad* g, pad_state_t* s) {
    memset(s, 0, sizeof *s);
    for (int b = 0; b <= JPT_PAD_DPAD_RIGHT; b++)
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) s->buttons |= JPT_PAD_BIT(b);
    s->lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX);
    s->ly = axis(g, SDL_GAMEPAD_AXIS_LEFTY);
    s->rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    s->ry = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    s->lt = axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s->rt = axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (s->lt < 0) s->lt = 0;
    if (s->rt < 0) s->rt = 0;
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
    family = jpt_pad_family_of(pad_type(SDL_GetGamepadType(g)), SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g), name);
    slot = pad_roster_add(&G.roster, (uint32_t)id, family);
    if (slot < 0) {
        SDL_CloseGamepad(g);
        return;
    }
    G.pads[slot] = g;
    _snprintf(G.names[slot], sizeof G.names[slot] - 1, "%s", name ? name : "unnamed controller");
    G.names[slot][sizeof G.names[slot] - 1] = 0;
    pad_activity_reset(&G.activity[slot]);
    read_pad(g, &st);
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

/* Only connections are taken from SDL's queue; state is read directly, so the per-input events are turned off. */
static void quiet_events(void) {
    static const Uint32 noisy[] = {
        SDL_EVENT_JOYSTICK_AXIS_MOTION,  SDL_EVENT_JOYSTICK_BALL_MOTION,     SDL_EVENT_JOYSTICK_HAT_MOTION,
        SDL_EVENT_JOYSTICK_BUTTON_DOWN,  SDL_EVENT_JOYSTICK_BUTTON_UP,      SDL_EVENT_JOYSTICK_BATTERY_UPDATED,
        SDL_EVENT_GAMEPAD_AXIS_MOTION,   SDL_EVENT_GAMEPAD_BUTTON_DOWN,     SDL_EVENT_GAMEPAD_BUTTON_UP,
        SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION, SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
        SDL_EVENT_GAMEPAD_SENSOR_UPDATE,
    };
    for (size_t i = 0; i < sizeof noisy / sizeof noisy[0]; i++) SDL_SetEventEnabled(noisy[i], false);
}

static int ensure_sdl(void) {
    int version;
    if (G.sdl) return G.sdl > 0;
    version = SDL_GetVersion();
    /* SDL's raw input joystick driver reads the thread's whole raw input buffer, the game's mouse included. */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        G.sdl = -1;
        host_log("gamepad", "SDL %d.%d.%d could not start its gamepad support (%s): keyboard and mouse only",
                 SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version),
                 SDL_GetError());
        return 0;
    }
    G.sdl = 1;
    quiet_events();
    host_log("gamepad", "SDL %d.%d.%d gamepads ready, connections followed while the game runs; the keyboard and mouse "
             "stay active", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
    open_connected();
    if (!pad_roster_count(&G.roster)) host_log("gamepad", "no controller connected yet");
    return 1;
}

static void take_events(void) {
    SDL_Event events[16];
    int n;
    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(events, 16, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0)
        for (int i = 0; i < n; i++) {
            if (events[i].type == SDL_EVENT_GAMEPAD_ADDED) open_pad(events[i].gdevice.which);
            else if (events[i].type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(events[i].gdevice.which);
        }
}

static void sample(int64_t now) {
    int focused;
    if (G.sampled && seconds_between(G.sampled, now) < SAMPLE_INTERVAL_SECONDS) return;
    G.sampled = now;
    memset(&G.state, 0, sizeof G.state);
    if (!ensure_sdl()) return;
    take_events();
    focused = window_host_input_focused();
    for (int slot = 0; slot < PAD_ROSTER_MAX; slot++) {
        pad_state_t st;
        if (!G.roster.slots[slot].used || !G.pads[slot]) continue;
        read_pad(G.pads[slot], &st);
        if (pad_activity_update(&G.activity[slot], &st, stick_deadzone(), trigger_deadzone()) && focused) {
            if (pad_roster_activity(&G.roster, slot))
                host_log("gamepad", "now using the %s controller: %s", slot_family(slot), G.names[slot]);
            if (pad_tracker_gamepad(&G.tracker)) log_device();
        }
        if (slot == G.roster.active && focused) G.state = st;
    }
}

void gamepad_idle(void) { sample(now_qpc()); }

static uint32_t buttons_now(void) {
    return pad_buttons_with_trigger_hold(&G.state, trigger_deadzone(), G.triggers_held);
}

uint32_t gamepad_buttons(void) { return G.state.buttons; }

static float rdf(uint32_t va) { return *(float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static int bound(int action, uint32_t buttons) { return pad_bound(&G.cfg, action, buttons); }

/* Before CInput reads: this frame's pad, and the right stick as mouse motion in the cursor it is about to read. */
static void gameplay_before_read(void) {
    int64_t now = now_qpc();
    double dt = G.last_play ? seconds_between(G.last_play, now) : 0.0;
    pad_read_t kind;
    int dx, dy;
    sample(now);
    kind = pad_read_kind(!G.last_play, G.left_gameplay, window_host_input_focused(), dt);
    G.left_gameplay = 0;
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

/* CInput::tinReadDefaultControls(this): keys, mouse buttons and mouse look into this + 8, returned in eax. */
static void cinput_read(void) {
    uint32_t self = g_ecx;
    int alias = g_alias_entry != 0;
    if (!alias) gameplay_before_read();
    G.cinput_lifted();
    if (alias) return;
    gameplay_after_read(self + SINPUT_IN_CINPUT);
    if (debug_menu_blocks_gameplay() || cheats_take_input(self + SINPUT_IN_CINPUT))
        memset((void*)ADDR(self + SINPUT_IN_CINPUT + SINPUT_ROTATE_X), 0, SINPUT_HIT + 4 - SINPUT_ROTATE_X);
}

int gamepad_install(void) {
    if (!guest_override_install(CINPUT_READ_VA, cinput_read, &G.cinput_lifted)) {
        host_log("gamepad", "warning: the game's input read could not be hooked; gamepads work in menus only");
        return 0;
    }
    return 1;
}

static ui_mode_t mode_of(const guest_ui_view_t* v) {
    if (v->kind == GUEST_WND_LOADER || v->kind == GUEST_WND_VIDEO) return MODE_NONE;
    if (v->ui == GUEST_UI_GAMEPLAY) return MODE_GAMEPLAY;
    if (v->ui == GUEST_UI_MENU || v->ui == GUEST_UI_DIALOG) return MODE_MENU;
    return MODE_NONE;
}

void gamepad_ui_loop(void) {
    guest_ui_view_t v;
    int64_t now = now_qpc();
    uint32_t buttons, held, pressed, released, before;
    double dt = G.last_ui ? seconds_between(G.last_ui, now) : 0.0;
    ui_mode_t mode;
    G.last_ui = now;
    sample(now);
    guest_ui_probe(&v);
    mode = mode_of(&v);
    buttons = buttons_now();
    if (mode != G.ui_mode || v.top != G.ui_top) {
        pad_edges_reset(&G.ui_edges, buttons);
        if (mode == MODE_MENU) gamepad_menu_reset();
        G.ui_mode = mode;
        G.ui_top = v.top;
    }
    before = G.ui_edges.previous;
    held = pad_edges_update(&G.ui_edges, buttons, &pressed);
    released = before & ~held;
    if (debug_menu_visible() && mode != MODE_NONE) {
        if (mode == MODE_MENU) G.left_gameplay = 1;
        debug_menu_pad(held, pressed);
    } else if (mode == MODE_GAMEPLAY) {
        if (pressed & JPT_PAD_BIT(JPT_PAD_START)) window_host_post_key(VK_ESCAPE);
        if ((pressed & JPT_PAD_BIT(JPT_PAD_BACK)) || G.card_request) tutorial_prompts_toggle_card();
        else if (!G.hint_shown) G.hint_shown = tutorial_prompts_hint();
        if (bound(JPT_ACT_JUMP, pressed)) window_host_post_key(VK_SPACE);
    } else {
        G.left_gameplay = 1;
        if (mode == MODE_MENU)
            gamepad_menu_frame(&G.state, held, pressed, released, G.tracker.device == PAD_DEVICE_GAMEPAD,
                               GetTickCount(), (float)(dt > 0.1 ? 0.1 : dt), stick_deadzone());
    }
    G.card_request = 0;
}

void gamepad_controls_key(void) { G.card_request = 1; }

void gamepad_video_loop(void) {
    const uint32_t skip = JPT_PAD_BIT(JPT_PAD_SOUTH) | JPT_PAD_BIT(JPT_PAD_EAST) | JPT_PAD_BIT(JPT_PAD_START);
    uint32_t pressed;
    G.left_gameplay = 1;
    sample(now_qpc());
    if (G.ui_mode != MODE_VIDEO) {
        pad_edges_reset(&G.ui_edges, buttons_now());
        G.ui_mode = MODE_VIDEO;
        G.ui_top = 0;
    }
    pad_edges_update(&G.ui_edges, buttons_now(), &pressed);
    if (pressed & skip) window_host_post_key_up(VK_ESCAPE);
}

int gamepad_menu_focus(void) {
    return G.tracker.device == PAD_DEVICE_GAMEPAD && G.ui_mode == MODE_MENU && !gamepad_menu_pointer_free();
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
    if (G.sdl <= 0 || active < 0 || !G.pads[active]) return 0;
    low = low < 0 ? 0 : low > 1 ? 1 : low;
    high = high < 0 ? 0 : high > 1 ? 1 : high;
    return SDL_RumbleGamepad(G.pads[active], (Uint16)(low * 65535.0f), (Uint16)(high * 65535.0f), ms);
}

void gamepad_summary(void) {
    host_log("gamepad", "summary: %u connected, %u disconnected, %u device changes, %lu gameplay frames with gamepad "
             "input%s", G.connections, G.disconnections, G.tracker.switches, G.play_frames,
             G.sdl < 0 ? " (SDL gamepad support unavailable)" : "");
}
