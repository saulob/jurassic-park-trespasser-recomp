#include "patch117_policy.h"

#include <string.h>

#include "jpt_hash.h"

#define GROFF_HEADER_SIZE 48u
#define GROFF_SECTION_SIZE 32u
#define SYMBOL_HEADER_SIZE 12u
#define TERRAIN_SECTION_SIZE 72u

const patch117_level_t patch117_levels[PATCH117_LEVEL_COUNT] = {
    { "be.scn", "BE", 312139u, "6c7ba65ab4c9486cc6d5368c8071bee84b91b14578f0de9366c61c44ca79cd59", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "jr.scn", "JR", 601847u, "5b6af513034c48fb7e2f31a18ec5c14f1b196c1ce389712c938d8141604d03ba", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "ij.scn", "IJ", 704545u, "6c784cbca163195ea191070e6a2c8c957c8c5fd3fa7d19b45c6d5e32093221d4", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "it.scn", "IT", 787800u, "6f620454bd6a817bb28b7641560d53bbb4896ed5723823565a08ccbfc7fb12fd", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "lab.scn", "LAB", 540951u, "8c4d5037a6f9a2a818213a6e8d6a5ea48b8a697d2a38df2d3386171e3b0e65c0", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "as.scn", "AS", 423131u, "c5f484e8268b9613ccecfa6fd9704e6a81014c3efee1c347f7dc8623c58ac373", { 8.0f, 10.0f }, { 20.0f, 50.0f },
      patch117_presences_as, PATCH117_PRESENCES_AS },
    { "as2.scn", "AS2", 314074u, "81a7282d22097d505a1f261be696ab90b7c5b4ba0a7359c7d7ad6447c833182d", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
    { "sum.scn", "SUM", 165399u, "bb79a9d5973ec543beabcf11c39309b8f83f1b2e4fc16a83914aa9ed61947e28", { 8.0f, 10.0f }, { 20.0f, 50.0f }, NULL, 0 },
};

const patch117_world_t patch117_worlds[PATCH117_WORLD_COUNT] = {
    { "ij.grf", "IJ", 6073762u, "e4691a6e2341a8a7f1a90a76d59fe3f89f88232dc39a2042159159c6e93997ea", 16150425u, patch117_places_ij,
      PATCH117_PLACES_IJ },
    { "it.grf", "IT", 7472799u, "dcad4794e6ea4dfe967dedbcd3ba20836a11a448621ddcb24a63ba8b829a6ea0", 21908188u, patch117_places_it,
      PATCH117_PLACES_IT },
};

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char* file_name(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    return name;
}

const patch117_level_t* patch117_level_named(const char* path) {
    const char* name;
    if (!path) return NULL;
    name = file_name(path);
    for (int i = 0; i < PATCH117_LEVEL_COUNT; i++)
        if (!_stricmp(name, patch117_levels[i].file)) return &patch117_levels[i];
    return NULL;
}

const patch117_level_t* patch117_identify(const char* path, const void* data, size_t size) {
    const patch117_level_t* level = patch117_level_named(path);
    char hex[65];
    if (!level || !data || size != level->size) return NULL;
    jpt_sha256_buffer_hex(data, size, hex);
    return strcmp(hex, level->sha256) ? NULL : level;
}

static int symbol_is(const uint8_t* image, size_t size, uint32_t offset, uint32_t entries, uint32_t handle,
                     const char* name) {
    size_t want = strlen(name) + 1;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t length;
        if (offset > size || size - offset < SYMBOL_HEADER_SIZE) return 0;
        length = rd32(image + offset + 4);
        if (size - offset - SYMBOL_HEADER_SIZE < length) return 0;
        if (rd32(image + offset) == handle)
            return length == want && !memcmp(image + offset + SYMBOL_HEADER_SIZE, name, want);
        offset += SYMBOL_HEADER_SIZE + length;
    }
    return 0;
}

uint8_t* patch117_groff_section(uint8_t* image, size_t size, const char* name, uint32_t* section_size) {
    uint32_t count, symbols, symtab;
    if (!image || size < GROFF_HEADER_SIZE || rd32(image) != PATCH117_GROFF_MAGIC) return NULL;
    count = rd32(image + 8);
    symbols = rd32(image + 12);
    symtab = rd32(image + 20);
    if (count > (size - GROFF_HEADER_SIZE) / GROFF_SECTION_SIZE) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* header = image + GROFF_HEADER_SIZE + (size_t)i * GROFF_SECTION_SIZE;
        uint32_t offset = rd32(header + 8), length = rd32(header + 12);
        if (!symbol_is(image, size, symtab, symbols, rd32(header), name)) continue;
        if (offset > size || size - offset < length) return NULL;
        if (section_size) *section_size = length;
        return image + offset;
    }
    return NULL;
}

static int holds(const uint8_t* p, float value) { return !memcmp(p, &value, sizeof value); }

int patch117_fix_terrain(const patch117_level_t* level, uint8_t* image, size_t size) {
    uint32_t length = 0;
    uint8_t* terrain = level ? patch117_groff_section(image, size, PATCH117_TERRAIN_SECTION, &length) : NULL;
    if (!terrain || length != TERRAIN_SECTION_SIZE || rd32(terrain) != PATCH117_TERRAIN_VERSION) return -1;
    if (!holds(terrain + 4, level->pixel_tol.retail) || !holds(terrain + 8, level->pixel_tol_far.retail)) return -1;
    memcpy(terrain + 4, &level->pixel_tol.official, sizeof(float));
    memcpy(terrain + 8, &level->pixel_tol_far.official, sizeof(float));
    return 2;
}

static uint8_t* presence_at(const patch117_presence_t* p, uint8_t* image, size_t size) {
    uint32_t length = 0;
    uint8_t* section = patch117_groff_section(image, size, p->section, &length);
    return section && length >= sizeof p->retail ? section : NULL;
}

static int presences_hold(const patch117_level_t* level, uint8_t* image, size_t size) {
    for (int i = 0; i < level->presence_count; i++) {
        const patch117_presence_t* p = &level->presences[i];
        uint8_t* at = presence_at(p, image, size);
        if (!at || memcmp(at, p->retail, sizeof p->retail)) return 0;
    }
    return 1;
}

static void write_presences(const patch117_level_t* level, uint8_t* image, size_t size) {
    for (int i = 0; i < level->presence_count; i++)
        memcpy(presence_at(&level->presences[i], image, size), level->presences[i].official, sizeof level->presences[i].official);
}

int patch117_fix_presences(const patch117_level_t* level, uint8_t* image, size_t size) {
    if (!level || !presences_hold(level, image, size)) return -1;
    write_presences(level, image, size);
    return level->presence_count;
}

int patch117_fix_scene(const patch117_level_t* level, uint8_t* image, size_t size) {
    int terrain;
    if (!level || !presences_hold(level, image, size)) return -1;
    terrain = patch117_fix_terrain(level, image, size);
    if (terrain < 0) return -1;
    write_presences(level, image, size);
    return terrain + level->presence_count;
}

const patch117_world_t* patch117_world_named(const char* path) {
    const char* name;
    if (!path) return NULL;
    name = file_name(path);
    for (int i = 0; i < PATCH117_WORLD_COUNT; i++)
        if (!_stricmp(name, patch117_worlds[i].file)) return &patch117_worlds[i];
    return NULL;
}

const patch117_world_t* patch117_world_identify(const char* path, const void* data, size_t size) {
    const patch117_world_t* world = patch117_world_named(path);
    char hex[65];
    if (!world || !data || size != world->size) return NULL;
    jpt_sha256_buffer_hex(data, size, hex);
    return strcmp(hex, world->sha256) ? NULL : world;
}

int patch117_fix_world(const patch117_world_t* world, uint8_t* image, size_t size) {
    if (!world || !image || size != world->expanded_size) return -1;
    for (int i = 0; i < world->count; i++) {
        const patch117_place_t* p = &world->places[i];
        if (p->offset > size - sizeof p->retail || memcmp(image + p->offset, p->retail, sizeof p->retail)) return -1;
    }
    for (int i = 0; i < world->count; i++)
        memcpy(image + world->places[i].offset, world->places[i].official, sizeof world->places[i].official);
    return world->count;
}

int patch117_add_child_flag(int fixes_on, int nested, int caller_flag, int own_flag) {
    return fixes_on && nested ? caller_flag != 0 : own_flag != 0;
}
