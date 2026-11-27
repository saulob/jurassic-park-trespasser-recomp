/* What the HD front end and the HD loading screen need from the host that is not the game: where their layer goes, the host pointer,
 * the clock and files. Windows hands the layer to the Direct3D 11 renderer (inline here); Linux composes it over the guest frame
 * (src/linux/linux_frontend.c). The painter, layouts, scenes and bridges are shared. */
#ifndef JPT_FRONTEND_PLATFORM_H
#define JPT_FRONTEND_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "frontend_layout.h"
#include "guest_abi.h"

#ifndef _WIN32
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
/* The game's text-drawing flags its windows carry (DrawText's). */
enum { DT_CENTER = 1, DT_RIGHT = 2, DT_VCENTER = 4, DT_BOTTOM = 8, DT_WORDBREAK = 16 };

/* Presenting: the layer (premultiplied BGRA, layer_w x layer_h) over the guest screen, with the part that changed; NULL clears it. */
void fe_host_layer(const uint32_t* pixels, int w, int h, const fe_rect_t* dirty);
/* The host pointer's sprite in layer pixels; a NULL sprite hides it. */
void fe_host_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y);
/* The layer cannot be shown at all (the renderer failed). */
int fe_host_layer_failed(void);
/* The layer's memory on the presenting side is released. */
void fe_host_layer_release(void);
/* The size of the frame the layer is shown in for a guest screen; 0 when it has none yet. */
int fe_host_frame_size(int guest_w, int guest_h, int* w, int* h);
/* The size the HD loading screen is painted at over a guest display of guest_w x guest_h: the display itself on Windows (the
 * renderer scales it), the picture's rectangle in the window on Linux. */
int fe_host_loading_size(int guest_w, int guest_h, int* w, int* h);
/* 1 when HD layers are shown at all (Windows: the hardware renderer; Linux: always). */
int fe_host_layer_available(void);
/* The host pointer in guest pixels; 0 when it is outside the game's screen. */
int fe_host_pointer_position(int* x, int* y);
/* 1 while gamepad focus drives the menus (every pointer is hidden). */
int fe_host_pad_focus(void);
/* The picture the game froze at the start of a save (RGB565 rows); 0 when there is none. */
int fe_host_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch);
/* A monotonic clock in milliseconds. */
double fe_host_ms(void);
uint32_t fe_host_tick(void);
int fe_host_cpu_count(void);
/* A file's last write and size; 0 when it does not exist. */
int fe_host_file_stamp(const char* path, uint64_t* stamp, uint64_t* size);
/* The game's user data folder (where saves are). */
const char* fe_host_user_folder(void);
/* The folder of the executable, with its trailing separator; 0 when unknown. */
int fe_host_exe_dir(char* out, size_t cap);
int fe_host_file_exists(const char* path);
/* The host's path of a scene or save the game names, from a package when it has it. */
int fe_host_level_file(const char* path, char* out, size_t cap);
/* A line over the game for a moment. */
void fe_host_notice(const char* text);
/* A level load: its start, the loaded level before the game counts the load as over, and after. */
void fe_host_level_load_begin(void);
void fe_host_level_loaded(void);
void fe_host_level_load_done(void);
/* The game's display is built (its screen raster has a size). */
int fe_host_display_built(void);
#endif

#ifdef _WIN32
#include "gamepad.h"
#include "host.h"

static inline void fe_host_layer(const uint32_t* pixels, int w, int h, const fe_rect_t* dirty) {
    RECT box;
    if (dirty) {
        box.left = dirty->left;
        box.top = dirty->top;
        box.right = dirty->right;
        box.bottom = dirty->bottom;
    }
    d3dr_set_frontend(pixels, w, h, dirty ? &box : NULL);
}
static inline void fe_host_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y) {
    d3dr_set_frontend_pointer(sprite, size, generation, x, y);
}
static inline int fe_host_layer_failed(void) { return d3dr_frontend_failed(); }
static inline void fe_host_layer_release(void) { d3dr_frontend_release(); }
static inline int fe_host_frame_size(int guest_w, int guest_h, int* w, int* h) { return d3dr_frame_size(guest_w, guest_h, w, h); }
static inline int fe_host_loading_size(int guest_w, int guest_h, int* w, int* h) {
    *w = guest_w;
    *h = guest_h;
    return 1;
}
static inline int fe_host_layer_available(void) { return d3dr_hardware(); }
static inline int fe_host_pointer_position(int* x, int* y) { return window_host_pointer(x, y); }
static inline int fe_host_pad_focus(void) { return gamepad_menu_focus(); }
static inline int fe_host_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch) { return ui_frozen_picture(bits, w, h, pitch); }
static inline double fe_host_ms(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static inline uint32_t fe_host_tick(void) { return GetTickCount(); }
static inline int fe_host_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
static inline int fe_host_file_stamp(const char* path, uint64_t* stamp, uint64_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    *stamp = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
    *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return 1;
}
static inline const char* fe_host_user_folder(void) { return io_trace_user_folder(); }
static inline void fe_host_level_load_begin(void) {
    d3dr_texture_polygon_reset();
}
static inline void fe_host_level_loaded(void) {
    d3dr_texture_polygon_reset();
    d3dr_texture_polygon_warm();
}
static inline void fe_host_level_load_done(void) { window_host_level_load_done(); }
static inline int fe_host_exe_dir(char* out, size_t cap) {
    char* slash;
    if (!GetModuleFileNameA(NULL, out, (DWORD)cap) || !(slash = strrchr(out, '\\'))) return 0;
    slash[1] = 0;
    return 1;
}
static inline int fe_host_file_exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static inline int fe_host_display_built(void) { return guest_display_built(); }
static inline void fe_host_notice(const char* text) { debug_menu_notice(text); }
static inline int fe_host_level_file(const char* path, char* out, size_t cap) {
    return custom_level_file(path, out, cap) || (strlen(path) < cap && strcpy(out, path) != NULL);
}
#endif

#endif
