#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;  /* x=depth_out, y=ao_depth_scale(0.1), z=ao_far_limit(4000.0), w=depth_floor */
    vec4 pass_extra0;
    vec4 pass_extra1;
    vec4 pass_extra2;
} pc;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_linear_z;

void main() {
    float d = texture(depth_tex, in_uv).r;
    float scale = pc.pass_params.y;
    float far_limit = pc.pass_params.z;
    float floor_d = pc.pass_params.w;
    out_linear_z = (d > floor_d) ? min(scale / d, far_limit) : far_limit;
}
