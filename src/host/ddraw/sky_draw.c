/* The layers of a custom sky on the guest's device: each is a draw of its own with the states it needs, in place of the engine's. */
#include "sky_draw.h"

#include <stdlib.h>
#include <string.h>

#include "d3d_translate.h"
#include "ddraw_compat.h"
#include "renderer_backend.h"

static dd_surface_t* surface_of(const jpt_picture_t* picture) {
    dd_surface_t* s = (dd_surface_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->refs = 1;
    s->caps = DDSCAPS_TEXTURE;
    s->width = picture->width;
    s->height = picture->height;
    s->pitch = (LONG)(picture->width * 4u);
    s->format.dwSize = sizeof s->format;
    s->format.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    s->format.dwRGBBitCount = 32;
    s->format.dwRBitMask = 0x00FF0000u;
    s->format.dwGBitMask = 0x0000FF00u;
    s->format.dwBBitMask = 0x000000FFu;
    s->format.dwRGBAlphaBitMask = 0xFF000000u;
    s->bits = (BYTE*)malloc((size_t)s->pitch * s->height);
    if (!s->bits) {
        free(s);
        return NULL;
    }
    memcpy(s->bits, picture->bgra, (size_t)s->pitch * s->height);
    return s;
}

void sky_draw_free(void* gpu) {
    dd_surface_t* s = (dd_surface_t*)gpu;
    d3dr_surface_freed(s);
    free(s->bits);
    free(s);
}

static void settle(d3d_device_t* dev) { d3dr_state_will_change(dev, D3DR_CHANGE_RENDER, D3DRENDERSTATE_ZENABLE, 0); }

static void states(d3d_device_t* dev, const jpt_sky_pass_t* pass) {
    const int blended = pass->blend != JPT_SKY_OPAQUE;
    DWORD* r = dev->render;
    r[D3DRENDERSTATE_ZENABLE] = D3DZB_FALSE;
    r[D3DRENDERSTATE_ZWRITEENABLE] = FALSE;
    r[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    r[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    r[D3DRENDERSTATE_CULLMODE] = D3DCULL_NONE;
    r[D3DRENDERSTATE_ALPHABLENDENABLE] = (DWORD)blended;
    r[D3DRENDERSTATE_SRCBLEND] = blended ? D3DBLEND_SRCALPHA : D3DBLEND_ONE;
    r[D3DRENDERSTATE_DESTBLEND] = !blended ? D3DBLEND_ZERO : pass->blend == JPT_SKY_BURN ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA;
    r[D3DRENDERSTATE_ALPHATESTENABLE] = (DWORD)blended;
    r[D3DRENDERSTATE_ALPHAREF] = 0;
    r[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_GREATER;
    r[D3DRENDERSTATE_TEXTUREMAPBLEND] = pass->twice ? D3DT_TBLEND_MODULATEALPHA_2X : D3DTBLEND_MODULATEALPHA;
    r[D3DRENDERSTATE_FOGENABLE] = (DWORD)pass->fog;
    r[D3DRENDERSTATE_SPECULARENABLE] = FALSE;
    r[D3DRENDERSTATE_COLORKEYENABLE] = FALSE;
    dev->stage[0][D3DTSS_ADDRESSU] = pass->wrap_u ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
    dev->stage[0][D3DTSS_ADDRESSV] = pass->wrap_v ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
}

static void emit(void* user, const sky_draw_t* draw) {
    d3d_device_t* dev = (d3d_device_t*)user;
    if (draw->picture && !*draw->gpu) *draw->gpu = surface_of(draw->picture);
    if (draw->picture && !*draw->gpu) return;
    states(dev, draw->pass);
    dev->texture = draw->picture ? (dd_surface_t*)*draw->gpu : NULL;
    d3dr_draw(dev, D3DPT_TRIANGLELIST, SKY_LAYERS_FVF, draw->vertices, (DWORD)draw->count, NULL, 0);
    settle(dev);
}

void sky_draw_layers(d3d_device_t* dev, const void* vertices, DWORD count) {
    DWORD render[D3D_RENDER_STATES], address_u = dev->stage[0][D3DTSS_ADDRESSU], address_v = dev->stage[0][D3DTSS_ADDRESSV];
    dd_surface_t* texture = dev->texture;
    float rect[4];
    if (!sky_layers_rect(vertices, count, rect)) return;
    memcpy(render, dev->render, sizeof render);
    settle(dev);
    sky_layers_emit(rect, emit, dev);
    memcpy(dev->render, render, sizeof render);
    dev->stage[0][D3DTSS_ADDRESSU] = address_u;
    dev->stage[0][D3DTSS_ADDRESSV] = address_v;
    dev->texture = texture;
}
