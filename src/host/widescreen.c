/*
 * True widescreen and field of view: gameplay runs at a mode of the chosen shape (jpt_gameplay_mode) and the main
 * camera, with the render cameras copied from it, keeps the vertical field of view of the 4:3 view (Hor+). The camera
 * projects, clips and culls before anything becomes a D3DTLVERTEX or a software span, so both renderers follow.
 */
#include "host.h"
#include "host_policy.h"

#include <math.h>
#include <string.h>

#include "fov_policy.h"
#include "jpt_config.h"
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
#define PROP_ZOOM 0x50u
#define LOG_LIMIT 8

static recomp_func_t s_enum_modes_lifted, s_set_properties_lifted;
static int s_wide, s_installed, s_fov = JPT_FOV_DEFAULT, s_fov_pending;
static unsigned long s_logs;

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

/* The caller's properties are the game's 4:3 ones; the camera gets their angle scaled by the field of view, widened
 * by (W/H)/(4/3) with the true W/H aspect, so its vertical extent is the 4:3 one. The stored properties are put back to
 * the caller's values, because callers copy them, edit and set them again, and savegames keep the angle. */
static void camera_set_properties(void) {
    uint32_t self = g_ecx, slot = g_esp + 4, props = MEM32(slot);
    uint8_t copy[PROPS_SIZE];
    float w, h, tan_half, aspect;
    double scale, view_tan;
    if (g_alias_entry || !props || !MEM8(props + PROP_PERSPECTIVE) || !has_main_view(self, props)) {
        s_set_properties_lifted();
        return;
    }
    w = rdf(props);
    h = rdf(props + 4);
    tan_half = rdf(props + PROP_TAN_HALF_WIDTH);
    aspect = rdf(props + PROP_ASPECT);
    scale = s_wide ? fov_hor_plus(w, h) : 1.0;
    if (scale <= 1.0001) scale = 1.0;
    view_tan = fov_tan_half(tan_half, s_fov) * scale;
    if (!(tan_half > 0.0f) || (scale == 1.0 && (float)view_tan == tan_half)) {
        s_set_properties_lifted();
        return;
    }
    memcpy(copy, (const void*)ADDR(props), PROPS_SIZE);
    *(float*)(copy + PROP_TAN_HALF_WIDTH) = (float)view_tan;
    if (scale > 1.0) *(float*)(copy + PROP_ASPECT) = w / h;
    MEM32(slot) = (uint32_t)(uintptr_t)copy;
    s_set_properties_lifted();
    MEM32(slot) = props;
    wrf(self + PROPS_IN_CAMERA + PROP_TAN_HALF_WIDTH, tan_half);
    wrf(self + PROPS_IN_CAMERA + PROP_ASPECT, aspect);
    if (self == main_camera() && s_logs++ < LOG_LIMIT)
        host_log("widescreen", "main camera %.0fx%.0f (far %.0f): field of view %d, horizontal x%.4f, tan %.4f -> %.4f "
                 "(%.1f x %.1f degrees)", w, h, rdf(self + PROPS_IN_CAMERA + PROP_FAR), s_fov, scale, tan_half, view_tan,
                 fov_horizontal_degrees(view_tan / scale, s_wide ? w : 4.0, s_wide ? h : 3.0),
                 fov_vertical_degrees(view_tan / scale));
}

int widescreen_install(int aspect) {
    int wide = aspect > 0 && aspect < JPT_ASPECT_COUNT;
    if (!guest_override_install(ENUM_MODES_VA, enum_modes_uncapped, &s_enum_modes_lifted)) return 0;
    if (!guest_override_install(CAMERA_SET_PROPERTIES_VA, camera_set_properties, &s_set_properties_lifted)) return 0;
    s_installed = 1;
    s_wide = wide;
    if (wide)
        host_log("widescreen", "%s: Hor+ (vertical view of the 4:3 original, horizontal x%.4f)", jpt_aspects[aspect].key,
                 (double)jpt_aspects[aspect].num / jpt_aspects[aspect].den / (4.0 / 3.0));
    host_log("widescreen", "field of view %d (degrees across the 4:3 view; the game's own is %d)", s_fov,
             FOV_GAME_DEGREES);
    return 1;
}

int widescreen_active(void) { return s_wide; }

void widescreen_set_fov(int degrees) {
    int fov = jpt_fov_clamp(degrees);
    if (fov == s_fov) return;
    s_fov = fov;
    s_fov_pending = 1;
    s_logs = 0;
}

int widescreen_fov(void) { return s_fov; }

/* Called where the game itself could set its camera (its UI loop): the main camera takes its own stored properties
 * again, through the wrapper above, so nothing but its projection changes. */
void widescreen_apply_fov(void) {
    uint32_t cam, esp = g_esp, arg;
    if (!s_fov_pending || !s_installed) return;
    s_fov_pending = 0;
    cam = main_camera();
    if (!cam || !guest_readable(cam + PROPS_IN_CAMERA, PROPS_SIZE) || !MEM8(cam + PROPS_IN_CAMERA + PROP_PERSPECTIVE))
        return;
    g_esp -= (PROPS_SIZE + 15u) & ~15u;
    arg = g_esp;
    memcpy((void*)ADDR(arg), (const void*)ADDR(cam + PROPS_IN_CAMERA), PROPS_SIZE);
    if (!guest_call(CAMERA_SET_PROPERTIES_VA, cam, &arg, 1, 0, NULL))
        host_log("widescreen", "field of view %d could not be applied to the main camera", s_fov);
    g_esp = esp;
}

static float view_tan_half_width(uint32_t props) { return (float)fov_tan_half(rdf(props + PROP_TAN_HALF_WIDTH), s_fov); }

float widescreen_tan_half_height(void) {
    uint32_t cam = main_camera(), props = cam + PROPS_IN_CAMERA;
    float aspect, tan_half;
    if (!cam || !guest_readable(props, PROPS_SIZE) || !MEM8(props + PROP_PERSPECTIVE)) return 0.0f;
    aspect = s_wide ? 4.0f / 3.0f : rdf(props + PROP_ASPECT);
    tan_half = view_tan_half_width(props);
    return aspect > 0.0f && tan_half > 0.0f && isfinite(tan_half) ? tan_half / aspect : 0.0f;
}

/* What the main camera projects with: SetProperties divides both tangents by fZoomFactor. */
float widescreen_view_tan_half_height(void) {
    uint32_t cam = main_camera(), props = cam + PROPS_IN_CAMERA;
    float tan_half = widescreen_tan_half_height(), zoom;
    if (!(tan_half > 0.0f)) return 0.0f;
    zoom = rdf(props + PROP_ZOOM);
    return zoom > 0.0f && isfinite(zoom) ? tan_half / zoom : tan_half;
}

/* The stored tan(half width) is the caller's 4:3 one; the projection keeps its vertical extent either way. */
float widescreen_pixels_per_metre(void) {
    uint32_t cam = main_camera(), props = cam + PROPS_IN_CAMERA;
    float aspect;
    if (!cam || !MEM8(props + PROP_PERSPECTIVE)) return 0.0f;
    aspect = s_wide ? 4.0f / 3.0f : rdf(props + PROP_ASPECT);
    if (!(aspect > 0.0f)) return 0.0f;
    return engine_view_pixels_per_metre(rdf(props + 4), view_tan_half_width(props) / aspect);
}
