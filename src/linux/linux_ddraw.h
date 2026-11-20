/* ddraw.dll as guest compatibility: the DirectDraw objects are guest COM objects (a vtable of thunks, 16-byte faces in guest memory),
 * surfaces keep their pixels in guest memory so Lock hands the guest a real address, and the primary surface is shown through
 * linux_screen.h. The display is the virtual 16 bpp RGB565 one of the Windows host (src/host/ddraw): same caps, same modes, same
 * flip-chain and Lock semantics, with no Direct3D. */
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

#endif
