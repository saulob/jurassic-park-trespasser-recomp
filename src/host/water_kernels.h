#ifndef TRESPASSER_WATER_KERNELS_H
#define TRESPASSER_WATER_KERNELS_H

#include "guest_va.h"
#include "recomp_types.h"

#define WATER_ROWS_VA GVA(0x0060E3F0)
#define WATER_WAVE_STEP_VA GVA(0x00638670)

/* Host equivalents of two lifted functions: everything the guest can see after them (memory, registers,
 * flags, the x87 model, the loop counter that paces guest threads) is what the lifted code leaves.
 * water_rows: one row of CEntityWater's alpha texture from the wave heights (0x0060E3F0).
 * water_wave_step: one CSimWaveVar2D integration step (0x00638670). */
void water_rows(void);
void water_wave_step(void);

/* The lifted bodies, which run the entries inside them (alias entries) these kernels do not have. */
extern recomp_func_t water_rows_lifted, water_wave_step_lifted;
/* Each texel water_rows writes, with the wave height's differences to the next column and row (refraction). */
extern void (*water_rows_observer)(uintptr_t texel, double right, double below);

/* The wave model integrates in steps of its own maximum (CWaveData::sStepMax, CSimWave2D::SetMaxStep) whatever the
 * frame rate: the time owed (sRequireStep) is paid in whole steps and the rest carried, so the physics' disturbances,
 * which land between two steps, reach the water as they did at the frame times it was built for. steps < 0: nothing
 * usable to pace, the game integrates as it always did. */
#define WATER_CATCH_UP_STEPS 4
typedef struct {
    int steps;
    float step, carry;
} water_cadence_t;
water_cadence_t water_cadence(float required, float step_max);

#endif
