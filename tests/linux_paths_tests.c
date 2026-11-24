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
#include "linux_log.h"
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

static void test_paths_with_spaces_and_unicode(void) {
    const char* mount = "/tmp/.mount_jpt999";
    const char* exe = "/tmp/.mount_jpt999/usr/bin/trespasser_recomp";
    const char* spaced_img = "/home/user/My Games/Jurassic Park Trespasser/JPT.AppImage";
    const char* utf8_img = "/home/user/\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B/Jurassic Park/JPT.AppImage";
    lx_paths_t p;
    char err[200];
    clear_environment();

    CHECK(lx_paths_resolve_for(&p, exe, spaced_img, NULL, NULL, NULL, err, sizeof err), "AppImage in a folder with spaces resolves");
    CHECK(!strcmp(p.game_dir, "/home/user/My Games/Jurassic Park Trespasser/game") &&
              !strcmp(p.saves_dir, "/home/user/My Games/Jurassic Park Trespasser/user_data/saves") && !inside(p.game_dir, mount),
          "paths with spaces stay outside the AppImage mount (%s)", p.game_dir);

    CHECK(lx_paths_resolve_for(&p, exe, utf8_img, NULL, NULL, NULL, err, sizeof err), "AppImage in a non-ASCII UTF-8 folder resolves");
    CHECK(!strcmp(p.game_dir, "/home/user/\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B/Jurassic Park/game") && !inside(p.game_dir, mount),
          "non-ASCII UTF-8 paths are preserved outside the AppImage mount (%s)", p.game_dir);
    clear_environment();
}

static int write_text_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

static int read_text_file(const char* path, char* out, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return 1;
}

static int ends_with(const char* s, const char* suffix) {
    size_t sl = strlen(s), sufl = strlen(suffix);
    return sl >= sufl && strcmp(s + sl - sufl, suffix) == 0;
}

static void test_linux_log(void) {
    char root[256], dir[400], file[512], buf[1024];
    const char* tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/jpt_linux_log_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    snprintf(dir, sizeof dir, "%s/logs", root);
    jpt_fs_mkdir(root);
    jpt_fs_mkdir(dir);
    lx_log_quiet(1);

    CHECK(lx_log_open(dir, NULL), "first numbered Linux log opens");
    CHECK(ends_with(lx_log_path(), "/trespasser_recomp_001.log") && !strstr(lx_log_path(), "trespasser_linux_"),
          "first log is trespasser_recomp_001.log (%s)", lx_log_path());
    lx_log("test", "first-run");
    lx_log_close();

    CHECK(lx_log_open(dir, NULL), "second numbered Linux log opens");
    CHECK(ends_with(lx_log_path(), "/trespasser_recomp_002.log"), "second log is trespasser_recomp_002.log (%s)", lx_log_path());
    lx_log("test", "second-run");
    lx_log_close();

    snprintf(file, sizeof file, "%s/trespasser_recomp_001.log", dir);
    CHECK(read_text_file(file, buf, sizeof buf) && strstr(buf, "first-run") && !strstr(buf, "second-run"),
          "first log preserved without overwrite");

    snprintf(file, sizeof file, "%s/trespasser_recomp_005.log", dir);
    CHECK(write_text_file(file, "existing-005\n"), "create gap fixture 005");
    CHECK(lx_log_open(dir, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_006.log"),
          "gap 003-004 is skipped, next is 006 (%s)", lx_log_path());
    lx_log_close();

    snprintf(file, sizeof file, "%s/trespasser_linux_010.log", dir);
    CHECK(write_text_file(file, "legacy-linux\n"), "create legacy Linux log 010");
    CHECK(lx_log_open(dir, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_011.log"),
          "legacy trespasser_linux_010 advances to 011 (%s)", lx_log_path());
    lx_log_close();

    snprintf(file, sizeof file, "%s/trespasser_015.log", dir);
    CHECK(write_text_file(file, "legacy-win\n"), "create legacy trespasser 015");
    CHECK(lx_log_open(dir, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_016.log"),
          "legacy trespasser_015 advances to 016 (%s)", lx_log_path());
    lx_log_close();

    snprintf(file, sizeof file, "%s/trespasser_recomp_020.dmp", dir);
    CHECK(write_text_file(file, "MDMP"), "create crash dump 020");
    snprintf(file, sizeof file, "%s/trespasser_recomp_025_2.dmp", dir);
    CHECK(write_text_file(file, "MDMP"), "create secondary crash dump 025_2");
    CHECK(lx_log_open(dir, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_026.log"),
          "crash dumps advance numbering to 026 (%s)", lx_log_path());
    lx_log_close();

    snprintf(file, sizeof file, "%s/trespasser_recomp_026.log", dir);
    CHECK(write_text_file(file, "untouched-026\n"), "mark 026 content");
    CHECK(lx_log_open_from(dir, 26) && ends_with(lx_log_path(), "/trespasser_recomp_027.log"),
          "EEXIST on 026 retries and creates 027 (%s)", lx_log_path());
    lx_log_close();
    CHECK(read_text_file(file, buf, sizeof buf) && !strcmp(buf, "untouched-026\n"), "026 content untouched after EEXIST");

    snprintf(file, sizeof file, "%s/not_a_dir", root);
    CHECK(write_text_file(file, "blocker"), "create blocker file");
    {
        char bad_dir[sizeof file + sizeof "/child"];
        int written = snprintf(bad_dir, sizeof bad_dir, "%s/child", file);
        CHECK(written > 0 && (size_t)written < sizeof bad_dir, "child under a file path fits its buffer");
        CHECK(!lx_log_open(bad_dir, NULL), "path under regular file fails");
        CHECK(lx_log_fd() == -1 && !lx_log_path()[0], "failed open leaves no fd or stale path");
    }
    if (geteuid() != 0) {
        char ro_dir[400];
        snprintf(ro_dir, sizeof ro_dir, "%s/readonly", root);
        jpt_fs_mkdir(ro_dir);
        chmod(ro_dir, 0555);
        CHECK(!lx_log_open(ro_dir, NULL), "read-only log directory fails");
        chmod(ro_dir, 0755);
        jpt_fs_remove_dir(ro_dir);
    }
    CHECK(!lx_log_open("", NULL), "empty dir fails");
    CHECK(!lx_log_open(dir, ""), "empty --log-file fails");

    snprintf(file, sizeof file, "%s/explicit_session.log", root);
    CHECK(write_text_file(file, "prior-line\n"), "seed explicit log file");
    CHECK(lx_log_open(dir, file) && !strcmp(lx_log_path(), file), "--log-file opens explicit path");
    lx_log("explicit", "appended-line");
    lx_log_close();
    CHECK(read_text_file(file, buf, sizeof buf) && strstr(buf, "prior-line") && strstr(buf, "appended-line"),
          "--log-file appends without truncating");
    jpt_fs_remove(file);

    {
        char spaced[450], unicode[450];
        snprintf(spaced, sizeof spaced, "%s/My Logs Folder", root);
        snprintf(unicode, sizeof unicode, "%s/\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B \xD0\x9B\xD0\xBE\xD0\xB3\xD0\xB8", root);
        CHECK(lx_log_open(spaced, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_001.log"),
              "directory with spaces creates trespasser_recomp_001.log (%s)", lx_log_path());
        snprintf(file, sizeof file, "%s", lx_log_path());
        lx_log_close();
        jpt_fs_remove(file);
        jpt_fs_remove_dir(spaced);

        CHECK(lx_log_open(unicode, NULL) && ends_with(lx_log_path(), "/trespasser_recomp_001.log"),
              "directory with Unicode creates trespasser_recomp_001.log (%s)", lx_log_path());
        snprintf(file, sizeof file, "%s", lx_log_path());
        lx_log_close();
        jpt_fs_remove(file);
        jpt_fs_remove_dir(unicode);
    }

    lx_log_quiet(0);
    {
        const char* files[] = {
            "logs/trespasser_recomp_001.log", "logs/trespasser_recomp_002.log", "logs/trespasser_recomp_005.log",
            "logs/trespasser_recomp_006.log", "logs/trespasser_linux_010.log",  "logs/trespasser_recomp_011.log",
            "logs/trespasser_015.log",        "logs/trespasser_recomp_016.log", "logs/trespasser_recomp_020.dmp",
            "logs/trespasser_recomp_025_2.dmp", "logs/trespasser_recomp_026.log", "logs/trespasser_recomp_027.log",
            "not_a_dir"
        };
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            snprintf(file, sizeof file, "%s/%s", root, files[i]);
            jpt_fs_remove(file);
        }
    }
    jpt_fs_remove_dir(dir);
    jpt_fs_remove_dir(root);
}

int main(void) {
    test_choice();
    test_prepare();
    test_appimage();
    test_appimage_logs();
    test_relaunch();
    test_steam_deck_appimage_and_sd_card();
    test_paths_with_spaces_and_unicode();
    test_linux_log();
    printf("linux_paths_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
