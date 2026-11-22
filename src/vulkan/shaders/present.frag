#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D gamma_lut;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 c = clamp(texture(tex0, in_uv), 0.0, 1.0);
    ivec3 k = ivec3(round(c.rgb * 255.0));
    float r = texelFetch(gamma_lut, ivec2(k.r, 0), 0).r;
    float g = texelFetch(gamma_lut, ivec2(k.g, 0), 0).g;
    float b = texelFetch(gamma_lut, ivec2(k.b, 0), 0).b;
    out_color = vec4(r, g, b, c.a);
}
