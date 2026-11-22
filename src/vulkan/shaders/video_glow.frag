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

const int GLOW_STEPS = 32;
const int GLOW_ROWS = 96;
const int GLOW_BAND = 8;

vec3 band(float row, int right) {
    float r = clamp(row, 0.0, float(pc.src.z - 1));
    int y0 = int(r);
    int y1 = min(y0 + 1, pc.src.z - 1);
    vec3 a = vec3(0.0);
    vec3 b = vec3(0.0);
    for (int k = 0; k < GLOW_BAND; k++) {
        int x = right != 0 ? pc.src.x - 1 - k : k;
        a += texelFetch(frame, ivec2(x, y0), 0).rgb;
        b += texelFetch(frame, ivec2(x, y1), 0).rgb;
    }
    return mix(a, b, r - float(y0)) / float(GLOW_BAND);
}

void main() {
    int gx = int(gl_FragCoord.x);
    int gy = int(gl_FragCoord.y);
    int right = gx >= GLOW_STEPS ? 1 : 0;
    float dist = (float(gx - right * GLOW_STEPS) + 0.5) / float(GLOW_STEPS);
    float rows = float(pc.src.z);
    float centre = (float(gy) + 0.5) / float(GLOW_ROWS) * rows - 0.5;
    float sigma = rows * (0.03 + 0.12 * dist);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int t = -12; t <= 12; t++) {
        float o = float(t) / 5.0;
        float w = exp(-0.5 * o * o);
        sum += band(centre + o * sigma, right) * w;
        total += w;
    }
    out_color = vec4(sum / total, 1.0);
}
