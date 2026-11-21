/*
 * Official Trespasser v1.1 build 117 level fixes, on both hosts. The game reads a level's scene (.scn) and world (.grf,
 * expanded by LZRead) through the LZ file calls; when a retail one is opened, its bytes are fixed in memory
 * (patch117_policy.c) and served in place of the file's, so every load starts from the retail file and the fixes are
 * applied exactly once. Saved games are never changed. CPartition::bAddChild is the code fix of the same patch.
 */
#include "guest_abi.h"
#include "guest_features.h"

#include <stdlib.h>
#include <string.h>

#include "jpt_sync.h"
#include "patch117_policy.h"

#define ADD_CHILD_VA GVA(0x00475F60) /* CPartition::bAddChild(child, test_priority), thiscall, every partition class */
#define LOG_REPEAT_MS 5000u

static int s_on;
static const patch117_level_t* s_last_logged;
static uint32_t s_last_logged_tick;
static recomp_func_t s_add_child_lifted;
static int s_add_child_depth, s_add_child_flag;

void patch117_configure(int on) {
    s_on = on != 0;
    host_log("patch117", "official Patch 1.1 fixes: %s", s_on ? "on" : "off");
}

/* The lifted body calls the children through the vtable, which reaches this entry again. */
static void add_child(void) {
    int caller = s_add_child_flag, flag;
    if (g_alias_entry) {
        s_add_child_lifted();
        return;
    }
    flag = patch117_add_child_flag(s_on, s_add_child_depth > 0, caller, (int)(GUEST_ARG(1) & 0xFFu));
    GUEST_ARG(1) = (GUEST_ARG(1) & ~0xFFu) | (uint32_t)flag;
    s_add_child_flag = flag;
    s_add_child_depth++;
    s_add_child_lifted();
    s_add_child_depth--;
    s_add_child_flag = caller;
}

void patch117_install(void) {
    if (!JPT_P117_ADD_CHILD) return;
    if (!guest_override_install(ADD_CHILD_VA, add_child, &s_add_child_lifted))
        host_log("patch117", "warning: CPartition::bAddChild could not be hooked: its official fix is off");
}

int patch117_file_wanted(const char* path) {
    return s_on && (patch117_level_named(path) || patch117_world_named(path));
}

static uint8_t* fixed_scene(const char* path, const uint8_t* raw, size_t raw_size) {
    const patch117_level_t* level = patch117_identify(path, raw, raw_size);
    uint8_t* data;
    int fixes;
    if (!level) {
        host_log("patch117", "%s is not the retail level file: official level fixes skipped", path);
        return NULL;
    }
    data = (uint8_t*)malloc(raw_size);
    if (!data) return NULL;
    memcpy(data, raw, raw_size);
    fixes = patch117_fix_scene(level, data, raw_size);
    if (fixes <= 0) {
        host_log("patch117", "%s: official level fixes did not match the retail data, skipped", level->id);
        free(data);
        return NULL;
    }
    /* One load opens the scene for its header and again for its contents. */
    if (level != s_last_logged || jpt_time_ms() - s_last_logged_tick > LOG_REPEAT_MS)
        host_log("patch117", "applied %d official level fixes to %s", fixes, level->id);
    s_last_logged = level;
    s_last_logged_tick = jpt_time_ms();
    return data;
}

static uint8_t* fixed_world(const char* path, const uint8_t* raw, size_t raw_size, patch117_expand_fn expand, void* context,
                            size_t* size) {
    const patch117_world_t* world = patch117_world_identify(path, raw, raw_size);
    uint8_t* image;
    int fixes;
    if (!world) {
        host_log("patch117", "%s is not the retail world file: official object placements skipped", path);
        return NULL;
    }
    image = expand(context, world->expanded_size);
    fixes = image ? patch117_fix_world(world, image, world->expanded_size) : -1;
    if (fixes <= 0) {
        host_log("patch117", "%s: official object placements did not match the retail data, skipped", world->id);
        free(image);
        return NULL;
    }
    host_log("patch117", "applied %d official object placements to %s", fixes, world->id);
    *size = world->expanded_size;
    return image;
}

uint8_t* patch117_level_bytes(const char* path, const uint8_t* raw, size_t raw_size, patch117_expand_fn expand, void* context,
                              size_t* size) {
    if (!s_on || !raw) return NULL;
    if (patch117_level_named(path)) {
        uint8_t* data = fixed_scene(path, raw, raw_size);
        *size = data ? raw_size : 0;
        return data;
    }
    if (patch117_world_named(path)) return fixed_world(path, raw, raw_size, expand, context, size);
    return NULL;
}
