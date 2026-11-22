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

layout(set = 0, binding = 1) readonly buffer Texels { uint data[]; } texels;
layout(set = 0, binding = 2) readonly buffer Clut { uint data[]; } clut;

layout(location = 0) out vec4 out_color;

uint texel_byte(uint i) { return (texels.data[i >> 2] >> ((i & 3u) * 8u)) & 255u; }
uint clut_entry(uint i) { return (clut.data[i >> 1] >> ((i & 1u) * 16u)) & 65535u; }

uint poly_index() {
    vec2 q = floor(gl_FragCoord.xy * p.size.zw) - p.origin.xy;
    float w = p.w.x + p.w.y * q.x + p.w.z * q.y;
    float u = (p.u.x + p.u.y * q.x + p.u.z * q.y) / w;
    float v = (p.v.x + p.v.y * q.x + p.v.z * q.y) / w;
    bool clamped = p.clamp_range.w != 0.0;
    if (clamped) {
        u = clamp(u, p.clamp_range.x, p.clamp_range.y);
        v = clamp(v, p.clamp_range.x, p.clamp_range.z);
    }
    int iu = int(floor(u));
    int iv = int(floor(v));
    uint row = clamped ? uint(iv) * p.tex.y : uint(iv) << 9;
    return (row + (uint(iu) & p.tex.z)) & p.tex.w;
}

vec4 poly_color(uint pixel) {
    uvec3 c = uvec3(pixel >> 11, (pixel >> 5) & 63u, pixel & 31u);
    c = uvec3((c.r << 3) | (c.r >> 2), (c.g << 2) | (c.g >> 4), (c.b << 3) | (c.b >> 2));
    return vec4(vec3(c) / 255.0, 1.0);
}

void main() {
    out_color = poly_color(clut_entry(p.lut.x + texel_byte(p.tex.x + poly_index())));
}
