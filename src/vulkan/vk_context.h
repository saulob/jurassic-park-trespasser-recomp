#ifndef JPT_VK_CONTEXT_H
#define JPT_VK_CONTEXT_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "vk_alloc.h"
#include "vk_loader.h"
#include "vk_policy.h"

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*jpt_vk_log_fn)(const char* message, void* user);

typedef struct {
    struct SDL_Window* window; /* SDL window (Linux / SDL hosts); NULL when using native_hwnd or headless */
    void* native_hwnd;         /* Win32 HWND on Windows; NULL when using SDL window or headless */
    const char* gpu_request;   /* "" or "auto" or label / stable ID / UUID */
    const char* cache_path;    /* e.g. <user_data>/cache/vulkan_pipeline.cache; NULL disables disk cache */
    int enable_validation;     /* dev/debug only; gracefully skipped if layer absent */
    jpt_vk_log_fn log_fn;
    void* log_user;
} jpt_vk_context_desc_t;

typedef enum {
    JPT_VK_PROBE_NOT_RUN = 0,
    JPT_VK_PROBE_OK,
    JPT_VK_PROBE_LOADER,
    JPT_VK_PROBE_INSTANCE,
    JPT_VK_PROBE_ENTRY_POINTS,
    JPT_VK_PROBE_ENUMERATION,
    JPT_VK_PROBE_NO_DEVICE,
    JPT_VK_PROBE_NO_USABLE_DEVICE
} jpt_vk_probe_status_t;

typedef struct {
    jpt_vk_probe_status_t status;
    int available;
    int gpu_count;
    int selected_index;
    jpt_vk_gpu_info_t gpus[JPT_VK_MAX_GPUS];
    char summary[256];
} jpt_vk_probe_result_t;

typedef struct {
    int initialized;
    int headless;
    struct SDL_Window* window;
    void* native_hwnd;
    jpt_vk_loader_t vk;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_utils_enabled;
    int validation_enabled;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    jpt_vk_gpu_info_t gpu;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures enabled_features;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device;
    uint32_t graphics_family;
    uint32_t present_family;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkPipelineCache pipeline_cache;
    char cache_path[512];
    jpt_vk_lifetime_t lifetime;
    jpt_vk_allocator_t alloc;
    jpt_vk_log_fn log_fn;
    void* log_user;
    char select_reason[192];
    char last_error[256];
    volatile long validation_errors;
    volatile long validation_warnings;
} jpt_vk_context_t;

int jpt_vk_context_init(jpt_vk_context_t* ctx, const jpt_vk_context_desc_t* desc);
int jpt_vk_context_recreate_surface(jpt_vk_context_t* ctx);
void jpt_vk_context_save_pipeline_cache(const jpt_vk_context_t* ctx);
void jpt_vk_context_shutdown(jpt_vk_context_t* ctx);

void jpt_vk_set_object_name(const jpt_vk_context_t* ctx, VkObjectType type, uint64_t handle, const char* name);
void jpt_vk_log(const jpt_vk_context_t* ctx, const char* fmt, ...);

/* Enumerates only: no device is created. Every failure is reported in `out`. */
int jpt_vk_probe_headless(const char* gpu_request, jpt_vk_probe_result_t* out);
/* The same over an open loader, which it closes. */
int jpt_vk_probe_loader(jpt_vk_loader_t* vk, const char* gpu_request, jpt_vk_probe_result_t* out);
const char* jpt_vk_probe_status_name(jpt_vk_probe_status_t status);

void jpt_vk_format_startup_summary(const jpt_vk_context_t* ctx, VkPresentModeKHR present_mode, VkFormat swap_format,
                                   uint32_t internal_w, uint32_t internal_h, uint32_t msaa, float anisotropy,
                                   char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
