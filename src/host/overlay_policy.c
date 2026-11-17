#include "overlay_policy.h"

#include <string.h>

const char* ov_name(ov_kind_t kind) {
    static const char* const names[] = { "none", "controls", "objectives", "cheats", "hint" };
    return kind >= OV_NONE && kind <= OV_HINT ? names[kind] : "?";
}

ov_plan_t ov_plan(ov_kind_t active, ov_kind_t request, int native) {
    ov_plan_t p = { OV_NONE, OV_NONE };
    if (request == OV_NONE) {
        p.close = active;
        return p;
    }
    if (native) return p;
    p.close = active;
    if (request != active) p.open = request;
    return p;
}

void ov_keys_take(ov_keys_t* k, unsigned vk) {
    vk &= 0xFFu;
    k->keys[vk >> 3] |= (uint8_t)(1u << (vk & 7));
}

int ov_keys_filter(ov_keys_t* k, unsigned vk, int down) {
    int taken;
    vk &= 0xFFu;
    taken = (k->keys[vk >> 3] >> (vk & 7)) & 1;
    if (taken && !down) k->keys[vk >> 3] &= (uint8_t)~(1u << (vk & 7));
    return taken;
}

int ov_commands_block(ov_commands_t* c, int blocked, uint32_t held_now) {
    blocked = blocked != 0;
    if (blocked == c->blocked) return 0;
    c->blocked = blocked;
    c->masked = held_now;
    return 1;
}

void ov_commands_resume(ov_commands_t* c, uint32_t held_now) {
    if (!c->blocked) c->masked = held_now;
}

uint32_t ov_commands_pass(ov_commands_t* c, uint32_t held_now) {
    if (c->blocked) {
        c->masked |= held_now;
        return 0;
    }
    c->masked &= held_now;
    return held_now & ~c->masked;
}
