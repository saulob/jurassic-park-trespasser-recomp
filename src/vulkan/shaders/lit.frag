#version 450

layout(push_constant) uniform Push {
    vec4 target;
    uvec4 draw_u; /* x=flags, y=blend_mode, z=alpha_func, w=pad */
    vec4 fog_color; /* rgb=fog, a=alpha_ref */
    vec4 fx_water;
    vec4 fx_sky;
    vec4 fx_extra;
    vec4 fx_water3;
} pc;

layout(set = 0, binding = 0) uniform texture2DArray world_tex[16];
layout(set = 0, binding = 1) uniform sampler world_samp[20];

layout(location = 0) in vec4 in_dark;
layout(location = 1) in vec4 in_full;
layout(location = 2) in vec2 in_uv;
layout(location = 3) flat in uint in_layer;
layout(location = 4) in vec4 in_light;
layout(location = 5) flat in vec4 in_material;
layout(location = 6) flat in vec2 in_shade;
layout(location = 7) flat in uint in_normal;

layout(location = 0) out vec4 out_color;

const uint F_TEX_ALPHA = 2u;
const uint F_KEY_TEST = 4u;
const uint F_ALPHA_TEST = 8u;
const uint F_FOG = 16u;

vec4 sample_array(uint slot, uint s_idx, vec3 c, vec2 gx, vec2 gy) {
    vec4 r = vec4(1.0);
    switch (slot) {
    case 0u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[0], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[0], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 1u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[1], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[1], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 2u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[2], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[2], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 3u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[3], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[3], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 4u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[4], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[4], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 5u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[5], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[5], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 6u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[6], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[6], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 7u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[7], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[7], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 8u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[8], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[8], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 9u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[9], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[9], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 10u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[10], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[10], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 11u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[11], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[11], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 12u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[12], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[12], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 13u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[13], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[13], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 14u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[14], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[14], world_samp[0]), c, gx, gy); break;
        }
        break;
    case 15u:
        switch (s_idx) {
        case 1u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[1]), c, gx, gy); break;
        case 2u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[2]), c, gx, gy); break;
        case 3u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[3]), c, gx, gy); break;
        case 4u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[4]), c, gx, gy); break;
        case 5u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[5]), c, gx, gy); break;
        case 6u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[6]), c, gx, gy); break;
        case 7u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[7]), c, gx, gy); break;
        case 8u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[8]), c, gx, gy); break;
        case 9u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[9]), c, gx, gy); break;
        case 10u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[10]), c, gx, gy); break;
        case 11u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[11]), c, gx, gy); break;
        case 12u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[12]), c, gx, gy); break;
        case 13u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[13]), c, gx, gy); break;
        case 14u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[14]), c, gx, gy); break;
        case 15u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[15]), c, gx, gy); break;
        case 16u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[16]), c, gx, gy); break;
        case 17u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[17]), c, gx, gy); break;
        case 18u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[18]), c, gx, gy); break;
        case 19u: r = textureGrad(sampler2DArray(world_tex[15], world_samp[19]), c, gx, gy); break;
        default: r = textureGrad(sampler2DArray(world_tex[15], world_samp[0]), c, gx, gy); break;
        }
        break;
    }
    return r;
}

vec4 sample_world(uint layer, vec2 uv, vec2 gx, vec2 gy) {
    vec3 c = vec3(uv, float(layer & 0xFFFFu));
    uint slot = (layer >> 16u) & 15u;
    uint s_idx = (layer >> 24u) & 31u;
    return sample_array(slot, s_idx, c, gx, gy);
}

bool alpha_pass(float a) {
    float r = pc.fog_color.a;
    uint fn = pc.draw_u.z;
    if (fn == 1u) return false;
    if (fn == 2u) return a < r;
    if (fn == 3u) return abs(a - r) < 0.5 / 255.0;
    if (fn == 4u) return a <= r;
    if (fn == 5u) return a > r;
    if (fn == 6u) return abs(a - r) >= 0.5 / 255.0;
    if (fn == 7u) return a >= r;
    return true;
}

/* jpt_lit_pixel */
vec4 lit_color() {
    uint flags = pc.draw_u.x;
    uint tf = (in_layer >> 20u) & 7u;
    vec2 gx = dFdx(in_uv);
    vec2 gy = dFdy(in_uv);
    vec4 t = sample_world(in_layer, in_uv, gx, gy);
    vec4 m = sample_world(in_normal, in_uv, gx, gy);
    float begins = in_material.z * 0.998749197;
    float len = length(in_light.xyz);
    float light = len > 1e-6 ? dot(m.rgb * 2.0 - 1.0, in_light.xyz / len) : 0.0;
    vec3 over = vec3(0.0);
    vec4 c;
    if (((tf & F_KEY_TEST) != 0u) && t.a < 0.5) discard;
    light = (light >= 0.0 ? light * in_light.w : 0.0) + in_material.w;
    float value = light * in_material.x;
    if (in_material.y > 0.0) value = mix(value, in_material.y, clamp((light - begins) / (0.998749197 - begins), 0.0, 1.0) * m.a);
    float shown = max(value >= 1.0 ? 1.0 : pow(max(value, 0.0), in_shade.x), in_shade.y);
    if (in_material.y > 0.0 && value >= 1.0) over = (value - 1.0) * in_full.rgb;
    c.rgb = mix(over, mix(in_dark.rgb, in_full.rgb, shown), t.rgb);
    c.a = ((tf & F_TEX_ALPHA) != 0u) ? t.a : in_dark.a;
    if (((flags & F_ALPHA_TEST) != 0u) && !alpha_pass(c.a)) discard;
    if ((flags & F_FOG) != 0u) c.rgb = mix(pc.fog_color.rgb, c.rgb, clamp(in_full.a, 0.0, 1.0));
    return clamp(c, 0.0, 1.0);
}

void main() {
    out_color = lit_color();
}
