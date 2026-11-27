/* What the Direct3D renderer needs from the lifted game, as ce_graphics.c, cpu_polygons.c and vk_win_policy.c take it on Windows: the
 * sky's draws, the water's rows, the level's fog colour, who locks the screen, and the box of a polygon the game rasterises. */
#include <string.h>

#include "ce_effects.h"
#include "guest_abi.h"
#include "guest_features.h"
#include "jpt_custom_level.h"
#include "linux_d3d.h"
#include "linux_features.h"
#include "scene_options.h"
#include "sky_layers.h"
#include "texhw_guest.h"
#include "water_kernels.h"

#define SKY_DRAW_D3D GVA(0x005242C0)
#define SKY_FOG_HIGH 0x2Cu
#define SKY_FOG_LOW 0x30u
#define SKY_HEIGHT 0x44u
#define SKY_SCROLL_U 0x48u
#define SKY_SCROLL_V 0x4Cu
#define CAMERA_POSITION GVA(0x00732978)

#define UPLOAD_POLYGONS_VA GVA(0x0055A4E0)
#define DRAW_POLYGON_VA GVA(0x0055F540)
#define RASTER_LOCK_VA GVA(0x00503400)
#define RASTER_WIN_LOCK_VA GVA(0x00504D20)
#define RASTER_WIN_LOCK_THUNK GVA(0x00504D40)
#define RASTER_WIN_VTABLE GVA(0x0064DB54)
#define RASTER_SURFACE 0x4Cu
#define RASTER_WIN_ORIGIN 0x90u
#define RASTER_VERTICES GVA(0x00737438)
#define RASTER_VERTEX_COUNT GVA(0x00737EEC)
#define RASTER_VERTEX_SIZE 0x28u
#define RASTER_VERTEX_X 0xCu
#define RASTER_VERTEX_Y 0x10u
#define MAX_VERTICES 64u
#define BOX_MARGIN 2
#define NO_LIMIT 0x7FFFFFFFL

static recomp_func_t s_sky_lifted, s_draw_lifted, s_lock_lifted;
static int s_drawing, s_store_only;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

/* thiscall, no arguments. */
static void sky_draw(void) {
    uint32_t sky = g_ecx;
    float p[5];
    if (g_alias_entry || !sky || !guest_readable(sky + SKY_FOG_HIGH, SKY_SCROLL_V + 4 - SKY_FOG_HIGH) ||
        !guest_readable(CAMERA_POSITION, 12)) {
        s_sky_lifted();
        return;
    }
    if (sky_layers_ready()) {
        sky_layers_begin(sky);
        lx_d3d_sky_layers(1);
        s_sky_lifted();
        lx_d3d_sky_layers(0);
        sky_layers_end(sky);
        return;
    }
    p[0] = rdf(CAMERA_POSITION) * FX_SKY_UV_PER_METRE + FX_SKY_U_OFFSET + rdf(sky + SKY_SCROLL_U);
    p[1] = rdf(CAMERA_POSITION + 4) * FX_SKY_UV_PER_METRE + rdf(sky + SKY_SCROLL_V);
    p[2] = (rdf(sky + SKY_HEIGHT) - rdf(CAMERA_POSITION + 8)) * FX_SKY_UV_PER_METRE;
    p[3] = rdf(sky + SKY_FOG_LOW);
    p[4] = rdf(sky + SKY_FOG_HIGH);
    lx_d3d_sky_begin(p);
    s_sky_lifted();
    lx_d3d_sky_end();
}

static void water_texel(uintptr_t texel, double right, double below) {
    fx_water_store(texel, right, below);
    lx_d3d_water_row();
}

static uint32_t lock_caller(void) {
    const uint32_t wrappers[] = { RASTER_LOCK_VA, RASTER_WIN_LOCK_VA, RASTER_WIN_LOCK_THUNK };
    uint32_t caller = g_cur_func, found;
    if (caller != RASTER_LOCK_VA && caller != RASTER_WIN_LOCK_VA && caller != RASTER_WIN_LOCK_THUNK) return caller;
    found = icall_ring_caller(g_icall_trace, g_icall_from, g_icall_trace_idx, ICALL_TRACE_SIZE, wrappers, 3);
    return found ? found : caller;
}

static int raster_box(long box[4]) {
    float x[MAX_VERTICES], y[MAX_VERTICES];
    uint32_t n = MEM32(RASTER_VERTEX_COUNT);
    if (n < 3 || n > MAX_VERTICES) return 0;
    for (uint32_t i = 0; i < n; i++) {
        x[i] = rdf(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + RASTER_VERTEX_X);
        y[i] = rdf(RASTER_VERTICES + i * RASTER_VERTEX_SIZE + RASTER_VERTEX_Y);
    }
    return cpu_polygon_box(x, y, n, NO_LIMIT, NO_LIMIT, BOX_MARGIN, box);
}

static void draw_polygon(void) {
    if (g_alias_entry) {
        s_draw_lifted();
        return;
    }
    s_drawing++;
    texhw_lit_begin(MEM32(g_esp + 4));
    s_draw_lifted();
    texhw_lit_end();
    s_drawing--;
}

static void raster_lock(void) {
    uint32_t raster = g_ecx, origin;
    long box[4];
    if (g_alias_entry || !s_drawing || !raster_box(box)) {
        s_lock_lifted();
        return;
    }
    origin = g_cur_func == RASTER_WIN_LOCK_VA && MEM32(raster) == RASTER_WIN_VTABLE ? MEM32(raster + RASTER_WIN_ORIGIN) : 0;
    lx_d3d_cpu_polygon(MEM32(raster + RASTER_SURFACE), box, origin, !s_store_only);
    s_lock_lifted();
    lx_d3d_cpu_polygon_done();
}

/* The primitives whose spans only store pixels (cpu_polygons.c). */
#define STORE_ONLY_COUNT 5
static const unsigned k_store_only[STORE_ONLY_COUNT] = {
    GVA_ID(0x0057B7B0), GVA_ID(0x0057DB50), GVA_ID(0x005668C0), GVA_ID(0x00568EF0), GVA_ID(0x00563890),
};
static recomp_func_t s_store_only_lifted[STORE_ONLY_COUNT];

static void store_only(int i) {
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

static int scene_shown(void) {
    guest_ui_view_t v;
    return guest_ui_probe(&v) && v.ui == GUEST_UI_GAMEPLAY;
}

static recomp_func_t s_upload_lifted;

static void upload_polygons(void) {
    const uint32_t count = g_alias_entry ? 0 : GUEST_ARG(0), polygons = g_alias_entry ? 0 : GUEST_ARG(1);
    s_upload_lifted();
    texhw_tag_polygons(count, polygons, lx_d3d_texture_override);
}

static void install_polygons(void) {
    static const recomp_func_t entries[STORE_ONLY_COUNT] = { store_only_0, store_only_1, store_only_2, store_only_3, store_only_4 };
    if (!guest_override_install(DRAW_POLYGON_VA, draw_polygon, &s_draw_lifted) ||
        !guest_override_install(RASTER_LOCK_VA, raster_lock, &s_lock_lifted)) {
        host_log("host", "warning: the game's software polygons could not be hooked: each one reads the whole screen back");
        return;
    }
    for (int i = 0; i < STORE_ONLY_COUNT; i++) guest_override_install(GVA_AT(k_store_only[i]), entries[i], &s_store_only_lifted[i]);
}

void lx_d3d_guest_install(const jpt_config_t* cfg) {
    if (!lx_d3d_hardware()) return;
    s_drawing = s_store_only = 0;
    lx_d3d_configure_effects(cfg->water_refraction, cfg->enhanced_sky, cfg->ambient_occlusion);
    lx_d3d_set_fog_sync(scene_fog_color);
    lx_d3d_set_view(scene_shown, widescreen_view_tan_half_height);
    lx_d3d_set_lock_caller(lock_caller);
    water_rows_observer = water_texel;
    sky_layers_install(lx_d3d_sky_free);
    if (!guest_override_install(WATER_ROWS_VA, water_rows, &water_rows_lifted))
        host_log("host", "warning: the water's rows could not be hooked: water refraction is not available");
    if (!guest_override_install(SKY_DRAW_D3D, sky_draw, &s_sky_lifted))
        host_log("host", "warning: the sky's Direct3D draw could not be hooked: Enhanced sky is not available");
    install_polygons();
    if (jpt_custom_level_requires(JPT_LEVEL_CAP_TEXHW)) guest_override_install(UPLOAD_POLYGONS_VA, upload_polygons, &s_upload_lifted);
}

void lx_d3d_guest_remove(void) {
    water_rows_observer = NULL;
    lx_d3d_set_fog_sync(NULL);
    lx_d3d_set_view(NULL, NULL);
    lx_d3d_set_lock_caller(NULL);
}
