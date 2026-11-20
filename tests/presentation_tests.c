/*
 * Headless checks of the window presentation policy: the game's screen changes size (menu, gameplay, videos) while
 * the host window is resized only with the first frame of the new screen, and the picture is the largest rectangle of
 * the game's shape centered in it. No window, no device.
 * Build: CMake target presentation_tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_display.h"

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static int view_is(const jpt_presentation_t* p, int left, int top, int right, int bottom) {
    return p->view.left == left && p->view.top == top && p->view.right == right && p->view.bottom == bottom;
}

static jpt_presentation_t shown(int host_w, int host_h, int guest_w, int guest_h) {
    jpt_presentation_t p;
    jpt_presentation_init(&p, 0);
    jpt_presentation_set_host(&p, host_w, host_h);
    jpt_presentation_set_guest(&p, guest_w, guest_h);
    return p;
}

static void test_four_by_three_in_widescreen_hosts(void) {
    jpt_presentation_t a = shown(2560, 1440, 1024, 768);
    jpt_presentation_t b = shown(1920, 1080, 1024, 768);
    jpt_presentation_t c = shown(1280, 720, 1024, 768);
    jpt_presentation_t d = shown(2560, 1440, 640, 480);
    CHECK(view_is(&a, 320, 0, 2240, 1440));
    CHECK(view_is(&b, 240, 0, 1680, 1080));
    CHECK(view_is(&c, 160, 0, 1120, 720));
    CHECK(view_is(&d, 320, 0, 2240, 1440));
}

static void test_other_shapes(void) {
    jpt_presentation_t wide = shown(1920, 1080, 2560, 1440);
    jpt_presentation_t tall = shown(960, 720, 2560, 1440);
    jpt_presentation_t ultra = shown(3440, 1440, 640, 480);
    CHECK(view_is(&wide, 0, 0, 1920, 1080));
    CHECK(view_is(&tall, 0, 90, 960, 630));
    CHECK(view_is(&ultra, 760, 0, 2680, 1440));
}

static void test_stretch_fills_the_client(void) {
    jpt_presentation_t p;
    jpt_presentation_init(&p, 1);
    jpt_presentation_set_host(&p, 2560, 1440);
    jpt_presentation_set_guest(&p, 640, 480);
    CHECK(view_is(&p, 0, 0, 2560, 1440));
}

/* The same two sizes always give the same view whatever the order they arrived in. */
static void test_view_does_not_depend_on_order(void) {
    jpt_presentation_t a, b;
    jpt_presentation_init(&a, 0);
    jpt_presentation_set_host(&a, 1920, 1080);
    jpt_presentation_set_guest(&a, 1024, 768);
    jpt_presentation_init(&b, 0);
    jpt_presentation_set_guest(&b, 1024, 768);
    jpt_presentation_set_host(&b, 1920, 1080);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}

static void test_pillarbox_offsets(void) {
    int hosts[][2] = { { 2560, 1440 }, { 1920, 1080 }, { 1280, 720 }, { 3440, 1440 }, { 1280, 1024 }, { 1001, 563 } };
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        jpt_presentation_t p = shown(hosts[i][0], hosts[i][1], 1024, 768);
        int view_w = p.view.right - p.view.left, view_h = p.view.bottom - p.view.top;
        CHECK(p.view.left >= 0 && p.view.top >= 0 && p.view.right <= hosts[i][0] && p.view.bottom <= hosts[i][1]);
        CHECK(abs(p.view.left - (hosts[i][0] - p.view.right)) <= 1); /* centered */
        CHECK(abs(p.view.top - (hosts[i][1] - p.view.bottom)) <= 1);
        CHECK(abs(view_w * 768 - view_h * 1024) <= 1024); /* shape kept to a pixel */
        CHECK(view_w == hosts[i][0] || view_h == hosts[i][1]); /* touches two opposite sides */
    }
}

static void test_mouse_mapping(void) {
    jpt_presentation_t p = shown(2560, 1440, 1024, 768);
    int x, y;
    CHECK(jpt_presentation_to_guest(&p, 1280, 720, &x, &y) && x == 512 && y == 384);
    CHECK(jpt_presentation_to_guest(&p, 320, 0, &x, &y) && x == 0 && y == 0);
    CHECK(jpt_presentation_to_guest(&p, 2239, 1439, &x, &y) && x == 1023 && y == 767);
    CHECK(!jpt_presentation_to_guest(&p, 319, 720, &x, &y) && x < 0); /* in the pillarbox */
    CHECK(!jpt_presentation_to_guest(&p, 2240, 720, &x, &y) && x >= 1024);
    CHECK(!jpt_presentation_to_guest(&p, 1280, -1, &x, &y) && y < 0);
    for (int gx = 0; gx < 1024; gx += 7) {
        int cx = p.view.left + (int)(((long long)gx * 1920 + 1023) / 1024), back_x, back_y;
        CHECK(jpt_presentation_to_guest(&p, cx, p.view.top + 100, &back_x, &back_y) && back_x == gx);
    }
    p = shown(960, 720, 2560, 1440); /* letterbox: bars above and below */
    CHECK(jpt_presentation_to_guest(&p, 480, 360, &x, &y) && x == 1280 && y == 720);
    CHECK(!jpt_presentation_to_guest(&p, 480, 89, &x, &y) && y < 0);
    CHECK(jpt_presentation_to_guest(&p, 959, 629, &x, &y) && x == 2557 && y == 1437);
    jpt_presentation_init(&p, 1);
    jpt_presentation_set_host(&p, 2560, 1440);
    jpt_presentation_set_guest(&p, 640, 480);
    CHECK(jpt_presentation_to_guest(&p, 2559, 1439, &x, &y) && x == 639 && y == 479);
}

/* Physical pixels at 100%, 150% and 225%: the same logical window gives the same shape at every scale. */
static void test_dpi_independence(void) {
    static const int scale_percent[] = { 100, 150, 225 };
    for (size_t i = 0; i < sizeof scale_percent / sizeof scale_percent[0]; i++) {
        int w = 1280 * scale_percent[i] / 100, h = 720 * scale_percent[i] / 100;
        jpt_presentation_t p = shown(w, h, 1024, 768);
        int view_w = p.view.right - p.view.left;
        CHECK(p.view.top == 0 && p.view.bottom == h);
        CHECK(abs(view_w - h * 4 / 3) <= 1);
        CHECK(abs(p.view.left - (w - p.view.right)) <= 1);
    }
}

static void test_degenerate_sizes(void) {
    jpt_presentation_t p;
    int x = 7, y = 7;
    jpt_presentation_init(&p, 0);
    CHECK(!jpt_presentation_to_guest(&p, 0, 0, &x, &y)); /* nothing known yet: no division by zero */
    CHECK(!jpt_presentation_set_host(&p, 0, 0) && !jpt_presentation_set_host(&p, -5, 100));
    CHECK(!jpt_presentation_set_guest(&p, 0, 480) && !jpt_presentation_set_guest(&p, 640, -1));
    CHECK(p.host.width == 0 && p.guest.width == 0);
    jpt_presentation_set_host(&p, 800, 600);
    CHECK(view_is(&p, 0, 0, 800, 600)); /* guest unknown: the whole client */
    CHECK(!jpt_presentation_to_guest(&p, 10, 10, &x, &y));
    jpt_presentation_set_guest(&p, 640, 480);
    CHECK(jpt_presentation_set_host(&p, 1, 1));
    CHECK(view_is(&p, 0, 0, 1, 1) && jpt_presentation_to_guest(&p, 0, 0, &x, &y) && x == 0 && y == 0);
    CHECK(!jpt_presentation_set_guest(&p, 0, 0) && p.guest.width == 640 && p.guest.height == 480);
    CHECK(p.host.width == 1 && p.host.height == 1);
    jpt_presentation_set_host(&p, 4096, 1);
    jpt_presentation_set_guest(&p, 4096, 4096);
    CHECK(view_is(&p, 2047, 0, 2048, 1));
    CHECK(jpt_presentation_to_guest(&p, 2047, 0, &x, &y) && x == 0 && y == 0);
    CHECK(!jpt_presentation_to_guest(&p, 0, 0, &x, &y) && x < 0);
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    char* text;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    if (text) text[size] = 0;
    fclose(f);
    return text;
}

/* A copy of the function's text, valid until the next call. */
static char* function_body(const char* source, const char* signature) {
    static char* kept;
    const char* start = strstr(source, signature);
    const char* end;
    free(kept);
    kept = NULL;
    if (!start) return NULL;
    end = strstr(start, "\n}");
    if (!end) return NULL;
    kept = (char*)malloc((size_t)(end - start) + 3);
    if (!kept) return NULL;
    memcpy(kept, start, (size_t)(end - start) + 2);
    kept[(end - start) + 2] = 0;
    return kept;
}

/* The transition code itself: the handler that receives the game's screen size resizes nothing. */
static void test_transition_code_does_not_resize_the_window(void) {
    char* source = read_file(JPT_SOURCE_DIR "\\src\\host\\window_host.c");
    char* body;
    CHECK(source != NULL);
    if (!source) return;
    body = function_body(source, "void window_host_set_display(HWND window, int width, int height, unsigned generation) {");
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "jpt_presentation_request_guest") != NULL);
        CHECK(strstr(body, "jpt_presentation_set_guest") == NULL && strstr(body, "jpt_presentation_commit") == NULL);
        CHECK(strstr(body, "SetWindowPos") == NULL && strstr(body, "MoveWindow") == NULL);
        CHECK(strstr(body, "AdjustWindowRect") == NULL && strstr(body, "SetWindowLong") == NULL);
        CHECK(strstr(body, "ShowWindow") == NULL && strstr(body, "SetWindowPlacement") == NULL);
        CHECK(strstr(body, "GetWindowRect") == NULL && strstr(body, "round_div") == NULL);
    }
    free(source);
    source = read_file(JPT_SOURCE_DIR "\\src\\host\\ddraw\\dd_device.c");
    CHECK(source != NULL);
    if (source) {
        body = function_body(source, "static void sync_window_display(dd_device_t* d) {");
        CHECK(body != NULL);
        if (body) CHECK(strstr(body, "SetWindowPos") == NULL && strstr(body, "MoveWindow") == NULL);
        free(source);
    }
}

/* A presenter as the host runs it: the frame's generation is offered to the presentation just before it is shown. */
static jpt_rect_t show_frame(jpt_presentation_t* p, unsigned generation) {
    jpt_presentation_commit(p, generation);
    return p->view;
}

static jpt_presentation_t menu_at_960x720(void) {
    jpt_presentation_t p;
    jpt_presentation_init(&p, 0);
    jpt_presentation_set_host(&p, 960, 720);
    jpt_presentation_request_guest(&p, 640, 480, 1);
    return p;
}

static void test_request_keeps_the_active_screen(void) {
    jpt_presentation_t p = menu_at_960x720();
    CHECK(p.guest.width == 640 && p.generation == 1 && !p.has_pending && view_is(&p, 0, 0, 960, 720));
    CHECK(!jpt_presentation_request_guest(&p, 2560, 1440, 2));
    CHECK(p.guest.width == 640 && p.guest.height == 480 && view_is(&p, 0, 0, 960, 720));
    CHECK(p.has_pending && p.pending_guest.width == 2560 && p.pending_guest.height == 1440 && p.pending_generation == 2);
}

static void test_first_frame_of_the_new_generation_commits(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(jpt_presentation_commit(&p, 2));
    CHECK(p.guest.width == 2560 && p.guest.height == 1440 && p.generation == 2 && !p.has_pending);
    CHECK(view_is(&p, 0, 90, 960, 630));
    CHECK(!jpt_presentation_commit(&p, 2)); /* later frames of it change nothing */
    CHECK(view_is(&p, 0, 90, 960, 630));
}

/* The reported bug: the menu is still on screen after SetDisplayMode, and showing it again must not squeeze it. */
static void test_old_frames_never_recompute_the_view(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_rect_t before = p.view, view;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    for (int i = 0; i < 5; i++) {
        view = show_frame(&p, 1);
        CHECK(memcmp(&view, &before, sizeof view) == 0 && p.guest.width == 640 && p.has_pending);
    }
    CHECK(!jpt_presentation_commit(&p, 0) && !jpt_presentation_commit(&p, 3) && p.guest.width == 640);
    view = show_frame(&p, 2);
    CHECK(view.left == 0 && view.top == 90 && view.right == 960 && view.bottom == 630);
    CHECK(p.guest.width == 2560 && !p.has_pending);
}

static void test_mouse_follows_the_active_screen(void) {
    jpt_presentation_t p = menu_at_960x720();
    int x, y;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(jpt_presentation_to_guest(&p, 480, 360, &x, &y) && x == 320 && y == 240); /* still the 640x480 menu */
    CHECK(jpt_presentation_to_guest(&p, 959, 719, &x, &y) && x == 639 && y == 479);
    CHECK(jpt_presentation_to_guest(&p, 480, 40, &x, &y) && y == 26); /* the old picture covers the top rows */
    jpt_presentation_commit(&p, 2);
    CHECK(jpt_presentation_to_guest(&p, 480, 360, &x, &y) && x == 1280 && y == 720);
    CHECK(!jpt_presentation_to_guest(&p, 480, 40, &x, &y) && y < 0); /* the new letterbox bar */
}

static void test_mouse_switches_with_the_commit(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_presentation_t before;
    int bx, by, ax, ay;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    before = p;
    jpt_presentation_commit(&p, 1); /* a stale frame: the transform is the one it was */
    CHECK(memcmp(&before, &p, sizeof p) == 0);
    jpt_presentation_to_guest(&p, 700, 300, &bx, &by);
    CHECK(jpt_presentation_commit(&p, 2));
    jpt_presentation_to_guest(&p, 700, 300, &ax, &ay);
    CHECK(bx == 466 && by == 200 && ax == 1866 && ay == 560);
    CHECK(p.view.top == 90); /* the view and the transform changed in the same step */
}

static void test_only_the_final_generation_commits(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    jpt_presentation_request_guest(&p, 1024, 576, 3);
    CHECK(p.has_pending && p.pending_guest.width == 1024 && p.pending_generation == 3);
    CHECK(!jpt_presentation_commit(&p, 2) && p.guest.width == 640);
    CHECK(jpt_presentation_commit(&p, 3));
    CHECK(p.guest.width == 1024 && p.guest.height == 576 && view_is(&p, 0, 90, 960, 630));
    CHECK(!jpt_presentation_commit(&p, 2) && p.guest.width == 1024);
}

static void test_returning_to_the_active_size_cancels(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(!jpt_presentation_request_guest(&p, 640, 480, 3));
    CHECK(!p.has_pending && !jpt_presentation_commit(&p, 2) && p.guest.width == 640 && view_is(&p, 0, 0, 960, 720));
}

static void test_gameplay_back_to_menu(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_rect_t view;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    show_frame(&p, 2);
    jpt_presentation_request_guest(&p, 640, 480, 3);
    view = show_frame(&p, 2); /* a gameplay frame after the request keeps the gameplay view */
    CHECK(view.top == 90 && view.bottom == 630 && p.guest.width == 2560);
    view = show_frame(&p, 3);
    CHECK(view.top == 0 && view.bottom == 720 && p.guest.width == 640 && p.generation == 3);
}

static void test_host_changes_stay_immediate(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(jpt_presentation_set_host(&p, 1600, 900)); /* manual resize, F11 or a DPI change */
    CHECK(view_is(&p, 200, 0, 1400, 900) && p.guest.width == 640 && p.has_pending);
    CHECK(jpt_presentation_set_host(&p, 2560, 1440)); /* full screen */
    CHECK(view_is(&p, 320, 0, 2240, 1440) && p.guest.width == 640 && p.has_pending);
    jpt_presentation_commit(&p, 2);
    CHECK(view_is(&p, 0, 0, 2560, 1440));
    CHECK(jpt_presentation_set_host(&p, 1920, 1080));
    CHECK(view_is(&p, 0, 0, 1920, 1080));
}

static void test_next_view_is_what_the_commit_gives(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_rect_t next = jpt_presentation_next_view(&p);
    CHECK(next.left == 0 && next.top == 0 && next.right == 960 && next.bottom == 720);
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    next = jpt_presentation_next_view(&p);
    CHECK(next.top == 90 && next.bottom == 630 && view_is(&p, 0, 0, 960, 720));
}

static void test_requests_with_nothing_shown_or_invalid(void) {
    jpt_presentation_t p, empty;
    jpt_presentation_init(&p, 0);
    CHECK(!jpt_presentation_request_guest(&p, 0, 480, 1) && !jpt_presentation_request_guest(&p, 640, -1, 1));
    CHECK(p.guest.width == 0 && !p.has_pending);
    jpt_presentation_set_host(&p, 800, 600);
    CHECK(jpt_presentation_request_guest(&p, 640, 480, 1)); /* nothing on screen to protect: active at once */
    CHECK(p.guest.width == 640 && !p.has_pending && p.generation == 1);
    CHECK(!jpt_presentation_request_guest(&p, 0, 0, 2) && !p.has_pending);
    CHECK(!jpt_presentation_commit(&p, 0));
    jpt_presentation_init(&empty, 0);
    CHECK(!jpt_presentation_commit(&empty, 0));
    CHECK(jpt_presentation_next_view(&empty).right == 0);
}

/* The presenters commit just before the swap chain's Present, and the DirectDraw layer stamps frames. */
static void test_presenters_commit_before_present(void) {
    char* present = read_file(JPT_SOURCE_DIR "\\src\\host\\ddraw\\d3d11_present.c");
    char* dd_surface = read_file(JPT_SOURCE_DIR "\\src\\host\\ddraw\\dd_surface.c");
    char* body;
    CHECK(present != NULL && dd_surface != NULL);
    if (present) {
        body = function_body(present, "int present_submit(int vsync, unsigned generation) {");
        CHECK(body != NULL);
        if (body) {
            char* commit = strstr(body, "window_host_frame_commit(generation)");
            char* swap = strstr(body, "IDXGISwapChain_Present");
            CHECK(commit && swap && commit < swap);
        }
        body = function_body(present, "void present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,");
        CHECK(body != NULL);
        if (body) {
            char* commit = strstr(body, "window_host_frame_commit(generation)");
            char* swap = strstr(body, "IDXGISwapChain_Present");
            CHECK(commit && swap && commit < swap);
        }
    }
    if (dd_surface) {
        CHECK(strstr(dd_surface, "front->display_generation") != NULL);
        CHECK(strstr(dd_surface, "s->display_generation = dd_display_generation()") != NULL);
    }
    free(present);
    free(dd_surface);
}

/* The host window as the window host drives it: Window size Auto follows the game's shape, a fixed size does not. */
#define FRAME_W 16
#define FRAME_H 39
#define WORK_W 1920
#define WORK_H 1040

static jpt_commit_t present_frame_of(jpt_presentation_t* p, unsigned generation, int follow) {
    return jpt_presentation_commit_frame(p, generation, follow, FRAME_W, FRAME_H, WORK_W, WORK_H);
}

static int host_is(const jpt_presentation_t* p, int w, int h) { return p->host.width == w && p->host.height == h; }

static void test_auto_window_waits_for_the_new_generation(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_commit_t c;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(host_is(&p, 960, 720) && view_is(&p, 0, 0, 960, 720)); /* A: the request alone changes nothing */
    c = present_frame_of(&p, 1, 1);
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 720)); /* C: an old frame cannot resize */
    c = present_frame_of(&p, 1, 1);
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 720) && view_is(&p, 0, 0, 960, 720));
    c = present_frame_of(&p, 2, 1); /* B: the first frame of the new generation */
    CHECK(c.committed && c.resize && c.client.width == 960 && c.client.height == 540);
    CHECK(host_is(&p, 960, 540) && view_is(&p, 0, 0, 960, 540) && p.guest.width == 2560);
    c = present_frame_of(&p, 2, 1);
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 540));
}

static void test_auto_window_returns_with_the_menu_frame(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_commit_t c;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    present_frame_of(&p, 2, 1);
    jpt_presentation_request_guest(&p, 640, 480, 3);
    c = present_frame_of(&p, 2, 1); /* D: gameplay frames keep the gameplay geometry */
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 540) && p.guest.width == 2560);
    c = present_frame_of(&p, 3, 1);
    CHECK(c.committed && c.resize && c.client.width == 960 && c.client.height == 720);
    CHECK(host_is(&p, 960, 720) && view_is(&p, 0, 0, 960, 720) && p.guest.width == 640);
}

static void test_fixed_window_never_resizes(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_commit_t c;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    c = present_frame_of(&p, 2, 0); /* E */
    CHECK(c.committed && !c.resize && host_is(&p, 960, 720) && view_is(&p, 0, 90, 960, 630));
    jpt_presentation_request_guest(&p, 640, 480, 3);
    c = present_frame_of(&p, 3, 0);
    CHECK(c.committed && !c.resize && host_is(&p, 960, 720) && view_is(&p, 0, 0, 960, 720));
}

static void test_mouse_changes_with_the_window(void) {
    jpt_presentation_t p = menu_at_960x720();
    int x, y;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(jpt_presentation_to_guest(&p, 700, 500, &x, &y) && x == 466 && y == 333); /* F: old window, old screen */
    present_frame_of(&p, 1, 1);
    CHECK(host_is(&p, 960, 720) && jpt_presentation_to_guest(&p, 700, 500, &x, &y) && x == 466 && y == 333);
    present_frame_of(&p, 2, 1);
    CHECK(host_is(&p, 960, 540) && jpt_presentation_to_guest(&p, 700, 500, &x, &y) && x == 1866 && y == 1333);
    CHECK(jpt_presentation_to_guest(&p, 480, 270, &x, &y) && x == 1280 && y == 720);
}

static void test_two_quick_generations_resize_once(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_commit_t c;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    jpt_presentation_request_guest(&p, 1024, 768, 3); /* G: the second request is 4:3 again */
    c = present_frame_of(&p, 2, 1);
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 720) && p.guest.width == 640);
    jpt_presentation_request_guest(&p, 1280, 720, 4);
    c = present_frame_of(&p, 3, 1);
    CHECK(!c.committed && !c.resize && host_is(&p, 960, 720));
    c = present_frame_of(&p, 4, 1);
    CHECK(c.committed && c.resize && host_is(&p, 960, 540) && p.guest.width == 1280);
}

static void test_full_screen_and_manual_sizes_are_immediate(void) {
    jpt_presentation_t p = menu_at_960x720();
    jpt_commit_t c;
    jpt_presentation_request_guest(&p, 2560, 1440, 2);
    CHECK(jpt_presentation_set_host(&p, 1920, 1080)); /* I: F11 does not wait for any frame */
    CHECK(host_is(&p, 1920, 1080) && p.has_pending && p.guest.width == 640 && view_is(&p, 240, 0, 1680, 1080));
    c = present_frame_of(&p, 2, 0); /* full screen: the window is not resized to the game's shape */
    CHECK(c.committed && !c.resize && host_is(&p, 1920, 1080) && view_is(&p, 0, 0, 1920, 1080));
}

/* A fixed window sees every screen the game goes through and keeps its size. */
static void test_guest_screens_never_touch_a_fixed_host(void) {
    static const int steps[][2] = { { 640, 480 }, { 2560, 1440 }, { 640, 480 }, { 1024, 576 }, { 800, 600 },
                                    { 640, 480 }, { 2560, 1440 }, { 640, 480 } };
    jpt_presentation_t p;
    unsigned generation = 1;
    jpt_presentation_init(&p, 0);
    jpt_presentation_set_host(&p, 2560, 1440);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        jpt_presentation_request_guest(&p, steps[i][0], steps[i][1], ++generation);
        present_frame_of(&p, generation, 0);
        CHECK(host_is(&p, 2560, 1440));
        CHECK(p.guest.width == steps[i][0] && p.guest.height == steps[i][1]);
    }
}

/* No frame of an old generation is ever shown in a window of the new one: every window change is made by the commit
 * of the generation being presented. */
static void test_old_frame_never_meets_the_new_window(void) {
    static const unsigned frames[] = { 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 5, 5, 3 };
    static const struct { size_t before_frame; int w, h; unsigned generation; } requests[] = {
        { 0, 2560, 1440, 2 }, { 6, 640, 480, 3 }, { 10, 1920, 1080, 5 }
    };
    jpt_presentation_t p = menu_at_960x720();
    for (size_t i = 0; i < sizeof frames / sizeof frames[0]; i++) {
        jpt_presentation_t before;
        jpt_commit_t c;
        for (size_t r = 0; r < sizeof requests / sizeof requests[0]; r++)
            if (requests[r].before_frame == i)
                jpt_presentation_request_guest(&p, requests[r].w, requests[r].h, requests[r].generation);
        before = p;
        c = present_frame_of(&p, frames[i], 1);
        if (!c.committed) {
            CHECK(host_is(&p, before.host.width, before.host.height) && memcmp(&p.view, &before.view, sizeof p.view) == 0);
            CHECK(p.generation == before.generation && !c.resize);
        } else {
            CHECK(p.generation == frames[i]); /* the window changed only for the generation shown */
        }
    }
    CHECK(host_is(&p, 960, 540) && p.guest.width == 1920 && p.generation == 5);
}

static void test_auto_client_fits_and_scales(void) {
    jpt_size_t big = { 1888, 1062 }, wide = { 960, 540 }, dpi150 = { 1440, 1080 };
    jpt_size_t guest43 = { 1024, 768 }, guest169 = { 2560, 1440 }, none = { 0, 0 };
    jpt_size_t r = jpt_auto_client(big, guest43, FRAME_W, FRAME_H, WORK_W, WORK_H);
    CHECK(r.height == WORK_H - FRAME_H && abs(r.width - 1335) <= 1); /* fits the work area, shape kept */
    r = jpt_auto_client(wide, guest43, FRAME_W, FRAME_H, WORK_W, WORK_H);
    CHECK(r.width == 960 && r.height == 720);
    r = jpt_auto_client(dpi150, guest169, 24, 59, 2880, 1560); /* 150%: the same shape rule */
    CHECK(r.width == 1440 && r.height == 810);
    r = jpt_auto_client(dpi150, guest169, 32, 88, 3840, 2080); /* 225% */
    CHECK(r.width == 1440 && r.height == 810);
    r = jpt_auto_client(wide, none, FRAME_W, FRAME_H, WORK_W, WORK_H);
    CHECK(r.width == 960 && r.height == 540);
    r = jpt_auto_client(none, guest43, FRAME_W, FRAME_H, WORK_W, WORK_H);
    CHECK(r.width == 0 && r.height == 0);
    r = jpt_auto_client(wide, guest169, FRAME_W, FRAME_H, 0, 0); /* an unknown work area never shrinks */
    CHECK(r.width == 960 && r.height == 540);
}

static void test_window_host_follows_only_in_auto(void) {
    char* source = read_file(JPT_SOURCE_DIR "\\src\\host\\window_host.c");
    char* body;
    CHECK(source != NULL);
    if (!source) return;
    body = function_body(source, "static int window_follows_guest(void) {");
    CHECK(body != NULL);
    if (body) {
        CHECK(strstr(body, "fixed_window_size()") && strstr(body, "s.cfg.stretch") && strstr(body, "s.fullscreen") &&
              strstr(body, "IsZoomed") && strstr(body, "IsIconic"));
    }
    body = function_body(source, "void window_host_frame_commit(unsigned generation) {");
    CHECK(body != NULL);
    if (body) {
        char* commit = strstr(body, "jpt_presentation_commit_frame");
        char* resize = strstr(body, "SetWindowPos");
        char* layout = strstr(body, "layout_surface()");
        CHECK(commit && resize && layout && commit < resize && resize < layout);
    }
    free(source);
}

int main(void) {
    test_four_by_three_in_widescreen_hosts();
    test_other_shapes();
    test_stretch_fills_the_client();
    test_view_does_not_depend_on_order();
    test_pillarbox_offsets();
    test_mouse_mapping();
    test_dpi_independence();
    test_degenerate_sizes();
    test_transition_code_does_not_resize_the_window();
    test_request_keeps_the_active_screen();
    test_first_frame_of_the_new_generation_commits();
    test_old_frames_never_recompute_the_view();
    test_mouse_follows_the_active_screen();
    test_mouse_switches_with_the_commit();
    test_only_the_final_generation_commits();
    test_returning_to_the_active_size_cancels();
    test_gameplay_back_to_menu();
    test_host_changes_stay_immediate();
    test_next_view_is_what_the_commit_gives();
    test_requests_with_nothing_shown_or_invalid();
    test_presenters_commit_before_present();
    test_auto_window_waits_for_the_new_generation();
    test_auto_window_returns_with_the_menu_frame();
    test_fixed_window_never_resizes();
    test_mouse_changes_with_the_window();
    test_two_quick_generations_resize_once();
    test_full_screen_and_manual_sizes_are_immediate();
    test_guest_screens_never_touch_a_fixed_host();
    test_old_frame_never_meets_the_new_window();
    test_auto_client_fits_and_scales();
    test_window_host_follows_only_in_auto();
    printf("presentation_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
