#include "host_policy.h"

#include <math.h>
#include <string.h>

#define FPS_STALL_WINDOWS 2

enum { MSG_KEYDOWN = 0x100, MSG_KEYUP = 0x101, MSG_SYSKEYDOWN = 0x104, MSG_SYSKEYUP = 0x105 };
enum { VK_ESC = 0x1B, VK_F1_ = 0x70, VK_F2_ = 0x71, VK_F4_ = 0x73, VK_F5_ = 0x74, VK_F11_ = 0x7A, VK_F12_ = 0x7B };

void fps_counter_init(fps_counter_t* c, int64_t frequency, int64_t now) {
    memset(c, 0, sizeof *c);
    c->frequency = frequency > 0 ? frequency : 1;
    c->window_start = now;
    c->last_present = now;
}

int fps_counter_present(fps_counter_t* c, int64_t now) {
    int64_t elapsed = now - c->window_start;
    c->frames++;
    c->total_frames++;
    c->last_present = now;
    if (elapsed < c->frequency) return 0;
    c->fps = (double)c->frames * (double)c->frequency / (double)elapsed;
    c->frames = 0;
    c->window_start = now;
    return 1;
}

int fps_counter_idle(fps_counter_t* c, int64_t now) {
    if (now - c->last_present < FPS_STALL_WINDOWS * c->frequency) return 0;
    c->frames = 0;
    c->window_start = now;
    if (c->fps == 0.0) return 0;
    c->fps = 0.0;
    return 1;
}

input_state_t input_state_from(const input_signals_t* s) {
    if (s->shutdown) return INPUT_SHUTDOWN;
    if (s->transition) return INPUT_FULLSCREEN_TRANSITION;
    if (s->size_move) return INPUT_MOVING_WINDOW;
    if (s->ui == GUEST_UI_DIALOG) return INPUT_DIALOG;
    if (s->ui == GUEST_UI_GAMEPLAY) return s->focused && !s->minimized ? INPUT_GAMEPLAY_CAPTURED : INPUT_GAMEPLAY_RELEASED;
    return INPUT_MENU;
}

const char* input_state_name(input_state_t state) {
    static const char* const names[INPUT_STATE_COUNT] = {
        "INPUT_MENU", "INPUT_GAMEPLAY_CAPTURED", "INPUT_GAMEPLAY_RELEASED", "INPUT_DIALOG",
        "INPUT_MOVING_WINDOW", "INPUT_FULLSCREEN_TRANSITION", "INPUT_SHUTDOWN",
    };
    return state >= 0 && state < INPUT_STATE_COUNT ? names[state] : "?";
}

int input_captured(input_state_t state) { return state == INPUT_GAMEPLAY_CAPTURED; }

int input_cursor_virtual(input_state_t state) {
    return state == INPUT_GAMEPLAY_CAPTURED || state == INPUT_GAMEPLAY_RELEASED || state == INPUT_MOVING_WINDOW ||
           state == INPUT_FULLSCREEN_TRANSITION || state == INPUT_SHUTDOWN;
}

int input_hide_pointer(input_state_t state, int game_cursor_hidden) {
    switch (state) {
    case INPUT_GAMEPLAY_CAPTURED: return 1;
    case INPUT_MENU:
    case INPUT_DIALOG: return game_cursor_hidden;
    default: return 0;
    }
}

int raster_source_clip(int32_t rect[4], int32_t source_w, int32_t source_h) {
    if (rect[0] < 0) rect[0] = 0;
    if (rect[1] < 0) rect[1] = 0;
    if (rect[2] > source_w) rect[2] = source_w;
    if (rect[3] > source_h) rect[3] = source_h;
    return rect[2] > rect[0] && rect[3] > rect[1];
}

int input_pad_hides_pointer(input_state_t state, int pad_focus) {
    return pad_focus && (state == INPUT_MENU || state == INPUT_DIALOG);
}

int input_should_clip(input_state_t state, int pointer_in_view, int button_held) {
    return input_captured(state) && (pointer_in_view || !button_held);
}

key_action_t key_policy(unsigned msg, unsigned vk, int repeat, int ctrl, int alt) {
    int down = msg == MSG_KEYDOWN || msg == MSG_SYSKEYDOWN;
    if (msg != MSG_KEYDOWN && msg != MSG_KEYUP && msg != MSG_SYSKEYDOWN && msg != MSG_SYSKEYUP) return KEY_TO_GAME;
    switch (vk) {
    case VK_F11_:
        if (ctrl) return KEY_TO_GAME;
        return down && !repeat ? KEY_HOST_FULLSCREEN : KEY_DROP;
    case VK_F5_: return down && !repeat ? KEY_HOST_VIDEO_TEST : KEY_DROP;
    case VK_F4_:
        if (!alt) return KEY_TO_GAME;
        return down && !repeat ? KEY_HOST_CLOSE : KEY_DROP;
    case VK_F2_: return down && !repeat ? KEY_HOST_CONTROLS : KEY_DROP;
    case VK_F12_: return KEY_DROP;
    case VK_ESC:
    case VK_F1_:
        /* Each opens a modal window of the game on key-down; auto-repeat would open it again. */
        return down && repeat ? KEY_DROP : KEY_TO_GAME;
    default: return KEY_TO_GAME;
    }
}

int key_opens_game_window(unsigned msg, unsigned vk, int repeat) {
    return msg == MSG_KEYDOWN && !repeat && (vk == VK_ESC || vk == VK_F1_);
}

int pointer_add_relative(input_state_t state, long* x, long* y, long dx, long dy) {
    if (!input_captured(state) || (!dx && !dy)) return 0;
    *x += dx;
    *y += dy;
    return 1;
}

void open_press_reset(open_press_t* p) { memset(p, 0, sizeof *p); }

void open_press_keydown(open_press_t* p, unsigned vk, int repeat, int gameplay, unsigned depth) {
    if (repeat || (vk != VK_ESC && vk != VK_F1_)) return;
    p->armed = gameplay != 0;
    p->vk = vk;
    p->depth = depth;
}

int open_press_keyup(open_press_t* p, unsigned vk, unsigned depth_now) {
    if (!p->armed || p->vk != vk) return 0;
    p->armed = 0;
    return depth_now > p->depth;
}

void shutdown_init(shutdown_state_t* s) { memset(s, 0, sizeof *s); }

int shutdown_request(shutdown_state_t* s, const char* source) {
    if (s->phase != SHUTDOWN_NONE) return 0;
    s->phase = SHUTDOWN_CLOSE_REQUESTED;
    s->source = source;
    s->host_requested = 1;
    return 1;
}

void shutdown_quit_requested(shutdown_state_t* s) { shutdown_advance(s, SHUTDOWN_GUEST_QUIT_REQUESTED, NULL); }

int shutdown_advance(shutdown_state_t* s, shutdown_phase_t phase, const char* source) {
    if (phase <= s->phase) return 0;
    if (s->phase == SHUTDOWN_NONE) s->source = source ? source : "the game's own menu";
    s->phase = phase;
    s->progress++;
    return 1;
}

void shutdown_note_progress(shutdown_state_t* s) { s->progress++; }

void shutdown_exited(shutdown_state_t* s) { shutdown_advance(s, SHUTDOWN_EXITED, NULL); }

int shutdown_active(const shutdown_state_t* s) { return s->phase != SHUTDOWN_NONE; }

int shutdown_host_driving(const shutdown_state_t* s) {
    return s->host_requested && s->phase >= SHUTDOWN_CLOSE_REQUESTED && s->phase < SHUTDOWN_CRT_EXITING;
}

int shutdown_guest_may_run(const shutdown_state_t* s) { return s->phase < SHUTDOWN_GUEST_STOPPED; }

int shutdown_renderer_allowed(const shutdown_state_t* s) { return s->phase < SHUTDOWN_HOST_CLEANUP; }

const char* shutdown_phase_name(shutdown_phase_t phase) {
    switch (phase) {
    case SHUTDOWN_NONE: return "Running";
    case SHUTDOWN_CLOSE_REQUESTED: return "CloseRequested";
    case SHUTDOWN_GUEST_QUIT_REQUESTED: return "GuestQuitRequested";
    case SHUTDOWN_MODAL_CLOSING: return "ModalClosing";
    case SHUTDOWN_GAMEWND_CLOSING: return "GameWndClosing";
    case SHUTDOWN_MAINSCREEN_CLOSING: return "MainScreenClosing";
    case SHUTDOWN_WINMAIN_RETURNING: return "WinMainReturning";
    case SHUTDOWN_CRT_EXITING: return "CRTExiting";
    case SHUTDOWN_GUEST_STOPPED: return "GuestStopped";
    case SHUTDOWN_HOST_CLEANUP: return "HostCleanup";
    case SHUTDOWN_EXITED: return "Exited";
    }
    return "?";
}

uint32_t shutdown_stall_threshold(unsigned reports) {
    if (!reports) return 250;
    return reports > 8 ? 1000u << 7 : 1000u << (reports - 1);
}

int ui_present_held(int in_ui_frame, int menu_on_canvas, guest_wnd_t top) {
    return !in_ui_frame && menu_on_canvas && top != GUEST_WND_VIDEO && top != GUEST_WND_LOADER && top != GUEST_WND_NONE;
}

void load_hold_arm(load_hold_t* h, uint32_t now_ms) {
    h->armed = 1;
    h->held = 0;
    h->since = now_ms;
}

void load_hold_release(load_hold_t* h) {
    h->armed = 0;
    h->held = 0;
}

int load_hold_present(load_hold_t* h, int loading_screen_present, uint32_t now_ms) {
    if (!h->armed || loading_screen_present) return 0;
    if (h->held >= LOAD_HOLD_MAX_PRESENTS || now_ms - h->since > LOAD_HOLD_MAX_MS) {
        load_hold_release(h);
        return 0;
    }
    h->held++;
    return 1;
}

quit_action_t quit_action_for(guest_wnd_t top, int at_video_pump) {
    quit_action_t a = { QUIT_WAIT, 0, 0, SHUTDOWN_NONE };
    if (at_video_pump) {
        if (top == GUEST_WND_VIDEO) {
            a.step = QUIT_SKIP_VIDEO;
            a.phase = SHUTDOWN_MODAL_CLOSING;
        }
        return a;
    }
    switch (top) {
    case GUEST_WND_GAME:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.quit_to_windows = 1;
        a.phase = SHUTDOWN_GAMEWND_CLOSING;
        break;
    case GUEST_WND_MAIN_MENU:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.phase = SHUTDOWN_MAINSCREEN_CLOSING;
        break;
    case GUEST_WND_INGAME_OPTIONS:
        a.step = QUIT_END_WINDOW;
        a.code = -1;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    case GUEST_WND_QUIT:
        a.step = QUIT_END_WINDOW;
        a.code = 1;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    case GUEST_WND_OTHER:
        a.step = QUIT_END_WINDOW;
        a.code = 0;
        a.phase = SHUTDOWN_MODAL_CLOSING;
        break;
    default: break;
    }
    return a;
}

int is_pixel_calibration_caller(uint32_t caller) {
    return caller == 0x00513660u || caller == 0x00514150u || caller == 0x00514070u;
}

readback_reason_t readback_classify(cpu_access_t access, int read_only, uint32_t caller) {
    if (is_pixel_calibration_caller(caller)) return READBACK_PIXEL_CALIBRATION;
    switch (access) {
    case CPU_ACCESS_LOCK: return read_only ? READBACK_LOCK_READ : READBACK_LOCK_WRITE;
    case CPU_ACCESS_GETDC: return READBACK_GETDC;
    case CPU_ACCESS_BLT_SOURCE: return READBACK_BLT_SOURCE;
    case CPU_ACCESS_FRONT: return READBACK_FRONT_BUFFER;
    default: return READBACK_OTHER;
    }
}

const char* readback_reason_name(readback_reason_t reason) {
    static const char* const names[READBACK_REASON_COUNT] = {
        "READBACK_LOCK_READ", "READBACK_LOCK_WRITE", "READBACK_GETDC", "READBACK_PIXEL_CALIBRATION",
        "READBACK_BLT_SOURCE", "READBACK_FRONT_BUFFER", "READBACK_OTHER",
    };
    return reason >= 0 && reason < READBACK_REASON_COUNT ? names[reason] : "?";
}

#define LOCK_FLAG_WRITEONLY 0x20u
#define LOCK_FLAG_DISCARDCONTENTS 0x2000u

int target_access_reads(cpu_access_t access, uint32_t lock_flags, uint32_t caller, int screen_blt_write_only) {
    switch (access) {
    case CPU_ACCESS_BLT_DEST: return 0;
    case CPU_ACCESS_GETDC: return caller != GETDC_CALLER_TEXT_OVERLAY;
    case CPU_ACCESS_LOCK:
        if (lock_flags & (LOCK_FLAG_WRITEONLY | LOCK_FLAG_DISCARDCONTENTS)) return 0;
        if (caller == LOCK_CALLER_RASTER_BLT) return !screen_blt_write_only;
        return caller != LOCK_CALLER_BEGIN_FRAME && caller != LOCK_CALLER_FLUSH_BATCH && caller != LOCK_CALLER_FILL_RECT &&
               caller != LOCK_CALLER_CLEAR;
    default: return 1;
    }
}

int safemode_value_virtual(int legacy) { return !legacy; }

int cpu_polygon_box(const float* x, const float* y, unsigned count, long width, long height, long margin, long box[4]) {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    if (!count) return 0;
    for (unsigned i = 0; i < count; i++) {
        if (!(x[i] > -1e6f && x[i] < 1e6f && y[i] > -1e6f && y[i] < 1e6f)) return 0;
        if (!i || x[i] < x0) x0 = x[i];
        if (!i || x[i] > x1) x1 = x[i];
        if (!i || y[i] < y0) y0 = y[i];
        if (!i || y[i] > y1) y1 = y[i];
    }
    box[0] = (long)floorf(x0) - margin;
    box[1] = (long)floorf(y0) - margin;
    box[2] = (long)ceilf(x1) + margin;
    box[3] = (long)ceilf(y1) + margin;
    if (box[0] < 0) box[0] = 0;
    if (box[1] < 0) box[1] = 0;
    if (box[2] > width) box[2] = width;
    if (box[3] > height) box[3] = height;
    return box[0] < box[2] && box[1] < box[3];
}

void cpu_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch) {
    long x, y;
    if (pitch <= 0) return;
    y = (long)(origin_bytes / (uint32_t)pitch);
    x = (long)(origin_bytes % (uint32_t)pitch) / 2;
    box[0] += x;
    box[2] += x;
    box[1] += y;
    box[3] += y;
}

engine_heap_t engine_heap_for(uint32_t bytes) {
    static const engine_heap_t k[] = {
        { 1758600u, 0, 0, TERRAIN_HEAP_SCALE },           /* CQuadRootTIN::AllocMemory(15000) */
        { 543400u, 0, 0, TERRAIN_HEAP_SCALE },            /* NMultiResolution's scaling arrays (22000 nodes) */
        { 160000u, 0, 0, TERRAIN_HEAP_SCALE },            /* CPolyIterator's 40000 triangles */
        { 0x30D400u, 0x00732EFCu, 80000u, ENGINE_PIPELINE_HEAP_SCALE }, /* darvVertices: 80000 x 0x28 */
        { 0x0004E200u, 0x00732F4Cu, 80000u, ENGINE_PIPELINE_HEAP_SCALE }, /* daprvVertices: 80000 x 4 */
        { 0x0025D780u, 0x00732F34u, 20000u, ENGINE_PIPELINE_HEAP_SCALE }, /* darpolyPolygons: 20000 x 0x7C */
        { 0x00013880u, 0x00732F1Cu, 20000u, ENGINE_PIPELINE_HEAP_SCALE }, /* darppolyPolygons: 20000 x 4 */
    };
    engine_heap_t none = { bytes, 0, 0, 1 };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].bytes == bytes) return k[i];
    return none;
}

uint32_t engine_heap_commit_bytes(uint32_t pv, uint32_t bytes, uint32_t reservation_end, uint32_t page) {
    uint32_t want = (bytes + page - 1u) & ~(page - 1u);
    if (pv >= reservation_end) return 0;
    if (want <= reservation_end - pv) return bytes;
    return (reservation_end - pv) & ~(page - 1u);
}

float engine_cull_distance(float radius, float max_at, float scale, float cap) {
    float r = radius < ENGINE_CULL_MAX_RADIUS ? radius : ENGINE_CULL_MAX_RADIUS;
    float d = r * max_at * scale / ENGINE_CULL_MAX_RADIUS;
    if (d < ENGINE_CULL_MIN) d = ENGINE_CULL_MIN;
    if (d > cap) d = cap;
    return d + radius;
}

float engine_view_pixels_per_metre(float view_height, float tan_half_height) {
    if (!(view_height > 0.0f) || !(tan_half_height > 0.0f)) return ENGINE_PIXELS_PER_METRE_DEFAULT;
    return view_height * 0.5f / tan_half_height;
}

static float positive_or_one(float v) { return v > 0.0f && v < 1e6f ? v : 1.0f; }

float engine_full_3d_distance(float radius, float pixels_per_metre, float cache_mul) {
    float mul = positive_or_one(cache_mul), ppm = pixels_per_metre > 0.0f ? pixels_per_metre
                                                                          : ENGINE_PIXELS_PER_METRE_DEFAULT;
    float d = (radius > 0.0f ? 2.0f * radius * ppm / ENGINE_IMPOSTOR_SWITCH_PX : 0.0f) * mul;
    if (d < ENGINE_FULL_3D_MIN) d = ENGINE_FULL_3D_MIN;
    if (d > ENGINE_FULL_3D_MAX) d = ENGINE_FULL_3D_MAX;
    if (d < ENGINE_CACHE_DISTANCE_ORIGINAL * mul) d = ENGINE_CACHE_DISTANCE_ORIGINAL * mul;
    return d;
}

engine_part_kind_t engine_partition_kind(uint32_t vtable) {
    switch (vtable) {
    case VTABLE_PARTITION:
    case VTABLE_PARTITION_SPACE:
    case VTABLE_PARTITION_SPACE_QUERY: return ENGINE_PART_SPATIAL;
    case VTABLE_INSTANCE:
    case VTABLE_TERRAIN_OBJECT: return ENGINE_PART_STATIC;
    default: return ENGINE_PART_ENTITY;
    }
}

float engine_cache_min_distance(int modern, int software, engine_part_kind_t kind, float radius, float pixels_per_metre,
                                float cache_mul) {
    if (!modern || software || kind == ENGINE_PART_ENTITY) return ENGINE_CACHE_DISTANCE_ORIGINAL;
    return engine_full_3d_distance(radius, pixels_per_metre, cache_mul) / positive_or_one(cache_mul);
}

float engine_projected_pixels(float radius, float distance_sqr, float pixels_per_metre) {
    float ppm = pixels_per_metre > 0.0f ? pixels_per_metre : ENGINE_PIXELS_PER_METRE_DEFAULT;
    if (!(distance_sqr > 0.0f)) return 1e30f;
    return 2.0f * radius * ppm / sqrtf(distance_sqr);
}

int engine_projected_cull(engine_part_kind_t kind, int uses_mesh, float radius, float distance_sqr,
                          float pixels_per_metre) {
    if (kind != ENGINE_PART_STATIC || !uses_mesh || !(radius > 0.0f)) return 0;
    return engine_projected_pixels(radius, distance_sqr, pixels_per_metre) < ENGINE_CULL_PIXELS;
}

static unsigned frame_slot(uint32_t key, unsigned size) { return (key * 2654435761u) & (size - 1u); }

void engine_frame_set_add(engine_frame_slot_t* slots, unsigned size, uint32_t key, uint32_t frame) {
    unsigned i = frame_slot(key, size);
    for (unsigned n = 0; n < size; n++, i = (i + 1u) & (size - 1u))
        if (slots[i].frame != frame || slots[i].key == key) {
            slots[i].key = key;
            slots[i].frame = frame;
            return;
        }
}

int engine_frame_set_has(const engine_frame_slot_t* slots, unsigned size, uint32_t key, uint32_t frame) {
    unsigned i = frame_slot(key, size);
    for (unsigned n = 0; n < size; n++, i = (i + 1u) & (size - 1u)) {
        if (slots[i].frame != frame) return 0;
        if (slots[i].key == key) return 1;
    }
    return 0;
}

engine_size_t engine_size_class(float radius) {
    if (radius < 1.0f) return ENGINE_SIZE_SMALL;
    if (radius < 4.0f) return ENGINE_SIZE_MEDIUM;
    if (radius < 10.0f) return ENGINE_SIZE_LARGE;
    return ENGINE_SIZE_LANDMARK;
}

cache_refusal_t engine_cache_refusal(const engine_partition_t* p, const engine_cache_settings_t* c, float distance_sqr) {
    float mul = p->cache_mul;
    if (!(p->flags & PARTITION_CACHEABLE))
        return (p->flags & PARTITION_CACHE_NEVER) ? CACHE_REFUSED_CACHE_NEVER : CACHE_REFUSED_NOT_CACHEABLE;
    if (p->flags & PARTITION_SIMPLE_OBJECT) return CACHE_REFUSED_SIMPLE_OBJECT;
    if (p->flags & PARTITION_ALWAYS_FACE) return CACHE_REFUSED_FACES_CAMERA;
    if (p->no_cache_until >= p->frame) return CACHE_REFUSED_MOVING;
    if (!(p->flags & PARTITION_CACHEABLE_VOLUME)) return CACHE_REFUSED_VOLUME;
    if ((c->hardware ? 4.0f : 2.5f) * p->sphere_sqr > distance_sqr) return CACHE_REFUSED_TOO_CLOSE;
    if (p->cylinder_sqr * c->max_accept_cylinder * mul > distance_sqr) return CACHE_REFUSED_TOO_BIG;
    if (!(p->flags & PARTITION_USES_MESH) &&
        (distance_sqr < 625.0f || !(p->area_efficiency_sqr < c->area_threshold_sqr * distance_sqr)))
        return CACHE_REFUSED_GROUP;
    if (sqrtf(distance_sqr) + sqrtf(p->cylinder_sqr) < c->min_distance * mul) return CACHE_REFUSED_3D_DISTANCE;
    return CACHE_REFUSED_PARALLAX;
}

int engine_refusal_holds_parts(cache_refusal_t reason) {
    return reason == CACHE_REFUSED_TOO_CLOSE || reason == CACHE_REFUSED_TOO_BIG || reason == CACHE_REFUSED_3D_DISTANCE ||
           reason == CACHE_REFUSED_PARALLAX;
}

const char* engine_cache_refusal_name(cache_refusal_t reason) {
    static const char* const names[CACHE_REFUSED_COUNT] = {
        "not cacheable (animated, alpha colour, backdrop, water or such a part)", "CacheNever (level)",
        "simple object", "faces the camera", "volume not cacheable", "moving",
        "too close (sphere)", "too big on screen (cylinder)", "group not worth one impostor", "within the 3D distance",
        "parallax (shear)",
    };
    return reason >= 0 && reason < CACHE_REFUSED_COUNT ? names[reason] : "?";
}

int engine_cache_never_lifted(int modern, uint32_t vtable, uint32_t flags) {
    return modern && vtable == VTABLE_INSTANCE && (flags & PARTITION_CACHE_NEVER) != 0;
}

int engine_cache_group_objects(int modern, int game_value) {
    return modern && game_value < ENGINE_CACHE_GROUP_OBJECTS_MODERN ? ENGINE_CACHE_GROUP_OBJECTS_MODERN : game_value;
}

unsigned engine_texture_budget_mb(unsigned game_mb, int modern, int fixed_split) {
    if (!modern || game_mb != ENGINE_TEXTURE_MB_ORIGINAL) return game_mb;
    return fixed_split ? ENGINE_TEXTURE_MB_MODERN : ENGINE_TEXTURE_MB_SAFE;
}

static int32_t wrap32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }

static int32_t engine_pool(int32_t free_bytes) {
    if (free_bytes < 0x600000) return 0x80000;
    return free_bytes >= 0x1200000 ? 0x1C0000 : 0x120000;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

int32_t engine_image_cache_bytes(int32_t total, int fixed_split) {
    int64_t share = (int64_t)total * 40 / 100, cap = (int64_t)ENGINE_IMAGE_CACHE_KB_MAX * 1024;
    if (!fixed_split) return wrap32((int64_t)total * 40) / 100;
    return (int32_t)(share > cap ? cap : share);
}

void engine_agp_marks(int32_t bytes, int32_t* allocated, int32_t* low_water_85, int32_t* high_water_95) {
    *allocated = bytes;
    *low_water_85 = (int32_t)((int64_t)bytes * 85 / 100);
    *high_water_95 = (int32_t)((int64_t)bytes * 95 / 100);
    if (*low_water_85 < 0x40000) *allocated = *low_water_85 = *high_water_95 = 0;
}

engine_texture_layout_t engine_texture_layout(unsigned budget_mb, uint32_t free_video_memory, int fixed_split) {
    engine_texture_layout_t l;
    uint32_t wanted = budget_mb << 20;
    int32_t free_bytes = (int32_t)(wanted <= free_video_memory ? wanted : free_video_memory);
    int64_t cache_product, cache_limit, regular_85, regular_95;
    memset(&l, 0, sizeof l);
    l.total = free_bytes - engine_pool(free_bytes);
    cache_product = (int64_t)l.total * 40;
    l.image_cache = engine_image_cache_bytes(l.total, fixed_split);
    l.water = (clamp32(l.total / 40, 0x60000, 0x100000) >> 10) << 10;
    l.terrain = (clamp32(l.total / 9, 0x140000, 0x1E0000) >> 10 << 10) / 0x8000 * 0x8000;
    l.regular = l.total - 0x20000 - engine_pool(l.total) - l.image_cache - l.water - l.terrain;
    if (l.regular <= 0x280000) l.regular = 0x280000;
    regular_85 = (int64_t)l.regular * 85;
    regular_95 = (int64_t)l.regular * 95;
    cache_limit = (int64_t)(l.image_cache >> 10) * 95 * 1024; /* CRenderCache::SetTextureMemSize 0x00547790 */
    if (fixed_split) {
        int32_t allocated;
        engine_agp_marks(l.regular, &allocated, &l.low_water_85, &l.high_water_95);
        l.overflow = cache_limit != wrap32(cache_limit);
    } else {
        l.low_water_85 = wrap32(regular_85) / 100;
        l.high_water_95 = wrap32(regular_95) / 100;
        l.overflow = cache_product != wrap32(cache_product) || regular_85 != wrap32(regular_85) ||
                     regular_95 != wrap32(regular_95) || cache_limit != wrap32(cache_limit);
    }
    l.accepted = l.low_water_85 >= 0x40000;
    return l;
}

unsigned engine_impostor_pages(unsigned cache_kb, unsigned dim) {
    unsigned page_kb = dim * dim * 2u / 1024u;
    return page_kb ? cache_kb / page_kb : 0u;
}

uint32_t engine_texture_dim_policy(int modern) { return modern ? ENGINE_TEX_DIM_MODERN : 0u; }

uint32_t engine_render_quality_policy(int hardware, int value_stored) {
    return hardware && !value_stored ? ENGINE_RENDER_QUALITY_TOP : 0u;
}

uint32_t engine_upload_clock(uint32_t start_tick, double elapsed_ms, double budget_ms) {
    return elapsed_ms <= budget_ms ? start_tick : start_tick + ENGINE_UPLOAD_MS_ORIGINAL + 1u;
}

terrain_settings_t terrain_modern_settings(terrain_settings_t level, float far_clip, float lines_scale) {
    terrain_settings_t used = level;
    float reach = far_clip > 0.0f && far_clip < TERRAIN_TEXTURE_DISTANCE_MAX ? far_clip : TERRAIN_TEXTURE_DISTANCE_MAX;
    if (level.distance_scale > 0.0f && reach / level.distance_scale > level.dynamic_texture_distance)
        used.dynamic_texture_distance = reach / level.distance_scale;
    if (lines_scale > 0.0f) {
        if (used.pixel_tolerance > TERRAIN_PIXEL_TOL_480 * lines_scale)
            used.pixel_tolerance = TERRAIN_PIXEL_TOL_480 * lines_scale;
        if (used.pixel_tolerance_far > TERRAIN_PIXEL_TOL_FAR_480 * lines_scale)
            used.pixel_tolerance_far = TERRAIN_PIXEL_TOL_FAR_480 * lines_scale;
    }
    if (used.pixel_tolerance_far < used.pixel_tolerance) used.pixel_tolerance_far = used.pixel_tolerance;
    return used;
}

int hardware_water_policy(int hardware_renderer, int value_stored, int software_water_requested) {
    if (!hardware_renderer || value_stored || software_water_requested) return -1;
    return 1;
}

static int base_name_is(const char* path, const char* name) {
    const char *base = path, *p;
    size_t n = strlen(name);
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    if (_strnicmp(base, name, n)) return 0;
    return !base[n] || !_stricmp(base + n, ".smk");
}

intro_video_t intro_video_of(const char* guest_name) {
    if (!guest_name) return INTRO_NONE;
    if (base_name_is(guest_name, "tpassintro")) return INTRO_STARTUP;
    if (base_name_is(guest_name, "newgame")) return INTRO_NEW_GAME;
    return INTRO_NONE;
}

const char* intro_video_name(intro_video_t video) {
    return video == INTRO_STARTUP ? "tpassintro.smk" : video == INTRO_NEW_GAME ? "newgame.smk" : "";
}

int skip_intro_applies(int enabled, intro_video_t video, int video_test_request) {
    return enabled && video != INTRO_NONE && !video_test_request;
}

static double single(double v) { return (double)(float)v; }

pelvis_mouth_t pelvis_mouth_clamps(float pos, float vel, float height, float bio_body_z, double knee, double low,
                                   double high) {
    double knees = single((double)height - knee), lowest = single(knees * low), highest = single(knees * high);
    pelvis_mouth_t m;
    m.pos = pos;
    m.vel = vel;
    if (!((double)m.pos >= lowest)) {
        m.pos = (float)lowest;
        m.vel = 0.0f;
    }
    if ((double)m.pos > highest) {
        m.pos = (float)highest;
        m.vel = 0.0f;
    }
    if (!(bio_body_z >= 0.0f) && m.vel > 0.0f) m.vel = 0.0f;
    return m;
}

int safemode_prompt_args(uint32_t func, uint32_t type, uint32_t title_id, uint32_t text_id) {
    return func == SAFEMODE_MSGBOX_HELPER && type == SAFEMODE_PROMPT_TYPE && title_id == SAFEMODE_PROMPT_TITLE &&
           text_id == SAFEMODE_PROMPT_TEXT;
}

static int is_wrapper(uint32_t va, const uint32_t* wrappers, unsigned wrapper_count) {
    for (unsigned w = 0; w < wrapper_count; w++)
        if (va == wrappers[w]) return 1;
    return 0;
}

uint32_t icall_ring_caller(const uint32_t* targets, const uint32_t* from, uint32_t next, unsigned size,
                           const uint32_t* wrappers, unsigned wrapper_count) {
    for (unsigned back = 1; back <= size && back <= next; back++) {
        unsigned k = (next - back) & (size - 1);
        if (is_wrapper(targets[k], wrappers, wrapper_count) && !is_wrapper(from[k], wrappers, wrapper_count))
            return from[k];
    }
    return 0;
}
