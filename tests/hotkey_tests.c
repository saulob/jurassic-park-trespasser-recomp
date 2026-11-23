/* The port's hotkey table (key_policy in host_policy.c), the one both hosts read: which keys are the port's and never reach the
 * game, which reach it, what auto-repeat and key-up do, how Ctrl and Alt change a key, and how the Quick Save and Quick Load keys
 * of trespasser.ini win over every other use of the same key. No Windows header: the same source runs on Windows and Linux. */
#include <stdio.h>
#include <string.h>

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

enum { DOWN = 0x100, UP = 0x101, SYSDOWN = 0x104, SYSUP = 0x105 };
enum { VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12 };

typedef struct {
    unsigned vk;
    key_action_t action;
    const char* what;
} entry_t;

static void test_the_table(void) {
    static const entry_t table[] = {
        { VK_F2, KEY_HOST_CONTROLS, "F2 the controls card" },
        { VK_F3, KEY_HOST_OBJECTIVES, "F3 Objectives" },
        { VK_F4, KEY_HOST_SHOW_OBJECTIVE, "F4 Show Objective" },
        { VK_F5, KEY_HOST_MINIMAP, "F5 the minimap" },
        { VK_F6, KEY_HOST_FRAME_COUNTER, "F6 the frame counter" },
        { VK_F9, KEY_HOST_SNAPSHOT, "F9 the physics snapshot" },
        { VK_F10, KEY_HOST_CHEATS, "F10 Cheats" },
        { VK_F11, KEY_HOST_FULLSCREEN, "F11 fullscreen" },
    };
    key_policy_set_quick_keys(0, 0);
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++) {
        const entry_t* e = &table[i];
        CHECK(key_policy(DOWN, e->vk, 0, 0, 0) == e->action, "%s: key-down is the port's action (%d)", e->what, (int)key_policy(DOWN, e->vk, 0, 0, 0));
        CHECK(key_policy(DOWN, e->vk, 1, 0, 0) == KEY_DROP, "%s: auto-repeat never repeats the action and never reaches the game", e->what);
        CHECK(key_policy(UP, e->vk, 0, 0, 0) == KEY_DROP, "%s: its key-up is dropped too", e->what);
    }
    CHECK(key_policy(DOWN, VK_F12, 0, 0, 0) == KEY_DROP && key_policy(UP, VK_F12, 0, 0, 0) == KEY_DROP, "F12, the game's controls window, is not passed on");
}

static void test_modifiers(void) {
    CHECK(key_policy(SYSDOWN, VK_F4, 0, 0, 1) == KEY_HOST_CLOSE, "Alt+F4 closes the game");
    CHECK(key_policy(SYSDOWN, VK_F4, 1, 0, 1) == KEY_DROP && key_policy(SYSUP, VK_F4, 0, 0, 1) == KEY_DROP, "its repeat and its key-up are dropped");
    CHECK(key_policy(DOWN, VK_F4, 0, 0, 0) == KEY_HOST_SHOW_OBJECTIVE, "F4 without Alt is Show Objective");
    CHECK(key_policy(DOWN, VK_F11, 0, 1, 0) == KEY_DROP && key_policy(DOWN, VK_F11, 1, 1, 0) == KEY_DROP &&
              key_policy(UP, VK_F11, 0, 1, 0) == KEY_DROP,
          "Ctrl+F11 is dropped");
}

static void test_keys_of_the_game(void) {
    CHECK(key_policy(DOWN, 0x1B, 0, 0, 0) == KEY_TO_GAME && key_policy(DOWN, VK_F1, 0, 0, 0) == KEY_TO_GAME, "Esc and F1 open a window of the game on key-down");
    CHECK(key_policy(DOWN, 0x1B, 1, 0, 0) == KEY_DROP && key_policy(DOWN, VK_F1, 1, 0, 0) == KEY_DROP, "and their auto-repeat would open it again: dropped");
    CHECK(key_policy(UP, 0x1B, 0, 0, 0) == KEY_TO_GAME, "the key-up of Esc reaches the game (it cancels a window)");
    CHECK(key_policy(DOWN, 'W', 0, 0, 0) == KEY_TO_GAME && key_policy(UP, 'W', 0, 0, 0) == KEY_TO_GAME && key_policy(DOWN, 0x20, 1, 0, 0) == KEY_TO_GAME,
          "the game's own keys, repeats included, reach it");
    CHECK(key_policy(0x0102, 'A', 0, 0, 0) == KEY_TO_GAME, "a message that is not a key message passes");
    CHECK(key_opens_game_window(DOWN, 0x1B, 0) && key_opens_game_window(DOWN, VK_F1, 0) && !key_opens_game_window(DOWN, 0x1B, 1) && !key_opens_game_window(UP, 0x1B, 0),
          "Esc and F1 key-downs (without repeat) are the ones that open a window");
}

static void test_quick_keys(void) {
    key_policy_set_quick_keys(VK_F7, VK_F8);
    CHECK(key_policy(DOWN, VK_F7, 0, 0, 0) == KEY_HOST_QUICK_SAVE && key_policy(DOWN, VK_F8, 0, 0, 0) == KEY_HOST_QUICK_LOAD, "F7 is Quick Save and F8 Quick Load");
    CHECK(key_policy(DOWN, VK_F7, 1, 0, 0) == KEY_DROP && key_policy(UP, VK_F7, 0, 0, 0) == KEY_DROP, "a held Quick Save key does not save again, and its key-up is dropped");
    CHECK(key_policy(SYSDOWN, VK_F8, 0, 0, 0) == KEY_DROP, "a Quick key with Alt is not a quick action");
    key_policy_set_quick_keys('Q', 'L');
    CHECK(key_policy(DOWN, 'Q', 0, 0, 0) == KEY_HOST_QUICK_SAVE && key_policy(DOWN, 'L', 0, 0, 0) == KEY_HOST_QUICK_LOAD, "the keys of trespasser.ini replace them");
    CHECK(key_policy(DOWN, VK_F7, 0, 0, 0) == KEY_TO_GAME, "and F7 is the game's again");
    key_policy_set_quick_keys(VK_F6, VK_F3);
    CHECK(key_policy(DOWN, VK_F6, 0, 0, 0) == KEY_HOST_QUICK_SAVE && key_policy(DOWN, VK_F3, 0, 0, 0) == KEY_HOST_QUICK_LOAD,
          "a Quick key wins over the port's other use of the same key (the frame counter, Objectives)");
    key_policy_set_quick_keys(0, 0);
    CHECK(key_policy(DOWN, VK_F6, 0, 0, 0) == KEY_HOST_FRAME_COUNTER, "cleared, the table is as it was");
}

int main(void) {
    test_the_table();
    test_modifiers();
    test_keys_of_the_game();
    test_quick_keys();
    printf("hotkey_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
