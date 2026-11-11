#include "page_guard.h"

#include <windows.h>

#define PAGE_MASK ((uintptr_t)0xFFF)

static struct {
    volatile int armed;
    uintptr_t lo, hi;
    page_guard_touched_t touched;
    void* context;
    void* handler;
} G;

static void open_pages(void) {
    DWORD old;
    VirtualProtect((void*)G.lo, G.hi - G.lo, PAGE_READWRITE, &old);
}

void page_guard_touch(void) {
    if (!G.armed) return;
    G.armed = 0;
    open_pages();
    G.touched(G.context);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    uintptr_t at;
    if (!G.armed || r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] == 8)
        return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)r->ExceptionInformation[1];
    if (at < G.lo || at >= G.hi) return EXCEPTION_CONTINUE_SEARCH;
    page_guard_touch();
    return EXCEPTION_CONTINUE_EXECUTION;
}

page_guard_result_t page_guard_arm(void* bits, size_t bytes, page_guard_touched_t touched, void* context) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t lo = (uintptr_t)bits;
    DWORD old;
    if (!lo || (lo & PAGE_MASK) || !VirtualQuery(bits, &mbi, sizeof mbi) || mbi.AllocationBase != bits)
        return PAGE_GUARD_SHARED_PAGES;
    if (!G.handler) G.handler = AddVectoredExceptionHandler(1, on_fault);
    if (!G.handler) return PAGE_GUARD_NO_HANDLER;
    page_guard_touch();
    G.lo = lo;
    G.hi = (lo + bytes + PAGE_MASK) & ~PAGE_MASK;
    G.touched = touched;
    G.context = context;
    if (!VirtualProtect(bits, G.hi - G.lo, PAGE_NOACCESS, &old)) return PAGE_GUARD_PROTECT_FAILED;
    G.armed = 1;
    return PAGE_GUARD_ARMED;
}

int page_guard_disarm(void) {
    if (!G.armed) return 0;
    G.armed = 0;
    open_pages();
    return 1;
}

int page_guard_armed(void) { return G.armed; }

const char* page_guard_result_name(page_guard_result_t result) {
    switch (result) {
    case PAGE_GUARD_ARMED: return "deferred to the first touch";
    case PAGE_GUARD_SHARED_PAGES: return "immediate, the pages are shared";
    case PAGE_GUARD_PROTECT_FAILED: return "immediate, the pages cannot be protected";
    default: return "immediate, no fault handler";
    }
}
