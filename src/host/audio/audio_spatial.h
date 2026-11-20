#ifndef TRESPASSER_AUDIO_SPATIAL_H
#define TRESPASSER_AUDIO_SPATIAL_H

/* Software positioning of one source for one listener, in the left-handed space the game uses (x right, y up, z forward),
 * with a distance factor and rolloff factor of 1 and no Doppler. */

typedef struct {
    float x, y, z;
} audio_vec3_t;

typedef struct {
    audio_vec3_t position, front, top;
} audio_listener_t;

typedef struct {
    audio_vec3_t position, cone_orientation;
    unsigned inside_angle, outside_angle; /* degrees: 360 and 360 is no cone */
    int outside_volume;                   /* hundredths of dB, <= 0 */
    float min_distance, max_distance;
} audio_source_t;

void audio_listener_default(audio_listener_t* listener);
void audio_source_default(audio_source_t* source);

/* Inverse-distance rolloff: full level up to the minimum distance, then 6 dB less per doubling, held from the maximum. */
float audio_distance_gain(float distance, float min_distance, float max_distance);
/* 1 inside the inside cone, the outside volume beyond the outside cone, dB-linear between. */
float audio_cone_gain(const audio_source_t* source, const audio_vec3_t* listener_position);
/* Left/right gains of the source's bearing: the ear away from it falls to silence at 90 degrees. */
void audio_pan_for_bearing(const audio_listener_t* listener, const audio_vec3_t* source_position, float* left, float* right);
void audio_spatial_gains(const audio_listener_t* listener, const audio_source_t* source, float* left, float* right);

#endif
