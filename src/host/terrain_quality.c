/*
 * Terrain detail with Direct3D 11. Under Direct3D a terrain node beyond TrrNoDynTextureDist (times the quality's
 * distance scale) is drawn from the level's small base texture, as is a node whose own texture finds no room in the
 * terrain texture pages; TrrNoTextureDist only applies to the software renderer. CQuadRootTIN::Update reads the range
 * variables once per call to build its evaluation constants, so the modern values are in place for that call only:
 * levels, save games and the software renderer keep their own.
 */
#include "host.h"

#include <string.h>

#define QUAD_ROOT_UPDATE_VA 0x0049E530u   /* CQuadRootTIN::Update(camera, force) */
#define EVALUATE_TEXTURE_VA 0x0049BD80u   /* CQuadNodeTIN::EvaluateTexture */
#define SCHEDULE_UPDATE_VA 0x00492730u    /* CTextureNode::ScheduleUpdate */
#define TEXTURE_UPDATE_VA 0x004933E0u     /* CTextureNode::CPriv::DoUpdate: page region + CPU render */
#define SET_STATIC_VA 0x00492C90u         /* CTextureNode::SetStaticTexture: a crop of the base texture */
#define SET_SOLID_VA 0x00492F10u          /* CTextureNode::SetSolidTexture */
#define RVAR_NO_DYN_TEXTURE 0x0066FB78u   /* CRangeVar<float> {data, max, min} */
#define RVAR_PIXEL_TOL 0x0066FAD8u
#define RVAR_PIXEL_TOL_FAR 0x0066FB68u
#define QUALITY_SETTINGS 0x0066C4E8u      /* qdQualitySettings[5], 0x5C each */
#define QUALITY_STRIDE 0x5Cu
#define QUALITY_DISTANCE_SCALE 0x44u      /* fTerrainDistanceScale */
#define QUALITY_PIXEL_TOL_ADJ 0x48u       /* fPixelTolAdj */
#define ROOT_QUALITY 0x40u                /* CQuadRootTIN::iLastQualitySetting */
#define MAIN_CAMERA_HOLDER 0x0073F444u    /* [[holder] + 0x14] is the main camera */
#define CAMERA_FAR 0xB0u                  /* SProperties (+0x74) rFarClipPlaneDist (+0x3C) */
#define NODE_HEAP_POINTER 0x0066FAF4u     /* CQuadNodeTIN's block store: its CFastHeap {.., +4 used, .., +0xC size} */
#define PAGE_SIZE 0x0066424Cu             /* terrain texture page size */
#define REGION_UPLOADS 0x006D1461u        /* d3dDriver.bUseRegionUploads */
#define BASE_TEXTURE 0x0066F9C4u          /* CRaster* of the level's base texture: +8 width, +0xC height */
#define BASE_REGION_SIZE 0x0066F9B0u      /* the base texture's region in the pages: width, height */

static recomp_func_t s_update_lifted, s_evaluate_lifted, s_schedule_lifted, s_texture_update_lifted;
static recomp_func_t s_static_lifted, s_solid_lifted;
static int s_installed;
static int s_in_evaluate, s_in_schedule, s_in_update, s_in_static;
static float s_render_lines_scale = 3.0f;
static terrain_settings_t s_last_logged;
static uint32_t s_node_heap_peak, s_last_pages;

/* DEBUG_SAULO: how each terrain node evaluation ended, per 5 s window and for the session. */
typedef struct {
    unsigned long long updates, evaluated, own_texture, static_by_distance, static_pages_full, solid_no_base;
    unsigned long long solid_disabled, solid_by_distance, solid_other, rendered;
} terrain_stats_t;
static terrain_stats_t s_window, s_total;

static float rdf(uint32_t va) { return *(float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

void terrain_quality_set_render_height(int height) {
    if (height > 0) s_render_lines_scale = (float)height / 480.0f;
}

static float main_camera_far(void) {
    uint32_t holder = MEM32(MAIN_CAMERA_HOLDER), cam = holder ? MEM32(holder + 0x14) : 0;
    return cam ? rdf(cam + CAMERA_FAR) : 0.0f;
}

static uint32_t quality_row(uint32_t root) {
    uint32_t q = MEM32(root + ROOT_QUALITY);
    return QUALITY_SETTINGS + (q <= ENGINE_RENDER_QUALITY_TOP ? q : ENGINE_RENDER_QUALITY_TOP) * QUALITY_STRIDE;
}

static terrain_settings_t settings_now(uint32_t root) {
    uint32_t row = quality_row(root);
    terrain_settings_t level;
    level.dynamic_texture_distance = rdf(RVAR_NO_DYN_TEXTURE);
    level.pixel_tolerance = rdf(RVAR_PIXEL_TOL);
    level.pixel_tolerance_far = rdf(RVAR_PIXEL_TOL_FAR);
    level.distance_scale = rdf(row + QUALITY_DISTANCE_SCALE);
    level.pixel_tolerance_scale = rdf(row + QUALITY_PIXEL_TOL_ADJ);
    return level;
}

static void log_settings(const terrain_settings_t* level, const terrain_settings_t* used, float far_clip) {
    if (!memcmp(used, &s_last_logged, sizeof *used)) return;
    s_last_logged = *used;
    host_log("terrain", "level TrrNoDynTextureDist %.0f, TrrPixelTol %.1f, TrrPixelTolFar %.1f (quality scales %.2f and "
             "%.2f), camera far clip %.0f m; used: own textures to %.0f m, mesh error %.1f px near and %.1f px far at "
             "the render size (%.1f and %.1f px at 480 lines)", level->dynamic_texture_distance, level->pixel_tolerance,
             level->pixel_tolerance_far, level->distance_scale, level->pixel_tolerance_scale, far_clip,
             used->dynamic_texture_distance * used->distance_scale, used->pixel_tolerance * used->pixel_tolerance_scale,
             used->pixel_tolerance_far * used->pixel_tolerance_scale,
             used->pixel_tolerance * used->pixel_tolerance_scale / s_render_lines_scale,
             used->pixel_tolerance_far * used->pixel_tolerance_scale / s_render_lines_scale);
}

/* DEBUG_SAULO: the page layout and base texture the level ended up with. */
static void log_pages(void) {
    uint32_t base = MEM32(BASE_TEXTURE), pages = MEM32(PAGE_SIZE) | (uint32_t)MEM8(REGION_UPLOADS) << 16;
    if (pages == s_last_pages || !base) return;
    s_last_pages = pages;
    host_log("terrain", "texture pages %u px, region uploads %s; base texture %ux%u in a %ux%u region%s",
             MEM32(PAGE_SIZE), MEM8(REGION_UPLOADS) ? "on" : "off", MEM32(base + 8), MEM32(base + 0xC),
             MEM32(BASE_REGION_SIZE), MEM32(BASE_REGION_SIZE + 4),
             MEM32(BASE_REGION_SIZE) && MEM32(BASE_REGION_SIZE) < MEM32(base + 8) ? " (cropped: far terrain shows "
             "part of the map)" : "");
}

static void sample_node_heap(void) {
    uint32_t heap = MEM32(NODE_HEAP_POINTER), used = heap ? MEM32(heap + 4) : 0;
    if (used > s_node_heap_peak) s_node_heap_peak = used;
}

static void quad_root_update(void) {
    terrain_settings_t level, used;
    float far_clip;
    if (g_alias_entry) {
        s_update_lifted();
        return;
    }
    s_window.updates++;
    if (!engine_limits_modern()) {
        s_update_lifted();
        sample_node_heap();
        return;
    }
    level = settings_now(g_ecx);
    far_clip = main_camera_far();
    used = terrain_modern_settings(level, far_clip, engine_heaps_enlarged() ? s_render_lines_scale : 0.0f);
    log_settings(&level, &used, far_clip);
    wrf(RVAR_NO_DYN_TEXTURE, used.dynamic_texture_distance);
    wrf(RVAR_PIXEL_TOL, used.pixel_tolerance);
    wrf(RVAR_PIXEL_TOL_FAR, used.pixel_tolerance_far);
    s_update_lifted();
    wrf(RVAR_NO_DYN_TEXTURE, level.dynamic_texture_distance);
    wrf(RVAR_PIXEL_TOL, level.pixel_tolerance);
    wrf(RVAR_PIXEL_TOL_FAR, level.pixel_tolerance_far);
    sample_node_heap();
    log_pages();
}

static void call_counted(recomp_func_t lifted, int* depth) {
    int alias = g_alias_entry != 0;
    if (!alias) ++*depth;
    lifted();
    if (!alias) --*depth;
}

static void evaluate_texture(void) {
    if (!g_alias_entry) s_window.evaluated++;
    call_counted(s_evaluate_lifted, &s_in_evaluate);
}

static void schedule_update(void) {
    if (!g_alias_entry && s_in_evaluate) s_window.own_texture++;
    call_counted(s_schedule_lifted, &s_in_schedule);
}

static void texture_update(void) {
    if (!g_alias_entry) s_window.rendered++;
    call_counted(s_texture_update_lifted, &s_in_update);
}

static void set_static_texture(void) {
    if (!g_alias_entry) {
        if (s_in_update) s_window.static_pages_full++;
        else if (s_in_evaluate) s_window.static_by_distance++;
    }
    call_counted(s_static_lifted, &s_in_static);
}

static void set_solid_texture(void) {
    if (!g_alias_entry) {
        if (s_in_static) s_window.solid_no_base++;
        else if (s_in_schedule) s_window.solid_disabled++;
        else if (s_in_evaluate) s_window.solid_by_distance++;
        else s_window.solid_other++;
    }
    s_solid_lifted();
}

void terrain_quality_install(void) {
    static const struct {
        uint32_t va;
        recomp_func_t replacement;
        recomp_func_t* lifted;
    } k[] = {
        { QUAD_ROOT_UPDATE_VA, quad_root_update, &s_update_lifted },
        { EVALUATE_TEXTURE_VA, evaluate_texture, &s_evaluate_lifted },
        { SCHEDULE_UPDATE_VA, schedule_update, &s_schedule_lifted },
        { TEXTURE_UPDATE_VA, texture_update, &s_texture_update_lifted },
        { SET_STATIC_VA, set_static_texture, &s_static_lifted },
        { SET_SOLID_VA, set_solid_texture, &s_solid_lifted },
    };
    s_installed = 1;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (guest_override_redirect(k[i].va, k[i].replacement, k[i].lifted)) continue;
        host_log("terrain", "sub_%08X has no incremental-link thunk: the terrain keeps the level's own settings",
                 k[i].va);
        s_installed = 0;
        return;
    }
}

void terrain_quality_log(void) {
    if (!s_installed || !engine_limits_modern()) return;
    host_log("graphics", "  terrain: own (dynamic) textures out to the camera's far clip, at most %.0f m (levels: "
             "230 m times 1.5 at quality 4); mesh error at most TrrPixelTol %.0f / TrrPixelTolFar %.0f at 480 lines%s; "
             "terrain heaps reserved %ux", TERRAIN_TEXTURE_DISTANCE_MAX, TERRAIN_PIXEL_TOL_480,
             TERRAIN_PIXEL_TOL_FAR_480,
             engine_heaps_enlarged() ? " (scaled to the render height)" : " (levels' own: heaps not enlarged)",
             engine_heaps_enlarged() ? TERRAIN_HEAP_SCALE : 1u);
}

static void add_terrain(terrain_stats_t* t, const terrain_stats_t* w) {
    const unsigned long long* from = (const unsigned long long*)w;
    unsigned long long* to = (unsigned long long*)t;
    for (size_t i = 0; i < sizeof *w / sizeof *from; i++) to[i] += from[i];
}

static void log_terrain(const terrain_stats_t* s, const char* when) {
    double n = s->updates ? (double)s->updates : 1.0;
    uint32_t heap = MEM32(NODE_HEAP_POINTER);
    host_log("terrain", "%s per terrain update (%llu): nodes %.0f: own texture %.0f, base texture by distance "
             "(TrrNoDynTextureDist) %.1f, base texture because the texture pages were full %.1f; solid colour: no base "
             "texture %.1f, textures disabled %.1f, by distance (TrrNoTextureDist, software) %.1f, other %.1f | node "
             "textures rendered %.1f | node heap peak %u KB (1998 size %u KB, reserved %u KB)", when, s->updates,
             s->evaluated / n, s->own_texture / n, s->static_by_distance / n, s->static_pages_full / n,
             s->solid_no_base / n, s->solid_disabled / n, s->solid_by_distance / n, s->solid_other / n,
             s->rendered / n, s_node_heap_peak / 1024u, heap ? MEM32(heap + 0xC) / 1024u : 0u,
             heap ? MEM32(heap + 0xC) / 1024u * (engine_heaps_enlarged() ? TERRAIN_HEAP_SCALE : 1u) : 0u);
}

void terrain_quality_summary(void) {
    if (!s_installed) return;
    add_terrain(&s_total, &s_window);
    memset(&s_window, 0, sizeof s_window);
    if (s_total.updates) log_terrain(&s_total, "session");
}
