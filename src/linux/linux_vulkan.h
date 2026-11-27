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
void lx_vk_set_env_renderer(const char* renderer_env);
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

/* The scene the guest's Direct3D device drew this frame (linux_d3d.h), with the host's overlays. */
void lx_vk_present_scene(const lx_scene_frame_t* frame);
/* The window cannot use Vulkan: this run goes on with the Software renderer. */
void lx_vk_window_failed(const char* why);

#ifdef __cplusplus
}
#endif

#endif
