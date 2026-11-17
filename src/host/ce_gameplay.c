/*
 * Gameplay features reimplemented from Trespasser CE over the retail code: Quick Save / Quick Load (the game's own save
 * writer and loader on SAVEGAME.QCK), Dual stow (the retail stow handler with CE's stow-only and back-retrieve
 * choices) and Maximum object detail (the renderer's bDetailReduce cleared, as CE's ForceMaxObjectDetail). Always on:
 * collision sounds (footsteps) re-armed when a load sets the game time back.
 */
#include "guest_va.h"
#include "host.h"

#include <string.h>

#include "ce_policy.h"
#include "jpt_tr.h"

#define APP GVA(0x0066E338)
#define WORLD GVA(0x0073F444)
#define WORLD_LOAD_PENDING 0x38u
#define PLAYER GVA(0x0066EF64)
#define PLAYER_DEAD_SLOT 0x1FCu
#define GAME_WND_PAUSE GVA(0x0040BCC0)
#define GAME_WND_RESUME GVA(0x0040BD70)
#define SAVE_DIR GVA(0x00411B30)
#define SAVE_DIR_SAVES 1u
#define APP_SAVE_FILE GVA(0x00414570)
#define READ_SAVE_HEADER GVA(0x00410420)
#define APP_MENU_SOUNDS_OFF GVA(0x00414D80)
#define APP_LOAD_SAVE GVA(0x004142C0)
#define APP_LOAD_FAILED GVA(0x00413160)
#define MAIN_MENU_PLAY 2

#define SAVE_LIST_VA GVA(0x00410540)
#define DISK_SPACE_CHECK_VA GVA(0x0040DA00)
#define SAVE_WND_VTABLE GVA(0x006462A8)
#define SAVE_WND_LIST 0x3Cu
#define SAVE_ENTRY_BYTES 0x54u
#define SAVE_ENTRY_FILE 0x28u
#define SAVE_ENTRY_SCENE 0x37u

#define STOW_VA GVA(0x004610D0)
#define STOW_CHOOSER GVA(0x0045ED80)
#define MSG_HELD 0x24u
#define MSG_PRESSED 0x28u
#define CMD_STOW 0x100u
#define CMD_ARM_OR_WRIST 0x18u
#define PLAYER_POSE_RETRIEVE 0xDB4u
#define PLAYER_POSE_HAND 0xDD0u
#define POSE_BYTES 28u
#define PLAYER_HOLDING 0xE30u
#define PLAYER_HOLDING_OBJECT 4u
#define PLAYER_BELT 0xE38u
#define PLAYER_BACK 0xE3Cu
#define PLAYER_SLOT 0xE40u
#define PLAYER_PHASE 0xE44u

#define RENDER_SETTINGS GVA(0x006D2224)
#define SETTINGS_DETAIL_REDUCE 0x33u

#define GAME_TIME GVA(0x0073F34C)
#define AUDIO_DAEMON GVA(0x0066E9F0)
#define DAEMON_EFFECTS 0xC4u
#define EFFECTS_MAP 0x2Cu
#define MAP_NIL GVA(0x0066EB00)
#define NODE_PARENT 4u
#define NODE_LEFT 8u
#define NODE_RIGHT 0xCu
#define NODE_EFFECT 0x18u
#define EFFECT_MIN_DELAY 4u
#define EFFECT_LAST_TIME 8u
#define EFFECTS_MAX 4096

#define SCRATCH_BYTES 0x200u

typedef enum { QS_NONE, QS_SAVE, QS_LOAD } qs_request_t;

static struct {
    int dual_stow, max_detail;
    qs_request_t request;
    recomp_func_t save_list_lifted, stow_lifted;
    uint32_t pose_player;
    int pose_valid, stow_only;
    uint8_t pose[2][POSE_BYTES], last_retrieve[POSE_BYTES];
    int detail_logged;
    float last_game_time;
} C;

void ce_gameplay_configure(int dual_stow, int max_object_detail) {
    C.dual_stow = dual_stow != 0;
    C.max_detail = max_object_detail != 0;
}

void quick_save_request(void) { C.request = QS_SAVE; }

void quick_load_request(void) { C.request = QS_LOAD; }

static uint32_t guest_text(uint32_t at, const char* text) {
    size_t n = strlen(text);
    memcpy((void*)ADDR(at), text, n + 1);
    return at;
}

static int player_alive(void) {
    uint32_t player, dead = 0;
    return guest_read32(PLAYER, &player) && player && guest_call_virtual(player, PLAYER_DEAD_SLOT, NULL, 0, &dead) &&
           !(dead & 0xFFu);
}

static int load_pending(void) {
    uint32_t world, v;
    return guest_read32(WORLD, &world) && world && guest_read32(world + WORLD_LOAD_PENDING, &v) && (v & 0xFFu);
}

static qs_place_t place_of(const guest_ui_view_t* v) {
    if (v->depth != 1) return QS_PLACE_NONE;
    if (v->kind == GUEST_WND_GAME && v->ui == GUEST_UI_GAMEPLAY) return QS_PLACE_LEVEL;
    if (v->kind == GUEST_WND_MAIN_MENU) return QS_PLACE_MAIN_MENU;
    return QS_PLACE_NONE;
}

/* The game's saves folder (its own call, as its Load list builds it) and SAVEGAME.QCK in it. */
static int quick_path(uint32_t scratch, char* path, size_t cap) {
    uint32_t args[3] = { SAVE_DIR_SAVES, scratch, MAX_PATH };
    char dir[MAX_PATH];
    *(char*)ADDR(scratch) = 0;
    return guest_call(SAVE_DIR, 0, args, 3, 1, NULL) && guest_read_string(scratch, dir, sizeof dir) &&
           qs_path(dir, path, cap);
}

static void feedback(const char* key) { debug_menu_notice(jpt_tr(key)); }

static void quick_save(const guest_ui_view_t* v, const char* path, uint32_t scratch) {
    char name[QS_NAME_MAX + 1];
    uint32_t args[2], resume = 1, written = 0;
    qs_display_name(jpt_tr_cp("Quick Save"), name, sizeof name);
    args[0] = guest_text(scratch, path);
    args[1] = guest_text(scratch + MAX_PATH, name);
    if (!guest_call(GAME_WND_PAUSE, v->top, NULL, 0, 0, NULL)) {
        host_log("quick-save", "the game could not be paused: nothing was saved");
        feedback("Quick Save failed");
        return;
    }
    guest_call(APP_SAVE_FILE, APP, args, 2, 0, &written);
    guest_call(GAME_WND_RESUME, v->top, &resume, 1, 0, NULL);
    if (written) {
        host_log("quick-save", "saved %s", path);
        feedback("Quick saved");
    } else {
        host_log("quick-save", "the game could not write %s", path);
        feedback("Quick Save failed");
    }
}

static void quick_load(const guest_ui_view_t* v, qs_place_t place, const char* path, uint32_t scratch) {
    uint32_t entry = scratch + MAX_PATH, args[2], ok = 0, result = 0, resume = 1;
    args[0] = guest_text(scratch, path);
    args[1] = entry;
    memset((void*)ADDR(entry), 0, SAVE_ENTRY_BYTES);
    if (!guest_call(READ_SAVE_HEADER, 0, args, 2, 1, &ok) || !ok) {
        host_log("quick-save", "%s is not a save the game can read: nothing was loaded", path);
        feedback("Quick Load failed");
        return;
    }
    args[1] = entry + SAVE_ENTRY_SCENE;
    if (place == QS_PLACE_LEVEL && !guest_call(GAME_WND_PAUSE, v->top, NULL, 0, 0, NULL)) return;
    if (!guest_call(APP_MENU_SOUNDS_OFF, APP, NULL, 0, 0, NULL) || !guest_call(APP_LOAD_SAVE, APP, args, 2, 0, &result)) {
        if (place == QS_PLACE_LEVEL) guest_call(GAME_WND_RESUME, v->top, &resume, 1, 0, NULL);
        return;
    }
    if ((int32_t)result < 0) {
        host_log("quick-save", "the game could not load %s (%d)", path, (int)(int32_t)result);
        guest_call(APP_LOAD_FAILED, 0, &result, 1, 1, NULL);
    } else {
        host_log("quick-save", "loaded %s", path);
    }
    if (place == QS_PLACE_LEVEL) guest_call(GAME_WND_RESUME, v->top, &resume, 1, 0, NULL);
    else if ((int32_t)result >= 0) guest_ui_end_window(v, MAIN_MENU_PLAY, 0);
}

static const char* refusal(qs_decision_t d, int save) {
    switch (d) {
    case QS_REFUSE_PLACE: return save ? "Quick Save works in a level" : NULL;
    case QS_REFUSE_DEAD: return "Quick Save is not available now";
    case QS_REFUSE_BUSY: return save ? "Quick Save is not available now" : "Quick Load is not available now";
    case QS_REFUSE_MISSING: return "No Quick Save yet";
    default: return NULL;
    }
}

static void run_request(qs_request_t request) {
    guest_ui_view_t v;
    qs_world_t w;
    qs_decision_t d;
    char path[MAX_PATH];
    const char* text;
    uint32_t esp = g_esp, scratch;
    memset(&w, 0, sizeof w);
    guest_ui_probe(&v);
    w.place = place_of(&v);
    if (w.place == QS_PLACE_NONE) {
        host_log("quick-save", "Quick %s ignored: no level or main menu in front (%s)", request == QS_SAVE ? "Save" : "Load",
                 guest_ui_class_name(v.top_vtable));
        return;
    }
    g_esp -= SCRATCH_BYTES + MAX_PATH;
    scratch = g_esp;
    if (!quick_path(scratch, path, sizeof path)) {
        host_log("quick-save", "the game's saves folder could not be read");
        g_esp = esp;
        return;
    }
    w.player_alive = w.place == QS_PLACE_LEVEL && player_alive();
    w.load_pending = load_pending();
    w.host_panel = debug_menu_visible() || overlay_blocks_gameplay();
    w.file_exists = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    d = request == QS_SAVE ? qs_decide_save(&w) : qs_decide_load(&w);
    if (d == QS_DO_SAVE) quick_save(&v, path, scratch);
    else if (d == QS_DO_LOAD_LEVEL || d == QS_DO_LOAD_MAIN_MENU) quick_load(&v, w.place, path, scratch);
    else {
        text = refusal(d, request == QS_SAVE);
        host_log("quick-save", "Quick %s refused: %s", request == QS_SAVE ? "Save" : "Load", text ? text : "not here");
        if (text) feedback(text);
    }
    g_esp = esp;
}

static void apply_max_detail(void) {
    uint32_t renderer, settings;
    if (!C.max_detail || !guest_read32(RENDER_SETTINGS, &renderer) || !renderer || !guest_read32(renderer, &settings) ||
        !settings || !guest_readable(settings + SETTINGS_DETAIL_REDUCE, 1))
        return;
    if (MEM8(settings + SETTINGS_DETAIL_REDUCE) && !C.detail_logged++)
        host_log("graphics", "Maximum object detail: every object keeps its most detailed model");
    MEM8(settings + SETTINGS_DETAIL_REDUCE) = 0;
}

/* CAudioDaemon's collision effects (a std::map of material pairs): every pair whose last play lies after the game
 * time a load restored would stay silent until the clock passed it again. */
static void rearm_collision_sounds(float now) {
    uint32_t daemon, effects, map, head, nil, stack[64], node, effect;
    int depth = 0, visited = 0, rearmed = 0;
    if (!guest_read32(AUDIO_DAEMON, &daemon) || !daemon || !guest_read32(daemon + DAEMON_EFFECTS, &effects) || !effects ||
        !guest_read32(effects + EFFECTS_MAP, &map) || !map || !guest_read32(map, &head) || !head ||
        !guest_read32(MAP_NIL, &nil) || !guest_read32(head + NODE_PARENT, &node))
        return;
    if (node && node != nil && node != head) stack[depth++] = node;
    while (depth && visited++ < EFFECTS_MAX) {
        uint32_t left, right;
        node = stack[--depth];
        if (!guest_readable(node, NODE_EFFECT + 4) || !guest_read32(node + NODE_EFFECT, &effect)) break;
        if (effect && guest_readable(effect + EFFECT_MIN_DELAY, 8))
            rearmed += sound_rearm((float*)ADDR(effect + EFFECT_LAST_TIME), *(const float*)ADDR(effect + EFFECT_MIN_DELAY), now);
        left = MEM32(node + NODE_LEFT);
        right = MEM32(node + NODE_RIGHT);
        if (left && left != nil && left != head && depth < 64) stack[depth++] = left;
        if (right && right != nil && right != head && depth < 64) stack[depth++] = right;
    }
    if (rearmed) host_log("audio", "game time went back to %.1f s: %d collision sounds re-armed", now, rearmed);
}

static void watch_game_time(void) {
    float now;
    if (!guest_readable(GAME_TIME, 4)) return;
    now = *(const float*)ADDR(GAME_TIME);
    if (now < C.last_game_time) rearm_collision_sounds(now);
    C.last_game_time = now;
}

void ce_gameplay_ui_loop(void) {
    qs_request_t request = C.request;
    watch_game_time();
    apply_max_detail();
    if (request == QS_NONE) return;
    C.request = QS_NONE;
    run_request(request);
}

/* CSaveGameWnd's list (the vector at +0x3C) without the Quick Save: the retail Save dialog would overwrite the row
 * picked as SAVEGAME.<atoi("QCK")>, i.e. SAVEGAME.000. The Load lists keep it. */
static void save_list(void) {
    uint32_t list = MEM32(g_esp + 4), first, last, vtable;
    int count, kept;
    if (g_alias_entry) {
        C.save_list_lifted();
        return;
    }
    C.save_list_lifted();
    if (list < SAVE_WND_LIST || !guest_read32(list - SAVE_WND_LIST, &vtable) || vtable != SAVE_WND_VTABLE ||
        !guest_read32(list, &first) || !guest_read32(list + 4, &last) || !first || last < first)
        return;
    count = (int)((last - first) / SAVE_ENTRY_BYTES);
    if (!guest_readable(first, last - first)) return;
    kept = qs_filter_entries((uint8_t*)ADDR(first), count, SAVE_ENTRY_BYTES, SAVE_ENTRY_FILE);
    if (kept != count) guest_write32(list + 4, first + (uint32_t)kept * SAVE_ENTRY_BYTES);
}

/* cdecl bool (megabytes): the check is skipped, so the caller's test of AL takes the path of enough space. */
static recomp_func_t s_disk_check_lifted;

static void disk_space_check(void) {
    static int logged;
    g_eax = startup_disk_check_passes();
    GUEST_RETURN_STDCALL(0);
    if (!logged++) host_log("host", "legacy 100 MB disk-space startup check bypassed");
}

static void copy_pose(uint32_t at, const uint8_t* pose) { memcpy((void*)ADDR(at), pose, POSE_BYTES); }

static void read_pose(uint32_t at, uint8_t* pose) { memcpy(pose, (const void*)ADDR(at), POSE_BYTES); }

/* CE keeps a retrieve pose per slot where retail has one; a pose the game wrote itself (a load, a new player) seeds
 * both, as CE does for retail saves. */
static void track_poses(uint32_t player) {
    uint8_t now[POSE_BYTES];
    read_pose(player + PLAYER_POSE_RETRIEVE, now);
    if (C.pose_valid && C.pose_player == player && !memcmp(now, C.last_retrieve, POSE_BYTES)) return;
    memcpy(C.pose[0], now, POSE_BYTES);
    memcpy(C.pose[1], now, POSE_BYTES);
    C.pose_player = player;
    C.pose_valid = 1;
    C.stow_only = 0;
}

/* CPlayer's stow handler, thiscall (message), ret 4. */
static void stow(void) {
    uint32_t player = g_ecx, msg = MEM32(g_esp + 4), belt, back, chooser = (uint32_t)-1, active;
    int phase, preload, slot_had;
    ds_press_t press;
    ds_plan_t plan;
    if (g_alias_entry || !C.dual_stow || !player || !guest_readable(player + PLAYER_POSE_RETRIEVE, 0xE48u - 0xDB4u) ||
        !guest_readable(msg + MSG_HELD, 8)) {
        C.stow_lifted();
        return;
    }
    track_poses(player);
    belt = MEM32(player + PLAYER_BELT);
    back = MEM32(player + PLAYER_BACK);
    phase = (int)MEM32(player + PLAYER_PHASE);
    memset(&press, 0, sizeof press);
    press.enabled = 1;
    press.stow_pressed = (MEM32(msg + MSG_PRESSED) & CMD_STOW) != 0;
    press.modifier = (MEM32(msg + MSG_HELD) & CMD_ARM_OR_WRIST) != 0;
    press.phase = phase;
    press.belt = belt != 0;
    press.back = back != 0;
    press.chooser = -1;
    if (ds_needs_chooser(&press) && guest_call(STOW_CHOOSER, player, NULL, 0, 0, &chooser)) press.chooser = (int)chooser;
    plan = ds_plan(&press);
    if (phase == 0) C.stow_only = plan.stow_only;
    active = MEM32(player + PLAYER_SLOT);
    preload = ds_pose_preload(phase, (int)active, MEM32(player + (active ? PLAYER_BELT : PLAYER_BACK)) != 0,
                              MEM32(player + PLAYER_HOLDING) == 0);
    if (preload >= 0) copy_pose(player + PLAYER_POSE_RETRIEVE, C.pose[preload]);
    slot_had = active <= 1 && MEM32(player + PLAYER_BELT + 4u * active) != 0;
    if (plan.hide_belt) MEM32(player + PLAYER_BELT) = 0;
    C.stow_lifted();
    if (plan.hide_belt) {
        MEM32(player + PLAYER_BELT) = belt;
        host_log("dual-stow", "retrieving from the back (Rotate Arm or Rotate Wrist held)");
    }
    if (ds_pose_swapped(phase, (int)MEM32(player + PLAYER_PHASE)) && active <= 1) {
        read_pose(player + PLAYER_POSE_RETRIEVE, C.pose[active]);
        if (!slot_had) copy_pose(player + PLAYER_POSE_HAND, C.pose[active]);
    }
    if (ds_stop_after_stow(C.stow_only, phase, (int)MEM32(player + PLAYER_PHASE), MEM32(player + PLAYER_HOLDING) == 0)) {
        MEM32(player + PLAYER_PHASE) = 0;
        host_log("dual-stow", "stowed, hand left empty (belt %s, back %s)", MEM32(player + PLAYER_BELT) ? "full" : "empty",
                 MEM32(player + PLAYER_BACK) ? "full" : "empty");
    }
    if (!MEM32(player + PLAYER_PHASE)) C.stow_only = 0;
    read_pose(player + PLAYER_POSE_RETRIEVE, C.last_retrieve);
}

void ce_gameplay_install(void) {
    if (!guest_override_install(DISK_SPACE_CHECK_VA, disk_space_check, &s_disk_check_lifted))
        host_log("host", "warning: the legacy disk-space startup check could not be bypassed");
    if (!guest_override_install(SAVE_LIST_VA, save_list, &C.save_list_lifted))
        host_log("host", "warning: the Save dialog's list could not be hooked; it may show the Quick Save");
    if (C.dual_stow && !guest_override_install(STOW_VA, stow, &C.stow_lifted))
        host_log("host", "warning: the stow handler could not be hooked: Dual stow is off");
}
