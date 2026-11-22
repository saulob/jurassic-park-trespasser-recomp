#ifndef TRESPASSER_HOST_H
#define TRESPASSER_HOST_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "ce_gameplay.h"
#include "debug_menu.h"
#include "engine_mode.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "host_input.h"
#include "lift_registry.h"

int host_log_open(const char* dir, const char* file);
const char* host_log_path(void);
void host_log_raw(const char* text, unsigned len);
/* --perf-log: the 5-second performance lines, periodic renderer summaries and the guest CPU profile. */
void host_perf_logging_enable(void);


void guest_env_init(const char* game_dir, const char* guest_args);
const char* guest_env_exe_path(void);
int guest_env_apply_command_line(void);
void shim_GetModuleHandleA(void);
void shim_GetModuleFileNameA(void);
void shim_MessageBoxA(void);
void shim_DialogBoxParamA(void);
void shim_OutputDebugStringA(void);

/* The store lives in user_dir. Installed Directory (saves, level swap files) is user_dir, Data Drive is game_dir. */
void guest_registry_init(const char* game_dir, const char* user_dir);
void registry_show_ddraw_cert_dialog(void);
void registry_software_water(void);
/* While on, the game's "NoVideo" reads 1: CVideoWnd::Play returns at once, as it does with the value stored. */
void registry_skip_video(int on);
void shim_RegOpenKeyExA(void);
void shim_RegCreateKeyA(void);
void shim_RegQueryValueExA(void);
void shim_RegSetValueExA(void);
void shim_RegDeleteValueA(void);
void shim_RegCloseKey(void);

int io_trace_init(void);
void io_trace_summary(void);
void io_trace_user_dir(const char* dir);
void io_trace_release_swap_files(void);
void shim_CreateFileA(void);
void shim_CloseHandle(void);
void shim_GetFileAttributesA(void);
void shim_FindFirstFileA(void);
void shim_LZOpenFileA(void);
void shim_LZRead(void);
void shim_LZSeek(void);
void shim_LZClose(void);
void shim_LoadLibraryA(void);
void shim_GetProcAddress(void);
void shim_FreeLibrary(void);
void shim_fopen(void);
void shim_open(void);
void shim_DeleteFileA(void);

void milestones_init(void);
void milestones_observe(uint32_t target);

void ddraw_trace_init(void);
recomp_func_t ddraw_trace_lookup(uint32_t va);
/* Grows when the observer starts watching more methods. */
unsigned ddraw_trace_entry_count(void);
int ddraw_trace_name(uint32_t va, char* out, int cap);
int ddraw_trace_last_call(char* out, int cap);
const char* ddraw_hresult_text(uint32_t hr, char* buf, size_t cap);

int ddraw_compat_init(int enabled);
int ddraw_compat_active(void);
recomp_func_t ddraw_compat_lookup(uint32_t va);
/* A compatibility-layer method the dispatch may run in place, keeping the machine lock (d3d_objects.c). */
int d3d_method_in_place(uint32_t va);
int ddraw_compat_last_call(char* out, int cap);

int selfmod_init(void);
recomp_func_t selfmod_lookup(uint32_t va);
/* While set, every SelfMod entry goes to the host function the interceptor returns instead of the native code. */
typedef recomp_func_t (*selfmod_interceptor_t)(uint32_t va);
void selfmod_intercept(selfmod_interceptor_t interceptor);
unsigned long selfmod_entries(void);
double selfmod_total_ms(void);
#define GUEST_PROFILE_MAX_SLOTS 64
unsigned guest_profile_slots(void);
const char* guest_profile_slot_name(unsigned slot);
void guest_profile_snapshot(unsigned* out);
int guest_profile_running(void);
/* The slots of the physics code's stages, and the time one sample stands for. */
int guest_profile_slot_physics(unsigned slot);
double guest_profile_sample_ms(void);
void selfmod_report(double frames, double frame_ms);

void diagnostics_install(unsigned watchdog_seconds);
/* Reports the machine state and ends the run with HOST_EXIT_SHUTDOWN_TIMEOUT. */
void diagnostics_shutdown_timeout(const char* detail);
/* After the exit code is logged: other DLLs' first-chance exceptions while unloading are theirs. */
void diagnostics_process_ending(void);

/* window_host.c: the game window, its presentation surface, the cursor and closing. */
void window_host_init(void);
/* trespasser.ini's Display settings; F11 writes display_mode back to settings_path. */
void window_host_configure(int borderless, int monitor, int window_w, int window_h, int stretch, int show_fps,
                           int show_resolution,
                           const char* settings_path);
int window_host_monitor_size(int monitor, int* width, int* height);
/* The game's screen size; the presentation follows it when a frame of this generation is about to be shown. */
void window_host_set_display(HWND window, int width, int height, unsigned generation);
void window_host_frame_commit(unsigned generation);
void window_host_clear_display(HWND window);
HWND window_host_surface(HWND window);
void window_host_frame_presented(void);
void window_host_release_cursor(void);
/* The host's menu opened or closed: the pointer is freed for it, or captured again for the game. */
void window_host_input_changed(void);
/* A level load ended: the game gets the activation change it was kept from during the load. */
void window_host_level_load_done(void);
void window_host_view_size(int* width, int* height);
/* While on, the game's cursor and mouse messages are in its 640x480 UI canvas, centred in its screen (ui_canvas.c). */
void window_host_set_ui_canvas(int on);
/* The game's current screen (its virtual display mode); 0x0 when none is set. */
void window_host_display_size(int* width, int* height);
/* The resolution the title names for each renderer, whatever the game's screen (menus and videos are 640x480). */
void window_host_set_title_sizes(int hardware_w, int hardware_h, int software_w, int software_h);
void window_host_input_summary(void);
const char* window_host_close_origin(void);
/* Presentation stops once the host is closing the game; the game's frames still run. */
int window_host_frames_allowed(void);
/* exit_policy.c drives the last phases: the CRT's exit, its atexit handlers done, the host's cleanup. */
void window_host_crt_exiting(void);
void window_host_guest_stopped(void);
void window_host_begin_cleanup(void);
void window_host_exited(void);
void present_release(void);
void shim_CreateWindowExA(void);
void shim_DestroyWindow(void);
void shim_PostQuitMessage(void);
void shim_SetWindowLongA(void);
void shim_SetWindowPos(void);
void shim_MoveWindow(void);
void shim_SetWindowTextA(void);
void shim_ShowWindow(void);
void shim_GetWindowRect(void);
void shim_GetClientRect(void);
void shim_ClientToScreen(void);
void shim_GetCursorPos(void);
void shim_GetDiskFreeSpaceA(void);
void shim_SetCursorPos(void);
void shim_ClipCursor(void);
void shim_ShowCursor(void);
void shim_SetCapture(void);
void shim_ReleaseCapture(void);
void shim_EnableMenuItem(void);
void shim_PeekMessageA(void);
void shim_IsIconic(void);
void shim_GetForegroundWindow(void);

/* guest_ui.c: the game's own window stack (guest_abi.h declares the probe). */
guest_wnd_t guest_ui_window_kind(uint32_t window);
int guest_display_built(void);

/* d3d11_renderer.c: --renderer, --render-resolution, --texture-filter, --msaa */
/* box 0x0: the original compatibility size; otherwise each target is the largest of its surface's shape inside it. */
void d3dr_configure(int hardware, unsigned box_w, unsigned box_h, int filter, unsigned anisotropy, unsigned msaa);
/* d3d11_present.c: Off never waits for the vertical blank; On keeps the game's own Flip request. */
void present_set_vsync(int on);
/* An adapter label (jpt_gpu.h); empty or not found: the first usable adapter. */
void present_set_graphics_card(const char* name);
int d3dr_startup(void);
/* The renderer d3dr_startup chose; it stays for the whole run. */
int d3dr_hardware(void);
/* The 3D view the game last drew a scene into, as fractions of the render target (x, y, w, h); 0 before any scene. */
int d3dr_scene_view(float out[4]);
const char* d3dr_name(void);
/* One polygon the game rasterises itself into the surface, inside this box of its raster, whose first pixel is
 * origin_bytes into the surface; about to Lock it or already holding the Lock; reads: its primitive is not known to
 * only store pixels. */
void d3dr_cpu_polygon(void* surface_iface, const long box[4], uint32_t origin_bytes, int reads);
void d3dr_cpu_polygon_done(void);
/* A perspective-textured polygon drawn by the GPU in place of the software primitive, in the surface's pixels; 0 when
 * the target cannot take it (the caller then draws it on the CPU). */
typedef struct {
    float x[64], y[64]; /* the raster's view pixels: origin_bytes from the surface's first pixel */
    unsigned count;
    uint32_t origin_bytes;
    float plane[4][3]; /* u/z, v/z, 1/z: value at origin, per pixel x, per row; plane[3] = origin x, y */
    int clamped;
    float umin, umax, vmax;
    uint32_t line, umask, tile_mask;
    const uint8_t* texels;
    uint32_t texel_bytes;
    const uint16_t* clut;
    uint32_t clut_entries;
    uint32_t kind; /* GPU_POLYGON_TEXTURE: 8-bit texels in the CLUT; GPU_POLYGON_BUMP: 16-bit texels, bump table */
    const uint8_t* bump_table;
    uint32_t light_theta;
} gpu_texture_polygon_t;
#define GPU_POLYGON_TEXTURE 0u
#define GPU_POLYGON_BUMP 1u
typedef enum { GPU_POLYGON_READY, GPU_POLYGON_NO_TARGET, GPU_POLYGON_BUSY, GPU_POLYGON_NO_RESOURCES } gpu_polygon_status_t;
/* Before the polygon's spans run: the target, the resources, the uploads and the order against CPU polygons. */
gpu_polygon_status_t d3dr_texture_polygon_prepare(void* surface_iface, const gpu_texture_polygon_t* p);
/* Draws a polygon d3dr_texture_polygon_prepare accepted. */
int d3dr_texture_polygon(void* surface_iface, const gpu_texture_polygon_t* p);
/* A level change: guest addresses may now hold other textures and CLUTs. */
void d3dr_texture_polygon_reset(void);
/* After a level load: creates the GPU polygon buffers on the current device. */
void d3dr_texture_polygon_warm(void);
/* The guest function behind the current raster Lock. */
uint32_t d3dr_lock_caller(void);
/* cpu_polygons.c: the software polygons drawn into the Direct3D frame (meshes Direct3D cannot take). */
void cpu_polygons_install(void);
/* DrawPolygon's cost by screen Lock caller, at exit. */
void cpu_polygons_summary(void);
/* Closes the frame's per-family counts; gameplay = a stable gameplay frame, slow = over 25 ms. */
void cpu_polygons_frame_end(int gameplay, int slow);
/* Ambient occlusion (JPT_AO_*), from trespasser.ini before d3dr_startup. */
void d3dr_configure_ambient_occlusion(int level);
/* Water refraction and Enhanced sky, from trespasser.ini. */
void d3dr_configure_effects(int water_refraction, int enhanced_sky);
/* sky: camera u, camera v, plane height above the camera (texture units), fog low, fog high. */
void d3dr_sky_begin(const float sky[5]);
void d3dr_sky_end(void);
/* --debug-d3d-output texture|vertex-color|depth: what the pixel shader returns for every draw. */
int d3dr_set_debug_output(const char* mode);
void d3dr_summary(void);
/* guest_overrides.c: the lifted functions of guest_override_list.h (guest_abi.h declares the installs). */
/* Host kernels byte-exact with the lifted functions they replace (tests/water_tests.c). */
void guest_kernels_install(void);
/* widescreen.c: lifts the mode table's width cap and makes the main camera Hor+; 0 when a site is missing. */
int widescreen_install_modes(void);
/* game_text.c: the gameplay text overlay keeps its share of the screen height and is drawn aliased. */
void game_text_install(void);
int game_text_drawing(void);
/* The overlay's TextOutA calls reach shim_TextOutA, so each string's box is known before it is drawn. */
int game_text_boxes_known(void);
void shim_TextOutA(void);
/* l10n.c */
void shim_LoadStringA(void);
void shim_CreateFontA(void);
void shim_CreateFontIndirectA(void);
/* The device box a TextOutA under the text overlay's DC is about to draw into (NULL: anywhere on the surface). */
void d3dr_text_box(HDC dc, const RECT* box);
/* The game's text of the frame being presented, kept as its own picture; 0 when there is none. The texture is valid until
 * the frame has been presented. */
int present_take_text_layer(void** texture, int* legacy_w, int* legacy_h, int* left, int* top, int* right, int* bottom);
/* ui_canvas.c: the game's 640x480 menus and dialogs on a larger game screen draw into an offscreen canvas the
 * renderer scales over the frame. */
void ui_canvas_install(void);
void menu_audio_install(void);
/* engine_limits.c */
void engine_limits_install(int hardware);
void engine_limits_log(int hardware, const char* filter, unsigned msaa, int vsync);
void engine_limits_summary(unsigned long long hardware_draw_calls);
/* texture_streaming.c: the game's per-frame Direct3D texture upload window. */
void texture_streaming_install(void);
void texture_streaming_log(void);
void texture_streaming_summary(void);
/* Image-cache decisions at exit (engine_limits.c). */
void engine_limits_cache_summary(void);
void shim_GetTickCount(void);
/* engine_heaps.c: the engine's fixed heaps (scene polygons and vertices, terrain) and their commits. */
void engine_heaps_install(void);
void engine_heaps_log(void);
/* Inside a RasterBlt from another raster onto the game's screen. */
int ui_screen_blt_writes_only(void);
/* A present now would show a blank screen between two menu frames (ui_present_held): the last frame stays up. */
int ui_canvas_present_held(void);
/* The gameplay picture the game froze behind its pause menus (RGB565), as its own save thumbnail is taken from. */
int ui_frozen_picture(const uint16_t** bits, int* w, int* h, long* pitch);
/* io_trace.c: the game's Installed Directory, where its saves are. */
const char* io_trace_user_folder(void);
void d3dr_set_ui_layer(const uint16_t* bits, long pitch, int guest_w, int guest_h);
/* The HD layer (premultiplied BGRA of the game's screen size; NULL: none this frame) and the changed rectangle. */
void d3dr_set_frontend(const uint32_t* pixels, int w, int h, const RECT* dirty);
/* The pointer sprite (size x size, premultiplied) with its corner at x, y of the game's screen; NULL hides it. */
void d3dr_set_frontend_pointer(const uint32_t* sprite, int size, unsigned generation, int x, int y);
int d3dr_frontend_failed(void);
void d3dr_frontend_release(void);
/* The frame Direct3D 11 presents for a game screen of guest_w x guest_h: the size an HD layer should have. */
int d3dr_frame_size(int guest_w, int guest_h, int* w, int* h);
/* --render-resolution's box; 0x0 when there is none. */
void d3dr_render_box(unsigned* w, unsigned* h);
/* video_hd.c: the game's Smacker videos (CVideoWnd::Play) presented in HD on a surface of their own. */
void video_hd_install(void);
/* A present of the game's screen; 1 when the video surface showed it. gamma: DDGAMMARAMP. */
int video_hd_present(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma);
/* After each Present of the game's own surface. */
void video_hd_main_presented(void);
void video_hd_release(void);
/* The next Play only (vl_play_mode_t); it is AUTO again from the one after. */
void video_hd_force(int mode);
/* Inside Play: the Smacker frame on screen and the picture's rectangle on the game's screen. */
int video_hd_picture(int32_t* frame, int* x, int* y, int* w, int* h);
/* video_subtitles.c: the dialogue of the retail videos drawn over the picture (key video_subtitles). */
typedef struct {
    const uint32_t* pixels; /* premultiplied BGRA, w x h */
    int x, y, w, h;         /* on the target */
    unsigned generation;    /* changes with the pixels */
} video_subtitle_image_t;
void video_subtitles_configure(int enabled, const char* game_dir);
void video_subtitles_begin(const char* guest_name);
void video_subtitles_end(void);
/* The subtitle on screen for a target_w x target_h target whose picture is at px, py (pw x ph); NULL for none. */
const video_subtitle_image_t* video_subtitles_image(int target_w, int target_h, int px, int py, int pw, int ph);
/* The same for the game's screen as presented (screen_w x screen_h scaled to target_w x target_h). */
const video_subtitle_image_t* video_subtitles_for_screen(int screen_w, int screen_h, int target_w, int target_h);
struct ID3D11RenderTargetView;
/* Hardware renderer: the image drawn over a render target. */
void d3dr_draw_video_subtitle(struct ID3D11RenderTargetView* target, int width, int height,
                              const video_subtitle_image_t* image);
/* video_test.c: the Video Test panel, with the main menu alone in front. No key opens it. */
void video_test_f5(void);
int video_test_visible(void);
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
/* Before each Present of the game's own surface: the panel over its back buffer. */
void video_test_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context,
                        struct ID3D11RenderTargetView* target, int width, int height);
/* A mouse message of the game window while the panel shows: 1 when the game must not get it. */
int video_test_mouse(UINT msg, WPARAM wparam, LPARAM lparam);
/* In the menu frame, before the game draws it: a requested playback runs here. */
void video_test_ui_frame(void);
void video_test_release(void);
/* debug_menu.h: F10 Cheats in a level or with the main menu alone in front, and F3 Objectives in a level. */
/* overlay.c: the one owner of the host's interfaces over gameplay (overlay_policy.h kinds). */


/* minimap.c: the minimap's world bounds and projection; key minimap (default on). */
void minimap_configure(int enabled);
/* F5 and L3 + R3: flips the minimap in gameplay, saves it to trespasser.ini and says so on the HUD. */
void window_host_toggle_minimap(const char* source);
/* The frame counter over the game: the launcher's choice at start, F6 for the session. The text is "FPS: n" from the
 * rate the window title shows; 0 until one is measured or while the counter is off. */
void window_host_set_frame_counter(int on);
void window_host_toggle_frame_counter(void);
int window_host_frame_counter_on(void);
int window_host_frame_counter_text(char* out, size_t cap);

/* ce_gameplay.c: Quick Save / Quick Load, Dual stow and Maximum object detail. Requests run in the game's UI loop. */
void ce_gameplay_ui_loop(void);
void quick_save_request(void);
void quick_load_request(void);
/* Frames the Direct3D 11 renderer presented with the hardware device. */
unsigned long long renderer_hardware_frames(void);
/* ce_graphics.c: the sky draw hook and the water rows' observer (Direct3D 11 only). */
void ce_graphics_install(int hardware);
/* Every key message first: 1 when the game must not see it. */
int overlay_key(UINT msg, WPARAM wparam, LPARAM lparam);
/* gamepad.c: an interface opened or closed: buttons held now wait for their release. */
void gamepad_overlay_changed(void);
/* 1 when the game must not see this key message. */
int debug_menu_key(UINT msg, WPARAM wparam, LPARAM lparam);
/* A mouse message over the open menu, x and y in the presentation surface's pixels: 1 when the menu took it. */
int debug_menu_mouse(UINT msg, int x, int y);
void debug_menu_windows_init(void);
int debug_menu_overlay_is_wanted(void);
void debug_menu_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context,
                        struct ID3D11RenderTargetView* target, int width, int height);
#ifndef OVERLAY_TILE_SHIFT
#define OVERLAY_TILE_SHIFT 5
#endif
typedef struct {
    uint8_t* mask;
    int tiles_w, tiles_h;
} overlay_tiles_t;
int debug_menu_overlay_rgba(uint8_t* rgba, int width, int height, overlay_tiles_t* tiles);
struct dmp_gpu_draw_s;
int debug_menu_overlay_gpu(struct dmp_gpu_draw_s* out, int width, int height);
void debug_menu_release(void);
HWND window_host_game_window(void);
/* window_host.c: the presentation surface inside the game window's client. */
int window_host_view_rect(RECT* view);
/* The real pointer in the game's screen pixels; 0 when it is outside the picture. */
int window_host_pointer(int* x, int* y);
/* Gamepad input reaches the game as its own key, character and mouse messages (gamepad.c, gamepad_menu.c). */
enum { PAD_MOUSE_DOWN = HOST_MOUSE_DOWN, PAD_MOUSE_UP = HOST_MOUSE_UP, PAD_MOUSE_DOUBLE = HOST_MOUSE_DOUBLE, PAD_MOUSE_MOVE = HOST_MOUSE_MOVE };
int window_host_input_focused(void);
void window_host_post_key(unsigned vk);
void window_host_post_key_up(unsigned vk);
void window_host_post_char(char c);
/* A left-button message at the game's pointer. */
void window_host_post_mouse(int kind);
/* The game's pointer in menus, in its own coordinates, placed by the gamepad until the mouse is used. */
void window_host_set_pad_cursor(int x, int y);
void window_host_release_pad_cursor(void);
/* Mouse counts from the right stick, into the same cursor the mouse moves in gameplay (the device stays the
 * pad). */
void window_host_pad_relative(int dx, int dy);
/* Mouse sensitivity for the mouse's own motion in gameplay (both axes; the gamepad keeps its own). */
void window_host_set_mouse_sensitivity(float value);
/* platform_pump: SDL's mouse motion, its timestamp in SDL nanoseconds. */
void window_host_mouse_motion(float dx, float dy, uint64_t when_ns);
/* Before SDL stops: the game's window stops being an SDL window and keeps the host's procedure. */
void window_host_detach_platform(void);
int window_host_game_cursor(int* x, int* y);
/* surface: the game's IDirectDrawSurface of its screen. 1 when drawn; 0 leaves the copy to the game. */
int d3dr_draw_still_picture(void* surface, const uint16_t* bits, long pitch, int w, int h);
/* dd_device.c: the gameplay modes of both renderers, listed whatever the desktop size. */
void dd_set_gameplay_modes(DWORD hardware_w, DWORD hardware_h, DWORD software_w, DWORD software_h);
/* registry.c: "Width"/"Height" answer the current renderer's gameplay mode whatever is stored. */
void registry_gameplay_sizes(int hardware_w, int hardware_h, int software_w, int software_h);

/* exit_policy.c */
int host_exit_code_for_guest_return(int code);
void shim_exit(void);
void shim__exit(void);
void shim_ExitProcess(void);
void shim_CreateThread(void);
/* Guest threads (CreateThread from lifted code) still running, the main guest thread included. */
unsigned guest_threads_alive(void);
/* diagnostics.c: lifted indirect calls answered from the dispatch cache, and the ones that searched the table. */
void dispatch_cache_counts(unsigned long long* hits, unsigned long long* misses);
/* guest_profile.c: sampled guest CPU time, logged with the 5-second performance lines (--perf-log) and at exit. */
void guest_profile_start(HANDLE guest_thread);
void guest_profile_add_worker(HANDLE thread, uint32_t entry);
void guest_profile_window(void);
void guest_profile_summary(void);
/* "[shutdown] guest threads <when>: N" and one line per worker still running. */
void guest_threads_log(const char* when);

/* registry.c / guest_env.c: the legacy "unstable last time, Safe Mode?" prompt. */
void registry_legacy_safemode_dialog(void);
int registry_legacy_safemode_enabled(void);


enum {
    HOST_EXIT_SETUP = 1,
    HOST_EXIT_NOT_LIFTED = 2,
    HOST_EXIT_FAULT = 3,
    HOST_EXIT_WATCHDOG = 4,
    HOST_EXIT_UNRESOLVED = 5,
    HOST_EXIT_GAME_FAILED = 6,
    HOST_EXIT_GUEST_ERROR = 7,
    HOST_EXIT_SHUTDOWN_TIMEOUT = 8
};

#endif
