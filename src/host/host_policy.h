#ifndef TRESPASSER_HOST_POLICY_H
#define TRESPASSER_HOST_POLICY_H

/* Window-system-free decisions of the host, kept apart so tests can run them headless. */

#include "guest_va.h"
#include <stdint.h>

typedef struct {
    int64_t frequency;
    int64_t window_start;
    int64_t last_present;
    uint32_t frames;
    uint64_t total_frames;
    double fps;
} fps_counter_t;

void fps_counter_init(fps_counter_t* c, int64_t frequency, int64_t now);
/* Returns 1 when a new rate is available (about once per second of presented frames). */
int fps_counter_present(fps_counter_t* c, int64_t now);
/* Returns 1 when presentation has stalled long enough that the shown rate must drop to 0. */
int fps_counter_idle(fps_counter_t* c, int64_t now);

typedef enum {
    INPUT_MENU,
    INPUT_GAMEPLAY_CAPTURED,
    INPUT_GAMEPLAY_RELEASED,
    INPUT_DIALOG,
    INPUT_MOVING_WINDOW,
    INPUT_FULLSCREEN_TRANSITION,
    INPUT_SHUTDOWN,
    INPUT_STATE_COUNT
} input_state_t;

/* What the game itself shows, read from its window stack. */
typedef enum { GUEST_UI_NONE, GUEST_UI_MENU, GUEST_UI_DIALOG, GUEST_UI_GAMEPLAY } guest_ui_t;

typedef struct {
    int shutdown;
    int transition;
    int size_move;
    int focused;
    int minimized;
    guest_ui_t ui;
} input_signals_t;

input_state_t input_state_from(const input_signals_t* s);
const char* input_state_name(input_state_t state);
/* Only focused gameplay takes relative Raw Input, hides the cursor and confines it to the picture. */
int input_captured(input_state_t state);
/* In gameplay the game reads a cursor it re-centres itself; elsewhere it follows the real one. */
int input_cursor_virtual(input_state_t state);
int input_hide_pointer(input_state_t state, int game_cursor_hidden);
/* RasterBlt's source rectangle (left, top, right, bottom) kept inside its source raster: the game sizes it from a
 * control's rect, which the host's layouts may make larger than the control's picture. 0 when nothing is left. */
int raster_source_clip(int32_t rect[4], int32_t source_w, int32_t source_h);
/* A point inside a button's rect, scaled onto its picture along each axis where the rect is larger (the picture is
 * drawn zoomed to fill it), so the game's mask hit test samples inside the picture. 1 when the point changed. */
int button_picture_point(const int32_t rect[4], int32_t picture_w, int32_t picture_h, int32_t point[2]);
/* Last input wins: no pointer in menus while the gamepad moves the focus. */
int input_pad_hides_pointer(input_state_t state, int pad_focus);
/* A capture waits while a button is held outside the picture (the click that activated the window
 * on its title bar), so that drag is not pulled into the view. */
int input_should_clip(input_state_t state, int pointer_in_view, int button_held);
/* The one place relative pointer motion lands, the mouse's (Raw Input) and the right stick's (whole counts) alike: the
 * virtual cursor CInput reads and re-centres, moved only while gameplay holds the pointer. 1 when it moved. */
int pointer_add_relative(input_state_t state, long* x, long* y, long dx, long dy);
/* The game takes its pointer back from a state where its cursor was the real one: the virtual cursor starts at the
 * centre CInput recentres to, not where the real cursor was left. 1 when it was set. */
int pointer_recapture(input_state_t before, input_state_t after, long* x, long* y, long width, long height);
/* The mouse's own motion for mouse-look, scaled by Mouse sensitivity before it reaches the virtual cursor. The parts of
 * a count a scale leaves carry to the next motion, so slow movements are kept; 1.0 passes counts through unchanged. */
typedef struct {
    double rest_x, rest_y;
} mouse_scale_t;
void mouse_scale_counts(mouse_scale_t* m, float sensitivity, long dx, long dy, long* out_x, long* out_y);

typedef enum {
    KEY_TO_GAME,
    KEY_DROP,
    KEY_HOST_FULLSCREEN,
    KEY_HOST_MINIMAP,
    KEY_HOST_CHEATS,
    KEY_HOST_CLOSE,
    KEY_HOST_CONTROLS,
    KEY_HOST_OBJECTIVES,
    KEY_HOST_SNAPSHOT,
    KEY_HOST_SHOW_OBJECTIVE,
    KEY_HOST_QUICK_SAVE,
    KEY_HOST_QUICK_LOAD
} key_action_t;

/* msg is WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP; repeat is lParam bit 30. F2 is the controls card (unused by
 * the game: CGameWnd's key table 0x0040C220 gives it no action), as are F3, Objectives, and F4 without Alt, Show
 * Objective; F12, the game's controls window, is not passed on. */
key_action_t key_policy(unsigned msg, unsigned vk, int repeat, int ctrl, int alt);
/* The Quick Save and Quick Load keys (trespasser.ini); they win over the port's other uses of the same key. */
void key_policy_set_quick_keys(unsigned save_vk, unsigned load_vk);
/* Esc and F1 open a window of the game over gameplay: the cursor is freed before it opens. */
int key_opens_game_window(unsigned msg, unsigned vk, int repeat);

/* CGameWnd opens its window on Esc/F1 key-down, but CUIWnd::OnKey (0x0040AF50) cancels a window
 * on Esc key-up: the key-up of the press that opened a window must not reach that window. */
typedef struct {
    unsigned vk, depth;
    int armed;
} open_press_t;

void open_press_reset(open_press_t* p);

/* The game's WM_ACTIVATEAPP handler (0x0040EFE0) releases its screen when deactivated and, reactivated, recreates the
 * display and the renderer, which rebuilds the sky texture from its source raster. During a level load that raster's
 * pixels are in the level's paged memory, which the loader thread is still committing, so the game must not see a
 * change of activation until the load ends: then it gets the last one, if it differs from what it saw. */
typedef struct {
    int game_active; /* what the game last saw */
    int wanted;      /* what Windows said last */
    int deferred;    /* a change arrived during the load */
} activation_defer_t;

void activation_defer_init(activation_defer_t* a);
/* WM_ACTIVATEAPP from Windows: 1 when the game gets it now. */
int activation_message(activation_defer_t* a, int loading, int active);
/* The load ended: 1 when the game must now get WM_ACTIVATEAPP with *active. */
int activation_after_load(activation_defer_t* a, int* active);
/* The host itself sent the game WM_ACTIVATEAPP. */
void activation_delivered(activation_defer_t* a, int active);
void open_press_keydown(open_press_t* p, unsigned vk, int repeat, int gameplay, unsigned depth);
/* 1 when this key-up belongs to the press that opened a window (the game's window stack grew). */
int open_press_keyup(open_press_t* p, unsigned vk, unsigned depth_now);

typedef enum {
    SHUTDOWN_NONE,
    SHUTDOWN_CLOSE_REQUESTED,
    SHUTDOWN_GUEST_QUIT_REQUESTED,
    SHUTDOWN_MODAL_CLOSING,
    SHUTDOWN_GAMEWND_CLOSING,
    SHUTDOWN_MAINSCREEN_CLOSING,
    SHUTDOWN_WINMAIN_RETURNING,
    SHUTDOWN_CRT_EXITING,
    SHUTDOWN_GUEST_STOPPED,
    SHUTDOWN_HOST_CLEANUP,
    SHUTDOWN_EXITED
} shutdown_phase_t;

typedef struct {
    shutdown_phase_t phase;
    const char* source;
    int host_requested;
    unsigned progress;
} shutdown_state_t;

void shutdown_init(shutdown_state_t* s);
/* 1 for the first close request; later ones (a second Alt+F4, the WM_CLOSE a close produces) are ignored. */
int shutdown_request(shutdown_state_t* s, const char* source);
void shutdown_quit_requested(shutdown_state_t* s);
/* Phases only move forward; 1 when this one is new. The game's own Exit starts at WinMainReturning. */
int shutdown_advance(shutdown_state_t* s, shutdown_phase_t phase, const char* source);
/* Any step of the game's way out counts as progress, even one that stays in the same phase. */
void shutdown_note_progress(shutdown_state_t* s);
void shutdown_exited(shutdown_state_t* s);
int shutdown_active(const shutdown_state_t* s);
/* The host leads the game out until its CRT takes over. */
int shutdown_host_driving(const shutdown_state_t* s);
int shutdown_guest_may_run(const shutdown_state_t* s);
int shutdown_renderer_allowed(const shutdown_state_t* s);
const char* shutdown_phase_name(shutdown_phase_t phase);
/* Milliseconds without progress after which the n-th stall report is due: 250, 1000, 2000, 4000... */
uint32_t shutdown_stall_threshold(unsigned reports);

/* The game's windows the host's quit path tells apart. */
typedef enum {
    GUEST_WND_NONE,
    GUEST_WND_GAME,
    GUEST_WND_INGAME_OPTIONS,
    GUEST_WND_QUIT,
    GUEST_WND_MAIN_MENU,
    GUEST_WND_VIDEO,
    GUEST_WND_LOADER,
    GUEST_WND_OTHER
} guest_wnd_t;

typedef enum { QUIT_WAIT, QUIT_END_WINDOW, QUIT_SKIP_VIDEO } quit_step_t;

typedef struct {
    quit_step_t step;
    int32_t code;
    int quit_to_windows;
    shutdown_phase_t phase;
} quit_action_t;

/* One step of the game's own way out for the window in front: the in-game Quit (CQuitWnd answer 1
 * ends gameplay with -1), the main menu's Exit (-1), Cancel (0) for any other window. */
quit_action_t quit_action_for(guest_wnd_t top, int at_video_pump);

/* A present the game makes outside its UI frame while its menus are on the UI canvas: DoModal (0x00417840) clears the
 * screen and flips it three times before a window's first frame, and those frames hold nothing. Held back, the screen
 * keeps the menu's last frame; videos and loading windows present themselves and are always shown. */
int ui_present_held(int in_ui_frame, int menu_on_canvas, guest_wnd_t top);

/* A level load clears and flips the screen outside any UI frame: its DirectDraw restart (0x00412B70) before the loading
 * screen, the next window's DoModal (0x00417840) after it, three times each. Held from the load's start until the first
 * UI frame, the screen keeps the last loading picture; the loading screen's own presents are never held, and the caps
 * let any other present through. */
#define LOAD_HOLD_MAX_PRESENTS 4u
#define LOAD_HOLD_MAX_MS 8000u

typedef struct {
    int armed;
    unsigned held;
    uint32_t since;
} load_hold_t;

void load_hold_arm(load_hold_t* h, uint32_t now_ms);
void load_hold_release(load_hold_t* h);
/* 1 when this present is held. */
int load_hold_present(load_hold_t* h, int loading_screen_present, uint32_t now_ms);

typedef enum {
    READBACK_LOCK_READ,
    READBACK_LOCK_WRITE,
    READBACK_GETDC,
    READBACK_PIXEL_CALIBRATION,
    READBACK_BLT_SOURCE,
    READBACK_FRONT_BUFFER,
    READBACK_OTHER,
    READBACK_REASON_COUNT
} readback_reason_t;

typedef enum { CPU_ACCESS_LOCK, CPU_ACCESS_GETDC, CPU_ACCESS_BLT_SOURCE, CPU_ACCESS_BLT_DEST, CPU_ACCESS_FRONT, CPU_ACCESS_OTHER } cpu_access_t;

/* calibration_caller is the game function that reads test polygons back at start-up. */
/* The game's start-up probe of the Direct3D device's pixel coverage (0x00514230): it reads a few pixels of a quad. */
int is_pixel_calibration_caller(uint32_t caller);
readback_reason_t readback_classify(cpu_access_t access, int read_only, uint32_t caller);
const char* readback_reason_name(readback_reason_t reason);

/* Raster Locks that touch no pixel: CScreenRenderDWI::BeginFrame only hands the surface address to the
 * span rasterisers (which Lock again before drawing), CScreenRenderAuxD3D::FlushBatch locks to wait for
 * the hardware. */
#define LOCK_CALLER_BEGIN_FRAME GVA(0x0055F1D0)
#define LOCK_CALLER_FLUSH_BATCH GVA(0x00555AB0)

/* The gameplay text overlay's draws (0x0050EAF0, one line 0x0050E830): GetDC, then aliased TextOutA only (the host
 * makes their font non-antialiased), so nothing under the text is read. */
#define GETDC_CALLER_TEXT_OVERLAY GVA(0x0050EAF0)

/* Whether a CPU access to a Direct3D render target needs the GPU's pixels in the CPU copy. The game
 * locks with DDLOCK_WAIT only, so write-only flags never come from it; callers decide instead. */
/* MyFillRect (0x004121B0) fills its rectangle; RasterBlt (0x004128A0) copies its source over the rectangle and reads
 * the screen only when the screen is also its source (screen_blt_write_only says it is not). */
#define LOCK_CALLER_FILL_RECT GVA(0x004121B0)
#define LOCK_CALLER_RASTER_BLT GVA(0x004128A0)
#define LOCK_CALLER_CLEAR GVA(0x005035B0) /* CRasterVid::Clear without DirectDraw: fills every row */
/* The pixels a rasterised polygon can write: its screen vertices' bounds widened by margin, inside width x height;
 * 0 when nothing is left or a coordinate is not finite. */
int cpu_polygon_box(const float* x, const float* y, unsigned count, long width, long height, long margin, long box[4]);
/* A box of a raster whose first pixel is origin_bytes into a 16 bpp surface of this pitch, in surface pixels. */
void cpu_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch);
int target_access_reads(cpu_access_t access, uint32_t lock_flags, uint32_t caller, int screen_blt_write_only);

/* The game's "unstable last time, Safe Mode?" prompt: WinMain sets registry "Safemode" to 1 at start
 * and to 0 at its end, and a 1 at start-up makes sub_0040D780 ask through the string-table message
 * box sub_0040E4D0 (type 0x23, title 1, text 11). By default the value reads as 0 and is never stored,
 * and that one message answers No. */
#define SAFEMODE_MSGBOX_HELPER GVA(0x0040E4D0)
#define SAFEMODE_PROMPT_TYPE 0x23u
#define SAFEMODE_PROMPT_TITLE 1u
#define SAFEMODE_PROMPT_TEXT 11u
int safemode_value_virtual(int legacy);
/* "Hardware Water" read by the game's driver set-up (0x00506A50, string at 0x00660094) while it builds
 * the texture formats: 1 makes water a CRasterD3D alpha texture drawn blended (render type 3); 0 keeps
 * the software water texture. Answers 1 or 0, or -1 to leave the stored value / game default. */
int hardware_water_policy(int hardware_renderer, int value_stored, int software_water_requested);

/* The two openings CVideoWnd::Play (0x00418AE0) shows: "menu\tpassintro" before the main menu (0x0040F2BC) and
 * "menu\newgame" after New Game (0x00415687). The credits and the ending are never openings. */
typedef enum { INTRO_NONE, INTRO_STARTUP, INTRO_NEW_GAME } intro_video_t;
intro_video_t intro_video_of(const char* guest_name);
const char* intro_video_name(intro_video_t video);
/* Skip Intro answers the game's own "NoVideo" for an opening it plays itself; the Video Test's requests still play. */
int skip_intro_applies(int enabled, intro_video_t video, int video_test_request);

/* The human pelvis's crouch height (Pel[me][MOUTH]) after Pelvic_IDOF's clamps (0x004F441D, 0x004F4442) and its ceiling
 * check (0x004F46F2), as the x87 computes them at the 24-bit precision CPhysicsSystem::Integrate (0x004CF8A0) selects:
 * the bounds rounded to single, so a height clamped and stored as a float never reads below its own bound again.
 * knee, low and high: the game's constants 0.2, 0.09 and 1.1. */
typedef struct {
    float pos, vel;
} pelvis_mouth_t;
pelvis_mouth_t pelvis_mouth_clamps(float pos, float vel, float height, float bio_body_z, double knee, double low,
                                   double high);

/* Movement Speed m: the right hand (a world-space DOF chasing body + rest pose through Update_Right_Arm, capped at
 * 150 m/s^2) is carried by k = 1 - 1/m of the body's own motion and its controller sees the rest, as at 1x. k is 0 at
 * 1x and below: the game's own arithmetic. */
float hand_frame_share(float multiplier);

/* The body as the previous step left it. The body's velocity also changes outside its acceleration (Foot_IDOF's
 * contact removes the part into the ground, a jump sets it) and its position is rebuilt from the foot every step, so
 * the carrier follows the measured body, never an integral of its acceleration. */
typedef struct {
    int valid;
    float body_pos[3], body_vel[3], dt;
} hand_carrier_t;

#define HAND_CARRIER_JUMP_M 1.0f /* a body displacement its velocity cannot explain (teleport, load): not carried */

/* At Pelvic_IDOF's entry, before Update_Right_Arm reads the hand: the hand gets k times the body's velocity change
 * and k times the displacement its carried velocity did not already give it, so after any number of steps the
 * carrier has moved it by exactly k times the body's displacement. k <= 0 stops carrying. */
void hand_carrier_step(hand_carrier_t* c, float k, float dt, const float body_pos[3], const float body_vel[3],
                       float hand_pos[3], float hand_vel[3]);
/* Update_Right_Arm damps hand velocity - 1.25 body velocity; relative to the carrier that is
 * (hand - k body) - 1.25 (1 - k) body, i.e. the hand's velocity shifted by 0.25 k body. sign +1 before, -1 after. */
void hand_damping_shift(float k, float sign, const float body_vel[3], float hand_vel[3]);

int safemode_prompt_args(uint32_t func, uint32_t type, uint32_t title_id, uint32_t text_id);

/* Lifted calls push a dummy return address, so a caller is found in pcrecomp's indirect-call ring:
 * the newest call into one of the wrappers from outside them names the function that made it (CRasterWin::LockAll
 * reaches CRasterVid::Lock by a tail jump, itself a ring entry); 0 if none is left. */
uint32_t icall_ring_caller(const uint32_t* targets, const uint32_t* from, uint32_t next, unsigned size,
                           const uint32_t* wrappers, unsigned wrapper_count);

/* Engine limits lifted with Direct3D 11 (engine_limits.c). */
#define ENGINE_CULL_SCALE_ORIGINAL 1.25f /* render quality 4 */
#define ENGINE_CULL_MAX_ORIGINAL 900.0f
#define ENGINE_CULL_MIN 20.0f
#define ENGINE_CULL_MAX_RADIUS 8.0f
/* bInRange (0x00477A40) compares squared distances with the cull distance squared times fCullDistanceCombined, which
 * SetGlobalPartitionData (0x004A24B0) sets to 1: every object, a level's own "Culling" included, stays drawn this many
 * times as far with the square in its place. */
#define ENGINE_OBJECT_DISTANCE_SCALE 1.4f
/* bShouldCache's fMinDistanceToCache (0x0064E458, ATX's ObjectClarityDrawDistance) times the object's CacheMul: nearer,
 * an object is never replaced by its image-cache impostor. Under Direct3D an object stays 3D while its bounding sphere
 * spans more than ENGINE_IMPOSTOR_SWITCH_PX on screen (half the 256 px impostor), within the min/max distances and
 * never nearer than 1998's. A mesh that is not hardware-able is drawn by the CPU into the Direct3D frame, so it and
 * every group holding one keep the 1998 distance. */
#define ENGINE_CACHE_DISTANCE_ORIGINAL 15.0f
#define ENGINE_FULL_3D_MIN 25.0f
#define ENGINE_FULL_3D_MAX 150.0f
#define ENGINE_IMPOSTOR_SWITCH_PX 256.0f
/* A static instance whose bounding sphere spans less than this on screen is not drawn. */
#define ENGINE_CULL_PIXELS 1.5f
#define ENGINE_PIXELS_PER_METRE_DEFAULT 1251.0f /* 1440 lines, the game's 4:3 tan(half width) 0.7673 */
float engine_view_pixels_per_metre(float view_height, float tan_half_height);
/* How far an object of this radius stays 3D (metres from its centre plus cylinder radius, as bShouldCache measures). */
float engine_full_3d_distance(float radius, float pixels_per_metre, float cache_mul);

/* By the partition's class (vtable): pure spatial groups, plain static instances, and everything with behaviour
 * (CEntity and its descendants: animals, the player, guns, triggers, water, backdrops...). */
#define VTABLE_PARTITION GVA(0x0064B3C8)
#define VTABLE_PARTITION_SPACE GVA(0x0064CAEC)
#define VTABLE_PARTITION_SPACE_QUERY GVA(0x0064CC54)
#define VTABLE_INSTANCE GVA(0x0064B22C)
#define VTABLE_TERRAIN_OBJECT GVA(0x0064CEA8)
typedef enum { ENGINE_PART_SPATIAL, ENGINE_PART_STATIC, ENGINE_PART_ENTITY } engine_part_kind_t;
engine_part_kind_t engine_partition_kind(uint32_t vtable);
/* The fMinDistanceToCache to store for one bShouldCache call, which multiplies it by the CacheMul again. Only static
 * objects and spatial groups get the modern distance; entities and meshes the CPU draws keep 1998's. */
float engine_cache_min_distance(int modern, int software, engine_part_kind_t kind, float radius, float pixels_per_metre,
                                float cache_mul);
/* Stored instead when the object a partition belongs to stays 3D this frame: every part of it stays 3D too. */
#define ENGINE_CACHE_DISTANCE_NEVER 1e30f
float engine_projected_pixels(float radius, float distance_sqr, float pixels_per_metre);
int engine_projected_cull(engine_part_kind_t kind, int uses_mesh, float radius, float distance_sqr,
                          float pixels_per_metre);

/* Partitions marked in the current frame (open addressing, size a power of two). */
typedef struct {
    uint32_t key, frame;
} engine_frame_slot_t;
void engine_frame_set_add(engine_frame_slot_t* slots, unsigned size, uint32_t key, uint32_t frame);
int engine_frame_set_has(const engine_frame_slot_t* slots, unsigned size, uint32_t key, uint32_t frame);

typedef enum { ENGINE_SIZE_SMALL, ENGINE_SIZE_MEDIUM, ENGINE_SIZE_LARGE, ENGINE_SIZE_LANDMARK, ENGINE_SIZE_COUNT } engine_size_t;
engine_size_t engine_size_class(float radius);

/* SPartitionData bits and the order of bShouldCache's (0x00543830) refusals: bCanCache (0x00477B60), the bounding
 * sphere, the bounding cylinder against fMaxAcceptCylinder, bCacheGroup (0x00477E90), the minimum distance, and the
 * parallax (shear) test last. */
#define PARTITION_USES_MESH 0x20u
#define PARTITION_CACHEABLE 0x100u
#define PARTITION_CACHEABLE_VOLUME 0x400u
#define PARTITION_CACHE_NEVER 0x800u /* the level's CacheNever */
#define PARTITION_SIMPLE_OBJECT 0x4000u
#define PARTITION_ALWAYS_FACE 0x8000u
#define PARTITION_HARDWARE_ABLE 0x10000u
typedef enum {
    CACHE_REFUSED_NOT_CACHEABLE,
    CACHE_REFUSED_CACHE_NEVER,
    CACHE_REFUSED_SIMPLE_OBJECT,
    CACHE_REFUSED_FACES_CAMERA,
    CACHE_REFUSED_VOLUME,
    CACHE_REFUSED_MOVING,
    CACHE_REFUSED_TOO_CLOSE,
    CACHE_REFUSED_TOO_BIG,
    CACHE_REFUSED_GROUP,
    CACHE_REFUSED_3D_DISTANCE,
    CACHE_REFUSED_PARALLAX,
    CACHE_REFUSED_COUNT
} cache_refusal_t;
typedef struct {
    uint32_t flags, no_cache_until, frame;
    float sphere_sqr, cylinder_sqr, area_efficiency_sqr, cache_mul;
} engine_partition_t;
typedef struct {
    int hardware;
    float max_accept_cylinder, area_threshold_sqr, min_distance;
} engine_cache_settings_t;
cache_refusal_t engine_cache_refusal(const engine_partition_t* p, const engine_cache_settings_t* c, float distance_sqr);
/* An object refused for these reasons becomes one impostor later, so its parts wait for it; one that can never be cached
 * (level flags, movement, volume) leaves each part to its own decision, as the 1998 engine does. */
int engine_refusal_holds_parts(cache_refusal_t reason);
const char* engine_cache_refusal_name(cache_refusal_t reason);
/* bFlagCacheable (0x00478A30) refuses a partition marked CacheNever before anything else; past that it refuses an
 * animated or alpha-colour mesh, a backdrop, water and any uncacheable part, which the image cache cannot draw. With
 * Direct3D 11 a plain static instance is judged by those alone: it moves only through physics, which stops its
 * caching while it moves (bCanCache's NoCacheUntil), and it is an impostor only past its 3D distance. */
int engine_cache_never_lifted(int modern, uint32_t vtable, uint32_t flags);
/* rcsRenderCacheSettings.iMaxObjectsPerCache (0x007371A0): CRenderCache::SetParameters sets 10, 15 or 20 by quality;
 * a spatial group with more instances is never one impostor (bFlagCacheableVolume, read when a level loads). */
#define ENGINE_CACHE_GROUP_OBJECTS_MODERN 40
int engine_cache_group_objects(int modern, int game_value);
#define ENGINE_TEX_DIM_ORIGINAL 128u     /* "Max Recommended Tex Dim" default; the 1998 setup offered 32-128 */
#define ENGINE_TEX_DIM_MODERN 256u       /* the largest level in the game's data */
#define ENGINE_TEXTURE_MB_ORIGINAL 24u   /* iLargeMemAllocMB with more than 92 MB of RAM */
#define ENGINE_TEXTURE_MB_SAFE 40u       /* below 46 MB, where the engine's own 32-bit arithmetic overflows */
#define ENGINE_TEXTURE_MB_MODERN 64u     /* with the host's 64-bit split */
#define ENGINE_IMAGE_CACHE_KB_MAX 20480u /* CRenderCache::SetTextureMemSize's kb * 95 * 1024 stays below 2^31 */
#define ENGINE_TERRAIN_TEXTURE_KB_MODERN 16384u /* the game asks 1280-1920 KB; 128 KB per 256 px page */
#define ENGINE_RENDER_QUALITY_TOP 4u
/* SPartitionSettings::fGetCullDistance (0x004A22D0) plus the radius: how far an object of that radius stays drawn
 * with level cull distance max_at (CullMaxDist, 800 by default). */
float engine_cull_distance(float radius, float max_at, float scale, float cap);
/* fixed_split: the host corrects the split's products (engine_limits.c), so the budget may pass 45 MB. */
unsigned engine_texture_budget_mb(unsigned game_mb, int modern, int fixed_split);
/* How CDirect3D splits a texture budget (bUseNonLocalMemory 0x00506DD0, bAllocateNonLocalMemory 0x00506EB0,
 * CAGPTextureMemManager::AllocateMemory 0x00514FE0). Without fixed_split, in the engine's own signed 32-bit
 * arithmetic: `accepted` is bIsActive (0x00515080), whose failure turns Direct3D off; `overflow` says a product
 * wrapped. With it, the host's: 64-bit products and an image cache of at most ENGINE_IMAGE_CACHE_KB_MAX. */
typedef struct {
    int32_t total, image_cache, water, terrain, regular, low_water_85, high_water_95;
    int accepted, overflow;
} engine_texture_layout_t;
engine_texture_layout_t engine_texture_layout(unsigned budget_mb, uint32_t free_video_memory, int fixed_split);
/* The image-cache share of the split: the engine's T * 40 / 100 in 32 bits, or the host's. */
int32_t engine_image_cache_bytes(int32_t total, int fixed_split);
/* AllocateMemory's 85% and 95% marks of the regular textures in 64-bit arithmetic; all 0 when the engine refuses. */
void engine_agp_marks(int32_t bytes, int32_t* allocated, int32_t* low_water_85, int32_t* high_water_95);
/* Full-size impostor pages (ARGB1555, dim x dim) that fit the cache. */
unsigned engine_impostor_pages(unsigned cache_kb, unsigned dim);
/* "Max Recommended Tex Dim" and "RenderQuality" answers: 0 leaves the stored value or the game default. */
uint32_t engine_texture_dim_policy(int modern);
uint32_t engine_render_quality_policy(int hardware, int value_stored);

/* The game's UseSubTitles registry value is On until a choice is stored. A store that holds the Off the game wrote at
 * exit before the port kept this default becomes On once (marker absent); a stored choice after that is kept. Returns 1
 * and the new value when the stored one is replaced. */
#define REGISTRY_SUBTITLES_ON 0xFFFFFFFFu
int registry_subtitles_migration(int marker_present, int value_present, uint32_t value, uint32_t* replacement);

/* The game's own Screen Size margin (XView, YView: steps of its legacy Video options): never, the 3D image fills the
 * resolution the launcher chose. */
#define REGISTRY_VIEW_MARGIN 0u
int registry_view_margin_name(const char* name);
/* UploadPolygonArray (0x0055A4E0) uploads textures until GetTickCount() - start > 7 (a .text immediate); with the
 * 15.6 ms tick the window is anywhere from one upload to 15 ms. What GetTickCount answers inside it instead: the start
 * tick until budget_ms of real time have passed, then a tick past the game's limit. */
#define ENGINE_UPLOAD_MS_ORIGINAL 7u
#define ENGINE_UPLOAD_BUDGET_MS 10.0
uint32_t engine_upload_clock(uint32_t start_tick, double elapsed_ms, double budget_ms);

/* Terrain with Direct3D 11 (terrain_quality.c). CQuadRootTIN::Update multiplies TrrNoDynTextureDist by the quality's
 * distance scale and the pixel tolerances by its tolerance scale; the tolerances are in render pixels. */
#define TERRAIN_TEXTURE_DISTANCE_MAX 1000.0f /* metres of own terrain texture when the far clip is farther */
#define TERRAIN_PIXEL_TOL_480 4.0f           /* the community's TrrPixelTol, at 480 render lines */
#define TERRAIN_PIXEL_TOL_FAR_480 12.0f      /* and TrrPixelTolFar */
#define TERRAIN_HEAP_SCALE 4u                /* the terrain's node heaps, reserved this many times larger */
/* The fixed heaps pHeapAlloc (0x005F8A20) reserves and pvCommitMem (0x005F8B10) commits on demand. The pipeline's
 * per-scene CDArrays (vertices, vertex pointers, polygons, polygon pointers) store their capacity at +0xC before the
 * reservation: it grows with them. */
#define ENGINE_PIPELINE_HEAP_SCALE 4u
typedef struct {
    uint32_t bytes, capacity_va, capacity;
    unsigned scale;
} engine_heap_t;
/* The heap a reservation of this size is, scale 1 when it is none of them. */
engine_heap_t engine_heap_for(uint32_t bytes);
/* What pvCommitMem may commit of a request at pv: whole pages that stay inside the reservation ending at end. */
uint32_t engine_heap_commit_bytes(uint32_t pv, uint32_t bytes, uint32_t reservation_end, uint32_t page);
typedef struct {
    float dynamic_texture_distance, pixel_tolerance, pixel_tolerance_far; /* the range variables */
    float distance_scale, pixel_tolerance_scale;                          /* the render quality's multipliers */
} terrain_settings_t;
/* What one Update uses: own textures out to the far clip (at most TERRAIN_TEXTURE_DISTANCE_MAX) unless the level
 * already goes farther, and the level's tolerances capped at the community's, scaled from 480 lines to the render
 * height (lines_scale = height / 480; 0 keeps the level's); far is never finer than near. */
terrain_settings_t terrain_modern_settings(terrain_settings_t level, float far_clip, float lines_scale);

/* Noclip. The box of each part of Anne's skeleton meets the world in the physics step: a contact kills the box's
 * velocity along its normal and adds a force to the part's BioTag, which Pelvic_IDOF reads. Both are linear in the
 * contacts, so keeping only their vertical components leaves every response that holds Anne up and drops every one that
 * stops her sideways, whatever the normal: a wall (normal horizontal) is gone, a floor or a ceiling (vertical) is whole,
 * a slope keeps the vertical share of its response. */
#define NC_PARTS 6
extern const unsigned nc_part_dof[NC_PARTS]; /* the DOF of each part's x; y and z follow */
#define NC_VELOCITY_NOISE 0.05f              /* m/s: below it a velocity is not a contact's work */
typedef struct {
    int valid;
    float vx[NC_PARTS], vy[NC_PARTS]; /* each part's horizontal velocity as the next step starts */
} nc_memory_t;
/* The velocity after Pelvis_integrate's Euler step (v + a dt, in the double the lifted code evaluates). */
float nc_next_velocity(float velocity, float acceleration, float dt);
/* A contact response (a BioTag x, y, z or a velocity change) with only its vertical component left. */
void nc_vertical_response(float response[3]);
/* The horizontal velocity a part keeps when it was `remembered` before the step and is `now` after the contacts:
 * the remembered one when the contacts changed it, else the current one. */
float nc_horizontal_velocity(float remembered, float now);
/* A contact's change of velocity (v - (v . n) n) with its horizontal part left out. */
void nc_vertical_kill(const float before[3], const float after[3], float out[3]);

/* A miss the game itself expects: the *.swp search before any level swap file exists. */
int host_expected_miss(const char* api, const char* path, uint32_t error);
/* The legacy Aureal A3D library: never loaded, the game takes its DirectSound path. */
int host_unavailable_library(const char* name);

#endif
