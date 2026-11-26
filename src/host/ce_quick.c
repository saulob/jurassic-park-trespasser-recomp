/*
Quick Save and Quick Load: the game's own save writer and loader on SAVEGAME.QCK, run from the UI loop where the game
could call them itself. Windows only: the decisions are ce_policy.c's, shared with Linux (src/linux/linux_quick.c).
 */
#include "guest_va.h"
#include "host.h"

#include <string.h>

#include "ce_policy.h"
#include "jpt_level_gate.h"
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

#define SCRATCH_BYTES 0x200u

typedef enum { QS_NONE, QS_SAVE, QS_LOAD } qs_request_t;

static qs_request_t s_request;

void quick_save_request(void) { s_request = QS_SAVE; }

void quick_load_request(void) { s_request = QS_LOAD; }

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
    jpt_gate_save_failed();
    guest_call(APP_SAVE_FILE, APP, args, 2, 0, &written);
    guest_call(GAME_WND_RESUME, v->top, &resume, 1, 0, NULL);
    if (written && !jpt_gate_save_failed()) {
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

void ce_gameplay_ui_loop(void) {
    qs_request_t request = s_request;
    ce_gameplay_step();
    if (request == QS_NONE) return;
    s_request = QS_NONE;
    run_request(request);
}
