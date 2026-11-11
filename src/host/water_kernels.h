#ifndef TRESPASSER_WATER_KERNELS_H
#define TRESPASSER_WATER_KERNELS_H

#include "recomp_types.h"

#define WATER_ROWS_VA 0x0060E3F0u
#define WATER_WAVE_STEP_VA 0x00638670u

/* Host equivalents of two lifted functions: everything the guest can see after them (memory, registers,
 * flags, the x87 model, the loop counter that paces guest threads) is what the lifted code leaves.
 * water_rows: one row of CEntityWater's alpha texture from the wave heights (0x0060E3F0).
 * water_wave_step: one CSimWaveVar2D integration step (0x00638670). */
void water_rows(void);
void water_wave_step(void);

/* The lifted bodies, which run the entries inside them (alias entries) these kernels do not have. */
extern recomp_func_t water_rows_lifted, water_wave_step_lifted;

#endif
