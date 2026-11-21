#ifndef TRESPASSER_GUEST_UI_MODEL_H
#define TRESPASSER_GUEST_UI_MODEL_H

#include <stdint.h>

#include "host_policy.h"

/* Which window of the game is in front, read from its window stack through a 32-bit reader: the one model the Windows
 * host (guest memory is process memory) and the Linux host (guest memory is a window of the process) share. */
typedef struct {
    guest_ui_t ui;
    guest_wnd_t kind;
    uint32_t top, top_vtable, manager;
    int stopped;
    unsigned depth;
} guest_ui_view_t;

typedef int (*guest_read32_fn)(void* context, uint32_t va, uint32_t* out);

/* `gva` is the table of guest addresses of the executable the guest runs (jpt_gva, or jpt_gva_canonical for the retail
 * 1.0.116 image). 1 when the stack was read; 0 (view left as "none") for a stack that is not built yet. */
int guest_ui_model_probe(const uint32_t* gva, guest_read32_fn read32, void* context, guest_ui_view_t* out);
guest_wnd_t guest_ui_model_kind(const uint32_t* gva, uint32_t vtable);
const char* guest_ui_model_class_name(const uint32_t* gva, uint32_t vtable);

#endif
