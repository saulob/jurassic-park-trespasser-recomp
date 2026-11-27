// A world of objects for the script API suites (script_bindings_tests.cpp, script_instance_tests.cpp): the engine side of
// jpt_script_engine.h without the game, recording what scripts asked of it.
#pragma once

#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "jpt_script_engine.h"

struct WorldObject {
    std::string name, type;
    jpt_vec3 position{0, 0, 0};
    jpt_rot3 rotation{1, 0, 0, 0};
    jpt_vec3 head{0, 0, 0};
    jpt_rot3 head_rotation{1, 0, 0, 0}; // a player's, relative to its body
    std::map<int, jpt_rot3> boxes; // boundary boxes by EBoundaryBoxes value, with their rotation
    bool movable = true;
    float hit_points = 100.0f;
    int ammo = 0, max_ammo = 0;
    uint32_t held = 0;
};

struct FixedMove {
    uint32_t handle;
    jpt_vec3 velocity;
    int milliseconds;
};

struct ShownText {
    std::string text;
    float seconds;
    uint32_t format, rgb, previous, type, handle;
};

struct Spoken {
    uint32_t player, sample;
};

struct Picture {
    int width, height;
    uint32_t rgba; // every texel
};

struct World {
    std::vector<ShownText> texts;
    std::map<std::string, Picture> pictures; // by the path a script asks for
    int screen_w = 1024, screen_h = 768;
    int loads = 0;
    std::map<uint32_t, WorldObject> objects;
    std::vector<uint32_t> teleported;
    std::vector<FixedMove> moves;
    std::map<uint32_t, bool> moving;
    float time = 0.0f;
    int lookups = 0;
    uint32_t player = 0;
    std::set<uint32_t> samples; // the sample IDs the game has
    std::vector<Spoken> spoken;   // what reached the game's voice
    int said = 0;                 // every call that reached the game, known sample or not

    uint32_t Add(const std::string& name, const std::string& type) {
        uint32_t h = 2166136261u;
        for (char c : name) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
        WorldObject o;
        o.name = name;
        o.type = type;
        objects[h] = o;
        return h;
    }

    WorldObject* At(uint32_t handle) {
        const auto it = objects.find(handle);
        return it == objects.end() ? nullptr : &it->second;
    }
};

static inline World* WorldOf(void* user) { return static_cast<World*>(user); }

static inline const char* WorldType(void* user, uint32_t handle) {
    World* w = WorldOf(user);
    w->lookups++;
    const WorldObject* o = w->At(handle);
    return o ? o->type.c_str() : nullptr;
}

static inline uint32_t WorldFind(void* user, const char* name) {
    for (const auto& [handle, o] : WorldOf(user)->objects)
        if (o.name == name) return handle;
    return 0;
}

static inline int WorldPlacement(void* user, uint32_t handle, int box, jpt_vec3* position, jpt_rot3* rotation) {
    const WorldObject* o = WorldOf(user)->At(handle);
    if (!o) return 0;
    if (box >= 0) {
        const auto it = o->boxes.find(box);
        if (it == o->boxes.end()) return 0;
        *position = o->head;
        *rotation = it->second;
        return 1;
    }
    *position = o->position;
    *rotation = o->rotation;
    return 1;
}

static inline int WorldHead(void* user, uint32_t handle, jpt_vec3* out) {
    const WorldObject* o = WorldOf(user)->At(handle);
    if (!o || o->type != "CAnimal") return 0;
    *out = o->head;
    return 1;
}

static inline int WorldTeleport(void* user, uint32_t handle, const jpt_vec3* position, const jpt_rot3* rotation) {
    World* w = WorldOf(user);
    WorldObject* o = w->At(handle);
    if (!o) return 0;
    o->position = *position;
    o->rotation = *rotation;
    w->teleported.push_back(handle);
    return 1;
}

static inline int WorldFixed(void* user, uint32_t handle, const jpt_vec3* velocity, int milliseconds, int fail_if_moving) {
    World* w = WorldOf(user);
    const WorldObject* o = w->At(handle);
    if (!o || !o->movable || (fail_if_moving && w->moving[handle])) return 0;
    w->moving[handle] = true;
    w->moves.push_back({handle, *velocity, milliseconds});
    return 1;
}

static inline float WorldTime(void* user) { return WorldOf(user)->time; }

static inline uint32_t WorldPlayer(void* user) { return WorldOf(user)->At(WorldOf(user)->player) ? WorldOf(user)->player : 0; }

static inline uint32_t WorldHeld(void* user, uint32_t player) {
    const WorldObject* o = WorldOf(user)->At(player);
    return o && WorldOf(user)->At(o->held) ? o->held : 0;
}

static inline bool Animate(const WorldObject* o) { return o && (o->type == "CAnimal" || o->type == "CPlayer" || o->type == "CAnimate"); }

static inline int WorldHealth(void* user, uint32_t handle, float* hit_points, int* dead) {
    const WorldObject* o = WorldOf(user)->At(handle);
    if (!Animate(o)) return 0;
    *hit_points = o->hit_points;
    *dead = o->hit_points <= 0.0f;
    return 1;
}

static inline int WorldAmmo(void* user, uint32_t gun, int* ammo, int* max_ammo) {
    const WorldObject* o = WorldOf(user)->At(gun);
    if (!o || o->type != "CGun") return 0;
    *ammo = o->ammo;
    *max_ammo = o->max_ammo;
    return 1;
}

static inline int WorldSetAmmo(void* user, uint32_t gun, int ammo, int max_ammo) {
    WorldObject* o = WorldOf(user)->At(gun);
    if (!o || o->type != "CGun" || ammo < 0 || ammo > max_ammo) return 0;
    o->ammo = ammo;
    o->max_ammo = max_ammo;
    return 1;
}

static inline uint32_t WorldShowText(void* user, const char* text, float seconds, uint32_t format, uint32_t rgb, uint32_t previous, uint32_t type) {
    World* w = WorldOf(user);
    bool held = !previous;
    for (const ShownText& t : w->texts) held = held || t.handle == previous;
    if (!held) return 0;
    const uint32_t handle = 0x1000u + static_cast<uint32_t>(w->texts.size()) * 0x40u;
    w->texts.push_back({text, seconds, format, rgb, previous, type, handle});
    return handle;
}

static inline uint32_t WorldTextEnd(void* user, uint32_t type) {
    const World* w = WorldOf(user);
    for (size_t i = w->texts.size(); i-- > 0;)
        if (w->texts[i].type == type) return w->texts[i].handle;
    return 0;
}

static inline uint8_t* WorldLoadTexture(void* user, const char* name, int* width, int* height) {
    World* w = WorldOf(user);
    const auto it = w->pictures.find(name);
    if (it == w->pictures.end()) return nullptr;
    w->loads++;
    const size_t n = static_cast<size_t>(it->second.width) * static_cast<size_t>(it->second.height);
    uint8_t* rgba = static_cast<uint8_t*>(malloc(n * 4u));
    for (size_t i = 0; rgba && i < n; i++) memcpy(rgba + 4 * i, &it->second.rgba, 4);
    *width = it->second.width;
    *height = it->second.height;
    return rgba;
}

static inline int WorldScreen(void* user, int* width, int* height) {
    *width = WorldOf(user)->screen_w;
    *height = WorldOf(user)->screen_h;
    return *width > 0 && *height > 0;
}

static inline int WorldHeadRotation(void* user, uint32_t player, jpt_rot3* out) {
    const WorldObject* o = WorldOf(user)->At(player);
    if (!o || o->type != "CPlayer") return 0;
    *out = o->head_rotation;
    return 1;
}

// The game plays a sample it has and ignores another, as CPlayer's voice does.
static inline int WorldSay(void* user, uint32_t player, uint32_t sample) {
    World* w = WorldOf(user);
    const WorldObject* o = w->At(player);
    if (!o || o->type != "CPlayer") return 0;
    w->said++;
    if (w->samples.count(sample)) w->spoken.push_back({player, sample});
    return 1;
}

static inline jpt_script_engine EngineOf(World& world) {
    jpt_script_engine e{};
    e.user = &world;
    e.native_type = WorldType;
    e.find = WorldFind;
    e.placement = WorldPlacement;
    e.head_position = WorldHead;
    e.teleport = WorldTeleport;
    e.fixed_velocity = WorldFixed;
    e.sim_time = WorldTime;
    e.player = WorldPlayer;
    e.held_object = WorldHeld;
    e.health = WorldHealth;
    e.gun_ammo = WorldAmmo;
    e.set_gun_ammo = WorldSetAmmo;
    e.show_text = WorldShowText;
    e.text_end = WorldTextEnd;
    e.load_texture = WorldLoadTexture;
    e.screen_size = WorldScreen;
    e.head_rotation = WorldHeadRotation;
    e.say = WorldSay;
    return e;
}
