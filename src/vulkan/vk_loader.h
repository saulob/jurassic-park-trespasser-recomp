#ifndef JPT_VK_LOADER_H
#define JPT_VK_LOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifndef VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#endif
typedef struct jpt_vk_win32_surface_ci_s {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* hinstance;
    void* hwnd;
} jpt_vk_win32_surface_ci_t;
typedef VkResult(VKAPI_PTR* PFN_jpt_vkCreateWin32SurfaceKHR)(VkInstance instance,
                                                             const jpt_vk_win32_surface_ci_t* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkSurfaceKHR* pSurface);
#endif

typedef struct {
    int loaded;
    int via_sdl_vulkan;
    void* shared_obj;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

    /* Global entry points */
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
    PFN_vkCreateInstance vkCreateInstance;

    /* Instance entry points */
    PFN_vkDestroyInstance vkDestroyInstance;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties2 vkGetPhysicalDeviceMemoryProperties2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
    PFN_vkCreateDevice vkCreateDevice;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

    /* Surface / WSI instance entry points */
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
#ifdef _WIN32
    PFN_jpt_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
#endif

    /* Optional debug utils */
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT;

    /* Device entry points */
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceQueue vkGetDeviceQueue;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
    PFN_vkQueueSubmit vkQueueSubmit;
    PFN_vkQueueWaitIdle vkQueueWaitIdle;

    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR vkQueuePresentKHR;

    PFN_vkAllocateMemory vkAllocateMemory;
    PFN_vkFreeMemory vkFreeMemory;
    PFN_vkMapMemory vkMapMemory;
    PFN_vkUnmapMemory vkUnmapMemory;
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
    PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;

    PFN_vkCreateBuffer vkCreateBuffer;
    PFN_vkDestroyBuffer vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
    PFN_vkBindBufferMemory vkBindBufferMemory;

    PFN_vkCreateImage vkCreateImage;
    PFN_vkDestroyImage vkDestroyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
    PFN_vkBindImageMemory vkBindImageMemory;
    PFN_vkCreateImageView vkCreateImageView;
    PFN_vkDestroyImageView vkDestroyImageView;

    PFN_vkCreateSampler vkCreateSampler;
    PFN_vkDestroySampler vkDestroySampler;

    PFN_vkCreateShaderModule vkCreateShaderModule;
    PFN_vkDestroyShaderModule vkDestroyShaderModule;

    PFN_vkCreateRenderPass vkCreateRenderPass;
    PFN_vkDestroyRenderPass vkDestroyRenderPass;
    PFN_vkCreateFramebuffer vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer;

    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
    PFN_vkResetDescriptorPool vkResetDescriptorPool;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;

    PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
    PFN_vkCreatePipelineCache vkCreatePipelineCache;
    PFN_vkDestroyPipelineCache vkDestroyPipelineCache;
    PFN_vkGetPipelineCacheData vkGetPipelineCacheData;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline vkDestroyPipeline;

    PFN_vkCreateCommandPool vkCreateCommandPool;
    PFN_vkDestroyCommandPool vkDestroyCommandPool;
    PFN_vkResetCommandPool vkResetCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer vkEndCommandBuffer;
    PFN_vkResetCommandBuffer vkResetCommandBuffer;

    PFN_vkCreateFence vkCreateFence;
    PFN_vkDestroyFence vkDestroyFence;
    PFN_vkWaitForFences vkWaitForFences;
    PFN_vkResetFences vkResetFences;
    PFN_vkCreateSemaphore vkCreateSemaphore;
    PFN_vkDestroySemaphore vkDestroySemaphore;

    /* Command buffer recording */
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline vkCmdBindPipeline;
    PFN_vkCmdSetViewport vkCmdSetViewport;
    PFN_vkCmdSetScissor vkCmdSetScissor;
    PFN_vkCmdSetDepthBias vkCmdSetDepthBias;
    PFN_vkCmdSetStencilReference vkCmdSetStencilReference;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
    PFN_vkCmdPushConstants vkCmdPushConstants;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer;
    PFN_vkCmdDraw vkCmdDraw;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed;
    PFN_vkCmdClearAttachments vkCmdClearAttachments;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
    PFN_vkCmdCopyImage vkCmdCopyImage;
    PFN_vkCmdBlitImage vkCmdBlitImage;
    PFN_vkCmdResolveImage vkCmdResolveImage;

    /* Timing queries (optional telemetry) */
    PFN_vkCreateQueryPool vkCreateQueryPool;
    PFN_vkDestroyQueryPool vkDestroyQueryPool;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults;
} jpt_vk_loader_t;

int jpt_vk_loader_open(jpt_vk_loader_t* l, int use_sdl_video, char* err, size_t err_cap);
/* Uses an entry point that is already loaded; closes `l` on failure. */
int jpt_vk_loader_bind(jpt_vk_loader_t* l, PFN_vkGetInstanceProcAddr get, char* err, size_t err_cap);
int jpt_vk_loader_load_instance(jpt_vk_loader_t* l, VkInstance instance, char* err, size_t err_cap);
int jpt_vk_loader_load_device(jpt_vk_loader_t* l, VkDevice device, char* err, size_t err_cap);
void jpt_vk_loader_close(jpt_vk_loader_t* l);

#ifdef __cplusplus
}
#endif

#endif
