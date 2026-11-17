/*
 * --perf-log: sampling profile of the guest thread. Every 2 ms a host thread suspends the guest thread, reads its
 * instruction pointer and resumes it; nothing between the suspend and the resume may take a lock (the guest thread can
 * hold the heap or CRT lock). A sample inside a lifted function is charged to its guest address; one in the host's own
 * code to a 32-byte block, logged as an offset in the executable to be named with its PDB.
 */
#include "host.h"
#include "recomp_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MS 2
#define MAX_MODULES 32
#define TOP_FUNCTIONS 10
#define TOP_HOST_BLOCKS 12
#define MAX_WORKERS 8
#define HOST_SLOTS 4096u
#define HOST_BLOCK_SHIFT 5
#define SYSTEM_SLOTS 512u
#define TOP_SYSTEM 8
#define STACK_WORDS 256u
#define ICALL_SLOTS 1024u
#define TOP_ICALLS 12

typedef struct {
    uintptr_t start;
    uint32_t va;
} lifted_fn_t;

typedef struct {
    uintptr_t base, end;
    char name[32];
    uint32_t* export_rva; /* sorted, with export_name in the same order */
    const char** export_name;
    unsigned export_count;
    int exports_read;
} module_range_t;

/* A sample in a system DLL: the export it is in, and the nearest caller in the guest image on the stack (a lifted
 * function's guest address, or a host offset). */
typedef struct {
    uint32_t key, caller;
    const char* import;
    unsigned hits;
} system_hit_t;

enum { BUCKET_LIFTED, BUCKET_SELFMOD, BUCKET_HOST, BUCKET_GRAPHICS, BUCKET_SYSTEM, BUCKET_OTHER, BUCKET_COUNT };

static const char* const k_bucket_names[BUCKET_COUNT] = { "lifted game code", "game native code", "port (host)",
                                                           "graphics driver", "system/waits", "other DLLs" };

/* Coarse subsystems by guest address, from the RTTI classes' method addresses (the linker kept the game's module order). */
static const struct {
    uint32_t start;
    const char* name;
} k_ranges[] = {
    { 0x00401000u, "UI/app" },          { 0x00419000u, "AI" },
    { 0x00436000u, "audio" },           { 0x0043E000u, "player/animation" },
    { 0x00465000u, "triggers/actions" }, { 0x00476000u, "partitions/scene" },
    { 0x0047C000u, "terrain" },         { 0x00488000u, "shapes/meshes" },
    { 0x00492000u, "terrain" },         { 0x004A2700u, "partitions/scene" },
    { 0x004A6E00u, "visibility/clipping" }, { 0x004A8900u, "texture paging" },
    { 0x004AA000u, "loader/values" },   { 0x004B7000u, "renderer setup" },
    { 0x004CF000u, "physics" },         { 0x00503000u, "raster backend" },
    { 0x0050F000u, "D3D rasters/texture memory" }, { 0x00516700u, "shapes/meshes" },
    { 0x0051C000u, "render pipeline" }, { 0x00544000u, "image cache" },
    { 0x00549000u, "rasteriser primitives" }, { 0x005F5000u, "system/paging" },
    { 0x00600000u, "world/entities" },  { 0x0060F000u, "water" },
    { 0x00611000u, "world/entities" },  { 0x00624A00u, "scene render" },
    { 0x00626B70u, "C runtime" },       { 0x006271A0u, "triggers/actions" },
    { 0x00631600u, "visibility/clipping" }, { 0x00636F80u, "water" },
    { 0x00639200u, "other" },           { 0x0063B000u, "game native code" },
};
#define RANGE_COUNT (sizeof k_ranges / sizeof k_ranges[0])

typedef struct {
    unsigned* fn_hits;
    unsigned bucket[BUCKET_COUNT];
    unsigned selfmod_range[RANGE_COUNT];
    unsigned module_hits[MAX_MODULES];
    unsigned total;
    unsigned worker_busy[MAX_WORKERS], worker_samples[MAX_WORKERS];
    uint32_t host_block[HOST_SLOTS];
    unsigned host_hits[HOST_SLOTS];
    system_hit_t system[SYSTEM_SLOTS];
    uint32_t icall_target[ICALL_SLOTS];
    unsigned icall_hits[ICALL_SLOTS], icall_samples;
} profile_t;

static lifted_fn_t* s_fns;
static unsigned s_fn_count;
static uintptr_t s_lifted_lo, s_lifted_hi, s_image_lo, s_image_hi;
static module_range_t s_modules[MAX_MODULES];
static unsigned s_module_count;
static HANDLE s_main;
static struct {
    HANDLE handle;
    uint32_t entry;
} s_workers[MAX_WORKERS];
static volatile LONG s_worker_count;
static profile_t s_window, s_session;
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_running;
static LARGE_INTEGER s_window_start;
static uint32_t s_icalls_at_start;
static unsigned long long s_cache_hits_at_start, s_cache_misses_at_start;

static uintptr_t function_body(recomp_func_t f) {
    const BYTE* p = (const BYTE*)f;
    if (p[0] == 0xE9) {
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        return (uintptr_t)(p + 5 + rel);
    }
    return (uintptr_t)p;
}

static int by_start(const void* a, const void* b) {
    uintptr_t x = ((const lifted_fn_t*)a)->start, y = ((const lifted_fn_t*)b)->start;
    return x < y ? -1 : x > y;
}

static int find_function(uintptr_t ip) {
    unsigned lo = 0, hi = s_fn_count;
    if (ip < s_lifted_lo || ip >= s_lifted_hi) return -1;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (s_fns[mid].start <= ip) lo = mid;
        else hi = mid;
    }
    return (int)lo;
}

static unsigned range_of(uint32_t va) {
    unsigned i = 0;
    while (i + 1 < RANGE_COUNT && k_ranges[i + 1].start <= va) i++;
    return i;
}

static int module_of(uintptr_t ip) {
    MEMORY_BASIC_INFORMATION m;
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS* nt;
    char path[MAX_PATH];
    const char* name;
    for (unsigned i = 0; i < s_module_count; i++)
        if (ip >= s_modules[i].base && ip < s_modules[i].end) return (int)i;
    if (s_module_count == MAX_MODULES || !VirtualQuery((LPCVOID)ip, &m, sizeof m) || !m.AllocationBase) return -1;
    dos = (const IMAGE_DOS_HEADER*)m.AllocationBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    nt = (const IMAGE_NT_HEADERS*)((const BYTE*)dos + dos->e_lfanew);
    if (!GetModuleFileNameA((HMODULE)m.AllocationBase, path, sizeof path)) return -1;
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    s_modules[s_module_count].base = (uintptr_t)dos;
    s_modules[s_module_count].end = (uintptr_t)dos + nt->OptionalHeader.SizeOfImage;
    strncpy(s_modules[s_module_count].name, name, sizeof s_modules[0].name - 1);
    _strlwr(s_modules[s_module_count].name);
    return (int)s_module_count++;
}

static int module_bucket(const char* name) {
    static const char* const graphics[] = { "d3d", "dxgi", "nv", "igd", "igx", "amd", "ati", "dxcore" };
    static const char* const system[] = { "ntdll", "kernel", "win32u", "user32", "gdi32", "combase", "ucrtbase",
                                          "msvcrt" };
    for (size_t i = 0; i < sizeof graphics / sizeof graphics[0]; i++)
        if (!strncmp(name, graphics[i], strlen(graphics[i]))) return BUCKET_GRAPHICS;
    for (size_t i = 0; i < sizeof system / sizeof system[0]; i++)
        if (!strncmp(name, system[i], strlen(system[i]))) return BUCKET_SYSTEM;
    return BUCKET_OTHER;
}

static void add_host_block(profile_t* p, uint32_t block, unsigned hits) {
    unsigned i = (block * 2654435761u) & (HOST_SLOTS - 1u);
    for (unsigned probe = 0; probe < 32; probe++, i = (i + 1u) & (HOST_SLOTS - 1u)) {
        if (p->host_block[i] == block || !p->host_hits[i]) {
            p->host_block[i] = block;
            p->host_hits[i] += hits;
            return;
        }
    }
}

static int by_rva(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void read_exports(module_range_t* mod) {
    const BYTE* base = (const BYTE*)mod->base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const IMAGE_EXPORT_DIRECTORY* e;
    const uint32_t *funcs, *names;
    const uint16_t* ords;
    uint32_t* pairs;
    mod->exports_read = 1;
    if (!dd->VirtualAddress || !dd->Size) return;
    e = (const IMAGE_EXPORT_DIRECTORY*)(base + dd->VirtualAddress);
    funcs = (const uint32_t*)(base + e->AddressOfFunctions);
    names = (const uint32_t*)(base + e->AddressOfNames);
    ords = (const uint16_t*)(base + e->AddressOfNameOrdinals);
    pairs = (uint32_t*)malloc(e->NumberOfNames * 2 * sizeof *pairs);
    mod->export_rva = (uint32_t*)malloc(e->NumberOfNames * sizeof *mod->export_rva);
    mod->export_name = (const char**)malloc(e->NumberOfNames * sizeof *mod->export_name);
    if (!pairs || !mod->export_rva || !mod->export_name) return;
    for (uint32_t i = 0; i < e->NumberOfNames; i++) {
        pairs[2 * i] = funcs[ords[i]];
        pairs[2 * i + 1] = names[i];
    }
    qsort(pairs, e->NumberOfNames, 2 * sizeof *pairs, by_rva);
    for (uint32_t i = 0; i < e->NumberOfNames; i++) {
        mod->export_rva[i] = pairs[2 * i];
        mod->export_name[i] = (const char*)(base + pairs[2 * i + 1]);
    }
    mod->export_count = e->NumberOfNames;
    free(pairs);
}

static int export_of(module_range_t* mod, uintptr_t ip) {
    uint32_t rva = (uint32_t)(ip - mod->base);
    unsigned lo = 0, hi;
    if (!mod->exports_read) read_exports(mod);
    hi = mod->export_count;
    if (!hi || rva < mod->export_rva[0]) return -1;
    while (hi - lo > 1) {
        unsigned mid = (lo + hi) / 2;
        if (mod->export_rva[mid] <= rva) lo = mid;
        else hi = mid;
    }
    return (int)lo;
}

/* The first word on the stack that returns into the guest image: its lifted function, or its host 32-byte block. */
static uint32_t stack_caller(const uint32_t* stack, unsigned words) {
    for (unsigned i = 0; i < words; i++) {
        uintptr_t v = stack[i];
        int fn;
        if (v < s_image_lo || v >= s_image_hi) continue;
        fn = find_function(v);
        return fn >= 0 ? s_fns[fn].va : 0x80000000u | (uint32_t)((v - s_image_lo) >> HOST_BLOCK_SHIFT);
    }
    return 0;
}

static void add_system(profile_t* p, uint32_t key, uint32_t caller, const char* import) {
    unsigned i = ((key ^ caller) * 2654435761u) & (SYSTEM_SLOTS - 1u);
    for (unsigned probe = 0; probe < 32; probe++, i = (i + 1u) & (SYSTEM_SLOTS - 1u)) {
        system_hit_t* h = &p->system[i];
        if (h->hits && (h->key != key || h->caller != caller)) continue;
        h->key = key;
        h->caller = caller;
        if (!h->hits) h->import = import;
        h->hits++;
        return;
    }
}

static void add_icall(profile_t* p, uint32_t target) {
    unsigned i = (target * 2654435761u) & (ICALL_SLOTS - 1u);
    p->icall_samples++;
    for (unsigned probe = 0; probe < 32; probe++, i = (i + 1u) & (ICALL_SLOTS - 1u))
        if (p->icall_target[i] == target || !p->icall_hits[i]) {
            p->icall_target[i] = target;
            p->icall_hits[i]++;
            return;
        }
}

static void record(profile_t* p, uintptr_t ip, const uint32_t* stack, unsigned words, const char* import,
                   uint32_t icall) {
    int fn = find_function(ip), m = -1, bucket, ex = -1;
    if (fn >= 0) bucket = BUCKET_LIFTED;
    else if (ip >= lift_selfmod_start && ip < lift_selfmod_end) bucket = BUCKET_SELFMOD;
    else if (ip >= s_image_lo && ip < s_image_hi) bucket = BUCKET_HOST;
    else bucket = (m = module_of(ip)) < 0 ? BUCKET_OTHER : module_bucket(s_modules[m].name);
    if (bucket == BUCKET_SYSTEM) ex = export_of(&s_modules[m], ip);
    AcquireSRWLockExclusive(&s_lock);
    if (bucket == BUCKET_SYSTEM)
        add_system(p, ((uint32_t)m << 20) | (uint32_t)(ex & 0xFFFFF), stack_caller(stack, words), import);
    if (icall) add_icall(p, icall);
    p->total++;
    p->bucket[bucket]++;
    if (fn >= 0) p->fn_hits[fn]++;
    if (bucket == BUCKET_SELFMOD) p->selfmod_range[range_of((uint32_t)ip)]++;
    if (bucket == BUCKET_HOST) add_host_block(p, (uint32_t)((ip - s_image_lo) >> HOST_BLOCK_SHIFT), 1);
    if (m >= 0) p->module_hits[m]++;
    ReleaseSRWLockExclusive(&s_lock);
}

/* The stack is read while the thread is held, with ReadProcessMemory: a bad address fails instead of faulting. */
static int sample(HANDLE thread, uintptr_t* ip, uint32_t* stack, unsigned* words) {
    CONTEXT c;
    BOOL ok;
    SIZE_T got = 0;
    if (SuspendThread(thread) == (DWORD)-1) return 0;
    c.ContextFlags = CONTEXT_CONTROL;
    ok = GetThreadContext(thread, &c);
    if (ok && stack)
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)c.Esp, stack, STACK_WORDS * sizeof *stack, &got);
    ResumeThread(thread);
    *ip = c.Eip;
    if (words) *words = (unsigned)(got / sizeof *stack);
    return ok;
}

static void sample_workers(void) {
    LONG n = s_worker_count;
    for (LONG i = 0; i < n && i < MAX_WORKERS; i++) {
        uintptr_t ip;
        if (WaitForSingleObject(s_workers[i].handle, 0) != WAIT_TIMEOUT || !sample(s_workers[i].handle, &ip, NULL, NULL))
            continue;
        AcquireSRWLockExclusive(&s_lock);
        s_window.worker_samples[i]++;
        if (find_function(ip) >= 0 || (ip >= lift_selfmod_start && ip < lift_selfmod_end))
            s_window.worker_busy[i]++;
        ReleaseSRWLockExclusive(&s_lock);
    }
}

static DWORD WINAPI sampler(LPVOID unused) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    unsigned tick = 0;
    (void)unused;
    static uint32_t stack[STACK_WORDS];
    while (s_running) {
        uintptr_t ip;
        unsigned words = 0;
        const char* import;
        uint32_t icall;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10000LL * SAMPLE_MS;
            if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) || WaitForSingleObject(timer, INFINITE)) Sleep(SAMPLE_MS);
        } else {
            Sleep(SAMPLE_MS);
        }
        if (WaitForSingleObject(s_main, 0) != WAIT_TIMEOUT) break;
        import = g_cur_import;
        icall = g_icall_trace_idx ? g_icall_trace[(g_icall_trace_idx - 1u) % ICALL_TRACE_SIZE] : 0;
        if (!sample(s_main, &ip, stack, &words)) continue;
        record(&s_window, ip, stack, words, import, icall);
        if (++tick % 8 == 0) sample_workers();
    }
    if (timer) CloseHandle(timer);
    return 0;
}

void guest_profile_start(HANDLE guest_thread) {
    HMODULE self = GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)self + ((const IMAGE_DOS_HEADER*)self)->e_lfanew);
    HANDLE thread;
    const lift_image_t* image = lift_active_image();
    if (!host_perf_logging()) return;
    s_fn_count = image->meta->function_count;
    s_fns = (lifted_fn_t*)malloc(s_fn_count * sizeof *s_fns);
    s_window.fn_hits = (unsigned*)calloc(s_fn_count, sizeof(unsigned));
    s_session.fn_hits = (unsigned*)calloc(s_fn_count, sizeof(unsigned));
    if (!s_fns || !s_window.fn_hits || !s_session.fn_hits ||
        !DuplicateHandle(GetCurrentProcess(), guest_thread, GetCurrentProcess(), &s_main, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    for (unsigned i = 0; i < s_fn_count; i++) {
        recomp_func_t lifted = guest_override_lifted(image->dispatch[i].address);
        s_fns[i].start = lifted ? (uintptr_t)lifted : function_body(image->dispatch[i].func);
        s_fns[i].va = image->dispatch[i].address;
    }
    qsort(s_fns, s_fn_count, sizeof *s_fns, by_start);
    s_lifted_lo = s_fns[0].start;
    s_lifted_hi = s_fns[s_fn_count - 1].start + 4096;
    s_image_lo = (uintptr_t)self;
    s_image_hi = s_image_lo + nt->OptionalHeader.SizeOfImage;
    QueryPerformanceCounter(&s_window_start);
    s_icalls_at_start = g_icall_count;
    s_running = 1;
    thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!thread) {
        s_running = 0;
        return;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
    CloseHandle(thread);
    host_log("profile", "guest CPU profile: the guest thread is sampled every %d ms (%u lifted functions mapped)", SAMPLE_MS,
             s_fn_count);
}

void guest_profile_add_worker(HANDLE thread, uint32_t entry) {
    LONG n = s_worker_count;
    if (!s_running || n >= MAX_WORKERS) return;
    if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &s_workers[n].handle,
                         SYNCHRONIZE | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0))
        return;
    s_workers[n].entry = entry;
    InterlockedExchange(&s_worker_count, n + 1);
}

static double share(unsigned n, unsigned total) { return total ? 100.0 * n / total : 0.0; }

static void log_host_blocks(const profile_t* p, const char* when) {
    char line[900];
    uint32_t top_block[TOP_HOST_BLOCKS];
    unsigned top_hits[TOP_HOST_BLOCKS], top_n = 0, k;
    int n = 0;
    for (unsigned i = 0; i < HOST_SLOTS; i++) {
        unsigned h = p->host_hits[i];
        if (!h) continue;
        if (top_n < TOP_HOST_BLOCKS) top_n++;
        else if (h <= top_hits[TOP_HOST_BLOCKS - 1]) continue;
        for (k = top_n - 1; k > 0 && top_hits[k - 1] < h; k--) {
            top_hits[k] = top_hits[k - 1];
            top_block[k] = top_block[k - 1];
        }
        top_hits[k] = h;
        top_block[k] = p->host_block[i];
    }
    for (k = 0; k < top_n; k++)
        n += _snprintf(line + n, sizeof line - 1 - n, "%s+0x%07X %.1f%%", k ? ", " : "",
                       top_block[k] << HOST_BLOCK_SHIFT, share(top_hits[k], p->total));
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, hottest port code (32-byte blocks, offsets in the guest image): %s", when, line);
}

static void describe_caller(uint32_t caller, char* out, size_t cap) {
    if (!caller) _snprintf(out, cap - 1, "?");
    else if (caller & 0x80000000u) _snprintf(out, cap - 1, "+0x%07X", (caller & 0x7FFFFFFFu) << HOST_BLOCK_SHIFT);
    else _snprintf(out, cap - 1, "sub_%08X", caller);
    out[cap - 1] = 0;
}

/* Which system API the guest thread is in, who called it, and the guest's import at that moment. */
static void log_system(const profile_t* p, const char* when) {
    char line[1400], caller[24];
    const system_hit_t* top[TOP_SYSTEM];
    unsigned top_n = 0, k;
    int n = 0;
    for (unsigned i = 0; i < SYSTEM_SLOTS; i++) {
        const system_hit_t* h = &p->system[i];
        if (!h->hits) continue;
        if (top_n < TOP_SYSTEM) top_n++;
        else if (h->hits <= top[TOP_SYSTEM - 1]->hits) continue;
        for (k = top_n - 1; k > 0 && top[k - 1]->hits < h->hits; k--) top[k] = top[k - 1];
        top[k] = h;
    }
    for (k = 0; k < top_n && n < (int)sizeof line - 200; k++) {
        unsigned m = top[k]->key >> 20, ex = top[k]->key & 0xFFFFFu;
        const module_range_t* mod = m < s_module_count ? &s_modules[m] : NULL;
        const char* api = mod && ex < mod->export_count ? mod->export_name[ex] : "?";
        describe_caller(top[k]->caller, caller, sizeof caller);
        n += _snprintf(line + n, sizeof line - 1 - n, "%s%s!%s from %s (guest import %s) %.1f%%", k ? ", " : "",
                       mod ? mod->name : "?", api, caller, top[k]->import ? top[k]->import : "none",
                       share(top[k]->hits, p->total));
    }
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, system time by API: %s", when, line);
}

static void log_icalls(const profile_t* p, const char* when) {
    char line[900];
    uint32_t top_target[TOP_ICALLS];
    unsigned top_hits[TOP_ICALLS], top_n = 0, k;
    int n = 0;
    for (unsigned i = 0; i < ICALL_SLOTS; i++) {
        unsigned h = p->icall_hits[i];
        if (!h) continue;
        if (top_n < TOP_ICALLS) top_n++;
        else if (h <= top_hits[TOP_ICALLS - 1]) continue;
        for (k = top_n - 1; k > 0 && top_hits[k - 1] < h; k--) {
            top_hits[k] = top_hits[k - 1];
            top_target[k] = top_target[k - 1];
        }
        top_hits[k] = h;
        top_target[k] = p->icall_target[i];
    }
    for (k = 0; k < top_n; k++)
        n += _snprintf(line + n, sizeof line - 1 - n, "%s0x%08X %.1f%%", k ? ", " : "", top_target[k],
                       share(top_hits[k], p->icall_samples));
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, newest indirect-call target when sampled: %s", when, line);
}

static void log_profile(const profile_t* p, const char* when) {
    char line[900];
    unsigned category[RANGE_COUNT];
    uint32_t top_va[TOP_FUNCTIONS];
    unsigned top_hits[TOP_FUNCTIONS], top_n = 0;
    int n = 0;
    if (!p->total) return;
    memcpy(category, p->selfmod_range, sizeof category);
    for (unsigned i = 0; i < s_fn_count; i++) {
        unsigned h = p->fn_hits[i], k;
        if (!h) continue;
        category[range_of(s_fns[i].va)] += h;
        if (top_n < TOP_FUNCTIONS) top_n++;
        else if (h <= top_hits[TOP_FUNCTIONS - 1]) continue;
        for (k = top_n - 1; k > 0 && top_hits[k - 1] < h; k--) {
            top_hits[k] = top_hits[k - 1];
            top_va[k] = top_va[k - 1];
        }
        top_hits[k] = h;
        top_va[k] = s_fns[i].va;
    }
    for (int b = 0; b < BUCKET_COUNT; b++)
        if (p->bucket[b])
            n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %.1f%%", n ? ", " : "", k_bucket_names[b],
                           share(p->bucket[b], p->total));
    line[sizeof line - 1] = 0;
    host_log("profile", "%s, guest thread (%u samples): %s", when, p->total, line);
    n = 0;
    for (int pass = 0; pass < 8; pass++) {
        unsigned best = 0, best_i = 0;
        for (unsigned r = 0; r < RANGE_COUNT; r++) {
            int seen = 0;
            for (unsigned q = 0; q < r; q++)
                if (!strcmp(k_ranges[q].name, k_ranges[r].name)) seen = 1;
            if (seen) continue;
            unsigned sum = 0;
            for (unsigned q = r; q < RANGE_COUNT; q++)
                if (!strcmp(k_ranges[q].name, k_ranges[r].name)) sum += category[q];
            if (sum > best) {
                best = sum;
                best_i = r;
            }
        }
        if (!best) break;
        n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %.1f%%", n ? ", " : "", k_ranges[best_i].name,
                       share(best, p->total));
        for (unsigned q = 0; q < RANGE_COUNT; q++)
            if (!strcmp(k_ranges[q].name, k_ranges[best_i].name)) category[q] = 0;
    }
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, game code by subsystem: %s", when, line);
    n = 0;
    for (unsigned i = 0; i < top_n; i++)
        n += _snprintf(line + n, sizeof line - 1 - n, "%ssub_%08X %.1f%%", i ? ", " : "", top_va[i],
                       share(top_hits[i], p->total));
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, hottest lifted functions: %s", when, line);
    log_host_blocks(p, when);
    log_system(p, when);
    log_icalls(p, when);
    n = 0;
    for (unsigned m = 0; m < s_module_count; m++)
        if (share(p->module_hits[m], p->total) >= 1.0)
            n += _snprintf(line + n, sizeof line - 1 - n, "%s%s %.1f%%", n ? ", " : "", s_modules[m].name,
                           share(p->module_hits[m], p->total));
    line[sizeof line - 1] = 0;
    if (n) host_log("profile", "%s, DLLs: %s", when, line);
    for (LONG i = 0; i < s_worker_count && i < MAX_WORKERS; i++)
        if (p->worker_samples[i])
            host_log("profile", "%s, guest worker sub_%08X busy in game code %.1f%% of its samples", when,
                     s_workers[i].entry, share(p->worker_busy[i], p->worker_samples[i]));
}

static void add_into(profile_t* total, const profile_t* p) {
    for (unsigned i = 0; i < s_fn_count; i++) total->fn_hits[i] += p->fn_hits[i];
    for (int b = 0; b < BUCKET_COUNT; b++) total->bucket[b] += p->bucket[b];
    for (unsigned r = 0; r < RANGE_COUNT; r++) total->selfmod_range[r] += p->selfmod_range[r];
    for (unsigned m = 0; m < MAX_MODULES; m++) total->module_hits[m] += p->module_hits[m];
    for (int w = 0; w < MAX_WORKERS; w++) {
        total->worker_busy[w] += p->worker_busy[w];
        total->worker_samples[w] += p->worker_samples[w];
    }
    for (unsigned i = 0; i < HOST_SLOTS; i++)
        if (p->host_hits[i]) add_host_block(total, p->host_block[i], p->host_hits[i]);
    for (unsigned i = 0; i < SYSTEM_SLOTS; i++)
        for (unsigned h = 0; h < p->system[i].hits; h++)
            add_system(total, p->system[i].key, p->system[i].caller, p->system[i].import);
    for (unsigned i = 0; i < ICALL_SLOTS; i++)
        for (unsigned h = 0; h < p->icall_hits[i]; h++) add_icall(total, p->icall_target[i]);
    total->total += p->total;
}

static void log_indirect_calls(void) {
    LARGE_INTEGER now, f;
    unsigned long long hits, misses, calls;
    uint32_t icalls = g_icall_count;
    double seconds;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    seconds = (double)(now.QuadPart - s_window_start.QuadPart) / (double)f.QuadPart;
    dispatch_cache_counts(&hits, &misses);
    hits -= s_cache_hits_at_start;
    misses -= s_cache_misses_at_start;
    calls = hits + misses;
    if (seconds > 0.0)
        host_log("profile", "last 5 s, lifted indirect calls %.2f million per second, %.1f%% of those into the game's "
                 "code answered by the dispatch cache", (double)(uint32_t)(icalls - s_icalls_at_start) / seconds / 1e6,
                 calls ? 100.0 * (double)hits / (double)calls : 0.0);
    s_window_start = now;
    s_icalls_at_start = icalls;
    s_cache_hits_at_start += hits;
    s_cache_misses_at_start += misses;
}

/* Called with the renderer's 5-second performance lines; the copy is taken under the lock, the logging after it. */
void guest_profile_window(void) {
    static profile_t copy;
    unsigned* hits;
    if (!s_running) return;
    if (!copy.fn_hits && !(copy.fn_hits = (unsigned*)calloc(s_fn_count, sizeof(unsigned)))) return;
    hits = copy.fn_hits;
    AcquireSRWLockExclusive(&s_lock);
    memcpy(hits, s_window.fn_hits, s_fn_count * sizeof(unsigned));
    memcpy(&copy, &s_window, sizeof copy);
    copy.fn_hits = hits;
    memset(s_window.fn_hits, 0, s_fn_count * sizeof(unsigned));
    hits = s_window.fn_hits;
    memset(&s_window, 0, sizeof s_window);
    s_window.fn_hits = hits;
    ReleaseSRWLockExclusive(&s_lock);
    add_into(&s_session, &copy);
    log_profile(&copy, "last 5 s");
    log_indirect_calls();
}

void guest_profile_summary(void) {
    if (!s_running) return;
    guest_profile_window();
    s_running = 0;
    log_profile(&s_session, "session");
}
