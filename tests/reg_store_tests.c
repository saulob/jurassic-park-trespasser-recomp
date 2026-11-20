/* The virtual registry's file: values of each type survive a save and a load, a file written by the Windows host (CRLF
 * line ends, 4-byte DWORDs) reads on any platform, and what is written does not depend on the width of long or of a
 * pointer. Runs on Windows and Linux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"
#include "jpt_reg_store.h"

#ifdef _WIN32
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

static void temp_path(char* out, size_t cap, const char* leaf) {
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(out, cap, "%s/jpt_reg_%d_%s", tmp, (int)getpid(), leaf);
}

static void write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char* read_text(const char* path) {
    FILE* f = fopen(path, "rb");
    char* text;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)size + 1, 1);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) text[0] = 0;
    fclose(f);
    if (text) {
        char* out = text;
        for (const char* in = text; *in; in++)
            if (*in != 13) *out++ = *in;
        *out = 0;
    }
    return text;
}

static void test_values(void) {
    jpt_reg_store_t* r = jpt_reg_new();
    uint32_t type = 0, size = 0, dword = 0;
    const unsigned char blob[4] = { 0xE0, 0x3D, 0xE6, 0x84 };
    CHECK(jpt_reg_set_dword(r, "PlayMusic", 1), "a DWORD");
    CHECK(jpt_reg_get_dword(r, "playmusic", &dword) && dword == 1, "names are case-insensitive");
    CHECK(jpt_reg_set_dword(r, "Width", 0xFFFFFFFFu) && jpt_reg_get_dword(r, "Width", &dword) && dword == 0xFFFFFFFFu,
          "all 32 bits of a DWORD");
    CHECK(jpt_reg_set_string(r, "Data Drive", "C:\\Game\\") && jpt_reg_get(r, "Data Drive", &type, &size) &&
              type == JPT_REG_SZ && size == 9 && !strcmp((const char*)jpt_reg_get(r, "data drive", NULL, NULL), "C:\\Game\\"),
          "a string keeps its terminator");
    CHECK(jpt_reg_set(r, "D3D GUID", JPT_REG_BINARY, blob, sizeof blob), "a binary value");
    CHECK(!jpt_reg_get_dword(r, "Data Drive", &dword), "a string is not a DWORD");
    CHECK(jpt_reg_get(r, "missing", NULL, NULL) == NULL, "an unset value");
    CHECK(jpt_reg_set_dword(r, "Width", 7) && jpt_reg_get_dword(r, "Width", &dword) && dword == 7, "overwrite");
    CHECK(jpt_reg_delete(r, "Width") && jpt_reg_get(r, "Width", NULL, NULL) == NULL && !jpt_reg_delete(r, "Width"), "delete");
    CHECK(jpt_reg_count(r) == 3, "three values remain (%d)", jpt_reg_count(r));
    {
        char longname[80];
        memset(longname, 'n', sizeof longname - 1);
        longname[sizeof longname - 1] = 0;
        CHECK(!jpt_reg_set_dword(r, longname, 1), "a name of 64 characters or more is refused");
    }
    jpt_reg_free(r);
}

static void test_round_trip(void) {
    char path[512];
    char* text;
    jpt_reg_store_t* a = jpt_reg_new();
    jpt_reg_store_t* b = jpt_reg_new();
    const unsigned char blob[16] = { 0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0, 0, 0xC0, 0x20, 0x15, 0x6E };
    uint32_t dword = 0, type = 0, size = 0;
    int ignored = -1;
    temp_path(path, sizeof path, "rt.txt");
    jpt_reg_set_dword(a, "Installed", 1);
    jpt_reg_set_dword(a, "UseSubTitles", 0x80000001u);
    jpt_reg_set_string(a, "Installed Directory", "user_data/saves/");
    jpt_reg_set(a, "D3D GUID", JPT_REG_BINARY, blob, sizeof blob);
    jpt_reg_set(a, "Odd", 7, "ab", 2);
    jpt_reg_set_string(a, "Two Lines", "a\nb");
    CHECK(jpt_reg_save(a, path), "save");
    text = read_text(path);
    CHECK(text && strstr(text, "Installed=dword:00000001\n") && strstr(text, "UseSubTitles=dword:80000001\n"),
          "DWORDs are eight upper-case hex digits");
    CHECK(text && strstr(text, "D3D GUID=hex(3):e03de684aa46cf11816f0000c020156e\n"), "binary values are lower-case hex");
    CHECK(text && strstr(text, "Odd=hex(7):6162\n") && strstr(text, "Two Lines=hex(1):610a6200\n"),
          "other types and strings with line breaks are hex");
    free(text);
    CHECK(jpt_reg_load(b, path, &ignored) == 6 && ignored == 0, "load reads the six values");
    CHECK(jpt_reg_get_dword(b, "UseSubTitles", &dword) && dword == 0x80000001u, "DWORD round trip");
    CHECK(jpt_reg_get(b, "D3D GUID", &type, &size) && type == JPT_REG_BINARY && size == 16 &&
              !memcmp(jpt_reg_get(b, "D3D GUID", NULL, NULL), blob, 16),
          "binary round trip");
    CHECK(jpt_reg_get(b, "Two Lines", &type, &size) && type == JPT_REG_SZ && size == 4, "a string with a line break round trips");
    jpt_fs_remove(path);
    jpt_reg_free(a);
    jpt_reg_free(b);
}

static void test_windows_written_file(void) {
    char path[512];
    jpt_reg_store_t* r = jpt_reg_new();
    uint32_t dword = 0;
    int ignored = -1;
    temp_path(path, sizeof path, "win.txt");
    write_text(path,
               "Installed=dword:00000001\r\n"
               "Installed Directory=sz:D:\\Data\\player\\saves\\\r\n"
               "Data Drive=sz:C:\\Games\\Trespasser\\\r\n"
               "UseSubTitles=dword:00000001\r\n"
               "JPT UseSubTitles Default=dword:00000001\r\n"
               "Width=dword:00000780\r\n"
               "D3D GUID=hex(3):e03de684aa46cf11816f0000c020156e\r\n"
               "\r\n"
               "not a value line\r\n"
               "Broken=hex(3):zz\r\n");
    CHECK(jpt_reg_load(r, path, &ignored) == 7, "the seven values of a Windows-written file");
    CHECK(ignored == 2, "two malformed lines are skipped and counted (%d)", ignored);
    CHECK(jpt_reg_get_dword(r, "Width", &dword) && dword == 1920, "a DWORD");
    CHECK(!strcmp((const char*)jpt_reg_get(r, "Data Drive", NULL, NULL), "C:\\Games\\Trespasser\\"), "a string with backslashes");
    jpt_fs_remove(path);
    CHECK(jpt_reg_load(r, path, &ignored) == 0 && ignored == 0, "a missing file is an empty load");
    jpt_reg_free(r);
}

static void test_layout_independence(void) {
    CHECK(sizeof(uint32_t) == 4, "uint32_t");
    {
        char path[512];
        jpt_reg_store_t* r = jpt_reg_new();
        char* text;
        temp_path(path, sizeof path, "width.txt");
        jpt_reg_set_dword(r, "Big", 0xDEADBEEFu);
        jpt_reg_save(r, path);
        text = read_text(path);
        CHECK(text && !strcmp(text, "Big=dword:DEADBEEF\n"), "the same bytes whatever the width of long (%s)", text ? text : "(none)");
        free(text);
        jpt_fs_remove(path);
        jpt_reg_free(r);
    }
}

int main(void) {
    test_values();
    test_round_trip();
    test_windows_written_file();
    test_layout_independence();
    printf("reg_store_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
