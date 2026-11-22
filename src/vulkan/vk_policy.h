#ifndef JPT_VK_POLICY_H
#define JPT_VK_POLICY_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "jpt_gpu.h"
#include "jpt_render_seam.h"
#include "jpt_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_VK_FRAMES_IN_FLIGHT 2
#define JPT_VK_SUBOPTIMAL_LIMIT 2
#define JPT_VK_MAX_GPUS 16

typedef enum {
    JPT_VK_DEPTH_BIT_D24S8 = 1u << 0,
    JPT_VK_DEPTH_BIT_D32S8 = 1u << 1,
    JPT_VK_DEPTH_BIT_D32 = 1u << 2,
    JPT_VK_DEPTH_BIT_D16 = 1u << 3
} jpt_vk_depth_bits_t;

typedef enum {
    JPT_VK_SC_ACTION_OK = 0,
    JPT_VK_SC_ACTION_WAIT_EXTENT = 1,
    JPT_VK_SC_ACTION_RECREATE = 2,
    JPT_VK_SC_ACTION_RECREATE_SURFACE = 3,
    JPT_VK_SC_ACTION_DEVICE_LOST = 4
} jpt_vk_sc_action_t;

typedef enum {
    JPT_VK_SHADER_FIXED = 0,
    JPT_VK_SHADER_WATER = 1,
    JPT_VK_SHADER_SKY = 2
} jpt_vk_shader_kind_t;

typedef enum {
    JPT_VK_RP_SCENE = 0,
    JPT_VK_RP_SCENE_MSAA = 1,
    JPT_VK_RP_PRESENT = 2,
    JPT_VK_RP_POST = 3
} jpt_vk_rp_kind_t;

typedef struct {
    char name[JPT_GPU_NAME_MAX];
    char label[JPT_GPU_LABEL_MAX];
    char stable_id[JPT_GPU_ID_MAX];
    uint32_t vendor_id;
    uint32_t device_id;
    VkPhysicalDeviceType device_type;
    uint32_t api_version;
    uint32_t driver_version;
    char driver_version_text[48];
    char driver_name[64];
    uint8_t uuid[16];
    uint64_t vram_bytes;
    int32_t graphics_queue_family;
    int32_t present_queue_family;
    int has_swapchain_ext;
    int has_anisotropy;
    float max_anisotropy;
    int has_fill_non_solid;
    VkSampleCountFlags msaa_color_counts;
    VkSampleCountFlags msaa_depth_counts;
    uint32_t depth_format_mask;
    VkFormat preferred_depth_format;
    int depth_has_stencil;
    int usable;
    char reject_reason[128];
} jpt_vk_gpu_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    int vsync;
    int fullscreen;
    uint32_t consecutive_suboptimal;
    uint32_t recreate_count;
    int device_lost;
    int surface_lost;
    int dirty;
} jpt_vk_swapchain_health_t;

typedef struct {
    uint8_t shader_kind;
    uint8_t topology;
    uint8_t cull_mode;
    uint8_t fill_mode;
    uint8_t blend_enable;
    uint8_t src_blend;
    uint8_t dst_blend;
    uint8_t z_enable;
    uint8_t z_write;
    uint8_t z_func;
    uint8_t stencil_mode;
    uint8_t sample_count;
    uint8_t renderpass_kind;
    uint8_t reserved[3];
} jpt_vk_pipeline_key_t;

typedef struct {
    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    VkAccessFlags src_access;
    VkAccessFlags dst_access;
    VkImageAspectFlags aspect;
} jpt_vk_barrier_plan_t;

typedef struct {
    uint32_t guest_w, guest_h;
    uint32_t internal_w, internal_h;
    uint32_t window_w, window_h;
    jpt_render_rect_t present_rect;
} jpt_vk_frame_geometry_t;

typedef struct {
    int32_t textures;
    int32_t image_views;
    int32_t buffers;
    int32_t allocations;
    int32_t pipelines;
    int32_t pipeline_layouts;
    int32_t descriptor_pools;
    int32_t framebuffers;
    int32_t render_passes;
    int32_t samplers;
    int32_t swapchain_images;
    int32_t command_pools;
    int32_t fences;
    int32_t semaphores;
    int64_t allocated_bytes;
} jpt_vk_lifetime_t;

const char* jpt_vk_result_str(VkResult result);
const char* jpt_vk_device_type_str(VkPhysicalDeviceType type);
void jpt_vk_format_driver_version(uint32_t vendor_id, uint32_t driver_version, char* out, size_t cap);

void jpt_vk_gpu_assign_labels(jpt_vk_gpu_info_t* gpus, int count);
int jpt_vk_gpu_check_requirements(jpt_vk_gpu_info_t* gpu, int require_present);
int jpt_vk_gpu_select(const jpt_vk_gpu_info_t* gpus, int count, const char* request, int has_env_override,
                      char* reason, size_t cap);

int jpt_vk_pick_depth_format(uint32_t supported_mask, VkFormat* out_format, int* out_has_stencil);
float jpt_vk_clamp_anisotropy(int filter_setting, int device_supports_aniso, float device_max_aniso);
uint32_t jpt_vk_clamp_msaa(uint32_t requested_msaa, VkSampleCountFlags color_counts, VkSampleCountFlags depth_counts);
VkPolygonMode jpt_vk_resolve_fill_mode(uint32_t d3d_fill_mode, int supports_non_solid);

VkPresentModeKHR jpt_vk_choose_present_mode(int vsync, const VkPresentModeKHR* modes, uint32_t count);
uint32_t jpt_vk_choose_image_count(uint32_t min_images, uint32_t max_images, VkPresentModeKHR mode);
VkSurfaceFormatKHR jpt_vk_choose_surface_format(const VkSurfaceFormatKHR* formats, uint32_t count);

void jpt_vk_sc_init(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px, int vsync, int fullscreen);
jpt_vk_sc_action_t jpt_vk_sc_on_resize(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px);
jpt_vk_sc_action_t jpt_vk_sc_on_settings(jpt_vk_swapchain_health_t* h, int vsync, int fullscreen);
jpt_vk_sc_action_t jpt_vk_sc_on_result(jpt_vk_swapchain_health_t* h, VkResult result);
void jpt_vk_sc_on_recreated(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px, int vsync, int fullscreen);

void jpt_vk_pipeline_key_from_state(const jpt_render_state_t* st, jpt_render_prim_t prim, uint32_t sample_count,
                                    jpt_vk_rp_kind_t rp_kind, int supports_non_solid, jpt_vk_pipeline_key_t* out);
int jpt_vk_pipeline_key_equal(const jpt_vk_pipeline_key_t* a, const jpt_vk_pipeline_key_t* b);
uint64_t jpt_vk_pipeline_key_hash(const jpt_vk_pipeline_key_t* k);

int jpt_vk_plan_image_barrier(VkImageLayout old_layout, VkImageLayout new_layout, VkImageAspectFlags aspect,
                              jpt_vk_barrier_plan_t* out);

void jpt_vk_compute_geometry(uint32_t guest_w, uint32_t guest_h, uint32_t internal_w, uint32_t internal_h,
                             uint32_t window_w, uint32_t window_h, int stretch, jpt_vk_frame_geometry_t* out);

int jpt_vk_lifetime_is_clean(const jpt_vk_lifetime_t* lt);
int jpt_vk_lifetime_total_objects(const jpt_vk_lifetime_t* lt);

#ifdef __cplusplus
}
#endif

#endif
