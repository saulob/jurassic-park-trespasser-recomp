/* The Linux HLE's kernel32, called the way the guest calls it: through thunks, with arguments on a guest stack and
 * structures in guest memory at their Win32 layouts. Covers time, synchronisation, critical sections, virtual memory,
 * the system structures, files over a case-sensitive host, directory listing, module lookup, and the refusal to answer
 * an import that has no implementation. Headless; the guest window is a 4 GB reservation of this process. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_fs.h"
#include "jpt_sync.h"
#include "jpt_vpath.h"
#include "linux_abi.h"
#include "linux_hle.h"

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

#define IMAGE_BASE 0x00400000u
static uint32_t s_stack, s_scratch;
static uint32_t s_exit_code = 0xFFFFFFFFu;
static char s_root[256];

static void on_exit_request(uint32_t code) { s_exit_code = code; }

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

#define K32(name, argc, ...) api("KERNEL32.dll", #name, argc, ##__VA_ARGS__)

static uint32_t put_string(uint32_t where, const char* text) {
    gp_copy_out(where, text, (uint32_t)strlen(text) + 1);
    return where;
}

static void make_file(const char* relative, const char* content) {
    char path[512];
    FILE* f;
    snprintf(path, sizeof path, "%s/%s", s_root, relative);
    f = fopen(path, "wb");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void test_time_and_errors(void) {
    uint32_t t0 = K32(GetTickCount, 0), t1;
    K32(Sleep, 1, 40u);
    t1 = K32(GetTickCount, 0);
    CHECK(t1 - t0 >= 35 && t1 - t0 < 1000, "Sleep and GetTickCount agree (%u ms)", t1 - t0);
    K32(CloseHandle, 1, 0x5555u);
    CHECK(K32(GetLastError, 0) == 6, "an invalid handle sets ERROR_INVALID_HANDLE");
    CHECK(K32(lstrlenA, 1, put_string(s_scratch, "hello")) == 5, "lstrlenA");
    CHECK(K32(lstrcpyA, 2, s_scratch + 64, s_scratch) == s_scratch + 64 && !strcmp((const char*)gm_ptr(s_scratch + 64, 6), "hello"),
          "lstrcpyA copies and returns the destination");
}

static void test_synchronisation(void) {
    uint32_t ev = K32(CreateEventA, 4, 0u, 1u, 0u, 0u);
    uint32_t sem = K32(CreateSemaphoreA, 4, 0u, 1u, 2u, 0u);
    uint32_t mtx = K32(CreateMutexA, 3, 0u, 0u, 0u);
    uint32_t list = s_scratch + 128;
    CHECK(ev && sem && mtx && ev != sem, "handles are created");
    CHECK(K32(WaitForSingleObject, 2, ev, 0u) == 0x102, "a clear manual-reset event times out");
    CHECK(K32(SetEvent, 1, ev) == 1 && K32(WaitForSingleObject, 2, ev, 0u) == 0 && K32(WaitForSingleObject, 2, ev, 0u) == 0,
          "SetEvent releases waits and stays set");
    CHECK(K32(WaitForSingleObject, 2, sem, 0u) == 0 && K32(WaitForSingleObject, 2, sem, 0u) == 0x102, "the semaphore's one unit");
    CHECK(K32(ReleaseSemaphore, 3, sem, 1u, s_scratch + 192) == 1 && *(uint32_t*)gm_ptr(s_scratch + 192, 4) == 0,
          "ReleaseSemaphore reports the previous count in guest memory");
    CHECK(K32(ReleaseSemaphore, 3, sem, 5u, 0u) == 0 && K32(GetLastError, 0) == 87, "past the maximum fails with ERROR_INVALID_PARAMETER");
    gp_write_u32(list, sem);
    gp_write_u32(list + 4, mtx);
    CHECK(K32(WaitForMultipleObjects, 4, 2u, list, 0u, 0u) == 0, "wait-any over a guest array of handles returns the first signalled");
    CHECK(K32(WaitForSingleObjectEx, 3, mtx, 0u, 0u) == 0, "the mutex is free");
    CHECK(K32(CloseHandle, 1, ev) == 1 && K32(WaitForSingleObject, 2, ev, 0u) == 0xFFFFFFFFu, "a closed handle is invalid");
    K32(CloseHandle, 1, sem);
    K32(CloseHandle, 1, mtx);

    {
        uint32_t cs = s_scratch + 256;
        K32(InitializeCriticalSection, 1, cs);
        K32(EnterCriticalSection, 1, cs);
        K32(EnterCriticalSection, 1, cs);
        K32(LeaveCriticalSection, 1, cs);
        K32(LeaveCriticalSection, 1, cs);
        K32(DeleteCriticalSection, 1, cs);
        CHECK(1, "a guest CRITICAL_SECTION is entered recursively through its address");
    }
}

static void test_memory(void) {
    uint32_t block = K32(VirtualAlloc, 4, 0u, 0x20000u, 0x3000u, 4u);
    uint32_t fixed = 0x30000000u;
    uint32_t info = s_scratch + 512;
    CHECK(block && (block & 0xFFFF) == 0, "reserve and commit returns a 64 KB aligned block");
    *(uint32_t*)gm_ptr(block, 4) = 0xCAFEBABEu;
    *(uint32_t*)gm_ptr(block + 0x1FFFC, 4) = 7;
    CHECK(*(uint32_t*)gm_ptr_checked(block, 4, 1) == 0xCAFEBABEu, "the pages are committed and writable");
    CHECK(K32(VirtualFree, 3, block + 0x1000, 0x1000u, 0x4000u) == 1 && !gm_ptr_checked(block + 0x1000, 4, 0),
          "MEM_DECOMMIT removes pages inside the reservation");
    CHECK(K32(VirtualAlloc, 4, block + 0x1000, 0x1000u, 0x1000u, 4u) == block + 0x1000 && gm_ptr_checked(block + 0x1000, 4, 1),
          "and MEM_COMMIT brings them back, inside the existing reservation");
    CHECK(K32(VirtualFree, 3, block, 0u, 0x8000u) == 1 && !gm_ptr_checked(block, 4, 0), "MEM_RELEASE frees the reservation");
    CHECK(K32(VirtualFree, 3, block, 0u, 0x8000u) == 0, "once");

    CHECK(K32(VirtualAlloc, 4, fixed, 0x10000u, 0x2000u, 4u) == fixed, "reserve at a requested address");
    CHECK(K32(VirtualAlloc, 4, fixed, 0x10000u, 0x2000u, 4u) == 0, "the same range cannot be reserved twice");
    CHECK(K32(VirtualAlloc, 4, fixed, 0x3000u, 0x1000u, 4u) == fixed && gm_ptr_checked(fixed, 0x3000, 1), "commit only part of it, rounded to pages");
    CHECK(!gm_ptr_checked(fixed + 0x3000, 4, 0), "and the rest stays reserved");
    K32(VirtualFree, 3, fixed, 0u, 0x8000u);

    K32(GetSystemInfo, 1, info);
    {
        guest_system_info_t si;
        gp_copy_in(&si, info, sizeof si);
        CHECK(si.dwPageSize == 4096 && si.dwAllocationGranularity == 0x10000 && si.dwNumberOfProcessors >= 1 &&
                  si.lpMinimumApplicationAddress == 0x10000 && si.lpMaximumApplicationAddress == 0x7FFEFFFFu && si.dwProcessorType == 586,
              "SYSTEM_INFO as a 32-bit Windows process sees it");
    }
    gp_write_u32(info, 32);
    K32(GlobalMemoryStatus, 1, info);
    {
        guest_memory_status_t ms;
        gp_copy_in(&ms, info, sizeof ms);
        CHECK(ms.dwLength == 32 && ms.dwTotalPhys > 0 && ms.dwTotalPhys <= 0x7FFF0000u && ms.dwAvailPhys <= ms.dwTotalPhys &&
                  ms.dwMemoryLoad <= 100 && ms.dwTotalVirtual == 0x7FFE0000u,
              "MEMORYSTATUS stays inside a 32-bit address space (total %u, load %u%%)", ms.dwTotalPhys, ms.dwMemoryLoad);
    }
}

static void test_files(void) {
    uint32_t path = s_scratch + 1024, buf = s_scratch + 1536, count = s_scratch + 1500, find = s_scratch + 2048;
    uint32_t h, size_high = s_scratch + 1504;
    CHECK((h = K32(CreateFileA, 7, put_string(path, "DATA\\menu\\title.TGA"), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u)) != 0xFFFFFFFFu,
          "CreateFileA finds a file under another case");
    CHECK(K32(GetFileSize, 2, h, 0u) == 11, "GetFileSize");
    CHECK(K32(ReadFile, 5, h, buf, 5u, count, 0u) == 1 && *(uint32_t*)gm_ptr(count, 4) == 5 && !memcmp(gm_ptr(buf, 5), "hello", 5),
          "ReadFile");
    CHECK(K32(SetFilePointer, 4, h, 6u, 0u, 0u) == 6 && K32(ReadFile, 5, h, buf, 100u, count, 0u) == 1 && *(uint32_t*)gm_ptr(count, 4) == 5 &&
              !memcmp(gm_ptr(buf, 5), "world", 5),
          "SetFilePointer from the start, and a short read at the end");
    CHECK(K32(SetFilePointer, 4, h, 0xFFFFFFFCu, 0u, 2u) == 7, "a negative distance from the end");
    gp_write_u32(size_high, 0);
    CHECK(K32(SetFilePointer, 4, h, 0u, size_high, 1u) == 7 && *(uint32_t*)gm_ptr(size_high, 4) == 0, "the high dword is written back");
    K32(CloseHandle, 1, h);

    CHECK(K32(CreateFileA, 7, put_string(path, "data\\menu\\absent.tga"), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u) == 0xFFFFFFFFu &&
              K32(GetLastError, 0) == 2,
          "a missing file: INVALID_HANDLE_VALUE and ERROR_FILE_NOT_FOUND");
    CHECK(K32(CreateFileA, 7, put_string(path, "nodir\\x.tga"), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u) == 0xFFFFFFFFu &&
              K32(GetLastError, 0) == 2,
          "a missing folder");
    CHECK(K32(CreateFileA, 7, put_string(path, "..\\..\\etc\\passwd"), 0x80000000u, 1u, 0u, 3u, 0x80u, 0u) == 0xFFFFFFFFu,
          "a path that climbs out of the game folder is refused");

    h = K32(CreateFileA, 7, put_string(path, "SaveGame\\SAVEGAME.001"), 0x40000000u, 0u, 0u, 2u, 0x80u, 0u);
    CHECK(h != 0xFFFFFFFFu, "CREATE_ALWAYS makes a new file in an existing folder");
    CHECK(K32(WriteFile, 5, h, put_string(buf, "SAVE"), 4u, count, 0u) == 1 && *(uint32_t*)gm_ptr(count, 4) == 4, "WriteFile");
    K32(CloseHandle, 1, h);
    CHECK(K32(GetFileAttributesA, 1, put_string(path, "savegame\\savegame.001")) == 0x80, "the new file is found under any case, as a normal file");
    CHECK(K32(GetFileAttributesA, 1, put_string(path, "data")) == 0x10, "a folder has the directory attribute");
    CHECK(K32(GetFileAttributesA, 1, put_string(path, "data\\nope")) == 0xFFFFFFFFu, "a missing file: INVALID_FILE_ATTRIBUTES");
    CHECK(K32(CreateFileA, 7, put_string(path, "savegame\\SAVEGAME.001"), 0x40000000u, 0u, 0u, 1u, 0x80u, 0u) == 0xFFFFFFFFu &&
              K32(GetLastError, 0) == 183,
          "CREATE_NEW over an existing file: ERROR_ALREADY_EXISTS");

    h = K32(FindFirstFileA, 2, put_string(path, "DATA\\*.TPA"), find);
    CHECK(h != 0xFFFFFFFFu, "FindFirstFileA over a wildcard");
    {
        guest_find_data_t fd;
        gp_copy_in(&fd, find, sizeof fd);
        CHECK(!strcmp(fd.cFileName, "Stream.tpa") && fd.dwFileAttributes == 0x80 && fd.nFileSizeLow == 6 && fd.nFileSizeHigh == 0 &&
                  (fd.ftLastWriteTime.dwHighDateTime != 0),
              "WIN32_FIND_DATAA: the host's spelling of the name, attributes, size and a FILETIME (%s)", fd.cFileName);
    }
    CHECK(K32(FindNextFileA, 2, h, find) == 0 && K32(GetLastError, 0) == 18, "FindNextFileA ends with ERROR_NO_MORE_FILES");
    CHECK(K32(FindClose, 1, h) == 1, "FindClose");
    CHECK(K32(FindFirstFileA, 2, put_string(path, "data\\*.zzz"), find) == 0xFFFFFFFFu && K32(GetLastError, 0) == 2, "no match: ERROR_FILE_NOT_FOUND");
    h = K32(FindFirstFileA, 2, put_string(path, "data\\menu\\T?TLE.*"), find);
    CHECK(h != 0xFFFFFFFFu, "? and * match case-insensitively");
    K32(FindClose, 1, h);

    CHECK(K32(DeleteFileA, 1, put_string(path, "SAVEGAME\\savegame.001")) == 1 && K32(GetFileAttributesA, 1, path) == 0xFFFFFFFFu, "DeleteFileA through a folded path");
    CHECK(K32(SetCurrentDirectoryA, 1, put_string(path, "DATA\\MENU")) == 1, "SetCurrentDirectoryA");
    CHECK(K32(GetFileAttributesA, 1, put_string(path, "TITLE.tga")) == 0x80, "relative names start at the new current folder");
}

static void test_modules_and_missing(void) {
    uint32_t name = s_scratch + 3072, out = s_scratch + 3200;
    uint32_t ddraw, proc;
    unsigned before;
    CHECK(K32(GetModuleHandleA, 1, 0u) == IMAGE_BASE, "GetModuleHandleA(NULL) is the guest image");
    CHECK((ddraw = K32(LoadLibraryA, 1, put_string(name, "DDRAW.DLL"))) != 0, "LoadLibraryA of a system module the host answers for");
    CHECK(K32(LoadLibraryA, 1, put_string(name, "A3d.dll")) == 0 && K32(GetLastError, 0) == 126, "A3d.dll is not found, as before");
    CHECK(K32(GetModuleHandleA, 1, put_string(name, "Kernel32")) != 0, "a module name without .dll");
    before = hle_missing_count();
    proc = K32(GetProcAddress, 2, ddraw, put_string(name, "DirectDrawCreateEx"));
    CHECK(proc != 0 && ht_is_thunk(proc), "GetProcAddress answers with a thunk, never a host address");
    CHECK(hle_missing_count() == before + 1, "an export with no implementation is counted as missing");
    s_exit_code = 0xFFFFFFFFu;
    call_n(proc, 0, NULL);
    CHECK(s_exit_code == HLE_EXIT_MISSING, "calling it ends the run with the missing-HLE exit code, it does not return a value (%u)", s_exit_code);
    CHECK(K32(GetProcAddress, 2, ddraw, 0u) == 0, "a NULL name is refused");
    CHECK(K32(GetModuleFileNameA, 3, 0u, out, 260u) == 15 && !strcmp((const char*)gm_ptr(out, 16), "C:\\trespass.exe"), "GetModuleFileNameA names the guest executable");
    CHECK(K32(GetModuleFileNameA, 3, 0u, out, 6u) == 5 && !strcmp((const char*)gm_ptr(out, 6), "C:\\tr"), "and truncates to the buffer");
    s_exit_code = 0xFFFFFFFFu;
    K32(ExitProcess, 1, 3u);
    CHECK(s_exit_code == 3, "ExitProcess reaches the exit handler with its code");
    CHECK(hle_find("kernel32.dll", "CreateFileA") != NULL && hle_find("USER32.DLL", "DialogBoxParamA") == NULL, "the table lists only what is implemented");
}

int main(void) {
    char tree[400];
    jpt_vpath_t* vp;
    snprintf(s_root, sizeof s_root, "%s/jpt_linux_hle_%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    jpt_fs_mkdir(s_root);
    snprintf(tree, sizeof tree, "%s/Data", s_root);
    jpt_fs_mkdir(tree);
    snprintf(tree, sizeof tree, "%s/Data/Menu", s_root);
    jpt_fs_mkdir(tree);
    snprintf(tree, sizeof tree, "%s/savegame", s_root);
    jpt_fs_mkdir(tree);
    make_file("Data/Menu/Title.TGA", "hello world");
    make_file("Data/Stream.tpa", "stream");

    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    s_stack = gm_alloc_pages(0, 0x10000, "stack");
    s_scratch = gm_alloc_pages(0, 0x10000, "scratch");
    vp = jpt_vpath_new();
    jpt_vpath_mount(vp, 'C', s_root);
    hle_init(vp, IMAGE_BASE, "C:\\trespass.exe");
    hle_set_exit_handler(on_exit_request);

    test_time_and_errors();
    test_synchronisation();
    test_memory();
    test_files();
    test_modules_and_missing();

    hle_shutdown();
    jpt_vpath_free(vp);
    gm_shutdown();
    {
        const char* files[] = { "Data/Menu/Title.TGA", "Data/Stream.tpa", "savegame/SAVEGAME.001" };
        const char* dirs[] = { "Data/Menu", "Data", "savegame", "" };
        char path[400];
        for (size_t i = 0; i < 3; i++) {
            snprintf(path, sizeof path, "%s/%s", s_root, files[i]);
            jpt_fs_remove(path);
        }
        for (size_t i = 0; i < 4; i++) {
            snprintf(path, sizeof path, "%s%s%s", s_root, *dirs[i] ? "/" : "", dirs[i]);
            jpt_fs_remove_dir(path);
        }
    }
    printf("linux_hle_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
