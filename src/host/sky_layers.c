#include "sky_layers.h"

#include <stdlib.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_custom_level.h"
#include "jpt_fs.h"

#define SKY_TEXTURE_SCALE 0x24u
#define SKY_FOG_FAR 0x2Cu
#define SKY_FOG_NEAR 0x30u
#define SKY_HEIGHT 0x44u
#define SKY_WIND_U 0x50u
#define SKY_WIND_V 0x54u
#define CAMERA_POSITION GVA(0x00732978)
#define VIEW_CORNERS GVA(0x007328E0) /* directions: top left, top right, bottom left, bottom right */
#define CORNER_BYTES 12u
#define GAME_SECONDS GVA(0x0073F34C)
#define FIXED_POINT 65536.0f
#define BELOW_EVERYTHING (-1.0e9f)
#define FILE_LIMIT (256u * 1024u)
#define SIDE_MAX 1024u
#define SCREEN_VERTICES_MAX 8192
#define VERTEX_BYTES 32u

static struct {
    void (*free_gpu)(void*);
    unsigned serial;
    int known;
    jpt_sky_t sky;
    jpt_picture_t pictures[JPT_SKY_LAYERS_MAX];
    void* gpu[JPT_SKY_LAYERS_MAX];
    int drawn[JPT_SKY_LAYERS_MAX], count;
    jpt_sky_moment_t moment;
    jpt_sky_view_t view;
    float height;
    jpt_sky_shape_t shape;
    jpt_sky_screen_vertex_t vertices[SCREEN_VERTICES_MAX];
} S;

static float rdf(uint32_t va) { return *(const float*)ADDR(va); }

static void drop(void) {
    for (int i = 0; i < JPT_SKY_LAYERS_MAX; i++) {
        if (S.gpu[i] && S.free_gpu) S.free_gpu(S.gpu[i]);
        S.gpu[i] = NULL;
        jpt_picture_free(&S.pictures[i]);
        memset(&S.pictures[i], 0, sizeof S.pictures[i]);
        S.drawn[i] = 0;
    }
    S.count = 0;
    S.known = 0;
}

static char* read_file(const char* path, size_t* size) {
    uint64_t length = 0;
    jpt_fs_file_t* f = jpt_fs_open_read(path, &length);
    char* bytes = f && length && length <= FILE_LIMIT ? (char*)malloc((size_t)length) : NULL;
    if (bytes && !jpt_fs_read_at(f, 0, bytes, (size_t)length)) {
        free(bytes);
        bytes = NULL;
    }
    if (f) jpt_fs_close(f);
    *size = bytes ? (size_t)length : 0;
    return bytes;
}

static int load_layer(int i) {
    const jpt_sky_layer_t* l = &S.sky.layers[i];
    char file[JPT_PATH_MAX];
    if (!jpt_sky_drawn(l)) {
        host_log("custom-level", "sky: layer %d (%s%s%s) is left out: the port does not draw it", l->number, jpt_sky_type_name(l->type), l->texture[0] ? ", " : "", l->texture);
        return 0;
    }
    if (!l->texture[0]) return 1;
    if (!jpt_custom_level_sky_texture(l->texture, file, sizeof file) || !jpt_picture_load(file, &S.pictures[i])) {
        host_log("custom-level", "sky: layer %d (%s) is left out: its texture %s cannot be read", l->number, jpt_sky_type_name(l->type), l->texture);
        return 0;
    }
    jpt_picture_limit(&S.pictures[i], SIDE_MAX);
    jpt_picture_complete(&S.pictures[i], 0);
    return 1;
}

static void load(void) {
    char file[JPT_PATH_MAX];
    char* text;
    size_t size = 0;
    drop();
    S.known = 1;
    S.serial = jpt_custom_level_serial();
    if (!jpt_custom_level_sky(file, sizeof file) || (text = read_file(file, &size)) == NULL) return;
    if (jpt_sky_parse(text, size, &S.sky))
        for (int i = 0; i < S.sky.count; i++) S.count += S.drawn[i] = load_layer(i);
    free(text);
    host_log("custom-level", "sky: %s, %d of %d layer(s) drawn", file, S.count, S.sky.count);
}

void sky_layers_install(void (*free_gpu)(void*)) {
    S.free_gpu = free_gpu;
    if (jpt_custom_level_active()) jpt_custom_level_on_end(drop);
}

int sky_layers_ready(void) {
    if (!jpt_custom_level_requires(JPT_LEVEL_CAP_SKY)) return 0;
    if (!S.known || S.serial != jpt_custom_level_serial()) load();
    return S.count > 0;
}

void sky_layers_begin(uint32_t sky) {
    float* const into[4] = { S.view.top_left, S.view.top_right, S.view.bottom_left, S.view.bottom_right };
    S.height = rdf(sky + SKY_HEIGHT);
    for (uint32_t i = 0; i < 3; i++) S.moment.camera[i] = rdf(CAMERA_POSITION + 4u * i);
    for (uint32_t c = 0; c < 4; c++)
        for (uint32_t i = 0; i < 3; i++) into[c][i] = rdf(VIEW_CORNERS + CORNER_BYTES * c + 4u * i);
    S.moment.height = S.height;
    S.moment.fog_near = rdf(sky + SKY_FOG_NEAR);
    S.moment.fog_far = rdf(sky + SKY_FOG_FAR);
    S.moment.texture_scale = rdf(sky + SKY_TEXTURE_SCALE);
    S.moment.wind[0] = rdf(sky + SKY_WIND_U) / FIXED_POINT;
    S.moment.wind[1] = rdf(sky + SKY_WIND_V) / FIXED_POINT;
    S.moment.seconds = rdf(GAME_SECONDS);
    *(float*)ADDR(sky + SKY_HEIGHT) = BELOW_EVERYTHING;
}

void sky_layers_end(uint32_t sky) { *(float*)ADDR(sky + SKY_HEIGHT) = S.height; }

int sky_layers_rect(const void* vertices, unsigned count, float rect[4]) {
    if (count != 4) return 0;
    for (unsigned i = 0; i < count; i++) {
        float at[2];
        memcpy(at, (const uint8_t*)vertices + i * VERTEX_BYTES, sizeof at);
        for (int k = 0; k < 2; k++) {
            if (!i || at[k] < rect[k]) rect[k] = at[k];
            if (!i || at[k] > rect[k + 2]) rect[k + 2] = at[k];
        }
    }
    return rect[2] > rect[0] && rect[3] > rect[1];
}

void sky_layers_emit(const float rect[4], sky_emit_fn emit, void* user) {
    S.view.left = rect[0], S.view.top = rect[1], S.view.right = rect[2], S.view.bottom = rect[3];
    for (int i = 0; i < S.sky.count; i++) {
        const jpt_sky_layer_t* l = &S.sky.layers[i];
        const jpt_picture_t* picture = S.pictures[i].bgra ? &S.pictures[i] : NULL;
        jpt_sky_pass_t pass;
        sky_draw_t draw;
        if (!S.drawn[i] || !jpt_sky_shape(&S.sky, l, &S.moment, picture ? (float)picture->height / (float)picture->width : 1.0f, &S.shape)) continue;
        draw.count = jpt_sky_project(&S.shape, &S.view, S.vertices, SCREEN_VERTICES_MAX);
        if (!draw.count) continue;
        draw.picture = picture;
        draw.gpu = &S.gpu[i];
        draw.vertices = S.vertices;
        jpt_sky_pass(l, &pass);
        draw.pass = &pass;
        emit(user, &draw);
    }
}
