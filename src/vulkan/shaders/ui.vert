#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_col;

layout(push_constant) uniform Projection {
    vec2 scale;
    vec2 translate;
} pc;

layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;

void main() {
    v_col = in_col;
    v_uv = in_uv;
    gl_Position = vec4(in_pos * pc.scale + pc.translate, 0.0, 1.0);
}
