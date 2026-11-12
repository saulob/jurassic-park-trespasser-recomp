#ifndef TRESPASSER_POLYGON_SUBMIT_H
#define TRESPASSER_POLYGON_SUBMIT_H

#include "recomp_types.h"

#define POLYGON_SUBMIT_VA 0x00559240u

/* Host equivalent of the per-polygon Direct3D submission (0x00559240, thiscall, one argument): the texture address
 * mode, a D3DTLVERTEX fan built on the guest stack (fog, lit colour, specular), three render states and DrawPrimitive,
 * each call made through the device's vtable as the lifted code makes it. */
void polygon_submit(void);

/* The lifted body: alias entries and input the host path does not take. */
extern recomp_func_t polygon_submit_lifted;

#endif
