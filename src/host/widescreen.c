/*
 * True widescreen: gameplay runs at a mode of the chosen shape (jpt_gameplay_mode) and the main camera, with the
 * render cameras copied from it, keeps the vertical field of view of the original 4:3 view (Hor+). The camera
 * projects, clips and culls before anything becomes a D3DTLVERTEX or a software span, so both renderers follow.
 */
#include "host.h"
#include "host_policy.h"

#include <math.h>
#include <string.h>

#include "jpt_display.h"

#define ENUM_MODES_VA 0x00502970u          /* fills the mode table; its byte argument lifts the 800-pixel width cap */
#define CAMERA_SET_PROPERTIES_VA 0x0051FD90u /* CCamera::SetProperties(const SProperties&), thiscall */
#define MAIN_CAMERA_HOLDER 0x0073F444u     /* [[holder] + 0x14] is the main camera */
#define PROPS_SIZE 0x58u
#define PROPS_IN_CAMERA 0x74u
#define PROP_PERSPECTIVE 0x34u
#define PROP_FAR 0x3Cu
#define PROP_TAN_HALF_WIDTH 0x48u
#define PROP_ASPECT 0x4Cu

static recomp_func_t s_enum_modes_lifted, s_set_properties_lifted;
static int s_wide;
static unsigned long s_corrections;

static float rdf(uint32_t va) { return *(float*)ADDR(va); }
static void wrf(uint32_t va, float v) { *(float*)ADDR(va) = v; }

static void enum_modes_uncapped(void) {
    if (!g_alias_entry) MEM8(g_esp + 4) = 1;
    s_enum_modes_lifted();
}

static uint32_t main_camera(void) {
    uint32_t holder = MEM32(MAIN_CAMERA_HOLDER);
    return holder ? MEM32(holder + 0x14) : 0;
}

/* The renderer draws each frame through cameras built from the main camera's stored properties (CRenderDB paint:
 * backdrop and scene), while terrain, occlusion and sky use the main camera itself; all must share one frustum. */
static int has_main_view(uint32_t self, uint32_t props) {
    uint32_t cam = main_camera(), own = cam + PROPS_IN_CAMERA;
    if (!cam) return 0;
    if (cam == self) return 1;
    return MEM32(props) == MEM32(own) && MEM32(props + 4) == MEM32(own + 4) &&
           MEM32(props + PROP_TAN_HALF_WIDTH) == MEM32(own + PROP_TAN_HALF_WIDTH) &&
           MEM32(props + PROP_ASPECT) == MEM32(own + PROP_ASPECT);
}

/* The caller's properties are the 4:3 ones; the camera gets tan(half width) widened by (W/H)/(4/3) and the true W/H
 * aspect, so its vertical extent is the 4:3 one. The stored properties are put back to the caller's values, because
 * callers copy them, edit and set them again, and savegames keep the angle. */
static void camera_set_properties(void) {
    uint32_t self = g_ecx, slot = g_esp + 4, props = MEM32(slot);
    uint8_t copy[PROPS_SIZE];
    float w, h, tan_half, aspect;
    double scale;
    int is_main;
    if (g_alias_entry || !props || !MEM8(props + PROP_PERSPECTIVE) || !has_main_view(self, props)) {
        s_set_properties_lifted();
        return;
    }
    is_main = self == main_camera();
    w = rdf(props);
    h = rdf(props + 4);
    tan_half = rdf(props + PROP_TAN_HALF_WIDTH);
    aspect = rdf(props + PROP_ASPECT);
    scale = (w > 0.0f && h > 0.0f) ? ((double)w / h) / (4.0 / 3.0) : 1.0;
    if (scale <= 1.0001 || !(tan_half > 0.0f)) {
        s_set_properties_lifted();
        return;
    }
    memcpy(copy, (const void*)ADDR(props), PROPS_SIZE);
    *(float*)(copy + PROP_TAN_HALF_WIDTH) = (float)(tan_half * scale);
    *(float*)(copy + PROP_ASPECT) = w / h;
    MEM32(slot) = (uint32_t)(uintptr_t)copy;
    s_set_properties_lifted();
    MEM32(slot) = props;
    wrf(self + PROPS_IN_CAMERA + PROP_TAN_HALF_WIDTH, tan_half);
    wrf(self + PROPS_IN_CAMERA + PROP_ASPECT, aspect);
    if (is_main && s_corrections++ < 4)
        host_log("widescreen", "main camera %.0fx%.0f (far %.0f): horizontal view x%.4f (tan %.4f -> %.4f), vertical "
                 "view kept", w, h, rdf(self + PROPS_IN_CAMERA + PROP_FAR), scale, tan_half, tan_half * scale);
}

int widescreen_install(int aspect) {
    if (!guest_override_install(ENUM_MODES_VA, enum_modes_uncapped, &s_enum_modes_lifted)) return 0;
    if (aspect <= 0 || aspect >= JPT_ASPECT_COUNT) return 1;
    if (!guest_override_install(CAMERA_SET_PROPERTIES_VA, camera_set_properties, &s_set_properties_lifted)) return 0;
    s_wide = 1;
    host_log("widescreen", "%s: Hor+ (vertical view of the 4:3 original, horizontal x%.4f)", jpt_aspects[aspect].key,
             (double)jpt_aspects[aspect].num / jpt_aspects[aspect].den / (4.0 / 3.0));
    return 1;
}

int widescreen_active(void) { return s_wide; }

float widescreen_tan_half_height(void) {
    uint32_t cam = main_camera(), props = cam + PROPS_IN_CAMERA;
    float aspect, tan_half;
    if (!cam || !guest_readable(props, PROPS_SIZE) || !MEM8(props + PROP_PERSPECTIVE)) return 0.0f;
    aspect = s_wide ? 4.0f / 3.0f : rdf(props + PROP_ASPECT);
    tan_half = rdf(props + PROP_TAN_HALF_WIDTH);
    return aspect > 0.0f && tan_half > 0.0f && isfinite(tan_half) ? tan_half / aspect : 0.0f;
}

/* The stored tan(half width) is the caller's 4:3 one; the projection keeps its vertical extent either way. */
float widescreen_pixels_per_metre(void) {
    uint32_t cam = main_camera(), props = cam + PROPS_IN_CAMERA;
    float aspect;
    if (!cam || !MEM8(props + PROP_PERSPECTIVE)) return 0.0f;
    aspect = s_wide ? 4.0f / 3.0f : rdf(props + PROP_ASPECT);
    if (!(aspect > 0.0f)) return 0.0f;
    return engine_view_pixels_per_metre(rdf(props + 4), rdf(props + PROP_TAN_HALF_WIDTH) / aspect);
}
