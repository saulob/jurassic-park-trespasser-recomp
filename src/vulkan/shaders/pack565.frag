#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out uint out_rgb565;

void main() {
    uvec3 k = uvec3(clamp(texture(tex0, in_uv).rgb, 0.0, 1.0) * 255.0 + 0.5);
    uvec3 q = (k * uvec3(31u, 63u, 31u) + uvec3(127u)) / 255u;
    out_rgb565 = (q.r << 11u) | (q.g << 5u) | q.b;
}
