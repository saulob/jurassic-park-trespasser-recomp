// CE's script API (API-reference.txt) bound into a VM; jpt_script_registry.inc lists what is here.
#include "jpt_script_vm_internal.h"

#include "jpt_hash.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace jpt::script {
namespace {

int kVectorTag, kRotateTag, kEntityTag, kTextureTag;

// A texture of TP_Engine.GetTexture: its id in the VM and the world it was loaded for.
struct TextureRef {
    uint32_t id;
    uint32_t world;
};

// uCMD_USER1 to uCMD_USER8: the port's values, bits above the game's own commands.
constexpr SQInteger kUserCommand = 0x10000;
constexpr int kUserCommands = 8;

const struct {
    const char* name;
    int value;
} kVirtualKeys[] = {{"VK_PRIOR", 0x21}, {"VK_NEXT", 0x22}, {"VK_END", 0x23}, {"VK_HOME", 0x24}, {"VK_LEFT", 0x25}, {"VK_RIGHT", 0x27}};

// EBoundaryBoxes of the engine: the aliases share a box.
const struct {
    const char* name;
    int value;
} kBoxes[] = {{"ebbBODY", 0}, {"ebbFOOT", 1}, {"ebbRIGHT_FOOT", 1}, {"ebbHAND", 2}, {"ebbHEAD", 3}, {"ebbTAIL", 4},
              {"ebbLEFT_FOOT", 5}, {"ebbARM", 5}, {"ebbFRIGHT_FOOT", 6}, {"ebbFLEFT_FOOT", 7}};

// CE's entity classes the port binds, each after its base.
const struct {
    const char* name;
    const char* base;
} kEntityClasses[] = {{"CPartition", nullptr}, {"CInstance", "CPartition"}, {"CBoundaryBox", "CInstance"}, {"CEntity", "CInstance"},
                      {"CAnimate", "CEntity"},   {"CAnimal", "CAnimate"},     {"CPlayer", "CAnimate"},      {"CGun", "CEntity"}};

// The text overlay's format bits and ETextType categories, as the game itself uses them (subtitles: centre and bottom, type 2).
const struct {
    const char* name;
    int value;
} kTextFormats[] = {{"TEXT_FORMAT_CENTER", 0x1}, {"TEXT_FORMAT_LEFT", 0x2},     {"TEXT_FORMAT_RIGHT", 0x4},
                    {"TEXT_FORMAT_TOP", 0x8},    {"TEXT_FORMAT_BOTTOM", 0x10}, {"TEXT_FORMAT_SOLID", 0x200}};
const struct {
    const char* name;
    int value;
} kTextTypes[] = {{"ettALL", 0}, {"ettUNKNOWN", 1}, {"ettSUBTITLE", 2}, {"ettTUTORIAL", 3}};

// Messages that send messages: past this depth the sender gets a script error instead of an endless chain.
constexpr int kMessageDepth = 16;

V3* VectorAt(HSQUIRRELVM v, SQInteger at) {
    SQUserPointer p = nullptr;
    return SQ_SUCCEEDED(sq_getinstanceup(v, at, &p, &kVectorTag, SQFalse)) ? static_cast<V3*>(p) : nullptr;
}

Q* RotateAt(HSQUIRRELVM v, SQInteger at) {
    SQUserPointer p = nullptr;
    return SQ_SUCCEEDED(sq_getinstanceup(v, at, &p, &kRotateTag, SQFalse)) ? static_cast<Q*>(p) : nullptr;
}

EntityRef* EntityAt(HSQUIRRELVM v, SQInteger at) {
    SQUserPointer p = nullptr;
    return SQ_SUCCEEDED(sq_getinstanceup(v, at, &p, &kEntityTag, SQFalse)) ? static_cast<EntityRef*>(p) : nullptr;
}

bool Number(HSQUIRRELVM v, SQInteger at, float& out) {
    const SQObjectType type = sq_gettype(v, at);
    SQFloat f = 0;
    if ((type != OT_FLOAT && type != OT_INTEGER) || SQ_FAILED(sq_getfloat(v, at, &f))) return false;
    out = static_cast<float>(f);
    return true;
}

bool Integer(HSQUIRRELVM v, SQInteger at, SQInteger& out) {
    const SQObjectType type = sq_gettype(v, at);
    if (type == OT_INTEGER) return SQ_SUCCEEDED(sq_getinteger(v, at, &out));
    float f = 0;
    if (type != OT_FLOAT || !Number(v, at, f) || !std::isfinite(f)) return false;
    out = static_cast<SQInteger>(f);
    return true;
}

SQInteger Push(HSQUIRRELVM v, float f) {
    sq_pushfloat(v, static_cast<SQFloat>(f));
    return 1;
}

SQInteger Throw(HSQUIRRELVM v, const std::string& text) { return sq_throwerror(v, text.c_str()); }

// CE's u4 values (handles, colours): the engine's 32 bits, positive where Squirrel's ints are 64-bit, as a script's own literals are.
SQInteger U4(uint32_t value) { return static_cast<SQInteger>(value); }

// ---- Vector3 ----

SQInteger VectorConstruct(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    const SQInteger n = sq_gettop(v);
    if (!self) return Throw(v, "Vector3: not a Vector3");
    if (n == 1) return *self = V3{}, 0;
    if (n == 2) {
        const V3* other = VectorAt(v, 2);
        if (!other) return Throw(v, "Vector3(v3): the argument is not a Vector3");
        return *self = *other, 0;
    }
    V3 value;
    if (n != 4 || !Number(v, 2, value.x) || !Number(v, 3, value.y) || !Number(v, 4, value.z))
        return Throw(v, "Vector3: expects (), (v3) or (fX, fY, fZ)");
    return *self = value, 0;
}

SQInteger Component(HSQUIRRELVM v, float* x, float* y, float* z, float* w) {
    const SQChar* key = nullptr;
    if (sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &key))) return sq_pushnull(v), sq_throwobject(v);
    if (!strcmp(key, "x")) return Push(v, *x);
    if (!strcmp(key, "y")) return Push(v, *y);
    if (!strcmp(key, "z")) return Push(v, *z);
    if (w && !strcmp(key, "w")) return Push(v, *w);
    sq_pushnull(v);
    return sq_throwobject(v);
}

SQInteger SetComponent(HSQUIRRELVM v, float* x, float* y, float* z, float* w) {
    const SQChar* key = nullptr;
    float value = 0;
    if (sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &key))) return sq_pushnull(v), sq_throwobject(v);
    float* at = !strcmp(key, "x") ? x : !strcmp(key, "y") ? y : !strcmp(key, "z") ? z : (w && !strcmp(key, "w")) ? w : nullptr;
    if (!at) return sq_pushnull(v), sq_throwobject(v);
    if (!Number(v, 3, value)) return Throw(v, std::string("the component ") + key + " takes a number");
    *at = value;
    return 0;
}

SQInteger VectorGet(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    return self ? Component(v, &self->x, &self->y, &self->z, nullptr) : Throw(v, "not a Vector3");
}

SQInteger VectorSet(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    return self ? SetComponent(v, &self->x, &self->y, &self->z, nullptr) : Throw(v, "not a Vector3");
}

template <typename Op>
SQInteger VectorArith(HSQUIRRELVM v, const char* op, Op apply) {
    const V3* self = VectorAt(v, 1);
    if (!self) return Throw(v, "not a Vector3");
    V3 out;
    std::string error;
    if (const V3* other = VectorAt(v, 2)) {
        if (!apply(*self, *other, nullptr, out, error)) return Throw(v, error);
    } else {
        float f = 0;
        if (!Number(v, 2, f)) return Throw(v, std::string("Vector3 ") + op + ": the other operand is not a Vector3 or a number");
        const V3 scalar{f, f, f};
        if (!apply(*self, scalar, &f, out, error)) return Throw(v, error);
    }
    PushVector(v, out);
    return 1;
}

SQInteger VectorAdd(HSQUIRRELVM v) {
    return VectorArith(v, "+", [](V3 a, V3 b, const float*, V3& out, std::string&) { return out = Add(a, b), true; });
}

SQInteger VectorSub(HSQUIRRELVM v) {
    return VectorArith(v, "-", [](V3 a, V3 b, const float*, V3& out, std::string&) { return out = Sub(a, b), true; });
}

SQInteger VectorMul(HSQUIRRELVM v) {
    if (const Q* r = RotateAt(v, 2)) {
        const V3* self = VectorAt(v, 1);
        if (!self) return Throw(v, "not a Vector3");
        PushVector(v, Rotate(*self, *r));
        return 1;
    }
    return VectorArith(v, "*", [](V3 a, V3 b, const float*, V3& out, std::string&) { return out = Mul(a, b), true; });
}

SQInteger VectorDiv(HSQUIRRELVM v) {
    return VectorArith(v, "/", [](V3 a, V3 b, const float*, V3& out, std::string& error) {
        if (b.x == 0.0f || b.y == 0.0f || b.z == 0.0f) return error = "Vector3 /: division by zero", false;
        return out = {a.x / b.x, a.y / b.y, a.z / b.z}, true;
    });
}

SQInteger VectorString(HSQUIRRELVM v) {
    const V3* self = VectorAt(v, 1);
    char text[96];
    snprintf(text, sizeof text, "Vector3(%g, %g, %g)", self ? self->x : 0.0, self ? self->y : 0.0, self ? self->z : 0.0);
    sq_pushstring(v, text, -1);
    return 1;
}

SQInteger VectorCloned(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    const V3* original = VectorAt(v, 2);
    if (self && original) *self = *original;
    return 0;
}

SQInteger VectorScale(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    float f = 0;
    if (!self || !Number(v, 2, f)) return Throw(v, "Vector3.Scale(fScale): expects a number");
    *self = Mul(*self, f);
    return 0;
}

SQInteger VectorNormalise(HSQUIRRELVM v) {
    V3* self = VectorAt(v, 1);
    if (!self) return Throw(v, "not a Vector3");
    *self = Normalised(*self);
    return 0;
}

SQInteger VectorDot(HSQUIRRELVM v) {
    const V3* self = VectorAt(v, 1);
    const V3* other = VectorAt(v, 2);
    if (!self || !other) return Throw(v, "Vector3.Dot(v3Other): expects a Vector3");
    return Push(v, Dot(*self, *other));
}

SQInteger VectorCross(HSQUIRRELVM v) {
    const V3* self = VectorAt(v, 1);
    const V3* other = VectorAt(v, 2);
    if (!self || !other) return Throw(v, "Vector3.Cross(v3Other): expects a Vector3");
    PushVector(v, Cross(*self, *other));
    return 1;
}

SQInteger VectorLength(HSQUIRRELVM v) {
    const V3* self = VectorAt(v, 1);
    return self ? Push(v, Length(*self)) : Throw(v, "not a Vector3");
}

SQInteger VectorLengthSqr(HSQUIRRELVM v) {
    const V3* self = VectorAt(v, 1);
    return self ? Push(v, LengthSqr(*self)) : Throw(v, "not a Vector3");
}

SQInteger XAxis(HSQUIRRELVM v) { return PushVector(v, {1.0f, 0.0f, 0.0f}), 1; }
SQInteger YAxis(HSQUIRRELVM v) { return PushVector(v, {0.0f, 1.0f, 0.0f}), 1; }
SQInteger ZAxis(HSQUIRRELVM v) { return PushVector(v, {0.0f, 0.0f, 1.0f}), 1; }

// ---- Rotate3 ----

SQInteger RotateConstruct(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    const SQInteger n = sq_gettop(v);
    if (!self) return Throw(v, "Rotate3: not a Rotate3");
    if (n == 1) return *self = Q{}, 0;
    if (n == 2) {
        const Q* other = RotateAt(v, 2);
        if (!other) return Throw(v, "Rotate3(r3): the argument is not a Rotate3");
        return *self = *other, 0;
    }
    Q value;
    if (n == 3) {
        const V3* part = VectorAt(v, 3);
        if (!Number(v, 2, value.w) || !part) return Throw(v, "Rotate3(fW, v3): expects a number and a Vector3");
        value.x = part->x, value.y = part->y, value.z = part->z;
        return *self = value, 0;
    }
    if (n != 5 || !Number(v, 2, value.w) || !Number(v, 3, value.x) || !Number(v, 4, value.y) || !Number(v, 5, value.z))
        return Throw(v, "Rotate3: expects (), (r3), (fW, v3) or (fW, fX, fY, fZ)");
    return *self = value, 0;
}

SQInteger RotateGet(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    return self ? Component(v, &self->x, &self->y, &self->z, &self->w) : Throw(v, "not a Rotate3");
}

SQInteger RotateSet(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    return self ? SetComponent(v, &self->x, &self->y, &self->z, &self->w) : Throw(v, "not a Rotate3");
}

SQInteger RotateMul(HSQUIRRELVM v) {
    const Q* self = RotateAt(v, 1);
    if (!self) return Throw(v, "not a Rotate3");
    if (const Q* other = RotateAt(v, 2)) return PushRotate(v, Then(*self, *other)), 1;
    float f = 0;
    if (!Number(v, 2, f)) return Throw(v, "Rotate3 *: the other operand is not a Rotate3 or a number");
    PushRotate(v, Scaled(*self, f));
    return 1;
}

SQInteger RotateDiv(HSQUIRRELVM v) {
    const Q* self = RotateAt(v, 1);
    const Q* other = RotateAt(v, 2);
    if (!self || !other) return Throw(v, "Rotate3 /: the other operand is not a Rotate3");
    PushRotate(v, Then(*self, Inverse(*other)));
    return 1;
}

SQInteger RotateUnm(HSQUIRRELVM v) {
    const Q* self = RotateAt(v, 1);
    if (!self) return Throw(v, "not a Rotate3");
    PushRotate(v, Inverse(*self));
    return 1;
}

SQInteger RotateString(HSQUIRRELVM v) {
    const Q* q = RotateAt(v, 1);
    char text[128];
    snprintf(text, sizeof text, "Rotate3(%g, %g, %g, %g)", q ? q->w : 1.0, q ? q->x : 0.0, q ? q->y : 0.0, q ? q->z : 0.0);
    sq_pushstring(v, text, -1);
    return 1;
}

SQInteger RotateCloned(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    const Q* original = RotateAt(v, 2);
    if (self && original) *self = *original;
    return 0;
}

SQInteger RotateScale(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    float f = 0;
    if (!self || !Number(v, 2, f)) return Throw(v, "Rotate3.Scale(fScale): expects a number");
    *self = Scaled(*self, f);
    return 0;
}

SQInteger RotateNormalise(HSQUIRRELVM v) {
    Q* self = RotateAt(v, 1);
    if (!self) return Throw(v, "not a Rotate3");
    *self = Normalised(*self);
    return 0;
}

SQInteger RotateLength(HSQUIRRELVM v) {
    const Q* self = RotateAt(v, 1);
    return self ? Push(v, Length(*self)) : Throw(v, "not a Rotate3");
}

SQInteger RotateLengthSqr(HSQUIRRELVM v) {
    const Q* self = RotateAt(v, 1);
    return self ? Push(v, LengthSqr(*self)) : Throw(v, "not a Rotate3");
}

// ---- the world's objects ----

const jpt_script_engine* Engine(jpt_script_vm* vm) { return vm->config.engine; }

// The object this method is called on, checked against the world; false with the script error raised.
bool Live(HSQUIRRELVM v, EntityRef& ref, const char* method) {
    jpt_script_vm* vm = Owner(v);
    const EntityRef* at = EntityAt(v, 1);
    std::string error;
    if (!at) error = "it is not an object of the world";
    else if (!at->handle) error = "it was made by a script and is no object of the world";
    else if (at->world != vm->world) error = "its world was unloaded";
    else if (!vm->world_ready || !Engine(vm)) error = "the world is not loaded";
    else if (!Engine(vm)->native_type(Engine(vm)->user, at->handle)) error = "the object is no longer in the world";
    if (!error.empty()) {
        Throw(v, std::string(method) + ": " + error);
        return false;
    }
    ref = *at;
    return true;
}

SQInteger Pos(HSQUIRRELVM v) {
    EntityRef ref;
    jpt_vec3 p;
    jpt_rot3 r;
    if (!Live(v, ref, "v3Pos")) return SQ_ERROR;
    if (!Engine(Owner(v))->placement(Engine(Owner(v))->user, ref.handle, ref.box, &p, &r)) return Throw(v, "v3Pos: the object has no place");
    PushVector(v, {p.x, p.y, p.z});
    return 1;
}

SQInteger Rot(HSQUIRRELVM v) {
    EntityRef ref;
    jpt_vec3 p;
    jpt_rot3 r;
    if (!Live(v, ref, "r3Rot")) return SQ_ERROR;
    if (!Engine(Owner(v))->placement(Engine(Owner(v))->user, ref.handle, ref.box, &p, &r)) return Throw(v, "r3Rot: the object has no place");
    PushRotate(v, {r.w, r.x, r.y, r.z});
    return 1;
}

bool WholeObject(HSQUIRRELVM v, const EntityRef& ref, const char* method) {
    if (ref.box < 0) return true;
    Throw(v, std::string(method) + ": a boundary box moves with its animate");
    return false;
}

SQInteger Handle(HSQUIRRELVM v) {
    EntityRef ref;
    if (!Live(v, ref, "u4GetUniqueHandle") || !WholeObject(v, ref, "u4GetUniqueHandle")) return SQ_ERROR;
    sq_pushinteger(v, U4(ref.handle));
    return 1;
}

SQInteger InstanceName(HSQUIRRELVM v) {
    EntityRef ref;
    char name[16];
    if (!Live(v, ref, "GetInstanceName") || !WholeObject(v, ref, "GetInstanceName")) return SQ_ERROR;
    snprintf(name, sizeof name, "%08X", static_cast<unsigned>(ref.handle));
    sq_pushstring(v, name, -1);
    return 1;
}

SQInteger Teleport(HSQUIRRELVM v) {
    EntityRef ref;
    if (!Live(v, ref, "Teleport") || !WholeObject(v, ref, "Teleport")) return SQ_ERROR;
    const V3* p = VectorAt(v, 2);
    const Q* r = RotateAt(v, 3);
    if (sq_gettop(v) != 3 || !p || !r) return Throw(v, "Teleport(v3Pos, r3Rot): expects a Vector3 and a Rotate3");
    if (!Finite(*p) || !Finite(*r)) return Throw(v, "Teleport: a position or rotation that is not finite");
    const Q unit = Normalised(*r);
    const jpt_vec3 pos{p->x, p->y, p->z};
    const jpt_rot3 rot{unit.w, unit.x, unit.y, unit.z};
    if (!Engine(Owner(v))->teleport(Engine(Owner(v))->user, ref.handle, &pos, &rot)) return Throw(v, "Teleport: the object cannot be moved");
    return 0;
}

SQInteger FixedVelocity(HSQUIRRELVM v) {
    EntityRef ref;
    if (!Live(v, ref, "MoveWithFixedVelocity") || !WholeObject(v, ref, "MoveWithFixedVelocity")) return SQ_ERROR;
    const SQInteger n = sq_gettop(v);
    const V3* velocity = VectorAt(v, 2);
    SQInteger ms = 0;
    SQBool fail = SQFalse;
    if ((n != 3 && n != 4) || !velocity || !Integer(v, 3, ms) || (n == 4 && SQ_FAILED(sq_getbool(v, 4, &fail))))
        return Throw(v, "MoveWithFixedVelocity(v3Vel, iDurationMS, bFailIfAlreadyHasFixedVelocity): expects a Vector3, an int and a bool");
    if (!Finite(*velocity)) return Throw(v, "MoveWithFixedVelocity: a velocity that is not finite");
    const jpt_vec3 vel{velocity->x, velocity->y, velocity->z};
    const int duration = ms > 0x7FFFFFFF ? 0x7FFFFFFF : ms < -1 ? -1 : static_cast<int>(ms);
    const int done = Engine(Owner(v))->fixed_velocity(Engine(Owner(v))->user, ref.handle, &vel, duration, fail != SQFalse);
    sq_pushbool(v, done ? SQTrue : SQFalse);
    return 1;
}

SQInteger BoundaryBox(HSQUIRRELVM v) {
    EntityRef ref;
    SQInteger which = 0;
    jpt_vec3 p;
    jpt_rot3 r;
    if (!Live(v, ref, "pbbBoundaryBox")) return SQ_ERROR;
    if (sq_gettop(v) != 2 || !Integer(v, 2, which)) return Throw(v, "pbbBoundaryBox(EBoundaryBoxes): expects an EBoundaryBoxes value");
    if (ref.box >= 0 || which < 0 || which >= JPT_SCRIPT_BOXES) return sq_pushnull(v), 1;
    jpt_script_vm* vm = Owner(v);
    if (!Engine(vm)->placement(Engine(vm)->user, ref.handle, static_cast<int>(which), &p, &r)) return sq_pushnull(v), 1;
    PushEntity(vm, ref.handle, static_cast<int>(which));
    return 1;
}

SQInteger HeadPos(HSQUIRRELVM v) {
    EntityRef ref;
    jpt_vec3 p;
    if (!Live(v, ref, "v3GetHeadPos") || !WholeObject(v, ref, "v3GetHeadPos")) return SQ_ERROR;
    if (!Engine(Owner(v))->head_position(Engine(Owner(v))->user, ref.handle, &p)) return Throw(v, "v3GetHeadPos: the animal has no head");
    PushVector(v, {p.x, p.y, p.z});
    return 1;
}

SQInteger Health(HSQUIRRELVM v) {
    EntityRef ref;
    float hit_points = 0.0f;
    int dead = 0;
    if (!Live(v, ref, "GetHealth") || !WholeObject(v, ref, "GetHealth")) return SQ_ERROR;
    if (!Engine(Owner(v))->health(Engine(Owner(v))->user, ref.handle, &hit_points, &dead)) return Throw(v, "GetHealth: not an animate");
    return Push(v, hit_points);
}

SQInteger Dead(HSQUIRRELVM v) {
    EntityRef ref;
    float hit_points = 0.0f;
    int dead = 0;
    if (!Live(v, ref, "bDead") || !WholeObject(v, ref, "bDead")) return SQ_ERROR;
    if (!Engine(Owner(v))->health(Engine(Owner(v))->user, ref.handle, &hit_points, &dead)) return Throw(v, "bDead: not an animate");
    sq_pushbool(v, dead ? SQTrue : SQFalse);
    return 1;
}

SQInteger HeldObject(HSQUIRRELVM v) {
    EntityRef ref;
    if (!Live(v, ref, "pinsHeldObject") || !WholeObject(v, ref, "pinsHeldObject")) return SQ_ERROR;
    jpt_script_vm* vm = Owner(v);
    PushEntity(vm, Engine(vm)->held_object(Engine(vm)->user, ref.handle), -1);
    return 1;
}

SQInteger HeadRotRel(HSQUIRRELVM v) {
    EntityRef ref;
    jpt_rot3 r;
    if (!Live(v, ref, "r3HeadRotRel") || !WholeObject(v, ref, "r3HeadRotRel")) return SQ_ERROR;
    const jpt_script_engine* e = Engine(Owner(v));
    if (!e->head_rotation || !e->head_rotation(e->user, ref.handle, &r)) return Throw(v, "r3HeadRotRel: the player has no head");
    PushRotate(v, {r.w, r.x, r.y, r.z});
    return 1;
}

// CE names a sample as the game does, without regard to case or the path separator.
uint32_t SampleId(const char* name) {
    std::string n = name;
    for (char& c : n)
        if (c == '\\') c = '/';
    return jpt_sample_id(n.c_str());
}

SQInteger PlayerSay(HSQUIRRELVM v) {
    EntityRef ref;
    const SQChar* name = nullptr;
    if (!Live(v, ref, "Say") || !WholeObject(v, ref, "Say")) return SQ_ERROR;
    if (sq_gettop(v) != 2 || sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &name))) return Throw(v, "Say(strSample): expects a string");
    const jpt_script_engine* e = Engine(Owner(v));
    const uint32_t id = SampleId(name);
    if (id && e->say) e->say(e->user, ref.handle, id);
    return 0;
}

bool GunField(HSQUIRRELVM v, const char*& name) {
    name = nullptr;
    if (sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &name))) return false;
    return !strcmp(name, "iAmmo") || !strcmp(name, "iMaxAmmo");
}

SQInteger GunGet(HSQUIRRELVM v) {
    const char* name;
    EntityRef ref;
    int ammo = 0, most = 0;
    if (!GunField(v, name)) return sq_pushnull(v), sq_throwobject(v);
    if (!Live(v, ref, name)) return SQ_ERROR;
    if (!Engine(Owner(v))->gun_ammo(Engine(Owner(v))->user, ref.handle, &ammo, &most)) return Throw(v, std::string(name) + ": not a gun");
    sq_pushinteger(v, !strcmp(name, "iAmmo") ? ammo : most);
    return 1;
}

SQInteger GunSet(HSQUIRRELVM v) {
    const char* name;
    EntityRef ref;
    SQInteger value = 0;
    int ammo = 0, most = 0;
    if (!GunField(v, name)) return sq_pushnull(v), sq_throwobject(v);
    if (!Live(v, ref, name)) return SQ_ERROR;
    if (!Integer(v, 3, value) || value < 0 || value > 0x7FFFFFFF) return Throw(v, std::string(name) + ": takes an int from 0");
    if (!Engine(Owner(v))->gun_ammo(Engine(Owner(v))->user, ref.handle, &ammo, &most)) return Throw(v, std::string(name) + ": not a gun");
    if (!strcmp(name, "iAmmo")) ammo = static_cast<int>(value);
    else most = static_cast<int>(value);
    if (ammo > most) ammo = most;
    Engine(Owner(v))->set_gun_ammo(Engine(Owner(v))->user, ref.handle, ammo, most);
    return 0;
}

SQInteger ActivateSimStep(HSQUIRRELVM v) {
    EntityRef ref;
    SQBool on = SQFalse;
    if (!Live(v, ref, "ActivateSimStep") || !WholeObject(v, ref, "ActivateSimStep")) return SQ_ERROR;
    if (sq_gettop(v) != 2 || SQ_FAILED(sq_getbool(v, 2, &on))) return Throw(v, "ActivateSimStep(bActivate): expects a bool");
    jpt_script_vm_set_stepping(Owner(v), ref.handle, on != SQFalse);
    return 0;
}

SQInteger SimStepActive(HSQUIRRELVM v) {
    EntityRef ref;
    if (!Live(v, ref, "bIsSimStepActive") || !WholeObject(v, ref, "bIsSimStepActive")) return SQ_ERROR;
    sq_pushbool(v, jpt_script_vm_stepping(Owner(v), ref.handle) ? SQTrue : SQFalse);
    return 1;
}

// SendLGMessage(strMessage [, param]): every game and level script but the sender, now, in their order.
SQInteger SendToGameAndLevel(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQInteger n = sq_gettop(v);
    if ((n != 2 && n != 3) || sq_gettype(v, 2) != OT_STRING) return Throw(v, "SendLGMessage(strMessage [, param]): expects a string");
    if (vm->message_depth >= kMessageDepth) return Throw(v, "SendLGMessage: messages nested deeper than " + std::to_string(kMessageDepth));
    const SQInteger at[2] = {2, 3};
    const std::vector<int> receivers = vm->receivers;
    vm->message_depth++;
    for (const int instance : receivers)
        if (instance != vm->current) CallFromScript(vm, instance, "OnMessage", at, static_cast<int>(n - 1), false);
    vm->message_depth--;
    return 0;
}

// SendMessage(pinsTo, strMessage [, param]): the target's instance script answers now; null when it has none, or for itself.
SQInteger SendToObject(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQInteger n = sq_gettop(v);
    uint32_t target = 0;
    if ((n != 3 && n != 4) || sq_gettype(v, 3) != OT_STRING || (sq_gettype(v, 2) != OT_NULL && !GetEntity(v, 2, target)))
        return Throw(v, "SendMessage(pinsTo, strMessage [, param]): expects an object of the world and a string");
    const auto to = vm->entities.find(target);
    if (!target || to == vm->entities.end() || to->second == vm->current) return sq_pushnull(v), 1;
    if (vm->message_depth >= kMessageDepth) return Throw(v, "SendMessage: messages nested deeper than " + std::to_string(kMessageDepth));
    uint32_t from = 0;
    for (const auto& [handle, instance] : vm->entities)
        if (instance == vm->current) from = handle;
    PushEntity(vm, from, -1);
    const SQInteger at[3] = {sq_gettop(v), 3, 4};
    vm->message_depth++;
    const jpt_script_result result = CallFromScript(vm, to->second, "OnMessage", at, static_cast<int>(n - 1), true);
    vm->message_depth--;
    if (result != JPT_SCRIPT_OK) sq_pushnull(v);
    return 1;
}

// ---- game scripts: input and modal mode ----

int InstanceIdOf(jpt_script_vm* vm, SQInteger at) {
    HSQOBJECT self;
    if (sq_gettype(vm->v, at) != OT_INSTANCE || SQ_FAILED(sq_getstackobj(vm->v, at, &self))) return 0;
    for (size_t i = 0; i < vm->instances.size(); ++i)
        if (vm->instances[i]._unVal.pInstance == self._unVal.pInstance) return static_cast<int>(i) + 1;
    return 0;
}

bool ReadingInput(HSQUIRRELVM v, const char* name) {
    if (Owner(v)->reading_input) return true;
    Throw(v, std::string(name) + ": only in OnHandleInput or OnHandleModalInput");
    return false;
}

// The bit of a uCMD_USER constant; -1 for any other value.
int UserCommand(HSQUIRRELVM v, SQInteger at) {
    SQInteger value = 0;
    if (!Integer(v, at, value)) return -1;
    for (int i = 0; i < kUserCommands; ++i)
        if (value == kUserCommand << i) return i;
    return -1;
}

SQInteger KeyState(HSQUIRRELVM v, const char* name, bool hit) {
    if (!ReadingInput(v, name)) return SQ_ERROR;
    const int bit = sq_gettop(v) == 2 ? UserCommand(v, 2) : -1;
    if (bit < 0) return Throw(v, std::string(name) + "(uCMD): expects a uCMD_USER command");
    const uint32_t bits = hit ? Owner(v)->user_hit : Owner(v)->user_held;
    sq_pushbool(v, (bits >> bit & 1u) ? SQTrue : SQFalse);
    return 1;
}

SQInteger KeyHit(HSQUIRRELVM v) { return KeyState(v, "bKeyHit", true); }
SQInteger KeyPressed(HSQUIRRELVM v) { return KeyState(v, "bKeyPressed", false); }

SQInteger MoveY(HSQUIRRELVM v) {
    if (!ReadingInput(v, "fGetInputMoveAmountY")) return SQ_ERROR;
    return Push(v, Owner(v)->move_y);
}

SQInteger SetModal(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    SQBool on = SQFalse;
    const int self = InstanceIdOf(vm, 1);
    if (!self || sq_gettop(v) != 2 || SQ_FAILED(sq_getbool(v, 2, &on))) return Throw(v, "bSetModalMode(bEnable): expects a bool, on a game script");
    if (on && vm->modal && vm->modal != self) return sq_pushbool(v, SQFalse), 1;
    if (on) vm->modal = self;
    else if (vm->modal == self) vm->modal = 0;
    sq_pushbool(v, SQTrue);
    return 1;
}

SQInteger IsModal(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const int self = InstanceIdOf(vm, 1);
    sq_pushbool(v, self && vm->modal == self ? SQTrue : SQFalse);
    return 1;
}

// ---- drawing ----

const jpt_script_vm::Texture* TextureAt(HSQUIRRELVM v, SQInteger at, const char* name) {
    jpt_script_vm* vm = Owner(v);
    SQUserPointer p = nullptr;
    if (SQ_FAILED(sq_getinstanceup(v, at, &p, &kTextureTag, SQFalse))) {
        Throw(v, std::string(name) + ": not a texture of TP_Engine.GetTexture");
        return nullptr;
    }
    const TextureRef* ref = static_cast<const TextureRef*>(p);
    if (ref->world != vm->world || ref->id == 0 || ref->id > vm->textures.size()) {
        Throw(v, std::string(name) + ": the texture belongs to a world that is gone");
        return nullptr;
    }
    return &vm->textures[ref->id - 1];
}

std::string TextureKey(const char* name) {
    std::string key = name;
    for (char& c : key) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

void PushTexture(jpt_script_vm* vm, uint32_t id) {
    sq_pushobject(vm->v, vm->texture_class);
    sq_createinstance(vm->v, -1);
    sq_remove(vm->v, -2);
    SQUserPointer p = nullptr;
    if (SQ_SUCCEEDED(sq_getinstanceup(vm->v, -1, &p, &kTextureTag, SQFalse))) *static_cast<TextureRef*>(p) = {id, vm->world};
}

// GetTexture(strFileName): the same texture for the same name; null when the package has no such picture, as CE.
SQInteger GetTexture(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQChar* name = nullptr;
    if (sq_gettop(v) != 2 || sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &name))) return Throw(v, "TP_Engine.GetTexture(strFileName): expects a string");
    if (!vm->world_ready || !Engine(vm) || !Engine(vm)->load_texture) return Throw(v, "TP_Engine.GetTexture: the world is not loaded");
    const std::string key = TextureKey(name);
    for (size_t i = 0; i < vm->textures.size(); ++i)
        if (vm->textures[i].key == key) return PushTexture(vm, static_cast<uint32_t>(i) + 1), 1;
    int width = 0, height = 0;
    uint8_t* rgba = vm->textures.size() < JPT_DRAW_TEXTURES_MAX ? Engine(vm)->load_texture(Engine(vm)->user, name, &width, &height) : nullptr;
    const size_t bytes = rgba && width > 0 && height > 0 ? static_cast<size_t>(width) * static_cast<size_t>(height) * 4u : 0;
    if (!bytes || bytes > JPT_DRAW_TEXTURE_BYTES_MAX - vm->texture_bytes) {
        free(rgba);
        return sq_pushnull(v), 1;
    }
    jpt_script_vm::Texture texture;
    texture.key = key;
    texture.width = width;
    texture.height = height;
    texture.rgba.assign(rgba, rgba + bytes);
    free(rgba);
    vm->texture_bytes += bytes;
    vm->textures.push_back(std::move(texture));
    PushTexture(vm, static_cast<uint32_t>(vm->textures.size()));
    return 1;
}

SQInteger TextureWidth(HSQUIRRELVM v) {
    const jpt_script_vm::Texture* t = TextureAt(v, 1, "GetWidth");
    return t ? (sq_pushinteger(v, t->width), 1) : SQ_ERROR;
}

SQInteger TextureHeight(HSQUIRRELVM v) {
    const jpt_script_vm::Texture* t = TextureAt(v, 1, "GetHeight");
    return t ? (sq_pushinteger(v, t->height), 1) : SQ_ERROR;
}

// DrawTexture(texture, iSrcX, iSrcY, iSrcW, iSrcH, iDstX, iDstY, iDstW, iDstH, argbModulationColor, iBlendMode)
SQInteger DrawTexture(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    if (!vm->painting) return Throw(v, "TP_Engine.DrawTexture: only in OnPaintHW3D");
    if (sq_gettop(v) != 12) return Throw(v, "TP_Engine.DrawTexture(texture, iSrcX, iSrcY, iSrcW, iSrcH, iDstX, iDstY, iDstW, iDstH, argb, iBlendMode): expects 11 arguments");
    const jpt_script_vm::Texture* t = TextureAt(v, 2, "DrawTexture");
    if (!t) return SQ_ERROR;
    float r[8];
    SQInteger argb = 0, blend = 0;
    for (int k = 0; k < 8; ++k)
        if (!Number(v, 3 + k, r[k]) || !std::isfinite(r[k])) return Throw(v, "TP_Engine.DrawTexture: a rectangle of numbers");
    if (!Integer(v, 11, argb) || !Integer(v, 12, blend) || blend < JPT_DRAW_OPAQUE || blend > JPT_DRAW_ADD)
        return Throw(v, "TP_Engine.DrawTexture: an ARGB int and a blend mode 0, 1 or 2");
    if (vm->draws.size() >= JPT_DRAW_COMMANDS_MAX) return Throw(v, "TP_Engine.DrawTexture: more than " + std::to_string(JPT_DRAW_COMMANDS_MAX) + " draws in a frame");
    if (!(r[2] > 0.0f) || !(r[3] > 0.0f) || !(r[6] > 0.0f) || !(r[7] > 0.0f)) return 0;
    const uint32_t id = static_cast<uint32_t>(t - vm->textures.data()) + 1;
    vm->draws.push_back({id, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], static_cast<uint32_t>(argb), static_cast<int>(blend)});
    vm->draw_owners.push_back(vm->current);
    return 0;
}

SQInteger ScreenSize(HSQUIRRELVM v, bool width) {
    jpt_script_vm* vm = Owner(v);
    int w = 0, h = 0;
    if (!Engine(vm) || !Engine(vm)->screen_size || !Engine(vm)->screen_size(Engine(vm)->user, &w, &h))
        return Throw(v, width ? "TP_Engine.GetScreenWidth: no screen" : "TP_Engine.GetScreenHeight: no screen");
    sq_pushinteger(v, width ? w : h);
    return 1;
}

SQInteger ScreenWidth(HSQUIRRELVM v) { return ScreenSize(v, true); }
SQInteger ScreenHeight(HSQUIRRELVM v) { return ScreenSize(v, false); }

// The port draws OnPaintHW3D's textures with every renderer, the software one included.
SQInteger SoftwareMode(HSQUIRRELVM v) { return sq_pushbool(v, SQFalse), 1; }

// ---- TP_Engine ----

uint32_t NextRandom(jpt_script_vm* vm) {
    uint32_t x = vm->random ? vm->random : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return vm->random = x;
}

SQInteger ElapsedSimTime(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    if (!Engine(vm)) return Throw(v, "TP_Engine.GetElapsedSimTime: the world is not loaded");
    return Push(v, Engine(vm)->sim_time(Engine(vm)->user));
}

SQInteger RandInt(HSQUIRRELVM v) {
    sq_pushinteger(v, static_cast<SQInteger>(static_cast<int32_t>(NextRandom(Owner(v)))));
    return 1;
}

SQInteger RandIntRanged(HSQUIRRELVM v) {
    SQInteger lo = 0, hi = 0;
    if (sq_gettop(v) != 3 || !Integer(v, 2, lo) || !Integer(v, 3, hi)) return Throw(v, "TP_Engine.GetRandIntRanged(u_lo, u_hi): expects two ints");
    const uint32_t low = static_cast<uint32_t>(lo), high = static_cast<uint32_t>(hi);
    const uint32_t value = high > low ? low + NextRandom(Owner(v)) % (high - low) : low;
    sq_pushinteger(v, U4(value));
    return 1;
}

SQInteger Player(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    if (!vm->world_ready || !Engine(vm)) return sq_pushnull(v), 1;
    PushEntity(vm, Engine(vm)->player(Engine(vm)->user), -1);
    return 1;
}

int Channel(float f) { return f >= 1.0f ? 255 : f > 0.0f ? static_cast<int>(f * 255.0f + 0.5f) : 0; }
int Channel(SQInteger i) { return i > 255 ? 255 : i < 0 ? 0 : static_cast<int>(i); }

// rgb(r, g, b), rgba(r, g, b [, a]): ints 0-255; frgb, frgba: floats 0.0-1.0; out of range is clamped.
SQInteger Colour(HSQUIRRELVM v, bool floats, bool alpha, const char* usage) {
    const SQInteger n = sq_gettop(v);
    int c[4] = {0, 0, 0, 255};
    if (n != 4 && !(alpha && n == 5)) return Throw(v, usage);
    for (SQInteger k = 0; k + 2 <= n; ++k) {
        if (floats) {
            float f = 0;
            if (!Number(v, k + 2, f)) return Throw(v, usage);
            c[k] = Channel(f);
        } else {
            SQInteger i = 0;
            if (!Integer(v, k + 2, i)) return Throw(v, usage);
            c[k] = Channel(i);
        }
    }
    const uint32_t argb = static_cast<uint32_t>(c[3]) << 24 | static_cast<uint32_t>(c[0]) << 16 | static_cast<uint32_t>(c[1]) << 8 | static_cast<uint32_t>(c[2]);
    sq_pushinteger(v, U4(argb));
    return 1;
}

SQInteger Rgb(HSQUIRRELVM v) { return Colour(v, false, false, "TP_Engine.rgb(r, g, b): expects three ints"); }
SQInteger Rgba(HSQUIRRELVM v) { return Colour(v, false, true, "TP_Engine.rgba(r, g, b, a): expects three or four ints"); }
SQInteger Frgb(HSQUIRRELVM v) { return Colour(v, true, false, "TP_Engine.frgb(fr, fg, fb): expects three numbers"); }
SQInteger Frgba(HSQUIRRELVM v) { return Colour(v, true, true, "TP_Engine.frgba(fr, fg, fb, fa): expects three or four numbers"); }

uint32_t Handle32(SQInteger i) { return static_cast<uint32_t>(i); }

// u4DisplayFormattedString(str_text, f_time, i_fmt_flags, rgb_color [, handle_prev [, ETextType]]).
SQInteger DisplayText(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQInteger n = sq_gettop(v);
    const SQChar* text = nullptr;
    float seconds = 0;
    SQInteger format = 0, rgb = 0, previous = 0, type = 1;
    if (n < 5 || n > 7 || sq_gettype(v, 2) != OT_STRING || SQ_FAILED(sq_getstring(v, 2, &text)) || !Number(v, 3, seconds) ||
        !Integer(v, 4, format) || !Integer(v, 5, rgb) || (n >= 6 && !Integer(v, 6, previous)) || (n == 7 && !Integer(v, 7, type)))
        return Throw(v, "TP_Engine.u4DisplayFormattedString(str_text, f_time, i_fmt_flags, rgb_color, handle_prev, ETextType): expects a string, a number and ints");
    if (!std::isfinite(seconds) || seconds < 0.0f) return Throw(v, "u4DisplayFormattedString: a time that is not a number of seconds from 0");
    if (!vm->world_ready || !Engine(vm)) return Throw(v, "u4DisplayFormattedString: the world is not loaded");
    if (static_cast<size_t>(sq_getsize(v, 2)) >= JPT_TEXT_MAX) return Throw(v, "u4DisplayFormattedString: a text of " + std::to_string(JPT_TEXT_MAX) + " bytes or more");
    const uint32_t handle = Engine(vm)->show_text(Engine(vm)->user, text, seconds, Handle32(format) & JPT_TEXT_FORMATS, Handle32(rgb) & 0xFFFFFFu,
                                                  Handle32(previous), Handle32(type));
    sq_pushinteger(v, U4(handle));
    return 1;
}

SQInteger TextEnd(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    SQInteger type = 0;
    if (sq_gettop(v) != 2 || !Integer(v, 2, type)) return Throw(v, "TP_Engine.u4FindSequenceEnd(ETextType): expects an ETextType value");
    if (!vm->world_ready || !Engine(vm)) return Throw(v, "u4FindSequenceEnd: the world is not loaded");
    sq_pushinteger(v, U4(Engine(vm)->text_end(Engine(vm)->user, Handle32(type))));
    return 1;
}

SQInteger RandFloatRanged(HSQUIRRELVM v) {
    float lo = 0, hi = 0;
    if (sq_gettop(v) != 3 || !Number(v, 2, lo) || !Number(v, 3, hi)) return Throw(v, "TP_Engine.GetRandFloatRanged(f_lo, f_hi): expects two numbers");
    const double unit = static_cast<double>(NextRandom(Owner(v))) / 4294967295.0;
    return Push(v, static_cast<float>(lo + (static_cast<double>(hi) - lo) * unit));
}

// ---- binding ----

void Bind(HSQUIRRELVM v, const char* name, SQFUNCTION f, bool is_static = false) {
    sq_pushstring(v, name, -1);
    sq_newclosure(v, f, 0);
    sq_setnativeclosurename(v, -1, name);
    sq_newslot(v, -3, is_static ? SQTrue : SQFalse);
}

// A class of the root table, left on the stack.
HSQOBJECT NewClass(HSQUIRRELVM v, const char* name, const HSQOBJECT* base, SQUserPointer tag, SQInteger udsize) {
    sq_pushroottable(v);
    sq_pushstring(v, name, -1);
    if (base) sq_pushobject(v, *base);
    sq_newclass(v, base ? SQTrue : SQFalse);
    if (tag) sq_settypetag(v, -1, tag);
    if (udsize) sq_setclassudsize(v, -1, udsize);
    HSQOBJECT klass;
    sq_getstackobj(v, -1, &klass);
    sq_addref(v, &klass);
    sq_newslot(v, -3, SQFalse);
    sq_pop(v, 1);
    sq_pushobject(v, klass);
    return klass;
}

void BindVector(jpt_script_vm* vm) {
    HSQUIRRELVM v = vm->v;
    vm->vector_class = NewClass(v, "Vector3", nullptr, &kVectorTag, sizeof(V3));
    Bind(v, "constructor", VectorConstruct);
    Bind(v, "_get", VectorGet);
    Bind(v, "_set", VectorSet);
    Bind(v, "_add", VectorAdd);
    Bind(v, "_sub", VectorSub);
    Bind(v, "_mul", VectorMul);
    Bind(v, "_div", VectorDiv);
    Bind(v, "_tostring", VectorString);
    Bind(v, "_cloned", VectorCloned);
    Bind(v, "Scale", VectorScale);
    Bind(v, "Normalise", VectorNormalise);
    Bind(v, "Dot", VectorDot);
    Bind(v, "Cross", VectorCross);
    Bind(v, "Length", VectorLength);
    Bind(v, "LengthSqr", VectorLengthSqr);
    Bind(v, "XAxis", XAxis, true);
    Bind(v, "YAxis", YAxis, true);
    Bind(v, "ZAxis", ZAxis, true);
    sq_pop(v, 1);
}

void BindRotate(jpt_script_vm* vm) {
    HSQUIRRELVM v = vm->v;
    vm->rotate_class = NewClass(v, "Rotate3", nullptr, &kRotateTag, sizeof(Q));
    Bind(v, "constructor", RotateConstruct);
    Bind(v, "_get", RotateGet);
    Bind(v, "_set", RotateSet);
    Bind(v, "_mul", RotateMul);
    Bind(v, "_div", RotateDiv);
    Bind(v, "_unm", RotateUnm);
    Bind(v, "_tostring", RotateString);
    Bind(v, "_cloned", RotateCloned);
    Bind(v, "Scale", RotateScale);
    Bind(v, "Normalise", RotateNormalise);
    Bind(v, "Length", RotateLength);
    Bind(v, "LengthSqr", RotateLengthSqr);
    sq_pop(v, 1);
}

void BindEntities(jpt_script_vm* vm) {
    HSQUIRRELVM v = vm->v;
    for (const auto& c : kEntityClasses) {
        const HSQOBJECT* base = c.base ? &vm->natives.at(c.base) : nullptr;
        vm->natives[c.name] = NewClass(v, c.name, base, base ? nullptr : &kEntityTag, base ? 0 : sizeof(EntityRef));
        const std::string name = c.name;
        if (name == "CPartition") {
            Bind(v, "v3Pos", Pos);
            Bind(v, "r3Rot", Rot);
        } else if (name == "CInstance") {
            Bind(v, "GetInstanceName", InstanceName);
            Bind(v, "u4GetUniqueHandle", Handle);
            Bind(v, "Teleport", Teleport);
            Bind(v, "MoveWithFixedVelocity", FixedVelocity);
            Bind(v, "ActivateSimStep", ActivateSimStep);
            Bind(v, "bIsSimStepActive", SimStepActive);
        } else if (name == "CAnimate") {
            Bind(v, "pbbBoundaryBox", BoundaryBox);
            Bind(v, "GetHealth", Health);
            Bind(v, "bDead", Dead);
        } else if (name == "CPlayer") {
            Bind(v, "pinsHeldObject", HeldObject);
            Bind(v, "r3HeadRotRel", HeadRotRel);
            Bind(v, "Say", PlayerSay);
        } else if (name == "CGun") {
            Bind(v, "_get", GunGet);
            Bind(v, "_set", GunSet);
        } else if (name == "CAnimal") {
            Bind(v, "v3GetHeadPos", HeadPos);
        }
        sq_pop(v, 1);
    }
}

void BindScriptBases(jpt_script_vm* vm) {
    HSQUIRRELVM v = vm->v;
    HSQOBJECT game = NewClass(v, "GameScript", nullptr, nullptr, 0);
    Bind(v, "bKeyHit", KeyHit, true);
    Bind(v, "bKeyPressed", KeyPressed, true);
    Bind(v, "fGetInputMoveAmountY", MoveY, true);
    Bind(v, "bSetModalMode", SetModal);
    Bind(v, "bIsModalMode", IsModal);
    sq_pop(v, 1);
    sq_release(v, &game);
    HSQOBJECT level = NewClass(v, "LevelScript", nullptr, nullptr, 0);
    sq_pop(v, 1);
    sq_release(v, &level);
}

// A class of the table on top of the stack, left on the stack.
HSQOBJECT TableClass(HSQUIRRELVM v, const char* name, const HSQOBJECT* base) {
    sq_pushstring(v, name, -1);
    if (base) sq_pushobject(v, *base);
    sq_newclass(v, base ? SQTrue : SQFalse);
    if (!base) {
        sq_settypetag(v, -1, &kTextureTag);
        sq_setclassudsize(v, -1, sizeof(TextureRef));
    }
    HSQOBJECT klass;
    sq_getstackobj(v, -1, &klass);
    sq_addref(v, &klass);
    sq_newslot(v, -3, SQFalse);
    sq_pushobject(v, klass);
    return klass;
}

void BindEngine(jpt_script_vm* vm) {
    HSQUIRRELVM v = vm->v;
    sq_pushroottable(v);
    sq_pushstring(v, "TP_Engine", -1);
    sq_newtable(v);
    HSQOBJECT image = TableClass(v, "Image", nullptr);
    Bind(v, "GetWidth", TextureWidth);
    Bind(v, "GetHeight", TextureHeight);
    sq_pop(v, 1);
    vm->texture_class = TableClass(v, "Texture", &image);
    sq_pop(v, 1);
    sq_release(v, &image);
    Bind(v, "GetTexture", GetTexture);
    Bind(v, "DrawTexture", DrawTexture);
    Bind(v, "GetScreenWidth", ScreenWidth);
    Bind(v, "GetScreenHeight", ScreenHeight);
    Bind(v, "IsSoftwareMode", SoftwareMode);
    Bind(v, "GetElapsedSimTime", ElapsedSimTime);
    Bind(v, "GetRandInt", RandInt);
    Bind(v, "GetRandIntRanged", RandIntRanged);
    Bind(v, "GetRandFloatRanged", RandFloatRanged);
    Bind(v, "GetPlayer", Player);
    Bind(v, "rgb", Rgb);
    Bind(v, "rgba", Rgba);
    Bind(v, "frgb", Frgb);
    Bind(v, "frgba", Frgba);
    Bind(v, "u4DisplayFormattedString", DisplayText);
    Bind(v, "u4FindSequenceEnd", TextEnd);
    sq_newslot(v, -3, SQFalse);
    for (const auto& f : kTextFormats) {
        sq_pushstring(v, f.name, -1);
        sq_pushinteger(v, f.value);
        sq_newslot(v, -3, SQFalse);
    }
    sq_pushstring(v, "ETextType", -1);
    sq_newtable(v);
    for (const auto& t : kTextTypes) {
        sq_pushstring(v, t.name, -1);
        sq_pushinteger(v, t.value);
        sq_newslot(v, -3, SQFalse);
    }
    sq_newslot(v, -3, SQFalse);
    Bind(v, "SendLGMessage", SendToGameAndLevel);
    Bind(v, "SendMessage", SendToObject);
    for (int i = 0; i < kUserCommands; ++i) {
        const std::string name = "uCMD_USER" + std::to_string(i + 1);
        sq_pushstring(v, name.c_str(), -1);
        sq_pushinteger(v, kUserCommand << i);
        sq_newslot(v, -3, SQFalse);
    }
    for (const auto& k : kVirtualKeys) {
        sq_pushstring(v, k.name, -1);
        sq_pushinteger(v, k.value);
        sq_newslot(v, -3, SQFalse);
    }

    sq_pushstring(v, "EBoundaryBoxes", -1);
    sq_newtable(v);
    for (const auto& b : kBoxes) {
        sq_pushstring(v, b.name, -1);
        sq_pushinteger(v, b.value);
        sq_newslot(v, -3, SQFalse);
    }
    sq_newslot(v, -3, SQFalse);
    sq_pop(v, 1);
}

uint64_t WrapperKey(uint32_t handle, int box) { return static_cast<uint64_t>(handle) << 8 | static_cast<uint64_t>(box + 1); }

} // namespace

void BindApi(jpt_script_vm* vm) {
    vm->random = vm->config.random_seed ? vm->config.random_seed : 0x2545F491u;
    BindScriptBases(vm);
    BindVector(vm);
    BindRotate(vm);
    BindEntities(vm);
    BindEngine(vm);
}

void ReleaseApi(jpt_script_vm* vm) {
    if (!vm->v) return;
    for (auto& [key, object] : vm->wrappers) sq_release(vm->v, &object);
    for (auto& [handle, object] : vm->scripted) sq_release(vm->v, &object);
    for (auto& [name, object] : vm->natives) sq_release(vm->v, &object);
    sq_release(vm->v, &vm->vector_class);
    sq_release(vm->v, &vm->rotate_class);
    sq_release(vm->v, &vm->texture_class);
    vm->wrappers.clear();
    vm->scripted.clear();
    vm->natives.clear();
}

void PushVector(HSQUIRRELVM v, const V3& value) {
    sq_pushobject(v, Owner(v)->vector_class);
    sq_createinstance(v, -1);
    sq_remove(v, -2);
    if (V3* p = VectorAt(v, -1)) *p = value;
}

void PushRotate(HSQUIRRELVM v, const Q& value) {
    sq_pushobject(v, Owner(v)->rotate_class);
    sq_createinstance(v, -1);
    sq_remove(v, -2);
    if (Q* p = RotateAt(v, -1)) *p = value;
}

bool GetVector(HSQUIRRELVM v, SQInteger at, V3& out) {
    const V3* p = VectorAt(v, at);
    return p && (out = *p, true);
}

bool GetRotate(HSQUIRRELVM v, SQInteger at, Q& out) {
    const Q* p = RotateAt(v, at);
    return p && (out = *p, true);
}

bool GetEntity(HSQUIRRELVM v, SQInteger at, uint32_t& handle) {
    const EntityRef* ref = EntityAt(v, at);
    if (!ref || !ref->handle || ref->box >= 0) return false;
    handle = ref->handle;
    return true;
}

bool SetEntity(HSQUIRRELVM v, SQInteger at, uint32_t handle, int box, unsigned world) {
    EntityRef* ref = EntityAt(v, at);
    if (!ref) return false;
    *ref = {handle, box, world};
    return true;
}

// Wrappers live as long as the VM: an object handed out twice is the same instance, and the world's size bounds them.
void PushEntity(jpt_script_vm* vm, uint32_t handle, int box) {
    HSQUIRRELVM v = vm->v;
    if (!handle) return sq_pushnull(v);
    if (box < 0) {
        const auto scripted = vm->scripted.find(handle);
        if (scripted != vm->scripted.end()) return sq_pushobject(v, scripted->second);
    }
    const uint64_t key = WrapperKey(handle, box);
    const auto known = vm->wrappers.find(key);
    if (known != vm->wrappers.end()) return sq_pushobject(v, known->second);
    std::string type = box >= 0 ? "CBoundaryBox" : "CInstance";
    if (box < 0 && vm->world_ready && vm->config.engine) {
        const char* native = vm->config.engine->native_type(vm->config.engine->user, handle);
        if (native && vm->natives.count(native)) type = native;
    }
    sq_pushobject(v, vm->natives.at(type));
    sq_createinstance(v, -1);
    sq_remove(v, -2);
    SetEntity(v, -1, handle, box, vm->world);
    HSQOBJECT object;
    sq_getstackobj(v, -1, &object);
    sq_addref(v, &object);
    vm->wrappers[key] = object;
}

std::string NativeBase(jpt_script_vm* vm, const HSQOBJECT& klass) {
    HSQUIRRELVM v = vm->v;
    const SQInteger top = sq_gettop(v);
    sq_pushobject(v, klass);
    std::string found;
    for (int depth = 0; depth < 64 && found.empty() && sq_gettype(v, -1) == OT_CLASS; ++depth) {
        HSQOBJECT at;
        sq_getstackobj(v, -1, &at);
        for (const auto& [name, native] : vm->natives)
            if (native._unVal.pClass == at._unVal.pClass) found = name;
        if (SQ_FAILED(sq_getbase(v, -1))) break;
        sq_remove(v, -2);
    }
    sq_settop(v, top);
    return found;
}

} // namespace jpt::script
