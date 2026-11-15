#include "debug_menu_policy.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats_policy.h"
#include "jpt_gamepad.h"
#include "jpt_tr.h"

#define VK_BACK_ 0x08u
#define VK_RETURN_ 0x0Du
#define VK_ESCAPE_ 0x1Bu
#define VK_LEFT_ 0x25u
#define VK_UP_ 0x26u
#define VK_RIGHT_ 0x27u
#define VK_DOWN_ 0x28u
#define REPEAT_DELAY_MS 400u
#define REPEAT_EVERY_MS 80u
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char* const k_labels[DM_ITEM_COUNT] = {
    "Level Select", "Restart Level", "Reload Current Level", "X", "Y", "Z", "Teleport", "Previous Key Point",
    "Next Key Point", "Set Checkpoint", "Return to Checkpoint", "Show Coordinates", "Copy Coordinates", "Freeze AI",
    "Movement Speed", "Jump Height", "Invulnerability", "Infinite Ammo", "Heal", "Super Health", "Teleport",
    "Third Person", "Fly / Free Camera", "Compass", "Ambient Occlusion", "Water Refraction", "Enhanced Sky", "Close",
};

static const char* const k_ao_levels[DM_AO_LEVELS] = { "Off", "Low", "Medium", "High", "Ultra" };

static const dm_item_t k_debug_main[] = {
    DM_LEVEL_SELECT, DM_RESTART,  DM_RELOAD,   DM_FIELD_X,        DM_FIELD_Y,           DM_FIELD_Z, DM_TELEPORT,
    DM_PREV_KEY,     DM_NEXT_KEY, DM_SET_CHECKPOINT, DM_RETURN_CHECKPOINT, DM_COMPASS, DM_SHOW_COORDS, DM_COPY_COORDS,
    DM_FREEZE_AI,    DM_AMBIENT_OCCLUSION, DM_WATER_REFRACTION, DM_ENHANCED_SKY, DM_CLOSE,
};
static const dm_item_t k_cheats_main[] = {
    DM_MOVE_SPEED,    DM_JUMP_HEIGHT,  DM_INVULNERABLE, DM_INFINITE_AMMO, DM_FREEZE_AI,   DM_HEAL,  DM_SUPER_HEALTH,
    DM_TELEPORT_PAGE, DM_LEVEL_SELECT, DM_SHOW_COORDS,  DM_THIRD_PERSON,  DM_FREE_CAMERA, DM_CLOSE,
};
static const dm_item_t k_teleport_page[] = { DM_FIELD_X, DM_FIELD_Y, DM_FIELD_Z, DM_TELEPORT };
static const dm_item_t k_objectives_page[] = { DM_CLOSE };

void dm_init(dm_state_t* s) {
    memset(s, 0, sizeof *s);
    s->cursor = DM_CLOSE;
}

const char* dm_item_label(dm_menu_t menu, dm_item_t item) {
    if (menu == DM_MENU_CHEATS && item == DM_FREEZE_AI) return jpt_tr("Freeze Dinosaurs");
    return item >= 0 && item < DM_ITEM_COUNT ? jpt_tr(k_labels[item]) : "?";
}

const char* dm_menu_title(dm_menu_t menu) {
    return jpt_tr(menu == DM_MENU_CHEATS ? "Cheats" : menu == DM_MENU_OBJECTIVES ? "Objectives" : "Debug / Test Menu");
}

static int is_field(int item) { return item == DM_FIELD_X || item == DM_FIELD_Y || item == DM_FIELD_Z; }

static int is_multiplier(int item) { return item == DM_MOVE_SPEED || item == DM_JUMP_HEIGHT; }

static int fields_shown(const dm_state_t* s) {
    return s->page == DM_PAGE_TELEPORT || (s->page == DM_PAGE_MAIN && s->menu == DM_MENU_DEBUG);
}

static int same_level(const char* a, const char* b) { return a[0] && b[0] && !strcmp(a, b); }

static int page_items(const dm_state_t* s, const dm_item_t** items) {
    if (s->page == DM_PAGE_TELEPORT) {
        *items = k_teleport_page;
        return COUNT(k_teleport_page);
    }
    if (s->menu == DM_MENU_CHEATS) {
        *items = k_cheats_main;
        return COUNT(k_cheats_main);
    }
    if (s->menu == DM_MENU_OBJECTIVES) {
        *items = k_objectives_page;
        return COUNT(k_objectives_page);
    }
    *items = k_debug_main;
    return COUNT(k_debug_main);
}

int dm_item_enabled(const dm_state_t* s, const dm_world_t* w, dm_item_t item) {
    int level = w->place == DM_PLACE_LEVEL, player = level && w->player;
    switch (item) {
    case DM_LEVEL_SELECT: return w->place != DM_PLACE_NONE;
    case DM_RESTART: return level;
    case DM_RELOAD: return level && dm_scene_valid(w->scene);
    case DM_FIELD_X:
    case DM_FIELD_Y:
    case DM_FIELD_Z:
    case DM_TELEPORT_PAGE: return player;
    case DM_TELEPORT: return player && dm_teleport_valid(s->field);
    case DM_PREV_KEY:
    case DM_NEXT_KEY: return player && w->key_count > 0;
    case DM_SET_CHECKPOINT:
    case DM_COPY_COORDS:
    case DM_MOVE_SPEED:
    case DM_JUMP_HEIGHT: return player;
    case DM_RETURN_CHECKPOINT: return player && s->checkpoint.set && same_level(s->checkpoint.level, w->level);
    case DM_FREEZE_AI: return level && w->ai >= 0;
    case DM_INVULNERABLE: return player && w->invulnerable >= 0;
    case DM_INFINITE_AMMO: return level && w->ammo >= 0;
    case DM_HEAL: return player && w->hp > 0.0f && w->hp_max > 0.0f;
    case DM_SUPER_HEALTH: return player && (w->super_health || (w->hp > 0.0f && w->hp_max > 0.0f));
    case DM_THIRD_PERSON:
    case DM_FREE_CAMERA: return player && w->camera_head;
    case DM_COMPASS: return level;
    case DM_AMBIENT_OCCLUSION: return w->ambient_occlusion >= 0 && w->ambient_occlusion < DM_AO_LEVELS;
    case DM_WATER_REFRACTION: return w->water_refraction >= 0;
    case DM_ENHANCED_SKY: return w->enhanced_sky >= 0;
    case DM_SHOW_COORDS:
    case DM_CLOSE: return 1;
    default: return 0;
    }
}

static void settle_cursor(dm_state_t* s, const dm_world_t* w) {
    const dm_item_t* items;
    int n = page_items(s, &items), here = 0;
    for (int i = 0; i < n; i++) here |= items[i] == (dm_item_t)s->cursor;
    if (here && dm_item_enabled(s, w, (dm_item_t)s->cursor)) return;
    for (int i = 0; i < n; i++)
        if (dm_item_enabled(s, w, items[i])) {
            s->cursor = items[i];
            return;
        }
    s->cursor = DM_CLOSE;
}

void dm_close(dm_state_t* s) {
    s->open = 0;
    s->page = DM_PAGE_MAIN;
    s->editing = 0;
    s->repeat.bit = 0;
}

int dm_toggle(dm_state_t* s, const dm_world_t* w, dm_menu_t menu) {
    if (s->open && s->menu == menu) {
        dm_close(s);
        return 1;
    }
    if (w->place == DM_PLACE_NONE || (menu == DM_MENU_OBJECTIVES && w->place != DM_PLACE_LEVEL)) {
        if (!s->open) return 0;
        dm_close(s);
        return 1;
    }
    s->open = 1;
    s->menu = menu;
    s->page = DM_PAGE_MAIN;
    s->editing = 0;
    s->repeat.bit = 0;
    s->fill_fields = 1;
    if (menu == DM_MENU_OBJECTIVES) s->cursor = DM_CLOSE;
    else if (menu == DM_MENU_CHEATS) s->cursor = w->place == DM_PLACE_LEVEL ? DM_MOVE_SPEED : DM_LEVEL_SELECT;
    else s->cursor = w->place == DM_PLACE_LEVEL ? DM_TELEPORT : DM_LEVEL_SELECT;
    if (w->player) dm_fill_fields(s, &w->at);
    settle_cursor(s, w);
    return 1;
}

int dm_f5(dm_state_t* s, const dm_world_t* w) { return dm_toggle(s, w, DM_MENU_DEBUG); }

void dm_fill_fields(dm_state_t* s, const dm_placement_t* at) {
    for (int i = 0; i < 3; i++) s->field[i] = at->pos[i];
    s->fill_fields = 0;
    s->editing = 0;
}

void dm_feedback(dm_state_t* s, const char* text, uint32_t now_ms) {
    snprintf(s->feedback, sizeof s->feedback, "%s", text);
    s->feedback_until = now_ms + DM_FEEDBACK_MS;
}

const char* dm_feedback_text(const dm_state_t* s, uint32_t now_ms) {
    return s->feedback[0] && (int32_t)(s->feedback_until - now_ms) > 0 ? s->feedback : NULL;
}

static void request(dm_state_t* s, dm_action_kind_t kind) {
    memset(&s->pending, 0, sizeof s->pending);
    s->pending.kind = kind;
}

static void request_value(dm_state_t* s, dm_action_kind_t kind, int value) {
    request(s, kind);
    s->pending.value = value;
}

static void commit_edit(dm_state_t* s) {
    char* end = NULL;
    double v;
    if (!s->editing) return;
    s->editing = 0;
    if (!is_field(s->cursor) || !s->edit[0]) return;
    v = strtod(s->edit, &end);
    if (end && !*end && isfinite(v) && fabs(v) <= DM_COORD_LIMIT) s->field[s->cursor - DM_FIELD_X] = (float)v;
}

static void move_cursor(dm_state_t* s, const dm_world_t* w, int step) {
    const dm_item_t* items;
    int n, at = -1;
    if (s->page == DM_PAGE_LEVELS) {
        n = s->levels.count;
        if (n > 0) s->level_cursor = (s->level_cursor + step + n) % n;
        return;
    }
    commit_edit(s);
    n = page_items(s, &items);
    for (int i = 0; i < n; i++)
        if (items[i] == (dm_item_t)s->cursor) at = i;
    if (at < 0) at = step > 0 ? -1 : 0;
    for (int i = 1; i <= n; i++) {
        dm_item_t c = items[((at + step * i) % n + n) % n];
        if (dm_item_enabled(s, w, c)) {
            s->cursor = c;
            return;
        }
    }
}

static void step_multiplier(dm_state_t* s, const dm_world_t* w, int step) {
    int speed = s->cursor == DM_MOVE_SPEED, now = speed ? w->speed : w->jump;
    int next = speed ? ch_speed_clamp(now + step) : ch_multiplier_clamp(now + step);
    if (next != now) request_value(s, speed ? DM_ACT_SPEED : DM_ACT_JUMP, next);
}

static void toggle(dm_state_t* s, dm_action_kind_t kind, int on);

static void step_ambient_occlusion(dm_state_t* s, const dm_world_t* w, int step) {
    int next = w->ambient_occlusion + step;
    if (next >= 0 && next < DM_AO_LEVELS) request_value(s, DM_ACT_AMBIENT_OCCLUSION, next);
}

static void adjust(dm_state_t* s, const dm_world_t* w, float step) {
    float* f;
    if (s->page == DM_PAGE_LEVELS || !dm_item_enabled(s, w, (dm_item_t)s->cursor)) return;
    if (is_multiplier(s->cursor)) {
        step_multiplier(s, w, step > 0.0f ? 1 : -1);
        return;
    }
    if (s->cursor == DM_AMBIENT_OCCLUSION) {
        step_ambient_occlusion(s, w, step > 0.0f ? 1 : -1);
        return;
    }
    if (s->cursor == DM_WATER_REFRACTION || s->cursor == DM_ENHANCED_SKY) {
        int on = step > 0.0f, now = s->cursor == DM_WATER_REFRACTION ? w->water_refraction : w->enhanced_sky;
        if (on != now) toggle(s, s->cursor == DM_WATER_REFRACTION ? DM_ACT_WATER_REFRACTION : DM_ACT_ENHANCED_SKY, on);
        return;
    }
    if (!fields_shown(s) || !is_field(s->cursor)) return;
    commit_edit(s);
    f = &s->field[s->cursor - DM_FIELD_X];
    *f += step;
    if (*f > DM_COORD_LIMIT) *f = DM_COORD_LIMIT;
    if (*f < -DM_COORD_LIMIT) *f = -DM_COORD_LIMIT;
}

static void open_levels(dm_state_t* s, const dm_world_t* w) {
    s->page = DM_PAGE_LEVELS;
    s->level_cursor = 0;
    for (int i = 0; i < s->levels.count; i++)
        if (!_stricmp(s->levels.scene[i], w->scene)) s->level_cursor = i;
}

static void camera_mode(dm_state_t* s, const dm_world_t* w, int mode) {
    request_value(s, DM_ACT_CAMERA, w->camera == mode ? CH_CAMERA_NORMAL : mode);
}

static void toggle(dm_state_t* s, dm_action_kind_t kind, int on) {
    request(s, kind);
    s->pending.on = on;
}

static void activate(dm_state_t* s, const dm_world_t* w, uint32_t now_ms) {
    if (s->page == DM_PAGE_LEVELS) {
        if (w->place == DM_PLACE_NONE || s->level_cursor < 0 || s->level_cursor >= s->levels.count) return;
        request(s, DM_ACT_LOAD_LEVEL);
        snprintf(s->pending.scene, sizeof s->pending.scene, "%s", s->levels.scene[s->level_cursor]);
        dm_close(s);
        return;
    }
    if (is_field(s->cursor)) {
        commit_edit(s);
        return;
    }
    commit_edit(s);
    if (!dm_item_enabled(s, w, (dm_item_t)s->cursor)) return;
    switch ((dm_item_t)s->cursor) {
    case DM_LEVEL_SELECT: open_levels(s, w); break;
    case DM_TELEPORT_PAGE:
        s->page = DM_PAGE_TELEPORT;
        s->cursor = DM_FIELD_X;
        break;
    case DM_RESTART:
        request(s, DM_ACT_RESTART);
        dm_close(s);
        break;
    case DM_RELOAD:
        request(s, DM_ACT_RELOAD);
        snprintf(s->pending.scene, sizeof s->pending.scene, "%s", w->scene);
        dm_close(s);
        break;
    case DM_TELEPORT:
        request(s, DM_ACT_TELEPORT);
        for (int i = 0; i < 3; i++) s->pending.to.pos[i] = s->field[i];
        break;
    case DM_PREV_KEY: request(s, DM_ACT_PREV_KEY); break;
    case DM_NEXT_KEY: request(s, DM_ACT_NEXT_KEY); break;
    case DM_SET_CHECKPOINT:
        s->checkpoint.set = 1;
        snprintf(s->checkpoint.level, sizeof s->checkpoint.level, "%s", w->level);
        s->checkpoint.at = w->at;
        dm_feedback(s, jpt_tr("Checkpoint set"), now_ms);
        break;
    case DM_RETURN_CHECKPOINT:
        request(s, DM_ACT_RETURN_CHECKPOINT);
        s->pending.to = s->checkpoint.at;
        break;
    case DM_SHOW_COORDS:
        s->show_coords = !s->show_coords;
        dm_feedback(s, jpt_tr(s->show_coords ? "Coordinates shown" : "Coordinates hidden"), now_ms);
        break;
    case DM_COPY_COORDS: request(s, DM_ACT_COPY); break;
    case DM_FREEZE_AI: toggle(s, DM_ACT_FREEZE_AI, w->ai == 0); break;
    case DM_MOVE_SPEED: request_value(s, DM_ACT_SPEED, (w->speed + 1) % CH_SPEED_COUNT); break;
    case DM_JUMP_HEIGHT: request_value(s, DM_ACT_JUMP, (w->jump + 1) % CH_MULTIPLIER_COUNT); break;
    case DM_INVULNERABLE: toggle(s, DM_ACT_INVULNERABLE, w->invulnerable == 0); break;
    case DM_INFINITE_AMMO: toggle(s, DM_ACT_AMMO, w->ammo == 0); break;
    case DM_HEAL: request(s, DM_ACT_HEAL); break;
    case DM_SUPER_HEALTH: toggle(s, DM_ACT_SUPER_HEALTH, !w->super_health); break;
    case DM_THIRD_PERSON: camera_mode(s, w, CH_CAMERA_THIRD); break;
    case DM_FREE_CAMERA: camera_mode(s, w, CH_CAMERA_FREE); break;
    case DM_COMPASS:
        s->compass_off = !s->compass_off;
        dm_feedback(s, jpt_tr(s->compass_off ? "Compass off" : "Compass on"), now_ms);
        break;
    case DM_AMBIENT_OCCLUSION:
        request_value(s, DM_ACT_AMBIENT_OCCLUSION, (w->ambient_occlusion + 1) % DM_AO_LEVELS);
        break;
    case DM_WATER_REFRACTION: toggle(s, DM_ACT_WATER_REFRACTION, !w->water_refraction); break;
    case DM_ENHANCED_SKY: toggle(s, DM_ACT_ENHANCED_SKY, !w->enhanced_sky); break;
    case DM_CLOSE: dm_close(s); break;
    default: break;
    }
}

static void back(dm_state_t* s) {
    if (s->editing) {
        s->editing = 0;
    } else if (s->page == DM_PAGE_TELEPORT) {
        s->page = DM_PAGE_MAIN;
        s->cursor = DM_TELEPORT_PAGE;
    } else if (s->page == DM_PAGE_LEVELS) {
        s->page = DM_PAGE_MAIN;
        s->cursor = DM_LEVEL_SELECT;
    } else {
        dm_close(s);
    }
}

static void take_key(dm_state_t* s, unsigned vk, int on) {
    if (on) s->taken[vk >> 3] |= (uint8_t)(1u << (vk & 7));
    else s->taken[vk >> 3] &= (uint8_t)~(1u << (vk & 7));
}

static int key_taken(const dm_state_t* s, unsigned vk) { return (s->taken[vk >> 3] >> (vk & 7)) & 1; }

static void edit_backspace(dm_state_t* s) {
    size_t n;
    if (!fields_shown(s) || !is_field(s->cursor)) return;
    if (!s->editing) {
        snprintf(s->edit, sizeof s->edit, "%.3f", s->field[s->cursor - DM_FIELD_X]);
        s->editing = 1;
    }
    n = strlen(s->edit);
    if (n) s->edit[n - 1] = 0;
}

int dm_key(dm_state_t* s, const dm_world_t* w, unsigned vk, int down, int repeat, int shift, int ctrl,
           uint32_t now_ms) {
    vk &= 0xFFu;
    if (!down) {
        if (!key_taken(s, vk)) return 0;
        take_key(s, vk, 0);
        return 1;
    }
    if (!s->open) {
        if (key_taken(s, vk) && repeat) return 1;
        take_key(s, vk, 0);
        s->char_pending = 0;
        return 0;
    }
    take_key(s, vk, 1);
    s->char_pending = 1;
    switch (vk) {
    case VK_UP_: move_cursor(s, w, -1); break;
    case VK_DOWN_: move_cursor(s, w, 1); break;
    case VK_LEFT_: adjust(s, w, ctrl ? -0.1f : shift ? -10.0f : -1.0f); break;
    case VK_RIGHT_: adjust(s, w, ctrl ? 0.1f : shift ? 10.0f : 1.0f); break;
    case VK_RETURN_: activate(s, w, now_ms); break;
    case VK_ESCAPE_: back(s); break;
    case VK_BACK_: edit_backspace(s); break;
    default: break;
    }
    return 1;
}

int dm_char(dm_state_t* s, const dm_world_t* w, char c) {
    size_t n;
    int pending = s->char_pending;
    s->char_pending = 0;
    if (!s->open) return pending;
    if (!fields_shown(s) || !is_field(s->cursor) || !dm_item_enabled(s, w, (dm_item_t)s->cursor)) return 1;
    if (!isdigit((unsigned char)c) && c != '-' && c != '.') return 1;
    if (!s->editing) {
        s->edit[0] = 0;
        s->editing = 1;
    }
    n = strlen(s->edit);
    if (n + 1 < sizeof s->edit) {
        s->edit[n] = c;
        s->edit[n + 1] = 0;
    }
    return 1;
}

void dm_pad(dm_state_t* s, const dm_world_t* w, uint32_t pressed, uint32_t now_ms) {
    if (!s->open || !pressed) return;
    if (s->menu == DM_MENU_OBJECTIVES && (pressed & JPT_PAD_BIT(JPT_PAD_OBJECTIVES_CONTROL))) {
        dm_close(s);
        return;
    }
    if (pressed & JPT_PAD_BIT(JPT_PAD_DPAD_UP)) move_cursor(s, w, -1);
    if (pressed & JPT_PAD_BIT(JPT_PAD_DPAD_DOWN)) move_cursor(s, w, 1);
    if (pressed & JPT_PAD_BIT(JPT_PAD_DPAD_LEFT)) adjust(s, w, -1.0f);
    if (pressed & JPT_PAD_BIT(JPT_PAD_DPAD_RIGHT)) adjust(s, w, 1.0f);
    if (pressed & JPT_PAD_BIT(JPT_PAD_LEFT_SHOULDER)) adjust(s, w, -10.0f);
    if (pressed & JPT_PAD_BIT(JPT_PAD_RIGHT_SHOULDER)) adjust(s, w, 10.0f);
    if (pressed & JPT_PAD_BIT(JPT_PAD_SOUTH)) activate(s, w, now_ms);
    else if (pressed & JPT_PAD_BIT(JPT_PAD_EAST)) back(s);
    else if (pressed & JPT_PAD_BIT(JPT_PAD_START)) dm_close(s);
}

uint32_t dm_pad_repeat(dm_repeat_t* r, uint32_t held, uint32_t pressed, uint32_t now_ms) {
    const uint32_t dpad = JPT_PAD_BIT(JPT_PAD_DPAD_UP) | JPT_PAD_BIT(JPT_PAD_DPAD_DOWN) |
                          JPT_PAD_BIT(JPT_PAD_DPAD_LEFT) | JPT_PAD_BIT(JPT_PAD_DPAD_RIGHT);
    uint32_t fresh = pressed & dpad;
    if (fresh) {
        r->bit = fresh & (0u - fresh);
        r->next_ms = now_ms + REPEAT_DELAY_MS;
        return 0;
    }
    if (!r->bit || !(held & r->bit)) {
        r->bit = 0;
        return 0;
    }
    if ((int32_t)(now_ms - r->next_ms) < 0) return 0;
    r->next_ms = now_ms + REPEAT_EVERY_MS;
    return r->bit;
}

void dm_world_seen(dm_state_t* s, const dm_world_t* w) {
    if (s->checkpoint.set && (w->place == DM_PLACE_MAIN_MENU ||
                              (w->place == DM_PLACE_LEVEL && w->level[0] && strcmp(w->level, s->checkpoint.level))))
        memset(&s->checkpoint, 0, sizeof s->checkpoint);
    if (!s->open) return;
    if (w->place == DM_PLACE_NONE || (s->menu == DM_MENU_OBJECTIVES && w->place != DM_PLACE_LEVEL)) {
        dm_close(s);
        return;
    }
    if (s->fill_fields && w->player) dm_fill_fields(s, &w->at);
    if (s->page != DM_PAGE_LEVELS && !s->editing) settle_cursor(s, w);
}

int dm_take(dm_state_t* s, dm_action_t* out) {
    if (s->pending.kind == DM_ACT_NONE) return 0;
    *out = s->pending;
    memset(&s->pending, 0, sizeof s->pending);
    return 1;
}

void dm_set_levels(dm_state_t* s, const dm_levels_t* levels) {
    s->levels = *levels;
    if (s->level_cursor >= s->levels.count) s->level_cursor = 0;
}

int dm_scene_valid(const char* scene) {
    size_t n = scene ? strlen(scene) : 0;
    if (n < 5 || n >= DM_SCENE_MAX || _stricmp(scene + n - 4, ".scn")) return 0;
    for (size_t i = 0; i + 4 < n; i++)
        if (!isalnum((unsigned char)scene[i]) && scene[i] != '_' && scene[i] != '-') return 0;
    return 1;
}

int dm_level_id(const char* scene, char* out, size_t cap) {
    size_t n;
    if (!cap) return 0;
    out[0] = 0;
    if (!dm_scene_valid(scene)) return 0;
    n = strlen(scene) - 4;
    if (n >= cap) return 0;
    for (size_t i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)scene[i]);
    out[n] = 0;
    return 1;
}

static const struct {
    const char* id;
    const char* name;
} k_retail_levels[] = {
    { "BE", "Beach" },
    { "JR", "Jungle Road" },
    { "IJ", "Industrial Jungle" },
    { "IT", "InGen Town" },
    { "LAB", "InGen Lab" },
    { "AS", "Ascent, Part 1" },
    { "AS2", "Ascent, Part 2" },
    { "SUM", "Summit" },
    { "TESTSCENE", "Test Scene" },
    { "TESTSCNNGHT", "Test Scene Night" },
};

static int retail_rank(const char* scene) {
    char id[DM_LEVEL_ID_MAX];
    if (!dm_level_id(scene, id, sizeof id)) return COUNT(k_retail_levels);
    for (int i = 0; i < COUNT(k_retail_levels); i++)
        if (!strcmp(id, k_retail_levels[i].id)) return i;
    return COUNT(k_retail_levels);
}

const char* dm_level_name(const char* scene) {
    int rank = retail_rank(scene);
    return rank < COUNT(k_retail_levels) ? jpt_tr(k_retail_levels[rank].name) : NULL;
}

int dm_level_title(const char* scene, char* out, size_t cap) {
    char id[DM_LEVEL_ID_MAX];
    const char* name = dm_level_name(scene);
    int n;
    if (!dm_level_id(scene, id, sizeof id)) return 0;
    n = name ? snprintf(out, cap, "%s - %s", id, name) : snprintf(out, cap, "%s", id);
    return n > 0 && (size_t)n < cap;
}

int dm_levels_add(dm_levels_t* l, const char* scene) {
    int at = 0, rank = retail_rank(scene);
    if (!dm_scene_valid(scene) || l->count >= DM_LEVELS_MAX) return 0;
    for (int i = 0; i < l->count; i++) {
        int other = retail_rank(l->scene[i]), c = rank != other ? rank - other : _stricmp(scene, l->scene[i]);
        if (!c) return 0;
        if (c > 0) at = i + 1;
    }
    memmove(&l->scene[at + 1], &l->scene[at], (size_t)(l->count - at) * sizeof l->scene[0]);
    snprintf(l->scene[at], sizeof l->scene[at], "%s", scene);
    l->count++;
    return 1;
}

int dm_teleport_valid(const float pos[3]) {
    for (int i = 0; i < 3; i++)
        if (!isfinite(pos[i]) || fabsf(pos[i]) > DM_COORD_LIMIT) return 0;
    return 1;
}

float dm_yaw(const float rot[4]) {
    double c = rot[0], x = rot[1], y = rot[2], z = rot[3];
    double fx = 2.0 * (x * y - c * z), fy = 1.0 - 2.0 * (x * x + z * z);
    return fx == 0.0 && fy == 0.0 ? 0.0f : (float)atan2(-fx, fy);
}

static double tidy(double v) { return fabs(v) < 0.0005 ? 0.0 : v; }

int dm_format_coords(char* out, size_t cap, const char* level, const dm_placement_t* at) {
    int n = snprintf(out, cap, "Level=%s X=%.3f Y=%.3f Z=%.3f Yaw=%.3f", level && level[0] ? level : "-",
                     tidy(at->pos[0]), tidy(at->pos[1]), tidy(at->pos[2]), tidy(dm_yaw(at->rot)));
    return n > 0 && (size_t)n < cap;
}

int dm_keys_walk(dm_read32_t read32, uint32_t head, uint32_t iterator, dm_keys_t* out) {
    uint32_t node, next, prev, at = 0;
    int count = 0, it_pos = -1, target;
    memset(out, 0, sizeof *out);
    out->index = -1;
    if (!head || !read32(head, &node)) return 0;
    prev = head;
    while (node != head) {
        uint32_t back_link;
        if (count >= DM_KEYS_MAX || !node || !read32(node, &next) || !read32(node + 4, &back_link) || back_link != prev)
            return 0;
        if (node == iterator) it_pos = count;
        prev = node;
        node = next;
        count++;
    }
    if (iterator == head) it_pos = count;
    if (it_pos < 0) return 0;
    out->count = count;
    out->index = it_pos > 0 ? it_pos - 1 : -1;
    if (!count) return 1;
    target = dm_prev_key_index(count, out->index);
    read32(head, &at);
    for (int i = 0; i < target; i++) read32(at, &at);
    out->prev_target = at;
    return 1;
}

int dm_prev_key_index(int count, int index) {
    if (count <= 0) return -1;
    if (index <= 0 || index >= count) return count - 1;
    return index - 1;
}

static void add_row(dm_row_t* rows, int max, int* n, dm_row_kind_t kind, int item, int enabled, int selected,
                    const char* text, const char* value) {
    dm_row_t* r;
    if (*n >= max) return;
    r = &rows[(*n)++];
    r->kind = kind;
    r->item = item;
    r->enabled = enabled;
    r->selected = selected;
    snprintf(r->text, sizeof r->text, "%s", text);
    snprintf(r->value, sizeof r->value, "%s", value ? value : "");
}

static const char* on_off(int v) { return v < 0 ? "-" : jpt_tr(v ? "On" : "Off"); }

static void item_value(const dm_state_t* s, const dm_world_t* w, dm_item_t item, int enabled, char* value, size_t cap) {
    value[0] = 0;
    if (is_field(item)) {
        if (s->editing && s->cursor == (int)item) snprintf(value, cap, "%s_", s->edit);
        else if (enabled) snprintf(value, cap, "%.3f", s->field[item - DM_FIELD_X]);
        else snprintf(value, cap, "-");
        return;
    }
    switch (item) {
    case DM_SHOW_COORDS: snprintf(value, cap, "%s", on_off(s->show_coords)); break;
    case DM_FREEZE_AI: snprintf(value, cap, "%s", on_off(w->ai)); break;
    case DM_INVULNERABLE: snprintf(value, cap, "%s", on_off(w->invulnerable)); break;
    case DM_INFINITE_AMMO: snprintf(value, cap, "%s", on_off(w->ammo)); break;
    case DM_SUPER_HEALTH: snprintf(value, cap, "%s", on_off(w->super_health)); break;
    case DM_THIRD_PERSON: snprintf(value, cap, "%s", on_off(w->camera == CH_CAMERA_THIRD)); break;
    case DM_FREE_CAMERA: snprintf(value, cap, "%s", on_off(w->camera == CH_CAMERA_FREE)); break;
    case DM_MOVE_SPEED: snprintf(value, cap, "%s", ch_speed_text(w->speed)); break;
    case DM_JUMP_HEIGHT: snprintf(value, cap, "%s", ch_multiplier_text(w->jump)); break;
    case DM_HEAL:
        if (enabled) snprintf(value, cap, "%.0f / %.0f  >", w->hp, w->hp_max);
        break;
    case DM_TELEPORT_PAGE: snprintf(value, cap, ">"); break;
    case DM_COMPASS: snprintf(value, cap, "%s", on_off(!s->compass_off)); break;
    case DM_AMBIENT_OCCLUSION:
        snprintf(value, cap, "%s", jpt_tr(enabled ? k_ao_levels[w->ambient_occlusion] : "Unavailable"));
        break;
    case DM_WATER_REFRACTION: snprintf(value, cap, "%s", enabled ? on_off(w->water_refraction) : jpt_tr("Unavailable")); break;
    case DM_ENHANCED_SKY: snprintf(value, cap, "%s", enabled ? on_off(w->enhanced_sky) : jpt_tr("Unavailable")); break;
    case DM_LEVEL_SELECT:
        if (s->menu == DM_MENU_CHEATS) snprintf(value, cap, ">");
        break;
    case DM_RETURN_CHECKPOINT:
        if (s->checkpoint.set)
            snprintf(value, cap, "%s %.1f %.1f %.1f", s->checkpoint.level, s->checkpoint.at.pos[0],
                     s->checkpoint.at.pos[1], s->checkpoint.at.pos[2]);
        break;
    case DM_PREV_KEY:
    case DM_NEXT_KEY:
        if (w->place != DM_PLACE_LEVEL) break;
        if (w->key_count <= 0) snprintf(value, cap, "%s", jpt_tr("none"));
        else if (item == DM_NEXT_KEY && w->key_index >= 0)
            snprintf(value, cap, "%d / %d", w->key_index + 1, w->key_count);
        else if (item == DM_NEXT_KEY) snprintf(value, cap, "- / %d", w->key_count);
        break;
    default: break;
    }
}

static void item_row(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max, int* n, dm_item_t item) {
    char value[DM_TEXT_MAX];
    int enabled = dm_item_enabled(s, w, item);
    item_value(s, w, item, enabled, value, sizeof value);
    add_row(rows, max, n, DM_ROW_ITEM, (int)item, enabled, s->page != DM_PAGE_LEVELS && s->cursor == (int)item,
            dm_item_label(s->menu, item), value);
}

static void items_rows(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max, int* n,
                       const dm_item_t* items, int count) {
    for (int i = 0; i < count; i++) item_row(s, w, rows, max, n, items[i]);
}

void dm_notice_for(dm_state_t* s, const char* text, uint32_t now_ms, uint32_t duration_ms) {
    snprintf(s->notice, sizeof s->notice, "%s", text);
    s->notice_until = now_ms + duration_ms;
}

void dm_notice(dm_state_t* s, const char* text, uint32_t now_ms) { dm_notice_for(s, text, now_ms, DM_FEEDBACK_MS); }

void dm_navigate(const dm_state_t* s, const dm_world_t* w, const cp_scene_t* scene, const cp_goal_t* goal,
                 uint32_t now_ms, cp_hud_t* hud) {
    int next;
    memset(hud, 0, sizeof *hud);
    if (!cp_hud_allowed(scene)) return;
    if (s->notice[0] && (int32_t)(s->notice_until - now_ms) > 0)
        snprintf(hud->notice, sizeof hud->notice, "%s", s->notice);
    hud->alpha = 1.0f;
    if (s->compass_off || !w->eye) return;
    hud->compass = 1;
    hud->heading = cp_heading(dm_yaw(w->eye_at.rot));
    hud->view = w->tan_half_h > 0.0f;
    memcpy(hud->camera.pos, w->eye_at.pos, sizeof hud->camera.pos);
    memcpy(hud->camera.rot, w->eye_at.rot, sizeof hud->camera.rot);
    hud->camera.tan_half_h = w->tan_half_h;
    if (!goal || !goal->set || !goal->target) return;
    hud->marker = 1;
    memcpy(hud->target, goal->pos, sizeof hud->target);
    hud->relative = cp_relative(cp_bearing(w->eye_at.pos, goal->pos), hud->heading);
    if (w->player) cp_format_distance(cp_distance_m(w->at.pos, goal->pos), hud->distance, sizeof hud->distance);
    if (!w->player) return;
    for (next = cp_route_next(goal->route, goal->route_count, w->at.pos);
         next < goal->route_count && hud->trail_count < CP_TRAIL_MAX; next++) {
        if (cp_distance_m(w->at.pos, goal->route[next]) > CP_TRAIL_RANGE_M) break;
        memcpy(hud->trail[hud->trail_count++], goal->route[next], sizeof hud->trail[0]);
    }
}

static int objective_rows(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max) {
    char title[DM_TEXT_MAX];
    int n = 0, unknown = 0;
    add_row(rows, max, &n, DM_ROW_TITLE, -1, 1, 0, dm_menu_title(s->menu), NULL);
    if (!dm_level_title(w->scene, title, sizeof title)) snprintf(title, sizeof title, "%s", w->level[0] ? w->level : "?");
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, title, NULL);
    if (!w->goals.available) {
        add_row(rows, max, &n, DM_ROW_NOTE, -1, 0, 0, jpt_tr("Objectives unavailable for this level"), NULL);
        return n;
    }
    for (int i = 0; i < w->goals.count && i < DM_GOALS_MAX; i++) {
        if (w->goals.item[i].status == DM_GOAL_UNKNOWN) {
            unknown = 1;
            continue;
        }
        add_row(rows, max, &n, DM_ROW_GOAL, w->goals.item[i].status, 1, 0, w->goals.item[i].title, NULL);
        if (w->goals.item[i].status == DM_GOAL_CURRENT && w->goals.item[i].hint[0])
            add_row(rows, max, &n, DM_ROW_NOTE, -1, 1, 0, w->goals.item[i].hint, NULL);
    }
    if (unknown) add_row(rows, max, &n, DM_ROW_GOAL, DM_GOAL_UNKNOWN, 0, 0, "???", NULL);
    item_row(s, w, rows, max, &n, DM_CLOSE);
    return n;
}

int dm_rows(const dm_state_t* s, const dm_world_t* w, dm_row_t* rows, int max) {
    char line[DM_TEXT_MAX];
    int n = 0;
    if (s->menu == DM_MENU_OBJECTIVES) return objective_rows(s, w, rows, max);
    add_row(rows, max, &n, DM_ROW_TITLE, -1, 1, 0, dm_menu_title(s->menu), NULL);
    add_row(rows, max, &n, DM_ROW_INFO, -1, 1, 0, jpt_tr("Level"),
            w->place == DM_PLACE_LEVEL ? (w->level[0] ? w->level : "?")
                                       : jpt_tr(w->place == DM_PLACE_MAIN_MENU ? "none (main menu)" : "none"));
    if (w->place == DM_PLACE_LEVEL && w->player) {
        snprintf(line, sizeof line, "X %.3f  Y %.3f  Z %.3f  Yaw %.3f", tidy(w->at.pos[0]), tidy(w->at.pos[1]),
                 tidy(w->at.pos[2]), tidy(dm_yaw(w->at.rot)));
        add_row(rows, max, &n, DM_ROW_INFO, -1, 1, 0, jpt_tr("Player"), line);
    } else {
        add_row(rows, max, &n, DM_ROW_INFO, -1, 0, 0, jpt_tr("Player"), jpt_tr("unavailable"));
    }
    if (s->page == DM_PAGE_LEVELS) {
        add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Level Select"), NULL);
        if (!s->levels.count)
            add_row(rows, max, &n, DM_ROW_INFO, -1, 0, 0, jpt_tr("No scene files in the data folder"), NULL);
        for (int i = 0; i < s->levels.count; i++) {
            char title[DM_TEXT_MAX];
            dm_level_title(s->levels.scene[i], title, sizeof title);
            add_row(rows, max, &n, DM_ROW_ITEM, i, w->place != DM_PLACE_NONE, s->level_cursor == i, title,
                    _stricmp(s->levels.scene[i], w->scene) || w->place != DM_PLACE_LEVEL ? s->levels.scene[i]
                                                                                       : jpt_tr("current"));
        }
        return n;
    }
    if (s->page == DM_PAGE_TELEPORT) {
        add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Teleport"), NULL);
        items_rows(s, w, rows, max, &n, k_teleport_page, COUNT(k_teleport_page));
        return n;
    }
    if (s->menu == DM_MENU_CHEATS) {
        add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Cheats"), NULL);
        items_rows(s, w, rows, max, &n, k_cheats_main, COUNT(k_cheats_main));
        return n;
    }
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Level"), NULL);
    items_rows(s, w, rows, max, &n, k_debug_main, 3);
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Teleport"), NULL);
    items_rows(s, w, rows, max, &n, k_debug_main + 3, 8);
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Navigation"), NULL);
    items_rows(s, w, rows, max, &n, k_debug_main + 11, 1);
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Debug"), NULL);
    items_rows(s, w, rows, max, &n, k_debug_main + 12, 3);
    add_row(rows, max, &n, DM_ROW_SECTION, -1, 1, 0, jpt_tr("Graphics"), NULL);
    items_rows(s, w, rows, max, &n, k_debug_main + 15, 3);
    items_rows(s, w, rows, max, &n, k_debug_main + 18, 1);
    return n;
}
