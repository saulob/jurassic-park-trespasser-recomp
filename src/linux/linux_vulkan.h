#ifndef JPT_LINUX_VULKAN_H
#define JPT_LINUX_VULKAN_H

#include <stdint.h>

#include "jpt_config.h"
#include "jpt_renderer.h"
#include "linux_window.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

void lx_vk_configure(const jpt_config_t* cfg, const char* cache_dir);
void lx_vk_set_cli_overrides(const char* renderer_opt, const char* gpu_opt);
int lx_vk_active(void);
const jpt_renderer_decision_t* lx_vk_decision(void);
/* Why the game runs with the Software renderer although the settings asked for another (or auto found no usable GPU): one
 * line for the player, "" when nothing fell back. The saved setting is never changed. */
const char* lx_vk_graphics_notice(void);

int lx_vk_attach_window(struct SDL_Window* window, uint32_t width, uint32_t height, int fullscreen);
void lx_vk_on_resize(uint32_t width, uint32_t height);
void lx_vk_set_fullscreen(int fullscreen);
void lx_vk_present_frame(const lx_frame_t* frame);
void lx_vk_shutdown(void);

/* DirectDraw / Direct3D 6 guest COM bridge (32-bit guest pointers via host64) */
uint32_t lx_vk_query_direct3d3(void);
uint32_t lx_vk_surface_query_texture2(uint32_t surface_slot, uint32_t width, uint32_t height, uint32_t pitch,
                                      uint32_t guest_bits, const uint32_t pf[8], int has_colorkey, uint32_t ck_lo,
                                      uint32_t ck_hi);
void lx_vk_surface_notify_unlock(uint32_t surface_slot, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t guest_bits, const uint32_t pf[8], int has_colorkey, uint32_t ck_lo,
                                 uint32_t ck_hi);
void lx_vk_surface_notify_destroy(uint32_t surface_slot);
void lx_vk_reset_com(void);

#ifdef __cplusplus
}
#endif

#endif
