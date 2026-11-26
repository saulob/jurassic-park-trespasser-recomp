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

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_diffuse;
layout(location = 2) in vec4 in_specular;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in uint in_layer;
layout(location = 5) in vec4 in_light;
layout(location = 6) in vec4 in_material;
layout(location = 7) in vec2 in_shade;
layout(location = 8) in uint in_normal;

layout(location = 0) out vec4 out_dark;
layout(location = 1) out vec4 out_full;
layout(location = 2) out vec2 out_uv;
layout(location = 3) flat out uint out_layer;
layout(location = 4) out vec4 out_light;
layout(location = 5) flat out vec4 out_material;
layout(location = 6) flat out vec2 out_shade;
layout(location = 7) flat out uint out_normal;

void main() {
    float w = in_pos.w > 1e-20 ? 1.0 / in_pos.w : 1.0;
    vec2 ndc = vec2(
        (in_pos.x + 0.5) * pc.target.z * 2.0 - 1.0,
        (in_pos.y + 0.5) * pc.target.w * 2.0 - 1.0
    );
    gl_Position = vec4(ndc * w, clamp(in_pos.z, 0.0, 1.0) * w, w);
    out_dark = in_diffuse;
    out_full = in_specular;
    out_uv = in_uv;
    out_layer = in_layer;
    out_light = in_light;
    out_material = in_material;
    out_shade = in_shade;
    out_normal = in_normal;
}
