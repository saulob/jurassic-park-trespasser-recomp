/* The guest's allocations while a custom level plays: what fails ends the load with its cause in the log, nothing outside a
 * reservation is committed, and the campaign and the next level are as before. Synthetic data only. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"
#include "jpt_level_gate.h"
#include "jpt_user_data.h"
#include "jpt_vpath.h"
#include "linux_hle.h"
#include "linux_log.h"

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

#define RESERVE 0x2000u
#define COMMIT 0x1000u
#define PAGE_RW 4u

static uint32_t s_stack, s_exit_code;
static char s_root[256], s_log[320];

static void on_exit_request(uint32_t code) {
    if (!s_exit_code) s_exit_code = code;
}

static uint32_t api(const char* dll, const char* name, int argc, ...) {
    uint32_t thunk = hle_resolve(dll, name, 0, NULL), esp = s_stack + 0x8000 - 4 * (uint32_t)(argc + 1), eax = 0, edx = 0;
    va_list list;
    gp_write_u32(esp, 0x00401000u);
    va_start(list, argc);
    for (int i = 0; i < argc; i++) gp_write_u32(esp + 4 + 4 * (uint32_t)i, va_arg(list, uint32_t));
    va_end(list);
    s_exit_code = 0;
    return thunk && ht_invoke(thunk, &esp, &eax, &edx) ? eax : 0xBAD0BAD0u;
}

#define ALLOC(va, bytes, type) api("KERNEL32.dll", "VirtualAlloc", 4, (uint32_t)(va), (uint32_t)(bytes), (uint32_t)(type), PAGE_RW)

static int logged(const char* text) {
    static char bytes[65536];
    FILE* f = fopen(s_log, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(bytes, 1, sizeof bytes - 1, f);
    fclose(f);
    bytes[n] = 0;
    return strstr(bytes, text) != NULL;
}

static void put_word(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* A scene of version 22 with a header alone. */
static size_t make_scene(uint8_t* out) {
    const uint32_t header = 4 + 5 * 256 + 16, at = 48 + 32;
    memset(out, 0, 4096);
    put_word(out, 0x0ACEBABE);
    put_word(out + 8, 1);
    put_word(out + 12, 1);
    put_word(out + 16, 19);
    put_word(out + 20, at + header);
    put_word(out + 32, 10);
    put_word(out + 48, 0x1000);
    put_word(out + 48 + 8, at);
    put_word(out + 48 + 12, header);
    put_word(out + at, 22);
    memcpy(out + at + 4, "w.grf", 5);
    put_word(out + at + header, 0x1000);
    put_word(out + at + header + 4, 7);
    memcpy(out + at + header + 12, "Header", 7);
    return at + header + 19;
}

static int activate(void) {
    static uint8_t scene[4096];
    jpt_custom_level_request_t request;
    char dir[600], file[700], error[256] = "";
    FILE* f;
    const size_t n = make_scene(scene);
    snprintf(dir, sizeof dir, "%s/user/custom_levels/Isle", s_root);
    jpt_make_dirs(dir);
    snprintf(file, sizeof file, "%s/isle.scn", dir);
    f = fopen(file, "wb");
    if (!f || fwrite(scene, 1, n, f) != n) return 0;
    fclose(f);
    memset(&request, 0, sizeof request);
    snprintf(request.package, sizeof request.package, "Isle");
    snprintf(request.scene, sizeof request.scene, "isle.scn");
    snprintf(dir, sizeof dir, "%s/user", s_root);
    if (jpt_custom_level_activate(dir, &request, error, sizeof error)) return 1;
    printf("not active: %s\n", error);
    return 0;
}

static void test_regions(void) {
    const uint32_t first = ALLOC(0, 0x100000, RESERVE), second = ALLOC(0, 0x100000, RESERVE);
    CHECK(first && second, "two reservations");
    CHECK(ALLOC(first, 0x100000, COMMIT) == first && !s_exit_code, "one is committed to its end");
    *(uint8_t*)gm_ptr(first + 0xFFFFF, 1) = 7;
    CHECK(ALLOC(first + 0xFF000, 0x2000, COMMIT) == 0 && s_exit_code == HLE_EXIT_LEVEL, "a commit past the end ends the load (%u)", s_exit_code);
    CHECK(logged("ENGINE_LIMIT_EXCEEDED: a region of the engine") && logged("CONTROLLED_LOAD_FAILURE"), "with its cause in the log");
    CHECK(!gm_ptr_checked(second, 1, 1), "and nothing of the next reservation was taken");
    CHECK(ALLOC(0x7F000000u, 0x1000, COMMIT) == 0 && s_exit_code == HLE_EXIT_LEVEL, "a commit of no reservation ends the load");
    CHECK(ALLOC(0, 0xF0000000u, RESERVE | COMMIT) == 0 && s_exit_code == HLE_EXIT_LEVEL && logged("ALLOCATION_FAILED: a reservation of the engine"), "a reservation without room ends the load");
    CHECK(ALLOC(second, 0x1000, COMMIT) == second && !s_exit_code, "the next one is taken");
}

static void test_heap(void) {
    CHECK(api("msvcrt.dll", "??2@YAPAXI@Z", 1, 0x88u) && !s_exit_code, "operator new");
    CHECK(!api("msvcrt.dll", "??2@YAPAXI@Z", 1, 0xF0000000u) && s_exit_code == HLE_EXIT_LEVEL && logged("ALLOCATION_FAILED: operator new"), "operator new without memory ends the load");
    CHECK(!api("msvcrt.dll", "malloc", 1, 0xF0000000u) && s_exit_code == HLE_EXIT_LEVEL && logged("ALLOCATION_FAILED: malloc"), "malloc without memory ends the load");
    CHECK(api("msvcrt.dll", "malloc", 1, 0x10000u) && !s_exit_code, "the next one is given");
}

static void test_campaign(void) {
    const uint32_t region = ALLOC(0, 0x10000, RESERVE);
    CHECK(region && ALLOC(region, 0x20000, COMMIT) == 0 && !s_exit_code, "the campaign's own failures are its own");
    CHECK(!api("msvcrt.dll", "malloc", 1, 0xF0000000u) && !s_exit_code, "its malloc too");
    CHECK(ALLOC(region, 0x10000, COMMIT) == region && !s_exit_code, "and its memory is there");
}

int main(void) {
    jpt_vpath_t* vp;
    char command[700];
    snprintf(s_root, sizeof s_root, "%s/jpt_level_abort_%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    jpt_make_dirs(s_root);
    snprintf(s_log, sizeof s_log, "%s/log.txt", s_root);
    lx_log_open(s_root, s_log);
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    s_stack = gm_alloc_pages(0, 0x10000, "stack");
    vp = jpt_vpath_new();
    jpt_vpath_mount(vp, 'C', s_root);
    hle_init(vp, 0x00400000u, "C:\\trespass.exe");
    hle_set_exit_handler(on_exit_request);
    test_campaign();
    CHECK(activate(), "a custom level is active");
    test_regions();
    test_heap();
    CHECK(activate(), "the next custom level");
    CHECK(ALLOC(0, 0x10000, RESERVE | COMMIT) && !s_exit_code, "its memory is there");
    jpt_custom_level_deactivate();
    test_campaign();
    hle_shutdown();
    jpt_vpath_free(vp);
    gm_shutdown();
    snprintf(command, sizeof command, "rm -rf '%s'", s_root);
    if (system(command) != 0) printf("note: %s was not removed\n", s_root);
    printf("linux_level_abort_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
