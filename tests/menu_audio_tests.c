/* The menus' selection sound, from the retail executable and Menu.tpa read as plain files (nothing runs, nothing plays),
 * and the host's lookup of the file the menus load it from. The retail checks are skipped when the game is not there. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_hash.h"
#include "menu_audio_policy.h"

#ifdef JPT_SOURCE_DIR
#define GAME_DIR JPT_SOURCE_DIR "/game/"
#else
#define GAME_DIR "game/"
#endif

#define IMAGE_BASE 0x400000u
#define TEXT_BEGIN 0x401000u
#define TEXT_END 0x645000u

#define PLAY_SELECTION_SOUND 0x00414EF0u
#define SOUND_STOP 0x0043D850u
#define SOUND_PLAY 0x0043D280u
#define SOUND_LOAD 0x00414D50u
#define SAMPLE_CTOR 0x0043CE40u
#define SOUND_SET_VOLUME 0x0043D7E0u
#define MENU_AUDIO_OPEN 0x004148F0u
#define WINDOW_NOTIFY 0x00415400u
#define SAVE_WINDOW_FULL_NAME 0x00411000u
#define BUTTON_PRESS 0x00403790u
#define BUTTON_RELEASE 0x00403820u
#define BUTTON_MOVE 0x00403600u
#define SAVE_WINDOW_KEY 0x00410FE0u
#define EDITBOX_CHAR 0x004078C0u
#define SELECTION_NAME_VA 0x00661064u
#define INSTALLED_DIRECTORY_VA 0x00660074u
#define MENU_TPA_VA 0x00660F90u
#define SFX_FLAGS 0x006625ACu
#define SOUNDS_LOAD 0x00414980u
#define MUSIC_ON 0x00414E80u
#define AMBIENT_PLAY 0x00414EC0u
#define SOUNDS_FREE 0x00414D80u
#define MAIN_MENU_INIT 0x004154C0u
#define MAIN_MENU_TIMER 0x004155B0u
#define MENU_SAMPLE_NAMES_VA 0x00660F9Cu
#define MISSING_NAME_VA 0x00662728u

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

static uint8_t* g_exe;
static size_t g_exe_size;

static uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc((size_t)n);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* This executable's sections sit at their virtual addresses in the file. */
static const uint8_t* at(uint32_t va) { return va - IMAGE_BASE < g_exe_size ? g_exe + (va - IMAGE_BASE) : NULL; }

static int bytes_at(uint32_t va, const uint8_t* want, size_t n) {
    const uint8_t* p = at(va);
    return p && va - IMAGE_BASE + n <= g_exe_size && !memcmp(p, want, n);
}

static int has_in_range(uint32_t begin, uint32_t end, const uint8_t* want, size_t n) {
    for (uint32_t va = begin; va + n <= end; va++)
        if (bytes_at(va, want, n)) return 1;
    return 0;
}

static int call_target_at(uint32_t va, uint32_t target) {
    const uint8_t* p = at(va);
    return p && p[0] == 0xE8 && va + 5 + rd32(p + 1) == target;
}

static int calls_in(uint32_t target, uint32_t begin, uint32_t end, uint32_t* first) {
    int n = 0;
    for (uint32_t va = begin; va + 5 <= end; va++)
        if (call_target_at(va, target)) {
            if (!n && first) *first = va;
            n++;
        }
    return n;
}

static int call_sites(uint32_t target, uint32_t* out, int max) {
    int n = 0;
    for (uint32_t va = TEXT_BEGIN; va + 5 <= TEXT_END; va++)
        if (call_target_at(va, target)) {
            if (n < max) out[n] = va;
            n++;
        }
    return n;
}

static int aligned_pointers_to(uint32_t target) {
    int n = 0;
    for (uint32_t va = 0x645000; va + 4 <= 0x6A0000 && va - IMAGE_BASE + 4 <= g_exe_size; va += 4)
        if (rd32(at(va)) == target) n++;
    return n;
}

static int mentions(uint32_t begin, uint32_t end, uint32_t value) {
    for (uint32_t va = begin; va + 4 <= end; va++)
        if (rd32(at(va)) == value) return 1;
    return 0;
}

static void retail_executable(void) {
    static const uint8_t notify[] = { 0x83, 0x7C, 0x24, 0x08, 0x01, 0x75, 0x0A, 0xB9, 0x38, 0xE3, 0x66, 0x00 };
    static const uint8_t notify_tail[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00 };
    static const uint8_t full_name[] = { 0xB9, 0x38, 0xE3, 0x66, 0x00 };
    static const uint8_t play_head[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x60, 0x01, 0x00, 0x00, 0x85, 0xC9, 0x74, 0x12 };
    static const uint8_t play_mid[] = { 0x8B, 0x8E, 0x60, 0x01, 0x00, 0x00, 0x6A, 0x00 };
    static const uint8_t press[] = { 0x6A, 0x01, 0x89, 0x5E, 0x28, 0x8B, 0x11, 0x56, 0xFF, 0x52, 0x20 };
    static const uint8_t release[] = { 0x6A, 0x00, 0x89, 0x46, 0x28, 0x8B, 0x11, 0x56, 0xFF, 0x52, 0x20 };
    static const uint8_t notify_call[] = { 0xFF, 0x52, 0x20 };
    static const uint8_t editbox_full[] = { 0x8B, 0x11, 0x56, 0xFF, 0x52, 0x38 };
    static const uint8_t open_menu_tpa[] = { 0x68, 0x04, 0x01, 0x00, 0x00, 0x8D, 0x44, 0x24, 0x14, 0x50, 0x6A, 0x01 };
    uint32_t caller = 0;
    CHECK(g_exe_size > 0x270000 && g_exe[0] == 'M' && g_exe[1] == 'Z', "the retail executable is the expected image");

    CHECK(bytes_at(WINDOW_NOTIFY, notify, sizeof notify) && call_target_at(WINDOW_NOTIFY + 0x0C, PLAY_SELECTION_SOUND) &&
              bytes_at(WINDOW_NOTIFY + 0x11, notify_tail, sizeof notify_tail),
          "a window's notification plays the selection sound only for state 1 and returns 1");
    CHECK(bytes_at(SAVE_WINDOW_FULL_NAME, full_name, sizeof full_name) &&
              call_target_at(SAVE_WINDOW_FULL_NAME + 5, PLAY_SELECTION_SOUND),
          "the Save window plays it when a typed name is full");
    CHECK(bytes_at(PLAY_SELECTION_SOUND, play_head, sizeof play_head) && call_target_at(PLAY_SELECTION_SOUND + 0x0D, SOUND_STOP) &&
              bytes_at(PLAY_SELECTION_SOUND + 0x12, play_mid, sizeof play_mid) &&
              call_target_at(PLAY_SELECTION_SOUND + 0x1A, SOUND_PLAY),
          "one request is one stop and one play of the loaded sample, not looped");

    CHECK(calls_in(PLAY_SELECTION_SOUND, TEXT_BEGIN, TEXT_END, &caller) == 2, "only two places request the sound");
    CHECK(calls_in(WINDOW_NOTIFY, TEXT_BEGIN, TEXT_END, NULL) == 0 && calls_in(SAVE_WINDOW_FULL_NAME, TEXT_BEGIN, TEXT_END, NULL) == 0,
          "the notifications are reached through window tables only");
    CHECK(aligned_pointers_to(WINDOW_NOTIFY) == 19, "nineteen dialog windows share the notification");
    CHECK(aligned_pointers_to(SAVE_WINDOW_FULL_NAME) == 1, "one window has the full-name notification");

    CHECK(has_in_range(BUTTON_PRESS, BUTTON_RELEASE, press, sizeof press), "pressing a button notifies its window with state 1");
    CHECK(has_in_range(BUTTON_RELEASE, 0x004038B0u, release, sizeof release), "releasing a button notifies with state 0");
    CHECK(calls_in(BUTTON_MOVE, TEXT_BEGIN, TEXT_END, NULL) == 0 && has_in_range(BUTTON_MOVE, BUTTON_PRESS, notify_call, sizeof notify_call),
          "pointer movement over a button notifies with its hover state");
    CHECK(has_in_range(EDITBOX_CHAR, EDITBOX_CHAR + 0x60, editbox_full, sizeof editbox_full),
          "a character typed into a full edit box notifies its window");
    CHECK(calls_in(PLAY_SELECTION_SOUND, SAVE_WINDOW_KEY, SAVE_WINDOW_KEY + 0x20, NULL) == 0 &&
              !has_in_range(SAVE_WINDOW_KEY, SAVE_WINDOW_KEY + 0x20, full_name, sizeof full_name),
          "Enter and Esc in the menus do not request the sound");

    CHECK(!mentions(PLAY_SELECTION_SOUND, PLAY_SELECTION_SOUND + 0x20, SFX_FLAGS) && !mentions(SOUND_PLAY, SOUND_PLAY + 0x146, SFX_FLAGS) &&
              !mentions(SOUND_STOP, SOUND_STOP + 0x6F, SFX_FLAGS),
          "the selection sound does not read the Play Sound Effects flag");

    CHECK(!strcmp((const char*)at(SELECTION_NAME_VA), "DINO - TREX FOOT") && call_target_at(SOUND_LOAD + 0x15, 0x0043DA80u) &&
              jpt_sample_id("DINO - TREX FOOT") == 0x15E936B2u,
          "the sample is named DINO - TREX FOOT");
    CHECK(has_in_range(MENU_AUDIO_OPEN + 0x0A, MENU_AUDIO_OPEN + 0x16, open_menu_tpa, sizeof open_menu_tpa) &&
              call_target_at(MENU_AUDIO_OPEN + 0x18, 0x00411B30u) && !strcmp((const char*)at(INSTALLED_DIRECTORY_VA), "Installed Directory") &&
              !strcmp((const char*)at(MENU_TPA_VA), "menu.tpa"),
          "the menus open menu.tpa under the Installed Directory");
}

static int sites_are(uint32_t target, const uint32_t* want, int n) {
    uint32_t got[16];
    if (call_sites(target, got, 16) != n) return 0;
    for (int i = 0; i < n; i++)
        if (got[i] != want[i]) return 0;
    return 1;
}

static void menu_sounds(void) {
    static const uint32_t loader[] = { 0x40C01B, 0x40C0FD, 0x4154D3, 0x4164C3 };
    static const uint32_t selection[] = { 0x40C025, 0x40C107, 0x414D39, 0x4164CD };
    static const uint32_t all_sounds[] = { 0x414E94, 0x4154DD };
    static const uint32_t music[] = { 0x4155D1, 0x416404 };
    static const uint32_t ambient[] = { 0x4155E3 };
    static const uint8_t timer_ids[] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xE8, 0x65, 0x74, 0x25, 0x48, 0x75, 0x2C };
    static const uint8_t set_music_timer[] = { 0x68, 0xD0, 0x07, 0x00, 0x00, 0x6A, 0x66 };
    const uint8_t* names = at(MENU_SAMPLE_NAMES_VA);
    size_t tpa_size = 0;
    uint8_t* tpa = read_file(GAME_DIR "data/Menu.tpa", &tpa_size);
    char list[16][32];
    int count = 0;

    CHECK(sites_are(MENU_AUDIO_OPEN, loader, 4), "menu.tpa is opened by the in-game menu, the Options window and the main menu");
    CHECK(sites_are(SOUND_LOAD, selection, 4), "the selection sound is loaded wherever menu.tpa is opened for a window");
    CHECK(sites_are(SOUNDS_LOAD, all_sounds, 2), "music and ambience are loaded only by the main menu");
    CHECK(sites_are(MUSIC_ON, music, 2) && sites_are(AMBIENT_PLAY, ambient, 1),
          "music starts from the main menu's timer or after the credits, ambience from the timer only");
    CHECK(bytes_at(MAIN_MENU_TIMER, timer_ids, sizeof timer_ids) && aligned_pointers_to(MAIN_MENU_TIMER) == 1 &&
              has_in_range(MAIN_MENU_INIT, MAIN_MENU_INIT + 0x40, set_music_timer, sizeof set_music_timer),
          "the main menu arms a 2 s music timer and its timer handler starts music, then ambience every 10 s");
    CHECK(!mentions(MUSIC_ON, MUSIC_ON + 0x30, SFX_FLAGS) && !mentions(AMBIENT_PLAY, AMBIENT_PLAY + 0x25, SFX_FLAGS) &&
              !mentions(MAIN_MENU_TIMER, MAIN_MENU_TIMER + 0x3B, SFX_FLAGS),
          "the menu's own sounds do not read the audio switches");
    CHECK(sites_are(SOUNDS_FREE, (const uint32_t[]){ 0x40C038, 0x40C11A, 0x410BEA, 0x414E8D, 0x415498, 0x41566C, 0x4160D6, 0x41616A, 0x41649E }, 9),
          "the menu's sounds are freed when each window ends or a game starts");

    while (names && count < 16 && (size_t)(names - g_exe) < g_exe_size - 40) {
        size_t n = strlen((const char*)names);
        if (n) {
            snprintf(list[count++], sizeof list[0], "%s", (const char*)names);
            if (!strcmp((const char*)names, "DINO - TREX FOOT")) break;
        }
        names += n + 1;
    }
    CHECK(count == 15 && !strcmp(list[0], "OPTIONS - MAIN LOOP") && !strcmp(list[14], "DINO - TREX FOOT"),
          "the main menu requests 15 samples: the music, 13 ambient sounds and the selection sound");
    if (tpa && count == 15) {
        uint32_t entries = rd32(tpa + 4), seen = 0;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t id = rd32(tpa + 0x18 + i * 0x38);
            int requested = 0;
            for (int k = 0; k < count; k++) requested += jpt_sample_id(list[k]) == id;
            seen += requested;
            if (!requested) CHECK(id == jpt_sample_id((const char*)at(MISSING_NAME_VA)), "Menu.tpa holds no other unrequested sample");
        }
        CHECK(entries == 16 && seen == 15, "every requested sample is in Menu.tpa; the sixteenth is the loader's MISSING stand-in");
    }
    free(tpa);
}

static void menu_tpa(void) {
    size_t size = 0;
    uint8_t* tpa = read_file(GAME_DIR "data/Menu.tpa", &size);
    uint32_t id = jpt_sample_id("DINO - TREX FOOT"), count, found = 0;
    if (!tpa) {
        printf("menu_audio_tests: game/data/Menu.tpa is not there, its checks are skipped\n");
        return;
    }
    count = size >= 0x18 ? rd32(tpa + 4) : 0;
    CHECK(size >= 0x18 && rd32(tpa) == 0x150 && rd32(tpa + 8) == 0x18 && count > 0 && count < 256 && 0x18 + (uint64_t)count * 0x38 <= size,
          "Menu.tpa is a TPA");
    for (uint32_t i = 0; i < count && 0x18 + (uint64_t)(i + 1) * 0x38 <= size; i++) {
        const uint8_t* e = tpa + 0x18 + i * 0x38;
        if (rd32(e) == id) {
            found++;
            CHECK(rd32(e + 8) > 0 && (uint64_t)rd32(e + 4) + rd32(e + 8) <= size, "the sample's data lies inside Menu.tpa");
        }
    }
    CHECK(found == 1, "Menu.tpa holds the selection sound once");
    free(tpa);
}

static void file_lookup(void) {
    char out[260];
    const char* saves = "S:\\Game\\user_data\\saves";
    const char* game = "S:\\Game\\game";
    CHECK(menu_audio_source("S:\\Game\\user_data\\saves\\menu.tpa", saves, game, out, sizeof out) &&
              !strcmp(out, "S:\\Game\\game\\data\\Menu.tpa"),
          "the menus' file in the Installed Directory is served from data");
    CHECK(menu_audio_source("s:\\game\\USER_DATA\\Saves\\Menu.TPA", saves, game, out, sizeof out), "the match ignores case");
    CHECK(menu_audio_source("S:\\Game\\user_data\\saves\\menu.tpa", "S:\\Game\\user_data\\saves\\", game, out, sizeof out),
          "a trailing separator on the Installed Directory is accepted");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\saves\\SAVEGAME.001", saves, game, out, sizeof out), "other files are untouched");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\saves\\menu.tpa.bak", saves, game, out, sizeof out), "a longer name is untouched");
    CHECK(!menu_audio_source("S:\\Game\\game\\data\\Menu.tpa", saves, game, out, sizeof out), "the data file itself is untouched");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\saves\\sub\\menu.tpa", saves, game, out, sizeof out), "only the folder itself matches");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\savesX\\menu.tpa", saves, game, out, sizeof out), "a sibling folder is untouched");
    CHECK(!menu_audio_source("menu.tpa", saves, game, out, sizeof out), "a relative name is untouched");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\saves\\menu.tpa", "", game, out, sizeof out), "no Installed Directory, no match");
    CHECK(!menu_audio_source("S:\\Game\\user_data\\saves\\menu.tpa", saves, game, out, 8), "a short buffer is refused");
    CHECK(!menu_audio_source(NULL, saves, game, out, sizeof out) && !menu_audio_source("x", NULL, game, out, sizeof out),
          "missing arguments are refused");
}

static void effect_gain(void) {
    float arg = menu_effect_volume_arg(0.0f, 0.0f), shifted = menu_effect_volume_arg(-3.0f, -2.0f);
    CHECK(MENU_EFFECT_GAIN == 0.5f, "the menu effect gain is half");
    CHECK(fabsf(powf(10.0f, arg / 20.0f) - 0.5f) < 1e-4f && fabsf(arg + 6.0206f) < 1e-3f, "full volume becomes half the amplitude, -6.02 dB");
    CHECK(fabsf((shifted + -2.0f) - (-3.0f - 6.0206f)) < 1e-3f, "the setter's own attenuation is accounted for");
    if (g_exe) {
        static const uint8_t store_sample[] = { 0x89, 0x86, 0x60, 0x01, 0x00, 0x00 };
        static const uint8_t ctor_zero[] = { 0x89, 0x5E, 0x5C, 0x89, 0x5E, 0x0C };
        static const uint8_t setter_head[] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7E, 0x18 };
        CHECK(bytes_at(SOUND_LOAD + 0x26, store_sample, sizeof store_sample), "the loader stores the sample at window+0x160");
        CHECK(bytes_at(SAMPLE_CTOR + 0x35, ctor_zero, sizeof ctor_zero), "a new sample starts at 0 dB with no attenuation");
        CHECK(bytes_at(SOUND_SET_VOLUME, setter_head, sizeof setter_head), "the sample volume setter is where the host calls it");
    }
}

int main(void) {
    g_exe = read_file(GAME_DIR "trespass.exe", &g_exe_size);
    effect_gain();
    if (g_exe) {
        retail_executable();
        menu_sounds();
        menu_tpa();
        free(g_exe);
    } else {
        printf("menu_audio_tests: game/trespass.exe is not there, the retail checks are skipped\n");
    }
    file_lookup();
    printf("menu_audio_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
