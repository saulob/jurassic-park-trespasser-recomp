/*
 * The host's menus over the game and its HUD: F10 Cheats and F3 Objectives, the compass, the objective markers and the
 * minimap. CWorld::DeferredLoad (the level change of the game's triggers) and the Direct Load sequence of the main menu
 * back Level Select. Retail compiled TELE out; Teleport is PlayerTeleportToNextLocation's body built from its own
 * calls. Everything runs in the game's modal UI loop, before it reads its cursor.
 */
#include "debug_menu.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "perf_sections.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cheats.h"
#include "debug_menu_panel.h"
#include "debug_menu_policy.h"
#include "jpt_config.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_gamepad.h"
#include "jpt_sync.h"
#include "jpt_tr.h"
#include "objectives.h"
#include "tutorial_prompts.h"

#define WORLD GVA(0x0073F444)
#define WORLD_CAMERA 0x14u
#define WORLD_LOAD_PENDING 0x38u
#define CAMERA_PRESENCE 0x30u
#define CAMERA_ATTACHED 0x50u
#define PLAYER GVA(0x0066EF64)
#define PLAYER_DEAD_SLOT 0x1FCu
#define PLACEMENT_SLOT 0x18u
#define MOVE_SLOT 0x38u
#define PLACEMENT_BYTES 28u
#define PURGE_RENDER_CACHES GVA(0x00548150)
#define SET_NO_PAGE_FRAMES GVA(0x006264D0)
#define APP GVA(0x0066E338)
#define APP_SCENE GVA(0x0066E342)
#define APP_MENU_SOUNDS_OFF GVA(0x00414D80)
#define APP_LOAD_LEVEL GVA(0x00414100)
#define APP_LOAD_FAILED GVA(0x00413160)
#define WORLD_DEFERRED_LOAD GVA(0x00607E90)
#define MAIN_MENU_PLAY 2 /* the main menu's EndModal code after New Game and Direct Load */
#define SCRATCH_BYTES 0x100u
#define LEVEL_PATH_MAX 512

static const char k_objectives_help[] = "Esc/B or F3 close";

static struct {
    dm_state_t state;
    dm_world_t world;
    dm_row_t rows[DM_ROWS_MAX];
    dm_start_t start;
    int was_open;
    dm_menu_t was_menu;
    cp_hud_t hud;
    cp_reminder_t reminder;
    int pointer;
    float pointer_x, pointer_y;
    void (*changed)(void);
    const dml_hits_t* (*hits)(void);
} D = { .world = { .ai = -1 } };

void debug_menu_set_hooks(void (*input_changed)(void), const dml_hits_t* (*hits)(void)) {
    D.changed = input_changed;
    D.hits = hits;
}

static uint32_t mover(void) {
    uint32_t world, camera, attached;
    if (!guest_read32(WORLD, &world) || !world || !guest_read32(world + WORLD_CAMERA, &camera) || !camera ||
        !guest_read32(camera + CAMERA_ATTACHED, &attached))
        return 0;
    if (attached) return attached;
    attached = cheats_detached_from(camera);
    return attached ? attached : camera;
}

int debug_menu_placement(uint32_t instance, uint32_t buffer, dm_placement_t* out) {
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

static int read_eye(dm_placement_t* out) {
    uint32_t world, camera;
    if (!guest_read32(WORLD, &world) || !world || !guest_read32(world + WORLD_CAMERA, &camera) || !camera ||
        !guest_readable(camera + CAMERA_PRESENCE, PLACEMENT_BYTES))
        return 0;
    memcpy(out, (const void*)ADDR(camera + CAMERA_PRESENCE), PLACEMENT_BYTES);
    for (int i = 0; i < 3; i++)
        if (!isfinite(out->pos[i])) return 0;
    for (int i = 0; i < 4; i++)
        if (!isfinite(out->rot[i])) return 0;
    return 1;
}

static int load_pending(void) {
    uint32_t world, v;
    return guest_read32(WORLD, &world) && world && guest_read32(world + WORLD_LOAD_PENDING, &v) && (v & 0xFFu);
}

static void sample(const guest_ui_view_t* v, uint32_t scratch) {
    dm_world_t* w = &D.world;
    uint32_t system, flags, ins;
    memset(w, 0, sizeof *w);
    w->ai = -1;
    w->place = place_of(v);
    cheats_world(w);
    if (w->place != DM_PLACE_LEVEL) return;
    if (guest_read_string(APP_SCENE, w->scene, sizeof w->scene)) dm_level_id(w->scene, w->level, sizeof w->level);
    if (cheats_read_ai(&system, &flags)) w->ai = (int)((flags >> CHEATS_AI_BORING_SHIFT) & 0xFFu) != 0;
    ins = mover();
    w->player = ins && debug_menu_placement(ins, scratch, &w->at) && player_alive();
    w->eye = read_eye(&w->eye_at);
    w->tan_half_h = widescreen_tan_half_height();
}

/* PlayerTeleportToXYZ as the source has it (compiled out of retail), with TNEXT's own calls: the render caches go,
 * the camera's mover moves, pages load synchronously for 2 frames. The rotation is kept: TELE's rule. */
static int move_player(const dm_placement_t* to, uint32_t scratch) {
    uint32_t ins = mover(), args[2], frames = 2;
    dm_placement_t at;
    if (!ins || !debug_menu_placement(ins, scratch, &at)) return 0;
    memcpy(at.pos, to->pos, sizeof at.pos);
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

void debug_menu_start_custom_level(void) {
    if (!jpt_custom_level_active()) return;
    if (!dm_start_request(&D.start, jpt_custom_level_scene()))
        host_log("custom-level", "the game cannot load a scene named %s", jpt_custom_level_scene());
}

static void start_level(const guest_ui_view_t* v, uint32_t scratch, uint32_t now) {
    char scene[DM_SCENE_MAX];
    if (!dm_start_due(&D.start, D.world.place, now, scene)) return;
    cheats_suspend();
    host_log("custom-level", "loading %s of %s", scene, jpt_custom_level_package());
    if (!load_level(scene, v, scratch)) host_log("custom-level", "%s could not be loaded", scene);
}

static dm_item_t action_item(dm_action_kind_t kind) {
    switch (kind) {
    case DM_ACT_LOAD_LEVEL: return DM_LEVEL_SELECT;
    case DM_ACT_TELEPORT: return DM_TELEPORT;
    case DM_ACT_SPEED: return DM_MOVE_SPEED;
    case DM_ACT_JUMP: return DM_JUMP_HEIGHT;
    case DM_ACT_INVULNERABLE: return DM_INVULNERABLE;
    case DM_ACT_AMMO: return DM_INFINITE_AMMO;
    case DM_ACT_HEAL: return DM_HEAL;
    case DM_ACT_SUPER_HEALTH: return DM_SUPER_HEALTH;
    case DM_ACT_CAMERA: return DM_FREE_CAMERA;
    case DM_ACT_NOCLIP: return DM_NOCLIP;
    case DM_ACT_STEALTH: return DM_STEALTH;
    default: return DM_FREEZE_AI;
    }
}

static int moves_the_game(dm_action_kind_t kind) { return kind == DM_ACT_LOAD_LEVEL || kind == DM_ACT_TELEPORT; }

static void say(const char* text, uint32_t now) {
    dm_feedback(&D.state, jpt_tr(text), now);
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
    case DM_ACT_TELEPORT:
        moved = move_player(&a->to, scratch);
        if (moved) host_log("debug-menu", "teleported to %.3f %.3f %.3f", a->to.pos[0], a->to.pos[1], a->to.pos[2]);
        else say("Teleport failed", now);
        break;
    case DM_ACT_FREEZE_AI:
        if (cheats_freeze_ai(a->on)) {
            if (ch_format_change(line, sizeof line, a->kind, a->on, 0)) host_log("cheats", "%s", line);
        } else {
            say("Freeze AI failed", now);
        }
        break;
    default: {
        const char* result = cheats_run(a, D.world.level, scratch);
        if (result) dm_feedback(&D.state, result, now);
        break;
    }
    }
    if (!moved) return;
    sample(v, scratch);
    if (D.world.player) dm_fill_fields(&D.state, &D.world.at);
}

static void scene_of(cp_scene_t* scene) {
    memset(scene, 0, sizeof *scene);
    scene->level = D.world.place == DM_PLACE_LEVEL;
    scene->menu = D.state.open;
    scene->player = D.world.player;
    if (!scene->level) return;
    scene->loading = load_pending();
    tutorial_prompts_overlay_state(&scene->card, &scene->game_text);
}

static char minimap_seen[DM_LEVEL_ID_MAX];
static void minimap_level_seen(const char* level) {
    if (!level || !level[0] || !strcmp(minimap_seen, level)) return;
    snprintf(minimap_seen, sizeof minimap_seen, "%s", level);
    if (!minimap_bounds(level)) host_log("minimap", "no map data for level %s: the minimap is hidden", level);
}

static void minimap_creatures(uint32_t scratch) {
    host_animal_t animals[MM_CREATURES_MAX * 4];
    minimap_creature_t all[MM_CREATURES_MAX * 4];
    int n = active_ai_animals(scratch, animals, (int)(sizeof animals / sizeof animals[0])), count = 0;
    for (int i = 0; i < n; i++) {
        all[count].x = animals[i].x;
        all[count].y = animals[i].y;
        all[count].threat = minimap_creature_kind(0, animals[i].engaged) == MM_CREATURE_THREAT;
        all[count].size = minimap_creature_size_class(animals[i].scale);
        count++;
    }
    D.hud.creature_count = minimap_creatures_select(D.hud.player_pos[0], D.hud.player_pos[1],
                                                    minimap_creature_range(MM_ZOOM), all, count,
                                                    D.hud.creatures, MM_CREATURES_MAX);
}

static void navigate(uint32_t scratch, uint32_t now) {
    cp_scene_t scene;
    const char* notice;
    scene_of(&scene);
    int64_t section = perf_section_begin();
    notice = objectives_frame(&D.world, D.state.open && D.state.menu == DM_MENU_OBJECTIVES, scratch);
    perf_section_end(PS_OBJECTIVES, section);
    if (notice) dm_notice(&D.state, notice, now);
    minimap_level_seen(D.world.level);
    dm_navigate(&D.state, &D.world, &scene, objectives_goal(), now, &D.hud);
    section = perf_section_begin();
    if (D.hud.minimap) minimap_creatures(scratch);
    perf_section_end(PS_MINIMAP_STATE, section);
    cp_hud_reminder(&D.hud, objectives_goal(), cp_reminder_step(&D.reminder, now, objectives_goal()));
}

static void note_changes(void) {
    if (D.state.open != D.was_open || (D.state.open && D.state.menu != D.was_menu))
        host_log("debug-menu", "%s %s", dm_menu_title(D.state.open ? D.state.menu : D.was_menu),
                 D.state.open ? "opened" : "closed");
    if (!D.state.open) D.pointer = 0;
    if (D.state.open != D.was_open && D.changed) D.changed();
    D.was_menu = D.state.menu;
    D.was_open = D.state.open;
}

int debug_menu_positions(float player[3], float camera[3]) {
    int found = 0;
    if (D.world.player) {
        memcpy(player, D.world.at.pos, 3 * sizeof(float));
        found |= 1;
    }
    if (D.world.eye) {
        memcpy(camera, D.world.eye_at.pos, 3 * sizeof(float));
        found |= 2;
    }
    return found;
}

void debug_menu_ui_loop(void) {
    guest_ui_view_t v;
    dm_action_t action;
    uint32_t esp = g_esp, scratch, now = jpt_time_ms();
    guest_ui_probe(&v);
    g_esp -= SCRATCH_BYTES;
    scratch = g_esp;
    sample(&v, scratch);
    dm_world_seen(&D.state, &D.world);
    start_level(&v, scratch, now);
    if (dm_take(&D.state, &action)) run(&action, &v, scratch, now);
    {
        int64_t section = perf_section_begin();
        cheats_frame(&D.world, scratch);
        perf_section_end(PS_CHEATS, section);
    }
    navigate(scratch, now);
    g_esp = esp;
    note_changes();
}

static void add_level(const char* name, int is_dir, void* user) {
    if (!is_dir && strlen(name) > 4 && !_stricmp(name + strlen(name) - 4, ".scn")) dm_levels_add((dm_levels_t*)user, name);
}

static void read_levels(void) {
    char dir[LEVEL_PATH_MAX];
    dm_levels_t levels;
    memset(&levels, 0, sizeof levels);
    for (int i = 0; i < jpt_custom_level_scene_count(); i++) dm_levels_add(&levels, jpt_custom_level_scene_at(i));
    if (guest_data_dir(dir, sizeof dir)) jpt_fs_list(dir, add_level, &levels);
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

void debug_menu_f10(void) { toggle(DM_MENU_CHEATS, "F10"); }

void debug_menu_f3(void) { toggle(DM_MENU_OBJECTIVES, "F3"); }

void debug_menu_show_objective(void) {
    uint32_t now = jpt_time_ms();
    if (cp_reminder_press(&D.reminder, now, objectives_goal())) {
        host_log("objectives", "Show Objective: %s", D.reminder.title);
        return;
    }
    dm_notice_for(&D.state, jpt_tr("No objective"), now, CP_NO_OBJECTIVE_MS);
}

void debug_menu_notice(const char* text) { dm_notice(&D.state, text, jpt_time_ms()); }

int debug_menu_open_menu(void) {
    if (!D.state.open) return 0;
    return D.state.menu == DM_MENU_OBJECTIVES ? 2 : 1;
}

void debug_menu_close(void) {
    dm_close(&D.state);
    note_changes();
}

int debug_menu_visible(void) { return D.state.open; }

int debug_menu_blocks_gameplay(void) { return D.state.open && D.world.place == DM_PLACE_LEVEL; }

static const char* pad_name(uint32_t pressed) {
    static const struct {
        int control;
        const char* name;
    } names[] = { { JPT_PAD_SOUTH, "A" }, { JPT_PAD_EAST, "B" }, { JPT_PAD_START, "Menu" },
                  { JPT_PAD_OBJECTIVES_CONTROL, "D-pad Right" } };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (pressed & JPT_PAD_BIT(names[i].control)) return names[i].name;
    return "the gamepad";
}

static void note_closed_by(int was_open, dm_menu_t menu, const char* by) {
    if (was_open && !D.state.open)
        host_log("overlay", "%s closed by %s, held back from the game until released", dm_menu_title(menu), by);
}

int debug_menu_key_event(unsigned vk, int down, int repeat, int shift, int control) {
    int r, was_open = D.state.open;
    dm_menu_t menu = D.state.menu;
    r = dm_key(&D.state, &D.world, vk, down, repeat, shift, control, jpt_time_ms());
    note_closed_by(was_open, menu, vk == 0x1B ? "Esc" : vk == 0x0D ? "Enter" : "a key");
    note_changes();
    return r;
}

int debug_menu_char(char c) { return dm_char(&D.state, &D.world, c); }

int debug_menu_pointer(int click, int x, int y) {
    const int was_open = D.state.open;
    const dm_menu_t menu = D.state.menu;
    const dml_hits_t* hits = D.hits ? D.hits() : NULL;
    if (!D.state.open) return 0;
    D.pointer = 1;
    D.pointer_x = (float)x;
    D.pointer_y = (float)y;
    if (!click) {
        dm_mouse_move(&D.state, &D.world, hits, D.pointer_x, D.pointer_y);
    } else {
        dm_mouse_click(&D.state, &D.world, hits, D.pointer_x, D.pointer_y, jpt_time_ms());
        note_closed_by(was_open, menu, "the mouse");
        note_changes();
    }
    return 1;
}

void debug_menu_pad(uint32_t held, uint32_t pressed) {
    uint32_t now = jpt_time_ms();
    int was_open = D.state.open;
    dm_menu_t menu = D.state.menu;
    pressed |= dm_pad_repeat(&D.state.repeat, held, pressed, now);
    dm_pad(&D.state, &D.world, pressed, now);
    note_closed_by(was_open, menu, pad_name(pressed));
    note_changes();
}

static int hud_wanted(void) {
    return D.hud.compass || D.hud.marker || D.hud.objective[0] || D.hud.notice[0] || D.hud.minimap;
}

int debug_menu_overlay_wanted(int frame_counter_on) { return D.state.open || hud_wanted() || frame_counter_on; }

/* The UI loop that refreshes the HUD does not run under videos or loads: the picture is checked as it is shown. */
int debug_menu_hud_on_screen(void) {
    guest_ui_view_t v;
    return hud_wanted() && guest_ui_probe(&v) && place_of(&v) == DM_PLACE_LEVEL;
}

void debug_menu_fill_frame(dmp_frame_t* f) {
    f->menu = D.state.open;
    f->objectives = D.state.open && D.state.menu == DM_MENU_OBJECTIVES;
    if (debug_menu_hud_on_screen()) f->hud = &D.hud;
    if (f->menu) {
        f->rows = D.rows;
        f->row_count = dm_rows(&D.state, &D.world, D.rows, DM_ROWS_MAX);
        f->feedback = dm_feedback_text(&D.state, jpt_time_ms());
        f->menu_id = D.state.menu;
        f->page = D.state.page;
        f->pointer = D.pointer;
        f->pointer_x = D.pointer_x;
        f->pointer_y = D.pointer_y;
        f->context_help = dm_help_text(&D.state, &D.world);
        f->help_lines = DMP_HELP_LINES;
        if (D.state.menu == DM_MENU_OBJECTIVES) f->help = jpt_tr(k_objectives_help);
        f->title = dm_menu_title(D.state.menu);
    }
}
