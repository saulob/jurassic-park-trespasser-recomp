#ifndef JPT_LINUX_D3D_INTERNAL_H
#define JPT_LINUX_D3D_INTERNAL_H

#include "ddraw/d3d_translate.h"
#include "linux_d3d.h"

#define LX_D3D_RENDER_STATES 256u
#define LX_D3D_STAGES 8u
#define LX_D3D_STAGE_STATES 32u
#define LX_VIEWPORT2_BYTES 44u

#define LX_D3D_OK 0u
#define LX_DDERR_INVALIDPARAMS 0x80070057u
#define LX_DDERR_UNSUPPORTED 0x80004001u
#define LX_DDERR_GENERIC 0x80004005u
#define LX_DDERR_OUTOFMEMORY 0x8007000Eu
#define LX_DDERR_SURFACEBUSY 0x887601AEu
#define LX_DDERR_ALREADYINITIALIZED 0x88760005u
#define LX_E_NOINTERFACE 0x80004002u
#define LX_CLASS_E_NOAGGREGATION 0x80040110u
#define LX_D3DERR_SCENE_IN_SCENE 0x887602F8u
#define LX_D3DERR_SCENE_NOT_IN_SCENE 0x887602F9u
#define LX_D3DERR_NOVIEWPORTS 0x88760304u
#define LX_D3DERR_VIEWPORTDATANOTSET 0x88760305u
#define LX_D3DERR_VIEWPORTHASNODEVICE 0x88760306u
#define LX_D3DERR_NOCURRENTVIEWPORT 0x88760307u

#define LX_D3DCLEAR_TARGET 1u
#define LX_D3DCLEAR_ZBUFFER 2u
#define LX_D3DCLEAR_STENCIL 4u

enum {
    LX_RS_TEXTUREHANDLE = 1,
    LX_RS_TEXTUREADDRESS = 3,
    LX_RS_TEXTUREPERSPECTIVE = 4,
    LX_RS_ZENABLE = 7,
    LX_RS_FILLMODE = 8,
    LX_RS_SHADEMODE = 9,
    LX_RS_ZWRITEENABLE = 14,
    LX_RS_ALPHATESTENABLE = 15,
    LX_RS_LASTPIXEL = 16,
    LX_RS_TEXTUREMAG = 17,
    LX_RS_TEXTUREMIN = 18,
    LX_RS_SRCBLEND = 19,
    LX_RS_DESTBLEND = 20,
    LX_RS_TEXTUREMAPBLEND = 21,
    LX_RS_CULLMODE = 22,
    LX_RS_ZFUNC = 23,
    LX_RS_ALPHAREF = 24,
    LX_RS_ALPHAFUNC = 25,
    LX_RS_DITHERENABLE = 26,
    LX_RS_ALPHABLENDENABLE = 27,
    LX_RS_FOGENABLE = 28,
    LX_RS_SPECULARENABLE = 29,
    LX_RS_SUBPIXEL = 31,
    LX_RS_SUBPIXELX = 32,
    LX_RS_STIPPLEDALPHA = 33,
    LX_RS_FOGCOLOR = 34,
    LX_RS_FOGTABLEMODE = 35,
    LX_RS_COLORKEYENABLE = 41,
    LX_RS_TEXTUREADDRESSU = 44,
    LX_RS_TEXTUREADDRESSV = 45,
    LX_RS_ZBIAS = 47,
    LX_RS_RANGEFOGENABLE = 48
};

enum {
    LX_TSS_COLOROP = 1,
    LX_TSS_COLORARG1 = 2,
    LX_TSS_COLORARG2 = 3,
    LX_TSS_ALPHAOP = 4,
    LX_TSS_ALPHAARG1 = 5,
    LX_TSS_ALPHAARG2 = 6,
    LX_TSS_TEXCOORDINDEX = 11,
    LX_TSS_ADDRESS = 12,
    LX_TSS_ADDRESSU = 13,
    LX_TSS_ADDRESSV = 14,
    LX_TSS_MAGFILTER = 16,
    LX_TSS_MINFILTER = 17,
    LX_TSS_MIPFILTER = 18,
    LX_TSS_MAXANISOTROPY = 21
};

struct lx_device_s;

typedef struct lx_viewport_s {
    uint32_t kind, guest;
    int32_t refs;
    struct lx_device_s* device;
    struct lx_viewport_s* next;
    int data_set;
    uint32_t data[LX_VIEWPORT2_BYTES / 4u]; /* D3DVIEWPORT2: size, x, y, width, height, clip x, y, width, height, min z, max z */
} lx_viewport_t;

typedef struct lx_device_s {
    uint32_t kind, guest;
    int32_t refs;
    int dd, target; /* linux_ddraw.c slots */
    int in_scene;
    lx_viewport_t *viewports, *current;
    uint32_t render[LX_D3D_RENDER_STATES];
    uint32_t stage[LX_D3D_STAGES][LX_D3D_STAGE_STATES];
    float transforms[3][16];
    int texture[LX_D3D_STAGES]; /* slots, -1 for none */
} lx_device_t;

int lx_target_attach(lx_device_t* dev);
void lx_target_detach(lx_device_t* dev);
void lx_target_begin_scene(lx_device_t* dev);
uint32_t lx_target_draw(lx_device_t* dev, uint32_t type, uint32_t fvf, uint32_t vertices_va, uint32_t count, const uint16_t* indices,
                        uint32_t index_count);
uint32_t lx_target_clear(int target_slot, uint32_t count, const int32_t* rects, uint32_t flags, uint32_t color, float z);
void lx_target_texture_loaded(int dst, int src);
void lx_target_reset(void);
lx_d3d_stats_t* lx_target_stats(void);

#endif
