#include "narration_policy.h"

#include "jpt_hammond.h"

static volatile int s_disabled;

void narration_set_disabled(int disabled) { s_disabled = disabled != 0; }

int narration_disabled(void) { return s_disabled; }

int narration_blocked(uint32_t sample_id) { return s_disabled && jpt_hammond_sample(sample_id); }
