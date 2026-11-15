#include "ce_effects.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void fx_switch_start(fx_switch_t* s, int setting) {
    s->setting = setting != 0;
    s->running = s->setting;
}

int fx_switch_set(fx_switch_t* s, int on) {
    on = on != 0;
    if (s->running == on) return 0;
    s->running = on;
    return 1;
}

float fx_sky_fog(float v, float fog_low, float fog_high) {
    if (v <= fog_low) return 0.0f;
    if (v >= fog_high) return 1.0f;
    return (v - fog_low) / (fog_high - fog_low);
}

int fx_sky_usable(float fog_low, float fog_high, float plane_height_uv) {
    return isfinite(fog_low) && isfinite(fog_high) && isfinite(plane_height_uv) && fog_low >= -1.0f && fog_low <= 2.0f &&
           fog_high >= -1.0f && fog_high <= 2.0f;
}

float fx_sky_horizontal(float du, float dv, float height_uv) {
    float flat = sqrtf(du * du + dv * dv), all = sqrtf(du * du + dv * dv + height_uv * height_uv);
    return all > 0.0f ? flat / all : 0.0f;
}

typedef struct {
    uintptr_t page;
    unsigned used;
    uint8_t written[256];
    float data[4096];
} water_page_t;

static struct {
    water_page_t pages[FX_WATER_PAGES];
    int count;
    unsigned serial, clock;
    int hint;
    fx_water_link_t links[FX_WATER_LINKS];
    int link_count;
    fx_water_raw_t raw;
} W;

float fx_water_clamp(double d) {
    if (!(d == d)) return 0.0f;
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return (float)d;
}

static water_page_t* find_page(uintptr_t page) {
    if (W.hint < W.count && W.pages[W.hint].page == page) return &W.pages[W.hint];
    for (int i = 0; i < W.count; i++)
        if (W.pages[i].page == page) {
            W.hint = i;
            return &W.pages[i];
        }
    return NULL;
}

static water_page_t* take_page(uintptr_t page) {
    int at = W.count, oldest = 0;
    if (W.count == FX_WATER_PAGES) {
        for (int i = 1; i < W.count; i++)
            if (W.pages[i].used < W.pages[oldest].used) oldest = i;
        at = oldest;
    } else {
        W.count++;
    }
    memset(&W.pages[at], 0, sizeof W.pages[at]);
    W.pages[at].page = page;
    W.hint = at;
    return &W.pages[at];
}

static int is_written(const water_page_t* p, size_t texel) { return (p->written[texel >> 3] >> (texel & 7)) & 1; }

static void note_raw(double a, double b) {
    fx_water_raw_t* r = &W.raw;
    if (!r->written || a < r->a_min) r->a_min = a;
    if (!r->written || a > r->a_max) r->a_max = a;
    if (!r->written || b < r->b_min) r->b_min = b;
    if (!r->written || b > r->b_max) r->b_max = b;
    r->written++;
    if (a != 0.0 || b != 0.0) r->nonzero++;
}

void fx_water_store(uintptr_t texel, double a, double b) {
    uintptr_t page = texel >> 12;
    size_t index = (texel & 0xFFE) >> 1;
    water_page_t* p = find_page(page);
    if (!p) p = take_page(page);
    p->used = ++W.clock;
    p->data[index * 2] = fx_water_clamp(a);
    p->data[index * 2 + 1] = fx_water_clamp(b);
    p->written[index >> 3] |= (uint8_t)(1u << (index & 7));
    note_raw(a, b);
    W.serial++;
}

unsigned fx_water_serial(void) { return W.serial; }

const fx_water_raw_t* fx_water_raw(void) { return &W.raw; }

double fx_water_abs_max(void) {
    double m = 0.0;
    const fx_water_raw_t* r = &W.raw;
    if (fabs(r->a_min) > m) m = fabs(r->a_min);
    if (fabs(r->a_max) > m) m = fabs(r->a_max);
    if (fabs(r->b_min) > m) m = fabs(r->b_min);
    if (fabs(r->b_max) > m) m = fabs(r->b_max);
    return m;
}

/* The texels of a page inside [lo, hi): first and one past the last. */
static int texel_span(const water_page_t* p, uintptr_t lo, uintptr_t hi, size_t* first, size_t* end) {
    uintptr_t start = p->page << 12, from = lo > start ? lo : start, to = hi < start + 4096 ? hi : start + 4096;
    if (from >= to) return 0;
    *first = (from - start + 1) >> 1;
    *end = (to - start) >> 1;
    return *first < *end;
}

int fx_water_written(uintptr_t lo, uintptr_t hi) {
    for (int i = 0; i < W.count; i++) {
        size_t first, end;
        if (!texel_span(&W.pages[i], lo, hi, &first, &end)) continue;
        for (size_t t = first; t < end; t++)
            if (is_written(&W.pages[i], t)) return 1;
    }
    return 0;
}

void fx_water_forget(uintptr_t lo, uintptr_t hi) {
    for (int i = 0; i < W.count; i++) {
        size_t first, end;
        if (!texel_span(&W.pages[i], lo, hi, &first, &end)) continue;
        for (size_t t = first; t < end; t++) W.pages[i].written[t >> 3] &= (uint8_t)~(1u << (t & 7));
    }
}

int fx_water_fill(uintptr_t bits, long pitch, int width, int height, float* out, fx_water_stats_t* stats) {
    int any = 0;
    double sum = 0.0;
    unsigned written = 0;
    float peak = 0.0f;
    for (int y = 0; y < height; y++) {
        uintptr_t row = bits + (uintptr_t)((long long)y * pitch);
        for (int x = 0; x < width; x++) {
            uintptr_t texel = row + (uintptr_t)x * 2;
            water_page_t* p = find_page(texel >> 12);
            float* o = out + ((size_t)y * width + x) * 2;
            size_t index = (texel & 0xFFE) >> 1;
            if (p && is_written(p, index)) {
                float ax = fabsf(p->data[index * 2]), ay = fabsf(p->data[index * 2 + 1]), m = ax > ay ? ax : ay;
                o[0] = p->data[index * 2];
                o[1] = p->data[index * 2 + 1];
                sum += m;
                written++;
                if (m > peak) peak = m;
                any = 1;
            } else {
                o[0] = o[1] = 0.0f;
            }
        }
    }
    if (stats) {
        stats->texels = written;
        stats->max = peak;
        stats->mean = written ? (float)(sum / written) : 0.0f;
    }
    return any;
}

void fx_water_reset(void) {
    memset(&W, 0, sizeof W);
    memset(fx_water_shifts(), 0, sizeof(fx_shift_t));
}

int fx_water_alpha_range(uintptr_t bits, long pitch, int width, int height, int* lo, int* hi) {
    int any = 0;
    *lo = 15;
    *hi = 0;
    for (int y = 0; y < height; y++) {
        uintptr_t row = bits + (uintptr_t)((long long)y * pitch);
        for (int x = 0; x < width; x++) {
            uintptr_t texel = row + (uintptr_t)x * 2;
            water_page_t* p = find_page(texel >> 12);
            int a;
            if (!p || !is_written(p, (texel & 0xFFE) >> 1)) continue;
            a = *(const uint16_t*)texel >> 12;
            if (a < *lo) *lo = a;
            if (a > *hi) *hi = a;
            any = 1;
        }
    }
    if (!any) *lo = *hi = 0;
    return any;
}

double fx_water_attenuation(double rhw) {
    double a = rhw / FX_WATER_RHW_METRE;
    return a > 0.0 ? (a < 1.0 ? a : 1.0) : 0.0;
}

double fx_water_displacement_px(double difference, double strength, double rhw, double render_height) {
    return difference * strength * fx_water_attenuation(rhw) * render_height;
}

void fx_water_offset_px(double ax, double ay, double strength, double rhw, double render_height, double* dx, double* dy) {
    *dx = fx_water_displacement_px(ax, strength, rhw, render_height);
    *dy = fx_water_displacement_px(ay, strength, rhw, render_height);
}

void fx_water_scene_bounds(int left, int top, int right, int bottom, int width, int height, float out[4]) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) left = top = 0, right = width, bottom = height;
    out[0] = (left + 0.5f) / width;
    out[1] = (top + 0.5f) / height;
    out[2] = (right - 0.5f) / width;
    out[3] = (bottom - 0.5f) / height;
}

void fx_water_refract_uv(const float base[2], const float offset[2], const float bounds[4], float out[2]) {
    float t = 1.0f;
    for (int a = 0; a < 2; a++) {
        float limit;
        if (offset[a] > 0.0f) limit = (bounds[2 + a] - base[a]) / offset[a];
        else if (offset[a] < 0.0f) limit = (bounds[a] - base[a]) / offset[a];
        else continue;
        if (limit < t) t = limit;
    }
    if (t < 0.0f) t = 0.0f;
    out[0] = base[0] + offset[0] * t;
    out[1] = base[1] + offset[1] * t;
}

void fx_water_wave_at(const float* values, int width, int height, double u, double v, double out[2]) {
    double x = u * width - 0.5, y = v * height - 0.5;
    int x0 = (int)floor(x), y0 = (int)floor(y);
    double fx = x - x0, fy = y - y0;
    out[0] = out[1] = 0.0;
    for (int c = 0; c < 2; c++) {
        double row[2];
        for (int j = 0; j < 2; j++) {
            int yy = y0 + j, xa = x0, xb = x0 + 1;
            yy = yy < 0 ? 0 : yy >= height ? height - 1 : yy;
            xa = xa < 0 ? 0 : xa >= width ? width - 1 : xa;
            xb = xb < 0 ? 0 : xb >= width ? width - 1 : xb;
            row[j] = values[((size_t)yy * width + xa) * 2 + c] * (1.0 - fx) + values[((size_t)yy * width + xb) * 2 + c] * fx;
        }
        out[c] = row[0] * (1.0 - fy) + row[1] * fy;
    }
}

static fx_shift_t g_shifts;

fx_shift_t* fx_water_shifts(void) { return &g_shifts; }

void fx_shift_add(fx_shift_t* s, double px, double weight, double wave, double rhw) {
    int bin;
    if (!(weight > 0.0) || !(px >= 0.0)) return;
    bin = (int)(px / FX_SHIFT_BIN_PX);
    if (bin >= FX_SHIFT_BINS) bin = FX_SHIFT_BINS - 1;
    s->bins[bin] += weight;
    s->weight += weight;
    s->sum += px * weight;
    if (px > s->max) {
        s->max = px;
        s->max_wave = wave;
        s->max_rhw = rhw;
    }
}

double fx_shift_mean(const fx_shift_t* s) { return s->weight > 0.0 ? s->sum / s->weight : 0.0; }

double fx_shift_percentile(const fx_shift_t* s, double p) {
    double need = s->weight * p, seen = 0.0;
    for (int i = 0; i < FX_SHIFT_BINS; i++) {
        seen += s->bins[i];
        if (seen >= need && s->bins[i] > 0.0) return (i + 1) * FX_SHIFT_BIN_PX;
    }
    return 0.0;
}

static void drop_link(int i) { W.links[i] = W.links[--W.link_count]; }

int fx_water_loaded(const void* drawn, const void* source, uintptr_t bits, long pitch, int width, int height,
                    int pixel_bytes) {
    int water = pixel_bytes == 2 && bits && pitch > 0 && width > 0 && height > 0 &&
                fx_water_written(bits, bits + (uintptr_t)pitch * (uintptr_t)height);
    for (int i = 0; i < W.link_count; i++)
        if (W.links[i].drawn == drawn) {
            drop_link(i);
            break;
        }
    if (!water) return 0;
    if (W.link_count == FX_WATER_LINKS) drop_link(0);
    W.links[W.link_count].drawn = drawn;
    W.links[W.link_count].source = source;
    W.links[W.link_count].bits = bits;
    W.links[W.link_count].pitch = pitch;
    W.links[W.link_count].width = width;
    W.links[W.link_count].height = height;
    W.link_count++;
    return 1;
}

const fx_water_link_t* fx_water_link(const void* drawn) {
    for (int i = 0; i < W.link_count; i++)
        if (W.links[i].drawn == drawn) return &W.links[i];
    return NULL;
}

void fx_water_surface_freed(const void* surface, uintptr_t lo, uintptr_t hi) {
    for (int i = W.link_count - 1; i >= 0; i--)
        if (W.links[i].drawn == surface || W.links[i].source == surface) drop_link(i);
    if (hi > lo) fx_water_forget(lo, hi);
}

fx_water_plan_t fx_water_plan(int running, int available, int blend_over, unsigned long long frame,
                              unsigned long long copied_frame, int copied_valid, unsigned samples) {
    fx_water_plan_t p = { 0, 0, 0 };
    if (!running || !available || !blend_over) return p;
    p.refract = 1;
    p.copy_scene = !copied_valid || copied_frame != frame;
    p.resolve = p.copy_scene && samples > 1;
    return p;
}

int fx_scene_copy_fits(unsigned have_w, unsigned have_h, unsigned have_format, unsigned want_w, unsigned want_h,
                       unsigned want_format) {
    return have_w == want_w && have_h == want_h && have_format == want_format && want_w && want_h;
}
