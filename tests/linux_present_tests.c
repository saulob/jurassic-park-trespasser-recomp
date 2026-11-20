/* The Linux presentation policy: where the guest's frame lands in the host window (jpt_presentation, letterboxed) for the sizes the
 * game and its users meet, how a host pixel maps back to a guest pixel, and the stable transition rule (a new guest screen size
 * only takes effect with the first frame of that size). Pure: the virtual backend shows nothing. */
#include <stdio.h>
#include <string.h>

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

static int view_is(lx_view_t v, int x, int y, int w, int h) { return v.x == x && v.y == y && v.w == w && v.h == h; }

static void test_views(void) {
    lx_view_t v;
    v = lx_present_view(640, 480, 640, 480, 0);
    CHECK(view_is(v, 0, 0, 640, 480), "640x480 in 640x480 fills the window (%d,%d %dx%d)", v.x, v.y, v.w, v.h);
    v = lx_present_view(800, 600, 640, 480, 0);
    CHECK(view_is(v, 0, 0, 800, 600), "640x480 in 800x600 scales to 800x600 (%d,%d %dx%d)", v.x, v.y, v.w, v.h);
    v = lx_present_view(1280, 720, 640, 480, 0);
    CHECK(view_is(v, 160, 0, 960, 720), "640x480 in 1280x720 is letterboxed 960x720 at x=160 (%d,%d %dx%d)", v.x, v.y, v.w, v.h);
    v = lx_present_view(2560, 1440, 640, 480, 0);
    CHECK(view_is(v, 320, 0, 1920, 1440), "640x480 in 2560x1440 is 1920x1440 at x=320 (%d,%d %dx%d)", v.x, v.y, v.w, v.h);
    v = lx_present_view(2560, 1440, 640, 480, 1);
    CHECK(view_is(v, 0, 0, 2560, 1440), "stretch fills the window");
    v = lx_present_view(640, 480, 1280, 720, 0);
    CHECK(view_is(v, 0, 60, 640, 360), "a 16:9 screen in a 4:3 window is bars above and below (%d,%d %dx%d)", v.x, v.y, v.w, v.h);
    v = lx_present_view(0, 0, 640, 480, 0);
    CHECK(v.w >= 0 && v.h >= 0, "an empty window gives an empty view");
}

static void test_mapping(void) {
    int gx = -1, gy = -1;
    CHECK(lx_present_to_guest(1280, 720, 640, 480, 0, 640, 360, &gx, &gy) && gx == 320 && gy == 240, "the centre of the window is the centre of the screen (%d,%d)", gx, gy);
    CHECK(!lx_present_to_guest(1280, 720, 640, 480, 0, 100, 300, &gx, &gy), "a pixel in the left bar is outside the screen");
    CHECK(lx_present_to_guest(1280, 720, 640, 480, 0, 160, 0, &gx, &gy) && gx == 0 && gy == 0, "the picture's top-left pixel is guest (0,0)");
    CHECK(lx_present_to_guest(1280, 720, 640, 480, 0, 1119, 719, &gx, &gy) && gx == 639 && gy == 479, "and its bottom-right pixel is guest (639,479) (%d,%d)", gx, gy);
    CHECK(lx_present_to_guest(640, 480, 640, 480, 0, 17, 33, &gx, &gy) && gx == 17 && gy == 33, "1:1 maps identically");
    CHECK(lx_present_to_guest(2560, 1440, 640, 480, 0, 1280, 720, &gx, &gy) && gx == 320 && gy == 240, "2560x1440 centre");
}

static void test_transition(void) {
    static uint8_t px[800 * 600 * 2];
    lx_frame_t f;
    int gx, gy;
    lx_virtual_reset();
    lx_screen_reset();
    lx_window_set_backend(NULL);
    CHECK(lx_screen_mode().width == 640 && lx_screen_mode().height == 480 && lx_screen_mode().bpp == 16, "the default guest screen is 640x480x16");
    CHECK(lx_screen_window_open("t", 1280, 720) && lx_virtual_state()->opened && lx_virtual_state()->width == 1280, "the host window is opened at its own size");
    f.pixels = px, f.width = 640, f.height = 480, f.pitch = 1280, f.format = LX_PF_RGB565, f.generation = 0;
    lx_screen_present(&f);
    CHECK(lx_virtual_state()->presents == 1 && lx_virtual_state()->last.width == 640, "a 640x480 frame is presented");
    CHECK(lx_screen_client_to_guest(640, 360, &gx, &gy) && gx == 320 && gy == 240, "the mouse maps through the 640x480 view");

    lx_screen_set_mode(800, 600, 16);
    CHECK(lx_screen_client_to_guest(640, 360, &gx, &gy) && gx == 320 && gy == 240, "a mode change alone does not move the view");
    lx_screen_present(&f);
    CHECK(lx_screen_client_to_guest(640, 360, &gx, &gy) && gx == 320 && gy == 240, "a frame of the old size does not commit the new screen");
    f.width = 800, f.height = 600, f.pitch = 1600;
    lx_screen_present(&f);
    CHECK(lx_screen_client_to_guest(640, 360, &gx, &gy) && gx == 400 && gy == 300, "the first 800x600 frame commits it: the centre is now (400,300) (%d,%d)", gx, gy);
    CHECK(lx_virtual_state()->last.generation == 1 && lx_screen_frames() == 3, "the frame carries its generation (%u), three frames in all", lx_virtual_state()->last.generation);

    lx_screen_set_client_size(1920, 1080);
    CHECK(lx_virtual_state()->width == 1920 && lx_screen_client_to_guest(960, 540, &gx, &gy) && gx == 400 && gy == 300, "the host size follows the user at once");
    lx_screen_window_close();
    CHECK(!lx_virtual_state()->opened, "the window closes");
}

int main(void) {
    test_views();
    test_mapping();
    test_transition();
    printf("linux_present_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
