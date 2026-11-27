#ifndef TRESPASSER_DEBUG_MENU_PANEL_H
#define TRESPASSER_DEBUG_MENU_PANEL_H

#include <stddef.h>
#include <stdint.h>

#include "debug_menu_layout.h"
#include "debug_menu_policy.h"
#include "jpt_script_draw.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

#define DMP_HELP_LINES 2

/* The game's own text of this frame (subtitles, hints), kept apart from the picture so the HUD can sit under it. */
typedef struct {
    void* texture; /* D3D11 SRV (legacy_w x legacy_h), or RGBA8 dirty-rect pixels when software_rgba is set */
    int legacy_w, legacy_h;
    int left, top, right, bottom; /* the part of it that holds text, in texture pixels */
    int software_rgba;
} dmp_text_layer_t;

typedef struct {
    float view_w, view_h; /* the presentation surface, in its own pixels */
    float scene_x, scene_y, scene_w, scene_h; /* the game's 3D view inside it; w 0: the whole surface */
    float dpi_scale;
    int menu;
    int objectives; /* the menu is Objectives: larger text */
    const char* title;
    const dm_row_t* rows;
    int row_count;
    const char* feedback;     /* what could not be done; shown in the help area, over the context help */
    const char* help;         /* the keys; NULL for none */
    const char* context_help; /* what the selected row does; NULL for none */
    int help_lines;           /* the help area's fixed height in lines, filled or not, so the panel never resizes */
    int menu_id, page;        /* dm_menu_t and dm_page_t of the rows: the hit table's stamp */
    int pointer;              /* the pointer is over the view */
    float pointer_x, pointer_y;
    const cp_hud_t* hud; /* the compass and its notice; NULL when the HUD is not on screen */
    const dmp_text_layer_t* text_layer; /* drawn over the HUD; NULL for none */
    const char* counter;                /* the frame counter's text; NULL for none */
    const jpt_draw_frame_t* script;     /* what game scripts drew this frame (OnPaintHW3D), under everything else; NULL for none */
} dmp_frame_t;

typedef struct {
    float clip[4];
    uint32_t idx_offset, idx_count;
    int32_t vtx_offset;
    uint32_t texture;
    uint32_t blend; /* 0 alpha, 1 additive (a script's DrawTexture mode 2) */
} dmp_gpu_cmd_t;

typedef struct {
    uint32_t id, width, height, bytes_per_pixel;
    const uint8_t* pixels;
    uint32_t revision;
} dmp_gpu_texture_t;

/* Textures one GPU overlay frame may hold, the white texel included (jpt_vk_renderer_submit_ui's limit). */
#define DMP_GPU_TEXTURES 33

typedef struct dmp_gpu_draw_s {
    float display_pos[2], display_size[2];
    uint32_t target_w, target_h;
    const void* vertices;
    uint32_t vertex_count;
    const uint16_t* indices;
    uint32_t index_count;
    const dmp_gpu_cmd_t* cmds;
    uint32_t cmd_count;
    const dmp_gpu_texture_t* textures;
    uint32_t texture_count;
} dmp_gpu_draw_t;

#ifdef __cplusplus
extern "C" {
#endif

void dmp_draw(struct ID3D11Device* device, struct ID3D11DeviceContext* context, struct ID3D11RenderTargetView* target,
              int bb_w, int bb_h, const dmp_frame_t* frame);
void dmp_release(void);
/* The same frame drawn by the CPU into 32-bit XRGB pixels (width x height, pitch in pixels): the host without a GPU device. */
void dmp_draw_software(uint32_t* pixels, int width, int height, int pitch_pixels, const dmp_frame_t* frame);
/* The same frame drawn into straight-alpha RGBA8 pixels (width x height x 4): composited by Vulkan as an overlay layer. */
int dmp_draw_rgba(uint8_t* rgba, int width, int height, const dmp_frame_t* frame, uint8_t* tiles, int tiles_w, int tiles_h);
/* The same frame as Dear ImGui's draw lists flattened for a GPU: 1 drawn, 0 nothing to show. Valid until the next call. */
int dmp_build_gpu(dmp_gpu_draw_t* out, int width, int height, const dmp_frame_t* frame);
/* The same, from the bytes of a TrueType face. */
void dmp_set_fallback_font_memory(const void* ttf, size_t size);
/* Where the last drawn menu can be clicked; not valid when no menu was drawn. */
const dml_hits_t* dmp_hits(void);
/* How many layout errors Dear ImGui has reported to the panel's context. */
int dmp_imgui_errors(void);
/* A TrueType font whose glyphs fill in what the panel's own font lacks (translated hints); before the first draw. */
void dmp_set_fallback_font(const char* path);

#ifdef __cplusplus
}
#endif

#endif
