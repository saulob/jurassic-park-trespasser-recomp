#include "jpt_monitors.h"

#include <string.h>

#include "jpt_display.h"

typedef struct {
    jpt_monitor_t* out;
    int cap, count;
} listing_t;

static void describe(jpt_monitor_t* m) {
    DISPLAY_DEVICEA dd;
    memset(&dd, 0, sizeof dd);
    dd.cb = sizeof dd;
    strncpy(m->name, m->device, sizeof m->name - 1);
    if (EnumDisplayDevicesA(m->device, 0, &dd, 0) && dd.DeviceString[0])
        strncpy(m->name, dd.DeviceString, sizeof m->name - 1);
}

static BOOL CALLBACK collect(HMONITOR handle, HDC dc, LPRECT rect, LPARAM param) {
    listing_t* l = (listing_t*)param;
    MONITORINFOEXA mi;
    jpt_monitor_t* m;
    (void)dc;
    (void)rect;
    if (l->count == l->cap) return FALSE;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(handle, (MONITORINFO*)&mi)) return TRUE;
    m = &l->out[l->count++];
    memset(m, 0, sizeof *m);
    m->handle = handle;
    m->bounds = mi.rcMonitor;
    m->work = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    strncpy(m->device, mi.szDevice, sizeof m->device - 1);
    describe(m);
    return TRUE;
}

int jpt_list_monitors(jpt_monitor_t* out, int cap) {
    listing_t l = { out, cap, 0 };
    EnumDisplayMonitors(NULL, NULL, collect, (LPARAM)&l);
    for (int i = 1; i < l.count; i++) {
        if (!out[i].primary) continue;
        jpt_monitor_t primary = out[i];
        memmove(&out[1], &out[0], (size_t)i * sizeof *out);
        out[0] = primary;
        break;
    }
    return l.count;
}

int jpt_find_monitor(int setting, jpt_monitor_t* out) {
    jpt_monitor_t all[JPT_MAX_MONITORS];
    int count = jpt_list_monitors(all, JPT_MAX_MONITORS);
    if (count <= 0) return 0;
    *out = all[jpt_monitor_index(setting, count)];
    return 1;
}
