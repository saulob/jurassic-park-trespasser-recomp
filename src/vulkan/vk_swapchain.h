#ifndef JPT_VK_SWAPCHAIN_H
#define JPT_VK_SWAPCHAIN_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "vk_context.h"
#include "vk_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_VK_MAX_SWAPCHAIN_IMAGES 8

typedef struct {
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;
    VkFence in_flight;
    VkSemaphore image_available;
} jpt_vk_frame_sync_t;

typedef struct {
    jpt_vk_context_t* ctx;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[JPT_VK_MAX_SWAPCHAIN_IMAGES];
    VkImageView views[JPT_VK_MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[JPT_VK_MAX_SWAPCHAIN_IMAGES];
    VkSemaphore render_finished[JPT_VK_MAX_SWAPCHAIN_IMAGES];

    /* Offscreen target used when ctx->headless is set */
    VkImage headless_image;
    jpt_vk_allocation_t headless_alloc;

    VkRenderPass present_pass;
    jpt_vk_frame_sync_t frames[JPT_VK_FRAMES_IN_FLIGHT];
    uint32_t frame_index;
    uint32_t acquired_image;
    int frame_begun;
    jpt_vk_swapchain_health_t health;
} jpt_vk_swapchain_t;

int jpt_vk_swapchain_init(jpt_vk_swapchain_t* sc, jpt_vk_context_t* ctx, uint32_t width, uint32_t height, int vsync,
                          int fullscreen);
int jpt_vk_swapchain_recreate(jpt_vk_swapchain_t* sc, uint32_t width, uint32_t height, int vsync, int fullscreen);
void jpt_vk_swapchain_shutdown(jpt_vk_swapchain_t* sc);

/* Returns JPT_VK_SC_ACTION_OK with *out_cmd ready to record when a frame can proceed. */
jpt_vk_sc_action_t jpt_vk_swapchain_begin_frame(jpt_vk_swapchain_t* sc, VkCommandBuffer* out_cmd,
                                                VkFramebuffer* out_fb, VkExtent2D* out_extent);
jpt_vk_sc_action_t jpt_vk_swapchain_end_frame(jpt_vk_swapchain_t* sc);

/* Submits what the open frame recorded, waits for it, and continues recording in the same frame (readbacks mid-frame). */
int jpt_vk_swapchain_flush_frame(jpt_vk_swapchain_t* sc);

/* Reads back the last rendered headless frame as tightly packed RGBA8 pixels. */
int jpt_vk_swapchain_readback_headless(jpt_vk_swapchain_t* sc, uint8_t* dst_rgba, size_t dst_bytes);

#ifdef __cplusplus
}
#endif

#endif
