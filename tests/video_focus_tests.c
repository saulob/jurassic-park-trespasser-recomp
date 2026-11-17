/*
 * Alt+Tab during a video (log 140: 0xC0000005 at 0xCCCCCCCC in sub_004188C0). WM_ACTIVATEAPP(false) reaches the
 * game's handler 0x0040EFE0, which releases the screen raster (0x0073DC40 -> the empty sentinel 0x00669B90) when the
 * game window is alive (flag 0x0066E340); the Smacker loop 0x00418AE0 waits only on IsIconic, which an Alt+Tab of a
 * windowed port never makes true, and its frame step 0x004188C0 then reads the sentinel's +0x94 as a surface. The game
 * never plays a video under a live game window; this test holds the facts that keep it so in the lifted code and in
 * the host. The lifted sources are generated, so without src/recomp/gen the code checks are skipped.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failures, g_skipped;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(c)) {                                                                                                    \
            g_failures++;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char* read_file(const char* relative) {
    char path[MAX_PATH * 2];
    FILE* f;
    char* data;
    long n;
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, relative);
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) data[n] = 0;
    return data;
}

typedef struct {
    char* all[48];
    int count;
} gen_t;

static gen_t s_gen;

static void load_gen(void) {
    for (int i = 0; i < 48; i++) {
        char name[96];
        snprintf(name, sizeof name, "src/recomp/gen/recomp_%04d.c", i);
        if (!(s_gen.all[s_gen.count] = read_file(name))) continue;
        s_gen.count++;
    }
}

/* The text of lifted function `name` of lift116 (from its definition to the closing brace of column 0). */
static char* function_body(const char* name) {
    char head[64], hooked[80];
    snprintf(head, sizeof head, "void lift116_%s(void) {", name);
    snprintf(hooked, sizeof hooked, "void lift116_lifted_%s(void) {", name);
    for (int i = 0; i < s_gen.count; i++) {
        const char* at = strstr(s_gen.all[i], head);
        const char* end;
        char* out;
        if (!at) at = strstr(s_gen.all[i], hooked);
        if (!at) continue;
        end = strstr(at, "\n}");
        if (!end) return NULL;
        out = (char*)malloc((size_t)(end - at) + 3);
        memcpy(out, at, (size_t)(end - at) + 2);
        out[(end - at) + 2] = 0;
        return out;
    }
    return NULL;
}

static int count_all(const char* needle) {
    int n = 0;
    for (int i = 0; i < s_gen.count; i++)
        for (const char* p = s_gen.all[i]; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static const char* find_in(const char* text, const char* needle) { return strstr(text, needle); }

static void test_activation_handler(void) {
    char* h = function_body("sub_0040EFE0");
    const char *active_branch, *flag, *skip, *release, *inactive_mark;
    CHECK(h != NULL, "WM_ACTIVATEAPP handler 0x0040EFE0 is lifted");
    if (!h) return;
    active_branch = find_in(h, "goto L_0040F062; /* 0x0040EFFC: jne 0x40f062 */");
    inactive_mark = find_in(h, "MEM8(eax + 0x5) = (uint8_t)(1); /* 0x0040F007");
    flag = find_in(h, "MEM8(0x66E340)); /* 0x0040F01B");
    skip = find_in(h, "goto L_0040F048; /* 0x0040F022: je 0x40f048 */");
    release = find_in(h, "MEM32(0x73DC40) = 0x00669B90u; /* 0x0040F03E");
    CHECK(active_branch && inactive_mark && flag && skip && release, "the handler's branches are where the analysis put them");
    CHECK(active_branch && inactive_mark && active_branch < inactive_mark, "an activation takes the other branch: only a deactivation reaches the release");
    CHECK(flag && skip && release && flag < skip && skip < release,
          "the screen raster is released only after the game-window flag (0x0066E340) was found set");
    free(h);
}

static void test_game_window_flag(void) {
    char* ctor = function_body("sub_0040B910");
    int writes = 0;
    CHECK(ctor && find_in(ctor, "MEM8(0x66E340) = (uint8_t)(LO8(eax)); /* 0x0040B940"), "the game window's constructor sets the flag");
    for (int i = 0; i < s_gen.count; i++)
        for (const char* p = s_gen.all[i]; (p = strstr(p, "MEM8(0x66E340) = ")); p += 10) writes++;
    CHECK(writes == 2, "the flag has two writers, the game window's constructor and destructor (%d)", writes);
    CHECK(count_all("MEM8(0x66E340) = (uint8_t)(0); /* 0x0040B9AF") == 1, "and the destructor clears it");
    free(ctor);
}

static void test_play_and_frame_step(void) {
    char* frame = function_body("sub_004188C0");
    char* loop;
    CHECK(frame != NULL, "the Smacker frame step 0x004188C0 is lifted");
    if (frame) {
        CHECK(find_in(frame, "eax = MEM32(0x73DC40); /* 0x004188D9") && find_in(frame, "ebp = MEM32(eax + 0x94); /* 0x004188E6"),
              "the frame step takes the screen raster's surface from +0x94");
        CHECK(!find_in(frame, "0x00669B90"), "and never looks for the sentinel: it relies on no video playing without a screen");
        free(frame);
    }
    CHECK(count_all("RECOMP_CALL(lift116_sub_00418AE0)") == 5, "CVideoWnd::Play has five callers (%d)", count_all("RECOMP_CALL(lift116_sub_00418AE0)"));
    CHECK(count_all("RECOMP_CALL(lift116_sub_00418AE0); /* 0x0040F2BC") == 1 && count_all("RECOMP_CALL(lift116_sub_00418AE0); /* 0x00415687") == 1 &&
              count_all("RECOMP_CALL(lift116_sub_00418AE0); /* 0x004163FA") == 1 && count_all("RECOMP_CALL(lift116_sub_00418AE0); /* 0x0040F440") == 1 &&
              count_all("RECOMP_CALL(lift116_sub_00418AE0); /* 0x0040F46A") == 1,
          "tpassintro (start-up), newgame (New Game), credits (menu), win and credits (the finished game): the game's own five calls");
    loop = function_body("sub_00418AE0");
    CHECK(loop && find_in(loop, "RECOMP_ICALL(MEM32(0x645390)); /* 0x00418EED") && !find_in(loop, "MEM32(0x645398)"),
          "Play's loop waits on IsIconic alone: no foreground or activation check before each frame");
    free(loop);
}

static void test_only_the_games_own_callers_play(void) {
    static const char* const hosts[] = { "src/host/video_hd.c", "src/host/video_test.c", "src/host/window_host.c" };
    static const char* const others[] = { "src/host/debug_menu.c", "src/host/cheats.c", "src/host/overlay.c", "src/host/ce_gameplay.c",
                                          "src/host/gamepad.c", "src/host/main.c", "src/host/ui_canvas.c" };
    char* test = read_file("src/host/video_test.c");
    CHECK(test && strstr(test, "vt_start(&s_state, main_menu_front(&front), &video, &mode)") &&
              strstr(test, "return v->kind == GUEST_WND_MAIN_MENU && v->ui == GUEST_UI_MENU && v->depth == 1;"),
          "the Video Test plays only with the main menu alone in front, where no game window is alive");
    free(test);
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) CHECK(read_file(hosts[i]) != NULL, "%s", hosts[i]);
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        char* text = read_file(others[i]);
        CHECK(text && !strstr(text, "0x00418AE0") && !strstr(text, "VIDEO_WND_PLAY") && !strstr(text, "0x00418700"),
              "%s does not start a video", others[i]);
        free(text);
    }
}

int main(void) {
    load_gen();
    if (!s_gen.count) {
        g_skipped = 1;
        printf("note: src/recomp/gen is not present: the lifted-code checks were skipped\n");
    } else {
        test_activation_handler();
        test_game_window_flag();
        test_play_and_frame_step();
    }
    test_only_the_games_own_callers_play();
    printf("video_focus_tests: %d checks, %d failures%s\n", g_checks, g_failures, g_skipped ? " (lifted code skipped)" : "");
    return g_failures ? 1 : 0;
}
