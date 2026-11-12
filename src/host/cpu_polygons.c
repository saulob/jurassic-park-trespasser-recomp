/*
 * Polygons Direct3D cannot take (meshes that are not hardware-able, such as bump-mapped ones) are rasterised by the
 * game into the Direct3D frame through CScreenRenderDWI::DrawPolygon. Every primitive copies the polygon into
 * arvRasterVertices before its walker locks the screen, so each Lock call inside DrawPolygon names the polygon's box.
 */
#include "host.h"

#define DRAW_POLYGON_VA 0x0055F540u /* CScreenRenderDWIT<uint16>::DrawPolygon, vtable 0x0064E760 slot 10 */
#define RASTER_LOCK_VA 0x00503400u  /* CRasterVid::Lock: locks the DirectDraw surface unless already locked */
#define RASTER_SURFACE 0x4Cu        /* CRasterVid: IDirectDrawSurface4* */
/* CRasterWin::Lock (vtable 0x0064DB54) calls CRasterVid::Lock, then moves the pixel pointer by iOffsetPointer: the
 * game's view sits inside the surface, and polygon coordinates are relative to the view. */
#define RASTER_WIN_LOCK_VA 0x00504D20u
#define RASTER_WIN_VTABLE 0x0064DB54u
#define RASTER_WIN_ORIGIN 0x90u
#define RASTER_VERTICES 0x00737438u /* arvRasterVertices: screen X at +0xC and Y at +0x10 */
#define RASTER_VERTEX_COUNT 0x00737EECu
#define RASTER_VERTEX_SIZE 0x28u
#define MAX_VERTICES 64u
#define BOX_MARGIN 2
#define NO_LIMIT 0x7FFFFFFFL

/* Primitives whose spans only store pixels (SelfMod 0x00640FE0, 0x006402F0, 0x0063DC60, 0x0063D020 and the flat fill
 * 0x00555450 each write [ebp+edi*2] and never read the screen). Any other primitive reads its box from the GPU. */
static const uint32_t k_store_only[] = {
    0x0057B7B0u, /* bump, linear */
    0x0057DB50u, /* bump, perspective */
    0x005668C0u, /* texture, linear */
    0x00568EF0u, /* texture, perspective */
    0x00563890u, /* flat */
};
#define STORE_ONLY_COUNT (sizeof k_store_only / sizeof k_store_only[0])

static recomp_func_t s_draw_lifted, s_lock_lifted, s_store_only_lifted[STORE_ONLY_COUNT];
static int s_installed, s_drawing, s_store_only;

static int raster_box(long box[4]) {
    float x[MAX_VERTICES], y[MAX_VERTICES];
    uint32_t n = MEM32(RASTER_VERTEX_COUNT);
    if (n < 3 || n > MAX_VERTICES) return 0;
    for (uint32_t i = 0; i < n; i++) {
        x[i] = *(const float*)ADDR(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + 0xC);
        y[i] = *(const float*)ADDR(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + 0x10);
    }
    return cpu_polygon_box(x, y, n, NO_LIMIT, NO_LIMIT, BOX_MARGIN, box);
}

static uint32_t raster_origin(uint32_t raster) {
    return g_cur_func == RASTER_WIN_LOCK_VA && MEM32(raster) == RASTER_WIN_VTABLE ? MEM32(raster + RASTER_WIN_ORIGIN) : 0;
}

static void draw_polygon(void) {
    if (g_alias_entry) {
        s_draw_lifted();
        return;
    }
    s_drawing++;
    s_draw_lifted();
    s_drawing--;
}

static void raster_lock(void) {
    long box[4];
    uint32_t raster = g_ecx;
    if (g_alias_entry || !s_drawing) {
        s_lock_lifted();
        return;
    }
    if (!raster_box(box)) {
        s_lock_lifted();
        return;
    }
    d3dr_cpu_polygon((void*)ADDR(MEM32(raster + RASTER_SURFACE)), box, raster_origin(raster), !s_store_only);
    s_lock_lifted();
    d3dr_cpu_polygon_done();
}

static void store_only(unsigned i) {
    if (g_alias_entry) {
        s_store_only_lifted[i]();
        return;
    }
    s_store_only++;
    s_store_only_lifted[i]();
    s_store_only--;
}

static void store_only_0(void) { store_only(0); }
static void store_only_1(void) { store_only(1); }
static void store_only_2(void) { store_only(2); }
static void store_only_3(void) { store_only(3); }
static void store_only_4(void) { store_only(4); }

void cpu_polygons_install(void) {
    static const recomp_func_t entries[STORE_ONLY_COUNT] = { store_only_0, store_only_1, store_only_2, store_only_3,
                                                             store_only_4 };
    unsigned known = 0;
    s_installed = guest_override_redirect(DRAW_POLYGON_VA, draw_polygon, &s_draw_lifted) &&
                  guest_override_redirect(RASTER_LOCK_VA, raster_lock, &s_lock_lifted);
    if (!s_installed) {
        host_log("renderer", "CScreenRenderDWI::DrawPolygon or CRasterVid::Lock has no incremental-link thunk: software "
                 "polygons in Direct3D frames read and composite the whole screen");
        return;
    }
    for (unsigned i = 0; i < STORE_ONLY_COUNT; i++)
        if (guest_override_redirect(k_store_only[i], entries[i], &s_store_only_lifted[i])) known++;
        else host_log("renderer", "primitive sub_%08X has no incremental-link thunk: its polygons read their box",
                      k_store_only[i]);
    host_log("renderer", "software polygons in Direct3D frames: Locks limited to each polygon's box, %u of %u "
             "store-only primitives known", known, (unsigned)STORE_ONLY_COUNT);
}

