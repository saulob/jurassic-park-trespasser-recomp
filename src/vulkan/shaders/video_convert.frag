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

layout(set = 0, binding = 0) uniform usampler2D codes;
layout(set = 0, binding = 1) uniform sampler2D palette_map;

layout(location = 0) out vec4 out_color;

vec3 expand(uint c) {
    if (pc.opt.z != 0) return vec3(float((c >> 11) & 31u), float((c >> 5) & 63u), float(c & 31u)) / vec3(31.0, 63.0, 31.0);
    return vec3(float((c >> 10) & 31u), float((c >> 5) & 31u), float(c & 31u)) / 31.0;
}

uint code_at(int x, int y) {
    return texelFetch(codes, ivec2(clamp(x, 0, pc.src.x - 1), clamp(y, 0, pc.src.y - 1)), 0).r;
}

vec3 stored(int x, int y) {
    uint c = code_at(x, y);
    if (pc.opt.y != 0) {
        vec4 m = texelFetch(palette_map, ivec2(int(c & 255u), int(c >> 8)), 0);
        if (m.a > 0.5) return m.rgb;
    }
    return expand(c);
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 between_rows(int x, int s) {
    float vertical = 0.0;
    float best = 1e9;
    int pick = 0;
    for (int k = -1; k <= 1; k++) vertical += abs(luma(stored(x + k, s)) - luma(stored(x + k, s + 1)));
    for (int d = -2; d <= 2; d++) {
        if (d != 0) {
            float cost = 0.0;
            for (int j = -1; j <= 1; j++) cost += abs(luma(stored(x + d + j, s)) - luma(stored(x - d + j, s + 1)));
            if (cost < best) {
                best = cost;
                pick = d;
            }
        }
    }
    if (vertical < 0.06 || best >= 0.5 * vertical) pick = 0;
    return 0.5 * (stored(x + pick, s) + stored(x - pick, s + 1));
}

int stray_row(int x, int y) {
    if (pc.stray.x == 0 || y != pc.src.y - 1 || y < 3 || code_at(x, y) == 0u) return y;
    return (code_at(x, y - 1) | code_at(x, y - 2) | code_at(x, y - 3)) == 0u ? y - 1 : y;
}

void main() {
    int x = int(gl_FragCoord.x);
    int y = int(gl_FragCoord.y);
    if (pc.src.w == 0) {
        out_color = vec4(stored(x, stray_row(x, y)), 1.0);
        return;
    }
    int r = y - pc.opt.x;
    if (r < 0) {
        out_color = vec4(stored(x, 0), 1.0);
        return;
    }
    if ((r & 1) == 0) {
        out_color = vec4(stored(x, r >> 1), 1.0);
        return;
    }
    out_color = vec4(between_rows(x, r >> 1), 1.0);
}
