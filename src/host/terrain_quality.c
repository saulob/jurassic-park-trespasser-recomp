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
#define PAGE_SIZE 0x0066424Cu             /* terrain texture page size */
#define REGION_UPLOADS 0x006D1461u        /* d3dDriver.bUseRegionUploads */
#define BASE_TEXTURE 0x0066F9C4u          /* CRaster* of the level's base texture: +8 width, +0xC height */
#define BASE_REGION_SIZE 0x0066F9B0u      /* the base texture's region in the pages: width, height */

static recomp_func_t s_update_lifted;
static int s_installed;
static float s_render_lines_scale = 3.0f;
static terrain_settings_t s_last_logged;
static uint32_t s_last_pages;

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

/* The page layout and base texture the level ended up with. */
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

static void quad_root_update(void) {
    terrain_settings_t level, used;
    float far_clip;
    if (g_alias_entry) {
        s_update_lifted();
        return;
    }
    if (!engine_limits_modern()) {
        s_update_lifted();
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
    log_pages();
}

void terrain_quality_install(void) {
    s_installed = guest_override_install(QUAD_ROOT_UPDATE_VA, quad_root_update, &s_update_lifted);
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
