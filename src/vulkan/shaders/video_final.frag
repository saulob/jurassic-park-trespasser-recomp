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
layout(set = 0, binding = 3) uniform sampler2D glow;
layout(set = 0, binding = 4) uniform sampler2D gamma_lut;

layout(location = 0) out vec4 out_color;

const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
const int GLOW_STEPS = 32;

vec3 catmull(vec2 s) {
    vec2 t = s - 0.5;
    vec2 f = floor(t);
    vec2 a = t - f;
    ivec2 b = ivec2(f);
    ivec2 last = ivec2(pc.src.x - 1, pc.src.z - 1);
    vec2 w0 = ((-0.5 * a + 1.0) * a - 0.5) * a;
    vec2 w1 = (1.5 * a - 2.5) * a * a + 1.0;
    vec2 w2 = ((-1.5 * a + 2.0) * a + 0.5) * a;
    vec2 w3 = (0.5 * a - 0.5) * a * a;
    vec4 wx = vec4(w0.x, w1.x, w2.x, w3.x);
    vec4 wy = vec4(w0.y, w1.y, w2.y, w3.y);
    vec3 sum = vec3(0.0);
    vec3 lo = vec3(1e9);
    vec3 hi = vec3(-1e9);
    for (int j = 0; j < 4; j++) {
        vec3 row = vec3(0.0);
        for (int k = 0; k < 4; k++) {
            vec3 c = texelFetch(frame, clamp(b + ivec2(k - 1, j - 1), ivec2(0, 0), last), 0).rgb;
            row += c * wx[k];
            if ((j == 1 || j == 2) && (k == 1 || k == 2)) {
                lo = min(lo, c);
                hi = max(hi, c);
            }
        }
        sum += row * wy[j];
    }
    return clamp(sum, lo, hi);
}

vec3 ramp(vec3 c) {
    if (pc.opt.w == 0) return c;
    ivec3 k = ivec3(round(c * 255.0));
    return vec3(texelFetch(gamma_lut, ivec2(k.r, 0), 0).r, texelFetch(gamma_lut, ivec2(k.g, 0), 0).g,
                texelFetch(gamma_lut, ivec2(k.b, 0), 0).b);
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 c = vec3(0.0);
    bool rows = p.y >= pc.video.y && p.y < pc.video.y + pc.video.w;
    if (rows && p.x >= pc.video.x && p.x < pc.video.x + pc.video.z) {
        c = catmull((p - pc.video.xy) * vec2(float(pc.src.x), float(pc.src.z)) / pc.video.zw);
    } else if (rows && pc.outp.w > 0.0) {
        bool right = p.x >= pc.video.x + pc.video.z;
        float dist = clamp((right ? p.x - pc.video.x - pc.video.z : pc.video.x - p.x) / max(pc.outp.z, 1.0), 0.0, 1.0);
        float u = (right ? 0.5 : 0.0) + clamp(dist, 0.5 / float(GLOW_STEPS), 1.0 - 0.5 / float(GLOW_STEPS)) * 0.5;
        vec3 g = textureLod(glow, vec2(u, (p.y - pc.video.y) / pc.video.w), 0.0).rgb;
        ivec2 q = ivec2(p) & 3;
        c = g * mix(pc.tune.y, pc.tune.z, smoothstep(0.0, 1.0, dist)) + (bayer[q.y * 4 + q.x] / 16.0 - 0.46875) / 255.0;
    }
    out_color = vec4(ramp(clamp(c, 0.0, 1.0)), 1.0);
}
