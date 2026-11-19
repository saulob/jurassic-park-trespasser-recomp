/* Disable Hammond Narration (key disable_hammond_narration): John Hammond's voice-over is not started, so neither his
 * voice nor his captions appear. Anne's lines, effects, music and ambience are never affected. */
#ifndef NARRATION_POLICY_H
#define NARRATION_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Takes effect from the next voice-over; one already playing is left to finish. */
void narration_set_disabled(int disabled);
int narration_disabled(void);
/* 1 when a voice-over of this sample ID must not be started now. */
int narration_blocked(uint32_t sample_id);

#ifdef __cplusplus
}
#endif

#endif
