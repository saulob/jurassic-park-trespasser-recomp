#include "linux_guest_ui.h"

#include "active_profile.h"
#include "guest_memory.h"

static lx_guest_ui_fn s_probe;

static int read32(void* context, uint32_t va, uint32_t* out) {
    const uint32_t* p = (const uint32_t*)gm_ptr_checked(va, 4, 0);
    (void)context;
    if (!p) return 0;
    *out = *p;
    return 1;
}

void lx_guest_ui_set_probe(lx_guest_ui_fn fn) { s_probe = fn; }

int lx_guest_ui_probe(guest_ui_view_t* out) {
    if (s_probe) return s_probe(out);
    return guest_ui_model_probe(jpt_gva_table(), read32, NULL, out);
}
