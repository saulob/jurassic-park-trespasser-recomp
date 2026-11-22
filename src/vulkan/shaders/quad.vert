#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;
    vec4 pass_params;
} pc;

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 k = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 pos = mix(pc.pass_rect.xy, pc.pass_rect.zw, k);
    gl_Position = vec4(pos, pc.pass_params.x, 1.0);
    out_uv = mix(pc.pass_uv.xy, pc.pass_uv.zw, k);
}
