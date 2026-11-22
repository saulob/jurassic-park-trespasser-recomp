#ifndef JPT_VK_WIN_ACCOUNT_H
#define JPT_VK_WIN_ACCOUNT_H

#include <stddef.h>

/* Where the guest thread's time goes between two presented frames. The Vulkan backend times its own entry points with
 * nested, exclusive scopes (a scope's time excludes the scopes opened inside it); everything outside them is the guest's
 * own code and the host hooks it runs. A window's wall time splits into those categories and the guest residual, and
 * separately into thread CPU time and blocked time, of which the renderer's own waits are the attributed part. */

typedef enum {
    VKA_DRAW,
    VKA_TEXTURE,
    VKA_SYNC,
    VKA_CPU_ACCESS,
    VKA_READBACK,
    VKA_LAYERS,
    VKA_FRAME_BEGIN,
    VKA_FLIP,
    VKA_VIDEO,
    VKA_OTHER,
    VKA_HOST_OVERLAYS,
    VKA_SUBTITLES,
    VKA_COUNT
} vka_cat_t;

#define VKA_MAX_DEPTH 12

typedef struct {
    double ms[VKA_COUNT];
    double max_ms[VKA_COUNT];
    unsigned long long calls[VKA_COUNT];
    int depth;
    unsigned overflow;
    unsigned unbalanced;
    struct {
        int cat;
        double start;
        double child;
    } stack[VKA_MAX_DEPTH];
} vka_t;

typedef struct {
    double frame_ms;       /* wall time per frame */
    double backend_ms;     /* the timed backend scopes */
    double guest_ms;       /* everything else on the thread: the game's code and the host hooks it runs */
    double cpu_ms;         /* the thread's CPU time */
    double blocked_ms;     /* wall time the thread was not running */
    double waits_ms;       /* of which waits the renderer timed itself (frame sync, one-shot submits, acquire, present) */
    double unaccounted_ms; /* blocked time no timed wait explains */
    double unaccounted_pct;
} vka_split_t;

const char* vka_name(vka_cat_t cat);
void vka_reset(vka_t* a);
void vka_enter(vka_t* a, vka_cat_t cat, double now_ms);
void vka_leave(vka_t* a, double now_ms);
double vka_total_ms(const vka_t* a);
/* a - base per category; no scope may be open in either. */
void vka_delta(const vka_t* now, const vka_t* base, vka_t* out);
void vka_split(double wall_ms, double cpu_ms, double backend_ms, double waits_ms, unsigned long long frames,
               vka_split_t* out);

#endif
