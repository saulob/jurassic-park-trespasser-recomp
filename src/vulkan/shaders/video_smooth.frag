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

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float smooth_weight(vec3 c, vec3 n) {
    vec3 e = abs(n - c);
    return clamp((pc.tune.w - max(e.r, max(e.g, e.b))) / (pc.tune.w - pc.tune.x), 0.0, 1.0);
}

void main() {
    const ivec2 pairs[4] = ivec2[4](ivec2(1, 0), ivec2(0, 1), ivec2(1, 1), ivec2(1, -1));
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(pc.src.x - 1, pc.src.z - 1);
    vec3 c = texelFetch(frame, p, 0).rgb;
    vec3 sum = c;
    float lc = luma(c);
    float total = 1.0;
    for (int i = 0; i < 4; i++) {
        vec3 a = texelFetch(frame, clamp(p - pairs[i], ivec2(0, 0), last), 0).rgb;
        vec3 b = texelFetch(frame, clamp(p + pairs[i], ivec2(0, 0), last), 0).rgb;
        float da = luma(a) - lc;
        float db = luma(b) - lc;
        float k = i < 2 ? 1.0 / 6.0 : 0.25;
        bool extremum = da * db > 0.0;
        float wa = (extremum || abs(da) < 0.002) ? smooth_weight(c, a) * k : 0.0;
        float wb = (extremum || abs(db) < 0.002) ? smooth_weight(c, b) * k : 0.0;
        sum += a * wa + b * wb;
        total += wa + wb;
    }
    out_color = vec4(sum / total, 1.0);
}
