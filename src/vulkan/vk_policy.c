#include "vk_policy.h"

#include <stdio.h>
#include <string.h>

#include "jpt_config.h"

const char* jpt_vk_result_str(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    default: return "VK_RESULT_UNKNOWN";
    }
}

const char* jpt_vk_device_type_str(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

void jpt_vk_format_driver_version(uint32_t vendor_id, uint32_t driver_version, char* out, size_t cap) {
    if (!out || !cap) return;
    if (vendor_id == 0x10DEu) {
        snprintf(out, cap, "%u.%u.%u.%u", (driver_version >> 22) & 0x3FFu, (driver_version >> 14) & 0xFFu,
                 (driver_version >> 6) & 0xFFu, driver_version & 0x3Fu);
        return;
    }
#ifdef _WIN32
    if (vendor_id == 0x8086u) {
        snprintf(out, cap, "%u.%u", driver_version >> 14, driver_version & 0x3FFFu);
        return;
    }
#endif
    snprintf(out, cap, "%u.%u.%u", VK_API_VERSION_MAJOR(driver_version), VK_API_VERSION_MINOR(driver_version),
             VK_API_VERSION_PATCH(driver_version));
}

void jpt_vk_gpu_assign_labels(jpt_vk_gpu_info_t* gpus, int count) {
    for (int i = 0; i < count; i++) {
        int same = 1;
        gpus[i].label[0] = 0;
        jpt_gpu_id_format(gpus[i].vendor_id, gpus[i].device_id, gpus[i].uuid, gpus[i].stable_id,
                          sizeof gpus[i].stable_id);
        if (!gpus[i].name[0]) continue;
        for (int j = 0; j < i; j++)
            if (!strcmp(gpus[j].name, gpus[i].name)) same++;
        if (same == 1) snprintf(gpus[i].label, sizeof gpus[i].label, "%s", gpus[i].name);
        else snprintf(gpus[i].label, sizeof gpus[i].label, "%s (%d)", gpus[i].name, same);
    }
}

int jpt_vk_pick_depth_format(uint32_t supported_mask, VkFormat* out_format, int* out_has_stencil) {
    if (supported_mask & JPT_VK_DEPTH_BIT_D24S8) {
        if (out_format) *out_format = VK_FORMAT_D24_UNORM_S8_UINT;
        if (out_has_stencil) *out_has_stencil = 1;
        return 1;
    }
    if (supported_mask & JPT_VK_DEPTH_BIT_D32S8) {
        if (out_format) *out_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (out_has_stencil) *out_has_stencil = 1;
        return 1;
    }
    if (supported_mask & JPT_VK_DEPTH_BIT_D32) {
        if (out_format) *out_format = VK_FORMAT_D32_SFLOAT;
        if (out_has_stencil) *out_has_stencil = 0;
        return 1;
    }
    if (supported_mask & JPT_VK_DEPTH_BIT_D16) {
        if (out_format) *out_format = VK_FORMAT_D16_UNORM;
        if (out_has_stencil) *out_has_stencil = 0;
        return 1;
    }
    if (out_format) *out_format = VK_FORMAT_UNDEFINED;
    if (out_has_stencil) *out_has_stencil = 0;
    return 0;
}

int jpt_vk_gpu_check_requirements(jpt_vk_gpu_info_t* gpu, int require_present) {
    if (!gpu) return 0;
    gpu->usable = 0;
    gpu->reject_reason[0] = 0;

    if (gpu->api_version < VK_API_VERSION_1_1) {
        snprintf(gpu->reject_reason, sizeof gpu->reject_reason, "Vulkan API %u.%u below minimum 1.1",
                 VK_API_VERSION_MAJOR(gpu->api_version), VK_API_VERSION_MINOR(gpu->api_version));
        return 0;
    }
    if (gpu->graphics_queue_family < 0) {
        snprintf(gpu->reject_reason, sizeof gpu->reject_reason, "no graphics queue family");
        return 0;
    }
    if (require_present) {
        if (!gpu->has_swapchain_ext) {
            snprintf(gpu->reject_reason, sizeof gpu->reject_reason, "missing VK_KHR_swapchain");
            return 0;
        }
        if (gpu->present_queue_family < 0) {
            snprintf(gpu->reject_reason, sizeof gpu->reject_reason, "no presentation queue for surface");
            return 0;
        }
    }
    if (!jpt_vk_pick_depth_format(gpu->depth_format_mask, &gpu->preferred_depth_format, &gpu->depth_has_stencil)) {
        snprintf(gpu->reject_reason, sizeof gpu->reject_reason, "no supported depth attachment format");
        return 0;
    }
    gpu->usable = 1;
    return 1;
}

static int device_tier_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 0;
    default: return 1;
    }
}

static int gpu_better_for_auto(const jpt_vk_gpu_info_t* a, const jpt_vk_gpu_info_t* b) {
    int rank_a = device_tier_rank(a->device_type);
    int rank_b = device_tier_rank(b->device_type);
    if (rank_a != rank_b) return rank_a > rank_b;
    if (a->api_version >= VK_API_VERSION_1_2 && b->api_version < VK_API_VERSION_1_2) return 1;
    if (b->api_version >= VK_API_VERSION_1_2 && a->api_version < VK_API_VERSION_1_2) return 0;
    if (a->vram_bytes != b->vram_bytes) return a->vram_bytes > b->vram_bytes;
    return 0;
}

int jpt_vk_gpu_select(const jpt_vk_gpu_info_t* gpus, int count, const char* request, int has_env_override,
                      char* reason, size_t cap) {
    int best = -1;
    int env_first = -1;
    int explicit_unusable = -1;

    if (reason && cap) reason[0] = 0;
    if (!gpus || count <= 0) {
        if (reason && cap) snprintf(reason, cap, "no Vulkan physical devices found");
        return -1;
    }

    if (!jpt_gpu_request_is_auto(request)) {
        for (int i = 0; i < count; i++) {
            if (!jpt_gpu_match_request(request, gpus[i].label, gpus[i].name, gpus[i].vendor_id, gpus[i].device_id,
                                       gpus[i].uuid))
                continue;
            if (gpus[i].usable) {
                if (reason && cap) snprintf(reason, cap, "selected requested GPU '%s'", gpus[i].label);
                return i;
            }
            explicit_unusable = i;
        }
    }

    for (int i = 0; i < count; i++) {
        if (!gpus[i].usable) continue;
        if (env_first < 0) env_first = i;
        if (best < 0 || gpu_better_for_auto(&gpus[i], &gpus[best])) best = i;
    }

    if (best < 0) {
        if (reason && cap) {
            if (explicit_unusable >= 0 && gpus[explicit_unusable].reject_reason[0])
                snprintf(reason, cap, "requested GPU '%s' is unusable (%s) and no other usable GPU exists",
                         gpus[explicit_unusable].label, gpus[explicit_unusable].reject_reason);
            else
                snprintf(reason, cap, "no Vulkan device meets minimum requirements");
        }
        return -1;
    }

    if (jpt_gpu_request_is_auto(request) && has_env_override && env_first >= 0) {
        if (reason && cap)
            snprintf(reason, cap, "selected loader-preferred GPU '%s' (environment override)", gpus[env_first].label);
        return env_first;
    }

    if (reason && cap) {
        if (explicit_unusable >= 0) {
            snprintf(reason, cap, "requested GPU '%s' unusable (%s); auto-selected '%s'",
                     gpus[explicit_unusable].label, gpus[explicit_unusable].reject_reason, gpus[best].label);
        } else if (!jpt_gpu_request_is_auto(request)) {
            snprintf(reason, cap, "requested GPU '%s' not found; auto-selected '%s'", request, gpus[best].label);
        } else {
            snprintf(reason, cap, "auto-selected %s GPU '%s'", jpt_vk_device_type_str(gpus[best].device_type),
                     gpus[best].label);
        }
    }
    return best;
}

float jpt_vk_clamp_anisotropy(int filter_setting, int device_supports_aniso, float device_max_aniso) {
    float requested = 1.0f;
    if (!device_supports_aniso || device_max_aniso < 1.0f) return 1.0f;
    switch (filter_setting) {
    case JPT_FILTER_ANISO4: requested = 4.0f; break;
    case JPT_FILTER_ANISO8: requested = 8.0f; break;
    case JPT_FILTER_ANISO16: requested = 16.0f; break;
    default: return 1.0f;
    }
    return requested > device_max_aniso ? device_max_aniso : requested;
}

uint32_t jpt_vk_world_sampler_class(int filter_setting, int game_mag, int game_min) {
    const int forced = filter_setting == JPT_FILTER_NEAREST || filter_setting == JPT_FILTER_BILINEAR;
    const int mag = forced || game_mag, min = forced || game_min;
    if (!mag && !min) return JPT_VK_SAMPLER_POINT;
    if (mag && min) return filter_setting == JPT_FILTER_ORIGINAL ? JPT_VK_SAMPLER_LINEAR : JPT_VK_SAMPLER_MIPS;
    return mag ? JPT_VK_SAMPLER_MAG_LINEAR : JPT_VK_SAMPLER_MIN_LINEAR;
}

/* Nearest turns the mipmapped class into point sampling at the nearest level, as Direct3D 11's MIN_MAG_MIP_POINT. */
jpt_vk_world_sampler_t jpt_vk_world_sampler(int filter_setting, uint32_t sampler_class, float active_aniso) {
    static const int k_class[JPT_VK_SAMPLER_CLASSES][3] = { { 1, 1, 1 }, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 },
                                                             { 1, 1, 0 } };
    const int* c = k_class[sampler_class < JPT_VK_SAMPLER_CLASSES ? sampler_class : JPT_VK_SAMPLER_POINT];
    const int nearest = filter_setting == JPT_FILTER_NEAREST;
    jpt_vk_world_sampler_t s;
    s.mag = c[0] && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.min = c[1] && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    s.mipmap = c[2] && !nearest ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.max_lod = c[2] ? 12.0f : 0.0f;
    s.anisotropy = c[2] && !nearest && active_aniso > 1.0f ? active_aniso : 1.0f;
    return s;
}

uint32_t jpt_vk_clamp_msaa(uint32_t requested_msaa, VkSampleCountFlags color_counts, VkSampleCountFlags depth_counts) {
    VkSampleCountFlags both = color_counts & depth_counts;
    if (requested_msaa >= 8 && (both & VK_SAMPLE_COUNT_8_BIT)) return 8;
    if (requested_msaa >= 4 && (both & VK_SAMPLE_COUNT_4_BIT)) return 4;
    if (requested_msaa >= 2 && (both & VK_SAMPLE_COUNT_2_BIT)) return 2;
    return 1;
}

VkPolygonMode jpt_vk_resolve_fill_mode(uint32_t d3d_fill_mode, int supports_non_solid) {
    if (!supports_non_solid) return VK_POLYGON_MODE_FILL;
    if (d3d_fill_mode == 1u) return VK_POLYGON_MODE_POINT;
    if (d3d_fill_mode == 2u) return VK_POLYGON_MODE_LINE;
    return VK_POLYGON_MODE_FILL;
}

static int mode_present(const VkPresentModeKHR* modes, uint32_t count, VkPresentModeKHR want) {
    for (uint32_t i = 0; i < count; i++)
        if (modes[i] == want) return 1;
    return 0;
}

VkPresentModeKHR jpt_vk_choose_present_mode(int vsync, const VkPresentModeKHR* modes, uint32_t count) {
    if (vsync) return VK_PRESENT_MODE_FIFO_KHR;
    if (modes && count) {
        if (mode_present(modes, count, VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        if (mode_present(modes, count, VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (mode_present(modes, count, VK_PRESENT_MODE_FIFO_RELAXED_KHR)) return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

uint32_t jpt_vk_choose_image_count_for(uint32_t min_images, uint32_t max_images, VkPresentModeKHR mode, int gamescope) {
    /* Gamescope already queues frames in its compositor and reports minImageCount >= 3; adding one only adds latency. */
    uint32_t want = gamescope ? (min_images > 2u ? min_images : 2u) : (min_images + 1u);
    if (!gamescope && mode == VK_PRESENT_MODE_MAILBOX_KHR && want < 3u) want = 3u;
    if (want < 2u) want = 2u;
    if (max_images > 0u && want > max_images) want = max_images;
    if (want < min_images) want = min_images;
    return want;
}

uint32_t jpt_vk_choose_image_count(uint32_t min_images, uint32_t max_images, VkPresentModeKHR mode) {
    return jpt_vk_choose_image_count_for(min_images, max_images, mode, 0);
}

VkSurfaceFormatKHR jpt_vk_choose_surface_format(const VkSurfaceFormatKHR* formats, uint32_t count) {
    VkSurfaceFormatKHR fallback = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    if (!formats || !count) return fallback;
    for (uint32_t i = 0; i < count; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[i];
    for (uint32_t i = 0; i < count; i++)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return formats[i];
    return formats[0];
}

void jpt_vk_sc_init(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px, int vsync, int fullscreen) {
    if (!h) return;
    memset(h, 0, sizeof *h);
    h->width = w;
    h->height = h_px;
    h->vsync = vsync ? 1 : 0;
    h->fullscreen = fullscreen ? 1 : 0;
    h->dirty = (w > 0 && h_px > 0) ? 1 : 0;
}

jpt_vk_sc_action_t jpt_vk_sc_on_resize(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px) {
    if (!h) return JPT_VK_SC_ACTION_OK;
    if (h->device_lost) return JPT_VK_SC_ACTION_DEVICE_LOST;
    if (w == 0 || h_px == 0) {
        h->width = 0;
        h->height = 0;
        h->dirty = 0;
        return JPT_VK_SC_ACTION_WAIT_EXTENT;
    }
    if (w != h->width || h_px != h->height) {
        h->width = w;
        h->height = h_px;
        h->consecutive_suboptimal = 0;
        h->dirty = 1;
        return JPT_VK_SC_ACTION_RECREATE;
    }
    return h->dirty ? JPT_VK_SC_ACTION_RECREATE : JPT_VK_SC_ACTION_OK;
}

jpt_vk_sc_action_t jpt_vk_sc_on_settings(jpt_vk_swapchain_health_t* h, int vsync, int fullscreen) {
    int v = vsync ? 1 : 0;
    int fs = fullscreen ? 1 : 0;
    if (!h) return JPT_VK_SC_ACTION_OK;
    if (h->device_lost) return JPT_VK_SC_ACTION_DEVICE_LOST;
    if (v != h->vsync || fs != h->fullscreen) {
        h->vsync = v;
        h->fullscreen = fs;
        h->consecutive_suboptimal = 0;
        if (h->width == 0 || h->height == 0) return JPT_VK_SC_ACTION_WAIT_EXTENT;
        h->dirty = 1;
        return JPT_VK_SC_ACTION_RECREATE;
    }
    return JPT_VK_SC_ACTION_OK;
}

jpt_vk_sc_action_t jpt_vk_sc_on_result(jpt_vk_swapchain_health_t* h, VkResult result) {
    if (!h) return JPT_VK_SC_ACTION_OK;
    if (h->device_lost) return JPT_VK_SC_ACTION_DEVICE_LOST;
    if (result == VK_SUCCESS) {
        h->consecutive_suboptimal = 0;
        h->surface_lost = 0;
        return (h->width == 0 || h->height == 0) ? JPT_VK_SC_ACTION_WAIT_EXTENT : JPT_VK_SC_ACTION_OK;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        h->device_lost = 1;
        return JPT_VK_SC_ACTION_DEVICE_LOST;
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        h->surface_lost = 1;
        h->consecutive_suboptimal = 0;
        if (h->width == 0 || h->height == 0) return JPT_VK_SC_ACTION_WAIT_EXTENT;
        h->dirty = 1;
        return JPT_VK_SC_ACTION_RECREATE_SURFACE;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        h->consecutive_suboptimal = 0;
        if (h->width == 0 || h->height == 0) return JPT_VK_SC_ACTION_WAIT_EXTENT;
        h->dirty = 1;
        return JPT_VK_SC_ACTION_RECREATE;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        if (h->width == 0 || h->height == 0) return JPT_VK_SC_ACTION_WAIT_EXTENT;
        if (h->consecutive_suboptimal < JPT_VK_SUBOPTIMAL_LIMIT) {
            h->consecutive_suboptimal++;
            h->dirty = 1;
            return JPT_VK_SC_ACTION_RECREATE;
        }
        /* Accept persistent SUBOPTIMAL (Gamescope / rotated compositor) instead of recreating every frame. */
        return JPT_VK_SC_ACTION_OK;
    }
    return JPT_VK_SC_ACTION_OK;
}

void jpt_vk_sc_on_recreated(jpt_vk_swapchain_health_t* h, uint32_t w, uint32_t h_px, int vsync, int fullscreen) {
    if (!h) return;
    h->width = w;
    h->height = h_px;
    h->vsync = vsync ? 1 : 0;
    h->fullscreen = fullscreen ? 1 : 0;
    h->dirty = 0;
    h->surface_lost = 0;
    h->recreate_count++;
}

static uint8_t topology_class(jpt_render_prim_t prim) {
    switch (prim) {
    case JPT_PRIM_POINTLIST: return 1;
    case JPT_PRIM_LINELIST:
    case JPT_PRIM_LINESTRIP: return 2;
    default: return 0; /* triangles / strips / fans expand into indexed triangles */
    }
}

void jpt_vk_pipeline_key_from_state(const jpt_render_state_t* st, jpt_render_prim_t prim, uint32_t sample_count,
                                    jpt_vk_rp_kind_t rp_kind, int supports_non_solid, jpt_vk_pipeline_key_t* out) {
    memset(out, 0, sizeof *out);
    if (!st) return;
    out->shader_kind = (uint8_t)(st->water_mode ? JPT_VK_SHADER_WATER
                                                : st->sky_mode ? JPT_VK_SHADER_SKY
                                                : st->lit ? JPT_VK_SHADER_LIT : JPT_VK_SHADER_FIXED);
    out->topology = topology_class(prim);
    out->cull_mode = (uint8_t)(st->cull_mode <= 3u ? st->cull_mode : 1u);
    out->fill_mode = (uint8_t)jpt_vk_resolve_fill_mode(st->fill_mode, supports_non_solid);
    out->blend_enable = st->alpha_blend ? 1u : 0u;
    out->src_blend = out->blend_enable ? (uint8_t)(st->src_blend & 0x1Fu) : 0u;
    out->dst_blend = out->blend_enable ? (uint8_t)(st->dst_blend & 0x1Fu) : 0u;
    out->z_enable = st->z_enable ? 1u : 0u;
    out->z_write = (st->z_enable && st->z_write) ? 1u : 0u;
    out->z_func = out->z_enable ? (uint8_t)(st->z_func & 0xFu) : 0u;
    out->stencil_mode = (uint8_t)(st->stencil_mark & 0x3u);
    out->sample_count = (uint8_t)(sample_count ? sample_count : 1u);
    out->renderpass_kind = (uint8_t)rp_kind;
}

int jpt_vk_pipeline_key_equal(const jpt_vk_pipeline_key_t* a, const jpt_vk_pipeline_key_t* b) {
    return a && b && memcmp(a, b, sizeof *a) == 0;
}

uint64_t jpt_vk_pipeline_key_hash(const jpt_vk_pipeline_key_t* k) {
    const uint8_t* p = (const uint8_t*)k;
    uint64_t h = 14695981039346656037ULL;
    if (!k) return 0;
    for (size_t i = 0; i < sizeof *k; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void layout_stage_access(VkImageLayout layout, int is_src, VkPipelineStageFlags* stage, VkAccessFlags* access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        *stage = is_src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        *access = 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        *stage = is_src ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        *access = is_src ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
        break;
    default:
        *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    }
}

int jpt_vk_plan_image_barrier(VkImageLayout old_layout, VkImageLayout new_layout, VkImageAspectFlags aspect,
                              jpt_vk_barrier_plan_t* out) {
    if (!out || aspect == 0) return 0;
    layout_stage_access(old_layout, 1, &out->src_stage, &out->src_access);
    layout_stage_access(new_layout, 0, &out->dst_stage, &out->dst_access);
    out->aspect = aspect;
    return 1;
}

void jpt_vk_compute_geometry(uint32_t guest_w, uint32_t guest_h, uint32_t internal_w, uint32_t internal_h,
                             uint32_t window_w, uint32_t window_h, int stretch, jpt_vk_frame_geometry_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->guest_w = guest_w ? guest_w : 640u;
    out->guest_h = guest_h ? guest_h : 480u;
    out->internal_w = internal_w ? internal_w : out->guest_w;
    out->internal_h = internal_h ? internal_h : out->guest_h;
    out->window_w = window_w;
    out->window_h = window_h;
    if (window_w == 0 || window_h == 0) return;
    if (stretch) {
        out->present_rect.x = 0;
        out->present_rect.y = 0;
        out->present_rect.w = window_w;
        out->present_rect.h = window_h;
        return;
    }
    uint64_t lhs = (uint64_t)window_w * (uint64_t)out->internal_h;
    uint64_t rhs = (uint64_t)window_h * (uint64_t)out->internal_w;
    if (lhs > rhs) {
        uint32_t dst_w = (uint32_t)((rhs + out->internal_h / 2u) / out->internal_h);
        if (dst_w > window_w) dst_w = window_w;
        if (dst_w == 0) dst_w = 1;
        out->present_rect.x = (int32_t)((window_w - dst_w) / 2u);
        out->present_rect.y = 0;
        out->present_rect.w = dst_w;
        out->present_rect.h = window_h;
    } else if (lhs < rhs) {
        uint32_t dst_h = (uint32_t)((lhs + out->internal_w / 2u) / out->internal_w);
        if (dst_h > window_h) dst_h = window_h;
        if (dst_h == 0) dst_h = 1;
        out->present_rect.x = 0;
        out->present_rect.y = (int32_t)((window_h - dst_h) / 2u);
        out->present_rect.w = window_w;
        out->present_rect.h = dst_h;
    } else {
        out->present_rect.x = 0;
        out->present_rect.y = 0;
        out->present_rect.w = window_w;
        out->present_rect.h = window_h;
    }
}

int jpt_vk_lifetime_total_objects(const jpt_vk_lifetime_t* lt) {
    if (!lt) return 0;
    return lt->textures + lt->image_views + lt->buffers + lt->allocations + lt->pipelines + lt->pipeline_layouts +
           lt->descriptor_pools + lt->framebuffers + lt->render_passes + lt->samplers + lt->swapchain_images +
           lt->command_pools + lt->fences + lt->semaphores;
}

int jpt_vk_lifetime_is_clean(const jpt_vk_lifetime_t* lt) {
    if (!lt) return 0;
    return lt->textures == 0 && lt->image_views == 0 && lt->buffers == 0 && lt->allocations == 0 &&
           lt->pipelines == 0 && lt->pipeline_layouts == 0 && lt->descriptor_pools == 0 && lt->framebuffers == 0 &&
           lt->render_passes == 0 && lt->samplers == 0 && lt->swapchain_images == 0 && lt->command_pools == 0 &&
           lt->fences == 0 && lt->semaphores == 0 && lt->allocated_bytes == 0;
}
