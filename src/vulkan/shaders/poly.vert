#version 450

layout(set = 0, binding = 0) uniform Poly {
    vec4 xy[32];
    vec4 u;
    vec4 v;
    vec4 w;
    vec4 origin;
    vec4 size;
    vec4 clamp_range;
    uvec4 tex;
    uvec4 lut;
    uvec4 bump;
} p;

void main() {
    uint id = uint(gl_VertexIndex);
    uint tri = id / 3u;
    uint corner = id - tri * 3u;
    uint k = corner == 0u ? 0u : tri + corner;
    vec4 pair = p.xy[k >> 1];
    vec2 q = (k & 1u) != 0u ? pair.zw : pair.xy;
    gl_Position = vec4(q.x / p.size.x * 2.0 - 1.0, q.y / p.size.y * 2.0 - 1.0, 0.0, 1.0);
}
