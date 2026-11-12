#ifndef TRESPASSER_OCCLUSION_KERNEL_H
#define TRESPASSER_OCCLUSION_KERNEL_H

#include "recomp_types.h"

#define OCCLUSION_TEST_VA 0x00630D90u

/* Host equivalent of the object occlusion test (0x00630D90): everything the guest can see afterwards (memory,
 * registers, flags, the x87 model, the loop counter that paces guest threads) is what the lifted code leaves. */
void occlusion_test(void);

/* The lifted body, which runs the entries inside it (alias entries) the kernel does not have. */
extern recomp_func_t occlusion_test_lifted;

/* DEBUG_SAULO: calls since the last read and, on every eighth engine frame, calls repeating an earlier call of the
 * same frame (same caller, occluder set, points and epsilon). */
void occlusion_take_counts(unsigned long long* calls, unsigned long long* census_calls, unsigned long long* repeats);

#endif
