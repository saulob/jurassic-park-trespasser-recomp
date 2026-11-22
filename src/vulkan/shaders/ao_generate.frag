#version 450

layout(push_constant) uniform PassPush {
    vec4 pass_rect;
    vec4 pass_uv;
    vec4 pass_color;   /* x=radius, y=shadow_mult, z=shadow_power, w=shadow_clamp */
    vec4 pass_params;  /* x=0, y=horizon_thresh, z=fade_from, w=fade_to */
    vec4 pass_extra0;  /* x=tan_half_x, y=tan_half_y, z=inv_w, w=inv_h */
    vec4 pass_extra1;  /* x=tap_count, y=far_limit, z=0, w=0 */
    vec4 pass_extra2;
} pc;

layout(set = 0, binding = 0) uniform sampler2D lin_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

vec3 reconstruct_pos(vec2 uv, float z) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * pc.pass_extra0.x * z, ndc.y * pc.pass_extra0.y * z, z);
}

void main() {
    float far_limit = pc.pass_extra1.y;
    float z = texture(lin_depth, in_uv).r;
    if (z >= far_limit * 0.99 || z <= 0.001) {
        out_ao = 1.0;
        return;
    }

    vec2 px = pc.pass_extra0.zw;
    vec3 P = reconstruct_pos(in_uv, z);

    float zl = texture(lin_depth, in_uv - vec2(px.x, 0.0)).r;
    float zr = texture(lin_depth, in_uv + vec2(px.x, 0.0)).r;
    float zu = texture(lin_depth, in_uv - vec2(0.0, px.y)).r;
    float zd = texture(lin_depth, in_uv + vec2(0.0, px.y)).r;

    vec3 Pl = reconstruct_pos(in_uv - vec2(px.x, 0.0), zl);
    vec3 Pr = reconstruct_pos(in_uv + vec2(px.x, 0.0), zr);
    vec3 Pu = reconstruct_pos(in_uv - vec2(0.0, px.y), zu);
    vec3 Pd = reconstruct_pos(in_uv + vec2(0.0, px.y), zd);

    vec3 dpdx = (abs(zr - z) < abs(z - zl)) ? (Pr - P) : (P - Pl);
    vec3 dpdy = (abs(zd - z) < abs(z - zu)) ? (Pd - P) : (P - Pu);
    vec3 N = normalize(cross(dpdy, dpdx));
    if (N.z > 0.0) N = -N;

    float radius = pc.pass_color.x;
    float shadow_mult = pc.pass_color.y;
    float shadow_pow = pc.pass_color.z;
    float shadow_clamp = pc.pass_color.w;
    float bias = pc.pass_params.y;
    float fade_from = pc.pass_params.z;
    float fade_to = pc.pass_params.w;

    vec2 screen_rad = vec2(radius / (z * pc.pass_extra0.x), radius / (z * pc.pass_extra0.y)) * 0.5;
    screen_rad = clamp(screen_rad, px * 1.5, vec2(0.12));

    /* Interleaved gradient noise per pixel for smooth horizon sampling without texture lookup */
    float ang0 = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    int taps = int(clamp(pc.pass_extra1.x, 4.0, 24.0));
    float occ = 0.0;

    for (int i = 0; i < 24; i++) {
        if (i >= taps) break;
        float fi = (float(i) + 0.5) / float(taps);
        float a = ang0 + fi * 6.2831853 * 2.3999632;
        vec2 dir = vec2(cos(a), sin(a));
        vec2 suv = clamp(in_uv + dir * screen_rad * (0.25 + 0.75 * fi), vec2(0.001), vec2(0.999));
        float sz = texture(lin_depth, suv).r;
        if (sz >= far_limit * 0.99) continue;
        vec3 S = reconstruct_pos(suv, sz);
        vec3 V = S - P;
        float dist2 = dot(V, V);
        float dist = sqrt(max(dist2, 1e-6));
        float ndotv = dot(N, V / dist);
        float falloff = clamp(1.0 - dist2 / (radius * radius * 1.25), 0.0, 1.0);
        float contrib = max(0.0, ndotv - bias) * falloff;
        occ += contrib;
    }

    occ = (occ / float(taps)) * shadow_mult * 2.2;
    float fade = 1.0 - clamp((z - fade_from) / max(fade_to - fade_from, 0.001), 0.0, 1.0);
    float vis = clamp(1.0 - occ * fade, 0.0, 1.0);
    vis = pow(vis, max(shadow_pow, 0.1));
    vis = max(vis, 1.0 - shadow_clamp);
    out_ao = clamp(vis, 0.0, 1.0);
}
