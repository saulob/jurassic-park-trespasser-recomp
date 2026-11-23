/* Where the Linux executable reads the game and writes everything else: the defaults next to the program, the
 * environment and the arguments that override them, and the proof that the user folders can be written. Runs
 * headless on Linux; nothing outside the temporary folder is touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpt_fs.h"
#include "jpt_port.h"
#include "linux_paths.h"

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

static void clear_environment(void) {
    unsetenv("TRESPASSER_GAME_DIR");
    unsetenv("TRESPASSER_USER_DATA_ROOT");
    unsetenv("TRESPASSER_LOG_DIR");
    unsetenv("APPIMAGE");
}

static void test_choice(void) {
    lx_paths_t p;
    char err[200];
    clear_environment();
    CHECK(lx_paths_resolve(&p, "/opt/trespasser/trespasser_recomp", NULL, NULL, NULL, err, sizeof err), "defaults");
    CHECK(!strcmp(p.port_dir, "/opt/trespasser"), "the program folder is the executable's (%s)", p.port_dir);
    CHECK(!strcmp(p.game_dir, "/opt/trespasser/game") && !strcmp(p.user_root, "/opt/trespasser/user_data") &&
              !strcmp(p.logs_dir, "/opt/trespasser/logs"),
          "game, user_data and logs sit next to the program");
    CHECK(!strcmp(p.launcher_dir, "/opt/trespasser/user_data/launcher") && !strcmp(p.saves_dir, "/opt/trespasser/user_data/saves") &&
              !strcmp(p.cache_dir, "/opt/trespasser/user_data/cache"),
          "the user_data layout of the Windows build");

    setenv("TRESPASSER_GAME_DIR", "/data/game", 1);
    setenv("TRESPASSER_USER_DATA_ROOT", "/home/p/tr", 1);
    setenv("TRESPASSER_LOG_DIR", "/var/tmp/trlogs", 1);
    CHECK(lx_paths_resolve(&p, "/opt/trespasser/trespasser_recomp", NULL, NULL, NULL, err, sizeof err), "environment");
    CHECK(!strcmp(p.game_dir, "/data/game") && !strcmp(p.user_root, "/home/p/tr") && !strcmp(p.logs_dir, "/var/tmp/trlogs"),
          "the environment overrides the defaults");
    CHECK(!strcmp(p.saves_dir, "/home/p/tr/saves"), "and the layout follows the user data root");
    CHECK(lx_paths_resolve(&p, "/opt/trespasser/trespasser_recomp", "/arg/game", "/arg/user", "/arg/logs", err, sizeof err) &&
              !strcmp(p.game_dir, "/arg/game") && !strcmp(p.user_root, "/arg/user") && !strcmp(p.logs_dir, "/arg/logs"),
          "an argument overrides the environment");
    clear_environment();

    CHECK(lx_paths_resolve(&p, "trespasser_recomp", NULL, NULL, NULL, err, sizeof err) && !strcmp(p.port_dir, "."), "no folder in the path");
    CHECK(lx_paths_resolve(&p, "/trespasser_recomp", NULL, NULL, NULL, err, sizeof err) && !strcmp(p.game_dir, "/game") &&
              !strcmp(p.port_dir, "/"),
          "an executable in the root folder");
    {
        char longpath[LX_PATH_MAX + 50];
        memset(longpath, 'a', sizeof longpath - 1);
        longpath[0] = '/';
        longpath[sizeof longpath - 1] = 0;
        CHECK(!lx_paths_resolve(&p, "/x/y", longpath, NULL, NULL, err, sizeof err) && strstr(err, "too long"), "a game folder name that is too long");
        CHECK(!lx_paths_resolve(&p, "/x/y", NULL, longpath, NULL, err, sizeof err) && strstr(err, "too long"), "a user data folder name that is too long");
    }
}

static void test_prepare(void) {
    char root[256], err[200], blocked[400];
    lx_paths_t p;
    const char* tmp = getenv("TMPDIR");
    FILE* f;
    clear_environment();
    snprintf(root, sizeof root, "%s/jpt_linux_paths_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    jpt_fs_mkdir(root);
    {
        char exe[400], user[400];
        snprintf(exe, sizeof exe, "%s/trespasser_recomp", root);
        snprintf(user, sizeof user, "%s/ud", root);
        CHECK(lx_paths_resolve(&p, exe, NULL, user, NULL, err, sizeof err), "paths under the temporary folder");
    }
    CHECK(lx_paths_prepare(&p, err, sizeof err), "prepare creates and proves the folders (%s)", err);
    CHECK(jpt_fs_is_dir(p.launcher_dir) && jpt_fs_is_dir(p.saves_dir) && jpt_fs_is_dir(p.cache_dir) && jpt_fs_is_dir(p.logs_dir),
          "config, saves, cache and logs exist");
    {
        char probe[LX_PATH_MAX + 32];
        snprintf(probe, sizeof probe, "%s/.write_probe", p.saves_dir);
        CHECK(!jpt_fs_exists(probe), "and the probe file is gone");
    }
    CHECK(lx_paths_prepare(&p, err, sizeof err), "prepare again over existing folders");

    snprintf(blocked, sizeof blocked, "%s/blocked", root);
    f = fopen(blocked, "wb");
    if (f) fclose(f);
    {
        char user[600];
        snprintf(user, sizeof user, "%s/inside_a_file", blocked);
        CHECK(lx_paths_resolve(&p, "/x/trespasser_recomp", NULL, user, NULL, err, sizeof err), "a root below a file");
        CHECK(!lx_paths_prepare(&p, err, sizeof err) && strstr(err, "cannot create folder"), "cannot be created: an error, not another folder (%s)", err);
    }
    if (geteuid() != 0) {
        char readonly_root[400], user[600];
        snprintf(readonly_root, sizeof readonly_root, "%s/readonly", root);
        jpt_fs_mkdir(readonly_root);
        chmod(readonly_root, 0555);
        snprintf(user, sizeof user, "%s/ud", readonly_root);
        CHECK(lx_paths_resolve(&p, "/x/trespasser_recomp", NULL, user, NULL, err, sizeof err), "a read-only parent");
        CHECK(!lx_paths_prepare(&p, err, sizeof err), "an unwritable location is refused (%s)", err);
        chmod(readonly_root, 0755);
        jpt_fs_remove_dir(readonly_root);
    }
    jpt_fs_remove(blocked);
    {
        const char* leaves[] = { "ud/launcher", "ud/saves", "ud/cache", "logs", "ud" };
        for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
            char path[400];
            snprintf(path, sizeof path, "%s/%s", root, leaves[i]);
            jpt_fs_remove_dir(path);
        }
    }
    jpt_fs_remove_dir(root);
}

static int inside(const char* path, const char* folder) {
    size_t n = strlen(folder);
    return !strncmp(path, folder, n) && (path[n] == 0 || path[n] == '/');
}

static void test_appimage(void) {
    const char* mount = "/tmp/.mount_TrespAbCdEf";
    const char* exe = "/tmp/.mount_TrespAbCdEf/usr/bin/trespasser_recomp";
    const char* image = "/home/player/Games/Trespasser/JurassicParkTrespasser-x86_64.AppImage";
    lx_paths_t p;
    char err[200];
    clear_environment();
    CHECK(lx_paths_resolve_for(&p, exe, image, NULL, NULL, NULL, err, sizeof err), "AppImage paths");
    CHECK(!strcmp(p.port_dir, "/home/player/Games/Trespasser"), "the program folder is the AppImage file's folder (%s)", p.port_dir);
    CHECK(!strcmp(p.game_dir, "/home/player/Games/Trespasser/game") && !strcmp(p.user_root, "/home/player/Games/Trespasser/user_data") &&
              !strcmp(p.logs_dir, "/home/player/Games/Trespasser/logs"),
          "game, user_data and logs sit beside the AppImage file");
    CHECK(!inside(p.game_dir, mount) && !inside(p.user_root, mount) && !inside(p.logs_dir, mount) && !inside(p.saves_dir, mount) &&
              !inside(p.launcher_dir, mount) && !inside(p.cache_dir, mount),
          "nothing resolves inside the read-only mount");
    CHECK(!strcmp(p.appimage, image), "the AppImage file is kept for the restart");

    setenv("APPIMAGE", image, 1);
    CHECK(lx_paths_resolve(&p, exe, NULL, NULL, NULL, err, sizeof err) && !strcmp(p.port_dir, "/home/player/Games/Trespasser"),
          "APPIMAGE in the environment decides the same");
    clear_environment();

    CHECK(lx_paths_resolve_for(&p, exe, image, "/data/game", "/data/user", "/data/logs", err, sizeof err) &&
              !strcmp(p.game_dir, "/data/game") && !strcmp(p.user_root, "/data/user") && !strcmp(p.logs_dir, "/data/logs") && p.logs_explicit,
          "the folder arguments still win inside an AppImage");
    CHECK(lx_paths_resolve_for(&p, "/opt/jpt/trespasser_recomp", "", NULL, NULL, NULL, err, sizeof err) &&
              !strcmp(p.port_dir, "/opt/jpt") && !p.appimage[0],
          "an empty APPIMAGE is not an AppImage");
}

static void test_appimage_logs(void) {
    char root[256], err[200], exe[400], image[500], beside[400];
    lx_paths_t p;
    const char* tmp = getenv("TMPDIR");
    clear_environment();
    snprintf(root, sizeof root, "%s/jpt_appimage_paths_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    snprintf(beside, sizeof beside, "%s/beside", root);
    snprintf(image, sizeof image, "%s/Trespasser.AppImage", beside);
    snprintf(exe, sizeof exe, "%s/mount/usr/bin/trespasser_recomp", root);
    jpt_fs_mkdir(root);
    jpt_fs_mkdir(beside);
    {
        char logs_blocker[420];
        FILE* f;
        snprintf(logs_blocker, sizeof logs_blocker, "%s/logs", beside);
        f = fopen(logs_blocker, "wb");
        if (f) fclose(f);
        CHECK(lx_paths_resolve_for(&p, exe, image, NULL, NULL, NULL, err, sizeof err), "AppImage beside a logs file");
        CHECK(lx_paths_prepare(&p, err, sizeof err), "a log folder that cannot be created falls back (%s)", err);
        {
            char expected[420];
            snprintf(expected, sizeof expected, "%s/user_data/logs", beside);
            CHECK(!strcmp(p.logs_dir, expected) && jpt_fs_is_dir(p.logs_dir), "to user_data/logs beside the AppImage (%s)", p.logs_dir);
        }
        {
            char plain[420];
            snprintf(plain, sizeof plain, "%s/trespasser_recomp", beside);
            CHECK(lx_paths_resolve_for(&p, plain, NULL, NULL, NULL, NULL, err, sizeof err) && !lx_paths_prepare(&p, err, sizeof err),
                  "outside an AppImage the same trouble is an error, not a fallback");
        }
        {
            char named[420];
            snprintf(named, sizeof named, "%s/logs", beside);
            CHECK(lx_paths_resolve_for(&p, exe, image, NULL, NULL, named, err, sizeof err) && !lx_paths_prepare(&p, err, sizeof err),
                  "a log folder named by the player is never replaced");
        }
        jpt_fs_remove(logs_blocker);
    }
    {
        const char* leaves[] = { "beside/user_data/launcher", "beside/user_data/saves", "beside/user_data/cache",
                                 "beside/user_data/logs", "beside/user_data", "beside/logs", "beside", "mount/usr/bin", "mount/usr", "mount" };
        for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
            char path[500];
            snprintf(path, sizeof path, "%s/%s", root, leaves[i]);
            jpt_fs_remove_dir(path);
        }
    }
    jpt_fs_remove_dir(root);
}

static void test_relaunch(void) {
    const char* argv[] = { "/tmp/.mount_x/usr/bin/trespasser_recomp", "--game", "/data", NULL };
    const char* out[8];
    int n;
    n = jpt_port_relaunch("/home/p/JPT.AppImage", argv[0], 3, argv, "--launcher", out, 8);
    CHECK(n == 4 && !strcmp(out[0], "/home/p/JPT.AppImage") && !strcmp(out[1], "--game") && !strcmp(out[3], "--launcher") && !out[4],
          "inside an AppImage the AppImage file starts again with the same arguments");
    n = jpt_port_relaunch(NULL, "/opt/jpt/trespasser_recomp", 3, argv, NULL, out, 8);
    CHECK(n == 3 && !strcmp(out[0], "/opt/jpt/trespasser_recomp") && !out[3], "outside one the executable starts again");
}

static void test_steam_deck_appimage_and_sd_card(void) {
    const char* mount = "/tmp/.mount_jpt123";
    const char* exe = "/tmp/.mount_jpt123/usr/bin/trespasser_recomp";
    const char* deck_img = "/home/deck/Games/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage";
    const char* sd_img = "/run/media/mmcblk0p1/Games/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage";
    lx_paths_t p;
    char err[200];
    clear_environment();

    setenv("APPIMAGE", deck_img, 1);
    CHECK(lx_paths_resolve(&p, exe, NULL, NULL, NULL, err, sizeof err), "Steam Deck simulated APPIMAGE resolves");
    CHECK(!strcmp(p.port_dir, "/home/deck/Games") && !strcmp(p.user_root, "/home/deck/Games/user_data") &&
              !strcmp(p.logs_dir, "/home/deck/Games/logs") && !strcmp(p.game_dir, "/home/deck/Games/game"),
          "Steam Deck paths sit beside the AppImage (%s)", p.port_dir);
    CHECK(!inside(p.port_dir, mount) && !inside(p.user_root, mount) && !inside(p.logs_dir, mount) && !inside(p.game_dir, mount),
          "Steam Deck paths never point inside /tmp/.mount_*");

    setenv("APPIMAGE", sd_img, 1);
    CHECK(lx_paths_resolve(&p, exe, NULL, NULL, NULL, err, sizeof err), "SD card simulated APPIMAGE resolves");
    CHECK(!strcmp(p.port_dir, "/run/media/mmcblk0p1/Games") &&
              !strcmp(p.user_root, "/run/media/mmcblk0p1/Games/user_data") &&
              !strcmp(p.logs_dir, "/run/media/mmcblk0p1/Games/logs") &&
              !strcmp(p.game_dir, "/run/media/mmcblk0p1/Games/game"),
          "SD card paths work with no /home/deck assumption (%s)", p.port_dir);
    CHECK(!inside(p.port_dir, mount) && !inside(p.user_root, mount) && !inside(p.logs_dir, mount) && !inside(p.game_dir, mount),
          "SD card paths never point inside /tmp/.mount_*");
    clear_environment();
}

int main(void) {
    test_choice();
    test_prepare();
    test_appimage();
    test_appimage_logs();
    test_relaunch();
    test_steam_deck_appimage_and_sd_card();
    printf("linux_paths_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
