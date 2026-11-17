/*
 * Official Trespasser v1.1 build 117 level fixes. The game reads a level's scene (.scn) and world (.grf, expanded by
 * LZRead) through LZOpenFileA/LZRead/LZSeek; when a retail one is opened, its bytes are read once, fixed in memory
 * (patch117_policy.c) and served to that handle, so every load starts from the retail file and the fixes are applied
 * exactly once. Saved games are never changed.
 */
#include "guest_va.h"
#include "host.h"

#include <lzexpand.h>
#include <stdlib.h>
#include <string.h>

#include "patch117_policy.h"

#define MAX_VIEWS 4
#define EXPAND_CHUNK (1 << 20)
#define ADD_CHILD_VA GVA(0x00475F60) /* CPartition::bAddChild(child, test_priority), thiscall, every partition class */

typedef struct {
    INT handle;
    uint8_t* data;
    LONG size, pos;
} scene_view_t;

static int s_on;
static scene_view_t s_views[MAX_VIEWS];
static const patch117_level_t* s_last_logged;
static DWORD s_last_logged_tick;
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

static uint8_t* read_whole(const char* path, LONG* size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER n;
    uint8_t* data = NULL;
    DWORD got = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(f, &n) && n.QuadPart > 0 && n.QuadPart < 64 * 1024 * 1024) data = (uint8_t*)malloc((size_t)n.QuadPart);
    if (data && (!ReadFile(f, data, (DWORD)n.QuadPart, &got, NULL) || got != (DWORD)n.QuadPart)) {
        free(data);
        data = NULL;
    }
    CloseHandle(f);
    *size = data ? (LONG)n.QuadPart : 0;
    return data;
}

static scene_view_t* view_of(INT handle) {
    for (int i = 0; i < MAX_VIEWS; i++)
        if (s_views[i].data && s_views[i].handle == handle) return &s_views[i];
    return NULL;
}

static scene_view_t* free_view(void) {
    for (int i = 0; i < MAX_VIEWS; i++)
        if (!s_views[i].data) return &s_views[i];
    return NULL;
}

static void serve(scene_view_t* slot, INT handle, uint8_t* data, LONG size) {
    slot->handle = handle;
    slot->data = data;
    slot->size = size;
    slot->pos = 0;
}

static void open_scene(const char* path, INT handle) {
    LONG size;
    uint8_t* data = read_whole(path, &size);
    const patch117_level_t* level = data ? patch117_identify(path, data, (size_t)size) : NULL;
    scene_view_t* slot = free_view();
    int fixes;
    if (!level) {
        host_log("patch117", "%s is not the retail level file: official level fixes skipped", path);
        free(data);
        return;
    }
    fixes = patch117_fix_scene(level, data, (size_t)size);
    if (fixes <= 0 || !slot) {
        if (fixes <= 0) host_log("patch117", "%s: official level fixes did not match the retail data, skipped", level->id);
        free(data);
        return;
    }
    serve(slot, handle, data, size);
    /* One load opens the scene for its header and again for its contents. */
    if (level != s_last_logged || GetTickCount() - s_last_logged_tick > 5000)
        host_log("patch117", "applied %d official level fixes to %s", fixes, level->id);
    s_last_logged = level;
    s_last_logged_tick = GetTickCount();
}

static uint8_t* read_expanded(const char* path, LONG size) {
    OFSTRUCT opened;
    INT h = LZOpenFileA((LPSTR)path, &opened, OF_READ);
    uint8_t* data = h >= 0 ? (uint8_t*)malloc((size_t)size) : NULL;
    LONG got = 0;
    while (data && got < size) {
        INT n = LZRead(h, (LPSTR)(data + got), size - got < EXPAND_CHUNK ? size - got : EXPAND_CHUNK);
        if (n <= 0) break;
        got += n;
    }
    if (h >= 0) LZClose(h);
    if (data && got != size) {
        free(data);
        data = NULL;
    }
    return data;
}

static void open_world(const char* path, INT handle) {
    LONG size;
    uint8_t* raw = read_whole(path, &size);
    const patch117_world_t* world = raw ? patch117_world_identify(path, raw, (size_t)size) : NULL;
    scene_view_t* slot = free_view();
    uint8_t* image;
    int fixes;
    free(raw);
    if (!world) {
        host_log("patch117", "%s is not the retail world file: official object placements skipped", path);
        return;
    }
    image = slot ? read_expanded(path, (LONG)world->expanded_size) : NULL;
    fixes = image ? patch117_fix_world(world, image, world->expanded_size) : -1;
    if (fixes <= 0) {
        host_log("patch117", "%s: official object placements did not match the retail data, skipped", world->id);
        free(image);
        return;
    }
    serve(slot, handle, image, (LONG)world->expanded_size);
    host_log("patch117", "applied %d official object placements to %s", fixes, world->id);
}

void patch117_lz_opened(const char* path, INT handle) {
    if (!s_on || handle < 0) return;
    if (patch117_level_named(path))
        open_scene(path, handle);
    else if (patch117_world_named(path))
        open_world(path, handle);
}

void shim_LZRead(void) {
    INT handle = (INT)GUEST_ARG(0);
    char* buffer = (char*)GUEST_PTR(1);
    INT wanted = (INT)GUEST_ARG(2);
    scene_view_t* v = view_of(handle);
    if (!v) {
        g_eax = (uint32_t)LZRead(handle, buffer, wanted);
    } else {
        LONG n = wanted < 0 ? 0 : v->size - v->pos < wanted ? v->size - v->pos : wanted;
        memcpy(buffer, v->data + v->pos, (size_t)n);
        v->pos += n;
        g_eax = (uint32_t)n;
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_LZSeek(void) {
    INT handle = (INT)GUEST_ARG(0);
    LONG offset = (LONG)GUEST_ARG(1);
    INT origin = (INT)GUEST_ARG(2);
    scene_view_t* v = view_of(handle);
    if (!v) {
        g_eax = (uint32_t)LZSeek(handle, offset, origin);
    } else {
        LONG base = origin == 1 ? v->pos : origin == 2 ? v->size : 0, to = base + offset;
        if (origin < 0 || origin > 2 || to < 0 || to > v->size) {
            g_eax = (uint32_t)LZERROR_BADVALUE;
        } else {
            v->pos = to;
            g_eax = (uint32_t)to;
        }
    }
    GUEST_RETURN_STDCALL(3);
}

void shim_LZClose(void) {
    INT handle = (INT)GUEST_ARG(0);
    scene_view_t* v = view_of(handle);
    if (v) {
        free(v->data);
        memset(v, 0, sizeof *v);
    }
    LZClose(handle);
    GUEST_RETURN_STDCALL(1);
}
