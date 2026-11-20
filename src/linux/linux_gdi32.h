/* The gdi32 functions the guest calls. Device contexts, bitmaps, brushes, pens, fonts and regions are guest objects behind 32-bit
 * tokens (a slot and a generation, so a stale token fails); DIB sections live in guest memory. A window's device context
 * draws into that window's client surface, which is presented through linux_screen.h. */
#ifndef JPT_LINUX_GDI32_H
#define JPT_LINUX_GDI32_H

#include <stddef.h>
#include <stdint.h>

#include "linux_hle.h"

const hle_entry_t* hle_gdi32_table(size_t* count);
void hle_gdi32_init(void);
void hle_gdi32_shutdown(void);

/* GetDC / BeginPaint for a window, and the matching release. 0 when the window has no client area. */
uint32_t lx_gdi_window_dc(uint32_t hwnd, int client_w, int client_h);
int lx_gdi_release_dc(uint32_t hdc);
/* The window no longer exists: its surface and the contexts on it go. */
void lx_gdi_window_gone(uint32_t hwnd);
/* A device context over a DirectDraw surface's pixels (guest memory, top row first); drawing writes them in place. */
uint32_t lx_gdi_surface_dc(uint32_t guest_bits, int width, int height, int pitch, int bpp, uint32_t green_mask);
int lx_gdi_release_surface_dc(uint32_t hdc);
/* Fills rect {left, top, right, bottom} of a device context with a brush token (or a COLOR_xxx + 1 value). */
int lx_gdi_fill_rect(uint32_t hdc, const int32_t* rect, uint32_t brush);
/* DrawTextExA: formats text_va (count -1 = NUL-terminated) in rect {left, top, right, bottom} (updated by DT_CALCRECT) and draws it.
 * margins = {left, right} from a DRAWTEXTPARAMS or NULL. Returns the text height, 0 on failure. */
int lx_gdi_draw_text(uint32_t hdc, uint32_t text_va, int count, int32_t* rect, uint32_t flags, const int32_t* margins);
/* DrawEdge on rect (shrunk by BF_ADJUST); 3D edges of the classic button palette. */
int lx_gdi_draw_edge(uint32_t hdc, int32_t* rect, uint32_t edge, uint32_t flags);
/* Presents the window's client surface. */
void lx_gdi_present_window(uint32_t hwnd);
unsigned lx_gdi_object_count(void);
unsigned lx_gdi_blit_count(void);

#endif
