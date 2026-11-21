/* The input context of the game, read from its window stack over a fake guest memory laid out as the retail game lays it out
 * (main window object -> window manager -> stack of windows, each starting with its vtable): the video, the main menu, the
 * loader, the level, the level paused by the game's own window, and a stack the game has not built yet. The same model is
 * read by the Windows host from process memory and by the Linux host from the guest window; here it is read through a
 * function over an array. Then the decisions that follow from it: the input state, the relative mouse mode and the pointer. No
 * Windows header: the same source runs on Windows and Linux. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "active_profile.h"
#include "guest_ui_model.h"
#include "host_policy.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

#define MAIN_WINDOW_OBJECT 0x0066E010u
#define VT_GAME 0x00645C3Cu
#define VT_OPTIONS_INGAME 0x00646B40u
#define VT_QUIT 0x00646970u
#define VT_MAIN_MENU 0x006466B8u
#define VT_LOADER 0x006467A0u
#define VT_VIDEO 0x00646FC8u
#define VT_CONTROLS 0x00645F38u
#define MAIN_WND 0x01000000u
#define MANAGER 0x01001000u
#define STACK 0x01002000u
#define WINDOW_BASE 0x01003000u

typedef struct {
    uint32_t va, value;
} cell_t;

static cell_t g_mem[64];
static unsigned g_cells;

static void poke(uint32_t va, uint32_t value) {
    for (unsigned i = 0; i < g_cells; i++)
        if (g_mem[i].va == va) {
            g_mem[i].value = value;
            return;
        }
    g_mem[g_cells].va = va, g_mem[g_cells].value = value, g_cells++;
}

static int read32(void* context, uint32_t va, uint32_t* out) {
    (void)context;
    for (unsigned i = 0; i < g_cells; i++)
        if (g_mem[i].va == va) {
            *out = g_mem[i].value;
            return 1;
        }
    return 0;
}

/* A stack of windows given by vtable, the last one in front; `stopped` is CGameWnd's +0x30 flag. */
static void build_stack(const uint32_t* vtables, unsigned count, int stopped) {
    g_cells = 0;
    poke(MAIN_WINDOW_OBJECT, MAIN_WND);
    poke(MAIN_WND + 8, MANAGER);
    poke(MANAGER + 0x38, STACK);
    poke(MANAGER + 0x3C, STACK + 4 * count);
    for (unsigned i = 0; i < count; i++) {
        uint32_t window = WINDOW_BASE + 0x100u * i;
        poke(STACK + 4 * i, window);
        poke(window, vtables[i]);
        if (vtables[i] == VT_GAME) poke(window + 0x30, (uint32_t)stopped);
    }
}

static guest_ui_view_t probe(int* ok) {
    guest_ui_view_t v;
    *ok = guest_ui_model_probe(jpt_gva_canonical, read32, NULL, &v);
    return v;
}

static input_state_t state_of(const guest_ui_view_t* v, int focused) {
    input_signals_t signals;
    memset(&signals, 0, sizeof signals);
    signals.focused = focused;
    signals.ui = v->ui;
    return input_state_from(&signals);
}

static void expect(const char* name, const uint32_t* stack, unsigned count, int stopped, guest_ui_t ui, guest_wnd_t kind, input_state_t focused_state) {
    int ok;
    guest_ui_view_t v;
    build_stack(stack, count, stopped);
    v = probe(&ok);
    CHECK(ok && v.ui == ui && v.kind == kind, "%s: the game shows ui %d, front window kind %d (read %d: ui %d, kind %d)", name, (int)ui, (int)kind, ok, (int)v.ui, (int)v.kind);
    CHECK(v.depth == count, "%s: the stack is %u deep (%u)", name, count, v.depth);
    CHECK(state_of(&v, 1) == focused_state, "%s: focused, the input state is %s (%s)", name, input_state_name(focused_state), input_state_name(state_of(&v, 1)));
}

static void test_the_game_windows(void) {
    static const uint32_t video[] = { VT_VIDEO };
    static const uint32_t menu[] = { VT_MAIN_MENU };
    static const uint32_t loader[] = { VT_MAIN_MENU, VT_LOADER };
    static const uint32_t level[] = { VT_GAME };
    static const uint32_t paused[] = { VT_GAME, VT_OPTIONS_INGAME };
    static const uint32_t quit[] = { VT_GAME, VT_OPTIONS_INGAME, VT_QUIT };
    static const uint32_t controls[] = { VT_MAIN_MENU, VT_CONTROLS };
    expect("the intro video", video, 1, 0, GUEST_UI_MENU, GUEST_WND_VIDEO, INPUT_MENU);
    expect("the main menu", menu, 1, 0, GUEST_UI_MENU, GUEST_WND_MAIN_MENU, INPUT_MENU);
    expect("the loading screen", loader, 2, 0, GUEST_UI_MENU, GUEST_WND_LOADER, INPUT_MENU);
    expect("a window of the main menu", controls, 2, 0, GUEST_UI_MENU, GUEST_WND_OTHER, INPUT_MENU);
    expect("the level", level, 1, 0, GUEST_UI_GAMEPLAY, GUEST_WND_GAME, INPUT_GAMEPLAY_CAPTURED);
    expect("the level paused (the game's options in front)", paused, 2, 0, GUEST_UI_DIALOG, GUEST_WND_INGAME_OPTIONS, INPUT_DIALOG);
    expect("the quit question over the options", quit, 3, 0, GUEST_UI_DIALOG, GUEST_WND_QUIT, INPUT_DIALOG);
    expect("the level stopped by the game", level, 1, 1, GUEST_UI_DIALOG, GUEST_WND_GAME, INPUT_DIALOG);
}

static void test_focus_and_shutdown(void) {
    static const uint32_t level[] = { VT_GAME };
    int ok;
    guest_ui_view_t v;
    input_signals_t signals;
    build_stack(level, 1, 0);
    v = probe(&ok);
    CHECK(state_of(&v, 0) == INPUT_GAMEPLAY_RELEASED, "the level without focus releases the pointer (%s)", input_state_name(state_of(&v, 0)));
    CHECK(input_captured(INPUT_GAMEPLAY_CAPTURED) && !input_captured(INPUT_GAMEPLAY_RELEASED) && !input_captured(INPUT_MENU) && !input_captured(INPUT_DIALOG),
          "only the focused level takes relative mouse mode");
    CHECK(input_hide_pointer(INPUT_GAMEPLAY_CAPTURED, 0) && !input_hide_pointer(INPUT_MENU, 0) && input_hide_pointer(INPUT_MENU, 1) && !input_hide_pointer(INPUT_DIALOG, 0) &&
              input_hide_pointer(INPUT_DIALOG, 1) && !input_hide_pointer(INPUT_GAMEPLAY_RELEASED, 1),
          "the pointer is hidden in the level, and in a menu or dialog only when the game hides its own cursor");
    CHECK(input_cursor_virtual(INPUT_GAMEPLAY_CAPTURED) && input_cursor_virtual(INPUT_GAMEPLAY_RELEASED) && !input_cursor_virtual(INPUT_MENU) && !input_cursor_virtual(INPUT_DIALOG),
          "the game reads a cursor it re-centres itself in the level; menus and dialogs follow the real pointer");
    memset(&signals, 0, sizeof signals);
    signals.focused = 1, signals.ui = GUEST_UI_GAMEPLAY, signals.shutdown = 1;
    CHECK(input_state_from(&signals) == INPUT_SHUTDOWN, "a quit request wins over the level");
}

static void test_stacks_that_are_not_built(void) {
    static const uint32_t level[] = { VT_GAME };
    int ok;
    guest_ui_view_t v;
    g_cells = 0;
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE && v.kind == GUEST_WND_NONE, "before the game has a main window nothing is read");
    CHECK(state_of(&v, 1) == INPUT_MENU, "and no stack is a menu, the free pointer: the game never loses its pointer while it starts");
    build_stack(level, 1, 0);
    poke(MANAGER + 0x3C, STACK);
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE, "an empty stack reads as none");
    build_stack(level, 1, 0);
    poke(MANAGER + 0x3C, STACK + 4 * 40);
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE, "a stack deeper than any window stack of the game reads as none");
    build_stack(level, 1, 0);
    poke(MANAGER + 0x3C, STACK + 6);
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE, "a stack that is not whole 32-bit entries reads as none");
    build_stack(level, 1, 0);
    poke(STACK, 0);
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE, "a null window in front reads as none");
    build_stack(level, 1, 0);
    g_cells--;
    v = probe(&ok);
    CHECK(!ok && v.ui == GUEST_UI_NONE, "a window whose flags cannot be read reads as none");
}

static void test_names(void) {
    CHECK(!strcmp(guest_ui_model_class_name(jpt_gva_canonical, VT_VIDEO), "CVideoWnd") && !strcmp(guest_ui_model_class_name(jpt_gva_canonical, VT_GAME), "CGameWnd") &&
              !strcmp(guest_ui_model_class_name(jpt_gva_canonical, 0x1234u), "a window of the game"),
          "the front window is named in the log");
    CHECK(guest_ui_model_kind(jpt_gva_canonical, VT_LOADER) == GUEST_WND_LOADER && guest_ui_model_kind(jpt_gva_canonical, 0x1234u) == GUEST_WND_OTHER,
          "an unknown window is another window of the game, never gameplay");
}

static void test_pointer_hand_over(void) {
    long x = 10, y = 10;
    CHECK(pointer_recapture(INPUT_MENU, INPUT_GAMEPLAY_CAPTURED, &x, &y, 512, 384) && x == 256 && y == 192,
          "a menu becoming the level puts the virtual cursor at the centre the game re-centres to (%ld, %ld)", x, y);
    x = 10, y = 10;
    CHECK(!pointer_recapture(INPUT_GAMEPLAY_CAPTURED, INPUT_DIALOG, &x, &y, 512, 384) && x == 10 && y == 10, "the level opening a dialog leaves the cursor where it is");
    x = 100, y = 100;
    CHECK(pointer_add_relative(INPUT_GAMEPLAY_CAPTURED, &x, &y, 5, -7) && x == 105 && y == 93, "relative motion moves the virtual cursor in the level (%ld, %ld)", x, y);
    x = 100, y = 100;
    CHECK(!pointer_add_relative(INPUT_MENU, &x, &y, 5, -7) && x == 100 && y == 100, "and nowhere else");
}

int main(void) {
    test_the_game_windows();
    test_focus_and_shutdown();
    test_stacks_that_are_not_built();
    test_names();
    test_pointer_hand_over();
    printf("input_context_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
