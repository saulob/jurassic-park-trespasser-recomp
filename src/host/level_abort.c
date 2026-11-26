#include "level_abort.h"

#include "guest_abi.h"
#include "jpt_custom_level.h"
#include "jpt_level_gate.h"

static void end_load(void) {
    host_log("custom-level", "CONTROLLED_LOAD_FAILURE: %s of %s", jpt_custom_level_current_scene(), jpt_custom_level_package());
    host_stop_level(jpt_gate_message(JPT_GATE_UNSAFE));
}

void level_limit_exceeded(const char* what, uint32_t at, uint32_t used, uint32_t asked, uint32_t capacity) {
    host_log("custom-level", "ENGINE_LIMIT_EXCEEDED: %s at 0x%08X holds %u bytes, %u are in use and %u more were asked for; nothing of them was written", what, at, capacity, used, asked);
    end_load();
}

void level_allocation_failed(const char* what, uint32_t at, uint32_t bytes) {
    host_log("custom-level", "ALLOCATION_FAILED: %s, %u bytes at 0x%08X", what, bytes, at);
    end_load();
}
