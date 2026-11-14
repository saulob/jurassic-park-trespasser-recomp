/*
 * F5 Debug / Test Menu: the game's own debug paths behind one host menu. TNEXT (PlayerTeleportToNextLocation) and
 * its CTeleport list, DINOS (CAISystem::bBoring), the death and options Restart, CWorld::DeferredLoad (the level
 * change of the game's triggers) and the Direct Load sequence of the main menu. Retail compiled TELE out; Teleport is
 * its body built from TNEXT's own calls. Everything runs in the game's modal UI loop, before it reads its cursor.
 */
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "cheats.h"
#include "debug_menu_panel.h"
#include "debug_menu_policy.h"

#define WORLD 0x0073F444u
#define WORLD_CAMERA 0x14u
#define CAMERA_ATTACHED 0x50u
#define PLAYER 0x0066EF64u
#define PLAYER_DEAD_SLOT 0x1FCu
#define PLACEMENT_SLOT 0x18u
#define MOVE_SLOT 0x38u
#define PLACEMENT_BYTES 28u
#define PURGE_RENDER_CACHES 0x00548150u
#define SET_NO_PAGE_FRAMES 0x006264D0u
#define TELEPORT_NEXT 0x0060CB70u
#define TELEPORT_LIST_HEAD 0x0073F600u
#define TELEPORT_ITERATOR 0x0073F614u
#define AI_SYSTEM 0x0066E77Cu
#define AI_FLAGS 0x50u /* bActive, bSleep, bBoring: DINOS toggles the third byte */
#define AI_BORING_SHIFT 16
#define APP 0x0066E338u
#define APP_SCENE 0x0066E342u
#define APP_RESTART 0x00414480u
#define APP_MENU_SOUNDS_OFF 0x00414D80u
#define APP_LOAD_LEVEL 0x00414100u
#define APP_LOAD_FAILED 0x00413160u
#define GAME_WND_PAUSE 0x0040BCC0u
#define GAME_WND_RESUME 0x0040BD70u
#define WORLD_DEFERRED_LOAD 0x00607E90u
#define MAIN_MENU_PLAY 2 /* the main menu's EndModal code after New Game and Direct Load */
#define SCRATCH_BYTES 0x100u

static const char k_help[] = "Up/Down select | Enter/A use | Left/Right adjust (Shift 10, Ctrl 0.1, LB/RB 10) | "
                             "digits edit | Esc/B back | F5 close";
static const char k_cheats_help[] = "Up/Down select | Enter/A use | Left/Right change | Esc/B back | F10 close. "
                                    "Free Camera: WASD/left stick move, mouse/right stick look, Space/A up, "
                                    "Ctrl/B down, Shift/LB fast";

static struct {
    dm_state_t state;
    dm_world_t world;
    dm_row_t rows[DM_ROWS_MAX];
    int was_open, had_checkpoint;
    dm_menu_t was_menu;
} D = { .world = { .ai = -1, .key_index = -1 } };

static uint32_t mover(void) {
    uint32_t world, camera, attached;
    if (!guest_read32(WORLD, &world) || !world || !guest_read32(world + WORLD_CAMERA, &camera) || !camera ||
        !guest_read32(camera + CAMERA_ATTACHED, &attached))
        return 0;
    if (attached) return attached;
    attached = cheats_detached_from(camera);
    return attached ? attached : camera;
}

static int read_placement(uint32_t instance, uint32_t buffer, dm_placement_t* out) {
    uint32_t at = 0;
    if (!guest_call_virtual(instance, PLACEMENT_SLOT, &buffer, 1, &at) || !guest_readable(at, PLACEMENT_BYTES))
        return 0;
    memcpy(out, (const void*)ADDR(at), PLACEMENT_BYTES);
    return 1;
}

static int player_alive(void) {
    uint32_t player, dead = 1;
    return guest_read32(PLAYER, &player) && player && guest_call_virtual(player, PLAYER_DEAD_SLOT, NULL, 0, &dead) &&
           !(dead & 0xFFu);
}

static dm_place_t place_of(const guest_ui_view_t* v) {
    if (v->depth != 1) return DM_PLACE_NONE;
    if (v->kind == GUEST_WND_GAME && v->ui == GUEST_UI_GAMEPLAY) return DM_PLACE_LEVEL;
    if (v->kind == GUEST_WND_MAIN_MENU) return DM_PLACE_MAIN_MENU;
    return DM_PLACE_NONE;
}

static int read_ai(uint32_t* system, uint32_t* flags) {
    return guest_read32(AI_SYSTEM, system) && *system && guest_read32(*system + AI_FLAGS, flags);
}

static int read_keys(dm_keys_t* keys) {
    uint32_t head, iterator;
    return guest_read32(TELEPORT_LIST_HEAD, &head) && guest_read32(TELEPORT_ITERATOR, &iterator) &&
           dm_keys_walk(guest_read32, head, iterator, keys);
}

static void sample(const guest_ui_view_t* v, int deep, uint32_t scratch) {
    dm_world_t* w = &D.world;
    uint32_t system, flags, ins;
    dm_keys_t keys;
    memset(w, 0, sizeof *w);
    w->ai = -1;
    w->key_index = -1;
    w->place = place_of(v);
    cheats_world(w);
    if (w->place != DM_PLACE_LEVEL) return;
    if (guest_read_string(APP_SCENE, w->scene, sizeof w->scene)) dm_level_id(w->scene, w->level, sizeof w->level);
    if (read_ai(&system, &flags)) w->ai = (int)((flags >> AI_BORING_SHIFT) & 0xFFu) != 0;
    if (!deep) return;
    ins = mover();
    w->player = ins && read_placement(ins, scratch, &w->at) && player_alive();
    if (read_keys(&keys)) {
        w->key_count = keys.count;
        w->key_index = keys.index;
    }
}

/* PlayerTeleportToXYZ as the source has it (compiled out of retail), with TNEXT's own calls: the render caches go,
 * the camera's mover moves, pages load synchronously for 2 frames. keep_rotation: TELE's rule. */
static int move_player(const dm_placement_t* to, int keep_rotation, uint32_t scratch) {
    uint32_t ins = mover(), args[2], frames = 2;
    dm_placement_t at;
    if (!ins || !read_placement(ins, scratch, &at)) return 0;
    if (keep_rotation) memcpy(at.pos, to->pos, sizeof at.pos);
    else at = *to;
    memcpy((void*)ADDR(scratch), &at, PLACEMENT_BYTES);
    if (!guest_call(PURGE_RENDER_CACHES, 0, NULL, 0, 0, NULL)) return 0;
    args[0] = scratch;
    args[1] = 0;
    if (!guest_call_virtual(ins, MOVE_SLOT, args, 2, NULL)) return 0;
    return guest_call(SET_NO_PAGE_FRAMES, 0, &frames, 1, 1, NULL);
}

static uint32_t guest_string(uint32_t scratch, const char* text) {
    size_t n = strlen(text) + 1;
    if (n > SCRATCH_BYTES) n = SCRATCH_BYTES;
    memcpy((void*)ADDR(scratch), text, n);
    *(char*)ADDR(scratch + (uint32_t)n - 1) = 0;
    return scratch;
}

static int listed_level(const char* scene) {
    for (int i = 0; i < D.state.levels.count; i++)
        if (!_stricmp(D.state.levels.scene[i], scene)) return 1;
    return 0;
}

static int load_level(const char* scene, const guest_ui_view_t* v, uint32_t scratch) {
    uint32_t world, name, result = 0;
    if (!dm_scene_valid(scene)) return 0;
    name = guest_string(scratch, scene);
    if (D.world.place == DM_PLACE_LEVEL)
        return guest_read32(WORLD, &world) && world && guest_call(WORLD_DEFERRED_LOAD, world, &name, 1, 0, NULL);
    if (D.world.place != DM_PLACE_MAIN_MENU || v->kind != GUEST_WND_MAIN_MENU) return 0;
    if (!guest_call(APP_MENU_SOUNDS_OFF, APP, NULL, 0, 0, NULL) ||
        !guest_call(APP_LOAD_LEVEL, APP, &name, 1, 0, &result))
        return 0;
    if ((int32_t)result < 0) {
        host_log("debug-menu", "the game could not load %s (%d)", scene, (int)(int32_t)result);
        guest_call(APP_LOAD_FAILED, 0, &result, 1, 1, NULL);
        return 0;
    }
    return guest_ui_end_window(v, MAIN_MENU_PLAY, 0) == 1;
}

static int restart_level(const guest_ui_view_t* v) {
    uint32_t resume = 1;
    if (v->kind != GUEST_WND_GAME || !guest_call(GAME_WND_PAUSE, v->top, NULL, 0, 0, NULL)) return 0;
    guest_call(APP_RESTART, APP, NULL, 0, 0, NULL);
    return guest_call(GAME_WND_RESUME, v->top, &resume, 1, 0, NULL);
}

static int previous_key(void) {
    dm_keys_t keys;
    if (!read_keys(&keys) || !keys.count || !guest_write32(TELEPORT_ITERATOR, keys.prev_target)) return 0;
    return guest_call(TELEPORT_NEXT, 0, NULL, 0, 0, NULL);
}

static int freeze_ai(int on) {
    uint32_t system, flags;
    if (!read_ai(&system, &flags)) return 0;
    flags = (flags & ~(0xFFu << AI_BORING_SHIFT)) | ((on ? 1u : 0u) << AI_BORING_SHIFT);
    return guest_write32(system + AI_FLAGS, flags);
}

static int copy_text(const char* text) {
    size_t n = strlen(text) + 1;
    HGLOBAL memory;
    int ok = 0;
    if (!OpenClipboard(window_host_game_window())) return 0;
    memory = GlobalAlloc(GMEM_MOVEABLE, n);
    if (memory) {
        memcpy(GlobalLock(memory), text, n);
        GlobalUnlock(memory);
        ok = EmptyClipboard() && SetClipboardData(CF_TEXT, memory);
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

static dm_item_t action_item(dm_action_kind_t kind) {
    switch (kind) {
    case DM_ACT_LOAD_LEVEL: return DM_LEVEL_SELECT;
    case DM_ACT_RESTART: return DM_RESTART;
    case DM_ACT_RELOAD: return DM_RELOAD;
    case DM_ACT_TELEPORT: return DM_TELEPORT;
    case DM_ACT_PREV_KEY: return DM_PREV_KEY;
    case DM_ACT_NEXT_KEY: return DM_NEXT_KEY;
    case DM_ACT_RETURN_CHECKPOINT: return DM_RETURN_CHECKPOINT;
    case DM_ACT_COPY: return DM_COPY_COORDS;
    case DM_ACT_SPEED: return DM_MOVE_SPEED;
    case DM_ACT_JUMP: return DM_JUMP_HEIGHT;
    case DM_ACT_INVULNERABLE: return DM_INVULNERABLE;
    case DM_ACT_AMMO: return DM_INFINITE_AMMO;
    case DM_ACT_HEAL: return DM_HEAL;
    case DM_ACT_SUPER_HEALTH: return DM_SUPER_HEALTH;
    case DM_ACT_CAMERA: return DM_FREE_CAMERA;
    default: return DM_FREEZE_AI;
    }
}

static int moves_the_game(dm_action_kind_t kind) {
    return kind == DM_ACT_LOAD_LEVEL || kind == DM_ACT_RELOAD || kind == DM_ACT_RESTART || kind == DM_ACT_TELEPORT ||
           kind == DM_ACT_RETURN_CHECKPOINT || kind == DM_ACT_NEXT_KEY || kind == DM_ACT_PREV_KEY;
}

static void say(const char* text, uint32_t now) {
    dm_feedback(&D.state, text, now);
    host_log("debug-menu", "%s", text);
}

static void run(const dm_action_t* a, const guest_ui_view_t* v, uint32_t scratch, uint32_t now) {
    char line[DM_TEXT_MAX * 2];
    int moved = 0;
    if (a->kind == DM_ACT_CAMERA ? !D.world.player || (a->value != CH_CAMERA_NORMAL && !D.world.camera_head)
                                 : !dm_item_enabled(&D.state, &D.world, action_item(a->kind))) {
        say("Not available here", now);
        return;
    }
    if (moves_the_game(a->kind)) cheats_suspend();
    switch (a->kind) {
    case DM_ACT_LOAD_LEVEL:
        if (!listed_level(a->scene) || !load_level(a->scene, v, scratch)) say("Level Select failed", now);
        else host_log("debug-menu", "Level Select: %s (%s)", a->scene,
                      D.world.place == DM_PLACE_LEVEL ? "CWorld::DeferredLoad" : "the main menu's Direct Load");
        break;
    case DM_ACT_RELOAD:
        if (!load_level(a->scene, v, scratch)) say("Reload failed", now);
        else host_log("debug-menu", "Reload Current Level: %s (CWorld::DeferredLoad)", a->scene);
        break;
    case DM_ACT_RESTART:
        if (!restart_level(v)) say("Restart failed", now);
        else host_log("debug-menu", "Restart Level: %s", D.world.scene);
        break;
    case DM_ACT_TELEPORT:
        moved = move_player(&a->to, 1, scratch);
        snprintf(line, sizeof line, moved ? "Teleported to %.3f %.3f %.3f" : "Teleport failed", a->to.pos[0],
                 a->to.pos[1], a->to.pos[2]);
        say(line, now);
        break;
    case DM_ACT_RETURN_CHECKPOINT:
        moved = move_player(&a->to, 0, scratch);
        say(moved ? "Returned to checkpoint" : "Return to Checkpoint failed", now);
        break;
    case DM_ACT_NEXT_KEY:
        moved = guest_call(TELEPORT_NEXT, 0, NULL, 0, 0, NULL);
        if (!moved) say("Next Key Point failed", now);
        break;
    case DM_ACT_PREV_KEY:
        moved = previous_key();
        if (!moved) say("Previous Key Point failed", now);
        break;
    case DM_ACT_COPY:
        dm_format_coords(line, sizeof line, D.world.level, &D.world.at);
        if (copy_text(line)) {
            dm_feedback(&D.state, "Coordinates copied", now);
            host_log("debug-menu", "copied: %s", line);
        } else {
            say("The clipboard could not be opened", now);
        }
        break;
    case DM_ACT_FREEZE_AI:
        say(freeze_ai(a->on) ? (a->on ? "AI frozen" : "AI running") : "Freeze AI failed", now);
        break;
    default: {
        const char* result = cheats_run(a, D.world.level, scratch);
        if (result) say(result, now);
        break;
    }
    }
    if (!moved) return;
    sample(v, 1, scratch);
    if (D.world.player) dm_fill_fields(&D.state, &D.world.at);
    if (a->kind == DM_ACT_NEXT_KEY || a->kind == DM_ACT_PREV_KEY) {
        snprintf(line, sizeof line, "Key point %d / %d", D.world.key_index + 1, D.world.key_count);
        say(line, now);
    }
}

static void note_changes(void) {
    if (D.state.open != D.was_open || (D.state.open && D.state.menu != D.was_menu))
        host_log("debug-menu", "%s %s", dm_menu_title(D.state.open ? D.state.menu : D.was_menu),
                 D.state.open ? "opened" : "closed");
    D.was_menu = D.state.menu;
    if (D.state.checkpoint.set && !D.had_checkpoint)
        host_log("debug-menu", "checkpoint set in %s at %.3f %.3f %.3f", D.state.checkpoint.level,
                 D.state.checkpoint.at.pos[0], D.state.checkpoint.at.pos[1], D.state.checkpoint.at.pos[2]);
    if (!D.state.checkpoint.set && D.had_checkpoint) host_log("debug-menu", "checkpoint cleared: its level was left");
    D.was_open = D.state.open;
    D.had_checkpoint = D.state.checkpoint.set;
}

void debug_menu_ui_loop(void) {
    guest_ui_view_t v;
    dm_action_t action;
    uint32_t esp = g_esp, scratch, now = GetTickCount();
    if (!D.state.open && !D.state.show_coords && !D.state.checkpoint.set && D.state.pending.kind == DM_ACT_NONE &&
        !cheats_busy())
        return;
    guest_ui_probe(&v);
    g_esp -= SCRATCH_BYTES;
    scratch = g_esp;
    sample(&v, D.state.open || D.state.show_coords, scratch);
    dm_world_seen(&D.state, &D.world);
    if (dm_take(&D.state, &action)) run(&action, &v, scratch, now);
    cheats_frame(&D.world, scratch);
    g_esp = esp;
    note_changes();
}

static void read_levels(void) {
    char dir[MAX_PATH], pattern[MAX_PATH];
    char* slash;
    WIN32_FIND_DATAA found;
    HANDLE h;
    dm_levels_t levels;
    memset(&levels, 0, sizeof levels);
    strncpy(dir, guest_env_exe_path(), sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    _snprintf(pattern, sizeof pattern - 1, "%s\\data\\*.scn", dir);
    pattern[sizeof pattern - 1] = 0;
    h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) dm_levels_add(&levels, found.cFileName);
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    dm_set_levels(&D.state, &levels);
}

static void toggle(dm_menu_t menu, const char* key) {
    guest_ui_view_t v;
    guest_ui_probe(&v);
    D.world.place = place_of(&v);
    if (D.world.place != DM_PLACE_LEVEL) D.world.player = 0;
    if (D.world.place != DM_PLACE_NONE) read_levels();
    if (!dm_toggle(&D.state, &D.world, menu) && !D.state.open)
        host_log("debug-menu", "%s: no level and no main menu in front (%s)", key, guest_ui_class_name(v.top_vtable));
    note_changes();
}

void debug_menu_f5(void) { toggle(DM_MENU_DEBUG, "F5"); }

void debug_menu_f10(void) { toggle(DM_MENU_CHEATS, "F10"); }

int debug_menu_visible(void) { return D.state.open; }

int debug_menu_blocks_gameplay(void) { return D.state.open && D.world.place == DM_PLACE_LEVEL; }

int debug_menu_key(UINT msg, WPARAM wparam, LPARAM lparam) {
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, r;
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return 0;
    r = dm_key(&D.state, &D.world, (unsigned)wparam, down, (lparam & 0x40000000) != 0, GetKeyState(VK_SHIFT) < 0,
               GetKeyState(VK_CONTROL) < 0, GetTickCount());
    note_changes();
    return r;
}

int debug_menu_char(WPARAM wparam) { return dm_char(&D.state, &D.world, (char)wparam); }

int debug_menu_mouse(void) { return D.state.open && D.world.place == DM_PLACE_MAIN_MENU; }

void debug_menu_pad(uint32_t held, uint32_t pressed) {
    uint32_t now = GetTickCount();
    pressed |= dm_pad_repeat(&D.state.repeat, held, pressed, now);
    dm_pad(&D.state, &D.world, pressed, now);
    note_changes();
}

int debug_menu_overlay_wanted(void) {
    return D.state.open || (D.state.show_coords && D.world.place == DM_PLACE_LEVEL);
}

static float dpi_scale(void) {
    static float scale;
    if (!scale) {
        HDC dc = GetDC(NULL);
        scale = dc ? (float)GetDeviceCaps(dc, LOGPIXELSY) / 96.0f : 1.0f;
        if (dc) ReleaseDC(NULL, dc);
        if (scale < 1.0f) scale = 1.0f;
    }
    return scale;
}

static void coordinate_lines(dmp_frame_t* f) {
    const dm_world_t* w = &D.world;
    if (!D.state.show_coords || w->place != DM_PLACE_LEVEL) return;
    snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "Level %s", w->level[0] ? w->level : "?");
    if (!w->player) {
        snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "Coordinates unavailable");
        return;
    }
    snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "X %.3f", w->at.pos[0]);
    snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "Y %.3f", w->at.pos[1]);
    snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "Z %.3f", w->at.pos[2]);
    snprintf(f->coords[f->coord_lines++], DM_TEXT_MAX, "Yaw %.3f", dm_yaw(w->at.rot));
}

void debug_menu_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context,
                        struct ID3D11RenderTargetView* target, int width, int height) {
    dmp_frame_t f;
    RECT view;
    if (!debug_menu_overlay_wanted() || !window_host_view_rect(&view)) return;
    memset(&f, 0, sizeof f);
    f.view_w = (float)(view.right - view.left);
    f.view_h = (float)(view.bottom - view.top);
    f.dpi_scale = dpi_scale();
    f.menu = D.state.open;
    if (f.menu) {
        f.rows = D.rows;
        f.row_count = dm_rows(&D.state, &D.world, D.rows, DM_ROWS_MAX);
        f.feedback = dm_feedback_text(&D.state, GetTickCount());
        f.help = D.state.menu == DM_MENU_CHEATS ? k_cheats_help : k_help;
        f.title = dm_menu_title(D.state.menu);
    }
    coordinate_lines(&f);
    dmp_draw(device, context, target, width, height, &f);
}

void debug_menu_release(void) { dmp_release(); }
