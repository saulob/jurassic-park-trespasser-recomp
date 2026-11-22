#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex0;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec4 c = texture(tex0, in_uv);
    if (c.a < 0.5) discard;
    out_color = c;
}
