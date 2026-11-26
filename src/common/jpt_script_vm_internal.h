// The VM's state, shared by the interpreter (jpt_script_vm.cpp) and CE's API bound into it (jpt_script_api.cpp).
#pragma once

#include "jpt_script_draw.h"
#include "jpt_script_engine.h"
#include "jpt_script_heap.h"
#include "jpt_script_math.h"
#include "jpt_script_vm.h"

#include <squirrel.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct jpt_script_vm {
    HSQUIRRELVM v = nullptr;
    jpt_script_config config{};
    std::unique_ptr<ScriptHeap> heap;
    bool abandoned = false;
    int printed = 0;
    std::vector<std::string> disabled;
    std::vector<HSQOBJECT> instances;
    std::string level;
    std::string error;
    std::string err_file, err_function;
    long long err_line = 0;

    unsigned world = 0;       // the world generation this VM's objects belong to
    bool world_ready = false; // the level's world is loaded: the engine may be reached
    uint32_t random = 0;
    HSQOBJECT vector_class{}, rotate_class{}, texture_class{};
    std::map<std::string, HSQOBJECT> natives;    // CE's entity classes by name
    std::map<uint64_t, HSQOBJECT> wrappers;      // objects of the world handed to scripts, by handle and box
    std::map<uint32_t, HSQOBJECT> scripted;      // an object's instance script, by handle
    std::map<uint32_t, int> entities;            // the same, as an instance handle of this VM
    std::vector<std::string> owners;             // of each instance, as its [script] lines name it
    std::vector<int> receivers;                  // game and level scripts, in the order SendLGMessage reaches them
    std::set<uint32_t> stepping;                 // objects whose instance script ActivateSimStep(true) turned on
    int current = 0;                             // the instance whose code runs now; 0 for none
    int message_depth = 0;
    std::string native_name;

    struct Texture {
        std::string key;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    std::vector<Texture> textures; // a texture's id is its index + 1
    size_t texture_bytes = 0;
    bool painting = false;         // inside OnPaintHW3D: DrawTexture is allowed
    std::vector<jpt_draw_cmd_t> draws;
    std::vector<int> draw_owners;

    bool reading_input = false;    // inside OnHandleInput or OnHandleModalInput
    uint32_t user_held = 0, user_hit = 0; // uCMD_USER1..8 as bits 0..7
    float move_x = 0.0f, move_y = 0.0f;
    int modal = 0;                 // the game script in modal mode; 0 for none
};

namespace jpt::script {

// The reference an entity instance keeps: never a pointer, looked up at each call.
struct EntityRef {
    uint32_t handle;
    int32_t box;    // -1: the object itself
    uint32_t world; // its VM's world generation
};

inline jpt_script_vm* Owner(HSQUIRRELVM v) { return static_cast<jpt_script_vm*>(sq_getforeignptr(v)); }

// Binds Vector3, Rotate3, the entity classes, TP_Engine and the enums into the root table.
void BindApi(jpt_script_vm* vm);
void ReleaseApi(jpt_script_vm* vm);

void PushVector(HSQUIRRELVM v, const V3& value);
void PushRotate(HSQUIRRELVM v, const Q& value);
bool GetVector(HSQUIRRELVM v, SQInteger at, V3& out);
bool GetRotate(HSQUIRRELVM v, SQInteger at, Q& out);
// The object as scripts see it (its instance script, or an object of its native class); null when the world has none.
void PushEntity(jpt_script_vm* vm, uint32_t handle, int box);
// The handle of an entity instance at, without checking the world; false when it is no entity.
bool GetEntity(HSQUIRRELVM v, SQInteger at, uint32_t& handle);
// The CE entity class a class derives from, nearest first; empty when none.
std::string NativeBase(jpt_script_vm* vm, const HSQOBJECT& klass);
bool SetEntity(HSQUIRRELVM v, SQInteger at, uint32_t handle, int box, unsigned world);
// Calls a method of one of the VM's instances from a native function, with the values at those stack positions; its owner is
// stopped when it fails. With retval its result is pushed when it succeeded.
jpt_script_result CallFromScript(jpt_script_vm* vm, int instance, const char* method, const SQInteger* at, int count, bool retval);

} // namespace jpt::script
