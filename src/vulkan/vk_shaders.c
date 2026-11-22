#include "vk_shaders.h"

#include <string.h>

#include "vk_shader_spv.inc"

const char* jpt_vk_shader_bundle_sha256(void) { return JPT_VK_SHADER_BUNDLE_SHA256; }

int jpt_vk_shader_count(void) { return JPT_VK_SHADER_COUNT; }

const char* jpt_vk_shader_name(int index) {
    if (index < 0 || index >= JPT_VK_SHADER_COUNT) return "";
    return k_vk_spv_bundle[index].name;
}

const uint32_t* jpt_vk_shader_words(int index, size_t* out_bytes) {
    if (index < 0 || index >= JPT_VK_SHADER_COUNT) {
        if (out_bytes) *out_bytes = 0;
        return NULL;
    }
    if (out_bytes) *out_bytes = k_vk_spv_bundle[index].byte_size;
    return k_vk_spv_bundle[index].words;
}

int jpt_vk_shaders_create(jpt_vk_context_t* ctx, VkShaderModule out_modules[JPT_VK_SH_COUNT]) {
    if (!ctx || !out_modules) return 0;
    memset(out_modules, 0, sizeof(VkShaderModule) * JPT_VK_SH_COUNT);
    for (int i = 0; i < JPT_VK_SHADER_COUNT; i++) {
        VkShaderModuleCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = k_vk_spv_bundle[i].byte_size;
        ci.pCode = k_vk_spv_bundle[i].words;
        if (ctx->vk.vkCreateShaderModule(ctx->device, &ci, NULL, &out_modules[i]) != VK_SUCCESS) {
            jpt_vk_shaders_destroy(ctx, out_modules);
            return 0;
        }
        jpt_vk_set_object_name(ctx, VK_OBJECT_TYPE_SHADER_MODULE, (uint64_t)(uintptr_t)out_modules[i],
                               k_vk_spv_bundle[i].name);
    }
    return 1;
}

void jpt_vk_shaders_destroy(jpt_vk_context_t* ctx, VkShaderModule modules[JPT_VK_SH_COUNT]) {
    if (!ctx || !modules) return;
    for (int i = 0; i < JPT_VK_SH_COUNT; i++) {
        if (modules[i] != VK_NULL_HANDLE) {
            ctx->vk.vkDestroyShaderModule(ctx->device, modules[i], NULL);
            modules[i] = VK_NULL_HANDLE;
        }
    }
}
