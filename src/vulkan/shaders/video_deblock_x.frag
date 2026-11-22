#version 450

layout(push_constant) uniform Video {
    ivec4 src;
    ivec4 opt;
    vec4 video;
    vec4 outp;
    vec4 tune;
    vec4 deblock;
    ivec4 stray;
} pc;

layout(set = 0, binding = 2) uniform sampler2D frame;

layout(location = 0) out vec4 out_color;

vec3 px(ivec2 p) {
    return texelFetch(frame, clamp(p, ivec2(0, 0), ivec2(pc.src.x - 1, pc.src.z - 1)), 0).rgb;
}

float chmax(vec3 v) { return max(v.r, max(v.g, v.b)); }

float calm(vec3 a, vec3 b, vec3 c) { return max(chmax(abs(b - a)), chmax(abs(c - b))); }

vec3 block_step(ivec2 e, ivec2 d) {
    ivec2 along = d.yx;
    ivec2 first = e - ((e.x * along.x + e.y * along.y) & 3) * along;
    float step_max = 0.0;
    float inner = 0.0;
    float darkest = 1.0;
    for (int k = 0; k < 4; k++) {
        ivec2 l = first + k * along;
        vec3 p0 = px(l - d);
        vec3 p1 = px(l - 2 * d);
        vec3 p2 = px(l - 3 * d);
        vec3 q0 = px(l);
        vec3 q1 = px(l + d);
        vec3 q2 = px(l + 2 * d);
        step_max = max(step_max, chmax(abs(q0 - p0)));
        inner = max(inner, max(calm(p0, p1, p2), calm(q0, q1, q2)));
        darkest = min(darkest, min(chmax(p0), chmax(q0)));
    }
    if (darkest <= 0.0 || step_max <= inner) return vec3(0.0);
    float w = (1.0 - smoothstep(pc.deblock.x, pc.deblock.y, step_max)) * (1.0 - smoothstep(pc.deblock.z, pc.deblock.w, inner));
    return (px(e) - px(e - d)) * w;
}

vec4 deblock_along(ivec2 p, ivec2 d) {
    int i = (p.x * d.x + p.y * d.y) & 3;
    int n = pc.src.x * d.x + pc.src.z * d.y;
    ivec2 first = p - i * d;
    vec3 c = px(p);
    int first_at = first.x * d.x + first.y * d.y;
    if (i < 2 && first_at > 0) c -= (i == 0 ? 0.375 : 0.125) * block_step(first, d);
    if (i >= 2 && first_at + 4 < n) c += (i == 3 ? 0.375 : 0.125) * block_step(first + 4 * d, d);
    return vec4(c, 1.0);
}

void main() { out_color = deblock_along(ivec2(gl_FragCoord.xy), ivec2(1, 0)); }
