/* Quick Save (F7) and Quick Load (F8) on Linux: the same decisions as the Windows host (ce_policy.c: where it works, a dead player,
 * a load under way, no save yet) over the game's own save writer and loader, called through the call spine on SAVEGAME.QCK in the
 * game's saves folder. The key only records the request; the game is called at the next presented frame, never inside a message. */
#ifndef JPT_LINUX_QUICK_H
#define JPT_LINUX_QUICK_H

#include <stdint.h>

typedef struct {
    int (*read32)(uint32_t va, uint32_t* out);
    uint32_t (*call)(uint32_t va, int convention, int argc, const uint32_t* args, uint32_t this_ecx);
    int (*put_text)(uint32_t va, const char* text);
    int (*get_text)(uint32_t va, char* out, unsigned capacity);
    int (*zero)(uint32_t va, unsigned bytes);
    int (*file_exists)(const char* guest_path);
    uint32_t scratch; /* a guest block of at least LX_QUICK_SCRATCH bytes */
    void (*notice)(const char* text);
} lx_quick_ops_t;

#define LX_QUICK_SCRATCH 0x400u

void lx_quick_reset(void);
void lx_quick_request(int save);
/* 1 while a request waits for the next frame. */
int lx_quick_pending(void);
/* At a frame boundary: runs the waiting request, if any, on the game. */
void lx_quick_pump(const lx_quick_ops_t* ops);
/* The operations over the real guest; NULL while the guest memory is not up. */
const lx_quick_ops_t* lx_quick_guest_ops(void);

#endif
