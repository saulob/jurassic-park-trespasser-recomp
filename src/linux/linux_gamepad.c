#include "linux_gamepad.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "guest_memory.h"
#include "guest_abi.h"
#include "guest_ptr.h"
#include "gamepad.h"
#include "gamepad_ui.h"
#include "jpt_sync.h"
#include "linux_features.h"
#include "minimap.h"
#include "linux_log.h"
#include "linux_user32.h"
#include "pad_sdl.h"

#define SINPUT_IN_CINPUT 0x08u
#define SINPUT_MOVE_X 0x0Cu
#define SINPUT_MOVE_Y 0x10u
#define SINPUT_STATE 0x14u
#define SINPUT_HIT 0x18u
#define INVERT_MOUSE_VA 0x006603D8u
#define HOTPLUG_SCAN_MS 200u
#define VK_ESCAPE 0x1Bu
#define VK_SPACE 0x20u

static struct {
    int ready;
    jpt_gamepad_settings_t cfg;
    void* pads[PAD_ROSTER_MAX];
    char names[PAD_ROSTER_MAX][64];
    pad_activity_t activity[PAD_ROSTER_MAX];
    pad_roster_t roster;
    pad_tracker_t tracker;
    pad_state_t state;
    pad_play_t play;
    uint8_t triggers_held[2];
    uint64_t last_scan_ms, last_play;
    int frame_ready, frame_looked;
    lx_gamepad_stats_t stats;
    pad_chord_t chord;
    int chord_fire;
    gamepad_ui_t ui;
    uint64_t last_ui;
} G;

/* ---- the SDL3 backend ---- */

static int sdl_init(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    return SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : 0;
}
static void sdl_quit(void) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); }
static void sdl_update(void) { SDL_UpdateGamepads(); }

static int sdl_list(uint32_t* ids, int capacity) {
    int count = 0, n;
    SDL_JoystickID* list = SDL_GetGamepads(&count);
    n = count < capacity ? count : capacity;
    for (int i = 0; list && i < n; i++) ids[i] = (uint32_t)list[i];
    SDL_free(list);
    return list ? n : 0;
}

static void* sdl_open(uint32_t id) { return SDL_OpenGamepad((SDL_JoystickID)id); }
static void sdl_close(void* h) { SDL_CloseGamepad((SDL_Gamepad*)h); }
static void sdl_read(void* h, pad_state_t* out) { pad_sdl_read((SDL_Gamepad*)h, out); }

static void sdl_info(void* h, lx_pad_info_t* out) {
    SDL_Gamepad* g = (SDL_Gamepad*)h;
    const char* name = SDL_GetGamepadName(g);
    snprintf(out->name, sizeof out->name, "%s", name ? name : "unnamed controller");
    out->type = pad_sdl_type(SDL_GetGamepadType(g));
    out->vendor = SDL_GetGamepadVendor(g);
    out->product = SDL_GetGamepadProduct(g);
    out->rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

static int sdl_rumble(void* h, uint16_t low, uint16_t high, unsigned ms) { return SDL_RumbleGamepad((SDL_Gamepad*)h, low, high, ms) ? 1 : 0; }

static const lx_pad_backend_t k_sdl = { sdl_init, sdl_quit, sdl_update, sdl_list, sdl_open, sdl_close, sdl_read, sdl_info, sdl_rumble };
static const lx_pad_backend_t* s_backend = &k_sdl;

void lx_gamepad_set_backend(const lx_pad_backend_t* backend) { s_backend = backend ? backend : &k_sdl; }

static float stick_deadzone(void) { return G.cfg.stick_deadzone / 100.0f; }
static float trigger_deadzone(void) { return G.cfg.trigger_deadzone / 100.0f; }

static double seconds_between(uint64_t a, uint64_t b) { return (double)(b - a) / (double)jpt_counter_frequency(); }

int lx_gamepad_init(const jpt_gamepad_settings_t* settings) {
    if (G.ready) lx_gamepad_shutdown();
    memset(&G, 0, sizeof G);
    if (!s_backend->init()) {
        lx_log("Linux gamepad", "gamepads are not available: %s", SDL_GetError());
        return 0;
    }
    G.ready = 1;
    G.cfg = *settings;
    pad_roster_init(&G.roster);
    pad_tracker_init(&G.tracker);
    lx_log("Linux gamepad", "settings: look %d%%/%d%%, invert Y %s, stick deadzone %d%%, trigger deadzone %d%%", G.cfg.look_x, G.cfg.look_y,
           G.cfg.invert_y ? "on" : "off", G.cfg.stick_deadzone, G.cfg.trigger_deadzone);
    return 1;
}

static void close_slot(int slot) {
    if (G.pads[slot]) s_backend->close(G.pads[slot]);
    G.pads[slot] = NULL;
}

void lx_gamepad_shutdown(void) {
    if (!G.ready) return;
    for (int i = 0; i < PAD_ROSTER_MAX; i++) close_slot(i);
    s_backend->quit();
    G.ready = 0;
}

static void open_pad(uint32_t id) {
    void* g;
    lx_pad_info_t info;
    jpt_pad_family_t family;
    pad_state_t st;
    int slot;
    if (pad_roster_find(&G.roster, id) >= 0) return;
    g = s_backend->open(id);
    if (!g) {
        lx_log("Linux gamepad", "a controller could not be opened: %s", SDL_GetError());
        return;
    }
    s_backend->info(g, &info);
    family = jpt_pad_family_of(info.type, info.vendor, info.product, info.name);
    slot = pad_roster_add(&G.roster, id, family);
    if (slot < 0) {
        s_backend->close(g);
        return;
    }
    G.pads[slot] = g;
    snprintf(G.names[slot], sizeof G.names[slot], "%s", info.name);
    pad_activity_reset(&G.activity[slot]);
    s_backend->read(g, &st);
    pad_activity_update(&G.activity[slot], &st, stick_deadzone(), trigger_deadzone());
    G.stats.connections++;
    lx_log("Linux gamepad", "%s controller connected: %s (%04x:%04x%s)%s", jpt_pad_family_name(family), G.names[slot], info.vendor, info.product,
           info.rumble ? ", rumble" : "", G.roster.active == slot ? ", in use" : "");
}

static void close_pad(int slot) {
    uint32_t id = G.roster.slots[slot].id;
    int was_active;
    lx_log("Linux gamepad", "controller disconnected: %s", G.names[slot]);
    close_slot(slot);
    G.stats.disconnections++;
    was_active = pad_roster_remove(&G.roster, id);
    if (was_active && G.roster.active >= 0) lx_log("Linux gamepad", "now using: %s", G.names[G.roster.active]);
    if (pad_tracker_pads_left(&G.tracker, pad_roster_count(&G.roster))) pad_play_release(&G.play);
    memset(&G.state, 0, sizeof G.state);
}

static void scan_devices(void) {
    uint32_t ids[PAD_ROSTER_MAX * 2];
    int count = s_backend->list(ids, PAD_ROSTER_MAX * 2);
    for (int slot = 0; slot < PAD_ROSTER_MAX; slot++) {
        int present = 0;
        if (!G.roster.slots[slot].used) continue;
        for (int i = 0; i < count; i++) present |= ids[i] == G.roster.slots[slot].id;
        if (!present) close_pad(slot);
    }
    for (int i = 0; i < count; i++) open_pad(ids[i]);
}

void lx_gamepad_poll(void) {
    uint64_t now = jpt_time_ms();
    if (!G.ready) return;
    s_backend->update();
    if (!G.last_scan_ms || now - G.last_scan_ms >= HOTPLUG_SCAN_MS) {
        G.last_scan_ms = now ? now : 1;
        scan_devices();
    }
    memset(&G.state, 0, sizeof G.state);
    for (int slot = 0; slot < PAD_ROSTER_MAX; slot++) {
        pad_state_t st;
        if (!G.roster.slots[slot].used || !G.pads[slot]) continue;
        s_backend->read(G.pads[slot], &st);
        if (pad_activity_update(&G.activity[slot], &st, stick_deadzone(), trigger_deadzone())) {
            if (pad_roster_activity(&G.roster, slot)) lx_log("Linux gamepad", "now using: %s", G.names[slot]);
            if (pad_tracker_gamepad(&G.tracker)) G.stats.switches++;
        }
        if (slot == G.roster.active) G.state = st;
    }
}

int lx_gamepad_connected(void) { return G.ready && pad_roster_count(&G.roster) > 0; }
const pad_state_t* lx_gamepad_state(void) { return &G.state; }
pad_device_t lx_gamepad_device(void) { return G.tracker.device; }

int gamepad_menu_focus(void) { return G.ready && G.tracker.device == PAD_DEVICE_GAMEPAD && G.ui.mode == GUI_MENU && !gamepad_menu_pointer_free(); }

void gamepad_prompts(gamepad_prompts_t* out) {
    const int active = G.ready ? G.roster.active : -1;
    const jpt_pad_family_t family = active >= 0 ? (jpt_pad_family_t)G.roster.slots[active].family : JPT_FAMILY_XBOX;
    out->gamepad = G.ready && G.tracker.device == PAD_DEVICE_GAMEPAD && active >= 0;
    out->family = jpt_glyph_family((jpt_glyph_style_t)G.cfg.glyphs, active >= 0, family);
    out->settings = &G.cfg;
}

static void to_keyboard_mouse(int changed) {
    if (!changed) return;
    pad_play_release(&G.play);
    gamepad_menu_forget_focus();
    G.stats.switches++;
}

void lx_gamepad_note_keyboard(void) { to_keyboard_mouse(pad_tracker_keyboard(&G.tracker)); }

void lx_gamepad_note_mouse_button(void) { to_keyboard_mouse(pad_tracker_mouse_button(&G.tracker)); }

void lx_gamepad_note_mouse_motion(int dx, int dy) { to_keyboard_mouse(pad_tracker_mouse_motion(&G.tracker, dx, dy, (uint32_t)jpt_time_ms())); }

static uint32_t buttons_now(void) { return pad_buttons_with_trigger_hold(&G.state, trigger_deadzone(), G.triggers_held); }

static float read_float(uint32_t va) {
    uint32_t bits = 0;
    float v;
    gp_read_u32(va, &bits);
    memcpy(&v, &bits, sizeof v);
    return v;
}

static void write_float(uint32_t va, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    gp_write_u32(va, bits);
}

static uint32_t read_u32(uint32_t va) {
    uint32_t v = 0;
    gp_read_u32(va, &v);
    return v;
}

static int invert_mouse(void) {
    const uint8_t* p = (const uint8_t*)gm_ptr_checked(INVERT_MOUSE_VA, 1, 0);
    return p && *p;
}

static void play_before(void) {
    uint64_t now = jpt_counter();
    double dt = G.last_play ? seconds_between(G.last_play, now) : 0.0;
    pad_read_t kind;
    int dx, dy;
    lx_gamepad_poll();
    kind = pad_read_kind(!G.last_play, 0, 1, dt);
    G.last_play = now;
    pad_play_before(&G.play, kind, &G.state, buttons_now(), &G.cfg, (float)dt, invert_mouse(), &dx, &dy);
    G.frame_looked = dx || dy;
    if (G.frame_looked) lx_user32_cursor_nudge(dx, dy);
    G.frame_ready = 1;
}

static void play_after(uint32_t sinput) {
    pad_sinput_t in;
    if (!G.frame_ready) return;
    G.frame_ready = 0;
    in.move_x = read_float(sinput + SINPUT_MOVE_X);
    in.move_y = read_float(sinput + SINPUT_MOVE_Y);
    in.state = read_u32(sinput + SINPUT_STATE);
    in.hit = read_u32(sinput + SINPUT_HIT);
    if (pad_play_after(&G.play, &G.cfg, &in)) {
        write_float(sinput + SINPUT_MOVE_X, in.move_x);
        write_float(sinput + SINPUT_MOVE_Y, in.move_y);
        gp_write_u32(sinput + SINPUT_STATE, in.state);
        gp_write_u32(sinput + SINPUT_HIT, in.hit);
        G.stats.play_frames++;
    } else if (G.frame_looked) {
        G.stats.play_frames++;
    }
}

void lx_gamepad_read_around(uint32_t self, void (*original)(void)) {
    if (!G.ready) {
        original();
        return;
    }
    play_before();
    original();
    play_after(self + SINPUT_IN_CINPUT);
}

void lx_gamepad_overlay_changed(void) {
    if (G.ready) gamepad_ui_overlay_changed(&G.ui, buttons_now());
}

void lx_gamepad_ui_loop(void) {
    gamepad_ui_in_t in;
    const uint64_t now = jpt_counter();
    const double dt = G.last_ui ? seconds_between(G.last_ui, now) : 0.0;
    if (!G.ready) return;
    G.last_ui = now;
    lx_gamepad_poll();
    G.chord_fire |= pad_chord_update(&G.chord, MM_PAD_CHORD, G.state.buttons, (uint32_t)jpt_time_ms(), &G.state.buttons);
    in.pad = &G.state;
    in.cfg = &G.cfg;
    in.buttons = buttons_now();
    in.now_ms = (uint32_t)jpt_time_ms();
    in.dt = (float)dt;
    in.device_gamepad = G.tracker.device == PAD_DEVICE_GAMEPAD;
    in.chord = G.chord_fire;
    G.chord_fire = 0;
    in.stick_deadzone = stick_deadzone();
    in.toggle_minimap = lx_features_toggle_minimap;
    gamepad_ui_step(&G.ui, &in);
}

int lx_gamepad_rumble(float low, float high, unsigned ms) {
    int active = G.roster.active;
    uint16_t motor_low, motor_high;
    if (!G.ready || active < 0 || !G.pads[active]) return 0;
    pad_rumble_motors(low, high, &motor_low, &motor_high);
    G.stats.rumble_requests++;
    return s_backend->rumble(G.pads[active], motor_low, motor_high, ms);
}

void lx_gamepad_stats(lx_gamepad_stats_t* out) { *out = G.stats; }

void lx_gamepad_log_summary(void) {
    lx_log("Linux gamepad", "summary: %u connected, %u disconnected, %u device changes, %u gameplay frames with gamepad input", G.stats.connections,
           G.stats.disconnections, G.stats.switches, G.stats.play_frames);
}
