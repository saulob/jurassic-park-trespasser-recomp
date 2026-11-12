#include "page_guard.h"

#include <windows.h>

#define PAGE_MASK ((uintptr_t)0xFFF)
#define MAX_ALIASES 8

typedef struct {
    uintptr_t lo, hi;
    volatile int open;
} alias_t;

static struct {
    volatile int armed;
    uintptr_t lo, hi;
    int alias;
    page_guard_touched_t touched;
    void* context;
    void* handler;
} G;

static alias_t s_aliases[MAX_ALIASES];

static void open_pages(void) {
    DWORD old;
    VirtualProtect((void*)G.lo, G.hi - G.lo, PAGE_READWRITE, &old);
}

static int set_alias_open(alias_t* a, int open) {
    DWORD old;
    if (a->open == open) return 1;
    if (!VirtualProtect((void*)a->lo, a->hi - a->lo, open ? PAGE_READWRITE : PAGE_NOACCESS, &old)) return 0;
    a->open = open;
    return 1;
}

void page_guard_touch(void) {
    if (!G.armed) return;
    G.armed = 0;
    if (G.alias) set_alias_open(&s_aliases[G.alias - 1], 1);
    else open_pages();
    G.touched(G.context);
}

static alias_t* closed_alias_at(uintptr_t at) {
    for (int i = 0; i < MAX_ALIASES; i++)
        if (s_aliases[i].lo && !s_aliases[i].open && at >= s_aliases[i].lo && at < s_aliases[i].hi) return &s_aliases[i];
    return NULL;
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    uintptr_t at;
    alias_t* a;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] == 8)
        return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)r->ExceptionInformation[1];
    if (G.armed && at >= G.lo && at < G.hi) {
        page_guard_touch();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if ((a = closed_alias_at(at)) != NULL && set_alias_open(a, 1)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

static int ensure_handler(void) {
    if (!G.handler) G.handler = AddVectoredExceptionHandler(1, on_fault);
    return G.handler != NULL;
}

page_guard_result_t page_guard_arm(void* bits, size_t bytes, page_guard_touched_t touched, void* context) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t lo = (uintptr_t)bits;
    DWORD old;
    if (!lo || (lo & PAGE_MASK) || !VirtualQuery(bits, &mbi, sizeof mbi) || mbi.AllocationBase != bits)
        return PAGE_GUARD_SHARED_PAGES;
    if (!ensure_handler()) return PAGE_GUARD_NO_HANDLER;
    page_guard_touch();
    G.lo = lo;
    G.hi = (lo + bytes + PAGE_MASK) & ~PAGE_MASK;
    G.alias = 0;
    G.touched = touched;
    G.context = context;
    if (!VirtualProtect(bits, G.hi - G.lo, PAGE_NOACCESS, &old)) return PAGE_GUARD_PROTECT_FAILED;
    G.armed = 1;
    return PAGE_GUARD_ARMED;
}

int page_guard_disarm(void) {
    if (!G.armed) return 0;
    G.armed = 0;
    if (!G.alias) open_pages();
    return 1;
}

int page_guard_armed(void) { return G.armed; }

int page_alias_create(void* section, size_t bytes) {
    DWORD old;
    void* view;
    int slot = -1;
    for (int i = 0; i < MAX_ALIASES && slot < 0; i++)
        if (!s_aliases[i].lo) slot = i;
    if (slot < 0 || !section || !ensure_handler()) return 0;
    view = MapViewOfFile((HANDLE)section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) return 0;
    if (!VirtualProtect(view, bytes, PAGE_NOACCESS, &old)) {
        UnmapViewOfFile(view);
        return 0;
    }
    s_aliases[slot].open = 0;
    s_aliases[slot].hi = ((uintptr_t)view + bytes + PAGE_MASK) & ~PAGE_MASK;
    s_aliases[slot].lo = (uintptr_t)view;
    return slot + 1;
}

void page_alias_destroy(int alias) {
    alias_t* a;
    if (alias <= 0 || alias > MAX_ALIASES) return;
    a = &s_aliases[alias - 1];
    if (G.armed && G.alias == alias) G.armed = 0;
    if (a->lo) UnmapViewOfFile((void*)a->lo);
    a->lo = a->hi = 0;
    a->open = 0;
}

unsigned char* page_alias_view(int alias) {
    return alias > 0 && alias <= MAX_ALIASES ? (unsigned char*)s_aliases[alias - 1].lo : NULL;
}

page_guard_result_t page_guard_arm_alias(int alias, page_guard_touched_t touched, void* context) {
    alias_t* a;
    if (alias <= 0 || alias > MAX_ALIASES || !s_aliases[alias - 1].lo) return PAGE_GUARD_SHARED_PAGES;
    a = &s_aliases[alias - 1];
    page_guard_touch();
    if (!set_alias_open(a, 0)) return PAGE_GUARD_PROTECT_FAILED;
    G.lo = a->lo;
    G.hi = a->hi;
    G.alias = alias;
    G.touched = touched;
    G.context = context;
    G.armed = 1;
    return PAGE_GUARD_ARMED;
}
