#include "audio_spatial.h"

#include <math.h>

#include "audio_policy.h"

#define PI_F 3.14159265358979f

static float dot(audio_vec3_t a, audio_vec3_t b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static audio_vec3_t sub(audio_vec3_t a, audio_vec3_t b) { audio_vec3_t r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static float length(audio_vec3_t a) { return sqrtf(dot(a, a)); }

static audio_vec3_t cross(audio_vec3_t a, audio_vec3_t b) {
    audio_vec3_t r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}

void audio_listener_default(audio_listener_t* l) {
    audio_vec3_t zero = { 0, 0, 0 }, front = { 0, 0, 1 }, top = { 0, 1, 0 };
    l->position = zero;
    l->front = front;
    l->top = top;
}

void audio_source_default(audio_source_t* s) {
    audio_vec3_t zero = { 0, 0, 0 }, front = { 0, 0, 1 };
    s->position = zero;
    s->cone_orientation = front;
    s->inside_angle = s->outside_angle = 360;
    s->outside_volume = 0;
    s->min_distance = 1.0f;
    s->max_distance = 1000000000.0f;
}

float audio_distance_gain(float distance, float min_distance, float max_distance) {
    if (min_distance <= 0.0f || distance <= min_distance) return 1.0f;
    if (distance > max_distance) distance = max_distance;
    return distance <= min_distance ? 1.0f : min_distance / distance;
}

float audio_cone_gain(const audio_source_t* s, const audio_vec3_t* listener_position) {
    audio_vec3_t to = sub(*listener_position, s->position);
    float lt = length(to), lo = length(s->cone_orientation), angle, inside, outside, t;
    if (s->inside_angle >= 360 && s->outside_angle >= 360) return 1.0f;
    if (lt <= 0.0f || lo <= 0.0f) return 1.0f;
    angle = acosf(fmaxf(-1.0f, fminf(1.0f, dot(to, s->cone_orientation) / (lt * lo)))) * 180.0f / PI_F;
    inside = (float)s->inside_angle * 0.5f;
    outside = (float)s->outside_angle * 0.5f;
    if (angle <= inside) return 1.0f;
    if (angle >= outside || outside <= inside) return audio_volume_gain(s->outside_volume);
    t = (angle - inside) / (outside - inside);
    return audio_volume_gain((int)((float)s->outside_volume * t));
}

void audio_pan_for_bearing(const audio_listener_t* l, const audio_vec3_t* source_position, float* left, float* right) {
    audio_vec3_t to = sub(*source_position, l->position), side = cross(l->top, l->front);
    float lt = length(to), ls = length(side), p = 0.0f;
    if (lt > 0.0f && ls > 0.0f) p = fmaxf(-1.0f, fminf(1.0f, dot(to, side) / (lt * ls)));
    *left = p > 0.0f ? fmaxf(0.0f, cosf(p * PI_F * 0.5f)) : 1.0f;
    *right = p < 0.0f ? fmaxf(0.0f, cosf(-p * PI_F * 0.5f)) : 1.0f;
}

void audio_spatial_gains(const audio_listener_t* l, const audio_source_t* s, float* left, float* right) {
    float pl, pr, g = audio_distance_gain(length(sub(s->position, l->position)), s->min_distance, s->max_distance) *
                      audio_cone_gain(s, &l->position);
    audio_pan_for_bearing(l, &s->position, &pl, &pr);
    *left = g * pl;
    *right = g * pr;
}
