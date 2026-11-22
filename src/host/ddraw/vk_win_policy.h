#ifndef JPT_VK_WIN_POLICY_H
#define JPT_VK_WIN_POLICY_H

#include "d3d_legacy.h"

/* What the game's Lock callers need from a render target, decided by the host's retail-address policy. The Vulkan
 * backend asks through this seam so its tests need no guest. */
typedef struct {
    uint32_t caller;
    int reads;
    int inert;
    int text_overlay;
} vkp_access_t;

void vkp_classify_access(cpu_access_t access, uint32_t lock_flags, vkp_access_t* out);
uint32_t vkp_lock_caller(void);
int vkp_polygon_caller_ok(uint32_t caller);
int vkp_polygon_store_only(uint32_t caller);
void vkp_polygon_to_surface(long box[4], uint32_t origin_bytes, long pitch);
int vkp_text_boxes_known(void);
int vkp_gameplay_now(void);
int vkp_ui_canvas_present_held(void);
int vkp_loading_hd_present_held(void);
int vkp_window_host_view_size(unsigned* w, unsigned* h);
float vkp_view_tan_half_height(void);
int vkp_guest_section_count(void);
const char* vkp_guest_section_name(int index);
double vkp_guest_section_ms(int index);

#endif
