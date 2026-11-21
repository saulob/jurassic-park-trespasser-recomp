/* Whole play sessions of the guest against the virtual window, scripted through its own menus and keys: New Game into the first
 * level, a Save through the pause menu, walking away, a Load that puts the player back, Quit to the main menu, a second New
 * Game in the same process, and the game's own Quit to the desktop. Pictures are compared as the game draws them (frames are kept
 * in <user root>/frames, and JPT_LINUX_SESSION_DUMP=<prefix> also writes them out). What the guest still holds when it ends is
 * checked. Needs the installed game; otherwise DATA UNAVAILABLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "jpt_fs.h"
#include "jpt_user_data.h"
#include "linux_boot.h"
#include "linux_hle.h"
#include "linux_log.h"
#include "linux_screen.h"
#include "linux_window.h"

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

#define USER_ROOT "build/linux_session_tests_user"
#define FRAMES_DIR USER_ROOT "/frames"

typedef enum { A_CLICK, A_KEY, A_KEYDOWN, A_KEYUP, A_TEXT, A_DUMP, A_WAIT_WIDTH, A_MARK, A_STOP } action_kind_t;

typedef struct {
    unsigned wait;       /* frames after the previous action (the least, for A_WAIT_WIDTH) */
    action_kind_t kind;
    int x, y;            /* a click; a key in x; A_WAIT_WIDTH: the guest screen width */
    const char* text;    /* typed text or a dump name */
} action_t;

#define KEY_ESCAPE 0x1B
#define KEY_W 0x57
#define KEY_A 0x41
#define KEY_S 0x53
#define KEY_D 0x44
#define KEY_SPACE 0x20

static const action_t k_script[] = {
    /* level 1 */
    { 10, A_WAIT_WIDTH, 512, 0, NULL },
    { 400, A_DUMP, 0, 0, "level_a" },
    /* Save */
    { 5, A_KEY, KEY_ESCAPE, 0, NULL },
    { 60, A_CLICK, 258, 137, NULL },
    { 60, A_TEXT, 0, 0, "JPTSAVE1" },
    { 30, A_CLICK, 142, 273, NULL },
    { 200, A_DUMP, 0, 0, "after_save" },
    /* walk away */
    { 5, A_CLICK, 315, 197, NULL },
    { 60, A_KEYDOWN, KEY_W, 0, NULL },
    { 200, A_KEYUP, KEY_W, 0, NULL },
    { 60, A_DUMP, 0, 0, "level_b" },
    /* Load: a double click on the save */
    { 5, A_KEY, KEY_ESCAPE, 0, NULL },
    { 60, A_CLICK, 165, 140, NULL },
    { 60, A_CLICK, 255, 114, NULL },
    { 8, A_CLICK, 255, 114, NULL },
    { 150, A_DUMP, 0, 0, "level_c" },
    /* Quit to the main menu */
    { 5, A_KEY, KEY_ESCAPE, 0, NULL },
    { 60, A_CLICK, 350, 140, NULL },
    { 60, A_DUMP, 0, 0, "quit_prompt" },
    { 5, A_CLICK, 260, 208, NULL },
    { 300, A_DUMP, 0, 0, "back_in_menu" },
    /* a second game */
    { 10, A_CLICK, 310, 105, NULL },
    { 60, A_WAIT_WIDTH, 512, 0, NULL },
    { 400, A_DUMP, 0, 0, "level_d" },
    /* a long walk: a few thousand frames of moving, strafing and jumping while the level streams and the audio plays */
    { 5, A_MARK, 0, 0, NULL },
    { 5, A_KEYDOWN, KEY_W, 0, NULL },
    { 1500, A_KEYDOWN, KEY_A, 0, NULL },
    { 500, A_KEYUP, KEY_A, 0, NULL },
    { 5, A_KEY, KEY_SPACE, 0, NULL },
    { 1000, A_KEYDOWN, KEY_D, 0, NULL },
    { 500, A_KEYUP, KEY_D, 0, NULL },
    { 5, A_KEY, KEY_SPACE, 0, NULL },
    { 1500, A_KEYDOWN, KEY_S, 0, NULL },
    { 1000, A_KEYUP, KEY_S, 0, NULL },
    { 1500, A_KEYUP, KEY_W, 0, NULL },
    { 100, A_MARK, 0, 0, NULL },
    /* the game's own Quit to the desktop */
    { 5, A_KEY, KEY_ESCAPE, 0, NULL },
    { 60, A_CLICK, 350, 140, NULL },
    { 60, A_CLICK, 262, 167, NULL },
    { 3000, A_STOP, 0, 0, NULL },
};

static struct {
    unsigned action, wait_from;
    const char* dump_prefix;
    unsigned marks, mark_frame[2];
    long mark_rss[2];
} T;

static long resident_kb(void) {
    long pages = 0, resident = 0;
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * 4;
}

static void key_event(uint32_t vk, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = LX_EV_KEY, ev.vk = vk, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static void mouse(lx_event_kind_t kind, int x, int y, int down) {
    lx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind, ev.x = x, ev.y = y, ev.vk = 1, ev.down = down;
    lx_virtual_queue_event(&ev);
}

static struct {
    unsigned frame;
    int x, y;
} s_click;

static void drive_click(unsigned frame) {
    if (!s_click.frame) return;
    if (frame == s_click.frame) mouse(LX_EV_MOUSE_MOVE, s_click.x, s_click.y, 0);
    if (frame == s_click.frame + 2) mouse(LX_EV_MOUSE_BUTTON, s_click.x, s_click.y, 1);
    if (frame == s_click.frame + 4) mouse(LX_EV_MOUSE_BUTTON, s_click.x, s_click.y, 0), s_click.frame = 0;
}

static void keep_frame(const char* name, unsigned frame) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s.ppm", FRAMES_DIR, name);
    lx_virtual_dump_ppm(path);
    if (T.dump_prefix) {
        snprintf(path, sizeof path, "%s_%s_%u.ppm", T.dump_prefix, name, frame);
        lx_virtual_dump_ppm(path);
    }
}

static void run_script(unsigned frame) {
    const action_t* a;
    const lx_virtual_state_t* v = lx_virtual_state();
    if (T.action >= sizeof k_script / sizeof k_script[0]) return;
    a = &k_script[T.action];
    if (frame < T.wait_from + a->wait) return;
    switch (a->kind) {
    case A_CLICK:
        if (s_click.frame) return;
        s_click.frame = frame + 2, s_click.x = a->x, s_click.y = a->y;
        break;
    case A_KEY: key_event((uint32_t)a->x, 1), key_event((uint32_t)a->x, 0); break;
    case A_KEYDOWN: key_event((uint32_t)a->x, 1); break;
    case A_KEYUP: key_event((uint32_t)a->x, 0); break;
    case A_TEXT:
        for (const char* c = a->text; *c; c++) {
            lx_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = LX_EV_CHAR, ev.vk = (uint32_t)*c;
            lx_virtual_queue_event(&ev);
        }
        break;
    case A_DUMP: keep_frame(a->text, frame); break;
    case A_WAIT_WIDTH:
        if (v->last.width != a->x) return;
        break;
    case A_MARK:
        if (T.marks < 2) T.mark_frame[T.marks] = frame, T.mark_rss[T.marks++] = resident_kb();
        break;
    case A_STOP: lx_screen_set_frame_limit(frame); break;
    }
    T.action++;
    T.wait_from = frame;
}

static void hook(unsigned frame, void* user) {
    (void)user;
    drive_click(frame);
    if (frame < 300) return;
    if (frame == 300) {
        s_click.frame = 302, s_click.x = 310, s_click.y = 105;
        T.wait_from = 310;
    }
    if (frame > 310) run_script(frame);
}

/* ---- pictures ---- */

typedef struct {
    int w, h;
    unsigned char* rgb;
} picture_t;

static int read_picture(const char* name, picture_t* p) {
    char path[256];
    FILE* f;
    int max = 0;
    snprintf(path, sizeof path, "%s/%s.ppm", FRAMES_DIR, name);
    memset(p, 0, sizeof *p);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fscanf(f, "P6 %d %d %d", &p->w, &p->h, &max) != 3 || fgetc(f) == EOF || p->w <= 0 || p->h <= 0) {
        fclose(f);
        return 0;
    }
    p->rgb = (unsigned char*)malloc((size_t)p->w * (size_t)p->h * 3);
    if (!p->rgb || fread(p->rgb, 1, (size_t)p->w * (size_t)p->h * 3, f) != (size_t)p->w * (size_t)p->h * 3) {
        free(p->rgb);
        p->rgb = NULL;
    }
    fclose(f);
    return p->rgb != NULL;
}

/* The mean absolute difference per channel (0..255) of the sky, the top third, which no animation reaches. */
static double sky_difference(const char* a, const char* b) {
    picture_t pa, pb;
    double result = -1;
    int have_a = read_picture(a, &pa), have_b = read_picture(b, &pb);
    if (have_a && have_b && pa.w == pb.w && pa.h == pb.h) {
        size_t n = (size_t)pa.w * (size_t)(pa.h / 3) * 3;
        unsigned long long total = 0;
        for (size_t i = 0; i < n; i++) total += (unsigned long long)abs((int)pa.rgb[i] - (int)pb.rgb[i]);
        result = (double)total / (double)n;
    }
    free(pa.rgb);
    free(pb.rgb);
    return result;
}

static unsigned distinct_colours(const char* name) {
    picture_t p;
    static unsigned char seen[1 << 9];
    unsigned count = 0;
    if (!read_picture(name, &p)) return 0;
    memset(seen, 0, sizeof seen);
    for (long i = 0; i < (long)p.w * p.h; i++) {
        unsigned key = ((unsigned)(p.rgb[i * 3] >> 5) << 6) | ((unsigned)(p.rgb[i * 3 + 1] >> 5) << 3) | (unsigned)(p.rgb[i * 3 + 2] >> 5);
        if (!(seen[key >> 3] & (1u << (key & 7)))) seen[key >> 3] |= (unsigned char)(1u << (key & 7)), count++;
    }
    free(p.rgb);
    return count;
}

static long file_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    lx_boot_options_t o;
    lx_boot_result_t r;
    double moved, restored, again;
    memset(&o, 0, sizeof o);
    o.game_dir = argc > 1 ? argv[1] : env && *env ? env : "game";
    o.version = "en-116";
    o.user_root = USER_ROOT;
    o.idle_limit = 0xFFFFFFFFu;
    o.frame_limit = 40000;
    o.frame_hook = hook;
    T.dump_prefix = getenv("JPT_LINUX_SESSION_DUMP");
    jpt_make_dirs(FRAMES_DIR);
    lx_boot_headless(&o, &r);
    if (r.status == LX_BOOT_DATA_UNAVAILABLE) {
        printf("linux_session_tests: skipped - DATA UNAVAILABLE (no guest image under %s)\n", o.game_dir);
        printf("linux_session_tests: 1 checks, 0 failures\n");
        return 0;
    }
    printf("  run: %s (%s), %u script steps of %zu done, exit code %u\n", lx_boot_status_name(r.status), r.detail, T.action,
           sizeof k_script / sizeof k_script[0], r.exit_code);
    printf("  at stop: %u files, %u Stream.tpa views, %u finds, %u DirectDraw surfaces, %u audio buffers, %u guest threads, %u Smack objects\n",
           r.at_stop.files, r.at_stop.stream_views, r.at_stop.finds, r.at_stop.ddraw_surfaces, r.at_stop.audio_buffers, r.at_stop.guest_threads,
           r.at_stop.smack_objects);

    CHECK(T.marks == 2 && T.mark_frame[1] - T.mark_frame[0] >= 6000, "the long walk ran %u frames", T.marks == 2 ? T.mark_frame[1] - T.mark_frame[0] : 0);
    printf("  long walk: %u frames, resident memory %ld KB -> %ld KB\n", T.mark_frame[1] - T.mark_frame[0], T.mark_rss[0], T.mark_rss[1]);
    CHECK(T.marks == 2 && T.mark_rss[1] - T.mark_rss[0] < 150000, "the walk does not grow the process by more than 150 MB (%ld KB)", T.mark_rss[1] - T.mark_rss[0]);
    CHECK(r.at_stop.stream_views == 0 && r.at_stop.finds == 0 && r.at_stop.ddraw_surfaces == 0 && r.at_stop.audio_buffers == 0 && r.at_stop.smack_objects == 0,
          "after the game's own quit nothing is left but its process-lifetime objects (views %u, finds %u, surfaces %u, audio buffers %u, Smack %u)",
          r.at_stop.stream_views, r.at_stop.finds, r.at_stop.ddraw_surfaces, r.at_stop.audio_buffers, r.at_stop.smack_objects);
    CHECK(r.at_stop.files <= 1 && r.at_stop.guest_threads <= 2, "the game leaves its level swap file and its streaming thread to the process exit (%u files, %u threads)",
          r.at_stop.files, r.at_stop.guest_threads);
    CHECK(T.action >= 39, "the script ran to its Quit to the desktop (%u of %zu steps)", T.action, sizeof k_script / sizeof k_script[0]);
    CHECK(distinct_colours("level_a") > 40, "the first level is a picture (%u colours)", distinct_colours("level_a"));
    CHECK(file_size(USER_ROOT "/saves/SAVEGAME.000") > 100000, "the Save wrote SAVEGAME.000 (%ld bytes)", file_size(USER_ROOT "/saves/SAVEGAME.000"));
    moved = sky_difference("level_a", "level_b");
    restored = sky_difference("level_a", "level_c");
    again = sky_difference("level_a", "level_d");
    printf("  sky difference from the saved moment: walked away %.2f, after Load %.2f, second game %.2f\n", moved, restored, again);
    CHECK(moved > 1.0, "walking changes the view (%.2f)", moved);
    CHECK(restored >= 0 && restored < moved / 2, "Load puts the player back where the Save was made (%.2f against %.2f)", restored, moved);
    CHECK(distinct_colours("back_in_menu") > 40, "Quit to the main menu draws the menu again (%u colours)", distinct_colours("back_in_menu"));
    CHECK(again >= 0 && again < moved / 2, "a second New Game in the same process starts where the first did (%.2f)", again);
    CHECK(r.status == LX_BOOT_EXITED || r.status == LX_BOOT_FRAME_PRESENTED, "the run ended in a defined way: %s (%s)", lx_boot_status_name(r.status), r.detail);
    printf("linux_session_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
