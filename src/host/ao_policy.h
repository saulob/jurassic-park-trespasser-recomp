/* Ambient occlusion without a device: the Intel ASSAO preset behind each quality level, the projection the scene
 * really uses and which frames get the effect. */
#ifndef AO_POLICY_H
#define AO_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Direct3D vertices carry sz = rhw = fInvZAdjust / normalised depth = 0.1 / metres (CCamera::SetProperties stores
 * 0.1f / far at camera + 0x120, 0x0051FF0B), clamped to 0.99. */
#define AO_DEPTH_SCALE 0.1f
#define AO_FAR_LIMIT 4000.0f
#define AO_DEPTH_FLOOR (AO_DEPTH_SCALE / AO_FAR_LIMIT)

/* ASSAO_Settings, field for field. */
typedef struct {
    int quality_level;
    int blur_pass_count;
    float radius;
    float shadow_multiplier;
    float shadow_power;
    float shadow_clamp;
    float horizon_angle_threshold;
    float fade_out_from;
    float fade_out_to;
    float adaptive_quality_limit;
    float sharpness;
    float detail_shadow_strength;
} ao_preset_t;

/* JPT_AO_LOW..JPT_AO_ULTRA; 0 for Off or an unknown level. */
int ao_preset(int level, ao_preset_t* out);

/* Row-major, left-handed, as ASSAO_Inputs::ProjectionMatrix: x and y by the view's tangents, depth = 0.1 / z. */
void ao_projection(float tan_half_x, float tan_half_y, float m[16]);
/* The viewspace depth ASSAO decodes from a device depth with that matrix (ASSAODX11::UpdateConstants). */
float ao_decoded_depth(const float m[16], float device_depth);

/* The target's tangents from the camera's vertical one; 0 when the camera is unknown. */
int ao_view_tangents(float tan_half_h, unsigned target_w, unsigned target_h, float* tan_half_x, float* tan_half_y);

typedef struct {
    int level;     /* JPT_AO_* */
    int ready;     /* the effect exists and the target's depth is readable */
    int masked;    /* the frame's stencil mask was written */
    int scene;     /* a depth-writing draw since the last present */
    int gameplay;  /* the game shows its 3D view, not a menu */
    float tan_half_h;
} ao_frame_t;

int ao_frame_wanted(const ao_frame_t* f);

#ifdef __cplusplus
}
#endif

#endif
