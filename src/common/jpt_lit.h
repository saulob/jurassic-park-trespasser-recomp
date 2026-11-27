/* Normal-mapped polygons of a custom level as Trespasser CE lights them: one composite light in the texture's space, its
 * direction interpolated between the vertices, the rest of a polygon the same. The shaders compute jpt_lit_pixel. */
#ifndef JPT_LIT_H
#define JPT_LIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_LIT_LIGHTS_MAX 8

typedef struct {
    float light[4];    /* towards the light in the texture's space (u, v, out of it), and its strength */
    float material[4]; /* diffuse, specular, where the highlight begins, ambient light */
    float shade[2];    /* the exponent of the reflectance, the least of it */
} jpt_lit_vertex_t;

typedef struct {
    float ambient;
    int count;
    struct {
        float towards[3], strength;
    } light[JPT_LIT_LIGHTS_MAX];
} jpt_lit_lights_t;

typedef struct {
    float strength, ambient, towards[3];
} jpt_lit_bump_t;

typedef struct {
    float diffuse, specular, highlight, emissive;
} jpt_lit_material_t;

/* Of a texture's colour table: reflectance to a value of it. */
typedef struct {
    float exponent, scale, least; /* scale * reflectance ^ exponent, no less than least */
    float shaded;                 /* the values up to the full colour */
} jpt_lit_shade_t;

typedef struct {
    float u[3], v[3], out[3]; /* towards a greater u, a greater v, and the eye */
} jpt_lit_frame_t;

/* The composite of the lights, as the engine makes one for a bump-mapped object. 0: no light at all. */
int jpt_lit_combine(const jpt_lit_lights_t* lights, jpt_lit_bump_t* out);
/* The same for a specular material, its highlight towards eye. */
void jpt_lit_specular(jpt_lit_bump_t* bump, const jpt_lit_material_t* m, const float eye[3]);
/* Of a polygon's corners relative to the eye. 0: degenerate. */
int jpt_lit_face(const float (*at)[3], const float (*uv)[2], unsigned count, jpt_lit_frame_t* out);
/* The cosine between a vertex's normal and the light, from the value the engine gave the vertex; below 0 when that cannot be told. */
float jpt_lit_cosine(const jpt_lit_shade_t* s, const jpt_lit_material_t* m, const jpt_lit_bump_t* bump, float value);
/* towards: the light's direction in the frame of a vertex with this normal; cosine: as jpt_lit_cosine tells it. */
void jpt_lit_towards(const jpt_lit_frame_t* face, const float normal[3], const jpt_lit_bump_t* bump, float cosine, float towards[3]);
/* texture, normal_map: RGBA; dark, full: the colours of no and of full reflectance. */
void jpt_lit_pixel(const jpt_lit_vertex_t* v, const float dark[3], const float full[3], const float texture[4], const float normal_map[4], float out[3]);

/* What the renderer takes instead of a polygon's own vertex colours. */
typedef struct {
    int normal; /* of jpt_texhw.h */
    unsigned count;
    uint32_t dark, full; /* 0x00RRGGBB */
    const jpt_lit_vertex_t* vertices;
} jpt_lit_draw_t;

/* The polygon being drawn; NULL: lit by the engine. */
void jpt_lit_set_draw(const jpt_lit_draw_t* draw);
const jpt_lit_draw_t* jpt_lit_draw(unsigned vertices);

#ifdef __cplusplus
}
#endif

#endif
