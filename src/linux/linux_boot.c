#include "linux_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_fs.h"
#include "jpt_hash.h"
#include "jpt_vpath.h"
#include "linux_advapi32.h"
#include "linux_dsound.h"
#include "linux_guest_quit.h"
#include "linux_shutdown.h"
#include "linux_hle.h"
#include "jpt_user_data.h"
#include "linux_lift116.h"
#include "linux_log.h"
#include "linux_modules.h"
#include "linux_screen.h"
#include "linux_spine.h"
#include "linux_user32.h"

#define GUEST_STACK_BYTES (2u * 1024u * 1024u)

typedef struct {
    uint32_t entry;
} entry_call_t;

static void prepare_saves(jpt_vpath_t* vp, const lx_boot_options_t* o) {
    char saves[JPT_PATH_MAX];
    if (o->saves_dir) snprintf(saves, sizeof saves, "%s", o->saves_dir);
    else snprintf(saves, sizeof saves, "%s/saves", o->user_root ? o->user_root : "user_data");
    jpt_make_dirs(saves);
    jpt_vpath_mount(vp, 'D', saves);
    hle_advapi32_init(saves, "C:\\", "D:\\");
}

#define THREAD_JOIN_MS 3000u

static void run_entry(void* arg) {
    lx_shutdown_mark_guest_main();
    if (!lx_modules_attach_imported()) lx_abort(LX_FATAL_ABI, 0, "DllMain(DLL_PROCESS_ATTACH) failed");
    lx_call_guest(((entry_call_t*)arg)->entry, LX_CDECL, 0, NULL, 0, NULL);
}

int lx_boot_process_exit_code(const lx_boot_result_t* r) {
    return r->quit_reason != LX_QUIT_NONE ? lx_shutdown_exit_code() : (int)r->exit_code;
}

const char* lx_boot_status_name(lx_boot_status_t status) {
    switch (status) {
    case LX_BOOT_SETUP_FAILED: return "BOOTSTRAP_SETUP_FAILED";
    case LX_BOOT_DATA_UNAVAILABLE: return "BOOTSTRAP_DATA_UNAVAILABLE";
    case LX_BOOT_EXITED: return "BOOTSTRAP_EXITED";
    case LX_BOOT_MISSING_IMPORT: return "BOOTSTRAP_MISSING_IMPORT";
    case LX_BOOT_REACHED_USER32: return "BOOTSTRAP_REACHED_USER32";
    case LX_BOOT_FATAL_ABI: return "BOOTSTRAP_FATAL_ABI";
    case LX_BOOT_BAD_DISPATCH: return "BOOTSTRAP_BAD_DISPATCH";
    case LX_BOOT_NOT_LIFTED: return "BOOTSTRAP_NOT_LIFTED";
    case LX_BOOT_FRAME_PRESENTED: return "BOOTSTRAP_FRAME_PRESENTED";
    case LX_BOOT_IDLE: return "BOOTSTRAP_IDLE";
    }
    return "BOOTSTRAP_UNKNOWN";
}

static lx_boot_status_t finish(lx_boot_result_t* r, lx_boot_status_t status, const char* detail) {
    r->status = status;
    if (detail) snprintf(r->detail, sizeof r->detail, "%s", detail);
    return status;
}

static void on_exit_request(uint32_t code) {
    if (code == HLE_EXIT_MISSING) {
        const char* name = ht_name(lx_current_import_va());
        lx_abort(LX_MISSING_IMPORT, lx_current_import_va(), name ? name : "?");
    }
    lx_abort(LX_EXIT, code, "ExitProcess");
}

static int read_file(const char* path, uint8_t** data, size_t* size) {
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    *data = (uint8_t*)malloc((size_t)n);
    if (!*data || fread(*data, 1, (size_t)n, f) != (size_t)n) {
        free(*data);
        fclose(f);
        return 0;
    }
    fclose(f);
    *size = (size_t)n;
    return 1;
}

static int find_image(jpt_vpath_t* vp, const char* version, char* host, size_t cap) {
    char guest[JPT_PATH_MAX];
    snprintf(guest, sizeof guest, "C:\\versions\\%s\\guest.img", version);
    return jpt_vpath_resolve(vp, guest, 0, host, cap) == JPT_VP_OK ||
           jpt_vpath_resolve(vp, "C:\\guest.img", 0, host, cap) == JPT_VP_OK ||
           jpt_vpath_resolve(vp, "C:\\trespass.exe", 0, host, cap) == JPT_VP_OK;
}

typedef struct {
    unsigned total, implemented;
} bind_count_t;

static uint32_t bind_import(const char* dll, const char* name, uint16_t ordinal, void* user) {
    bind_count_t* c = (bind_count_t*)user;
    c->total++;
    if (hle_find_import(dll, name, ordinal)) c->implemented++;
    return hle_resolve(dll, name, ordinal, NULL);
}

static lx_boot_status_t classify(const lx_diag_t* d, lx_boot_result_t* r) {
    r->guest_eip = lx_guest_eip();
    r->caller = d->caller;
    switch (d->status) {
    case LX_EXIT:
        r->exit_code = d->va;
        return finish(r, LX_BOOT_EXITED, d->what);
    case LX_MISSING_IMPORT:
        r->guest_eip = d->caller;
        return finish(r, !strncmp(d->what, "user32.dll!", 11) ? LX_BOOT_REACHED_USER32 : LX_BOOT_MISSING_IMPORT, d->what);
    case LX_STOP:
        return finish(r, !strncmp(d->what, "frame", 5) ? LX_BOOT_FRAME_PRESENTED : LX_BOOT_IDLE, d->what);
    case LX_BAD_DISPATCH: return finish(r, LX_BOOT_BAD_DISPATCH, d->what);
    case LX_NOT_LIFTED: return finish(r, LX_BOOT_NOT_LIFTED, d->what);
    default: return finish(r, LX_BOOT_FATAL_ABI, d->what);
    }
}

void lx_boot_options_init(lx_boot_options_t* o, const char* game_dir, const char* version, const char* user_root, const char* saves_dir,
                          lx_window_backend_t* backend, int unbounded) {
    memset(o, 0, sizeof *o);
    o->game_dir = game_dir, o->version = version, o->user_root = user_root, o->saves_dir = saves_dir;
    o->backend = backend, o->unbounded = unbounded;
}

lx_boot_status_t lx_boot_headless(const lx_boot_options_t* o, lx_boot_result_t* r) {
    const jpt_lift_meta_t* meta = lx_lift116_meta();
    char host_image[JPT_PATH_MAX], hash[65], err[128];
    uint8_t* bytes = NULL;
    size_t size = 0;
    jpt_vpath_t* vp = jpt_vpath_new();
    gm_image_t img;
    bind_count_t counts = { 0, 0 };
    lx_thread_t* thread;
    entry_call_t call;
    lx_status_t run_status;
    lx_boot_status_t status;
    int threads_ended;
    const uint32_t* reached;
    const lx_module_def_t* module_defs;
    size_t module_count;

    memset(r, 0, sizeof *r);
    if (!vp || !jpt_vpath_mount(vp, 'C', o->game_dir)) return finish(r, LX_BOOT_SETUP_FAILED, "cannot map the game folder");
    if (!find_image(vp, o->version ? o->version : "en-116", host_image, sizeof host_image)) {
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_DATA_UNAVAILABLE, "no guest image in the game folder");
    }
    if (!read_file(host_image, &bytes, &size)) return finish(r, LX_BOOT_SETUP_FAILED, "cannot read the guest image");
    jpt_sha256_buffer_hex(bytes, size, hash);
    if (strcmp(hash, meta->source_sha256) != 0) {
        free(bytes);
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_SETUP_FAILED, "the guest image is not the build lift116 was generated from");
    }
    if (!gm_init(0, 0) || !ht_init() ||
        gm_pe_map_memory(bytes, size, GM_PE_PREFERRED, 0, &img, err, sizeof err) != 0) {
        free(bytes);
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_SETUP_FAILED, "cannot lay the guest image out in its window");
    }
    free(bytes);
    r->entry = img.entry;
    r->selfmod_entries = (unsigned)lx_lift116_selfmod_table().count;
    r->hooks_not_installed = lx_lift116_hook_count();
    r->overrides_not_installed = lx_lift116_override_count();
    if (img.base != meta->image_base || img.entry != meta->entry_va) {
        gm_shutdown();
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_SETUP_FAILED, "the guest image does not sit where the lift expects");
    }

    lx_modules_reset();
    module_defs = lx_lift116_modules(&module_count);
    if (!lx_modules_map(module_defs, module_count, vp, o->version ? o->version : "en-116", err, sizeof err)) {
        gm_shutdown();
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_SETUP_FAILED, err);
    }
    lx_dsound_set_headless(o->backend == NULL);
    hle_init(vp, img.base, "C:\\trespass.exe");
    hle_set_exit_handler(on_exit_request);
    prepare_saves(vp, o);
    lx_window_set_backend(o->backend);
    lx_virtual_reset();
    lx_screen_set_frame_hook(o->frame_hook, o->hook_user);
    lx_screen_set_frame_limit(o->unbounded ? 0 : o->frame_limit ? o->frame_limit : 1);
    lx_user32_set_idle_limit(o->unbounded ? 0 : o->idle_limit ? o->idle_limit : 200000);
    lx_spine_init(lx_lift116_main_table(), lx_lift116_selfmod_table());
    if (!lx_modules_bind(&r->module_imports, &r->module_imports_implemented))
        return finish(r, LX_BOOT_SETUP_FAILED, "a guest module's imports cannot be bound");
    gm_pe_bind(&img, bind_import, &counts, NULL);
    r->imports_total = counts.total;
    r->imports_implemented = counts.implemented;
    lx_log("Linux guest", "entry %08X, SelfMod lifted entries %u, %u lift hooks and %u overrides not installed", r->entry,
           r->selfmod_entries, r->hooks_not_installed, r->overrides_not_installed);
    lx_log("Linux HLE", "resolved %u/%u", r->imports_implemented, r->imports_total);

    thread = lx_thread_new(GUEST_STACK_BYTES);
    if (!thread) {
        gm_shutdown();
        jpt_vpath_free(vp);
        return finish(r, LX_BOOT_SETUP_FAILED, "cannot allocate the guest stack");
    }
    lx_shutdown_new_run();
    lx_guest_quit_reset();
    lx_thread_attach(thread);
    call.entry = r->entry;
    run_status = lx_run(run_entry, &call);
    status = run_status == LX_OK ? finish(r, LX_BOOT_EXITED, "the entry point returned") : classify(lx_diag(), r);
    lx_shutdown_begin();
    r->quit_reason = (int)lx_shutdown_reason();
    r->host_ended_guest = lx_shutdown_forced_ends();
    r->windows_ended_by_host = lx_guest_quit_windows_ended();
    hle_retire_guest_threads();
    r->imports_reached = lx_reached_imports(&reached);
    r->modules = (unsigned)lx_module_count();
    for (size_t i = 0; i < lx_module_count(); i++) {
        r->module_functions += lx_module_at(i)->meta->function_count;
        r->dll_attach_count += lx_module_at(i)->attach_count;
    }
    lx_thread_detach();
    threads_ended = hle_join_guest_threads(THREAD_JOIN_MS);
    lx_thread_free(thread);
    hle_shutdown();
    lx_screen_window_close();
    lx_modules_reset();
    jpt_vpath_free(vp);
    r->cleanup_complete = threads_ended;
    if (threads_ended) gm_shutdown();
    else lx_log("boot", "guest memory stays mapped: a guest thread is still running");
    lx_shutdown_stopped();
    if (r->quit_reason != LX_QUIT_NONE)
        lx_log("quit", "stopped after %s: %s", lx_shutdown_reason_name((lx_quit_reason_t)r->quit_reason), r->host_ended_guest ? "the host ended the guest" : "the game left by itself");
    return status;
}
