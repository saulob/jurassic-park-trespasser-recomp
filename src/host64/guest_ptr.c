#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "guest_ptr.h"

static volatile LONG s_leaks;
static const char* s_last_what;
static uint64_t s_last_value;
static guest_ptr_t s_last_where;

unsigned gp_leak_count(void) { return (unsigned)s_leaks; }

void gp_leak_reset(void) {
    s_leaks = 0;
    s_last_what = NULL;
}

void gp_note_leak(const char* what, uint64_t value, guest_ptr_t where) {
    InterlockedIncrement(&s_leaks);
    s_last_what = what;
    s_last_value = value;
    s_last_where = where;
}

const char* gp_last_leak(uint64_t* value, guest_ptr_t* where) {
    if (value) *value = s_last_value;
    if (where) *where = s_last_where;
    return s_last_what;
}

const char* gp_string(guest_ptr_t va, uint32_t max) {
    uint32_t i = 0;
    const char* s = (const char*)gm_ptr_checked(va, 1, 0);
    if (!s) return NULL;
    while (i < max) {
        uint32_t page_left = GM_PAGE - ((va + i) & (GM_PAGE - 1));
        uint32_t k;
        if (!gm_ptr_checked(va + i, 1, 0)) return NULL;
        for (k = 0; k < page_left && i < max; k++, i++)
            if (!s[i]) return s;
    }
    return NULL;
}
