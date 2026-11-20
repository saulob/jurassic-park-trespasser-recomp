/* Water refraction and Enhanced sky for the Direct3D 11 renderer (after Trespasser CE): their switches and the
 * arithmetic the shaders share with the tests. No D3D11 here. */
#ifndef CE_EFFECTS_H
#define CE_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The setting chooses the state a run starts with; a change of the running state takes effect at the next draw of that
 * effect (never inside a batch). */
typedef struct {
    int setting, running;
} fx_switch_t;

void fx_switch_start(fx_switch_t* s, int setting);
/* 1 when the running state changed. */
int fx_switch_set(fx_switch_t* s, int on);

/* Enhanced sky, as CE draws its default dome: v is the horizontal length of the unit view direction (0 straight up, 1
 * at the horizon); the fog share is 0 up to fog_low (CSkyRender +0x30), 1 from fog_high (+0x2C), linear between. The
 * order of the tests is CE's, so equal or inverted bounds give a step and never a division. */
float fx_sky_fog(float v, float fog_low, float fog_high);
/* Bounds the enhanced sky can use; otherwise the retail sky is drawn. */
int fx_sky_usable(float fog_low, float fog_high, float plane_height_uv);
/* v for a point of the retail sky plane: its texture coordinates against the camera's (u = x * 1e-4 + 1.5 + scroll,
 * v = y * 1e-4 + scroll) and the plane's height above the camera in the same units. */
float fx_sky_horizontal(float du, float dv, float height_uv);

#define FX_SKY_UV_PER_METRE 1.0e-4f
#define FX_SKY_U_OFFSET 1.5f

/* CEntityWater's rows are written into a system-memory source surface, which the game then copies into the Direct3D
 * texture it draws (IDirect3DTexture::Load). water_rows gives each texel the wave heights' differences to the next
 * column and row (a, b), which CE's refraction texture holds as trunc(x * 127.5 + 127.5) per byte; they are kept here
 * exactly (floats, clamped to CE's range of -1 to 1) by the texel's address, and a Load whose source holds them links
 * the texture it fills to that source, which is how a draw of the water is told from any other. */
#define FX_WATER_PAGES 64
#define FX_WATER_LINKS 8
void fx_water_store(uintptr_t texel, double a, double b);
/* Bumped by every water row. */
unsigned fx_water_serial(void);
/* Whether a texel of [lo, hi) was written by a water row. */
int fx_water_written(uintptr_t lo, uintptr_t hi);
/* The memory is no longer a water texture's: its records go. */
void fx_water_forget(uintptr_t lo, uintptr_t hi);
float fx_water_clamp(double d);
void fx_water_reset(void);

/* What the rows have produced so far, before any clamping. */
typedef struct {
    double a_min, a_max, b_min, b_max;
    unsigned long long written, nonzero;
} fx_water_raw_t;
const fx_water_raw_t* fx_water_raw(void);
double fx_water_abs_max(void);

typedef struct {
    unsigned texels;
    float max, mean; /* of |difference| over the texels written */
} fx_water_stats_t;
/* The differences of a 16-bit texture as two floats per texel (zero where no row wrote); 1 when any was. */
int fx_water_fill(uintptr_t bits, long pitch, int width, int height, float* out, fx_water_stats_t* stats);

typedef struct {
    const void *drawn, *source;
    uintptr_t bits;
    long pitch;
    int width, height;
} fx_water_link_t;
/* A texture Load: when the source's texels were written by water rows the drawn texture is the water's; otherwise it
 * is not (a texture that was water and is loaded from elsewhere stops being). 1 when it is water. */
int fx_water_loaded(const void* drawn, const void* source, uintptr_t bits, long pitch, int width, int height,
                    int pixel_bytes);
const fx_water_link_t* fx_water_link(const void* drawn);
/* A surface (drawn or source) was freed, its memory being [lo, hi). */
void fx_water_surface_freed(const void* surface, uintptr_t lo, uintptr_t hi);

/* The alpha (0-15) of the texels water rows wrote in a 4444 texture; 0 when none was. */
int fx_water_alpha_range(uintptr_t bits, long pitch, int width, int height, int* lo, int* hi);

/* A vertex's rhw is 0.1 / its distance in metres (CCamera's fInvZAdjust over the normalised depth); CE fades the shift
 * by 1 / max(distance, 1 m). */
#define FX_WATER_RHW_METRE 0.1f
double fx_water_attenuation(double rhw);
/* The shift ps_water gives the scene behind the water, in pixels of a render target render_height high (x is scaled by
 * height / width, so it is the same in both directions). */
double fx_water_displacement_px(double difference, double strength, double rhw, double render_height);
/* The shift of one pixel in x and y: both differences scale by the target height. */
void fx_water_offset_px(double ax, double ay, double strength, double rhw, double render_height, double* dx, double* dy);
/* The part of a width x height scene copy the water may sample: the game's 3D viewport [left, right) x [top, bottom) in
 * pixels, as texel centres (uv lo.xy, hi.xy), so bilinear filtering never reaches the black outside the picture. An empty
 * rectangle is the whole copy. */
void fx_water_scene_bounds(int left, int top, int right, int bottom, int width, int height, float out[4]);
/* ps_water's sample position: base + offset, the offset shortened along its own direction until it stays inside
 * [lo, hi] (a base outside it is kept as it is). */
void fx_water_refract_uv(const float base[2], const float offset[2], const float bounds[4], float out[2]);
/* One wave texel pair under (u, v) of a width x height float pair texture (bilinear, clamped). */
void fx_water_wave_at(const float* values, int width, int height, double u, double v, double out[2]);

/* The distribution of the shifts the water draws make, weighted by the pixels each sample stands for. */
#define FX_SHIFT_BINS 4096
#define FX_SHIFT_BIN_PX 0.25
typedef struct {
    double weight, sum, max, max_wave, max_rhw;
    double bins[FX_SHIFT_BINS];
} fx_shift_t;
void fx_shift_add(fx_shift_t* s, double px, double weight, double wave, double rhw);
double fx_shift_mean(const fx_shift_t* s);
double fx_shift_percentile(const fx_shift_t* s, double p);
fx_shift_t* fx_water_shifts(void);

/* What a draw batch of a water texture does. */
typedef struct {
    int refract;     /* the refraction shader draws it */
    int copy_scene;  /* the scene behind is copied first (once a frame) */
    int resolve;     /* the copy is a multisample resolve */
} fx_water_plan_t;
fx_water_plan_t fx_water_plan(int running, int available, int blend_over, unsigned long long frame,
                              unsigned long long copied_frame, int copied_valid, unsigned samples);

/* The copy target fits the scene: same size and format, one sample. */
int fx_scene_copy_fits(unsigned have_w, unsigned have_h, unsigned have_format, unsigned want_w, unsigned want_h,
                       unsigned want_format);

/* Screen-space strength of the distortion (CE's 0.35 per unit of height difference, weighted by the vertex's 1/w). */
#define FX_WATER_STRENGTH 0.35f

#ifdef __cplusplus
}
#endif

#endif
