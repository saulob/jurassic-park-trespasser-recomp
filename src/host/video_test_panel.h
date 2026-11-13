#ifndef TRESPASSER_VIDEO_TEST_PANEL_H
#define TRESPASSER_VIDEO_TEST_PANEL_H

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

typedef struct {
    const char* file;
    char size[48];
    char length[16];
} vt_panel_row_t;

typedef struct {
    int count;
    const vt_panel_row_t* rows;
    int hd_available;
    float view_w, view_h; /* the presentation surface, in its own pixels: the panel's coordinates */
    float dpi_scale;
} vt_panel_frame_t;

/* The panel drawn on target, the bb_w x bb_h back buffer the surface shows; 1 with the row and vl_play_mode_t of a
 * clicked button. */
int vt_panel_draw(struct ID3D11Device* device, struct ID3D11DeviceContext* context, struct ID3D11RenderTargetView* target,
                  int bb_w, int bb_h, const vt_panel_frame_t* frame, int* video, int* mode);
void vt_panel_mouse_move(float x, float y);
void vt_panel_mouse_button(int button, int down);
void vt_panel_mouse_wheel(float steps);
void vt_panel_release(void);

#endif
