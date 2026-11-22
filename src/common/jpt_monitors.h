/* The monitors the Monitor setting numbers: the primary first, then the system's order. */
#ifndef JPT_MONITORS_H
#define JPT_MONITORS_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
/* The same fields over SDL's displays (src/common/jpt_monitors_sdl.c). handle is the SDL display ID. */
typedef struct {
    int left, top, right, bottom;
} RECT;
typedef void* HMONITOR;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_MAX_MONITORS 16

typedef struct {
    HMONITOR handle;
    RECT bounds, work;
    int primary;
    char device[32];
    char name[128];
} jpt_monitor_t;

/* Linux: needs the SDL video subsystem. */
int jpt_list_monitors(jpt_monitor_t* out, int cap);
/* The monitor a 1-based setting names, the primary one when it names none. 0 when no monitor is found. */
int jpt_find_monitor(int setting, jpt_monitor_t* out);

#ifdef __cplusplus
}
#endif

#endif
