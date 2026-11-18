#ifndef TRESPASSER_TEXTURE_GPU_POLICY_H
#define TRESPASSER_TEXTURE_GPU_POLICY_H

#include <stdint.h>

/* The perspective-textured primitive (TTexturePerspective, SelfMod 0x0063D020) as a GPU polygon: one row of its
 * DrawSubtriangle and the arithmetic its pixel shader repeats. */

/* The left edge of the scanline walk (pscan): 16.16 x, its own remainder, and u/z, v/z, 1/z at pixel x. */
typedef struct {
    int32_t fx, rem;
    float uz, vz, wz;
} tgpu_scan_t;

/* One row of the trapezoid (pdtri). */
typedef struct {
    int32_t y, y_end, line, len, dlen;
} tgpu_rows_t;

/* What each row adds to the edge, and the extra that a remainder carry adds. */
typedef struct {
    int32_t fx_inc, rem_inc;
    float step[3], carry[3];
    int32_t line_pixels;
} tgpu_steps_t;

typedef struct {
    float origin_x, origin_y;
    float u[3], v[3], w[3]; /* value at the origin, per pixel in x, per row in y */
} tgpu_plane_t;

typedef struct {
    int clamped;          /* u in [umin, umax], v in [umin, vmax], v times line; else v << 9 */
    float umin, umax, vmax;
    uint32_t line, umask, tile_mask;
} tgpu_texture_t;

/* Pixels [x0, x1) of the row the scan is on; 0 when the row is empty. */
int tgpu_row_span(const tgpu_scan_t* s, const tgpu_rows_t* r, int32_t* x0, int32_t* x1);
/* The native epilogue of one row: the edge and the trapezoid advance by one row. */
void tgpu_next_row(tgpu_scan_t* s, tgpu_rows_t* r, const tgpu_steps_t* d);
/* The planes through row samples (x0, y0, a0) and (x1, y1, a1) with the per-pixel x gradients; y1 == y0 leaves the
 * y gradient 0. */
void tgpu_plane(tgpu_plane_t* p, float x0, float y0, const float a0[3], float x1, float y1, const float a1[3],
                const float gx[3]);
/* The texel index at texel coordinates (u, v), as the shader computes it. */
uint32_t tgpu_texel_index(const tgpu_texture_t* t, float u, float v);
/* Bytes the texture can address from its base; 0 when the walk is unbounded. */
uint32_t tgpu_texture_bytes(const tgpu_texture_t* t);

/* Where a polygon of the primitive is drawn. The choice is made once, at its first screen Lock, before any span ran:
 * a polygon is drawn wholly by the GPU or wholly by the original CPU path. */
typedef enum { TGPU_OFF, TGPU_PENDING, TGPU_GPU, TGPU_CPU } tgpu_mode_t;

typedef enum {
    TGPU_REASON_NONE,
    TGPU_REASON_RASTER_LOCKED,
    TGPU_REASON_MEMORY_RASTER,
    TGPU_REASON_NO_SURFACE,
    TGPU_REASON_TEXTURE_INVALID,
    TGPU_REASON_TEXTURE_UNBOUNDED,
    TGPU_REASON_TEXTURE_UNREADABLE,
    TGPU_REASON_CLUT_INVALID,
    TGPU_REASON_ALTERNATE_LINES,
    TGPU_REASON_INVALID_SUBDIVISION,
    TGPU_REASON_INVALID_VERTICES,
    TGPU_REASON_TARGET_UNAVAILABLE,
    TGPU_REASON_RESOURCES_UNAVAILABLE,
    TGPU_REASON_UNSUPPORTED_STATE,
    TGPU_REASON_OTHER,
    TGPU_REASON_COUNT
} tgpu_reason_t;

typedef struct {
    int enabled, disabled_after_failure;
    tgpu_mode_t mode;
    int unsafe;
    unsigned long long gpu, cpu, dropped;
    unsigned long long reason[TGPU_REASON_COUNT];
} tgpu_session_t;

void tgpu_session_init(tgpu_session_t* s, int enabled);
/* A polygon starts: PENDING while the path is on, else OFF (the CPU path untouched). */
tgpu_mode_t tgpu_polygon_begin(tgpu_session_t* s);
/* The first screen Lock of a PENDING polygon decides it (NONE: the GPU; else the CPU, counted by reason); later Locks
 * keep the decision. A polygon that never Locks the screen ends as a memory raster polygon. */
tgpu_mode_t tgpu_polygon_lock(tgpu_session_t* s, tgpu_reason_t reason);
/* Something the preflight did not foresee happened after the GPU decision. */
void tgpu_polygon_unsafe(tgpu_session_t* s);
/* The polygon ended; drawn is the GPU draw's result. 1 when this polygon turned the path off for the session. */
int tgpu_polygon_end(tgpu_session_t* s, int drawn);
const char* tgpu_reason_name(tgpu_reason_t reason);

/* A GPU polygon against the surface's CPU polygons: an unlocked surface is drawn into at once; one the game holds
 * locked keeps its run when no pixel the CPU wrote lies under the polygon, else the run's pixels reach the target
 * first (split); a Lock nobody touched is taken over; any other Lock keeps the polygon on the CPU. */
typedef enum { TGPU_RUN_FRESH, TGPU_RUN_KEEP, TGPU_RUN_SPLIT, TGPU_RUN_ADOPT, TGPU_RUN_BUSY } tgpu_run_t;
tgpu_run_t tgpu_run_action(int locked, int run_active, int lock_untouched, int run_captured, int overlaps);

/* The bump-mapped perspective span (SelfMod 0x006402F0): a 16-bit texel is an angle (bits 0-9, less the light's theta)
 * into a 1024-byte intensity table and a CLUT row (bits 10-15), pixel = clut[intensity * 64 + (texel >> 10)]. */
#define TGPU_BUMP_TABLE_BYTES 1024u
#define TGPU_BUMP_ROW_ENTRIES 64u
/* CLUT entries the table can reach: (largest intensity + 1) rows. */
uint32_t tgpu_bump_clut_entries(const uint8_t table[TGPU_BUMP_TABLE_BYTES]);
uint32_t tgpu_bump_clut_index(uint32_t texel, uint32_t theta, const uint8_t table[TGPU_BUMP_TABLE_BYTES]);

/* The texel and CLUT caches of the GPU path. A texture is identified by its address and size and re-checked by hash
 * once per frame; a CLUT by its address and its 256 entries. Device and level changes empty both. */
#define TGPU_TEXTURE_SLOTS 2048u
#define TGPU_CLUT_SLOTS 2048u
#define TGPU_CLUT_ENTRIES 256u

typedef struct {
    struct {
        const uint8_t* src;
        uint32_t bytes, offset, hash;
        unsigned long long frame;
    } slot[TGPU_TEXTURE_SLOTS];
    uint32_t top, pool;
} tgpu_texture_cache_t;

typedef struct {
    struct {
        const uint16_t* src;
        uint16_t copy[TGPU_CLUT_ENTRIES];
    } slot[TGPU_CLUT_SLOTS];
} tgpu_clut_cache_t;

void tgpu_texture_cache_reset(tgpu_texture_cache_t* c, uint32_t pool);
/* The texture's first byte in the pool, or -1 when it cannot fit; *upload is set when its bytes must be sent. */
int64_t tgpu_texture_cache_find(tgpu_texture_cache_t* c, const uint8_t* src, uint32_t bytes, unsigned long long frame,
                                int* upload);
/* The same for contents that can change inside a frame: checked at every use. */
int64_t tgpu_texture_cache_find_checked(tgpu_texture_cache_t* c, const uint8_t* src, uint32_t bytes, int* upload);
void tgpu_clut_cache_reset(tgpu_clut_cache_t* c);
/* The CLUT's slot; *upload is set when its entries must be sent. */
uint32_t tgpu_clut_cache_find(tgpu_clut_cache_t* c, const uint16_t* src, int* upload);

#endif
