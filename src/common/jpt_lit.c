#include "jpt_lit.h"

#include <math.h>
#include <string.h>

#define FULL_HIGHLIGHT 0.998749197f
#define NO_LENGTH 1e-12f
#define DARK 1e-3f

static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int normalize(float v[3]) {
    const float length = sqrtf(dot(v, v));
    if (!(length > NO_LENGTH)) return 0;
    for (int i = 0; i < 3; i++) v[i] /= length;
    return 1;
}

static float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
static float combined(float a, float b) { return a + b - a * b; }

int jpt_lit_combine(const jpt_lit_lights_t* lights, jpt_lit_bump_t* out) {
    float sum = 0.0f, total = 0.0f, length;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < lights->count; i++) {
        const float strength = lights->light[i].strength;
        for (int k = 0; k < 3; k++) out->towards[k] += lights->light[i].towards[k] * strength;
        sum += strength;
        total = combined(total, strength);
    }
    length = sqrtf(dot(out->towards, out->towards));
    if (!(sum > 0.0f) || !normalize(out->towards)) return 0;
    out->strength = lights->count == 1 ? lights->light[0].strength : length * total / sum;
    out->ambient = lights->count == 1 ? lights->ambient : combined(lights->ambient, total - out->strength);
    out->strength *= 1.0f - lights->ambient;
    return 1;
}

void jpt_lit_specular(jpt_lit_bump_t* bump, const jpt_lit_material_t* m, const float eye[3]) {
    float half[3];
    for (int i = 0; i < 3; i++) half[i] = eye[i] + bump->towards[i];
    if (!normalize(half)) return;
    for (int i = 0; i < 3; i++) half[i] = bump->towards[i] * m->diffuse + half[i] * m->specular;
    if (normalize(half)) memcpy(bump->towards, half, sizeof half);
}

int jpt_lit_face(const float (*at)[3], const float (*uv)[2], unsigned count, jpt_lit_frame_t* out) {
    float normal[3] = { 0 }, best = 0.0f;
    memset(out, 0, sizeof *out);
    for (unsigned i = 1; i + 1 < count; i++) {
        const float u1 = uv[i][0] - uv[0][0], v1 = uv[i][1] - uv[0][1], u2 = uv[i + 1][0] - uv[0][0], v2 = uv[i + 1][1] - uv[0][1];
        const float det = u1 * v2 - u2 * v1;
        float e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) e1[k] = at[i][k] - at[0][k], e2[k] = at[i + 1][k] - at[0][k];
        cross(e1, e2, n);
        for (int k = 0; k < 3; k++) normal[k] += n[k];
        if (!(fabsf(det) > best)) continue;
        best = fabsf(det);
        for (int k = 0; k < 3; k++) {
            out->u[k] = (e1[k] * v2 - e2[k] * v1) / det;
            out->v[k] = (e2[k] * u1 - e1[k] * u2) / det;
        }
    }
    if (best <= NO_LENGTH || !normalize(normal)) return 0;
    if (dot(normal, at[0]) > 0.0f)
        for (int k = 0; k < 3; k++) normal[k] = -normal[k];
    memcpy(out->out, normal, sizeof normal);
    return normalize(out->u) && normalize(out->v);
}

float jpt_lit_cosine(const jpt_lit_shade_t* s, const jpt_lit_material_t* m, const jpt_lit_bump_t* bump, float value) {
    float reflected;
    if (m->specular != 0.0f || m->emissive != 0.0f || !(m->diffuse > 0.0f) || !(bump->strength > 0.0f)) return -1.0f;
    if (!(s->shaded > 0.0f) || !(s->scale > 0.0f) || !(s->exponent > 0.0f)) return -1.0f;
    reflected = s->least + clamp01(value / s->shaded) * (1.0f - s->least);
    if (reflected <= s->least + DARK) return -1.0f;
    reflected = (powf(reflected / s->scale, 1.0f / s->exponent) / m->diffuse - bump->ambient) / bump->strength;
    return reflected > DARK ? (reflected < 1.0f ? reflected : 1.0f) : -1.0f;
}

void jpt_lit_towards(const jpt_lit_frame_t* face, const float normal[3], const jpt_lit_bump_t* bump, float cosine, float towards[3]) {
    float u[3], v[3], handed[3], side;
    const float along = dot(face->u, normal);
    for (int k = 0; k < 3; k++) u[k] = face->u[k] - normal[k] * along;
    if (!normalize(u)) memcpy(u, face->u, sizeof u);
    cross(normal, u, v);
    cross(face->out, face->u, handed);
    if (dot(handed, face->v) < 0.0f)
        for (int k = 0; k < 3; k++) v[k] = -v[k];
    towards[0] = dot(bump->towards, u);
    towards[1] = dot(bump->towards, v);
    towards[2] = dot(bump->towards, normal);
    if (cosine < 0.0f) return;
    side = sqrtf(towards[0] * towards[0] + towards[1] * towards[1]);
    if (side > NO_LENGTH)
        for (int k = 0; k < 2; k++) towards[k] *= sqrtf(1.0f - cosine * cosine) / side;
    towards[2] = cosine;
}

void jpt_lit_pixel(const jpt_lit_vertex_t* v, const float dark[3], const float full[3], const float texture[4], const float normal_map[4], float out[3]) {
    const float diffuse = v->material[0], specular = v->material[1], begins = v->material[2] * FULL_HIGHLIGHT;
    float towards[3] = { v->light[0], v->light[1], v->light[2] }, normal[3], light, value, shown;
    for (int k = 0; k < 3; k++) normal[k] = normal_map[k] * 2.0f - 1.0f;
    light = normalize(towards) ? dot(normal, towards) : 0.0f;
    light = (light >= 0.0f ? light * v->light[3] : 0.0f) + v->material[3];
    value = light * diffuse;
    if (specular > 0.0f) value += (specular - value) * clamp01((light - begins) / (FULL_HIGHLIGHT - begins)) * normal_map[3];
    shown = value >= 1.0f ? 1.0f : powf(value, v->shade[0]);
    if (shown < v->shade[1]) shown = v->shade[1];
    for (int k = 0; k < 3; k++) {
        const float over = specular > 0.0f && value >= 1.0f ? (value - 1.0f) * full[k] : 0.0f;
        out[k] = over + texture[k] * (dark[k] + (full[k] - dark[k]) * shown - over);
    }
}

static const jpt_lit_draw_t* s_draw;

void jpt_lit_set_draw(const jpt_lit_draw_t* draw) { s_draw = draw; }
const jpt_lit_draw_t* jpt_lit_draw(unsigned vertices) { return s_draw && s_draw->count == vertices ? s_draw : NULL; }
