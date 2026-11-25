/* ddraw.dll as guest compatibility: the DirectDraw objects are guest COM objects (a vtable of thunks, 16-byte faces in guest memory),
 * surfaces keep their pixels in guest memory so Lock hands the guest a real address, and the primary surface is shown through
 * linux_screen.h. The display is the virtual 16 bpp RGB565 one of the Windows host (src/host/ddraw): same caps, same modes, same
 * flip-chain and Lock semantics. Direct3D 6 (linux_d3d.h) is offered when a GPU renderer is available. */
#ifndef JPT_LINUX_DDRAW_H
#define JPT_LINUX_DDRAW_H

#include <stddef.h>
#include <stdint.h>

#include "linux_hle.h"

const hle_entry_t* hle_ddraw_table(size_t* count);
void hle_ddraw_init(void);
void hle_ddraw_shutdown(void);

unsigned lx_ddraw_device_count(void);
unsigned lx_ddraw_surface_count(void);
unsigned lx_ddraw_present_count(void);
/* Times the guest restored the display mode (it does so when its window is deactivated and again when it is activated). */
unsigned lx_ddraw_restore_count(void);

/* What linux_d3d.c sees of the objects: slots, reference counts and faces. */
#define LX_DDRAW_OBJECTS 8192
#define LX_DDRAW_FACE_TEXTURE2 5 /* a surface's IDirect3DTexture2; faces 1 to 4 are its surface interfaces */

typedef struct {
    uint32_t width, height, caps, bytes; /* bytes per pixel */
    int32_t pitch;
    uint8_t* bits; /* host memory of the guest's pixels, NULL for none */
    uint32_t pf[8];
    const uint32_t* palette; /* PALETTEENTRY values, NULL for none */
    int has_src_key, locked, busy, zbuffer, root; /* root: the front buffer's slot of a back buffer, -1 for none */
    uint32_t src_key[2], lock_flags;
    int32_t lock_rect[4];
} lx_surface_view_t;

int lx_ddraw_surface_from(uint32_t iface);
int lx_ddraw_texture_from(uint32_t iface);
int lx_ddraw_surface_view(int slot, lx_surface_view_t* out);
uint32_t lx_ddraw_surface_face(int slot, int face);
uint32_t lx_ddraw_surface_addref(int slot);
uint32_t lx_ddraw_surface_release(int slot);
uint32_t lx_ddraw_surface_query(int slot, uint32_t iid, uint32_t out);
void lx_ddraw_surface_set_src_key(int slot, const uint32_t key[2]);
/* The DirectDraw object an IDirect3D3 is a face of. */
int lx_ddraw_device_from(uint32_t direct3d);
uint32_t lx_ddraw_direct3d_face(int dd);
uint32_t lx_ddraw_device_addref(int dd);
uint32_t lx_ddraw_device_release(int dd);
uint32_t lx_ddraw_device_query(int dd, uint32_t iid, uint32_t out);

#endif
