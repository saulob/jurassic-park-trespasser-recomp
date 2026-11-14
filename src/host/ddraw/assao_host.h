/* Intel ASSAO (third_party/assao) behind a small interface: the renderer hands it the scene's depth and stencil and a
 * render target to darken; nothing else of the effect is visible outside assao_host.cpp. */
#ifndef ASSAO_HOST_H
#define ASSAO_HOST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

typedef struct {
    struct ID3D11ShaderResourceView* depth;   /* R24_UNORM_X8_TYPELESS view, multisampled or not */
    struct ID3D11ShaderResourceView* stencil; /* X24_TYPELESS_G8_UINT view of the same buffer: 0 where AO applies */
    unsigned width, height, samples;
    float tan_half_x, tan_half_y;
} assao_scene_t;

/* hlsl: ASSAO.hlsl's text. Needs feature level 11.0; 0 (logged) when the effect cannot be created. */
int assao_init(struct ID3D11Device* device, const char* hlsl, size_t hlsl_size);
int assao_ready(void);
void assao_set_quality(int level); /* JPT_AO_* */
int assao_quality(void);
int assao_resize(unsigned width, unsigned height);
/* Multiplies out, the scene's size, by the occlusion of the pixels the stencil leaves open, and leaves out bound as the
 * only render target. 0 when nothing was drawn. */
int assao_render(struct ID3D11DeviceContext* context, const assao_scene_t* scene, struct ID3D11RenderTargetView* out);
void assao_shutdown(void);
unsigned long long assao_frames(void);

/* ASSAO.hlsl as compiled into the executable (assao_shader.cpp). */
const char* assao_embedded_shader(size_t* size);

#ifdef __cplusplus
}
#endif

#endif
