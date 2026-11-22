#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;
    vec4 pass_extra0;
    vec4 pass_extra1;
    vec4 pass_extra2;
} pc;

layout(set = 0, binding = 0) uniform sampler2D ao_tex;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    float ao = clamp(texture(ao_tex, in_uv).r, 0.0, 1.0);
    out_color = vec4(ao, ao, ao, 1.0);
}
