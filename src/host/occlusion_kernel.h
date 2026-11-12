#ifndef TRESPASSER_OCCLUSION_KERNEL_H
#define TRESPASSER_OCCLUSION_KERNEL_H

#include "recomp_types.h"

#define OCCLUSION_TEST_VA 0x00630D90u

/* Host equivalent of the object occlusion test (0x00630D90): everything the guest can see afterwards (memory,
 * registers, flags, the x87 model, the loop counter that paces guest threads) is what the lifted code leaves. */
void occlusion_test(void);

/* The lifted body, which runs the entries inside it (alias entries) the kernel does not have. */
extern recomp_func_t occlusion_test_lifted;

#endif
