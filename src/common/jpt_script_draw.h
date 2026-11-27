/* What game scripts draw in OnPaintHW3D, as commands every renderer consumes alike (src/host/debug_menu_panel.cpp): textures by a
 * logical id of their level's VM, rectangles in the game's screen pixels, ARGB modulation and CE's blend modes. Never a GPU, guest
 * or host pointer in a script. */
#ifndef JPT_SCRIPT_DRAW_H
#define JPT_SCRIPT_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TP_Engine.DrawTexture's iBlendMode. */
#define JPT_DRAW_OPAQUE 0
#define JPT_DRAW_ALPHA 1
#define JPT_DRAW_ADD 2

#define JPT_DRAW_COMMANDS_MAX 4096
#define JPT_DRAW_TEXTURES_MAX 256
#define JPT_DRAW_TEXTURE_BYTES_MAX (64u << 20)

typedef struct {
    uint32_t texture;                 /* jpt_draw_texture_t.id of the same frame */
    float src_x, src_y, src_w, src_h; /* texels */
    float x, y, w, h;                 /* screen pixels (jpt_draw_frame_t.screen_w x screen_h) */
    uint32_t argb;
    int blend;
} jpt_draw_cmd_t;

typedef struct {
    uint32_t id; /* unique within a generation */
    int width, height;
    const uint8_t* rgba; /* width x height x 4, straight alpha */
} jpt_draw_texture_t;

typedef struct {
    uint32_t generation; /* the VM's: textures of another generation are gone */
    int screen_w, screen_h;
    const jpt_draw_cmd_t* cmds;
    int count;
    const jpt_draw_texture_t* textures;
    int texture_count;
} jpt_draw_frame_t;

#ifdef __cplusplus
}
#endif

#endif
