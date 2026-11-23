/* The port folder, the log folder and its AppImage fallback, the restart command and the application ID (src/common/jpt_port.c),
 * on Windows and Linux. Scratch folders only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jpt_fs.h"
#include "jpt_port.h"
#include "jpt_user_data.h"

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

static const char* scratch_base(void) {
    const char* t = getenv("TMPDIR");
    if (!t || !*t) t = getenv("TEMP");
    return t && *t ? t : ".";
}

static void test_application_id(void) {
    const char* id = JPT_APPLICATION_ID;
    size_t n = strlen(id);
    CHECK(!strncmp(id, "io.github.saulob.", 17), "the ID is io.github.saulob.<project> (%s)", id);
    CHECK(n > 17 && id[n - 1] != '.' && id[n - 1] != '-', "the project part is not empty (%s)", id);
    for (size_t i = 0; i < n; i++)
        CHECK((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '.' || id[i] == '-',
              "only lower case letters, digits, dots and dashes: %c", id[i]);
}

static void test_port_folder(void) {
    char out[JPT_PORT_PATH_MAX];
    CHECK(jpt_port_folder("/opt/jpt/trespasser_recomp", NULL, out, sizeof out) && !strcmp(out, "/opt/jpt"), "the executable's folder (%s)", out);
    CHECK(jpt_port_folder("/opt/jpt/trespasser_recomp", "", out, sizeof out) && !strcmp(out, "/opt/jpt"), "an empty APPIMAGE is ignored");
    CHECK(jpt_port_folder("C:\\Games\\JPT\\trespasser_recomp.exe", NULL, out, sizeof out) && !strcmp(out, "C:\\Games\\JPT"),
          "a Windows path (%s)", out);
    CHECK(jpt_port_folder("trespasser_recomp", NULL, out, sizeof out) && !strcmp(out, "."), "no folder in the path");
    CHECK(jpt_port_folder("/trespasser_recomp", NULL, out, sizeof out) && !strcmp(out, "/"), "an executable in the root folder");
    CHECK(jpt_port_folder("/tmp/.mount_JptAbc/usr/bin/trespasser_recomp", "/home/p/Games/JPT.AppImage", out, sizeof out) &&
              !strcmp(out, "/home/p/Games"),
          "inside an AppImage: the folder of the AppImage file, not the mount (%s)", out);
    CHECK(jpt_port_folder("/tmp/.mount_jpt123/usr/bin/trespasser_recomp",
                          "/home/deck/Games/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage", out, sizeof out) &&
              !strcmp(out, "/home/deck/Games"),
          "Steam Deck AppImage resolves port root to /home/deck/Games (%s)", out);
    CHECK(jpt_port_folder("/tmp/.mount_jpt123/usr/bin/trespasser_recomp",
                          "/run/media/mmcblk0p1/Games/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage", out,
                          sizeof out) &&
              !strcmp(out, "/run/media/mmcblk0p1/Games"),
          "SD card AppImage resolves port root to SD card folder (%s)", out);
    CHECK(jpt_port_folder("/opt/jpt/trespasser_recomp", NULL, out, 5) == 0, "a folder that does not fit is refused");
}

static void test_gamescope(void) {
    CHECK(jpt_port_is_gamescope("gamescope-0", 0) == 1, "Linux with GAMESCOPE_WAYLAND_DISPLAY is Gamescope");
    CHECK(jpt_port_is_gamescope("", 0) == 0, "empty GAMESCOPE_WAYLAND_DISPLAY is not Gamescope");
    CHECK(jpt_port_is_gamescope(NULL, 0) == 0, "unset GAMESCOPE_WAYLAND_DISPLAY is not Gamescope");
    CHECK(jpt_port_is_gamescope("gamescope-0", 1) == 0, "Windows is never Gamescope");
}

static void test_inside(void) {
    CHECK(jpt_port_path_inside("/tmp/.mount_a/usr/bin/x", "/tmp/.mount_a"), "a file in the mount");
    CHECK(jpt_port_path_inside("/tmp/.mount_a", "/tmp/.mount_a/"), "the mount itself");
    CHECK(!jpt_port_path_inside("/tmp/.mount_ab/usr", "/tmp/.mount_a"), "whole components only");
    CHECK(!jpt_port_path_inside("/home/p/JPT.AppImage", "/tmp/.mount_a"), "a file outside the mount");
    CHECK(!jpt_port_path_inside("/x", ""), "no folder");
}

static void test_logs(void) {
    char root[512], port[600], user[600], out[JPT_PORT_PATH_MAX], logs[700], expected[700];
    FILE* f;
    snprintf(root, sizeof root, "%s/jpt_port_tests_%ld", scratch_base(), (long)time(NULL));
    snprintf(port, sizeof port, "%s/port", root);
    snprintf(user, sizeof user, "%s/port/user_data", root);
    snprintf(logs, sizeof logs, "%s/logs", port);
    jpt_make_dirs(port);

    CHECK(jpt_port_logs_folder("/p", "/p/user_data", "/elsewhere/trespasser_recomp_007.log", 0, out, sizeof out) &&
              !strcmp(out, "/elsewhere"),
          "the folder of the active log (%s)", out);
    snprintf(expected, sizeof expected, "%s/logs", port);
    CHECK(jpt_port_logs_folder(port, user, NULL, 0, out, sizeof out) && !strcmp(out, expected), "<port folder>/logs (%s)", out);
    CHECK(jpt_port_logs_folder(port, user, NULL, 1, out, sizeof out) && !strcmp(out, expected), "also in an AppImage that can write beside itself");
    CHECK(jpt_fs_is_dir(logs), "the writable check created the folder");
    jpt_fs_remove_dir(logs);

    f = fopen(logs, "wb");
    if (f) fclose(f);
    snprintf(expected, sizeof expected, "%s/logs", user);
    CHECK(jpt_port_logs_folder(port, user, NULL, 1, out, sizeof out) && !strcmp(out, expected),
          "an AppImage whose logs folder cannot be used falls back to user_data/logs (%s)", out);
    snprintf(expected, sizeof expected, "%s/logs", port);
    CHECK(jpt_port_logs_folder(port, user, NULL, 0, out, sizeof out) && !strcmp(out, expected), "outside an AppImage there is no fallback");
    CHECK(!jpt_port_logs_folder(port, NULL, NULL, 1, out, sizeof out), "no folder at all when there is nowhere to fall back to");
    CHECK(!jpt_port_folder_writable(logs), "a file is not a writable folder");
    jpt_fs_remove(logs);
    CHECK(jpt_port_folder_writable(logs) && jpt_fs_is_dir(logs), "a folder that can be made is writable");
    jpt_fs_remove_dir(logs);
    jpt_fs_remove_dir(port);
    jpt_fs_remove_dir(root);
}

static void test_relaunch(void) {
    const char* argv[] = { "/tmp/.mount_JptAbc/usr/bin/trespasser_recomp", "--user_data_root", "/data/u", "--renderer=vulkan" };
    const char* out[8];
    int n = jpt_port_relaunch("/home/p/JPT.AppImage", argv[0], 4, argv, "--launcher", out, 8);
    CHECK(n == 5 && !strcmp(out[0], "/home/p/JPT.AppImage") && !strcmp(out[1], "--user_data_root") && !strcmp(out[2], "/data/u") &&
              !strcmp(out[3], "--renderer=vulkan") && !strcmp(out[4], "--launcher") && !out[5],
          "inside an AppImage the AppImage file starts again with the same arguments and the extra one");
    n = jpt_port_relaunch("", "/opt/jpt/trespasser_recomp", 4, argv, NULL, out, 8);
    CHECK(n == 4 && !strcmp(out[0], "/opt/jpt/trespasser_recomp") && !strcmp(out[3], "--renderer=vulkan") && !out[4],
          "outside one the executable starts again");
    CHECK(jpt_port_relaunch(NULL, "", 4, argv, NULL, out, 8) == -1, "no program, no command");
    CHECK(jpt_port_relaunch(NULL, "/x", 4, argv, "--a", out, 4) == -1, "an argument list that does not fit is refused");
    CHECK(jpt_port_relaunch("/home/p/JPT.AppImage", "/x", 0, argv, NULL, out, 8) == -1, "an empty command line is refused");
}

int main(void) {
    test_application_id();
    test_port_folder();
    test_gamescope();
    test_inside();
    test_logs();
    test_relaunch();
    printf("port_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
