#include "vk_alloc.h"

#include <stdlib.h>
#include <string.h>

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1u) return value;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void free_range_list(jpt_vk_alloc_range_t* head) {
    while (head) {
        jpt_vk_alloc_range_t* next = head->next;
        free(head);
        head = next;
    }
}

void jpt_vk_allocator_init(jpt_vk_allocator_t* a, VkDevice device, const jpt_vk_loader_t* vk,
                           const VkPhysicalDeviceMemoryProperties* mem_props, VkDeviceSize granularity,
                           jpt_vk_lifetime_t* lifetime) {
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->device = device;
    a->vk = vk;
    if (mem_props) a->mem_props = *mem_props;
    a->buffer_image_granularity = granularity ? granularity : 1u;
    a->lifetime = lifetime;
}

static void destroy_block(jpt_vk_allocator_t* a, jpt_vk_alloc_block_t* b) {
    if (!b || b->memory == VK_NULL_HANDLE) return;
    if (b->mapped) {
        a->vk->vkUnmapMemory(a->device, b->memory);
        b->mapped = NULL;
    }
    a->vk->vkFreeMemory(a->device, b->memory, NULL);
    b->memory = VK_NULL_HANDLE;
    free_range_list(b->free_list);
    b->free_list = NULL;
    b->size = 0;
    b->live_allocs = 0;
}

void jpt_vk_allocator_shutdown(jpt_vk_allocator_t* a) {
    if (!a) return;
    for (uint32_t i = 0; i < a->block_count; i++) destroy_block(a, &a->blocks[i]);
    a->block_count = 0;
}

int jpt_vk_find_memory_type(const VkPhysicalDeviceMemoryProperties* props, uint32_t type_bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    if (!props) return -1;
    VkMemoryPropertyFlags ideal = required | preferred;
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & ideal) == ideal) return (int)i;
    }
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & required) == required) return (int)i;
    }
    return -1;
}

static int try_suballoc(jpt_vk_alloc_block_t* b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize* out_offset) {
    jpt_vk_alloc_range_t** prev = &b->free_list;
    jpt_vk_alloc_range_t* cur = b->free_list;
    while (cur) {
        VkDeviceSize aligned = align_up(cur->offset, align);
        VkDeviceSize pad = aligned - cur->offset;
        if (pad + size <= cur->size) {
            VkDeviceSize tail_off = aligned + size;
            VkDeviceSize tail_sz = (cur->offset + cur->size) - tail_off;
            if (pad > 0 && tail_sz > 0) {
                jpt_vk_alloc_range_t* tail = (jpt_vk_alloc_range_t*)malloc(sizeof *tail);
                if (!tail) return 0;
                tail->offset = tail_off;
                tail->size = tail_sz;
                tail->next = cur->next;
                cur->size = pad;
                cur->next = tail;
            } else if (pad > 0) {
                cur->size = pad;
            } else if (tail_sz > 0) {
                cur->offset = tail_off;
                cur->size = tail_sz;
            } else {
                *prev = cur->next;
                free(cur);
            }
            *out_offset = aligned;
            b->live_allocs++;
            return 1;
        }
        prev = &cur->next;
        cur = cur->next;
    }
    return 0;
}

static void insert_free_range(jpt_vk_alloc_block_t* b, VkDeviceSize offset, VkDeviceSize size) {
    jpt_vk_alloc_range_t* node = (jpt_vk_alloc_range_t*)malloc(sizeof *node);
    if (!node) return;
    node->offset = offset;
    node->size = size;
    node->next = NULL;

    jpt_vk_alloc_range_t** prev = &b->free_list;
    jpt_vk_alloc_range_t* cur = b->free_list;
    while (cur && cur->offset < offset) {
        prev = &cur->next;
        cur = cur->next;
    }
    node->next = cur;
    *prev = node;

    /* Coalesce adjacent free spans to avoid fragmentation across swapchain/texture rebuilds. */
    cur = b->free_list;
    while (cur && cur->next) {
        if (cur->offset + cur->size == cur->next->offset) {
            jpt_vk_alloc_range_t* dead = cur->next;
            cur->size += dead->size;
            cur->next = dead->next;
            free(dead);
        } else {
            cur = cur->next;
        }
    }
}

static int find_free_block_slot(jpt_vk_allocator_t* a) {
    for (uint32_t i = 0; i < a->block_count; i++)
        if (a->blocks[i].memory == VK_NULL_HANDLE) return (int)i;
    if (a->block_count < JPT_VK_ALLOC_MAX_BLOCKS) return (int)(a->block_count++);
    return -1;
}

VkResult jpt_vk_alloc_mem(jpt_vk_allocator_t* a, const VkMemoryRequirements* req, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, int map_host, jpt_vk_allocation_t* out) {
    if (!a || !req || !out) return VK_ERROR_INITIALIZATION_FAILED;
    memset(out, 0, sizeof *out);
    out->block_index = -1;

    if (map_host) required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    int mem_type = jpt_vk_find_memory_type(&a->mem_props, req->memoryTypeBits, required, preferred);
    if (mem_type < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkDeviceSize align = req->alignment ? req->alignment : 1u;
    if (align < a->buffer_image_granularity) align = a->buffer_image_granularity;
    VkDeviceSize size = req->size ? req->size : 1u;
    int dedicated = (size > JPT_VK_ALLOC_BLOCK_SIZE / 2u);

    if (!dedicated) {
        for (uint32_t i = 0; i < a->block_count; i++) {
            jpt_vk_alloc_block_t* b = &a->blocks[i];
            VkDeviceSize offset = 0;
            if (b->memory == VK_NULL_HANDLE || b->dedicated || b->memory_type != (uint32_t)mem_type) continue;
            if (map_host && !b->mapped) continue;
            if (try_suballoc(b, size, align, &offset)) {
                out->memory = b->memory;
                out->offset = offset;
                out->size = size;
                out->mapped = (map_host && b->mapped) ? ((uint8_t*)b->mapped + offset) : NULL;
                out->block_index = (int32_t)i;
                if (a->lifetime) {
                    a->lifetime->allocations++;
                    a->lifetime->allocated_bytes += (int64_t)size;
                }
                return VK_SUCCESS;
            }
        }
    }

    int slot = find_free_block_slot(a);
    if (slot < 0) return VK_ERROR_TOO_MANY_OBJECTS;

    VkDeviceSize block_size = dedicated ? size : JPT_VK_ALLOC_BLOCK_SIZE;
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = block_size;
    ai.memoryTypeIndex = (uint32_t)mem_type;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult vr = a->vk->vkAllocateMemory(a->device, &ai, NULL, &mem);
    if (vr != VK_SUCCESS) return vr;

    void* mapped = NULL;
    int host_vis = (a->mem_props.memoryTypes[mem_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (map_host && host_vis) {
        vr = a->vk->vkMapMemory(a->device, mem, 0, block_size, 0, &mapped);
        if (vr != VK_SUCCESS) {
            a->vk->vkFreeMemory(a->device, mem, NULL);
            return vr;
        }
    }

    jpt_vk_alloc_block_t* b = &a->blocks[slot];
    memset(b, 0, sizeof *b);
    b->memory = mem;
    b->size = block_size;
    b->memory_type = (uint32_t)mem_type;
    b->dedicated = dedicated;
    b->host_visible = host_vis;
    b->mapped = mapped;

    if (!dedicated) {
        insert_free_range(b, 0, block_size);
        VkDeviceSize offset = 0;
        if (!try_suballoc(b, size, align, &offset)) {
            destroy_block(a, b);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        out->offset = offset;
        out->mapped = (map_host && b->mapped) ? ((uint8_t*)b->mapped + offset) : NULL;
    } else {
        b->live_allocs = 1;
        out->offset = 0;
        out->mapped = (map_host && b->mapped) ? b->mapped : NULL;
    }
    out->memory = mem;
    out->size = size;
    out->block_index = slot;
    if (a->lifetime) {
        a->lifetime->allocations++;
        a->lifetime->allocated_bytes += (int64_t)size;
    }
    return VK_SUCCESS;
}

void jpt_vk_free_mem(jpt_vk_allocator_t* a, jpt_vk_allocation_t* alloc) {
    if (!a || !alloc || alloc->memory == VK_NULL_HANDLE) return;
    if (alloc->block_index >= 0 && (uint32_t)alloc->block_index < a->block_count) {
        jpt_vk_alloc_block_t* b = &a->blocks[alloc->block_index];
        if (b->live_allocs > 0) b->live_allocs--;
        if (b->dedicated || b->live_allocs == 0) {
            destroy_block(a, b);
        } else {
            insert_free_range(b, alloc->offset, alloc->size);
        }
    }
    if (a->lifetime) {
        a->lifetime->allocations--;
        a->lifetime->allocated_bytes -= (int64_t)alloc->size;
    }
    memset(alloc, 0, sizeof *alloc);
    alloc->block_index = -1;
}

VkResult jpt_vk_create_buffer(jpt_vk_allocator_t* a, VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, int map_host,
                              VkBuffer* out_buf, jpt_vk_allocation_t* out_alloc) {
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult vr;

    if (!a || !out_buf || !out_alloc) return VK_ERROR_INITIALIZATION_FAILED;
    *out_buf = VK_NULL_HANDLE;
    memset(out_alloc, 0, sizeof *out_alloc);
    out_alloc->block_index = -1;

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vr = a->vk->vkCreateBuffer(a->device, &bi, NULL, &buf);
    if (vr != VK_SUCCESS) return vr;
    a->vk->vkGetBufferMemoryRequirements(a->device, buf, &req);
    vr = jpt_vk_alloc_mem(a, &req, required, preferred, map_host, out_alloc);
    if (vr != VK_SUCCESS) {
        a->vk->vkDestroyBuffer(a->device, buf, NULL);
        return vr;
    }
    vr = a->vk->vkBindBufferMemory(a->device, buf, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        jpt_vk_free_mem(a, out_alloc);
        a->vk->vkDestroyBuffer(a->device, buf, NULL);
        return vr;
    }
    *out_buf = buf;
    if (a->lifetime) a->lifetime->buffers++;
    return VK_SUCCESS;
}

void jpt_vk_destroy_buffer(jpt_vk_allocator_t* a, VkBuffer* buf, jpt_vk_allocation_t* alloc) {
    if (!a) return;
    if (buf && *buf != VK_NULL_HANDLE) {
        a->vk->vkDestroyBuffer(a->device, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        if (a->lifetime) a->lifetime->buffers--;
    }
    if (alloc) jpt_vk_free_mem(a, alloc);
}

VkResult jpt_vk_create_image(jpt_vk_allocator_t* a, const VkImageCreateInfo* info, VkMemoryPropertyFlags required,
                             VkMemoryPropertyFlags preferred, VkImage* out_img, jpt_vk_allocation_t* out_alloc) {
    VkMemoryRequirements req;
    VkImage img = VK_NULL_HANDLE;
    VkResult vr;

    if (!a || !info || !out_img || !out_alloc) return VK_ERROR_INITIALIZATION_FAILED;
    *out_img = VK_NULL_HANDLE;
    memset(out_alloc, 0, sizeof *out_alloc);
    out_alloc->block_index = -1;

    vr = a->vk->vkCreateImage(a->device, info, NULL, &img);
    if (vr != VK_SUCCESS) return vr;
    a->vk->vkGetImageMemoryRequirements(a->device, img, &req);
    vr = jpt_vk_alloc_mem(a, &req, required, preferred, 0, out_alloc);
    if (vr != VK_SUCCESS) {
        a->vk->vkDestroyImage(a->device, img, NULL);
        return vr;
    }
    vr = a->vk->vkBindImageMemory(a->device, img, out_alloc->memory, out_alloc->offset);
    if (vr != VK_SUCCESS) {
        jpt_vk_free_mem(a, out_alloc);
        a->vk->vkDestroyImage(a->device, img, NULL);
        return vr;
    }
    *out_img = img;
    if (a->lifetime) a->lifetime->textures++;
    return VK_SUCCESS;
}

void jpt_vk_destroy_image(jpt_vk_allocator_t* a, VkImage* img, jpt_vk_allocation_t* alloc) {
    if (!a) return;
    if (img && *img != VK_NULL_HANDLE) {
        a->vk->vkDestroyImage(a->device, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (a->lifetime) a->lifetime->textures--;
    }
    if (alloc) jpt_vk_free_mem(a, alloc);
}
