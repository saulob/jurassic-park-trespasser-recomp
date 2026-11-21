#include "linux_quick.h"

#include <string.h>

#include "active_profile.h"
#include "ce_policy.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_va.h"
#include "jpt_sync.h"
#include "jpt_tr.h"
#include "jpt_vpath.h"
#include "linux_guest_ui.h"
#include "linux_hle.h"
#include "linux_log.h"
#include "linux_overlay.h"
#include "linux_spine.h"
#include "linux_window.h"

#define VA(address) (jpt_gva_canonical[GVA_ID(address)])
#define APP VA(0x0066E338)
#define WORLD VA(0x0073F444)
#define PLAYER VA(0x0066EF64)
#define GAME_WND_PAUSE VA(0x0040BCC0)
#define GAME_WND_RESUME VA(0x0040BD70)
#define SAVE_DIR VA(0x00411B30)
#define APP_SAVE_FILE VA(0x00414570)
#define READ_SAVE_HEADER VA(0x00410420)
#define APP_MENU_SOUNDS_OFF VA(0x00414D80)
#define APP_LOAD_SAVE VA(0x004142C0)
#define APP_LOAD_FAILED VA(0x00413160)
#define WORLD_LOAD_PENDING 0x38u
#define PLAYER_DEAD_SLOT 0x1FCu
#define SAVE_DIR_SAVES 1u
#define SAVE_ENTRY_BYTES 0x54u
#define SAVE_ENTRY_SCENE 0x37u
#define MAX_PATH_BYTES 260u

typedef enum { REQUEST_NONE, REQUEST_SAVE, REQUEST_LOAD } request_t;

static request_t s_request;

void lx_quick_reset(void) { s_request = REQUEST_NONE; }

void lx_quick_request(int save) { s_request = save ? REQUEST_SAVE : REQUEST_LOAD; }

int lx_quick_pending(void) { return s_request != REQUEST_NONE; }

static void say(const lx_quick_ops_t* ops, const char* key) {
    if (ops->notice) ops->notice(jpt_tr(key));
}

static int player_alive(const lx_quick_ops_t* ops) {
    uint32_t player, vtable, fn, dead;
    if (!ops->read32(PLAYER, &player) || !player || !ops->read32(player, &vtable) || !ops->read32(vtable + PLAYER_DEAD_SLOT, &fn) || !fn) return 0;
    dead = ops->call(fn, LX_THISCALL, 0, NULL, player);
    return !(dead & 0xFFu);
}

static int load_pending(const lx_quick_ops_t* ops) {
    uint32_t world, v;
    return ops->read32(WORLD, &world) && world && ops->read32(world + WORLD_LOAD_PENDING, &v) && (v & 0xFFu);
}

static qs_place_t place_of(const guest_ui_view_t* v) {
    if (v->depth != 1) return QS_PLACE_NONE;
    if (v->kind == GUEST_WND_GAME && v->ui == GUEST_UI_GAMEPLAY) return QS_PLACE_LEVEL;
    if (v->kind == GUEST_WND_MAIN_MENU) return QS_PLACE_MAIN_MENU;
    return QS_PLACE_NONE;
}

static int quick_path(const lx_quick_ops_t* ops, char* path, size_t cap) {
    uint32_t args[3];
    char dir[MAX_PATH_BYTES];
    args[0] = SAVE_DIR_SAVES, args[1] = ops->scratch, args[2] = MAX_PATH_BYTES;
    ops->put_text(ops->scratch, "");
    ops->call(SAVE_DIR, LX_CDECL, 3, args, 0);
    return ops->get_text(ops->scratch, dir, sizeof dir) && qs_path(dir, path, cap);
}

static void quick_save(const lx_quick_ops_t* ops, const guest_ui_view_t* v, const char* path) {
    char name[QS_NAME_MAX + 1];
    uint32_t args[2], resume = 1, written;
    qs_display_name(jpt_tr_cp("Quick Save"), name, sizeof name);
    ops->put_text(ops->scratch, path);
    ops->put_text(ops->scratch + MAX_PATH_BYTES, name);
    args[0] = ops->scratch, args[1] = ops->scratch + MAX_PATH_BYTES;
    ops->call(GAME_WND_PAUSE, LX_THISCALL, 0, NULL, v->top);
    written = ops->call(APP_SAVE_FILE, LX_THISCALL, 2, args, APP);
    ops->call(GAME_WND_RESUME, LX_THISCALL, 1, &resume, v->top);
    if (written) {
        lx_log("quick-save", "saved %s", path);
        say(ops, "Quick saved");
    } else {
        lx_log("quick-save", "the game could not write %s", path);
        say(ops, "Quick Save failed");
    }
}

static void quick_load(const lx_quick_ops_t* ops, const guest_ui_view_t* v, qs_place_t place, const char* path) {
    uint32_t entry = ops->scratch + MAX_PATH_BYTES, args[2], result, resume = 1;
    ops->put_text(ops->scratch, path);
    ops->zero(entry, SAVE_ENTRY_BYTES);
    args[0] = ops->scratch, args[1] = entry;
    if (!(ops->call(READ_SAVE_HEADER, LX_CDECL, 2, args, 0) & 0xFFu)) {
        lx_log("quick-save", "%s is not a save the game can read: nothing was loaded", path);
        say(ops, "Quick Load failed");
        return;
    }
    args[1] = entry + SAVE_ENTRY_SCENE;
    if (place == QS_PLACE_LEVEL) ops->call(GAME_WND_PAUSE, LX_THISCALL, 0, NULL, v->top);
    ops->call(APP_MENU_SOUNDS_OFF, LX_THISCALL, 0, NULL, APP);
    result = ops->call(APP_LOAD_SAVE, LX_THISCALL, 2, args, APP);
    if ((int32_t)result < 0) {
        lx_log("quick-save", "the game could not load %s (%d)", path, (int)(int32_t)result);
        ops->call(APP_LOAD_FAILED, LX_CDECL, 1, &result, 0);
    } else {
        lx_log("quick-save", "loaded %s", path);
    }
    if (place == QS_PLACE_LEVEL) ops->call(GAME_WND_RESUME, LX_THISCALL, 1, &resume, v->top);
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

void lx_quick_pump(const lx_quick_ops_t* ops) {
    guest_ui_view_t v;
    qs_world_t w;
    qs_decision_t d;
    char path[MAX_PATH_BYTES];
    request_t request = s_request;
    const char* text;
    if (request == REQUEST_NONE || !ops) return;
    s_request = REQUEST_NONE;
    memset(&w, 0, sizeof w);
    lx_guest_ui_probe(&v);
    w.place = place_of(&v);
    if (w.place == QS_PLACE_NONE) {
        lx_log("quick-save", "Quick %s ignored: no level or main menu in front (%s)", request == REQUEST_SAVE ? "Save" : "Load",
               guest_ui_model_class_name(jpt_gva_canonical, v.top_vtable));
        return;
    }
    if (!quick_path(ops, path, sizeof path)) {
        lx_log("quick-save", "the game's saves folder could not be read");
        return;
    }
    w.player_alive = w.place == QS_PLACE_LEVEL && player_alive(ops);
    w.load_pending = load_pending(ops);
    w.file_exists = ops->file_exists(path);
    d = request == REQUEST_SAVE ? qs_decide_save(&w) : qs_decide_load(&w);
    if (d == QS_DO_SAVE) quick_save(ops, &v, path);
    else if (d == QS_DO_LOAD_LEVEL || d == QS_DO_LOAD_MAIN_MENU) quick_load(ops, &v, w.place, path);
    else {
        text = refusal(d, request == REQUEST_SAVE);
        lx_log("quick-save", "Quick %s refused: %s", request == REQUEST_SAVE ? "Save" : "Load", text ? text : "not here");
        if (text) say(ops, text);
    }
}

static int guest_read32(uint32_t va, uint32_t* out) { return gp_read_u32(va, out); }

static uint32_t guest_call(uint32_t va, int convention, int argc, const uint32_t* args, uint32_t this_ecx) {
    return lx_call_guest(va, convention, argc, args, this_ecx, NULL);
}

static int guest_put_text(uint32_t va, const char* text) { return gp_copy_out(va, text, (uint32_t)strlen(text) + 1u); }

static int guest_get_text(uint32_t va, char* out, unsigned capacity) {
    const char* s = gp_string(va, capacity);
    if (!s) return 0;
    strncpy(out, s, capacity - 1u);
    out[capacity - 1u] = 0;
    return 1;
}

static int guest_zero(uint32_t va, unsigned bytes) {
    static const uint8_t zeros[SAVE_ENTRY_BYTES];
    return bytes <= sizeof zeros && gp_copy_out(va, zeros, bytes);
}

static int guest_file_exists(const char* guest_path) {
    char host[1024];
    return jpt_vpath_resolve(hle_vpath(), guest_path, 0, host, sizeof host) == JPT_VP_OK;
}

static void guest_notice(const char* text) { lx_overlay_notice(text, jpt_time_ms()); }

const lx_quick_ops_t* lx_quick_guest_ops(void) {
    static lx_quick_ops_t ops = { guest_read32, guest_call, guest_put_text, guest_get_text, guest_zero, guest_file_exists, 0, guest_notice };
    if (!ops.scratch) ops.scratch = gm_alloc_pages(0, LX_QUICK_SCRATCH, "quick save");
    return ops.scratch ? &ops : NULL;
}
