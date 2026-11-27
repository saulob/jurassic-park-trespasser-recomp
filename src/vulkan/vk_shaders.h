#ifndef JPT_VK_SHADERS_H
#define JPT_VK_SHADERS_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "vk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPT_VK_SH_TL_VERT = 0,
    JPT_VK_SH_FIXED_FRAG = 1,
    JPT_VK_SH_WATER_FRAG = 2,
    JPT_VK_SH_SKY_FRAG = 3,
    JPT_VK_SH_QUAD_VERT = 4,
    JPT_VK_SH_FILL_FRAG = 5,
    JPT_VK_SH_COPY_FRAG = 6,
    JPT_VK_SH_COPY_CPU_FRAG = 7,
    JPT_VK_SH_PACK565_FRAG = 8,
    JPT_VK_SH_PRESENT_FRAG = 9,
    JPT_VK_SH_AO_PREPARE_FRAG = 10,
    JPT_VK_SH_AO_PREPARE_MSAA_FRAG = 11,
    JPT_VK_SH_AO_GENERATE_FRAG = 12,
    JPT_VK_SH_AO_BLUR_FRAG = 13,
    JPT_VK_SH_AO_APPLY_FRAG = 14,
    JPT_VK_SH_VIDEO_CONVERT_FRAG = 15,
    JPT_VK_SH_VIDEO_SMOOTH_FRAG = 16,
    JPT_VK_SH_POLY_VERT = 17,
    JPT_VK_SH_POLY_TEXTURE_FRAG = 18,
    JPT_VK_SH_POLY_BUMP_FRAG = 19,
    JPT_VK_SH_AO_APPLY_MASKED_FRAG = 20,
    JPT_VK_SH_AO_APPLY_MASKED_MSAA_FRAG = 21,
    JPT_VK_SH_VIDEO_VERT = 22,
    JPT_VK_SH_VIDEO_DEBLOCK_X_FRAG = 23,
    JPT_VK_SH_VIDEO_DEBLOCK_Y_FRAG = 24,
    JPT_VK_SH_VIDEO_GLOW_FRAG = 25,
    JPT_VK_SH_VIDEO_FINAL_FRAG = 26,
    JPT_VK_SH_UI_VERT = 27,
    JPT_VK_SH_UI_FRAG = 28,
    JPT_VK_SH_LIT_VERT = 29,
    JPT_VK_SH_LIT_FRAG = 30,
    JPT_VK_SH_COUNT = 31
} jpt_vk_shader_id_t;

const char* jpt_vk_shader_bundle_sha256(void);
int jpt_vk_shader_count(void);
const char* jpt_vk_shader_name(int index);
const uint32_t* jpt_vk_shader_words(int index, size_t* out_bytes);

int jpt_vk_shaders_create(jpt_vk_context_t* ctx, VkShaderModule out_modules[JPT_VK_SH_COUNT]);
void jpt_vk_shaders_destroy(jpt_vk_context_t* ctx, VkShaderModule modules[JPT_VK_SH_COUNT]);

#ifdef __cplusplus
}
#endif

#endif
