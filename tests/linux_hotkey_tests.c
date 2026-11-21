/* The port's hotkeys on Linux: key events arriving from the window backend go through the shared table (key_policy) before the game
 * sees them. F11 swaps windowed and fullscreen, F6 the frame counter drawn over the picture, F7 and F8 record a Quick Save or Quick
 * Load that runs on the game at the next frame (the same decisions as the Windows host, over a fake of the game's calls here), the
 * keys whose panel has no Linux renderer yet are consumed, and the game's own keys reach it. trespasser.ini sets the frame counter, the
 * Quick keys and the mouse sensitivity. Headless: the virtual window backend, no guest, no sound. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "active_profile.h"
#include "jpt_config.h"
#include "guest_va.h"
#include "linux_guest_ui.h"
#include "linux_hostkeys.h"
#include "linux_mouse.h"
#include "linux_overlay.h"
#include "linux_quick.h"
#include "linux_screen.h"
#include "linux_settings.h"
#include "linux_spine.h"
#include "linux_user32.h"
#include "linux_win32_harness.h"

#define WS_STYLE 0x90080000u
#define VA(address) (jpt_gva_canonical[GVA_ID(address)])
enum { VK_F1 = 0x70, VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77, VK_F10 = 0x79, VK_F11 = 0x7A, WM_KEYDOWN_ = 0x100 };

static uint32_t s_msg;

static void create_main_window(void) {
    uint32_t wc[10] = { 8, FAKE_WNDPROC, 0, 0, 0x00400000u, 0, 0, 0, 0, 0 };
    uint32_t name = put_string(g_scratch, "DWI - Trespasser");
    wc[9] = name;
    gp_copy_out(g_scratch + 0x100, wc, sizeof wc);
    U32(RegisterClassA, 1, g_scratch + 0x100);
    U32(CreateWindowExA, 12, 0u, name, put_string(g_scratch + 0x200, "Trespasser"), WS_STYLE, 0u, 0u, 640u, 480u, 0u, 0u, 0x00400000u, 0u);
    s_msg = g_scratch + 0x800;
}

static unsigned pump_count(unsigned message, uint32_t wparam) {
    unsigned n = 0;
    for (int i = 0; i < 64 && U32(PeekMessageA, 5, s_msg, 0u, 0u, 0u, 1u) == 1; i++)
        if (rd(s_msg + 4) == message && (!wparam || rd(s_msg + 8) == wparam)) n++;
    return n;
}

static void key(unsigned vk, int down, int repeat) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.scancode = vk, ev.down = down, ev.repeat = repeat;
    lx_virtual_queue_event(&ev);
}

static void tap(unsigned vk) {
    key(vk, 1, 0);
    key(vk, 0, 0);
}

/* ---- a fake of the game's calls for the quick actions ---- */

enum { SCRATCH = 0x00200000, PLAYER_OBJ = 0x00300000, PLAYER_VTABLE = 0x00310000, PLAYER_DEAD_FN = 0x00320000, WORLD_OBJ = 0x00330000 };
static uint8_t s_memory[0x1000];
static struct {
    uint32_t va;
    int convention;
    uint32_t arg0;
} s_calls[32];
static unsigned s_call_count;
static int s_player_dead, s_world_pending, s_file_exists, s_header_ok, s_written = 1;
static int32_t s_load_result;
static char s_notice[96];
static char s_save_dir[] = "C:\\game\\saves\\";

static int fake_read32(uint32_t va, uint32_t* out) {
    if (va == VA(0x0066EF64)) *out = PLAYER_OBJ;
    else if (va == PLAYER_OBJ) *out = PLAYER_VTABLE;
    else if (va == PLAYER_VTABLE + 0x1FCu) *out = PLAYER_DEAD_FN;
    else if (va == VA(0x0073F444)) *out = WORLD_OBJ;
    else if (va == WORLD_OBJ + 0x38u) *out = (uint32_t)s_world_pending;
    else return 0;
    return 1;
}

static uint32_t fake_call(uint32_t va, int convention, int argc, const uint32_t* args, uint32_t this_ecx) {
    (void)this_ecx;
    if (s_call_count < 32) s_calls[s_call_count].va = va, s_calls[s_call_count].convention = convention, s_calls[s_call_count].arg0 = argc ? args[0] : 0, s_call_count++;
    if (va == PLAYER_DEAD_FN) return (uint32_t)s_player_dead;
    if (va == VA(0x00411B30)) {
        strcpy((char*)s_memory + (args[1] - SCRATCH), s_save_dir);
        return 1;
    }
    if (va == VA(0x00414570)) return (uint32_t)s_written;
    if (va == VA(0x00410420)) return (uint32_t)s_header_ok;
    if (va == VA(0x004142C0)) return (uint32_t)s_load_result;
    return 0;
}

static int fake_put_text(uint32_t va, const char* text) {
    strcpy((char*)s_memory + (va - SCRATCH), text);
    return 1;
}

static int fake_get_text(uint32_t va, char* out, unsigned capacity) {
    strncpy(out, (const char*)s_memory + (va - SCRATCH), capacity - 1u);
    out[capacity - 1u] = 0;
    return 1;
}

static int fake_zero(uint32_t va, unsigned bytes) {
    memset(s_memory + (va - SCRATCH), 0, bytes);
    return 1;
}

static int fake_file_exists(const char* path) {
    (void)path;
    return s_file_exists;
}

static void fake_notice(const char* text) {
    strncpy(s_notice, text, sizeof s_notice - 1);
    s_notice[sizeof s_notice - 1] = 0;
}

static lx_quick_ops_t s_ops = { fake_read32, fake_call, fake_put_text, fake_get_text, fake_zero, fake_file_exists, SCRATCH, fake_notice };

static guest_ui_view_t s_view;

static int fake_probe(guest_ui_view_t* out) {
    *out = s_view;
    return 1;
}

static void in_the_level(void) {
    memset(&s_view, 0, sizeof s_view);
    s_view.ui = GUEST_UI_GAMEPLAY, s_view.kind = GUEST_WND_GAME, s_view.top = 0x00400100u, s_view.top_vtable = VA(0x00645C3C), s_view.depth = 1;
}

static void in_the_main_menu(void) {
    memset(&s_view, 0, sizeof s_view);
    s_view.ui = GUEST_UI_MENU, s_view.kind = GUEST_WND_MAIN_MENU, s_view.top = 0x00400200u, s_view.top_vtable = VA(0x006466B8), s_view.depth = 1;
}

static int called(uint32_t va) {
    for (unsigned i = 0; i < s_call_count; i++)
        if (s_calls[i].va == va) return 1;
    return 0;
}

static int order(uint32_t first, uint32_t second) {
    int a = -1, b = -1;
    for (unsigned i = 0; i < s_call_count; i++) {
        if (s_calls[i].va == first && a < 0) a = (int)i;
        if (s_calls[i].va == second) b = (int)i;
    }
    return a >= 0 && b > a;
}

static void begin(int save_request, int level) {
    s_call_count = 0;
    s_notice[0] = 0;
    s_player_dead = s_world_pending = 0;
    s_file_exists = 1, s_header_ok = 1, s_written = 1, s_load_result = 0;
    if (level) in_the_level();
    else in_the_main_menu();
    lx_quick_request(save_request);
}

static void test_fullscreen_and_keys_of_the_game(void) {
    const lx_virtual_state_t* v = lx_virtual_state();
    unsigned reaching;
    tap(VK_F11);
    reaching = pump_count(WM_KEYDOWN_, VK_F11);
    CHECK(v->fullscreen_toggles == 1 && reaching == 0, "F11 swaps the window to fullscreen once and is never a key of the game (%u toggles, %u delivered)", v->fullscreen_toggles, reaching);
    key(VK_F11, 1, 1);
    pump_count(0, 0);
    CHECK(v->fullscreen_toggles == 1, "a held F11 does not toggle again");
    tap('W');
    CHECK(pump_count(WM_KEYDOWN_, 'W') == 1, "a letter reaches the game");
    tap(0x1B);
    CHECK(pump_count(WM_KEYDOWN_, 0x1B) == 1, "Esc reaches the game");
    tap(VK_F1);
    CHECK(pump_count(WM_KEYDOWN_, VK_F1) == 1, "and F1");
}

static void test_keys_without_a_renderer(void) {
    const lx_hostkeys_counts_t* c = lx_hostkeys_counts();
    unsigned before = c->unavailable, reaching = 0;
    tap(VK_F3);
    tap(VK_F5);
    tap(VK_F10);
    tap(VK_F4);
    reaching += pump_count(WM_KEYDOWN_, VK_F3) + pump_count(WM_KEYDOWN_, VK_F5) + pump_count(WM_KEYDOWN_, VK_F10) + pump_count(WM_KEYDOWN_, VK_F4);
    CHECK(c->unavailable - before == 4 && reaching == 0, "F3, F5, F10 and F4 are the port's keys: consumed, the game never sees them (%u consumed, %u delivered)", c->unavailable - before, reaching);
}

static void test_frame_counter(void) {
    char text[32];
    lx_frame_t in, out;
    static uint16_t pixels[640 * 480];
    unsigned changed = 0;
    CHECK(!lx_overlay_frame_counter_on(), "the frame counter starts off");
    tap(VK_F6);
    pump_count(0, 0);
    CHECK(lx_overlay_frame_counter_on() && lx_hostkeys_counts()->frame_counter == 1, "F6 switches it on");
    for (uint64_t t = 0; t <= 1100; t += 20) lx_overlay_frame_presented(t);
    CHECK(lx_overlay_frame_counter_text(text, sizeof text) && !strncmp(text, "FPS: ", 5), "its text is the shared one, from the measured rate (%s)", text);
    CHECK(!strncmp(text, "FPS: 5", 6), "and the rate is the frames presented per second: one every 20 ms is about 50 (%s)", text);
    memset(&in, 0, sizeof in);
    in.pixels = pixels, in.width = 640, in.height = 480, in.pitch = 1280, in.format = LX_PF_RGB565;
    CHECK(lx_overlay_compose(&in, &out, 1100) && out.format == LX_PF_XRGB8888 && out.width == 640 && out.pixels != in.pixels, "the presented frame is a copy with the counter drawn in");
    for (int y = 0; y < 40; y++)
        for (int x = 440; x < 640; x++) changed += ((const uint32_t*)out.pixels)[y * 640 + x] != 0;
    CHECK(changed > 100, "in the top right corner (%u pixels differ from the black frame)", changed);
    CHECK(pixels[0] == 0 && pixels[640 * 479] == 0, "and the game's own surface is untouched");
    tap(VK_F6);
    pump_count(0, 0);
    CHECK(!lx_overlay_frame_counter_on() && !lx_overlay_compose(&in, &out, 1100), "F6 again switches it off and nothing is drawn");
}

static void test_quick_save(void) {
    lx_hostkeys_set_quick_keys(VK_F7, VK_F8);
    begin(1, 1);
    lx_quick_pump(&s_ops);
    CHECK(called(VA(0x00411B30)) && order(VA(0x0040BCC0), VA(0x00414570)) && order(VA(0x00414570), VA(0x0040BD70)), "Quick Save asks the game for its saves folder, pauses the level, writes, and resumes it");
    CHECK(strstr((char*)s_memory, "C:\\game\\saves\\SAVEGAME.QCK") != NULL, "the file is SAVEGAME.QCK in that folder");
    CHECK(!called(VA(0x00410420)) && !called(VA(0x004142C0)), "a save does not touch the game's loader");

    begin(1, 1);
    s_player_dead = 1;
    lx_quick_pump(&s_ops);
    CHECK(!called(VA(0x00414570)) && s_notice[0], "a dead player cannot save: refused with a notice (%s)", s_notice);
    begin(1, 1);
    s_world_pending = 1;
    lx_quick_pump(&s_ops);
    CHECK(!called(VA(0x00414570)), "a level change under way refuses it");
    begin(1, 0);
    lx_quick_pump(&s_ops);
    CHECK(!called(VA(0x00414570)), "and so does the main menu, where there is nothing to save");
    begin(1, 1);
    s_written = 0;
    lx_quick_pump(&s_ops);
    CHECK(called(VA(0x00414570)) && s_notice[0], "a save the game cannot write says so (%s)", s_notice);
    CHECK(!lx_quick_pending(), "and every request is taken once");
}

static void test_quick_load(void) {
    begin(0, 1);
    lx_quick_pump(&s_ops);
    CHECK(order(VA(0x0040BCC0), VA(0x004142C0)) && order(VA(0x004142C0), VA(0x0040BD70)), "Quick Load pauses the level, loads the save, and resumes");
    begin(0, 0);
    lx_quick_pump(&s_ops);
    CHECK(called(VA(0x004142C0)) && !called(VA(0x0040BCC0)), "from the main menu it loads without pausing a level");
    begin(0, 1);
    s_file_exists = 0;
    lx_quick_pump(&s_ops);
    CHECK(!called(VA(0x004142C0)) && s_notice[0], "no Quick Save yet: nothing is loaded and the player is told (%s)", s_notice);
    begin(0, 1);
    s_header_ok = 0;
    lx_quick_pump(&s_ops);
    CHECK(!called(VA(0x004142C0)), "a file the game cannot read is not loaded");
    begin(0, 1);
    s_load_result = -5;
    lx_quick_pump(&s_ops);
    CHECK(called(VA(0x00413160)), "a load the game refuses goes through the game's own failure path");
    begin(0, 1);
    memset(&s_view, 0, sizeof s_view);
    s_view.ui = GUEST_UI_DIALOG, s_view.kind = GUEST_WND_INGAME_OPTIONS, s_view.depth = 2;
    lx_quick_pump(&s_ops);
    CHECK(s_call_count == 0, "with the game's options in front neither works and the game is not called");
}

static void test_the_keys_record_requests(void) {
    const lx_hostkeys_counts_t* c = lx_hostkeys_counts();
    unsigned reaching;
    lx_quick_reset();
    tap(VK_F7);
    reaching = pump_count(WM_KEYDOWN_, VK_F7);
    CHECK(lx_quick_pending() && c->quick_save == 1 && reaching == 0, "F7 records a Quick Save for the next frame and the game does not see the key");
    lx_quick_reset();
    tap(VK_F8);
    pump_count(0, 0);
    CHECK(lx_quick_pending() && c->quick_load == 1, "F8 records a Quick Load");
    key(VK_F8, 1, 1);
    pump_count(0, 0);
    CHECK(c->quick_load == 1, "a held key records nothing more");
    lx_quick_reset();
}

static void test_settings(void) {
    jpt_config_t cfg;
    char path[256];
    FILE* f;
    snprintf(path, sizeof path, "/tmp/jpt_linux_hotkey_tests_%d.ini", (int)getpid());
    f = fopen(path, "w");
    if (!f) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(f, "%s=1\n%s=2.00\n%s=F9\n%s=Insert\n", JPT_KEY_SHOW_FRAME_COUNTER, JPT_KEY_MOUSE_SENSITIVITY, JPT_KEY_QUICK_SAVE, JPT_KEY_QUICK_LOAD);
    fclose(f);
    lx_settings_load(path, &cfg);
    remove(path);
    CHECK(lx_overlay_frame_counter_on(), "trespasser.ini: the frame counter starts on when the launcher said so");
    CHECK(cfg.mouse_sensitivity == 2.0f, "the mouse sensitivity is read (%.2f)", (double)cfg.mouse_sensitivity);
    CHECK(key_policy(0x100, 0x78, 0, 0, 0) == KEY_HOST_QUICK_SAVE && key_policy(0x100, 0x2D, 0, 0, 0) == KEY_HOST_QUICK_LOAD, "and the Quick keys are the ones it names (F9 and Insert)");
    lx_settings_load("/nonexistent/trespasser.ini", &cfg);
    CHECK(!lx_overlay_frame_counter_on() && key_policy(0x100, VK_F7, 0, 0, 0) == KEY_HOST_QUICK_SAVE && cfg.mouse_sensitivity == 1.0f,
          "a missing file means the defaults: counter off, F7 and F8, sensitivity 1.00");
}

int main(void) {
    if (!harness_start()) return 2;
    create_main_window();
    lx_guest_ui_set_probe(fake_probe);
    in_the_level();
    lx_settings_load(NULL, &(jpt_config_t){ 0 });
    test_fullscreen_and_keys_of_the_game();
    test_keys_without_a_renderer();
    test_frame_counter();
    test_quick_save();
    test_quick_load();
    test_the_keys_record_requests();
    test_settings();
    lx_guest_ui_set_probe(NULL);
    harness_stop();
    printf("linux_hotkey_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
