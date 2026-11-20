#include "linux_resource.h"

#include <stdlib.h>
#include <string.h>

#include "guest_ptr.h"

#define MAX_NAME 128

/* A checked read: an image that is not mapped is a refusal, not a fault. */
static int read32(uint32_t va, uint32_t* out) { return gp_copy_in(out, va, 4); }

static int name_matches(uint32_t dir, uint32_t entry_name, uint32_t want) {
    char text[MAX_NAME];
    const char* wanted;
    uint16_t len;
    uint32_t p = dir + (entry_name & 0x7FFFFFFFu);
    if (!(entry_name & 0x80000000u)) return want <= 0xFFFFu && entry_name == want;
    if (want <= 0xFFFFu || !gp_copy_in(&len, p, 2) || len >= MAX_NAME) return 0;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t ch;
        if (!gp_copy_in(&ch, p + 2u + 2u * i, 2)) return 0;
        text[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    }
    text[len] = 0;
    wanted = gp_string(want, MAX_NAME);
    if (!wanted || strlen(wanted) != len) return 0;
    for (uint16_t i = 0; i < len; i++) {
        char c = wanted[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != text[i]) return 0;
    }
    return 1;
}

/* The entry of a directory that matches want, as the offset of its subdirectory or data entry; 0 when absent. */
static int find_entry(uint32_t dir, uint32_t want, uint32_t any, uint32_t* child) {
    uint16_t named = 0, ids = 0;
    if (!gp_copy_in(&named, dir + 12, 2) || !gp_copy_in(&ids, dir + 14, 2)) return 0;
    for (uint32_t i = 0; i < (uint32_t)named + ids; i++) {
        uint32_t name = 0, data = 0, e = dir + 16 + i * 8u;
        if (!read32(e, &name) || !read32(e + 4, &data)) return 0;
        if (any || name_matches(dir, name, want)) {
            *child = data;
            return 1;
        }
    }
    return 0;
}

static uint32_t id_of(uint32_t name) {
    const char* s;
    if (name <= 0xFFFFu) return name;
    s = gp_string(name, MAX_NAME);
    return s && s[0] == '#' ? (uint32_t)strtoul(s + 1, NULL, 10) : name;
}

int lx_resource_find(uint32_t image_base, uint32_t type, uint32_t name, uint32_t* data_va, uint32_t* size) {
    uint32_t pe = 0, rva = 0, root, dir, child = 0, data_rva = 0, bytes = 0;
    if (!read32(image_base + 0x3C, &pe) || !read32(image_base + pe + 0x88, &rva) || !rva) return 0;
    root = image_base + rva;
    if (!find_entry(root, id_of(type), 0, &child) || !(child & 0x80000000u)) return 0;
    dir = root + (child & 0x7FFFFFFFu);
    if (!find_entry(dir, id_of(name), 0, &child) || !(child & 0x80000000u)) return 0;
    dir = root + (child & 0x7FFFFFFFu);
    if (!find_entry(dir, 0, 1, &child) || (child & 0x80000000u)) return 0;
    if (!read32(root + child, &data_rva) || !read32(root + child + 4, &bytes)) return 0;
    *data_va = image_base + data_rva;
    *size = bytes;
    return 1;
}
