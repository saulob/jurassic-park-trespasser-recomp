/*
 * True widescreen: the game runs gameplay at a widescreen mode of its own (jpt_aspects) and its main camera keeps the
 * vertical field of view of the original 4:3 view (Hor+). The camera projects, clips and culls before anything becomes
 * a D3DTLVERTEX or a software span, so both renderers and the 2D overlay follow without other changes.
 */
#include "host.h"

#include <string.h>

#include "jpt_display.h"

#define ENUM_MODES_VA 0x00502970u          /* fills the mode table; its byte argument lifts the 800-pixel width cap */
#define CAMERA_SET_PROPERTIES_VA 0x0051FD90u /* CCamera::SetProperties(const SProperties&), thiscall */
#define MAIN_CAMERA_HOLDER 0x0073F444u     /* [[holder] + 0x14] is the main camera */
#define PROPS_SIZE 0x58u
#define PROPS_IN_CAMERA 0x74u
#define PROP_PERSPECTIVE 0x34u
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

static int is_main_camera(uint32_t self) {
    uint32_t holder = MEM32(MAIN_CAMERA_HOLDER);
    return holder && MEM32(holder + 0x14) == self;
}

/* The caller's properties are the 4:3 ones; the camera gets tan(half width) widened by (W/H)/(4/3) and the true W/H
 * aspect, so its vertical extent is the 4:3 one. The stored properties are put back to the caller's values, because
 * callers copy them, edit and set them again, and savegames keep the angle. */
static void camera_set_properties(void) {
    uint32_t self = g_ecx, slot = g_esp + 4, props = MEM32(slot);
    uint8_t copy[PROPS_SIZE];
    float w, h, tan_half, aspect;
    double scale;
    if (g_alias_entry || !props || !is_main_camera(self) || !MEM8(props + PROP_PERSPECTIVE)) {
        s_set_properties_lifted();
        return;
    }
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
    if (s_corrections++ < 4)
        host_log("widescreen", "main camera %.0fx%.0f: horizontal view x%.4f (tan %.4f -> %.4f), vertical view kept",
                 w, h, scale, tan_half, tan_half * scale);
}

static int redirect(uint32_t va, recomp_func_t replacement, recomp_func_t* body, const char* name) {
    if (guest_override_redirect(va, replacement, body)) return 1;
    host_log("widescreen", "sub_%08X (%s) has no incremental-link thunk: widescreen is off", va, name);
    return 0;
}

int widescreen_install(int aspect) {
    const jpt_aspect_t* a = &jpt_aspects[aspect];
    if (aspect <= 0 || aspect >= JPT_ASPECT_COUNT) return 0;
    if (!redirect(ENUM_MODES_VA, enum_modes_uncapped, &s_enum_modes_lifted, "mode table") ||
        !redirect(CAMERA_SET_PROPERTIES_VA, camera_set_properties, &s_set_properties_lifted, "CCamera::SetProperties"))
        return 0;
    s_wide = 1;
    dd_set_extra_mode((DWORD)a->guest_w, (DWORD)a->guest_h);
    host_log("widescreen", "%s: gameplay at %dx%d, Hor+ (vertical view of the 4:3 original, horizontal x%.4f); "
             "menus stay 640x480", a->key, a->guest_w, a->guest_h, jpt_hor_plus_scale(a->guest_w, a->guest_h));
    return 1;
}

int widescreen_active(void) { return s_wide; }
