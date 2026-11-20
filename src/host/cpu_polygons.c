/*
 * Polygons Direct3D cannot take (meshes that are not hardware-able, such as bump-mapped ones) are rasterised by the
 * game into the Direct3D frame through CScreenRenderDWI::DrawPolygon. Every primitive copies the polygon into
 * arvRasterVertices before its walker locks the screen, so each Lock call inside DrawPolygon names the polygon's box.
 */
#include "guest_va.h"
#include "host.h"

#include "perf_sections.h"
#include "texture_gpu_policy.h"
#include "ddraw/perf_stat.h"

#define DRAW_POLYGON_VA GVA(0x0055F540) /* CScreenRenderDWIT<uint16>::DrawPolygon, vtable 0x0064E760 slot 10 */
#define RASTER_LOCK_VA GVA(0x00503400)  /* CRasterVid::Lock: locks the DirectDraw surface unless already locked */
#define RASTER_SURFACE 0x4Cu        /* CRasterVid: IDirectDrawSurface4* */
/* CRasterWin::Lock (vtable 0x0064DB54) calls CRasterVid::Lock, then moves the pixel pointer by iOffsetPointer: the
 * game's view sits inside the surface, and polygon coordinates are relative to the view. */
#define RASTER_WIN_LOCK_VA GVA(0x00504D20)
#define RASTER_WIN_VTABLE GVA(0x0064DB54)
#define RASTER_WIN_ORIGIN 0x90u
#define RASTER_HEIGHT 0x0Cu
#define RASTER_VERTICES GVA(0x00737438) /* arvRasterVertices: screen X at +0xC and Y at +0x10 */
#define RASTER_VERTEX_COUNT GVA(0x00737EEC)
#define RASTER_VERTEX_SIZE 0x28u
#define MAX_VERTICES 64u
#define BOX_MARGIN 2
#define NO_LIMIT 0x7FFFFFFFL
#define SCREEN_RENDER_RASTER 0x04u /* CScreenRenderDWI: the raster DrawPolygon draws into */
/* The 1.0.116 address of sub_00555060's 1998 limit (-50.0f in .rdata, read by nothing else). */
#define GRADIENT_LIMIT_LIFT116 0x0064E6C4u

/* Primitives whose spans only store pixels (SelfMod 0x00640FE0, 0x006402F0, 0x0063DC60, 0x0063D020 and the flat fill
 * 0x00555450 each write [ebp+edi*2] and never read the screen). Any other primitive reads its box from the GPU. */
static const unsigned k_store_only[] = {
    GVA_ID(0x0057B7B0), /* bump, linear */
    GVA_ID(0x0057DB50), /* bump, perspective */
    GVA_ID(0x005668C0), /* texture, linear */
    GVA_ID(0x00568EF0), /* texture, perspective */
    GVA_ID(0x00563890), /* flat */
};
#define STORE_ONLY_COUNT (sizeof k_store_only / sizeof k_store_only[0])

static recomp_func_t s_draw_lifted, s_lock_lifted, s_store_only_lifted[STORE_ONLY_COUNT];
static uint32_t s_screen_render;
enum { POLY_TEXTURE, POLY_BUMP, POLY_KINDS };
static tgpu_session_t s_gpu[POLY_KINDS];
static tgpu_session_t* s_cur;
static unsigned s_cur_kind;
static int s_installed, s_drawing, s_store_only;
static volatile float* s_gradient_limit;

/* DrawPolygon's whole cost by the function that locked the screen (0: the polygon went to Direct3D). */
#define COST_SLOTS 24u
static struct {
    uint32_t caller;
    unsigned long long polygons, pixels;
    double ms;
    /* this frame */
    unsigned long long frame_polygons, frame_pixels;
    double frame_ms;
    /* stable gameplay frames, and those over 25 ms */
    unsigned long long gameplay_pixels, slow_frames, slow_polygons, slow_pixels;
    perf_stat_t slow_ms;
} s_cost[COST_SLOTS];
static unsigned long long s_gameplay_frames, s_slow_frames;
static uint32_t s_draw_caller;
static unsigned long long s_draw_pixels;
static double s_qpc_ms;

/* DrawPolygon cost by the polygon's material pointer, with the screen Lock caller that drew it. */
#define KEY_SLOTS 48u
static struct {
    uint32_t key, caller;
    unsigned long long polygons, pixels, frame_polygons, slow_polygons;
    double ms, frame_ms, slow_ms;
} s_key[KEY_SLOTS];
static uint32_t s_draw_key;

static void charge_key(uint32_t key, uint32_t caller, unsigned long long pixels, double ms) {
    unsigned i = 0;
    while (i < KEY_SLOTS - 1 && s_key[i].polygons && s_key[i].key != key) i++;
    s_key[i].key = key;
    s_key[i].caller = caller;
    s_key[i].polygons++;
    s_key[i].pixels += pixels;
    s_key[i].ms += ms;
    s_key[i].frame_polygons++;
    s_key[i].frame_ms += ms;
}

static void charge_polygon(uint32_t caller, unsigned long long pixels, double ms) {
    unsigned i = 0;
    while (i < COST_SLOTS && s_cost[i].polygons && s_cost[i].caller != caller) i++;
    if (i == COST_SLOTS) i = COST_SLOTS - 1;
    s_cost[i].caller = caller;
    s_cost[i].polygons++;
    s_cost[i].pixels += pixels;
    s_cost[i].ms += ms;
    s_cost[i].frame_polygons++;
    s_cost[i].frame_pixels += pixels;
    s_cost[i].frame_ms += ms;
}

void cpu_polygons_frame_end(int gameplay, int slow) {
    s_gameplay_frames += gameplay != 0;
    s_slow_frames += gameplay && slow;
    for (unsigned i = 0; i < KEY_SLOTS && s_key[i].polygons; i++) {
        if (gameplay && slow) {
            s_key[i].slow_polygons += s_key[i].frame_polygons;
            s_key[i].slow_ms += s_key[i].frame_ms;
        }
        s_key[i].frame_polygons = 0;
        s_key[i].frame_ms = 0.0;
    }
    for (unsigned i = 0; i < COST_SLOTS && s_cost[i].polygons; i++) {
        if (gameplay) s_cost[i].gameplay_pixels += s_cost[i].frame_pixels;
        if (gameplay && slow && s_cost[i].frame_polygons) {
            s_cost[i].slow_frames++;
            s_cost[i].slow_polygons += s_cost[i].frame_polygons;
            s_cost[i].slow_pixels += s_cost[i].frame_pixels;
            perf_stat_add(&s_cost[i].slow_ms, s_cost[i].frame_ms);
        }
        s_cost[i].frame_polygons = s_cost[i].frame_pixels = 0;
        s_cost[i].frame_ms = 0.0;
    }
}

static void slow_frame_families_summary(void) {
    char line[1800] = "";
    int n = 0, used[COST_SLOTS] = { 0 };
    for (unsigned k = 0; k < 10; k++) {
        int best = -1;
        for (unsigned i = 0; i < COST_SLOTS; i++)
            if (s_cost[i].slow_frames && !used[i] && (best < 0 || s_cost[i].slow_ms.sum > s_cost[best].slow_ms.sum)) best = (int)i;
        if (best < 0) break;
        used[best] = 1;
        {
            double f = (double)s_cost[best].slow_frames;
            n += _snprintf(line + n, sizeof line - 1 - (size_t)n, "%s%s%08X: %llu frames, %.0f polygons/frame, %.0f "
                           "px/frame, %.2f ms/frame, p95 %.2f", n ? "; " : "",
                           s_cost[best].caller ? "sub_" : "Direct3D batch ", s_cost[best].caller,
                           s_cost[best].slow_frames, s_cost[best].slow_polygons / f, s_cost[best].slow_pixels / f,
                           s_cost[best].slow_ms.sum / f, perf_stat_percentile(&s_cost[best].slow_ms, 0.95));
        }
        if (n < 0 || (size_t)n >= sizeof line - 1) break;
    }
    line[sizeof line - 1] = 0;
    host_log("perf-detail", "slow-frame CPU polygon families (stable gameplay frames over 25 ms: %llu of "
             "%llu; frames = slow frames the family drew in): %s", s_slow_frames, s_gameplay_frames,
             line[0] ? line : "none");
}

static void materials_summary(void) {
    int used[KEY_SLOTS] = { 0 };
    unsigned long long total = 0;
    for (unsigned i = 0; i < KEY_SLOTS && s_key[i].polygons; i++) total += s_key[i].pixels;
    for (unsigned k = 0; k < 6; k++) {
        int best = -1;
        for (unsigned i = 0; i < KEY_SLOTS && s_key[i].polygons; i++)
            if (!used[i] && (best < 0 || s_key[i].slow_ms > s_key[best].slow_ms)) best = (int)i;
        if (best < 0 || !s_key[best].slow_ms) break;
        used[best] = 1;
        host_log("perf-detail", "CPU polygon material %08X (drawn by sub_%08X): %llu polygons, %.0f px each, "
                 "%.0f ms (%.1f%% of box pixels); in slow frames %llu polygons %.0f ms", s_key[best].key,
                 s_key[best].caller, s_key[best].polygons, (double)s_key[best].pixels / (double)s_key[best].polygons,
                 s_key[best].ms, total ? 100.0 * (double)s_key[best].pixels / (double)total : 0.0,
                 s_key[best].slow_polygons, s_key[best].slow_ms);
    }
}

static void gpu_summary(const char* name, const tgpu_session_t* s) {
    char reasons[320] = "";
    int n = 0;
    for (unsigned i = 1; i < TGPU_REASON_COUNT; i++)
        if (s->reason[i] && n >= 0 && (size_t)n < sizeof reasons - 1)
            n += _snprintf(reasons + n, sizeof reasons - 1 - (size_t)n, "%s%s %llu", n ? ", " : "", tgpu_reason_name(i),
                           s->reason[i]);
    reasons[sizeof reasons - 1] = 0;
    host_log("perf-detail", "%s: GPU %llu, CPU fallback %llu, dropped %llu%s; fallback reasons: %s", name, s->gpu, s->cpu,
             s->dropped, s->disabled_after_failure ? " (GPU path disabled after unsafe state)"
                                                   : s->enabled ? "" : " (GPU path off)", reasons[0] ? reasons : "none");
}

void cpu_polygons_summary(void) {
    char line[1400] = "";
    int n = 0, used[COST_SLOTS] = { 0 };
    for (unsigned k = 0; k < 10; k++) {
        int best = -1;
        for (unsigned i = 0; i < COST_SLOTS; i++)
            if (s_cost[i].polygons && !used[i] && (best < 0 || s_cost[i].ms > s_cost[best].ms)) best = (int)i;
        if (best < 0) break;
        used[best] = 1;
        n += _snprintf(line + n, sizeof line - 1 - (size_t)n, "%s%s%08X %llu polygons %.0f ms (%.2f us each, %.0f px "
                       "each, %.0f px per gameplay frame)", n ? "; " : "",
                       s_cost[best].caller ? "sub_" : "Direct3D batch, no Lock ", s_cost[best].caller,
                       s_cost[best].polygons, s_cost[best].ms, 1000.0 * s_cost[best].ms / (double)s_cost[best].polygons,
                       (double)s_cost[best].pixels / (double)s_cost[best].polygons,
                       s_gameplay_frames ? (double)s_cost[best].gameplay_pixels / (double)s_gameplay_frames : 0.0);
        if (n < 0 || (size_t)n >= sizeof line - 1) break;
    }
    line[sizeof line - 1] = 0;
    if (line[0]) host_log("perf-detail", "DrawPolygon by screen Lock caller (box pixels): %s", line);
    slow_frame_families_summary();
    materials_summary();
    gpu_summary("perspective-textured polygons (sub_00568EF0, sub_005B67E0)", &s_gpu[POLY_TEXTURE]);
    gpu_summary("bump polygons (sub_0057DB50, sub_005D66F0, sub_005D5E70)", &s_gpu[POLY_BUMP]);
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

#define BUMP_PRIMITIVE_INDEX 1u
#define TEXTURE_PRIMITIVE_INDEX 3u

/* The perspective-textured primitive (feature 0x00568EF0, its walkers sub_005B67E0 and sub_005B5F40) drawn by the GPU.
 * Its setup (sub_0054F2C0) fills the span's inputs before any screen Lock, so the first Lock decides, before any span
 * ran, whether the whole polygon goes to the GPU (Locks skipped, the span 0x0063D020 replaced by its own row loop,
 * which records u/z, v/z and 1/z where the rows start) or wholly to the original CPU path. The primitive reuses its
 * argument slots as locals: it must never run twice for one call. The 1.0.116 addresses below are the span's inputs. */
#define SPAN_TEXTURE_PERSPECTIVE 0x0063D020u
#define SPAN_BUMP_PERSPECTIVE 0x006402F0u /* 16-bit texels; the primitive and its walkers reach no other SelfMod span */
#define BUMP_TABLE 0x00737E98u            /* au1SubBumpToIntensity row: 1024 bytes */
#define BUMP_LIGHT_THETA 0x00737410u
#define TEX_TEXELS 0x00737E88u   /* pvTextureBitmap */
#define TEX_CLUT 0x00737F00u     /* 256 screen pixels for this polygon's lighting and fog */
#define TEX_CLAMPED 0x00737F64u  /* non-zero: u, v clamped and v times the row length; else tiled, v << 9 */
#define TEX_UMAX 0x00737408u
#define TEX_VMAX 0x00737EBCu
#define TEX_UMIN 0x0064E6D8u
#define TEX_UMASK 0x006699FCu
#define TEX_TILE_MASK GVA(0x006699F8)
#define TEX_LINE GVA(0x00737F34)
#define TEX_EVEN_ROWS GVA(0x00737F68)
#define TEX_SCREEN_LINE GVA(0x00737EC4)
#define TEX_SUBDIVISION 0x006699F0u /* pixels between two perspective divides; the x gradients are per subdivision */
#define TEX_GRADIENT_U 0x00737EDCu
#define TEX_GRADIENT_V 0x00737430u
#define TEX_GRADIENT_W 0x007373D8u
#define TEX_CARRY_U 0x007373E4u
#define TEX_CARRY_V 0x00737428u
#define TEX_CARRY_W 0x00737EB4u

static const uint32_t k_span_va[POLY_KINDS] = { SPAN_TEXTURE_PERSPECTIVE, SPAN_BUMP_PERSPECTIVE };

typedef struct {
    tgpu_texture_t texture;
    uint32_t texels, clut, bump_table, light_theta;
} span_inputs_t;

static struct {
    int rows, spans;
    float x[2], y[2], a[2][3], gx[3];
    span_inputs_t inputs;
    uint32_t iface;
    gpu_texture_polygon_t polygon;
} s_capture;

static float guest_float(uint32_t va) { return *(const float*)ADDR(va); }

static void read_span_inputs(span_inputs_t* in) {
    memset(in, 0, sizeof *in);
    in->texture.clamped = MEM32(TEX_CLAMPED) != 0;
    in->texture.umin = guest_float(TEX_UMIN);
    in->texture.umax = guest_float(TEX_UMAX);
    in->texture.vmax = guest_float(TEX_VMAX);
    in->texture.line = MEM32(TEX_LINE);
    in->texture.umask = MEM32(TEX_UMASK);
    in->texture.tile_mask = MEM32(TEX_TILE_MASK);
    in->texels = MEM32(TEX_TEXELS);
    in->clut = MEM32(TEX_CLUT);
    if (s_cur_kind == POLY_BUMP) {
        in->bump_table = MEM32(BUMP_TABLE);
        in->light_theta = MEM32(BUMP_LIGHT_THETA);
    }
}

static int read_vertices(gpu_texture_polygon_t* p) {
    uint32_t n = MEM32(RASTER_VERTEX_COUNT);
    if (n < 3 || n > sizeof p->x / sizeof p->x[0]) return 0;
    for (uint32_t i = 0; i < n; i++) {
        p->x[i] = guest_float(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + 0xC);
        p->y[i] = guest_float(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + 0x10);
        if (!(p->x[i] > -1e6f && p->x[i] < 1e6f && p->y[i] > -1e6f && p->y[i] < 1e6f)) return 0;
    }
    p->count = n;
    return 1;
}

/* Texel bytes: 8-bit texels, or the bump path's 16-bit ones. */
static tgpu_reason_t texture_reason(const span_inputs_t* in, uint32_t* bytes) {
    uint32_t texels;
    if (!in->texels || !in->texture.line) return TGPU_REASON_TEXTURE_INVALID;
    if (!in->clut) return TGPU_REASON_CLUT_INVALID;
    if (MEM32(TEX_EVEN_ROWS) != 0) return TGPU_REASON_ALTERNATE_LINES;
    if ((int32_t)MEM32(TEX_SUBDIVISION) <= 0) return TGPU_REASON_INVALID_SUBDIVISION;
    if (!(texels = tgpu_texture_bytes(&in->texture))) return TGPU_REASON_TEXTURE_UNBOUNDED;
    *bytes = s_cur_kind == POLY_BUMP ? texels * 2u : texels;
    if (!guest_readable(in->texels, *bytes)) return TGPU_REASON_TEXTURE_UNREADABLE;
    return TGPU_REASON_NONE;
}

/* The CLUT the pixels come from: 256 entries, or the rows the bump table can reach. */
static tgpu_reason_t clut_reason(const span_inputs_t* in, gpu_texture_polygon_t* p) {
    p->clut_entries = TGPU_CLUT_ENTRIES;
    if (s_cur_kind == POLY_BUMP) {
        if (!in->bump_table || !guest_readable(in->bump_table, TGPU_BUMP_TABLE_BYTES)) return TGPU_REASON_TEXTURE_INVALID;
        p->kind = GPU_POLYGON_BUMP;
        p->bump_table = (const uint8_t*)ADDR(in->bump_table);
        p->light_theta = in->light_theta;
        p->clut_entries = tgpu_bump_clut_entries(p->bump_table);
    }
    return guest_readable(in->clut, p->clut_entries * 2u) ? TGPU_REASON_NONE : TGPU_REASON_CLUT_INVALID;
}

static tgpu_reason_t renderer_reason(gpu_polygon_status_t status) {
    switch (status) {
    case GPU_POLYGON_READY: return TGPU_REASON_NONE;
    case GPU_POLYGON_BUSY: return TGPU_REASON_RASTER_LOCKED;
    case GPU_POLYGON_NO_RESOURCES: return TGPU_REASON_RESOURCES_UNAVAILABLE;
    default: return TGPU_REASON_TARGET_UNAVAILABLE;
    }
}

/* Everything the GPU draw needs, checked at the polygon's first screen Lock, before any span ran. */
static tgpu_reason_t gpu_preflight(uint32_t raster) {
    span_inputs_t* in = &s_capture.inputs;
    gpu_texture_polygon_t* p = &s_capture.polygon;
    tgpu_reason_t reason;
    if (!s_screen_render || raster != MEM32(s_screen_render + SCREEN_RENDER_RASTER)) return TGPU_REASON_UNSUPPORTED_STATE;
    if (!(s_capture.iface = MEM32(raster + RASTER_SURFACE))) return TGPU_REASON_NO_SURFACE;
    read_span_inputs(in);
    if ((reason = texture_reason(in, &p->texel_bytes)) != TGPU_REASON_NONE) return reason;
    if ((reason = clut_reason(in, p)) != TGPU_REASON_NONE) return reason;
    if (!read_vertices(p)) return TGPU_REASON_INVALID_VERTICES;
    p->origin_bytes = MEM32(raster) == RASTER_WIN_VTABLE ? MEM32(raster + RASTER_WIN_ORIGIN) : 0;
    p->clamped = in->texture.clamped;
    p->umin = in->texture.umin;
    p->umax = in->texture.umax;
    p->vmax = in->texture.vmax;
    p->line = in->texture.line;
    p->umask = in->texture.umask;
    p->tile_mask = in->texture.tile_mask;
    p->texels = (const uint8_t*)ADDR(in->texels);
    p->clut = (const uint16_t*)ADDR(in->clut);
    return renderer_reason(d3dr_texture_polygon_prepare((void*)ADDR(s_capture.iface), p));
}

static void note_row(int32_t x, int32_t y, const tgpu_scan_t* s) {
    int k = s_capture.rows;
    if (k >= 2 || (k == 1 && (float)y == s_capture.y[0])) return;
    s_capture.x[k] = (float)x;
    s_capture.y[k] = (float)y;
    s_capture.a[k][0] = s->uz;
    s_capture.a[k][1] = s->vz;
    s_capture.a[k][2] = s->wz;
    s_capture.rows++;
}

/* DrawSubtriangle(pscan, pdtri), cdecl: only pscan's edge and pdtri's rows change, as the native loop leaves them. */
static void span_capture(void) {
    uint32_t pscan = MEM32(g_esp + 4), pdtri = MEM32(g_esp + 8), inc = MEM32(pdtri + 0x38) + 0x1C;
    tgpu_scan_t s;
    tgpu_rows_t r;
    tgpu_steps_t d;
    span_inputs_t now;
    int32_t x0, x1;
    g_esp += 4;
    read_span_inputs(&now);
    if (memcmp(&now, &s_capture.inputs, sizeof now)) tgpu_polygon_unsafe(s_cur);
    if (!s_capture.spans++) {
        float sub = (float)(int32_t)MEM32(TEX_SUBDIVISION);
        s_capture.gx[0] = guest_float(TEX_GRADIENT_U) / sub;
        s_capture.gx[1] = guest_float(TEX_GRADIENT_V) / sub;
        s_capture.gx[2] = guest_float(TEX_GRADIENT_W) / sub;
    }
    s.fx = (int32_t)MEM32(pscan);
    s.rem = (int32_t)MEM32(pscan + 4);
    s.uz = guest_float(pscan + 0x10);
    s.vz = guest_float(pscan + 0x14);
    s.wz = guest_float(pscan + 0x18);
    r.y = (int32_t)MEM32(pdtri);
    r.y_end = (int32_t)MEM32(pdtri + 4);
    r.line = (int32_t)MEM32(pdtri + 8);
    r.len = (int32_t)MEM32(pdtri + 0xC);
    r.dlen = (int32_t)MEM32(pdtri + 0x10);
    d.fx_inc = (int32_t)MEM32(inc);
    d.rem_inc = (int32_t)MEM32(inc + 4);
    d.step[0] = guest_float(inc + 0x10);
    d.step[1] = guest_float(inc + 0x14);
    d.step[2] = guest_float(inc + 0x18);
    d.carry[0] = guest_float(TEX_CARRY_U);
    d.carry[1] = guest_float(TEX_CARRY_V);
    d.carry[2] = guest_float(TEX_CARRY_W);
    d.line_pixels = (int32_t)MEM32(TEX_SCREEN_LINE);
    do {
        if (tgpu_row_span(&s, &r, &x0, &x1)) note_row(x0, r.y, &s);
        tgpu_next_row(&s, &r, &d);
    } while (r.y < r.y_end);
    MEM32(pscan) = (uint32_t)s.fx;
    MEM32(pscan + 4) = (uint32_t)s.rem;
    *(float*)ADDR(pscan + 0x10) = s.uz;
    *(float*)ADDR(pscan + 0x14) = s.vz;
    *(float*)ADDR(pscan + 0x18) = s.wz;
    MEM32(pdtri) = (uint32_t)r.y;
    MEM32(pdtri + 8) = (uint32_t)r.line;
    MEM32(pdtri + 0xC) = (uint32_t)r.len;
}

/* No other SelfMod code is reached by these primitives; anything else would draw through a screen nobody locked. */
static void span_refused(void) {
    g_esp += 4;
    tgpu_polygon_unsafe(s_cur);
}

static recomp_func_t capture_span(uint32_t va) { return va == k_span_va[s_cur_kind] ? span_capture : span_refused; }

static int draw_captured_polygon(void) {
    gpu_texture_polygon_t* p = &s_capture.polygon;
    int k = s_capture.rows > 1;
    tgpu_plane_t plane;
    if (!s_capture.rows) return 1;
    tgpu_plane(&plane, s_capture.x[0], s_capture.y[0], s_capture.a[0], s_capture.x[k], s_capture.y[k], s_capture.a[k],
               s_capture.gx);
    for (int i = 0; i < 3; i++) {
        p->plane[0][i] = plane.u[i];
        p->plane[1][i] = plane.v[i];
        p->plane[2][i] = plane.w[i];
    }
    p->plane[3][0] = plane.origin_x;
    p->plane[3][1] = plane.origin_y;
    p->plane[3][2] = 0.0f;
    return d3dr_texture_polygon((void*)ADDR(s_capture.iface), p);
}

/* The screen Lock while a polygon of these primitives is drawn: 1 when the Lock is skipped. */
static int texture_polygon_lock(uint32_t raster) {
    tgpu_mode_t mode = s_cur->mode;
    if (mode == TGPU_PENDING) {
        memset(&s_capture, 0, sizeof s_capture);
        mode = tgpu_polygon_lock(s_cur, gpu_preflight(raster));
        if (mode == TGPU_GPU) selfmod_intercept(capture_span);
    }
    return mode == TGPU_GPU;
}

static void gpu_polygon(unsigned kind, recomp_func_t run, const char* name) {
    tgpu_session_t* session = &s_gpu[kind];
    int gpu, drawn;
    s_cur = session;
    s_cur_kind = kind;
    tgpu_polygon_begin(session);
    run();
    gpu = session->mode == TGPU_GPU;
    if (gpu) selfmod_intercept(NULL);
    drawn = gpu && !session->unsafe && draw_captured_polygon();
    s_cur = NULL;
    if (tgpu_polygon_end(session, drawn))
        host_log("renderer", "GPU %s polygon path disabled after unsafe state, falling back to CPU for subsequent "
                 "polygons (%d spans, %d rows)", name, s_capture.spans, s_capture.rows);
}

static void run_texture_primitive(void) { store_only(TEXTURE_PRIMITIVE_INDEX); }

static void run_bump_primitive(void) { store_only(BUMP_PRIMITIVE_INDEX); }

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

static void follow_raster_lines(uint32_t screen_render) {
    uint32_t raster;
    float limit;
    if (!s_gradient_limit || !(raster = MEM32(screen_render + SCREEN_RENDER_RASTER))) return;
    limit = planar_gradient_limit((long)MEM32(raster + RASTER_HEIGHT));
    if (*s_gradient_limit != limit) *s_gradient_limit = limit;
}

static void draw_polygon(void) {
    int64_t begin, end;
    if (g_alias_entry) {
        s_draw_lifted();
        return;
    }
    begin = perf_section_begin();
    follow_raster_lines(g_ecx);
    s_screen_render = g_ecx;
    s_draw_caller = 0;
    s_draw_pixels = 0;
    s_draw_key = MEM32(MEM32(g_esp + 4) + 8);
    s_drawing++;
    s_draw_lifted();
    s_drawing--;
    end = perf_section_begin();
    perf_section_commit(PS_CPU_POLYGONS, begin, end);
    charge_polygon(s_draw_caller, s_draw_pixels, (double)(end - begin) * s_qpc_ms);
    if (s_draw_pixels) charge_key(s_draw_key, s_draw_caller, s_draw_pixels, (double)(end - begin) * s_qpc_ms);
}

static void raster_lock(void) {
    long box[4];
    uint32_t raster = g_ecx;
    if (g_alias_entry || !s_drawing) {
        s_lock_lifted();
        return;
    }
    if (s_cur && s_cur->mode != TGPU_OFF && texture_polygon_lock(raster)) {
        g_esp += 4;
        return;
    }
    if (!raster_box(box)) {
        s_lock_lifted();
        return;
    }
    if (!s_draw_caller) {
        s_draw_caller = d3dr_lock_caller();
        s_draw_pixels = (unsigned long long)(box[2] - box[0]) * (unsigned long long)(box[3] - box[1]);
    }
    d3dr_cpu_polygon((void*)ADDR(MEM32(raster + RASTER_SURFACE)), box, raster_origin(raster), !s_store_only);
    s_lock_lifted();
    d3dr_cpu_polygon_done();
}


static void store_only_0(void) { store_only(0); }
static void store_only_1(void) {
    if (g_alias_entry || !s_drawing || !s_gpu[POLY_BUMP].enabled || s_cur) {
        store_only(BUMP_PRIMITIVE_INDEX);
        return;
    }
    gpu_polygon(POLY_BUMP, run_bump_primitive, "bump");
}
static void store_only_2(void) { store_only(2); }
static void store_only_3(void) {
    if (g_alias_entry || !s_drawing || !s_gpu[POLY_TEXTURE].enabled || s_cur) {
        store_only(TEXTURE_PRIMITIVE_INDEX);
        return;
    }
    gpu_polygon(POLY_TEXTURE, run_texture_primitive, "textured");
}
static void store_only_4(void) { store_only(4); }

/* Only the 1.0.116 lift's address is known; the value must still be the game's before it becomes the host's. */
static void gradient_limit_install(void) {
    volatile float* p;
    DWORD old;
    if (jpt_active_lift() != JPT_LIFT_116) return;
    p = (volatile float*)ADDR(GRADIENT_LIMIT_LIFT116);
    if (*p != PLANAR_GRADIENT_LIMIT_1998 || !VirtualProtect((void*)p, sizeof *p, PAGE_READWRITE, &old)) {
        host_log("renderer", "software polygons: the trapezoid gradient limit was left as the game has it");
        return;
    }
    s_gradient_limit = p;
}

void cpu_polygons_install(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpc_ms = 1000.0 / (double)f.QuadPart;
    static const recomp_func_t entries[STORE_ONLY_COUNT] = { store_only_0, store_only_1, store_only_2, store_only_3,
                                                             store_only_4 };
    unsigned known = 0;
    s_installed = guest_override_install(DRAW_POLYGON_VA, draw_polygon, &s_draw_lifted) &&
                  guest_override_install(RASTER_LOCK_VA, raster_lock, &s_lock_lifted);
    if (!s_installed) return;
    for (unsigned i = 0; i < STORE_ONLY_COUNT; i++)
        if (guest_override_install(GVA_AT(k_store_only[i]), entries[i], &s_store_only_lifted[i])) known++;
    gradient_limit_install();
    {
        const char* flag = getenv("JPT_GPU_POLYGONS");
        const char* bump_flag = getenv("JPT_GPU_BUMP");
        int on = jpt_active_lift() == JPT_LIFT_116 && !(flag && flag[0] == '0');
        tgpu_session_init(&s_gpu[POLY_TEXTURE], on);
        tgpu_session_init(&s_gpu[POLY_BUMP], on && !(bump_flag && bump_flag[0] == '0'));
    }
    host_log("renderer", "software polygons in Direct3D frames: Locks limited to each polygon's box, %u of %u "
             "store-only primitives known; trapezoid gradients from a triple of at least %.0f px2 at 480 lines, scaled "
             "with the raster's lines (%.0f px2 at 2160)%s", known, (unsigned)STORE_ONLY_COUNT,
             -0.5f * PLANAR_GRADIENT_LIMIT_1998, -0.5f * planar_gradient_limit(2160),
             s_gradient_limit ? "" : ": not applied to this version");
}
