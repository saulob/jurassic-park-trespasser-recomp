/* The Linux implementation of guest_abi.h: guest memory reads and writes, guest calls and the host's scratch memory. */
#ifndef JPT_LINUX_HOST_H
#define JPT_LINUX_HOST_H

/* Forgets the scratch memory, which goes with the guest window (call after gm_shutdown). */
void lx_host_reset(void);

#endif
