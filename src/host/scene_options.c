#include "scene_options.h"

#include "guest_abi.h"
#include "host_policy.h"

#define FOG_VALUE_TABLE GVA(0x00732874) /* the terrain CFog's float table; the objects' CFog follows that object */
#define FOG_INT_TABLE 0x1Cu             /* CFog: the fog band by distance, {length, data} */
#define FOG_INT_DATA 0x20u
#define FOG_FLOAT_TABLE 0x24u           /* CFog: the fog level by distance, {length, data} */
#define FOG_FLOAT_DATA 0x28u
#define FOG_TERRAIN_START 0x30u
#define FOG_SIZE 0x38u
#define FOG_ENTRIES 1024u
#define FOG_OBJECTS 2u
#define FOG_COLOR GVA(0x006D1E94)
/* CRasterD3D's background: the colour its Clear2 passes, which the device's fog application fills with the fog colour. */
#define RENDER_BACKGROUND GVA(0x006D0F14)
#define QUALITY_SETTINGS GVA(0x0066C4E8) /* qdQualitySettings[5] */
#define QUALITY_STRIDE 0x5Cu
#define QUALITY_SHADOWS 0x38u            /* bShadows: read by the terrain texture evaluation only */

static struct {
    int fog_off, shadows_off;
    int fog_logged, fog_refused, shadows_logged;
} S;

void scene_options_configure(int fog, int shadows) {
    S.fog_off = !fog;
    S.shadows_off = !shadows;
    S.fog_logged = S.fog_refused = S.shadows_logged = 0;
    host_log("graphics", "fog %s, shadows %s", fog ? "on" : "off", shadows ? "on" : "off");
}

static int fog_clear(uint32_t fog) {
    uint32_t bands, values;
    if (!guest_readable(fog, FOG_SIZE)) return 0;
    bands = MEM32(fog + FOG_INT_DATA);
    values = MEM32(fog + FOG_FLOAT_DATA);
    if (!bands || !values) return 0;
    if (MEM32(fog + FOG_INT_TABLE) != FOG_ENTRIES || MEM32(fog + FOG_FLOAT_TABLE) != FOG_ENTRIES ||
        !guest_writable(bands, FOG_ENTRIES * 4u) || !guest_writable(values, FOG_ENTRIES * 4u)) {
        if (!S.fog_refused++) host_log("graphics", "fog: the fog tables at 0x%08X are not the engine's; left as they are", fog);
        return 0;
    }
    return scene_fog_clear((int32_t*)ADDR(bands), (float*)ADDR(values), FOG_ENTRIES,
                           (float*)ADDR(fog + FOG_TERRAIN_START));
}

static void apply_fog(void) {
    uint32_t fog = FOG_VALUE_TABLE - FOG_FLOAT_TABLE;
    int cleared = 0;
    if (!S.fog_off) return;
    for (uint32_t i = 0; i < FOG_OBJECTS; i++) cleared |= fog_clear(fog + i * FOG_SIZE);
    if (cleared && !S.fog_logged++) host_log("graphics", "fog off: the world's distance fog is cleared (the sky keeps its haze)");
}

static void apply_shadows(void) {
    uint32_t rows = QUALITY_SETTINGS;
    if (!S.shadows_off || !guest_writable(rows, (ENGINE_RENDER_QUALITY_TOP + 1u) * QUALITY_STRIDE)) return;
    for (uint32_t i = 0; i <= ENGINE_RENDER_QUALITY_TOP; i++) {
        uint32_t row = rows + i * QUALITY_STRIDE + QUALITY_SHADOWS;
        if (MEM8(row) != 1) continue;
        MEM8(row) = 0;
        if (!S.shadows_logged++) host_log("graphics", "shadows off: the terrain draws no object shadows");
    }
}

void scene_options_step(void) {
    apply_fog();
    apply_shadows();
}

int scene_fog_color(uint32_t* color) {
    uint32_t value, background;
    scene_options_step();
    if (!guest_read32(FOG_COLOR, &value)) return 0;
    *color = value | 0xFF000000u;
    if (guest_read32(RENDER_BACKGROUND, &background) && background != *color) guest_write32(RENDER_BACKGROUND, *color);
    return 1;
}
