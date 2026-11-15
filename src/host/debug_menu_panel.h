#ifndef TRESPASSER_DEBUG_MENU_PANEL_H
#define TRESPASSER_DEBUG_MENU_PANEL_H

#include "debug_menu_policy.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

#define DMP_COORD_LINES 5

typedef struct {
    float view_w, view_h; /* the presentation surface, in its own pixels */
    float dpi_scale;
    int menu;
    int objectives; /* the menu is Objectives: larger text */
    const char* title;
    const dm_row_t* rows;
    int row_count;
    const char* feedback;
    const char* help;
    int coord_lines;
    char coords[DMP_COORD_LINES][DM_TEXT_MAX];
    const cp_hud_t* hud; /* the compass and its notice; NULL when the HUD is not on screen */
} dmp_frame_t;

#ifdef __cplusplus
extern "C" {
#endif

void dmp_draw(struct ID3D11Device* device, struct ID3D11DeviceContext* context, struct ID3D11RenderTargetView* target,
              int bb_w, int bb_h, const dmp_frame_t* frame);
void dmp_release(void);
/* A TrueType font whose glyphs fill in what the panel's own font lacks (translated hints); before the first draw. */
void dmp_set_fallback_font(const char* path);

#ifdef __cplusplus
}
#endif

#endif
