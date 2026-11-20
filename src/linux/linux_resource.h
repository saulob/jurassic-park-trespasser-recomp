/* Resources of a PE image mapped in the guest window: found by walking the image's resource directory in guest memory. */
#ifndef JPT_LINUX_RESOURCE_H
#define JPT_LINUX_RESOURCE_H

#include <stdint.h>

/* type and name are a resource id (at most 0xFFFF) or the guest address of a name; "#123" names mean the id. The first
 * language is used. Returns 1 and the guest address and size of the data. */
int lx_resource_find(uint32_t image_base, uint32_t type, uint32_t name, uint32_t* data_va, uint32_t* size);

#endif
