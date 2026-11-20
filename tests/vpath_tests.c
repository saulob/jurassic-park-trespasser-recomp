/* Guest path resolution over a case-sensitive host: case-insensitive matching inside the mapped root, separators,
 * containment, create mode and the directory cache. Builds its own folder tree under the temporary directory. No
 * Windows header; runs on Windows and Linux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"
#include "jpt_vpath.h"

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

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

static char s_root[256];

static void make_file(const char* relative) {
    char path[JPT_PATH_MAX];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", s_root, relative);
    f = fopen(path, "wb");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
}

static void make_dir(const char* relative) {
    char path[JPT_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s_root, relative);
    jpt_fs_mkdir(path);
}

static void remove_file(const char* relative) {
    char path[JPT_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s_root, relative);
    jpt_fs_remove(path);
}

static void remove_dir(const char* relative) {
    char path[JPT_PATH_MAX];
    if (*relative) snprintf(path, sizeof path, "%s/%s", s_root, relative);
    else snprintf(path, sizeof path, "%s", s_root);
    jpt_fs_remove_dir(path);
}

static void build_tree(void) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(s_root, sizeof s_root, "%s/jpt_vpath_tests_%d", tmp, (int)getpid());
    jpt_fs_mkdir(s_root);
    make_dir("Data");
    make_dir("Data/Menu");
    make_dir("savegame");
    make_file("Data/Menu/Title.TGA");
    make_file("Data/Stream.tpa");
    make_file("trespass.exe");
}

static void clean_tree(void) {
    remove_file("Data/Menu/Title.TGA");
    remove_file("Data/Menu/new.ddf");
    remove_file("Data/Menu/NEW.DDF");
    remove_file("Data/Stream.tpa");
    remove_file("trespass.exe");
    remove_file("savegame/SAVEGAME.001");
    remove_dir("Data/Menu");
    remove_dir("Data");
    remove_dir("savegame");
    remove_dir("");
}

static const char* tail_of(const char* host) {
    size_t n = strlen(s_root);
    return strncmp(host, s_root, n) == 0 ? host + n : host;
}

static int resolves_to(jpt_vpath_t* v, const char* guest, const char* expected_tail) {
    char host[JPT_PATH_MAX];
    int r = jpt_vpath_resolve(v, guest, 0, host, sizeof host);
    if (r != JPT_VP_OK) {
        printf("  %s -> error %d\n", guest, r);
        return 0;
    }
    if (strcmp(tail_of(host), expected_tail) != 0) printf("  %s -> %s\n", guest, tail_of(host));
    return strcmp(tail_of(host), expected_tail) == 0;
}

static int fails_with(jpt_vpath_t* v, const char* guest, int create, int expected) {
    char host[JPT_PATH_MAX];
    int r = jpt_vpath_resolve(v, guest, create, host, sizeof host);
    if (r != expected) printf("  %s -> %d, wanted %d\n", guest, r, expected);
    return r == expected;
}

static void test_case_and_separators(void) {
    jpt_vpath_t* v = jpt_vpath_new();
    CHECK(jpt_vpath_mount(v, 'c', s_root), "mounting a drive");
    CHECK(!jpt_vpath_mount(v, 'C', s_root), "a drive is mounted once");
    CHECK(!strcmp(jpt_vpath_cwd(v), "C:\\"), "the first drive is current, at its root");

    CHECK(resolves_to(v, "Data\\Menu\\Title.TGA", "/Data/Menu/Title.TGA"), "exact spelling, backslashes");
    CHECK(resolves_to(v, "data\\menu\\title.tga", "/Data/Menu/Title.TGA"), "lower case");
    CHECK(resolves_to(v, "DATA/MENU/TITLE.TGA", "/Data/Menu/Title.TGA"), "upper case, slashes");
    CHECK(resolves_to(v, "dAtA\\\\menu//tItLe.tga", "/Data/Menu/Title.TGA"), "mixed case, repeated separators");
    CHECK(resolves_to(v, "C:\\data\\stream.TPA", "/Data/Stream.tpa"), "an absolute path with a drive");
    CHECK(resolves_to(v, "c:/DATA/stream.tpa", "/Data/Stream.tpa"), "lower-case drive, slashes");
    CHECK(resolves_to(v, "\\data\\stream.tpa", "/Data/Stream.tpa"), "rooted without a drive: the current drive");
    CHECK(resolves_to(v, ".\\data\\.\\menu\\", "/Data/Menu"), ". and a trailing separator");
    CHECK(resolves_to(v, "data\\menu\\..\\stream.tpa", "/Data/Stream.tpa"), ".. inside the tree");
    CHECK(resolves_to(v, "C:\\", ""), "the root itself");
    CHECK(fails_with(v, "data\\menu\\missing.tga", 0, JPT_VP_NOT_FOUND), "a missing file");
    CHECK(fails_with(v, "nodir\\file.tga", 0, JPT_VP_NOT_FOUND), "a missing folder");
    jpt_vpath_free(v);
}

static void test_containment(void) {
    jpt_vpath_t* v = jpt_vpath_new();
    jpt_vpath_mount(v, 'C', s_root);
    CHECK(fails_with(v, "..\\etc\\passwd", 0, JPT_VP_ESCAPE), ".. above the root");
    CHECK(fails_with(v, "data\\..\\..\\x", 0, JPT_VP_ESCAPE), ".. that climbs out after descending");
    CHECK(fails_with(v, "C:\\..", 0, JPT_VP_ESCAPE), "C:\\.. is not the parent of the mount");
    CHECK(fails_with(v, "../../../../etc/passwd", 1, JPT_VP_ESCAPE), "containment in create mode too");
    CHECK(fails_with(v, "D:\\data", 0, JPT_VP_BAD_PATH), "an unmounted drive");
    CHECK(fails_with(v, "", 0, JPT_VP_BAD_PATH), "an empty path");
    CHECK(fails_with(v, "data\nmenu", 0, JPT_VP_BAD_PATH), "a control character");
    {
        char longname[400];
        memset(longname, 'a', sizeof longname - 1);
        longname[sizeof longname - 1] = 0;
        CHECK(fails_with(v, longname, 0, JPT_VP_BAD_PATH), "a name longer than 255");
    }
    {
        char small[8], host[JPT_PATH_MAX];
        CHECK(jpt_vpath_resolve(v, "data\\stream.tpa", 0, small, sizeof small) == JPT_VP_NO_SPACE, "a small buffer is reported");
        CHECK(jpt_vpath_resolve(v, "data", 0, host, sizeof host) == JPT_VP_OK, "and the same path fits a large one");
    }
    jpt_vpath_free(v);
}

static void test_cwd(void) {
    jpt_vpath_t* v = jpt_vpath_new();
    jpt_vpath_mount(v, 'C', s_root);
    CHECK(jpt_vpath_chdir(v, "data\\menu"), "chdir into a guest folder");
    CHECK(!strcmp(jpt_vpath_cwd(v), "C:\\data\\menu\\"), "the cwd is a guest path");
    CHECK(resolves_to(v, "title.tga", "/Data/Menu/Title.TGA"), "a relative path starts at the cwd");
    CHECK(resolves_to(v, "..\\stream.tpa", "/Data/Stream.tpa"), ".. from the cwd");
    CHECK(fails_with(v, "..\\..\\..\\x", 0, JPT_VP_ESCAPE), "and cannot climb out from it");
    CHECK(!jpt_vpath_chdir(v, "D:\\"), "chdir to an unmounted drive fails");
    CHECK(!strcmp(jpt_vpath_cwd(v), "C:\\data\\menu\\"), "and leaves the cwd alone");
    jpt_vpath_free(v);
}

static void test_create_and_cache(void) {
    jpt_vpath_t* v = jpt_vpath_new();
    char host[JPT_PATH_MAX];
    unsigned listings_before, listings_after, hits;
    jpt_vpath_mount(v, 'C', s_root);

    CHECK(jpt_vpath_resolve(v, "savegame\\SAVEGAME.001", 1, host, sizeof host) == JPT_VP_OK, "create: a new file in an existing folder");
    CHECK(!strcmp(tail_of(host), "/savegame/SAVEGAME.001"), "the new name is kept as the guest spelled it");
    CHECK(jpt_vpath_resolve(v, "nodir\\file", 1, host, sizeof host) == JPT_VP_NOT_FOUND, "create: the parent must exist");
    CHECK(jpt_vpath_resolve(v, "DATA\\MENU\\TITLE.tga", 1, host, sizeof host) == JPT_VP_OK && !strcmp(tail_of(host), "/Data/Menu/Title.TGA"),
          "create: an existing entry under another case is that entry, not a second file");

    make_file("savegame/SAVEGAME.001");
    jpt_vpath_invalidate(v, s_root);
    {
        char savedir[JPT_PATH_MAX];
        snprintf(savedir, sizeof savedir, "%s/savegame", s_root);
        jpt_vpath_invalidate(v, savedir);
    }
    CHECK(resolves_to(v, "SAVEGAME\\savegame.001", "/savegame/SAVEGAME.001"), "after creation the entry is found under any case");

    jpt_vpath_stats(v, &listings_before, &hits);
    for (int i = 0; i < 50; i++) resolves_to(v, "data\\menu\\TITLE.tga", "/Data/Menu/Title.TGA");
    jpt_vpath_stats(v, &listings_after, &hits);
    CHECK(listings_after == listings_before, "repeated lookups list no folder again (%u -> %u)", listings_before, listings_after);
    CHECK(hits >= 100, "and are answered from the cache (%u hits)", hits);

    make_file("Data/Menu/new.ddf");
    CHECK(resolves_to(v, "data\\menu\\NEW.DDF", "/Data/Menu/new.ddf"), "a file created behind the cache's back is found by the refresh on a miss");
    jpt_vpath_stats(v, &listings_after, &hits);
    CHECK(listings_after == listings_before + 1, "with exactly one more listing (%u -> %u)", listings_before, listings_after);
    jpt_vpath_free(v);
}

static void test_exact_match_preferred(void) {
    jpt_vpath_t* v = jpt_vpath_new();
    char path[JPT_PATH_MAX];
    jpt_vpath_mount(v, 'C', s_root);
    make_file("Data/Menu/NEW.DDF");
    make_file("Data/Menu/new.ddf");
    snprintf(path, sizeof path, "%s/data/menu/TITLE.TGA", s_root);
    if (!jpt_fs_exists(path)) {
        CHECK(resolves_to(v, "data\\menu\\new.ddf", "/Data/Menu/new.ddf"), "two entries that differ by case: the exact spelling wins");
        CHECK(resolves_to(v, "data\\menu\\NEW.DDF", "/Data/Menu/NEW.DDF"), "in both directions");
    }
    jpt_vpath_free(v);
}

int main(void) {
    build_tree();
    test_case_and_separators();
    test_containment();
    test_cwd();
    test_create_and_cache();
    test_exact_match_preferred();
    clean_tree();
    printf("vpath_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
