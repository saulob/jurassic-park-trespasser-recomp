#include "terrain_shadows.h"

#include "guest_abi.h"

#define TERRAIN_BASE GVA(0x0066F9C4)        /* prasBaseTexture, among the texture nodes' statics */
#define TERRAIN_CLUT (TERRAIN_BASE + 0x48u) /* pclutDirect3D */
#define SHADOW_LEVEL (TERRAIN_BASE + 0x50u) /* iShadowLightVal */
#define CAMERA_PROPS 0x74u
#define SHADOW_NEAR 1.0f
#define SHADOW_FAR 2000.0f
#define RAMP_ROW 256u
#define FRAME_FROM_CAMERA 0x22Cu /* ShadowLighting builds its camera in its own frame */

typedef struct {
    uint32_t material, start_ramp, end_ramp, end_depth, reflection, rows, depths;
    float shade_scale, shade_offset, hilite_scale, hilite_offset;
    uint32_t shaded_rows;
    float start, most;
    uint32_t table, translated_table, pixel_bytes, ramp_shift, depth_shift, entries, terrain, unused, palette, format_vtable,
        format_palette;
    uint8_t bits, channels[6], pad;
} guest_clut_t;

typedef struct {
    uint32_t saved_ebp, return_slot, raster, shear_x, shear_y, light_level, moving, fixed;
} shadow_frame_t;

typedef struct {
    float width, height;
    uint32_t skipped[11];
    uint8_t perspective, pad[3];
    float nearest, farthest;
} camera_props_t;

static struct {
    int on, ready;
    unsigned ramps, refused;
} S;

void terrain_shadows_configure(int shadows) { S.on = shadows; }

static guest_clut_t* terrain_clut(void) {
    uint32_t clut;
    if (!guest_read32(TERRAIN_CLUT, &clut) || !clut || !guest_writable(clut, sizeof(guest_clut_t))) return NULL;
    return (guest_clut_t*)ADDR(clut);
}

static int ramp_usable(const guest_clut_t* c) {
    return c->rows == 32u && c->depths == 1u && c->pixel_bytes == 2u && c->entries == RAMP_ROW && c->shaded_rows > 1u &&
           c->shaded_rows <= c->rows && guest_writable(c->table, c->rows * RAMP_ROW * 2u);
}

static int reserve_shadow_row(guest_clut_t* c) {
    uint16_t* table = (uint16_t*)ADDR(c->table);
    const uint16_t* lit = table + (c->shaded_rows - 1u) * RAMP_ROW;
    if (!engine_ramp_reserve(&c->shade_scale, &c->shade_offset, c->start, c->shaded_rows)) return 0;
    for (uint32_t i = 0; i < RAMP_ROW; i++) table[i] = engine_scale_pixel(lit[i], c->channels, ENGINE_SHADOW_BRIGHTNESS);
    if (S.ramps++ < 4)
        host_log("graphics", "terrain shadows: %.2f of the lit ground (the ramp's darkest light was %.2f)",
                 ENGINE_SHADOW_BRIGHTNESS, c->start);
    return 1;
}

void terrain_shadows_frame(void) {
    guest_clut_t* c;
    S.ready = 0;
    if (!S.on || !(c = terrain_clut())) return;
    if (!ramp_usable(c)) {
        if (!S.refused++) host_log("graphics", "terrain shadows: the terrain ramp is not the engine's; left as it is");
        return;
    }
    S.ready = engine_ramp_reserved(c->shade_scale, c->shade_offset, c->start) || reserve_shadow_row(c);
}

static shadow_frame_t* shadow_frame(uint32_t camera, uint32_t props) {
    const camera_props_t* p;
    shadow_frame_t* frame;
    uint32_t level;
    if (props != camera + CAMERA_PROPS || !guest_readable(props, sizeof *p)) return NULL;
    p = (const camera_props_t*)ADDR(props);
    if (!p->perspective || p->nearest != SHADOW_NEAR || p->farthest != SHADOW_FAR) return NULL;
    if (!guest_writable(camera + FRAME_FROM_CAMERA, sizeof *frame)) return NULL;
    frame = (shadow_frame_t*)ADDR(camera + FRAME_FROM_CAMERA);
    if (!guest_read32(SHADOW_LEVEL, &level) || frame->light_level != level || frame->moving > 1u ||
        frame->fixed != 1u - frame->moving)
        return NULL;
    return frame;
}

void terrain_shadows_camera(uint32_t camera, uint32_t props) {
    shadow_frame_t* frame;
    const guest_clut_t* c;
    if (!S.ready || !(frame = shadow_frame(camera, props)) || !(c = terrain_clut())) return;
    if (!engine_ramp_reserved(c->shade_scale, c->shade_offset, c->start) ||
        engine_ramp_brightness(frame->light_level, c->shaded_rows, c->start) <= ENGINE_SHADOW_BRIGHTNESS)
        return;
    frame->light_level = 0;
}
