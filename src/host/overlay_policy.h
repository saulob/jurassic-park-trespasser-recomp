/* The host's interfaces over gameplay, one at a time, without windows or the guest: which one is up, what a request
 * closes and opens, and the input that must not reach the game around them. */
#ifndef OVERLAY_POLICY_H
#define OVERLAY_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { OV_NONE, OV_CONTROLS, OV_OBJECTIVES, OV_CHEATS, OV_HINT } ov_kind_t;

const char* ov_name(ov_kind_t kind);

/* A request for kind (OV_HINT: the game's own Hint) while active is up. native: a window of the game is in front
 * (Hint, pause menu, dialog), so nothing of the host opens over it. */
typedef struct {
    ov_kind_t close; /* closed first; OV_NONE for nothing */
    ov_kind_t open;  /* then opened; OV_NONE for nothing (a toggle off, or refused) */
} ov_plan_t;

ov_plan_t ov_plan(ov_kind_t active, ov_kind_t request, int native);

/* Keys and buttons a host interface used: the game gets neither their repeats nor their release. */
typedef struct {
    uint8_t keys[32];
} ov_keys_t;

void ov_keys_take(ov_keys_t* k, unsigned vk);
/* Every key message: 1 when the game must not see it (and the key is let go on its release). */
int ov_keys_filter(ov_keys_t* k, unsigned vk, int down);

/* Gameplay commands (SInput state bits) held when gameplay was blocked or unblocked stay out of the game until they
 * are released: a held mouse button or LT does not raise the hand on its own, a held B does not crouch. */
typedef struct {
    int blocked;
    uint32_t masked;
} ov_commands_t;

/* Each CInput read: 1 when blocked changed (the pad's held buttons must then wait for their release too). */
int ov_commands_block(ov_commands_t* c, int blocked, uint32_t held_now);
/* Gameplay resumes after a window of the game (Hint, pause menu, dialog): what is still held waits as well. */
void ov_commands_resume(ov_commands_t* c, uint32_t held_now);
/* The state bits the game may see this read; hit bits follow the same mask. */
uint32_t ov_commands_pass(ov_commands_t* c, uint32_t held_now);

#ifdef __cplusplus
}
#endif

#endif
