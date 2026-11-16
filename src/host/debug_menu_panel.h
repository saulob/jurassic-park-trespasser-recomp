#ifndef TRESPASSER_DEBUG_MENU_PANEL_H
#define TRESPASSER_DEBUG_MENU_PANEL_H

#include "debug_menu_layout.h"
#include "debug_menu_policy.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

#define DMP_COORD_LINES 5
#define DMP_HELP_LINES 2

typedef struct {
    float view_w, view_h; /* the presentation surface, in its own pixels */
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
