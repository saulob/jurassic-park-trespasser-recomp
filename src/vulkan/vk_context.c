#include "vk_context.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpt_fs.h"

#define JPT_VK_CACHE_MAGIC 0x4B56504Au /* "JPVK" */
#define JPT_VK_CACHE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint8_t pipeline_cache_uuid[VK_UUID_SIZE];
    uint32_t data_size;
} jpt_vk_cache_header_t;

static void set_error(jpt_vk_context_t* ctx, const char* fmt, ...) {
    va_list ap;
    if (!ctx) return;
    va_start(ap, fmt);
    vsnprintf(ctx->last_error, sizeof ctx->last_error, fmt, ap);
    va_end(ap);
}

void jpt_vk_log(const jpt_vk_context_t* ctx, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    if (!ctx || !ctx->log_fn) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ctx->log_fn(buf, ctx->log_user);
}

static int env_truthy(const char* name) {
    const char* v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "off") != 0;
}

static int has_gpu_env_override(void) {
    return env_truthy("DRI_PRIME") || env_truthy("MESA_VK_DEVICE_SELECT") || env_truthy("VK_LOADER_DEVICE_SELECT") ||
           env_truthy("__NV_PRIME_RENDER_OFFLOAD");
}

static int has_instance_ext(const VkExtensionProperties* exts, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(exts[i].extensionName, name)) return 1;
    return 0;
}

static int has_instance_layer(const VkLayerProperties* layers, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(layers[i].layerName, name)) return 1;
    return 0;
}

static int has_device_ext(const VkExtensionProperties* exts, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(exts[i].extensionName, name)) return 1;
    return 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    jpt_vk_context_t* ctx = (jpt_vk_context_t*)user;
    int is_error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const char* tag = is_error ? "ERROR" : "WARN";
    (void)types;
    if (ctx) {
        if (is_error) ctx->validation_errors++;
        else ctx->validation_warnings++;
    }
    if (ctx && data && data->pMessage) jpt_vk_log(ctx, "validation [%s]: %s", tag, data->pMessage);
    return VK_FALSE;
}

static uint32_t probe_depth_mask(const jpt_vk_loader_t* vk, VkPhysicalDevice pd) {
    static const struct {
        VkFormat fmt;
        uint32_t bit;
    } k_fmts[] = {
        { VK_FORMAT_D24_UNORM_S8_UINT, JPT_VK_DEPTH_BIT_D24S8 },
        { VK_FORMAT_D32_SFLOAT_S8_UINT, JPT_VK_DEPTH_BIT_D32S8 },
        { VK_FORMAT_D32_SFLOAT, JPT_VK_DEPTH_BIT_D32 },
        { VK_FORMAT_D16_UNORM, JPT_VK_DEPTH_BIT_D16 },
    };
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof k_fmts / sizeof k_fmts[0]; i++) {
        VkFormatProperties fp;
        memset(&fp, 0, sizeof fp);
        vk->vkGetPhysicalDeviceFormatProperties(pd, k_fmts[i].fmt, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) mask |= k_fmts[i].bit;
    }
    return mask;
}

static uint64_t estimate_vram_bytes(const jpt_vk_loader_t* vk, VkPhysicalDevice pd, int has_budget_ext) {
    uint64_t total = 0;
    if (has_budget_ext && vk->vkGetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
        VkPhysicalDeviceMemoryProperties2 mp2;
        memset(&budget, 0, sizeof budget);
        memset(&mp2, 0, sizeof mp2);
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mp2.pNext = &budget;
        vk->vkGetPhysicalDeviceMemoryProperties2(pd, &mp2);
        for (uint32_t i = 0; i < mp2.memoryProperties.memoryHeapCount; i++) {
            if (mp2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                uint64_t sz = budget.heapBudget[i] ? budget.heapBudget[i] : mp2.memoryProperties.memoryHeaps[i].size;
                total += sz;
            }
        }
        if (total) return total;
    }
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    vk->vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) total += mp.memoryHeaps[i].size;
    }
    return total;
}

static void inspect_physical_device(const jpt_vk_loader_t* vk, VkPhysicalDevice pd, VkSurfaceKHR surface,
                                    jpt_vk_gpu_info_t* out) {
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feats;
    uint32_t ext_count = 0;
    VkExtensionProperties* exts = NULL;
    uint32_t qcount = 0;
    VkQueueFamilyProperties* qprops = NULL;
    int has_budget = 0;

    memset(out, 0, sizeof *out);
    out->graphics_queue_family = -1;
    out->present_queue_family = -1;

    memset(&props, 0, sizeof props);
    vk->vkGetPhysicalDeviceProperties(pd, &props);
    memset(&feats, 0, sizeof feats);
    vk->vkGetPhysicalDeviceFeatures(pd, &feats);

    snprintf(out->name, sizeof out->name, "%.*s", (int)(sizeof out->name - 1), props.deviceName);
    out->vendor_id = props.vendorID;
    out->device_id = props.deviceID;
    out->device_type = props.deviceType;
    out->api_version = props.apiVersion;
    out->driver_version = props.driverVersion;
    jpt_vk_format_driver_version(props.vendorID, props.driverVersion, out->driver_version_text,
                                 sizeof out->driver_version_text);
    out->has_anisotropy = feats.samplerAnisotropy ? 1 : 0;
    out->max_anisotropy = feats.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
    out->has_fill_non_solid = feats.fillModeNonSolid ? 1 : 0;
    out->msaa_color_counts = props.limits.framebufferColorSampleCounts;
    out->msaa_depth_counts = props.limits.framebufferDepthSampleCounts;

    if (props.apiVersion >= VK_API_VERSION_1_1 && vk->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties id_props;
        VkPhysicalDeviceDriverProperties drv_props;
        VkPhysicalDeviceProperties2 props2;
        memset(&id_props, 0, sizeof id_props);
        memset(&drv_props, 0, sizeof drv_props);
        memset(&props2, 0, sizeof props2);
        id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &id_props;
        if (props.apiVersion >= VK_API_VERSION_1_2) {
            drv_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            id_props.pNext = &drv_props;
        }
        vk->vkGetPhysicalDeviceProperties2(pd, &props2);
        memcpy(out->uuid, id_props.deviceUUID, sizeof out->uuid);
        if (drv_props.driverName[0])
            snprintf(out->driver_name, sizeof out->driver_name, "%.*s", (int)(sizeof out->driver_name - 1),
                     drv_props.driverName);
    }

    if (vk->vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, NULL) == VK_SUCCESS && ext_count > 0) {
        exts = (VkExtensionProperties*)calloc(ext_count, sizeof *exts);
        if (exts && vk->vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, exts) == VK_SUCCESS) {
            out->has_swapchain_ext = has_device_ext(exts, ext_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            has_budget = has_device_ext(exts, ext_count, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
        free(exts);
    }

    out->vram_bytes = estimate_vram_bytes(vk, pd, has_budget);
    out->depth_format_mask = probe_depth_mask(vk, pd);

    vk->vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, NULL);
    if (qcount > 0) {
        qprops = (VkQueueFamilyProperties*)calloc(qcount, sizeof *qprops);
        if (qprops) {
            vk->vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops);
            for (uint32_t i = 0; i < qcount; i++) {
                int gfx = (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                VkBool32 pres = VK_FALSE;
                if (surface != VK_NULL_HANDLE && vk->vkGetPhysicalDeviceSurfaceSupportKHR) {
                    vk->vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &pres);
                }
                if (gfx && pres) {
                    out->graphics_queue_family = (int32_t)i;
                    out->present_queue_family = (int32_t)i;
                    break;
                }
                if (gfx && out->graphics_queue_family < 0) out->graphics_queue_family = (int32_t)i;
                if (pres && out->present_queue_family < 0) out->present_queue_family = (int32_t)i;
            }
            free(qprops);
        }
    }

    jpt_vk_gpu_check_requirements(out, surface != VK_NULL_HANDLE);
}

static VkResult create_instance(jpt_vk_context_t* ctx, int windowed, int want_validation) {
    uint32_t loader_api = VK_API_VERSION_1_0;
    uint32_t ext_count = 0;
    VkExtensionProperties* avail_exts = NULL;
    uint32_t layer_count = 0;
    VkLayerProperties* avail_layers = NULL;
    const char* req_exts[32];
    uint32_t req_ext_count = 0;
    const char* req_layers[4];
    uint32_t req_layer_count = 0;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkResult vr;

    if (ctx->vk.vkEnumerateInstanceVersion) ctx->vk.vkEnumerateInstanceVersion(&loader_api);

    if (ctx->vk.vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL) == VK_SUCCESS && ext_count > 0) {
        avail_exts = (VkExtensionProperties*)calloc(ext_count, sizeof *avail_exts);
        if (avail_exts) ctx->vk.vkEnumerateInstanceExtensionProperties(NULL, &ext_count, avail_exts);
    }

    if (windowed) {
        if (ctx->window) {
            Uint32 sdl_ext_count = 0;
            const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
            if (!sdl_exts || sdl_ext_count == 0) {
                free(avail_exts);
                set_error(ctx, "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
            for (Uint32 i = 0; i < sdl_ext_count && req_ext_count < 28; i++) req_exts[req_ext_count++] = sdl_exts[i];
        } else if (ctx->native_hwnd) {
#ifdef _WIN32
            if (!avail_exts || !has_instance_ext(avail_exts, ext_count, VK_KHR_SURFACE_EXTENSION_NAME) ||
                !has_instance_ext(avail_exts, ext_count, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
                free(avail_exts);
                set_error(ctx, "Vulkan instance missing VK_KHR_surface or VK_KHR_win32_surface");
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            }
            req_exts[req_ext_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
            req_exts[req_ext_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#else
            free(avail_exts);
            set_error(ctx, "native_hwnd surface is only supported on Windows");
            return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
        }
    }

    int debug_utils_avail = avail_exts && has_instance_ext(avail_exts, ext_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (want_validation || env_truthy("JPT_VK_VALIDATION")) {
        if (ctx->vk.vkEnumerateInstanceLayerProperties &&
            ctx->vk.vkEnumerateInstanceLayerProperties(&layer_count, NULL) == VK_SUCCESS && layer_count > 0) {
            avail_layers = (VkLayerProperties*)calloc(layer_count, sizeof *avail_layers);
            if (avail_layers) ctx->vk.vkEnumerateInstanceLayerProperties(&layer_count, avail_layers);
        }
        if (avail_layers && has_instance_layer(avail_layers, layer_count, "VK_LAYER_KHRONOS_validation")) {
            req_layers[req_layer_count++] = "VK_LAYER_KHRONOS_validation";
            ctx->validation_enabled = 1;
        }
        if (debug_utils_avail && req_ext_count < 31) {
            req_exts[req_ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            ctx->debug_utils_enabled = 1;
        }
    }
#if !defined(NDEBUG)
    if (!ctx->debug_utils_enabled && debug_utils_avail && req_ext_count < 31) {
        req_exts[req_ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ctx->debug_utils_enabled = 1;
    }
#endif

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Jurassic Park: Trespasser Recompiled";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "JPT-VK";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = (loader_api >= VK_API_VERSION_1_2) ? VK_API_VERSION_1_2
                   : (loader_api >= VK_API_VERSION_1_1) ? VK_API_VERSION_1_1
                                                        : VK_API_VERSION_1_0;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = req_ext_count;
    ci.ppEnabledExtensionNames = req_ext_count ? req_exts : NULL;
    ci.enabledLayerCount = req_layer_count;
    ci.ppEnabledLayerNames = req_layer_count ? req_layers : NULL;

    vr = ctx->vk.vkCreateInstance(&ci, NULL, &ctx->instance);
    free(avail_exts);
    free(avail_layers);
    if (vr != VK_SUCCESS) {
        set_error(ctx, "vkCreateInstance failed: %s (%d)", jpt_vk_result_str(vr), (int)vr);
        return vr;
    }
    return VK_SUCCESS;
}

static void load_pipeline_cache(jpt_vk_context_t* ctx) {
    VkPipelineCacheCreateInfo ci;
    void* blob = NULL;
    size_t blob_size = 0;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    if (ctx->cache_path[0]) {
        FILE* f = fopen(ctx->cache_path, "rb");
        if (f) {
            jpt_vk_cache_header_t hdr;
            if (fread(&hdr, 1, sizeof hdr, f) == sizeof hdr && hdr.magic == JPT_VK_CACHE_MAGIC &&
                hdr.version == JPT_VK_CACHE_VERSION && hdr.vendor_id == ctx->props.vendorID &&
                hdr.device_id == ctx->props.deviceID &&
                memcmp(hdr.pipeline_cache_uuid, ctx->props.pipelineCacheUUID, VK_UUID_SIZE) == 0 &&
                hdr.data_size > 0 && hdr.data_size <= 64u * 1024u * 1024u) {
                blob = malloc(hdr.data_size);
                if (blob && fread(blob, 1, hdr.data_size, f) == hdr.data_size) {
                    blob_size = hdr.data_size;
                } else {
                    free(blob);
                    blob = NULL;
                }
            }
            fclose(f);
        }
    }

    ci.initialDataSize = blob_size;
    ci.pInitialData = blob;
    if (ctx->vk.vkCreatePipelineCache(ctx->device, &ci, NULL, &ctx->pipeline_cache) != VK_SUCCESS) {
        /* Discard invalid cache data cleanly and create an empty cache. */
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        ctx->vk.vkCreatePipelineCache(ctx->device, &ci, NULL, &ctx->pipeline_cache);
    }
    free(blob);
}

void jpt_vk_context_save_pipeline_cache(const jpt_vk_context_t* ctx) {
    size_t size = 0;
    void* data = NULL;
    char tmp[544];
    char dir[512];
    char* slash;
    jpt_vk_cache_header_t hdr;
    FILE* f;

    if (!ctx || !ctx->initialized || !ctx->cache_path[0] || ctx->pipeline_cache == VK_NULL_HANDLE) return;
    if (ctx->vk.vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &size, NULL) != VK_SUCCESS || size == 0 ||
        size > 64u * 1024u * 1024u)
        return;

    data = malloc(size);
    if (!data) return;
    if (ctx->vk.vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &size, data) != VK_SUCCESS) {
        free(data);
        return;
    }

    snprintf(dir, sizeof dir, "%s", ctx->cache_path);
    slash = strrchr(dir, '/');
    {
        char* bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
    }
    if (slash) {
        *slash = 0;
        jpt_fs_mkdir(dir);
    }

    memset(&hdr, 0, sizeof hdr);
    hdr.magic = JPT_VK_CACHE_MAGIC;
    hdr.version = JPT_VK_CACHE_VERSION;
    hdr.vendor_id = ctx->props.vendorID;
    hdr.device_id = ctx->props.deviceID;
    memcpy(hdr.pipeline_cache_uuid, ctx->props.pipelineCacheUUID, VK_UUID_SIZE);
    hdr.data_size = (uint32_t)size;

    snprintf(tmp, sizeof tmp, "%s.tmp", ctx->cache_path);
    f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(&hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(data, 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
        if (ok) jpt_fs_replace(tmp, ctx->cache_path);
        else jpt_fs_remove(tmp);
    }
    free(data);
}

static int create_surface(jpt_vk_context_t* ctx) {
    if (ctx->window) {
        if (!SDL_Vulkan_CreateSurface(ctx->window, ctx->instance, NULL, &ctx->surface)) {
            set_error(ctx, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return 0;
        }
        return 1;
    }
#ifdef _WIN32
    if (ctx->native_hwnd) {
        jpt_vk_win32_surface_ci_t wci;
        VkResult vr;
        if (!ctx->vk.vkCreateWin32SurfaceKHR) {
            set_error(ctx, "vkCreateWin32SurfaceKHR symbol unavailable");
            return 0;
        }
        memset(&wci, 0, sizeof wci);
        wci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        wci.hinstance = GetModuleHandleA(NULL);
        wci.hwnd = ctx->native_hwnd;
        vr = ctx->vk.vkCreateWin32SurfaceKHR(ctx->instance, &wci, NULL, &ctx->surface);
        if (vr != VK_SUCCESS) {
            set_error(ctx, "vkCreateWin32SurfaceKHR failed: %s (%d)", jpt_vk_result_str(vr), (int)vr);
            return 0;
        }
        return 1;
    }
#endif
    set_error(ctx, "no window handle for Vulkan surface");
    return 0;
}

static void destroy_surface(jpt_vk_context_t* ctx) {
    if (!ctx || ctx->surface == VK_NULL_HANDLE) return;
    if (ctx->window) SDL_Vulkan_DestroySurface(ctx->instance, ctx->surface, NULL);
    else if (ctx->vk.vkDestroySurfaceKHR) ctx->vk.vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
    ctx->surface = VK_NULL_HANDLE;
}

int jpt_vk_context_recreate_surface(jpt_vk_context_t* ctx) {
    if (!ctx || !ctx->initialized || ctx->headless || (!ctx->window && !ctx->native_hwnd)) return 0;
    destroy_surface(ctx);
    return create_surface(ctx);
}

int jpt_vk_context_init(jpt_vk_context_t* ctx, const jpt_vk_context_desc_t* desc) {
    char err[256];
    uint32_t pd_count = 0;
    VkPhysicalDevice pds[JPT_VK_MAX_GPUS];
    jpt_vk_gpu_info_t gpus[JPT_VK_MAX_GPUS];
    int picked;
    VkResult vr;

    if (!ctx || !desc) return 0;
    memset(ctx, 0, sizeof *ctx);
    ctx->window = desc->window;
    ctx->native_hwnd = desc->native_hwnd;
    ctx->headless = (desc->window == NULL && desc->native_hwnd == NULL) ? 1 : 0;
    ctx->log_fn = desc->log_fn;
    ctx->log_user = desc->log_user;
    if (desc->cache_path && desc->cache_path[0]) snprintf(ctx->cache_path, sizeof ctx->cache_path, "%s", desc->cache_path);

    if (!jpt_vk_loader_open(&ctx->vk, desc->window != NULL, err, sizeof err)) {
        set_error(ctx, "%s", err);
        return 0;
    }

    if (create_instance(ctx, !ctx->headless, desc->enable_validation) != VK_SUCCESS) {
        jpt_vk_context_shutdown(ctx);
        return 0;
    }

    if (!jpt_vk_loader_load_instance(&ctx->vk, ctx->instance, err, sizeof err)) {
        set_error(ctx, "%s", err);
        jpt_vk_context_shutdown(ctx);
        return 0;
    }

    if (ctx->validation_enabled && ctx->debug_utils_enabled && ctx->vk.vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dci;
        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_callback;
        dci.pUserData = ctx;
        ctx->vk.vkCreateDebugUtilsMessengerEXT(ctx->instance, &dci, NULL, &ctx->debug_messenger);
    }

    if (!ctx->headless) {
        if (!create_surface(ctx)) {
            jpt_vk_context_shutdown(ctx);
            return 0;
        }
    }

    vr = ctx->vk.vkEnumeratePhysicalDevices(ctx->instance, &pd_count, NULL);
    if (vr != VK_SUCCESS || pd_count == 0) {
        set_error(ctx, "vkEnumeratePhysicalDevices found no Vulkan GPU (%s)", jpt_vk_result_str(vr));
        jpt_vk_context_shutdown(ctx);
        return 0;
    }
    if (pd_count > JPT_VK_MAX_GPUS) pd_count = JPT_VK_MAX_GPUS;
    vr = ctx->vk.vkEnumeratePhysicalDevices(ctx->instance, &pd_count, pds);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        set_error(ctx, "vkEnumeratePhysicalDevices failed: %s", jpt_vk_result_str(vr));
        jpt_vk_context_shutdown(ctx);
        return 0;
    }

    for (uint32_t i = 0; i < pd_count; i++) inspect_physical_device(&ctx->vk, pds[i], ctx->surface, &gpus[i]);
    jpt_vk_gpu_assign_labels(gpus, (int)pd_count);

    picked = jpt_vk_gpu_select(gpus, (int)pd_count, desc->gpu_request, has_gpu_env_override(), ctx->select_reason,
                               sizeof ctx->select_reason);
    if (picked < 0) {
        set_error(ctx, "%s", ctx->select_reason);
        jpt_vk_context_shutdown(ctx);
        return 0;
    }

    ctx->physical_device = pds[picked];
    ctx->gpu = gpus[picked];
    ctx->vk.vkGetPhysicalDeviceProperties(ctx->physical_device, &ctx->props);
    ctx->vk.vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &ctx->mem_props);
    ctx->graphics_family = (uint32_t)ctx->gpu.graphics_queue_family;
    ctx->present_family = ctx->headless ? ctx->graphics_family : (uint32_t)ctx->gpu.present_queue_family;

    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci[2];
        uint32_t qci_count = 1;
        const char* dev_exts[2];
        uint32_t dev_ext_count = 0;
        VkDeviceCreateInfo dci;

        memset(qci, 0, sizeof qci);
        qci[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci[0].queueFamilyIndex = ctx->graphics_family;
        qci[0].queueCount = 1;
        qci[0].pQueuePriorities = &prio;

        if (!ctx->headless && ctx->present_family != ctx->graphics_family) {
            qci[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci[1].queueFamilyIndex = ctx->present_family;
            qci[1].queueCount = 1;
            qci[1].pQueuePriorities = &prio;
            qci_count = 2;
        }

        if (!ctx->headless) dev_exts[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

        memset(&ctx->enabled_features, 0, sizeof ctx->enabled_features);
        if (ctx->gpu.has_anisotropy) ctx->enabled_features.samplerAnisotropy = VK_TRUE;
        if (ctx->gpu.has_fill_non_solid) ctx->enabled_features.fillModeNonSolid = VK_TRUE;

        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = qci_count;
        dci.pQueueCreateInfos = qci;
        dci.enabledExtensionCount = dev_ext_count;
        dci.ppEnabledExtensionNames = dev_ext_count ? dev_exts : NULL;
        dci.pEnabledFeatures = &ctx->enabled_features;

        vr = ctx->vk.vkCreateDevice(ctx->physical_device, &dci, NULL, &ctx->device);
        if (vr != VK_SUCCESS) {
            set_error(ctx, "vkCreateDevice on '%s' failed: %s (%d)", ctx->gpu.label, jpt_vk_result_str(vr), (int)vr);
            jpt_vk_context_shutdown(ctx);
            return 0;
        }
    }

    if (!jpt_vk_loader_load_device(&ctx->vk, ctx->device, err, sizeof err)) {
        set_error(ctx, "%s", err);
        jpt_vk_context_shutdown(ctx);
        return 0;
    }

    ctx->vk.vkGetDeviceQueue(ctx->device, ctx->graphics_family, 0, &ctx->graphics_queue);
    ctx->vk.vkGetDeviceQueue(ctx->device, ctx->present_family, 0, &ctx->present_queue);

    jpt_vk_allocator_init(&ctx->alloc, ctx->device, &ctx->vk, &ctx->mem_props,
                          ctx->props.limits.bufferImageGranularity, &ctx->lifetime);
    load_pipeline_cache(ctx);

    ctx->initialized = 1;
    return 1;
}

void jpt_vk_context_shutdown(jpt_vk_context_t* ctx) {
    if (!ctx) return;
    if (ctx->device != VK_NULL_HANDLE) {
        if (ctx->vk.vkDeviceWaitIdle) ctx->vk.vkDeviceWaitIdle(ctx->device);
        if (ctx->pipeline_cache != VK_NULL_HANDLE) {
            jpt_vk_context_save_pipeline_cache(ctx);
            ctx->vk.vkDestroyPipelineCache(ctx->device, ctx->pipeline_cache, NULL);
            ctx->pipeline_cache = VK_NULL_HANDLE;
        }
        jpt_vk_allocator_shutdown(&ctx->alloc);
        if (ctx->vk.vkDestroyDevice) ctx->vk.vkDestroyDevice(ctx->device, NULL);
        ctx->device = VK_NULL_HANDLE;
    }
    destroy_surface(ctx);
    if (ctx->debug_messenger != VK_NULL_HANDLE && ctx->vk.vkDestroyDebugUtilsMessengerEXT) {
        ctx->vk.vkDestroyDebugUtilsMessengerEXT(ctx->instance, ctx->debug_messenger, NULL);
        ctx->debug_messenger = VK_NULL_HANDLE;
    }
    if (ctx->instance != VK_NULL_HANDLE && ctx->vk.vkDestroyInstance) {
        ctx->vk.vkDestroyInstance(ctx->instance, NULL);
        ctx->instance = VK_NULL_HANDLE;
    }
    jpt_vk_loader_close(&ctx->vk);
    ctx->initialized = 0;
}

void jpt_vk_set_object_name(const jpt_vk_context_t* ctx, VkObjectType type, uint64_t handle, const char* name) {
    VkDebugUtilsObjectNameInfoEXT info;
    if (!ctx || !ctx->debug_utils_enabled || !ctx->vk.vkSetDebugUtilsObjectNameEXT || !handle || !name) return;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    ctx->vk.vkSetDebugUtilsObjectNameEXT(ctx->device, &info);
}

int jpt_vk_probe_headless(const char* gpu_request, jpt_vk_probe_result_t* out) {
    jpt_vk_context_t tmp;
    char err[256];
    uint32_t pd_count = 0;
    VkPhysicalDevice pds[JPT_VK_MAX_GPUS];
    VkResult vr;

    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->selected_index = -1;
    memset(&tmp, 0, sizeof tmp);

    if (!jpt_vk_loader_open(&tmp.vk, 0, err, sizeof err)) {
        snprintf(out->summary, sizeof out->summary, "%s", err);
        return 0;
    }
    if (create_instance(&tmp, 0, 0) != VK_SUCCESS) {
        snprintf(out->summary, sizeof out->summary, "%s", tmp.last_error);
        jpt_vk_context_shutdown(&tmp);
        return 0;
    }
    if (!jpt_vk_loader_load_instance(&tmp.vk, tmp.instance, err, sizeof err)) {
        snprintf(out->summary, sizeof out->summary, "%s", err);
        jpt_vk_context_shutdown(&tmp);
        return 0;
    }

    vr = tmp.vk.vkEnumeratePhysicalDevices(tmp.instance, &pd_count, NULL);
    if (vr != VK_SUCCESS || pd_count == 0) {
        snprintf(out->summary, sizeof out->summary, "no Vulkan physical devices found (%s)", jpt_vk_result_str(vr));
        jpt_vk_context_shutdown(&tmp);
        return 0;
    }
    if (pd_count > JPT_VK_MAX_GPUS) pd_count = JPT_VK_MAX_GPUS;
    tmp.vk.vkEnumeratePhysicalDevices(tmp.instance, &pd_count, pds);

    for (uint32_t i = 0; i < pd_count; i++) inspect_physical_device(&tmp.vk, pds[i], VK_NULL_HANDLE, &out->gpus[i]);
    jpt_vk_gpu_assign_labels(out->gpus, (int)pd_count);
    out->gpu_count = (int)pd_count;

    out->selected_index = jpt_vk_gpu_select(out->gpus, out->gpu_count, gpu_request, has_gpu_env_override(),
                                            out->summary, sizeof out->summary);
    out->available = out->selected_index >= 0 ? 1 : 0;
    jpt_vk_context_shutdown(&tmp);
    return out->available;
}

static const char* present_mode_name(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

static const char* format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8R8A8_SRGB";
    default: return "OTHER";
    }
}

void jpt_vk_format_startup_summary(const jpt_vk_context_t* ctx, VkPresentModeKHR present_mode, VkFormat swap_format,
                                   uint32_t internal_w, uint32_t internal_h, uint32_t msaa, float anisotropy,
                                   char* out, size_t cap) {
    if (!ctx || !out || !cap) return;
    unsigned vram_mib = (unsigned)(ctx->gpu.vram_bytes / (1024ULL * 1024ULL));
    snprintf(out, cap,
             "Vulkan %u.%u.%u | GPU: %s (%s, id=%s) | driver: %s%s%s | VRAM: %u MiB | present: %s (%s) | "
             "internal: %ux%u | MSAA: %ux | aniso: %.0fx",
             VK_API_VERSION_MAJOR(ctx->gpu.api_version), VK_API_VERSION_MINOR(ctx->gpu.api_version),
             VK_API_VERSION_PATCH(ctx->gpu.api_version), ctx->gpu.label, jpt_vk_device_type_str(ctx->gpu.device_type),
             ctx->gpu.stable_id, ctx->gpu.driver_name[0] ? ctx->gpu.driver_name : "driver",
             ctx->gpu.driver_name[0] ? " " : "", ctx->gpu.driver_version_text, vram_mib,
             present_mode_name(present_mode), format_name(swap_format), internal_w, internal_h, msaa, anisotropy);
}
