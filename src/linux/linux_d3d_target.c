/* The render target, the textures and the draws of the guest's Direct3D device. The target is hybrid, as in the Windows host
 * (target_sync.h): the game rasterises some polygons and all text into its own 16-bit copy, and only what it wrote reaches the GPU. */
#include <stdlib.h>
#include <string.h>

#include "ce_effects.h"
#include "ddraw/target_sync.h"
#include "guest_memory.h"
#include "jpt_render_seam.h"
#include "linux_d3d_internal.h"
#include "linux_ddraw.h"
#include "linux_log.h"

#define TARGETS 4
#define DDSCAPS_TEXTURE 0x1000u
#define BLEND_SRCALPHA 5u
#define BLEND_INVSRCALPHA 6u
#define PALETTE_ENTRIES 256u

typedef struct {
    lx_device_t* device;
    int slot;
    uint32_t w, h;
    tsync_t sync;
    uint16_t* reference;
    uint32_t* scratch;
    d3dt_dirty_t dirty;
    int inert_lock, ao_scene;
} target_t;

typedef struct {
    uint64_t id;
    uint32_t w, h, key_low, key_high, palette_sum;
    int format, keyed, dynamic, stale;
    unsigned levels;
    d3dt_upload_history_t history;
    d3dt_rect_t dirty;
} texture_t;

static target_t s_targets[TARGETS];
static texture_t s_textures[LX_DDRAW_OBJECTS];
static lx_d3d_stats_t s_stats;
static unsigned s_generation;
static int s_front_from_gpu = -1;
static int s_water_on, s_sky_on, s_ao_level;
static int s_sky_drawing, s_sky_usable, s_water_lock;
static float s_sky[5];
static int (*s_scene_shown)(void);
static float (*s_tan_half_height)(void);
static uint32_t (*s_lock_caller)(void);

static struct {
    int pending, reads, captured;
    d3dt_rect_t box;
    target_t* target;
    d3dt_dirty_t dirty;
    tsync_tiles_t tiles;
} s_polys;

static struct {
    int drawn; /* slot of the water texture, -1 for none */
    unsigned serial, used;
} s_waves[LX_GPU_WAVE_SLOTS];
static unsigned s_wave_clock;

typedef struct {
    void* p;
    size_t cap;
} buffer_t;
static buffer_t s_vertex_buf, s_texel_buf, s_wave_buf, s_read_buf;

static void* grow(buffer_t* b, size_t bytes) {
    if (bytes > b->cap) {
        void* p = realloc(b->p, bytes);
        if (!p) return NULL;
        b->p = p;
        b->cap = bytes;
    }
    return b->p;
}

lx_d3d_stats_t* lx_target_stats(void) { return &s_stats; }

static void forget_waves(void) {
    memset(s_waves, 0, sizeof s_waves);
    for (unsigned i = 0; i < LX_GPU_WAVE_SLOTS; i++) s_waves[i].drawn = -1;
    s_wave_clock = 0;
}

static void free_target(target_t* t) {
    free(t->reference);
    free(t->scratch);
    memset(t, 0, sizeof *t);
}

void lx_target_reset(void) {
    for (int i = 0; i < TARGETS; i++)
        if (s_targets[i].device) free_target(&s_targets[i]);
    memset(s_textures, 0, sizeof s_textures);
    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_polys, 0, sizeof s_polys);
    forget_waves();
    s_front_from_gpu = -1;
    s_sky_drawing = s_water_lock = 0;
    s_generation = lx_d3d_gpu() ? lx_d3d_gpu()->generation() : 0;
    fx_water_reset();
}

void lx_d3d_configure_effects(int water_refraction, int enhanced_sky, int ao_level) {
    s_water_on = water_refraction != 0;
    s_sky_on = enhanced_sky != 0;
    s_ao_level = ao_level;
    lx_log("video", "water refraction: %s, enhanced sky: %s (Vulkan)", s_water_on ? "on" : "off", s_sky_on ? "on" : "off");
}

void lx_d3d_set_view(int (*scene_shown)(void), float (*tan_half_height)(void)) {
    s_scene_shown = scene_shown;
    s_tan_half_height = tan_half_height;
}

void lx_d3d_set_lock_caller(uint32_t (*caller)(void)) { s_lock_caller = caller; }

void lx_d3d_sky_begin(const float sky[5]) {
    memcpy(s_sky, sky, sizeof s_sky);
    s_sky_usable = fx_sky_usable(sky[3], sky[4], sky[2]);
    s_sky_drawing = 1;
}

void lx_d3d_sky_end(void) { s_sky_drawing = 0; }
void lx_d3d_water_row(void) { s_water_lock = 1; }

static target_t* target_of(int slot) {
    for (int i = 0; i < TARGETS; i++)
        if (s_targets[i].device && s_targets[i].slot == slot) return &s_targets[i];
    return NULL;
}

/* A renderer created again holds none of the textures and an empty target. */
static void follow_generation(const lx_gpu_ops_t* gpu) {
    unsigned now = gpu->generation();
    if (now == s_generation) return;
    s_generation = now;
    for (int i = 0; i < LX_DDRAW_OBJECTS; i++) {
        s_textures[i].id = 0;
        s_textures[i].stale = 1;
    }
    for (int i = 0; i < TARGETS; i++)
        if (s_targets[i].device) tsync_gpu_gone(&s_targets[i].sync);
    forget_waves();
    s_front_from_gpu = -1;
}

static const lx_gpu_ops_t* begin(const target_t* t) {
    const lx_gpu_ops_t* gpu = lx_d3d_gpu();
    if (!gpu || !gpu->begin(t->w, t->h)) return NULL;
    follow_generation(gpu);
    return gpu;
}

/* ---- the hybrid picture ---- */

static d3dt_rect_t whole(const target_t* t) {
    d3dt_rect_t r = { 0, 0, (long)t->w, (long)t->h };
    return r;
}

static int copy_of(const target_t* t, lx_surface_view_t* v) {
    return lx_ddraw_surface_view(t->slot, v) && v->bits && v->bytes == 2 && v->width == t->w && v->height == t->h;
}

static d3dt_rect_t clipped(const target_t* t, const d3dt_rect_t* r) {
    d3dt_rect_t c = *r;
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    if (c.right > (long)t->w) c.right = (long)t->w;
    if (c.bottom > (long)t->h) c.bottom = (long)t->h;
    return c;
}

static void fill_copy(target_t* t, const d3dt_rect_t* r, uint16_t value) {
    lx_surface_view_t v;
    if (copy_of(t, &v)) tsync_fill(v.bits, v.pitch, t->reference, t->w, r, value);
}

static void capture_tiles(void* context, const d3dt_rect_t* r) { fill_copy((target_t*)context, r, TSYNC_UNWRITTEN); }

static jpt_render_rect_t as_rect(const d3dt_rect_t* r) {
    jpt_render_rect_t out = { (int32_t)r->left, (int32_t)r->top, (uint32_t)(r->right - r->left), (uint32_t)(r->bottom - r->top) };
    return out;
}

static void sync_readback(target_t* t) {
    const lx_gpu_ops_t* gpu = begin(t);
    lx_surface_view_t v;
    d3dt_rect_t all = whole(t);
    jpt_render_rect_t rect = as_rect(&all);
    uint16_t* pixels;
    if (!gpu || !copy_of(t, &v)) return;
    pixels = (uint16_t*)grow(&s_read_buf, (size_t)t->w * t->h * 2u);
    if (!pixels || !gpu->read_scene(&rect, t->w, t->h, pixels, t->w)) return;
    for (uint32_t y = 0; y < t->h; y++) memcpy(v.bits + (size_t)y * (size_t)v.pitch, pixels + (size_t)y * t->w, (size_t)t->w * 2u);
    memcpy(t->reference, pixels, (size_t)t->w * t->h * 2u);
    s_stats.readbacks++;
}

/* A polygon that reads what is under it takes its box from the GPU; what the CPU wrote there stays. */
static void sync_readback_box(target_t* t, const d3dt_rect_t* r) {
    const lx_gpu_ops_t* gpu = begin(t);
    lx_surface_view_t v;
    jpt_render_rect_t rect;
    uint16_t* pixels;
    if (!gpu || d3dt_rect_empty(r) || !copy_of(t, &v)) return;
    rect = as_rect(r);
    pixels = (uint16_t*)grow(&s_read_buf, (size_t)rect.w * rect.h * 2u);
    if (!pixels || !gpu->read_scene(&rect, t->w, t->h, pixels, rect.w)) return;
    tsync_fill_unwritten(v.bits, v.pitch, t->reference, t->w, r, pixels, rect.w);
    s_stats.readbacks++;
}

static void composite_region(target_t* t, const lx_gpu_ops_t* gpu, const lx_surface_view_t* v, const d3dt_rect_t* written) {
    d3dt_rect_t region = d3dt_rect_empty(written) ? whole(t) : clipped(t, written), box, apron;
    tsync_counts_t counts = { 0, 0 };
    jpt_render_rect_t rect;
    if (!tsync_changed_box(v->bits, v->pitch, t->reference, t->w, &region, &box)) return;
    apron = d3dt_overlay_upload_rect(&box, t->w, t->h);
    rect = as_rect(&apron);
    tsync_overlay_box(v->bits, v->pitch, t->reference, t->w, &apron, t->scratch, &counts);
    gpu->overlay(t->scratch, rect.w, &rect, t->w, t->h, 0);
    s_stats.composites++;
}

static void sync_composite(target_t* t) {
    const d3dt_dirty_t written = t->dirty;
    const d3dt_rect_t none = { 0, 0, 0, 0 };
    const lx_gpu_ops_t* gpu = begin(t);
    lx_surface_view_t v;
    memset(&t->dirty, 0, sizeof t->dirty);
    if (!gpu || !copy_of(t, &v)) return;
    if (!written.n) composite_region(t, gpu, &v, &none);
    for (unsigned i = 0; i < written.n; i++) composite_region(t, gpu, &v, &written.r[i]);
}

static void sync_upload(target_t* t) {
    const lx_gpu_ops_t* gpu = begin(t);
    lx_surface_view_t v;
    d3dt_rect_t all = whole(t);
    jpt_render_rect_t rect = as_rect(&all);
    if (!gpu || !copy_of(t, &v)) return;
    for (uint32_t y = 0; y < t->h; y++) {
        const uint16_t* row = (const uint16_t*)(const void*)(v.bits + (size_t)y * (size_t)v.pitch);
        uint32_t* out = t->scratch + (size_t)y * t->w;
        for (uint32_t x = 0; x < t->w; x++) out[x] = d3dt_rgb565_to_bgra(row[x]);
        memcpy(t->reference + (size_t)y * t->w, row, (size_t)t->w * 2u);
    }
    gpu->overlay(t->scratch, t->w, &rect, t->w, t->h, 1);
    s_stats.uploads++;
}

static int front_of(const target_t* t, lx_surface_view_t* back, lx_surface_view_t* front) {
    return copy_of(t, back) && back->root >= 0 && lx_ddraw_surface_view(back->root, front) && front->bits && front->bytes == 2 &&
           front->width == t->w && front->height == t->h;
}

static void sync_copy_front(target_t* t) {
    lx_surface_view_t v, front;
    if (!front_of(t, &v, &front)) return;
    for (uint32_t y = 0; y < t->h; y++) {
        const uint8_t* in = front.bits + (size_t)y * (size_t)front.pitch;
        memcpy(v.bits + (size_t)y * (size_t)v.pitch, in, (size_t)t->w * 2u);
        memcpy(t->reference + (size_t)y * t->w, in, (size_t)t->w * 2u);
    }
}

static void apply(target_t* t, unsigned acts, const d3dt_rect_t* region) {
    d3dt_rect_t all = whole(t), r = all;
    if (acts & TSYNC_COMPOSITE) sync_composite(t);
    if (acts & TSYNC_UPLOAD) sync_upload(t);
    if (acts & TSYNC_READBACK) sync_readback(t);
    if (acts & TSYNC_COPY_FRONT) sync_copy_front(t);
    if (acts & TSYNC_SYNTHESIZE) fill_copy(t, &all, t->sync.color);
    if (acts & (TSYNC_CAPTURE | TSYNC_CAPTURE_ALL)) {
        if (!(acts & TSYNC_CAPTURE_ALL) && region) r = clipped(t, region);
        fill_copy(t, &r, TSYNC_UNWRITTEN);
    }
}

static void check_front(target_t* t) {
    lx_surface_view_t v, front;
    if (t->sync.front_mirror && !front_of(t, &v, &front)) tsync_front_changed(&t->sync);
}

static void gpu_write(target_t* t) { apply(t, tsync_gpu_draw(&t->sync), NULL); }

static d3dt_rect_t clip_to_lock(const lx_surface_view_t* v, d3dt_rect_t box) {
    if (box.left < v->lock_rect[0]) box.left = v->lock_rect[0];
    if (box.top < v->lock_rect[1]) box.top = v->lock_rect[1];
    if (box.right > v->lock_rect[2]) box.right = v->lock_rect[2];
    if (box.bottom > v->lock_rect[3]) box.bottom = v->lock_rect[3];
    if (box.right < box.left) box.right = box.left;
    if (box.bottom < box.top) box.bottom = box.top;
    return box;
}

static void add_polygon(target_t* t, const d3dt_rect_t* box, int reads) {
    d3dt_dirty_add(&s_polys.dirty, box);
    s_stats.cpu_polygons++;
    if (d3dt_rect_empty(box)) return;
    if (s_polys.captured) {
        tsync_tiles_claim(&s_polys.tiles, box, capture_tiles, t);
        if (reads) sync_readback_box(t, box);
    }
}

static void begin_polygon_run(target_t* t) {
    unsigned acts;
    check_front(t);
    acts = tsync_cpu_access(&t->sync, TSYNC_ACCESS_NO_READ);
    apply(t, acts & ~(unsigned)(TSYNC_CAPTURE | TSYNC_CAPTURE_ALL), NULL);
    s_polys.target = t;
    s_polys.captured = !(t->sync.valid & TSYNC_CPU);
    memset(&s_polys.dirty, 0, sizeof s_polys.dirty);
    if (s_polys.captured && (!tsync_tiles_reset(&s_polys.tiles, t->w, t->h) || (acts & TSYNC_CAPTURE_ALL))) {
        d3dt_rect_t all = whole(t);
        fill_copy(t, &all, TSYNC_UNWRITTEN);
        tsync_tiles_claim_all(&s_polys.tiles);
    }
}

static void polygon_lock(target_t* t, const lx_surface_view_t* v) {
    d3dt_rect_t box = clip_to_lock(v, s_polys.box);
    int reads = s_polys.reads;
    s_polys.pending = 0;
    begin_polygon_run(t);
    add_polygon(t, &box, reads);
}

static int flush_polygon_run(target_t* t) {
    const d3dt_dirty_t polygons = s_polys.dirty;
    s_polys.target = NULL;
    if (!polygons.n) return 0;
    tsync_cpu_wrote(&t->sync);
    for (unsigned i = 0; i < polygons.n; i++) d3dt_dirty_add(&t->dirty, &polygons.r[i]);
    return 1;
}

void lx_d3d_cpu_polygon(uint32_t surface_iface, const long raster_box[4], uint32_t origin_bytes, int reads) {
    int slot = lx_ddraw_surface_from(surface_iface);
    target_t* t = slot >= 0 ? target_of(slot) : NULL;
    lx_surface_view_t v;
    long at[4] = { raster_box[0], raster_box[1], raster_box[2], raster_box[3] };
    uint32_t caller = s_lock_caller ? s_lock_caller() : 0;
    if (!t || !copy_of(t, &v)) return;
    if (caller && !target_access_reads(CPU_ACCESS_LOCK, 0, caller, 1)) return;
    cpu_polygon_to_surface(at, origin_bytes, v.pitch);
    s_polys.box.left = at[0], s_polys.box.top = at[1], s_polys.box.right = at[2], s_polys.box.bottom = at[3];
    s_polys.reads = reads && !(jpt_active_lift() == JPT_LIFT_116 && cpu_polygon_store_only_caller(caller) >= 0);
    if (!v.locked) {
        s_polys.pending = 1;
    } else if (s_polys.target == t) {
        d3dt_rect_t box = clip_to_lock(&v, s_polys.box);
        add_polygon(t, &box, s_polys.reads);
    } else if (t->inert_lock) {
        t->inert_lock = 0;
        polygon_lock(t, &v);
    }
}

void lx_d3d_cpu_polygon_done(void) { s_polys.pending = 0; }

/* ---- what the DirectDraw surfaces say ---- */

static void target_access(target_t* t, cpu_access_t access, int water_lock) {
    const uint32_t caller = s_lock_caller ? s_lock_caller() : 0;
    d3dt_rect_t region = whole(t);
    lx_surface_view_t v;
    int reads;
    if (!copy_of(t, &v)) return;
    if (access == CPU_ACCESS_LOCK && (water_lock || (caller && caller == LOCK_CALLER_FLUSH_BATCH))) {
        t->inert_lock = 1;
        return;
    }
    if (access == CPU_ACCESS_LOCK && s_polys.pending) {
        polygon_lock(t, &v);
        return;
    }
    if (access == CPU_ACCESS_LOCK) {
        region.left = v.lock_rect[0], region.top = v.lock_rect[1], region.right = v.lock_rect[2], region.bottom = v.lock_rect[3];
    }
    /* The Linux text rasteriser blends its edges, so a DC always needs what is under the text. */
    reads = access == CPU_ACCESS_GETDC || !caller ? access != CPU_ACCESS_BLT_DEST : target_access_reads(access, v.lock_flags, caller, 0);
    check_front(t);
    apply(t, tsync_cpu_access(&t->sync, reads ? TSYNC_ACCESS_READ : TSYNC_ACCESS_NO_READ), &region);
}

static void read_last_frame(int slot) {
    const lx_gpu_ops_t* gpu = lx_d3d_gpu();
    lx_surface_view_t v;
    uint16_t* pixels;
    if (!gpu || target_of(slot) || !lx_ddraw_surface_view(slot, &v) || !v.bits || v.bytes != 2) return;
    pixels = (uint16_t*)grow(&s_read_buf, (size_t)v.width * v.height * 2u);
    if (!pixels || !gpu->read_last(pixels, v.width, v.height, v.width)) return;
    for (uint32_t y = 0; y < v.height; y++)
        memcpy(v.bits + (size_t)y * (size_t)v.pitch, pixels + (size_t)y * v.width, (size_t)v.width * 2u);
    s_stats.readbacks++;
}

void lx_d3d_cpu_access(int slot, cpu_access_t access) {
    target_t* t;
    int water_lock = 0;
    if (!lx_d3d_gpu()) return;
    t = target_of(slot);
    if (access == CPU_ACCESS_LOCK) {
        const uint32_t caller = s_lock_caller ? s_lock_caller() : 0;
        if (!caller || caller != LOCK_CALLER_FLUSH_BATCH) {
            water_lock = s_water_lock && t;
            s_water_lock = 0;
        }
    }
    if (t) target_access(t, access, water_lock);
    if (s_front_from_gpu == slot) {
        s_front_from_gpu = -1;
        read_last_frame(slot);
    }
}

static void front_written(int slot) {
    lx_surface_view_t v;
    for (int i = 0; i < TARGETS; i++)
        if (s_targets[i].device && lx_ddraw_surface_view(s_targets[i].slot, &v) && v.root == slot) tsync_front_changed(&s_targets[i].sync);
}

void lx_d3d_cpu_written(int slot, const int32_t* rect) {
    target_t* t;
    lx_surface_view_t v;
    d3dt_rect_t r;
    if (!lx_d3d_gpu() || !lx_ddraw_surface_view(slot, &v)) return;
    r.left = rect ? rect[0] : 0;
    r.top = rect ? rect[1] : 0;
    r.right = rect ? rect[2] : (long)v.width;
    r.bottom = rect ? rect[3] : (long)v.height;
    front_written(slot);
    t = target_of(slot);
    if (t && s_polys.target == t) {
        flush_polygon_run(t);
    } else if (t) {
        tsync_cpu_wrote(&t->sync);
        d3dt_dirty_add(&t->dirty, &r);
    }
    if (v.caps & DDSCAPS_TEXTURE) {
        if (!rect) s_textures[slot].stale = 1;
        d3dt_rect_union(&s_textures[slot].dirty, &r);
    }
}

int lx_d3d_unlocked(int slot) {
    target_t* t = target_of(slot);
    if (!t || !t->inert_lock) return 0;
    t->inert_lock = 0;
    return 1;
}

int lx_d3d_fill(int slot, const int32_t rect[4], uint32_t color) {
    lx_surface_view_t v;
    target_t* t = target_of(slot);
    if (!t || !copy_of(t, &v)) return 0;
    return lx_target_clear(slot, 1, rect, LX_D3DCLEAR_TARGET, d3dt_rgb565_to_bgra((uint16_t)color), 0.0f) == LX_D3D_OK;
}

void lx_d3d_surface_freed(int slot) {
    const lx_gpu_ops_t* gpu = lx_d3d_gpu();
    lx_surface_view_t v;
    texture_t* e = &s_textures[slot];
    if (lx_ddraw_surface_view(slot, &v))
        fx_water_surface_freed((const void*)(uintptr_t)(slot + 1), (uintptr_t)v.bits,
                               v.bits ? (uintptr_t)v.bits + (uintptr_t)v.pitch * v.height : 0);
    for (unsigned i = 0; i < LX_GPU_WAVE_SLOTS; i++)
        if (s_waves[i].drawn == slot) s_waves[i].drawn = -1;
    if (gpu && e->id) gpu->texture_destroy(e->id);
    memset(e, 0, sizeof *e);
    if (s_front_from_gpu == slot) s_front_from_gpu = -1;
}

/* ---- the device ---- */

int lx_target_attach(lx_device_t* dev) {
    target_t* t = NULL;
    lx_surface_view_t v;
    if (target_of(dev->target) || !lx_ddraw_surface_view(dev->target, &v)) return 0;
    for (int i = 0; i < TARGETS && !t; i++)
        if (!s_targets[i].device) t = &s_targets[i];
    if (!t) return 0;
    memset(t, 0, sizeof *t);
    t->slot = dev->target;
    t->w = v.width;
    t->h = v.height;
    t->reference = (uint16_t*)calloc((size_t)t->w * t->h, sizeof(uint16_t));
    t->scratch = (uint32_t*)calloc((size_t)t->w * t->h, sizeof(uint32_t));
    if (!t->reference || !t->scratch) {
        free_target(t);
        return 0;
    }
    t->device = dev;
    tsync_attach(&t->sync);
    if (v.bits && v.bytes == 2)
        for (uint32_t y = 0; y < t->h; y++)
            memcpy(t->reference + (size_t)y * t->w, v.bits + (size_t)y * (size_t)v.pitch, (size_t)t->w * 2u);
    return 1;
}

void lx_target_detach(lx_device_t* dev) {
    target_t* t = target_of(dev->target);
    if (!t || t->device != dev) return;
    if (s_polys.target == t) s_polys.target = NULL;
    s_polys.pending = 0;
    apply(t, tsync_gpu_release(&t->sync), NULL);
    free_target(t);
}

void lx_target_begin_scene(lx_device_t* dev) {
    target_t* t = target_of(dev->target);
    if (t) begin(t);
}

/* ---- textures ---- */

static int format_of(const lx_surface_view_t* v) {
    return d3dt_format_from_pixelformat(v->pf[1], v->pf[3], v->pf[4], v->pf[5], v->pf[6], v->pf[7]);
}

static uint32_t palette_bgra(const lx_surface_view_t* v, uint32_t out[PALETTE_ENTRIES]) {
    uint32_t sum = 2166136261u;
    for (uint32_t i = 0; i < PALETTE_ENTRIES; i++) {
        uint32_t e = v->palette ? v->palette[i] : 0;
        out[i] = 0xFF000000u | (e & 0xFFu) << 16 | (e & 0xFF00u) | ((e >> 16) & 0xFFu);
        sum = (sum ^ out[i]) * 16777619u;
    }
    return sum;
}

static int upload(const lx_gpu_ops_t* gpu, texture_t* e, const lx_surface_view_t* v, int fmt, const jpt_render_rect_t* rect,
                  const uint32_t* palette) {
    const size_t texels = (size_t)v->width * v->height;
    const size_t chain = e->levels > 1 ? d3dt_mip_chain_texels(v->width, v->height, e->levels) : texels;
    const uint32_t x0 = rect ? (uint32_t)rect->x : 0, y0 = rect ? (uint32_t)rect->y : 0;
    const uint32_t w = rect ? rect->w : v->width, h = rect ? rect->h : v->height;
    const uint32_t low = v->src_key[0], high = v->src_key[1];
    jpt_render_tex_desc_t desc;
    uint32_t* rows = (uint32_t*)grow(&s_texel_buf, chain * 4u);
    if (!rows) return 0;
    for (uint32_t y = y0; y < y0 + h; y++) {
        const uint8_t* in = v->bits + (size_t)y * (size_t)v->pitch + (size_t)x0 * v->bytes;
        uint32_t* out = rows + (size_t)y * v->width + x0;
        if (fmt == D3DT_FMT_PAL8) d3dt_convert_row_pal8(in, out, w, palette, v->has_src_key, low, high);
        else d3dt_convert_row(fmt, in, out, w, v->has_src_key, low, high);
    }
    if (e->levels > 1) d3dt_build_mips(rows, v->width, v->height, e->levels, d3dt_alpha_kind(rows, texels));
    memset(&desc, 0, sizeof desc);
    desc.width = v->width;
    desc.height = v->height;
    desc.pitch = v->width * 4u;
    desc.format = JPT_TEXFMT_BGRA8888_PREPARED;
    desc.has_alpha = d3dt_format_has_alpha(fmt);
    desc.mip_levels = e->levels > 1 ? e->levels : 0;
    desc.has_colorkey = v->has_src_key;
    desc.colorkey_lo = low;
    desc.colorkey_hi = high;
    s_stats.texture_uploads++;
    if (!e->id || !gpu->texture_alive(e->id)) {
        e->id = gpu->texture_create(&desc, rows);
        return e->id != 0;
    }
    if (rect) s_stats.partial_uploads++;
    return gpu->texture_update(e->id, &desc, rows, rect);
}

static unsigned levels_for(const lx_gpu_ops_t* gpu, const texture_t* e, const lx_surface_view_t* v) {
    return !e->dynamic && gpu->uses_mips() ? d3dt_mip_count(v->width, v->height) : 1;
}

/* The surface's texture on the GPU, brought up to date; 0 when it cannot be drawn. */
static uint64_t texture_for(const lx_gpu_ops_t* gpu, int slot) {
    texture_t* e = &s_textures[slot];
    uint32_t palette[PALETTE_ENTRIES], sum = 0;
    lx_surface_view_t v;
    jpt_render_rect_t box;
    int fmt, full, partial;
    if (!lx_ddraw_surface_view(slot, &v) || !v.bits) return 0;
    fmt = format_of(&v);
    if (fmt == D3DT_FMT_UNKNOWN) return 0;
    if (fmt == D3DT_FMT_PAL8) sum = palette_bgra(&v, palette);
    if (!e->id) e->levels = levels_for(gpu, e, &v);
    full = e->stale || !e->id || e->format != fmt || e->keyed != v.has_src_key || e->palette_sum != sum ||
           (v.has_src_key && (e->key_low != v.src_key[0] || e->key_high != v.src_key[1]));
    if (!full && d3dt_rect_empty(&e->dirty)) return e->id;
    if (e->levels > 1 && !full && d3dt_note_upload(&e->history, s_stats.gpu_frames)) {
        gpu->texture_destroy(e->id);
        e->id = 0;
        e->dynamic = 1;
        e->levels = 1;
        full = 1;
        s_stats.dynamic_textures++;
    }
    partial = !full && e->levels <= 1;
    if (partial) {
        d3dt_rect_t d = e->dirty;
        if (d.left < 0) d.left = 0;
        if (d.top < 0) d.top = 0;
        if (d.right > (long)v.width) d.right = (long)v.width;
        if (d.bottom > (long)v.height) d.bottom = (long)v.height;
        partial = !d3dt_rect_empty(&d);
        if (partial) box = as_rect(&d);
    }
    if (!upload(gpu, e, &v, fmt, partial ? &box : NULL, palette)) {
        e->stale = 1;
        return 0;
    }
    e->format = fmt;
    e->keyed = v.has_src_key;
    e->key_low = v.src_key[0];
    e->key_high = v.src_key[1];
    e->palette_sum = sum;
    e->stale = 0;
    memset(&e->dirty, 0, sizeof e->dirty);
    return e->id;
}

void lx_target_texture_loaded(int dst, int src) {
    lx_surface_view_t v;
    if (!lx_ddraw_surface_view(src, &v) || !v.bits) return;
    lx_d3d_cpu_access(src, CPU_ACCESS_BLT_SOURCE);
    lx_d3d_cpu_access(dst, CPU_ACCESS_BLT_DEST);
    fx_water_loaded((const void*)(uintptr_t)(dst + 1), (const void*)(uintptr_t)(src + 1), (uintptr_t)v.bits, v.pitch, (int)v.width,
                    (int)v.height, (int)v.bytes);
}

/* ---- draws ---- */

/* Each water texture keeps its own wave field. */
static unsigned wave_slot_for(int drawn) {
    unsigned at = 0;
    for (unsigned i = 0; i < LX_GPU_WAVE_SLOTS; i++) {
        if (s_waves[i].drawn == drawn) {
            at = i;
            break;
        }
        if (s_waves[i].used < s_waves[at].used) at = i;
    }
    s_waves[at].used = ++s_wave_clock;
    return at;
}

static int water_draw(const lx_gpu_ops_t* gpu, int texture, const int32_t vp[4], uint32_t rt_w, uint32_t rt_h) {
    const fx_water_link_t* link = fx_water_link((const void*)(uintptr_t)(texture + 1));
    const float* waves = NULL;
    float bounds[4];
    unsigned serial, slot;
    float* buf;
    if (!link || link->width <= 0 || link->height <= 0) return 0;
    buf = (float*)grow(&s_wave_buf, (size_t)link->width * (size_t)link->height * 2u * sizeof(float));
    if (!buf) return 0;
    serial = fx_water_serial();
    slot = wave_slot_for(texture);
    if (s_waves[slot].drawn != texture || s_waves[slot].serial != serial) {
        fx_water_fill(link->bits, link->pitch, link->width, link->height, buf, NULL);
        s_waves[slot].drawn = texture;
        s_waves[slot].serial = serial;
        waves = buf;
        s_stats.wave_uploads++;
    }
    fx_water_scene_bounds(vp[0], vp[1], vp[2], vp[3], (int)rt_w, (int)rt_h, bounds);
    gpu->select_waves(slot);
    gpu->water_params(FX_WATER_STRENGTH, FX_WATER_RHW_METRE, bounds, waves, (uint32_t)link->width, (uint32_t)link->height);
    s_stats.water_draws++;
    return 1;
}

static void state_of(const lx_device_t* dev, jpt_render_state_t* st) {
    const uint32_t* r = dev->render;
    const uint32_t* s = dev->stage[0];
    memset(st, 0, sizeof *st);
    st->z_enable = r[LX_RS_ZENABLE];
    st->z_write = r[LX_RS_ZWRITEENABLE];
    st->z_func = r[LX_RS_ZFUNC];
    st->z_bias = r[LX_RS_ZBIAS];
    st->alpha_blend = r[LX_RS_ALPHABLENDENABLE];
    st->src_blend = r[LX_RS_SRCBLEND];
    st->dst_blend = r[LX_RS_DESTBLEND];
    st->alpha_test = r[LX_RS_ALPHATESTENABLE];
    st->alpha_func = r[LX_RS_ALPHAFUNC];
    st->alpha_ref = d3dt_alpha_ref(r[LX_RS_ALPHAREF]);
    st->fog_enable = r[LX_RS_FOGENABLE];
    st->fog_color = r[LX_RS_FOGCOLOR];
    st->specular_enable = r[LX_RS_SPECULARENABLE];
    st->shade_mode = r[LX_RS_SHADEMODE];
    st->cull_mode = r[LX_RS_CULLMODE];
    st->fill_mode = r[LX_RS_FILLMODE];
    st->colorkey_enable = r[LX_RS_COLORKEYENABLE];
    st->tex_map_blend = r[LX_RS_TEXTUREMAPBLEND];
    st->tex_addr_u = s[LX_TSS_ADDRESSU];
    st->tex_addr_v = s[LX_TSS_ADDRESSV];
    st->tex_mag_filter = s[LX_TSS_MAGFILTER];
    st->tex_min_filter = s[LX_TSS_MINFILTER];
    st->tex_mip_filter = s[LX_TSS_MIPFILTER];
}

static uint32_t skipped(uint32_t hr) {
    s_stats.skipped_draws++;
    return hr;
}

/* Left, top, right and bottom of the device's viewport in target pixels. */
static void viewport_box(const lx_device_t* dev, const target_t* t, uint32_t rt_w, uint32_t rt_h, int32_t out[4]) {
    out[0] = out[1] = 0;
    out[2] = (int32_t)rt_w;
    out[3] = (int32_t)rt_h;
    if (dev->current && dev->current->data_set) {
        const uint32_t* d = dev->current->data;
        d3dt_rect_t r = d3dt_scale_rect((long)(int32_t)d[1], (long)(int32_t)d[2], (long)d[3], (long)d[4], t->w, t->h, rt_w, rt_h);
        if (d3dt_rect_empty(&r)) return;
        out[0] = (int32_t)r.left, out[1] = (int32_t)r.top, out[2] = (int32_t)r.right, out[3] = (int32_t)r.bottom;
    }
}

uint32_t lx_target_draw(lx_device_t* dev, uint32_t type, uint32_t fvf, uint32_t vertices_va, uint32_t count, const uint16_t* indices,
                        uint32_t index_count) {
    target_t* t = target_of(dev->target);
    unsigned stride = 0, at_diffuse = 0, at_specular = 0, at_uv = 0;
    const lx_gpu_ops_t* gpu;
    const uint8_t* in;
    jpt_render_vertex_t* out;
    jpt_render_state_t st;
    uint32_t rt_w = 0, rt_h = 0;
    int32_t vp[4];
    float scale_x, scale_y;
    if (!t || type < D3DT_PT_POINTLIST || type > D3DT_PT_TRIANGLEFAN) return skipped(LX_DDERR_INVALIDPARAMS);
    if (!d3dt_fvf_layout(fvf, &stride, &at_diffuse, &at_specular, &at_uv)) return skipped(LX_DDERR_INVALIDPARAMS);
    if (!dev->in_scene) return skipped(LX_D3DERR_SCENE_NOT_IN_SCENE);
    if (!count || !d3dt_primitive_count((int)type, indices ? index_count : count)) return LX_D3D_OK;
    if (count > 0x100000u) return skipped(LX_DDERR_INVALIDPARAMS);
    in = (const uint8_t*)gm_ptr_checked(vertices_va, count * stride, 0);
    if (!in) return skipped(LX_DDERR_INVALIDPARAMS);
    for (uint32_t i = 0; indices && i < index_count; i++)
        if (indices[i] >= count) return skipped(LX_DDERR_INVALIDPARAMS);
    gpu = begin(t);
    if (!gpu) return skipped(LX_DDERR_GENERIC);
    gpu->target_size(&rt_w, &rt_h);
    if (!rt_w || !rt_h) return skipped(LX_DDERR_GENERIC);
    scale_x = (float)rt_w / (float)t->w;
    scale_y = (float)rt_h / (float)t->h;
    viewport_box(dev, t, rt_w, rt_h, vp);
    gpu->scissor(1, vp[0], vp[1], (uint32_t)(vp[2] - vp[0]), (uint32_t)(vp[3] - vp[1]));
    state_of(dev, &st);
    if (dev->texture[0] >= 0) {
        st.texture_id = texture_for(gpu, dev->texture[0]);
        if (!st.texture_id) return skipped(LX_D3D_OK);
        st.has_texture = 1;
    }
    if (s_sky_drawing && s_sky_on && s_sky_usable) {
        gpu->sky_params(s_sky);
        st.sky_mode = 1;
        s_stats.sky_draws++;
    } else if (s_water_on && dev->texture[0] >= 0 && st.alpha_blend && st.src_blend == BLEND_SRCALPHA && st.dst_blend == BLEND_INVSRCALPHA) {
        st.water_mode = (uint32_t)water_draw(gpu, dev->texture[0], vp, rt_w, rt_h);
    }
    out = (jpt_render_vertex_t*)grow(&s_vertex_buf, (size_t)count * sizeof *out);
    if (!out) return skipped(LX_DDERR_OUTOFMEMORY);
    for (uint32_t i = 0; i < count; i++, in += stride) {
        float f[4];
        memcpy(f, in, sizeof f);
        out[i].sx = f[0] * scale_x;
        out[i].sy = f[1] * scale_y;
        out[i].sz = f[2];
        out[i].rhw = f[3];
        out[i].diffuse = 0xFFFFFFFFu;
        out[i].specular = 0xFF000000u;
        out[i].tu = out[i].tv = 0.0f;
        if (at_diffuse) memcpy(&out[i].diffuse, in + at_diffuse, 4);
        if (at_specular) memcpy(&out[i].specular, in + at_specular, 4);
        if (at_uv) {
            memcpy(&out[i].tu, in + at_uv, 4);
            memcpy(&out[i].tv, in + at_uv + 4, 4);
        }
    }
    gpu_write(t);
    s_stats.draws++;
    gpu->draw(&st, (int)type, out, count, indices, index_count);
    if (st.z_write) t->ao_scene = 1;
    return LX_D3D_OK;
}

uint32_t lx_target_clear(int target_slot, uint32_t count, const int32_t* rects, uint32_t flags, uint32_t color, float z) {
    target_t* t = target_of(target_slot);
    const lx_gpu_ops_t* gpu = t ? begin(t) : NULL;
    const int clear_color = (flags & LX_D3DCLEAR_TARGET) != 0, clear_depth = (flags & LX_D3DCLEAR_ZBUFFER) != 0;
    uint32_t rt_w = 0, rt_h = 0;
    if (!gpu) return LX_DDERR_GENERIC;
    gpu->target_size(&rt_w, &rt_h);
    for (uint32_t i = 0; i < count; i++) {
        const int32_t* r = rects + i * 4u;
        long x = r[0], y = r[1], w = (long)r[2] - r[0], h = (long)r[3] - r[1];
        d3dt_rect_t scaled = d3dt_scale_rect(x, y, w, h, t->w, t->h, rt_w, rt_h);
        if (d3dt_rect_empty(&scaled)) continue;
        if (clear_color && x <= 0 && y <= 0 && x + w >= (long)t->w && y + h >= (long)t->h) {
            tsync_gpu_clear(&t->sync, d3dt_bgra_to_rgb565(color));
            memset(&t->dirty, 0, sizeof t->dirty);
        } else if (clear_color) {
            gpu_write(t);
        }
        gpu->scissor(1, (int32_t)scaled.left, (int32_t)scaled.top, (uint32_t)(scaled.right - scaled.left),
                     (uint32_t)(scaled.bottom - scaled.top));
        gpu->clear(clear_color, color, clear_depth, z);
        s_stats.clears++;
    }
    gpu->scissor(0, 0, 0, rt_w, rt_h);
    return LX_D3D_OK;
}

/* ---- Flip ---- */

static target_t* chain_target(const int* chain, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        target_t* t = target_of(chain[i]);
        if (t) return t;
    }
    return NULL;
}

int lx_d3d_flip(const int* chain, unsigned count) {
    target_t* t = chain_target(chain, count);
    const lx_gpu_ops_t* gpu;
    unsigned acts;
    if (!t || !lx_d3d_gpu()) return 0;
    acts = tsync_present(&t->sync);
    if (!tsync_presents_gpu(&t->sync)) {
        t->ao_scene = 0;
        s_stats.cpu_frames++;
        return 0;
    }
    apply(t, acts, NULL);
    if (s_scene_shown && !s_scene_shown()) {
        apply(t, tsync_gpu_release(&t->sync), NULL);
        memset(&t->dirty, 0, sizeof t->dirty);
        t->ao_scene = 0;
        s_stats.cpu_frames++;
        return 0;
    }
    gpu = begin(t);
    if (gpu) {
        float tan_half = s_tan_half_height ? s_tan_half_height() : 0.0f;
        gpu->ao_params(t->ao_scene ? s_ao_level : 0, tan_half, t->ao_scene);
    }
    return 1;
}

void lx_d3d_flipped(int front, const int* chain, unsigned count) {
    target_t* t = chain_target(chain, count);
    if (!t) return;
    s_front_from_gpu = (t->sync.valid & TSYNC_CPU) ? -1 : front;
    tsync_flipped(&t->sync);
    memset(&t->dirty, 0, sizeof t->dirty);
    t->ao_scene = 0;
    s_stats.gpu_frames++;
}
