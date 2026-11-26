/* Trespasser CE's custom skies (.sky): the layers a file names, their shapes around the camera as CE builds them, and their
 * triangles on the screen. World axes: x east, y north, z up; every position is relative to the camera. */
#ifndef JPT_SKY_H
#define JPT_SKY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_SKY_LAYERS_MAX 12
#define JPT_SKY_TEXTURE_MAX 260
#define JPT_SKY_SHAPE_VERTICES 144
#define JPT_SKY_SHAPE_INDICES 786

typedef enum { JPT_SKY_DOME, JPT_SKY_BODY, JPT_SKY_STRIP, JPT_SKY_STARS, JPT_SKY_METEORS } jpt_sky_type_t;
typedef enum { JPT_SKY_OPAQUE, JPT_SKY_ALPHA, JPT_SKY_BURN, JPT_SKY_ALPHA_2X, JPT_SKY_BURN_2X } jpt_sky_blend_t;

typedef struct {
    int number; /* of its section */
    jpt_sky_type_t type;
    jpt_sky_blend_t blend;
    int fogged, fog_as_alpha, second_horizon;
    char texture[JPT_SKY_TEXTURE_MAX]; /* empty: none */
    float top[4], horizon[4], horizon2[4], bottom[4], color[4]; /* r, g, b, a, 0 to 1 */
    float gradient, horizon_gradient, horizon_rotation;
    float uv_scale, offset_u, offset_v, scroll_speed;
    float curvature, height_offset, horizon_bias, absolute_height;
    float width_scale, height_scale, elevation, azimuth;
    float top_z, bottom_z, top_fog, bottom_fog, parallax_range, parallax_amplitude;
} jpt_sky_layer_t;

typedef struct {
    int has_fog_near, has_fog_far, has_height;
    float fog_near, fog_far, height;
    jpt_sky_layer_t layers[JPT_SKY_LAYERS_MAX];
    int count;
} jpt_sky_t;

/* The level's own sky and the moment it is drawn at. */
typedef struct {
    float camera[3];
    float height, fog_near, fog_far, texture_scale; /* the file's values before these */
    float wind[2];                                  /* texels a second */
    float seconds;
} jpt_sky_moment_t;

typedef struct {
    float position[3], u, v, color[4];
    float clear; /* 1 without fog, 0 all fog */
} jpt_sky_vertex_t;

typedef struct {
    jpt_sky_vertex_t vertices[JPT_SKY_SHAPE_VERTICES];
    uint16_t indices[JPT_SKY_SHAPE_INDICES];
    int vertex_count, index_count;
} jpt_sky_shape_t;

/* What the camera sees: the directions of the screen's corners, and where on the screen they are. */
typedef struct {
    float top_left[3], top_right[3], bottom_right[3], bottom_left[3];
    float left, top, right, bottom;
} jpt_sky_view_t;

typedef struct {
    float x, y, z, rhw;
    uint32_t diffuse, specular;
    float u, v;
} jpt_sky_screen_vertex_t;

/* One draw of a layer. */
typedef struct {
    jpt_sky_blend_t blend; /* JPT_SKY_OPAQUE, JPT_SKY_ALPHA or JPT_SKY_BURN */
    int fog, wrap_u, wrap_v;
    int twice; /* the texture times the colour, times two and saturated, before it is blended */
} jpt_sky_pass_t;

int jpt_sky_parse(const char* text, size_t size, jpt_sky_t* out);
const char* jpt_sky_type_name(jpt_sky_type_t type);
/* Whether the port draws it. */
int jpt_sky_drawn(const jpt_sky_layer_t* layer);

/* aspect: the texture's height by its width. */
int jpt_sky_shape(const jpt_sky_t* sky, const jpt_sky_layer_t* layer, const jpt_sky_moment_t* moment, float aspect, jpt_sky_shape_t* out);
/* Triangles of three vertices each; the number of vertices, at most cap. */
int jpt_sky_project(const jpt_sky_shape_t* shape, const jpt_sky_view_t* view, jpt_sky_screen_vertex_t* out, int cap);
/* How a layer is drawn. */
void jpt_sky_pass(const jpt_sky_layer_t* layer, jpt_sky_pass_t* out);

#ifdef __cplusplus
}
#endif

#endif
