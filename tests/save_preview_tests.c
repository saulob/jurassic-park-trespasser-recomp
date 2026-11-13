/* Save pictures (save_preview.c) in a temporary folder: naming, writing over, deleting, unreadable pictures and the
 * picture sizes for every screen shape. */
#include "save_preview.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int* g_checks;
static int* g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        (*g_checks)++;                                                                                                  \
        if (!(cond)) {                                                                                                  \
            (*g_failures)++;                                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static uint16_t* screen(int w, int h, int left_red) {
    uint16_t* px = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)w * h);
    for (int y = 0; px && y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * w + x] = (uint16_t)((x < w / 2) == left_red ? 0xF800 : 0x001F);
    return px;
}

static uint32_t at(const sp_image_t* im, int x, int y) { return im->pixels[(size_t)y * im->width + x]; }

static int reddish(uint32_t c) { return ((c >> 16) & 255) > 180 && (c & 255) < 80; }
static int bluish(uint32_t c) { return (c & 255) > 180 && ((c >> 16) & 255) < 80; }

static void test_names(void) {
    char out[MAX_PATH];
    CHECK(save_preview_path("C:\\game\\user\\SAVEGAME.003", out, sizeof out) &&
              !strcmp(out, "C:\\game\\user\\previews\\SAVEGAME.003.jpg"),
          "picture path %s", out);
    CHECK(!save_preview_path("SAVEGAME.003", out, sizeof out), "a picture path for a bare name");
    CHECK(!save_preview_path("C:\\game\\user\\SAVEGAME.003", out, 12), "a cut picture path");
    CHECK(save_preview_is_save("C:\\game\\user\\SAVEGAME.003", "C:\\game\\user", "SAVEGAME"), "a save not recognised");
    CHECK(save_preview_is_save("c:\\GAME\\user\\savegame.010", "C:\\game\\user\\", "SAVEGAME"), "case or trailing slash");
    CHECK(!save_preview_is_save("C:\\game\\user\\SAVEGAME.03", "C:\\game\\user", "SAVEGAME"), "two digits taken as a save");
    CHECK(!save_preview_is_save("C:\\game\\user\\SAVEGAME.0034", "C:\\game\\user", "SAVEGAME"), "four digits taken as a save");
    CHECK(!save_preview_is_save("C:\\game\\user\\previews\\SAVEGAME.003.jpg", "C:\\game\\user", "SAVEGAME"),
          "a picture taken as a save");
    CHECK(!save_preview_is_save("C:\\game\\user\\temp.scx", "C:\\game\\user", "SAVEGAME"), "a level file taken as a save");
    CHECK(!save_preview_is_save("C:\\other\\SAVEGAME.003", "C:\\game\\user", "SAVEGAME"), "another folder's save");
}

static void test_sizes(void) {
    static const int shapes[][2] = { { 2560, 1920 }, { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 }, { 5120, 1440 },
                                     { 3840, 2160 }, { 640, 480 } };
    for (int i = 0; i < 7; i++) {
        sp_size_t s = save_preview_size(shapes[i][0], shapes[i][1]);
        double a = (double)shapes[i][0] / shapes[i][1], b = (double)s.width / s.height;
        CHECK(fabs(a / b - 1.0) < 0.01, "%dx%d picture %dx%d changes shape", shapes[i][0], shapes[i][1], s.width, s.height);
        CHECK(s.width <= shapes[i][0] && s.height <= shapes[i][1], "a picture larger than its screen");
        if (shapes[i][0] * shapes[i][1] > SAVE_PREVIEW_PIXELS)
            CHECK(fabs((double)s.width * s.height / SAVE_PREVIEW_PIXELS - 1.0) < 0.01, "%dx%d picture %dx%d is not ~960x540 pixels",
                  shapes[i][0], shapes[i][1], s.width, s.height);
    }
}

static void test_files(void) {
    char dir[MAX_PATH], save[MAX_PATH], picture[MAX_PATH];
    sp_image_t im;
    uint16_t *red_left = screen(2560, 1440, 1), *blue_left = screen(2560, 1440, 0);
    FILE* f;
    GetTempPathA(sizeof dir, dir);
    _snprintf(dir + strlen(dir), sizeof dir - strlen(dir) - 1, "jpt_save_preview_%lu", GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    _snprintf(save, sizeof save - 1, "%s\\SAVEGAME.001", dir);
    save_preview_path(save, picture, sizeof picture);
    CHECK(!save_preview_load(save, &im) && !im.pixels, "a missing picture loads");
    CHECK(red_left && blue_left && save_preview_write(save, red_left, 2560, 1440, 2560), "a picture is not written");
    CHECK(GetFileAttributesA(picture) != INVALID_FILE_ATTRIBUTES, "no picture file at %s", picture);
    CHECK(save_preview_load(save, &im) && im.width == save_preview_size(2560, 1440).width &&
              im.height == save_preview_size(2560, 1440).height,
          "the written picture does not load at its size");
    if (im.pixels) {
        CHECK(reddish(at(&im, im.width / 8, im.height / 2)) && bluish(at(&im, im.width * 7 / 8, im.height / 2)),
              "the picture's colours are wrong");
        save_preview_free(&im);
    }
    CHECK(save_preview_write(save, blue_left, 2560, 1440, 2560), "a picture is not written over");
    CHECK(save_preview_load(save, &im) && bluish(at(&im, im.width / 8, im.height / 2)), "the old picture is still shown");
    save_preview_free(&im);
    CHECK(save_preview_delete(save) && GetFileAttributesA(picture) == INVALID_FILE_ATTRIBUTES, "the picture survives Delete");
    CHECK(save_preview_delete(save), "deleting a save without a picture fails");
    CHECK(!save_preview_load(save, &im), "a deleted picture loads");
    f = fopen(picture, "wb");
    if (f) {
        fputs("not a picture", f);
        fclose(f);
    }
    CHECK(!save_preview_load(save, &im) && !im.pixels, "a damaged picture loads");
    DeleteFileA(picture);
    CHECK(!save_preview_write("Q:\\no\\such\\folder\\SAVEGAME.000", red_left, 2560, 1440, 2560),
          "a picture is written where it cannot be");
    CHECK(!save_preview_write(save, NULL, 2560, 1440, 2560), "a picture is written without a screen");
    {
        char previews[MAX_PATH];
        _snprintf(previews, sizeof previews - 1, "%s\\previews", dir);
        RemoveDirectoryA(previews);
        RemoveDirectoryA(dir);
    }
    free(red_left);
    free(blue_left);
}

static void test_shrink(void) {
    uint16_t* px = screen(5120, 1440, 1);
    sp_image_t im;
    CHECK(px && save_preview_shrink(px, 5120, 1440, 5120, &im) && im.width == save_preview_size(5120, 1440).width,
          "a 32:9 screen is not shrunk");
    if (im.pixels) {
        CHECK(reddish(at(&im, 2, im.height / 2)) && bluish(at(&im, im.width - 3, im.height / 2)), "shrunk colours are wrong");
        save_preview_free(&im);
    }
    free(px);
}

void save_preview_tests(int* checks, int* failures) {
    g_checks = checks;
    g_failures = failures;
    test_names();
    test_sizes();
    test_files();
    test_shrink();
}
