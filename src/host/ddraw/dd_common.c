#include "ddraw_compat.h"
#include "host.h"

#include <stdarg.h>
#include <stdio.h>

#define FAILURE_LINE_LIMIT 200

dd_census_t dd_census = { 0 };

static CRITICAL_SECTION s_lock;
static LONG s_lock_ready;
static dd_method_t* s_last;
static HRESULT s_last_hr;
static uint32_t s_last_from;
static int s_last_returned;
static int s_failure_lines;

void dd_lock(void) {
    if (!InterlockedCompareExchange(&s_lock_ready, 1, 0)) InitializeCriticalSection(&s_lock);
    EnterCriticalSection(&s_lock);
}

void dd_unlock(void) { LeaveCriticalSection(&s_lock); }

void dd_log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    host_log("ddraw", "%s", line);
}

void dd_begin(dd_method_t* m) {
    s_last = m;
    s_last_from = g_cur_func;
    s_last_returned = 0;
}

HRESULT dd_done(dd_method_t* m, HRESULT hr) {
    char buf[40];
    s_last = m;
    s_last_hr = hr;
    s_last_returned = 1;
    if (m->calls++ == 0)
        dd_log("first call to %s -> %s", m->name, ddraw_hresult_text((uint32_t)hr, buf, sizeof buf));
    else if (FAILED(hr) && s_failure_lines < FAILURE_LINE_LIMIT) {
        dd_log("%s FAILED -> %s", m->name, ddraw_hresult_text((uint32_t)hr, buf, sizeof buf));
        if (++s_failure_lines == FAILURE_LINE_LIMIT) dd_log("failure line limit reached; later failures are not logged");
    }
    return hr;
}

HRESULT dd_unsupported(dd_method_t* m, const char* reason, HRESULT hr) {
    if (m->calls == 0) dd_log("%s is not implemented by the compatibility layer: %s", m->name, reason);
    return dd_done(m, hr);
}

int ddraw_compat_last_call(char* out, int cap) {
    char buf[40];
    if (!s_last || cap <= 0) return 0;
    if (s_last_returned)
        _snprintf(out, (size_t)cap - 1, "%s from sub_%08X -> %s (compatibility layer)", s_last->name, s_last_from,
                  ddraw_hresult_text((uint32_t)s_last_hr, buf, sizeof buf));
    else
        _snprintf(out, (size_t)cap - 1, "%s from sub_%08X (had not returned; compatibility layer)", s_last->name,
                  s_last_from);
    out[cap - 1] = 0;
    return 1;
}

void dd_format_rgb565(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800;
    pf->dwGBitMask = 0x07E0;
    pf->dwBBitMask = 0x001F;
}

int dd_format_bytes(const DDPIXELFORMAT* pf) { return (int)(pf->dwRGBBitCount + 7) / 8; }

int dd_format_supported(const DDPIXELFORMAT* pf) {
    if (pf->dwFlags & DDPF_PALETTEINDEXED8) return pf->dwRGBBitCount == 8;
    if (!(pf->dwFlags & DDPF_RGB) || (pf->dwFlags & (DDPF_FOURCC | DDPF_ZBUFFER | DDPF_PALETTEINDEXED4))) return 0;
    switch (pf->dwRGBBitCount) {
    case 16:
    case 32: return pf->dwRBitMask && pf->dwGBitMask && pf->dwBBitMask;
    default: return 0;
    }
}

int dd_format_equal(const DDPIXELFORMAT* a, const DDPIXELFORMAT* b) {
    return a->dwRGBBitCount == b->dwRGBBitCount && a->dwRBitMask == b->dwRBitMask && a->dwGBitMask == b->dwGBitMask &&
           a->dwBBitMask == b->dwBBitMask && (a->dwFlags & DDPF_PALETTEINDEXED8) == (b->dwFlags & DDPF_PALETTEINDEXED8);
}
