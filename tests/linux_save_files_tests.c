/* The file operations the game's save and load use (msvcrt _open/_read/_write/_lseek/_close/remove/rename/_mkdir over D:, the
 * saves folder), called as the guest calls them and checked on the host's files: creating, overwriting, reading back, a
 * truncated file, names in any case, folders made on the way, and a saves folder that cannot be written. Synthetic data only. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "active_profile.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_fs.h"
#include "jpt_game_version.h"
#include "jpt_user_data.h"
#include "jpt_vpath.h"
#include "linux_hle.h"
#include "linux_spine.h"
#include "save_preview.h"

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

#define O_BINARY_G 0x8000u
#define O_RDONLY_G 0u
#define O_RDWR_G 2u
#define O_CREAT_G 0x100u
#define O_TRUNC_G 0x200u
#define O_CREATE_SAVE (O_BINARY_G | O_CREAT_G | O_TRUNC_G | O_RDWR_G)

static uint32_t s_stack, s_scratch;
static char s_root[256], s_saves[300];

static uint32_t call_n(uint32_t thunk, int argc, const uint32_t* args) {
    uint32_t esp = s_stack + 0x8000 - 4 * (uint32_t)(argc + 1), eax = 0, edx = 0;
    gp_write_u32(esp, 0x00401000u);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    if (!ht_invoke(thunk, &esp, &eax, &edx)) return 0xBAD0BAD0u;
    return eax;
}

static uint32_t api(const char* dll, const char* name, int argc, ...) {
    uint32_t args[16], thunk;
    va_list list;
    va_start(list, argc);
    for (int i = 0; i < argc; i++) args[i] = va_arg(list, uint32_t);
    va_end(list);
    thunk = hle_resolve(dll, name, 0, NULL);
    return thunk ? call_n(thunk, argc, args) : 0xBAD1BAD1u;
}

#define CRT(name, argc, ...) api("msvcrt.dll", #name, argc, ##__VA_ARGS__)
#define K32(name, argc, ...) api("KERNEL32.dll", #name, argc, ##__VA_ARGS__)

static uint32_t put_string(uint32_t where, const char* text) {
    gp_copy_out(where, text, (uint32_t)strlen(text) + 1);
    return where;
}

static uint32_t put_bytes(uint32_t where, const void* data, uint32_t n) {
    gp_copy_out(where, data, n);
    return where;
}

static int open_save(const char* name, uint32_t mode) { return (int)CRT(_open, 3, put_string(s_scratch, name), mode, 0x180u); }

static int write_save(const char* name, const void* data, uint32_t n) {
    int fd = open_save(name, O_CREATE_SAVE);
    int written;
    if (fd < 0) return -1;
    written = (int)CRT(_write, 3, (uint32_t)fd, put_bytes(s_scratch + 0x1000, data, n), n);
    CRT(_close, 1, (uint32_t)fd);
    return written;
}

static int read_save(const char* name, void* data, uint32_t cap) {
    int fd = open_save(name, O_BINARY_G | O_RDONLY_G), n;
    if (fd < 0) return -1;
    n = (int)CRT(_read, 3, (uint32_t)fd, s_scratch + 0x4000, cap);
    if (n > 0) gp_copy_in(data, s_scratch + 0x4000, (uint32_t)n);
    CRT(_close, 1, (uint32_t)fd);
    return n;
}

static long host_size(const char* relative) {
    char path[600];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", s_saves, relative);
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}


/* The HD picture of a save: made from the game's frozen gameplay picture when the save is created, read back as the same picture,
 * deleted with the save, and never made for a file that is not a save. */
static void test_save_pictures(void) {
    char error[256];
    const jpt_game_version_t* v = jpt_game_version_by_id("en-116");
    uint32_t frozen = 0x00660100u, bits;
    sp_image_t image;
    uint8_t payload[100];
    memset(payload, 7, sizeof payload);
    lx_spine_init((lx_table_t){ NULL, 0 }, (lx_table_t){ NULL, 0 });
    if (!v || !jpt_profile_activate(v, error, sizeof error)) {
        CHECK(0, "the en-116 profile is active for the save picture test (%s)", error);
        return;
    }
    gm_alloc_pages(0x00660000u, 0x10000, "game data");
    bits = gm_alloc_pages(0, 640u * 480u * 2u, "frozen picture");
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) *(uint16_t*)gm_ptr(bits + (uint32_t)(y * 640 + x) * 2u, 2) = (uint16_t)(x < 320 ? 0xF800 : 0x001F);
    gp_write_u32(0x0066E338u, frozen);
    gp_write_u32(frozen + 0x08, 640);
    gp_write_u32(frozen + 0x0C, 480);
    gp_write_u32(frozen + 0x10, 16);
    gp_write_u32(frozen + 0x14, 640);
    gp_write_u32(frozen + 0x18, bits);
    CHECK(write_save("D:\\SAVEGAME.007", payload, sizeof payload) == (int)sizeof payload, "a save is created");
    CHECK(host_size("previews/SAVEGAME.007.png") > 200, "its HD picture is a PNG beside it in previews (%ld bytes)", host_size("previews/SAVEGAME.007.png"));
    memset(&image, 0, sizeof image);
    CHECK(save_preview_load("D:\\SAVEGAME.007", &image) && image.width == 640 && image.height == 480, "it reads back at the picture's size (%dx%d)", image.width, image.height);
    if (image.pixels) {
        CHECK(image.pixels[240 * 640 + 100] == 0xFFFF0000u || (image.pixels[240 * 640 + 100] & 0xFFFFF8F8u) == 0xFFF80000u, "the left half is the game's red (%08X)", image.pixels[240 * 640 + 100]);
        CHECK((image.pixels[240 * 640 + 500] & 0xFF0000FFu) == 0xFF0000F8u || (image.pixels[240 * 640 + 500] & 0xFF0000FFu) == 0xFF0000FFu, "and the right half the game's blue (%08X)", image.pixels[240 * 640 + 500]);
    }
    save_preview_free(&image);
    CHECK(write_save("D:\\notes.txt", payload, sizeof payload) == (int)sizeof payload && host_size("previews/notes.txt.png") < 0, "a file that is not a save gets no picture");
    CHECK(K32(DeleteFileA, 1, put_string(s_scratch, "D:\\SAVEGAME.007")) == 1 && host_size("SAVEGAME.007") < 0 && host_size("previews/SAVEGAME.007.png") < 0, "deleting the save deletes its picture");
    jpt_profile_reset();
}

int main(void) {
    uint8_t payload[3000], back[4000];
    jpt_vpath_t* vp;
    char path[600];
    int fd;
    snprintf(s_root, sizeof s_root, "%s/jpt_linux_save_%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    snprintf(s_saves, sizeof s_saves, "%s/saves", s_root);
    jpt_make_dirs(s_saves);
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 31u + 7u);

    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    s_stack = gm_alloc_pages(0, 0x10000, "stack");
    s_scratch = gm_alloc_pages(0, 0x10000, "scratch");
    vp = jpt_vpath_new();
    jpt_vpath_mount(vp, 'C', s_root);
    jpt_vpath_mount(vp, 'D', s_saves);
    hle_init(vp, 0x00400000u, "C:\\trespass.exe");

    CHECK(write_save("D:\\SAVEGAME.000", payload, sizeof payload) == (int)sizeof payload, "a save is created with _open(O_CREAT|O_TRUNC) and _write");
    CHECK(host_size("SAVEGAME.000") == (long)sizeof payload, "and is a file of that size on the host (%ld)", host_size("SAVEGAME.000"));
    memset(back, 0, sizeof back);
    CHECK(read_save("D:\\SAVEGAME.000", back, sizeof back) == (int)sizeof payload && !memcmp(back, payload, sizeof payload), "_read gives the bytes back exactly");
    CHECK(read_save("d:\\savegame.000", back, sizeof back) == (int)sizeof payload, "the name is found in any case");
    CHECK(read_save("D:\\SaveGame.000", back, sizeof back) == (int)sizeof payload, "mixed case too");

    CHECK(write_save("D:\\SAVEGAME.000", payload, 1200) == 1200 && host_size("SAVEGAME.000") == 1200, "an overwrite truncates: the new, shorter save replaces the old (%ld)", host_size("SAVEGAME.000"));
    CHECK(read_save("D:\\SAVEGAME.000", back, sizeof back) == 1200 && !memcmp(back, payload, 1200), "and reads back as written");
    CHECK(read_save("D:\\SAVEGAME.000", back, 2000) == 1200, "a read of more than the file holds returns what is there (a truncated save)");

    fd = open_save("D:\\SAVEGAME.000", O_BINARY_G | O_RDONLY_G);
    CHECK(fd >= 3, "a descriptor");
    CHECK((int)CRT(_lseek, 3, (uint32_t)fd, 1000u, 0u) == 1000 && (int)CRT(_read, 3, (uint32_t)fd, s_scratch + 0x4000, 500u) == 200, "a read near the end returns the rest");
    CHECK((int)CRT(_lseek, 3, (uint32_t)fd, 0u, 2u) == 1200, "_lseek to the end gives the size");
    CRT(_close, 1, (uint32_t)fd);

    CHECK(open_save("D:\\SAVEGAME.999", O_BINARY_G | O_RDONLY_G) < 0, "a save that does not exist: -1");
    CHECK(CRT(_open, 3, put_string(s_scratch, "D:\\..\\..\\etc\\passwd"), O_BINARY_G, 0u) == 0xFFFFFFFFu, "a name that climbs out of the saves folder is refused");

    CHECK(CRT(rename, 2, put_string(s_scratch, "D:\\SAVEGAME.000"), put_string(s_scratch + 0x200, "D:\\SAVEGAME.001")) == 0 && host_size("SAVEGAME.001") == 1200 && host_size("SAVEGAME.000") < 0,
          "rename moves a save");
    CHECK(CRT(remove, 1, put_string(s_scratch, "d:\\savegame.001")) == 0 && host_size("SAVEGAME.001") < 0, "remove deletes it, whatever the case of the name");
    CHECK(CRT(remove, 1, put_string(s_scratch, "D:\\SAVEGAME.001")) == 0xFFFFFFFFu, "removing it again fails");

    CHECK(CRT(_mkdir, 1, put_string(s_scratch, "D:\\previews")) == 0 && host_size("previews") >= 0, "a folder is made on the way");
    CHECK(write_save("D:\\previews\\SAVEGAME.002.jpg", payload, 64) == 64 && host_size("previews/SAVEGAME.002.jpg") == 64, "a file in it");
    CHECK(CRT(_mkdir, 1, put_string(s_scratch, "D:\\previews")) == 0xFFFFFFFFu, "making it again fails");

    {
        uint32_t h = K32(CreateFileA, 7, put_string(s_scratch, "D:\\SAVEGAME.003"), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
        uint32_t written = s_scratch + 0x3F00;
        CHECK(h != 0xFFFFFFFFu && K32(WriteFile, 5, h, put_bytes(s_scratch + 0x1000, payload, 100), 100u, written, 0u) == 1 && host_size("SAVEGAME.003") == 100,
              "the kernel32 path writes a save as well");
        K32(CloseHandle, 1, h);
        CHECK(read_save("D:\\SAVEGAME.003", back, sizeof back) == 100 && !memcmp(back, payload, 100), "and _read sees it: both layers share one folder");
    }

    test_save_pictures();

    chmod(s_saves, 0555);
    CHECK(open_save("D:\\SAVEGAME.004", O_CREATE_SAVE) < 0 || geteuid() == 0, "a saves folder that cannot be written refuses the save instead of pretending");
    chmod(s_saves, 0755);

    hle_shutdown();
    jpt_vpath_free(vp);
    gm_shutdown();
    snprintf(path, sizeof path, "rm -rf '%s'", s_root);
    if (system(path) != 0) printf("note: %s was not removed\n", s_root);
    printf("linux_save_files_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
