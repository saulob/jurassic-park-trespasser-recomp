/* Where the Linux executable reads the game and writes everything else: the defaults next to the program, the
 * environment and the arguments that override them, and the proof that the user folders can be written. Runs
 * headless on Linux; nothing outside the temporary folder is touched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jpt_fs.h"
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

int main(void) {
    test_choice();
    test_prepare();
    printf("linux_paths_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
