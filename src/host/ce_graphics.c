/*
 * Enhanced sky: CSkyRender::DrawD3D draws the retail sky plane as it always did; the renderer gets what it needs to
 * fog that plane by view direction (ce_effects.h) for the draws in between.
 */
#include "guest_va.h"
#include "host.h"

#include "ce_effects.h"
#include "ddraw/d3d_legacy.h"
#include "water_kernels.h"

#define SKY_DRAW_D3D GVA(0x005242C0)
#define SKY_FOG_HIGH 0x2Cu
#define SKY_FOG_LOW 0x30u
#define SKY_HEIGHT 0x44u
#define SKY_SCROLL_U 0x48u
#define SKY_SCROLL_V 0x4Cu
#define CAMERA_POSITION GVA(0x00732978) /* CSkyRender's copy, refreshed by 0x00523D00 before each draw */
#define FOG_COLOR GVA(0x006D1E94)

static recomp_func_t s_sky_lifted;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

/* The game keeps the level's fog colour in its own state; the device's copy is a cache that a recreated device loses. */
static void sync_fog_color(d3d_device_t* dev) {
    uint32_t color;
    if (guest_read32(FOG_COLOR, &color)) d3dr_set_fog_color(dev, color);
}

/* thiscall, no arguments. */
static void sky_draw(void) {
    uint32_t sky = g_ecx;
    float p[5];
    if (g_alias_entry || !sky || !guest_readable(sky + SKY_FOG_HIGH, SKY_SCROLL_V + 4 - SKY_FOG_HIGH) ||
        !guest_readable(CAMERA_POSITION, 12)) {
        s_sky_lifted();
        return;
    }
    p[0] = rdf(CAMERA_POSITION) * FX_SKY_UV_PER_METRE + FX_SKY_U_OFFSET + rdf(sky + SKY_SCROLL_U);
    p[1] = rdf(CAMERA_POSITION + 4) * FX_SKY_UV_PER_METRE + rdf(sky + SKY_SCROLL_V);
    p[2] = (rdf(sky + SKY_HEIGHT) - rdf(CAMERA_POSITION + 8)) * FX_SKY_UV_PER_METRE;
    p[3] = rdf(sky + SKY_FOG_LOW);
    p[4] = rdf(sky + SKY_FOG_HIGH);
    d3dr_sky_begin(p);
    s_sky_lifted();
    d3dr_sky_end();
}

void ce_graphics_install(int hardware) {
    if (!hardware) return;
    d3d_set_fog_sync(sync_fog_color);
    water_rows_observer = fx_water_store;
    if (!guest_override_install(SKY_DRAW_D3D, sky_draw, &s_sky_lifted))
        host_log("host", "warning: the sky's Direct3D draw could not be hooked: Enhanced sky is not available");
}
