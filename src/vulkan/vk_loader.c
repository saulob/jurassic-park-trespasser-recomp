#include "vk_loader.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <string.h>

static void set_err(char* err, size_t cap, const char* msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

/* ISO C forbids direct conversion between void* and function pointers under -Wpedantic; memcpy avoids UB. */
static void copy_fn(void* dst_fn_ptr, void* raw_sym) {
    memcpy(dst_fn_ptr, &raw_sym, sizeof raw_sym);
}

#define LOAD_GLOBAL(name) \
    do { \
        PFN_vkVoidFunction fn_ = l->vkGetInstanceProcAddr(VK_NULL_HANDLE, #name); \
        memcpy(&l->name, &fn_, sizeof fn_); \
    } while (0)

#define LOAD_INST(inst, name) \
    do { \
        PFN_vkVoidFunction fn_ = l->vkGetInstanceProcAddr((inst), #name); \
        memcpy(&l->name, &fn_, sizeof fn_); \
    } while (0)

#define LOAD_DEV(dev, name) \
    do { \
        PFN_vkVoidFunction fn_ = l->vkGetDeviceProcAddr((dev), #name); \
        memcpy(&l->name, &fn_, sizeof fn_); \
    } while (0)

int jpt_vk_loader_open(jpt_vk_loader_t* l, int use_sdl_video, char* err, size_t err_cap) {
    if (!l) return 0;
    memset(l, 0, sizeof *l);

    if (use_sdl_video) {
        if (!SDL_Vulkan_LoadLibrary(NULL)) {
            snprintf(err, err_cap, "SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
            return 0;
        }
        l->via_sdl_vulkan = 1;
        SDL_FunctionPointer sym = SDL_Vulkan_GetVkGetInstanceProcAddr();
        if (!sym) {
            set_err(err, err_cap, "SDL_Vulkan_GetVkGetInstanceProcAddr returned NULL");
            SDL_Vulkan_UnloadLibrary();
            l->via_sdl_vulkan = 0;
            return 0;
        }
        memcpy(&l->vkGetInstanceProcAddr, &sym, sizeof sym);
    } else {
        SDL_SharedObject* obj = NULL;
#ifdef _WIN32
        obj = SDL_LoadObject("vulkan-1.dll");
#else
        obj = SDL_LoadObject("libvulkan.so.1");
        if (!obj) obj = SDL_LoadObject("libvulkan.so");
#endif
        if (!obj) {
            snprintf(err, err_cap, "Vulkan loader library not found (%s)", SDL_GetError());
            return 0;
        }
        SDL_FunctionPointer sym = SDL_LoadFunction(obj, "vkGetInstanceProcAddr");
        if (!sym) {
            SDL_UnloadObject(obj);
            set_err(err, err_cap, "vkGetInstanceProcAddr symbol not found in Vulkan loader");
            return 0;
        }
        l->shared_obj = obj;
        memcpy(&l->vkGetInstanceProcAddr, &sym, sizeof sym);
    }

    (void)copy_fn;
    LOAD_GLOBAL(vkEnumerateInstanceVersion);
    LOAD_GLOBAL(vkEnumerateInstanceExtensionProperties);
    LOAD_GLOBAL(vkEnumerateInstanceLayerProperties);
    LOAD_GLOBAL(vkCreateInstance);

    if (!l->vkCreateInstance || !l->vkEnumerateInstanceExtensionProperties) {
        set_err(err, err_cap, "Vulkan loader missing vkCreateInstance / vkEnumerateInstanceExtensionProperties");
        jpt_vk_loader_close(l);
        return 0;
    }

    l->loaded = 1;
    return 1;
}

int jpt_vk_loader_load_instance(jpt_vk_loader_t* l, VkInstance instance, char* err, size_t err_cap) {
    if (!l || !l->loaded || instance == VK_NULL_HANDLE) {
        set_err(err, err_cap, "invalid Vulkan loader or instance");
        return 0;
    }
    LOAD_INST(instance, vkDestroyInstance);
    LOAD_INST(instance, vkEnumeratePhysicalDevices);
    LOAD_INST(instance, vkGetPhysicalDeviceProperties);
    LOAD_INST(instance, vkGetPhysicalDeviceProperties2);
    LOAD_INST(instance, vkGetPhysicalDeviceFeatures);
    LOAD_INST(instance, vkGetPhysicalDeviceFeatures2);
    LOAD_INST(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INST(instance, vkGetPhysicalDeviceMemoryProperties2);
    LOAD_INST(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(instance, vkGetPhysicalDeviceFormatProperties);
    LOAD_INST(instance, vkEnumerateDeviceExtensionProperties);
    LOAD_INST(instance, vkCreateDevice);
    LOAD_INST(instance, vkGetDeviceProcAddr);

    LOAD_INST(instance, vkDestroySurfaceKHR);
    LOAD_INST(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INST(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INST(instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INST(instance, vkGetPhysicalDeviceSurfacePresentModesKHR);
#ifdef _WIN32
    LOAD_INST(instance, vkCreateWin32SurfaceKHR);
#endif

    LOAD_INST(instance, vkCreateDebugUtilsMessengerEXT);
    LOAD_INST(instance, vkDestroyDebugUtilsMessengerEXT);
    LOAD_INST(instance, vkSetDebugUtilsObjectNameEXT);

    if (!l->vkDestroyInstance || !l->vkEnumeratePhysicalDevices || !l->vkGetPhysicalDeviceProperties ||
        !l->vkCreateDevice || !l->vkGetDeviceProcAddr) {
        set_err(err, err_cap, "Vulkan instance missing core entry points");
        return 0;
    }
    return 1;
}

int jpt_vk_loader_load_device(jpt_vk_loader_t* l, VkDevice device, char* err, size_t err_cap) {
    if (!l || !l->vkGetDeviceProcAddr || device == VK_NULL_HANDLE) {
        set_err(err, err_cap, "invalid Vulkan loader or device");
        return 0;
    }
    LOAD_DEV(device, vkDestroyDevice);
    LOAD_DEV(device, vkGetDeviceQueue);
    LOAD_DEV(device, vkDeviceWaitIdle);
    LOAD_DEV(device, vkQueueSubmit);
    LOAD_DEV(device, vkQueueWaitIdle);

    LOAD_DEV(device, vkCreateSwapchainKHR);
    LOAD_DEV(device, vkDestroySwapchainKHR);
    LOAD_DEV(device, vkGetSwapchainImagesKHR);
    LOAD_DEV(device, vkAcquireNextImageKHR);
    LOAD_DEV(device, vkQueuePresentKHR);

    LOAD_DEV(device, vkAllocateMemory);
    LOAD_DEV(device, vkFreeMemory);
    LOAD_DEV(device, vkMapMemory);
    LOAD_DEV(device, vkUnmapMemory);
    LOAD_DEV(device, vkFlushMappedMemoryRanges);
    LOAD_DEV(device, vkInvalidateMappedMemoryRanges);

    LOAD_DEV(device, vkCreateBuffer);
    LOAD_DEV(device, vkDestroyBuffer);
    LOAD_DEV(device, vkGetBufferMemoryRequirements);
    LOAD_DEV(device, vkBindBufferMemory);

    LOAD_DEV(device, vkCreateImage);
    LOAD_DEV(device, vkDestroyImage);
    LOAD_DEV(device, vkGetImageMemoryRequirements);
    LOAD_DEV(device, vkBindImageMemory);
    LOAD_DEV(device, vkCreateImageView);
    LOAD_DEV(device, vkDestroyImageView);

    LOAD_DEV(device, vkCreateSampler);
    LOAD_DEV(device, vkDestroySampler);

    LOAD_DEV(device, vkCreateShaderModule);
    LOAD_DEV(device, vkDestroyShaderModule);

    LOAD_DEV(device, vkCreateRenderPass);
    LOAD_DEV(device, vkDestroyRenderPass);
    LOAD_DEV(device, vkCreateFramebuffer);
    LOAD_DEV(device, vkDestroyFramebuffer);

    LOAD_DEV(device, vkCreateDescriptorSetLayout);
    LOAD_DEV(device, vkDestroyDescriptorSetLayout);
    LOAD_DEV(device, vkCreateDescriptorPool);
    LOAD_DEV(device, vkDestroyDescriptorPool);
    LOAD_DEV(device, vkResetDescriptorPool);
    LOAD_DEV(device, vkAllocateDescriptorSets);
    LOAD_DEV(device, vkUpdateDescriptorSets);

    LOAD_DEV(device, vkCreatePipelineLayout);
    LOAD_DEV(device, vkDestroyPipelineLayout);
    LOAD_DEV(device, vkCreatePipelineCache);
    LOAD_DEV(device, vkDestroyPipelineCache);
    LOAD_DEV(device, vkGetPipelineCacheData);
    LOAD_DEV(device, vkCreateGraphicsPipelines);
    LOAD_DEV(device, vkDestroyPipeline);

    LOAD_DEV(device, vkCreateCommandPool);
    LOAD_DEV(device, vkDestroyCommandPool);
    LOAD_DEV(device, vkResetCommandPool);
    LOAD_DEV(device, vkAllocateCommandBuffers);
    LOAD_DEV(device, vkFreeCommandBuffers);
    LOAD_DEV(device, vkBeginCommandBuffer);
    LOAD_DEV(device, vkEndCommandBuffer);
    LOAD_DEV(device, vkResetCommandBuffer);

    LOAD_DEV(device, vkCreateFence);
    LOAD_DEV(device, vkDestroyFence);
    LOAD_DEV(device, vkWaitForFences);
    LOAD_DEV(device, vkResetFences);
    LOAD_DEV(device, vkCreateSemaphore);
    LOAD_DEV(device, vkDestroySemaphore);

    LOAD_DEV(device, vkCmdBeginRenderPass);
    LOAD_DEV(device, vkCmdEndRenderPass);
    LOAD_DEV(device, vkCmdBindPipeline);
    LOAD_DEV(device, vkCmdSetViewport);
    LOAD_DEV(device, vkCmdSetScissor);
    LOAD_DEV(device, vkCmdSetDepthBias);
    LOAD_DEV(device, vkCmdSetStencilReference);
    LOAD_DEV(device, vkCmdBindDescriptorSets);
    LOAD_DEV(device, vkCmdPushConstants);
    LOAD_DEV(device, vkCmdBindVertexBuffers);
    LOAD_DEV(device, vkCmdBindIndexBuffer);
    LOAD_DEV(device, vkCmdDraw);
    LOAD_DEV(device, vkCmdDrawIndexed);
    LOAD_DEV(device, vkCmdClearAttachments);
    LOAD_DEV(device, vkCmdPipelineBarrier);
    LOAD_DEV(device, vkCmdCopyBuffer);
    LOAD_DEV(device, vkCmdCopyBufferToImage);
    LOAD_DEV(device, vkCmdCopyImageToBuffer);
    LOAD_DEV(device, vkCmdCopyImage);
    LOAD_DEV(device, vkCmdBlitImage);
    LOAD_DEV(device, vkCmdResolveImage);
    LOAD_DEV(device, vkCreateQueryPool);
    LOAD_DEV(device, vkDestroyQueryPool);
    LOAD_DEV(device, vkCmdResetQueryPool);
    LOAD_DEV(device, vkCmdWriteTimestamp);
    LOAD_DEV(device, vkGetQueryPoolResults);

    if (!l->vkDestroyDevice || !l->vkGetDeviceQueue || !l->vkQueueSubmit || !l->vkAllocateMemory ||
        !l->vkCreateBuffer || !l->vkCreateImage || !l->vkCreateGraphicsPipelines || !l->vkCmdDraw) {
        set_err(err, err_cap, "Vulkan device missing core entry points");
        return 0;
    }
    return 1;
}

void jpt_vk_loader_close(jpt_vk_loader_t* l) {
    if (!l) return;
    if (l->via_sdl_vulkan) SDL_Vulkan_UnloadLibrary();
    if (l->shared_obj) SDL_UnloadObject((SDL_SharedObject*)l->shared_obj);
    memset(l, 0, sizeof *l);
}
