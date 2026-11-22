#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;   /* x=dir_x, y=dir_y, z=sharpness, w=0 */
    vec4 pass_params;
    vec4 pass_extra0;
    vec4 pass_extra1;
    vec4 pass_extra2;
} pc;

layout(set = 0, binding = 0) uniform sampler2D ao_tex;
layout(set = 0, binding = 1) uniform sampler2D lin_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

void main() {
    float c_ao = texture(ao_tex, in_uv).r;
    float c_z = texture(lin_depth, in_uv).r;
    vec2 step_uv = pc.pass_color.xy;
    float sharp = max(pc.pass_color.z, 0.01) * 4.0;

    float sum = c_ao * 0.30;
    float wsum = 0.30;
    const float w[2] = float[2](0.24, 0.11);

    for (int i = 1; i <= 2; i++) {
        vec2 off = step_uv * float(i);
        vec2 uvp = clamp(in_uv + off, vec2(0.001), vec2(0.999));
        vec2 uvn = clamp(in_uv - off, vec2(0.001), vec2(0.999));
        float zp = texture(lin_depth, uvp).r;
        float zn = texture(lin_depth, uvn).r;
        float wp = w[i - 1] * exp(-abs(zp - c_z) * sharp / max(c_z, 0.25));
        float wn = w[i - 1] * exp(-abs(zn - c_z) * sharp / max(c_z, 0.25));
        sum += texture(ao_tex, uvp).r * wp + texture(ao_tex, uvn).r * wn;
        wsum += wp + wn;
    }

    out_ao = clamp(sum / max(wsum, 1e-4), 0.0, 1.0);
}
