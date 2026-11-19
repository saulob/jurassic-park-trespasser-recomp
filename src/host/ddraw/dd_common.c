#include "ddraw_compat.h"
#include "host.h"

#include <stdarg.h>
#include <stdio.h>

#define FAILURE_LINE_LIMIT 200

static CRITICAL_SECTION s_lock;
static volatile LONG s_lock_state; /* 0 none, 1 being initialised, 2 ready */
static dd_method_t* s_last;
static HRESULT s_last_hr;
static uint32_t s_last_from;
static DWORD s_last_thread;
static dd_method_t* volatile s_methods;
#define METHOD_SAMPLE 8u
static int s_last_returned;
static int s_failure_lines;

/* Once, by whichever thread comes first; every later call only reads the state. */
static void init_lock(void) {
    if (InterlockedCompareExchange(&s_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&s_lock);
        InterlockedExchange(&s_lock_state, 2);
    }
    while (s_lock_state != 2) SwitchToThread();
}

void dd_lock(void) {
    if (s_lock_state != 2) init_lock();
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

static double qpc_ms_per_tick(void) {
    static double ms;
    if (ms == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        ms = 1000.0 / (double)f.QuadPart;
    }
    return ms;
}

void dd_begin(dd_method_t* m) {
    if (!m->next && s_methods != m) {
        dd_method_t* head;
        do {
            head = s_methods;
            m->next = head ? head : m;
        } while (InterlockedCompareExchangePointer((PVOID volatile*)&s_methods, m, head) != head);
    }
    if (!(m->calls % METHOD_SAMPLE)) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        m->started = t.QuadPart;
    } else {
        m->started = 0;
    }
    s_last = m;
    s_last_from = g_cur_func;
    s_last_thread = GetCurrentThreadId();
    s_last_returned = 0;
}

void dd_returned(dd_method_t* m, HRESULT hr) {
    if (m->started) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        m->sampled_ms += (double)(t.QuadPart - m->started) * qpc_ms_per_tick() * METHOD_SAMPLE;
        m->started = 0;
    }
    s_last = m;
    s_last_hr = hr;
    s_last_returned = 1;
}

HRESULT dd_done(dd_method_t* m, HRESULT hr) {
    char buf[40];
    dd_returned(m, hr);
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
        _snprintf(out, (size_t)cap - 1, "%s from sub_%08X on thread %lu -> %s (compatibility layer)", s_last->name,
                  s_last_from, s_last_thread, ddraw_hresult_text((uint32_t)s_last_hr, buf, sizeof buf));
    else
        _snprintf(out, (size_t)cap - 1, "%s from sub_%08X on thread %lu (had not returned; compatibility layer)",
                  s_last->name, s_last_from, s_last_thread);
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

void dd_methods_summary(unsigned long long hardware_frames) {
    const dd_method_t* shown[16] = { 0 };
    char line[1500] = "";
    double frames = hardware_frames ? (double)hardware_frames : 1.0;
    int n = 0;
    for (int k = 0; k < 16; k++) {
        const dd_method_t* best = NULL;
        for (const dd_method_t* m = s_methods; m; m = m->next == m ? NULL : m->next) {
            int taken = 0;
            for (int i = 0; i < k; i++) taken |= shown[i] == m;
            if (!taken && m->sampled_ms > 0.0 && (!best || m->sampled_ms > best->sampled_ms)) best = m;
        }
        if (!best) break;
        shown[k] = best;
        n += _snprintf(line + n, sizeof line - 1 - (size_t)n, "%s%s %lu calls %.2f ms per hardware frame (%.2f us each)",
                       k ? "; " : "", best->name, best->calls, best->sampled_ms / frames,
                       best->calls ? 1000.0 * best->sampled_ms / (double)best->calls : 0.0);
        if (n < 0 || (size_t)n >= sizeof line - 1) break;
    }
    line[sizeof line - 1] = 0;
    if (line[0])
        host_log("perf-detail", "D3D/DirectDraw methods by time (one call in %u timed, nested host work "
                 "included): %s", METHOD_SAMPLE, line);
}
