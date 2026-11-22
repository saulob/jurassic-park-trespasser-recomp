#version 450

layout(push_constant) uniform Push {
    vec4 target;
    uvec4 draw_u;
    vec4 fog_color;
    vec4 fx_water;
    vec4 fx_sky;
    vec4 fx_extra;
    vec4 fx_water3;
} pc;

layout(set = 0, binding = 0) uniform texture2DArray world_tex[16];
layout(set = 0, binding = 1) uniform sampler world_samp[5];

layout(location = 0) in vec4 in_diffuse;
layout(location = 1) in vec4 in_specular;
layout(location = 2) flat in vec4 in_flat_diffuse;
layout(location = 3) flat in vec4 in_flat_specular;
layout(location = 4) in vec2 in_uv;
layout(location = 5) flat in uint in_layer;
layout(location = 6) noperspective in float in_rhw;

layout(location = 0) out vec4 out_color;

const uint F_TEXTURED = 1u;
const uint F_TEX_ALPHA = 2u;
const uint F_KEY_TEST = 4u;
const uint F_ALPHA_TEST = 8u;
const uint F_FOG = 16u;
const uint F_SPECULAR = 32u;
const uint F_FLAT = 64u;

#define SAMPLE_SLOT(T) \
    (s_idx == 1u ? textureGrad(sampler2DArray(world_tex[T], world_samp[1]), c, gx, gy) \
   : s_idx == 2u ? textureGrad(sampler2DArray(world_tex[T], world_samp[2]), c, gx, gy) \
   : s_idx == 3u ? textureGrad(sampler2DArray(world_tex[T], world_samp[3]), c, gx, gy) \
   : s_idx == 4u ? textureGrad(sampler2DArray(world_tex[T], world_samp[4]), c, gx, gy) \
                 : textureGrad(sampler2DArray(world_tex[T], world_samp[0]), c, gx, gy))

vec4 sample_world(uint layer, vec2 uv, vec2 gx, vec2 gy) {
    vec3 c = vec3(uv, float(layer & 0xFFFFu));
    uint slot = (layer >> 16u) & 15u;
    uint s_idx = (layer >> 24u) & 7u;
    switch (slot) {
    case 1u: return SAMPLE_SLOT(1);
    case 2u: return SAMPLE_SLOT(2);
    case 3u: return SAMPLE_SLOT(3);
    case 4u: return SAMPLE_SLOT(4);
    case 5u: return SAMPLE_SLOT(5);
    case 6u: return SAMPLE_SLOT(6);
    case 7u: return SAMPLE_SLOT(7);
    case 8u: return SAMPLE_SLOT(8);
    case 9u: return SAMPLE_SLOT(9);
    case 10u: return SAMPLE_SLOT(10);
    case 11u: return SAMPLE_SLOT(11);
    case 12u: return SAMPLE_SLOT(12);
    case 13u: return SAMPLE_SLOT(13);
    case 14u: return SAMPLE_SLOT(14);
    case 15u: return SAMPLE_SLOT(15);
    default: return SAMPLE_SLOT(0);
    }
}

void main() {
    vec2 dd = in_uv - pc.fx_sky.xy;
    float all_d = sqrt(dot(dd, dd) + pc.fx_sky.z * pc.fx_sky.z);
    float v = all_d > 0.0 ? sqrt(dot(dd, dd)) / all_d : 0.0;
    float f = v <= pc.fx_sky.w ? 0.0 : (v >= pc.fx_extra.x ? 1.0 : (v - pc.fx_sky.w) / (pc.fx_extra.x - pc.fx_sky.w));
    float sky_fog = 1.0 - f;

    uint flags = pc.draw_u.x;
    uint blend_mode = pc.draw_u.y;
    vec4 d = ((flags & F_FLAT) != 0u) ? in_flat_diffuse : in_diffuse;
    vec3 s = ((flags & F_FLAT) != 0u) ? in_flat_specular.rgb : in_specular.rgb;
    vec4 c = d;
    uint tf = (in_layer >> 20u) & 7u;
    vec2 gx = dFdx(in_uv);
    vec2 gy = dFdy(in_uv);

    if ((tf & F_TEXTURED) != 0u) {
        vec4 t = sample_world(in_layer, in_uv, gx, gy);
        if (((tf & F_KEY_TEST) != 0u) && t.a < 0.5) discard;
        if ((tf & F_TEX_ALPHA) == 0u) t.a = 1.0;
        if (blend_mode == 1u || blend_mode == 7u) c = t;
        else if (blend_mode == 3u) c = vec4(mix(d.rgb, t.rgb, t.a), d.a);
        else if (blend_mode == 4u) c = t * d;
        else if (blend_mode == 8u) c = vec4(clamp(t.rgb + d.rgb, 0.0, 1.0), d.a);
        else c = vec4(t.rgb * d.rgb, ((tf & F_TEX_ALPHA) != 0u) ? t.a : d.a);
    }

    if ((flags & F_SPECULAR) != 0u) c.rgb += s;
    if ((flags & F_FOG) != 0u) c.rgb = mix(pc.fog_color.rgb, c.rgb, clamp(sky_fog, 0.0, 1.0));
    out_color = clamp(c, 0.0, 1.0);
}
