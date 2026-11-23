/* The guest DLL the lift carries (MSVCIRT): the installed DLL mapped at its base and verified by SHA-256, its exports by name and
 * ordinal, LoadLibrary, FreeLibrary, GetModuleHandle and GetProcAddress over it, DllMain called once on attach, and the C++
 * object layout its lifted constructor and destructor build in guest memory (vtable and virtual base table inside the DLL's own
 * data, read by the guest directly). Needs the game's MSVCIRT.DLL; without it the suite says DATA UNAVAILABLE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"
#include "jpt_fs.h"
#include "jpt_vpath.h"
#include "linux_hle.h"
#include "linux_lift.h"
#include "linux_modules.h"
#include "linux_spine.h"

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

static void exit_handler(uint32_t code) {
    if (code == HLE_EXIT_MISSING) {
        const char* name = ht_name(lx_current_import_va());
        lx_abort(LX_MISSING_IMPORT, lx_current_import_va(), name ? name : "?");
    }
    lx_abort(LX_EXIT, code, "ExitProcess");
}

typedef struct {
    uint32_t va;
    int convention, argc;
    uint32_t args[4], ecx, result;
} call_t;

static void do_call(void* arg) {
    call_t* c = (call_t*)arg;
    c->result = lx_call_guest(c->va, c->convention, c->argc, c->args, c->ecx, NULL);
}

static lx_status_t call(call_t* c) { return lx_run(do_call, c); }

static uint32_t api(const char* name, int argc, uint32_t a0, uint32_t a1) {
    call_t c;
    memset(&c, 0, sizeof c);
    c.va = hle_resolve("kernel32.dll", name, 0, NULL);
    c.argc = argc;
    c.args[0] = a0;
    c.args[1] = a1;
    return call(&c) == LX_OK ? c.result : 0xBADBAD00u;
}

static uint32_t put_string(uint32_t at, const char* text) {
    gp_copy_out(at, text, (uint32_t)strlen(text) + 1);
    return at;
}

static uint32_t read32(uint32_t va) {
    uint32_t v = 0;
    gp_read_u32(va, &v);
    return v;
}

static void test_lookup_and_exports(lx_module_t* m) {
    uint32_t ctor = lx_module_export(m, "??0ofstream@@QAE@XZ", 0);
    unsigned code = 0, data = 0, none = 0;
    uint32_t cout_va = lx_module_export(m, "?cout@@3Vostream_withassign@@A", 0);
    CHECK(lx_module_find("msvcirt") == m && lx_module_find("MSVCIRT.DLL") == m && lx_module_find("C:\\Game\\MsvcIrt.dll") == m,
          "found by name, with or without .dll, any case, with a path");
    CHECK(lx_module_find("msvcirt2") == NULL && lx_module_find("kernel32") == NULL && lx_module_find(NULL) == NULL, "unknown names are not modules");
    CHECK(lx_module_by_base(0x780A0000u) == m && lx_module_by_base(0x780A1000u) == NULL, "identified by its guest base");
    CHECK(m->image.base == 0x780A0000u && m->image.entry == 0x780A1000u && m->meta->function_count == 578 && m->meta->export_count == 445,
          "base, entry, function and export counts");
    CHECK(ctor >= m->meta->text_start && ctor < m->meta->text_end && lx_find_entry(ctor) != NULL, "a code export is a lifted entry point (0x%08X)", ctor);
    CHECK(cout_va >= m->image.base && cout_va < m->image.base + m->image.span && lx_find_entry(cout_va) == NULL,
          "a data export is an address in the mapped image, not a dispatch entry (0x%08X)", cout_va);
    for (uint16_t ordinal = 1; ordinal <= 445; ordinal++) {
        uint32_t va = lx_module_export(m, NULL, ordinal);
        if (!va) none++;
        else if (lx_find_entry(va)) code++;
        else data++;
    }
    CHECK(none == 0 && code == 390 && data == 55, "445 ordinals: 390 code, 55 data, 0 unresolved (%u, %u, %u)", code, data, none);
    CHECK(lx_module_export(m, "?nope@@YAXXZ", 0) == 0 && lx_module_export(m, NULL, 9999) == 0 && lx_module_export(m, NULL, 0) == 0,
          "an unknown name or ordinal has no address");
    CHECK(lx_module_export(m, "??0OFSTREAM@@QAE@XZ", 0) == 0, "names are matched exactly, case and decoration included");
}

static void test_win32_views(lx_module_t* m) {
    uint32_t scratch = gm_alloc_pages(0, 0x1000, "scratch");
    uint32_t ctor = lx_module_export(m, "??0ofstream@@QAE@XZ", 0);
    uint32_t ordinal_va = lx_module_export(m, NULL, 1);
    CHECK(api("GetModuleHandleA", 1, put_string(scratch, "msvcirt.dll"), 0) == m->image.base, "GetModuleHandleA(\"msvcirt.dll\") is the base");
    CHECK(api("GetModuleHandleA", 1, put_string(scratch, "MSVCIRT"), 0) == m->image.base, "without the extension, any case");
    CHECK(api("GetModuleHandleA", 1, 0, 0) == 0x00400000u, "NULL is the executable");
    CHECK(api("GetModuleHandleA", 1, put_string(scratch, "kernel32.dll"), 0) != 0, "a system module the host answers for");
    CHECK(m->refcount == 1, "GetModuleHandleA does not count a reference (%d)", m->refcount);
    CHECK(api("GetProcAddress", 2, m->image.base, put_string(scratch, "??0ofstream@@QAE@XZ")) == ctor, "GetProcAddress by name returns the guest address");
    CHECK(api("GetProcAddress", 2, m->image.base, 1) == ordinal_va && ordinal_va != 0, "GetProcAddress by ordinal (a small value in place of the name)");
    CHECK(api("GetProcAddress", 2, m->image.base, put_string(scratch, "?absent@@YAXXZ")) == 0, "an unknown export gives 0");
    CHECK(api("GetProcAddress", 2, m->image.base + 0x1000, put_string(scratch, "x")) == 0, "an address inside the image is not a module handle");
    gm_release(scratch);
}

static void test_lifetime(lx_module_t* m) {
    uint32_t scratch = gm_alloc_pages(0, 0x1000, "scratch");
    uint32_t name = put_string(scratch, "msvcirt.dll");
    CHECK(m->attach_count == 0 && m->refcount == 1, "before the process starts nothing has attached");
    CHECK(lx_modules_attach_imported() == 1 && m->attach_count == 1, "DllMain(DLL_PROCESS_ATTACH) runs for the imported module");
    CHECK(lx_modules_attach_imported() == 1 && m->attach_count == 1, "and only once");
    CHECK(api("LoadLibraryA", 1, name, 0) == m->image.base && api("LoadLibraryA", 1, put_string(scratch + 64, "MSVCIRT.DLL"), 0) == m->image.base,
          "LoadLibraryA returns the same module handle");
    CHECK(m->refcount == 3 && m->attach_count == 1, "each call counts a reference and does not attach again (%d)", m->refcount);
    CHECK(api("FreeLibrary", 1, m->image.base, 0) == 1 && api("FreeLibrary", 1, m->image.base, 0) == 1 && m->refcount == 1 && m->detach_count == 0,
          "FreeLibrary only drops a reference while others remain");
    CHECK(api("FreeLibrary", 1, m->image.base, 0) == 1 && m->refcount == 0 && m->detach_count == 1, "the last one runs DllMain(DLL_PROCESS_DETACH)");
    CHECK(api("FreeLibrary", 1, m->image.base, 0) == 0, "a module with no references cannot be freed again");
    CHECK(api("LoadLibraryA", 1, name, 0) == m->image.base && m->refcount == 1 && m->attach_count == 2, "loading it again attaches again");
    CHECK(api("FreeLibrary", 1, 0x12345678u, 0) == 0, "FreeLibrary of a handle that is no module fails");
    CHECK(api("LoadLibraryA", 1, put_string(scratch + 128, "msvcirt_missing.dll"), 0) == 0, "an unknown library is not found");
    gm_release(scratch);
}

static void test_ofstream_layout(lx_module_t* m) {
    uint32_t ctor = lx_module_export(m, "??0ofstream@@QAE@XZ", 0);
    uint32_t dtor = lx_module_export(m, "??1ofstream@@UAE@XZ", 0);
    uint32_t object = gm_heap_alloc(0x100, 1), vbptr, vbtable_second, filebuf_off;
    call_t c;
    memset(&c, 0, sizeof c);
    CHECK(ctor != 0 && dtor != 0 && object != 0, "the constructor and destructor exports, an object block");
    c.va = ctor;
    c.convention = LX_THISCALL;
    c.argc = 1;
    c.args[0] = 1;
    c.ecx = object;
    CHECK(call(&c) == LX_OK, "ofstream::ofstream() runs through the call spine: no missing import, bad dispatch or stack fault (%s)", lx_diag()->what);
    CHECK(c.result == object, "it returns this (0x%08X)", c.result);
    vbptr = read32(object);
    CHECK(vbptr >= m->image.base && vbptr < m->image.base + m->image.span, "the object's first word is a guest address inside the DLL's data: 0x%08X", vbptr);
    CHECK(read32(vbptr) == 0, "that virtual base table starts with 0");
    vbtable_second = read32(vbptr + 4);
    CHECK(vbtable_second >= 8 && vbtable_second < 0x100, "its second entry is the offset of the ios base inside the object (%u)", vbtable_second);
    filebuf_off = read32(object + vbtable_second + 4);
    CHECK(filebuf_off != 0, "the ios base holds the file buffer built by the constructor (0x%08X)", filebuf_off);
    CHECK(read32(object + vbtable_second) >= m->image.base && read32(object + vbtable_second) < m->image.base + m->image.span,
          "the ios base starts with its own vtable in the DLL's data");

    memset(&c, 0, sizeof c);
    c.va = dtor;
    c.convention = LX_THISCALL;
    c.ecx = object + 8;
    CHECK(call(&c) == LX_OK, "the destructor runs through the same path (%s)", lx_diag()->what);
    gm_heap_free(object);
}

static void test_wrong_dll_is_refused(void) {
    char root[256], err[160];
    char path[300];
    jpt_vpath_t* vp = jpt_vpath_new();
    size_t count;
    const lx_lift_t* image = lx_lift_get(JPT_LIFT_116);
    const lx_module_def_t* defs = image ? image->modules : NULL;
    count = image ? image->module_count : 0;
    FILE* f;
    snprintf(root, sizeof root, "%s/jpt_module_tests_%d", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    jpt_fs_mkdir(root);
    jpt_vpath_mount(vp, 'C', root);
    CHECK(!lx_modules_map(defs, count, vp, "en-116", err, sizeof err) && strstr(err, "not in the game folder"), "no DLL in the game folder: %s", err);
    snprintf(path, sizeof path, "%s/MSVCIRT.DLL", root);
    f = fopen(path, "wb");
    if (f) {
        fputs("MZ not the supported DLL", f);
        fclose(f);
    }
    jpt_vpath_invalidate(vp, root);
    CHECK(!lx_modules_map(defs, count, vp, "en-116", err, sizeof err) && strstr(err, "not the DLL this port was lifted from"),
          "another DLL under the same name is refused: %s", err);
    CHECK(lx_module_count() == 0, "and nothing is registered");
    jpt_fs_remove(path);
    jpt_fs_remove_dir(root);
    jpt_vpath_free(vp);
}

int main(int argc, char** argv) {
    const char* env = getenv("TRESPASSER_GAME_DIR");
    const char* game = argc > 1 ? argv[1] : env && *env ? env : "game";
    jpt_vpath_t* vp;
    size_t count;
    const lx_lift_t* image;
    const lx_module_def_t* defs;
    char err[160];
    lx_thread_t* thread;
    unsigned bound = 0, implemented = 0;

    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    lx_modules_reset();
    test_wrong_dll_is_refused();
    vp = jpt_vpath_new();
    if (!jpt_vpath_mount(vp, 'C', game)) return 2;
    image = lx_lift_get(JPT_LIFT_116);
    defs = image->modules;
    count = image->module_count;
    if (!lx_modules_map(defs, count, vp, "en-116", err, sizeof err)) {
        printf("linux_module_tests: skipped - DATA UNAVAILABLE (%s)\n", err);
        printf("linux_module_tests: %d checks, %d failures\n", g_checks + 1, g_failures);
        return g_failures ? 1 : 0;
    }
    hle_init(vp, 0x00400000u, "C:\\trespass.exe");
    hle_set_exit_handler(exit_handler);
    lx_spine_init((lx_table_t){ NULL, 0 }, (lx_table_t){ NULL, 0 });
    CHECK(lx_modules_bind(&bound, &implemented) && bound == 48, "the DLL's 48 imports bind through the host's resolver (%u, %u implemented)", bound, implemented);
    thread = lx_thread_new(2 * 1024 * 1024);
    lx_thread_attach(thread);

    test_lookup_and_exports(lx_module_at(0));
    test_win32_views(lx_module_at(0));
    test_lifetime(lx_module_at(0));
    test_ofstream_layout(lx_module_at(0));

    lx_thread_detach();
    lx_thread_free(thread);
    hle_shutdown();
    lx_modules_reset();
    jpt_vpath_free(vp);
    gm_shutdown();
    printf("linux_module_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
