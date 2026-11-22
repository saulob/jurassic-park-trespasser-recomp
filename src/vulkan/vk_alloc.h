#ifndef JPT_VK_ALLOC_H
#define JPT_VK_ALLOC_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>

#include "vk_loader.h"
#include "vk_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_VK_ALLOC_BLOCK_SIZE (16u * 1024u * 1024u)
#define JPT_VK_ALLOC_MAX_BLOCKS 128

typedef struct jpt_vk_alloc_range_s {
    VkDeviceSize offset;
    VkDeviceSize size;
    struct jpt_vk_alloc_range_s* next;
} jpt_vk_alloc_range_t;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize size;
    uint32_t memory_type;
    int dedicated;
    int host_visible;
    void* mapped;
    jpt_vk_alloc_range_t* free_list;
    uint32_t live_allocs;
} jpt_vk_alloc_block_t;

typedef struct {
    VkDevice device;
    const jpt_vk_loader_t* vk;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDeviceSize buffer_image_granularity;
    jpt_vk_lifetime_t* lifetime;
    jpt_vk_alloc_block_t blocks[JPT_VK_ALLOC_MAX_BLOCKS];
    uint32_t block_count;
} jpt_vk_allocator_t;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void* mapped;
    int32_t block_index;
} jpt_vk_allocation_t;

void jpt_vk_allocator_init(jpt_vk_allocator_t* a, VkDevice device, const jpt_vk_loader_t* vk,
                           const VkPhysicalDeviceMemoryProperties* mem_props, VkDeviceSize granularity,
                           jpt_vk_lifetime_t* lifetime);
void jpt_vk_allocator_shutdown(jpt_vk_allocator_t* a);

int jpt_vk_find_memory_type(const VkPhysicalDeviceMemoryProperties* props, uint32_t type_bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred);

VkResult jpt_vk_alloc_mem(jpt_vk_allocator_t* a, const VkMemoryRequirements* req, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, int map_host, jpt_vk_allocation_t* out);
void jpt_vk_free_mem(jpt_vk_allocator_t* a, jpt_vk_allocation_t* alloc);

VkResult jpt_vk_create_buffer(jpt_vk_allocator_t* a, VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, int map_host,
                              VkBuffer* out_buf, jpt_vk_allocation_t* out_alloc);
void jpt_vk_destroy_buffer(jpt_vk_allocator_t* a, VkBuffer* buf, jpt_vk_allocation_t* alloc);

VkResult jpt_vk_create_image(jpt_vk_allocator_t* a, const VkImageCreateInfo* info, VkMemoryPropertyFlags required,
                             VkMemoryPropertyFlags preferred, VkImage* out_img, jpt_vk_allocation_t* out_alloc);
void jpt_vk_destroy_image(jpt_vk_allocator_t* a, VkImage* img, jpt_vk_allocation_t* alloc);

#ifdef __cplusplus
}
#endif

#endif
