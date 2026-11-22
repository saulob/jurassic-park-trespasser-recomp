#ifndef JPT_RENDERER_H
#define JPT_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPT_RENDERER_AUTO = 0,
    JPT_RENDERER_D3D11 = 1,
    JPT_RENDERER_VULKAN = 2,
    JPT_RENDERER_SOFTWARE = 3,
    JPT_RENDERER_COUNT = 4
} jpt_renderer_t;

typedef struct {
    jpt_renderer_t requested;
    jpt_renderer_t chosen;
    int fallback;
    char reason[192];
} jpt_renderer_decision_t;

const char* jpt_renderer_key(jpt_renderer_t renderer);
const char* jpt_renderer_label(jpt_renderer_t renderer);
int jpt_renderer_parse(const char* text, jpt_renderer_t* out);
int jpt_renderer_available_on_platform(jpt_renderer_t renderer, int is_windows);
int jpt_renderer_is_hardware(jpt_renderer_t renderer);

void jpt_renderer_resolve_policy(jpt_renderer_t requested, int is_windows, int d3d11_available, int vulkan_available,
                                 const char* vulkan_unavailable_reason, jpt_renderer_decision_t* out);

#define JPT_GPU_ID_MAX 64

void jpt_gpu_id_format(uint32_t vendor_id, uint32_t device_id, const uint8_t uuid[16], char* out, size_t cap);
int jpt_gpu_request_is_auto(const char* request);
int jpt_gpu_match_request(const char* request, const char* label, const char* name, uint32_t vendor_id,
                          uint32_t device_id, const uint8_t uuid[16]);

#ifdef __cplusplus
}
#endif

#endif
