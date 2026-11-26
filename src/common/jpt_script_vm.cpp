#include "jpt_script_vm.h"

#include "jpt_script_heap.h"

#include <squirrel.h>
#include <sqstdblob.h>
#include <sqstdmath.h>
#include <sqstdstring.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <new>
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
};

namespace {

using jpt::script::Slot;
using jpt::script::Value;

// What a script may not reach: the error handler, debug hooks, the root and const tables, threads and suspension.
const char* const kRemoved[] = {"seterrorhandler", "setdebughook", "enabledebuginfo", "setroottable", "setconsttable",
                                "newthread", "suspend"};
const size_t kLogText = 300;

jpt_script_vm* Owner(HSQUIRRELVM v) { return static_cast<jpt_script_vm*>(sq_getforeignptr(v)); }

// Script text never forms a line of its own in the log, nor a field of one.
std::string Clean(const char* text) {
    std::string out;
    for (const char* p = text ? text : ""; *p && out.size() < kLogText; ++p)
        out += (static_cast<unsigned char>(*p) < 0x20 || *p == 0x7f) ? ' ' : *p;
    return out;
}

std::string Field(const std::string& text) {
    std::string out = Clean(text.c_str());
    std::replace(out.begin(), out.end(), ' ', '_');
    return out.empty() ? "-" : out;
}

void Log(jpt_script_vm* vm, const char* format, ...) {
    if (!vm->config.log) return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    vm->config.log(vm->config.user, line);
}

void PrintText(jpt_script_vm* vm, const char* text) {
    const char* start = text;
    for (;;) {
        const char* end = strchr(start, '\n');
        const std::string piece(start, end ? static_cast<size_t>(end - start) : strlen(start));
        if (!piece.empty() || end) {
            if (vm->printed < vm->config.print_lines)
                Log(vm, "[script] print: %s", Clean(piece.c_str()).c_str());
            else if (vm->printed == vm->config.print_lines)
                Log(vm, "[script] print: further output of this level is not logged");
            if (vm->printed <= vm->config.print_lines) vm->printed++;
        }
        if (!end) break;
        start = end + 1;
        if (!*start) break;
    }
}

void Print(HSQUIRRELVM v, const SQChar* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    PrintText(Owner(v), text);
}

void CompileError(HSQUIRRELVM v, const SQChar* desc, const SQChar* source, SQInteger line, SQInteger column) {
    jpt_script_vm* vm = Owner(v);
    Log(vm, "[script] level=%s file=%s callback=load line=%lld column=%lld result=failed error=%s", Field(vm->level).c_str(),
        Field(source).c_str(), static_cast<long long>(line), static_cast<long long>(column), Clean(desc).c_str());
}

SQInteger RuntimeError(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQChar* text = "error";
    if (sq_gettop(v) >= 2 && SQ_SUCCEEDED(sq_tostring(v, 2))) sq_getstring(v, -1, &text);
    vm->error = Clean(text);
    vm->err_file.clear();
    vm->err_function.clear();
    vm->err_line = 0;
    SQStackInfos info;
    for (SQInteger level = 0; SQ_SUCCEEDED(sq_stackinfos(v, level, &info)); ++level) {
        if (!info.source || !strcmp(info.source, "NATIVE")) continue;
        vm->err_file = info.source;
        vm->err_line = static_cast<long long>(info.line);
        vm->err_function = info.funcname ? info.funcname : "unknown";
        break;
    }
    return 0;
}

bool Disabled(const jpt_script_vm* vm, const std::string& owner) {
    if (vm->abandoned) return true;
    return std::find(vm->disabled.begin(), vm->disabled.end(), owner) != vm->disabled.end();
}

void Stop(jpt_script_vm* vm, const std::string& owner, const std::string& callback, const char* result) {
    std::string line = "[script] level=" + Field(vm->level) + " script=" + Field(owner);
    if (!vm->err_file.empty()) line += " file=" + Field(vm->err_file);
    line += " callback=" + Field(callback);
    if (vm->err_line > 0) line += " line=" + std::to_string(vm->err_line);
    if (!vm->err_function.empty() && vm->err_function != callback) line += " function=" + Field(vm->err_function);
    line += std::string(" result=") + result + " error=" + (vm->error.empty() ? "error" : vm->error);
    Log(vm, "%s", line.c_str());
    if (std::find(vm->disabled.begin(), vm->disabled.end(), owner) == vm->disabled.end()) vm->disabled.push_back(owner);
}

// Nothing of Squirrel runs again: its state may be half updated where the allocation failed.
void Abandon(jpt_script_vm* vm, const std::string& owner, const std::string& callback, const ScriptHeapFailure& failure) {
    char error[160];
    if (failure.budget)
        snprintf(error, sizeof error, "memory budget exceeded (limit %zu bytes)", vm->heap->Stats().limit);
    else
        snprintf(error, sizeof error, "out of memory (the system refused %zu bytes)", failure.requested);
    Log(vm, "[script] level=%s script=%s callback=%s result=vm_disabled error=%s", Field(vm->level).c_str(), Field(owner).c_str(),
        Field(callback).c_str(), error);
    Log(vm, "[script] level=%s result=vm_disabled every script of this VM stops", Field(vm->level).c_str());
    vm->v = nullptr;
    vm->abandoned = true;
    vm->instances.clear();
    vm->heap->ReleaseAll();
}

template <typename Body>
jpt_script_result Guarded(jpt_script_vm* vm, const std::string& owner, const std::string& callback, Body body) {
    ScriptHeapScope scope(vm->heap.get());
    try {
        return body();
    } catch (const ScriptHeapFailure& failure) {
        Abandon(vm, owner, callback, failure);
        return JPT_SCRIPT_FAILED;
    } catch (const std::bad_alloc&) {
        Abandon(vm, owner, callback, ScriptHeapFailure{false, 0});
        return JPT_SCRIPT_FAILED;
    }
}

// Calls the closure below the params parameters on the stack (this first); a failure stops owner until the level ends.
jpt_script_result Call(jpt_script_vm* vm, const std::string& owner, const std::string& callback, SQInteger params, bool retval,
                       const char* result_word = "disabled") {
    vm->error.clear();
    vm->err_file.clear();
    vm->err_function.clear();
    vm->err_line = 0;
    sq_setinstructionbudget(vm->v, static_cast<SQInteger>(vm->config.instructions));
    const SQRESULT result = sq_call(vm->v, params, retval ? SQTrue : SQFalse, SQTrue);
    const bool exhausted = sq_getinstructionbudget(vm->v) == 0 && vm->error == "instruction budget exceeded";
    sq_setinstructionbudget(vm->v, -1);
    if (SQ_SUCCEEDED(result)) return JPT_SCRIPT_OK;
    if (exhausted) vm->error += " (" + std::to_string(vm->config.instructions) + ")";
    Stop(vm, owner, callback, result_word);
    return JPT_SCRIPT_FAILED;
}

void Open(jpt_script_vm* vm) {
    vm->v = sq_open(1024);
    sq_setforeignptr(vm->v, vm);
    sq_setprintfunc(vm->v, Print, Print);
    sq_setcompilererrorhandler(vm->v, CompileError);
    sq_enabledebuginfo(vm->v, SQTrue);
    sq_newclosure(vm->v, RuntimeError, 0);
    sq_seterrorhandler(vm->v);

    sq_pushroottable(vm->v);
    sqstd_register_mathlib(vm->v);
    sqstd_register_stringlib(vm->v);
    sqstd_register_bloblib(vm->v);
    for (const char* name : kRemoved) {
        sq_pushstring(vm->v, name, -1);
        sq_deleteslot(vm->v, -2, SQFalse);
    }
    sq_pop(vm->v, 1);
}

// The root table's slot of that name pushed; false (nothing pushed) when it has none.
bool PushRoot(HSQUIRRELVM v, const char* name) {
    sq_pushroottable(v);
    sq_pushstring(v, name, -1);
    if (SQ_FAILED(sq_rawget(v, -2))) {
        sq_pop(v, 1);
        return false;
    }
    sq_remove(v, -2);
    return true;
}

const HSQOBJECT* InstanceOf(jpt_script_vm* vm, int instance) {
    if (!vm || !vm->v || instance <= 0 || static_cast<size_t>(instance) > vm->instances.size()) return nullptr;
    return &vm->instances[static_cast<size_t>(instance) - 1];
}

// The closure of a method pushed; false (nothing pushed) when the instance has no such function.
bool PushMethod(jpt_script_vm* vm, const HSQOBJECT& instance, const char* method) {
    sq_pushobject(vm->v, instance);
    sq_pushstring(vm->v, method, -1);
    if (SQ_FAILED(sq_get(vm->v, -2))) {
        sq_pop(vm->v, 1);
        return false;
    }
    sq_remove(vm->v, -2);
    const SQObjectType type = sq_gettype(vm->v, -1);
    if (type == OT_CLOSURE || type == OT_NATIVECLOSURE) return true;
    sq_pop(vm->v, 1);
    return false;
}

bool ToValue(HSQUIRRELVM v, SQInteger at, Value& out, bool in_array, std::string& why) {
    switch (sq_gettype(v, at)) {
    case OT_NULL: out.type = Value::kNull; return true;
    case OT_INTEGER: {
        SQInteger i = 0;
        sq_getinteger(v, at, &i);
        out.type = Value::kInt;
        out.i = static_cast<int64_t>(i);
        return true;
    }
    case OT_FLOAT: {
        SQFloat f = 0;
        sq_getfloat(v, at, &f);
        out.type = Value::kFloat;
        out.f = static_cast<float>(f);
        return true;
    }
    case OT_BOOL: {
        SQBool b = SQFalse;
        sq_getbool(v, at, &b);
        out.type = Value::kBool;
        out.b = b != SQFalse;
        return true;
    }
    case OT_STRING: {
        const SQChar* s = nullptr;
        sq_getstring(v, at, &s);
        out.type = Value::kString;
        out.bytes.assign(s, static_cast<size_t>(sq_getsize(v, at)));
        return true;
    }
    case OT_ARRAY: {
        if (in_array) return why = "an array inside an array", false;
        out.type = Value::kArray;
        const SQInteger size = sq_getsize(v, at);
        const SQInteger count = std::min<SQInteger>(size, static_cast<SQInteger>(jpt::script::kArrayMax));
        if (size > count) why = "an array past " + std::to_string(jpt::script::kArrayMax) + " elements";
        for (SQInteger k = 0; k < count; ++k) {
            sq_pushinteger(v, k);
            if (SQ_FAILED(sq_rawget(v, at < 0 ? at - 1 : at))) break;
            Value item;
            std::string inner;
            if (!ToValue(v, -1, item, true, inner)) {
                if (why.empty()) why = inner;
                item = Value{};
            }
            out.items.push_back(item);
            sq_pop(v, 1);
        }
        return true;
    }
    case OT_INSTANCE: {
        SQUserPointer data = nullptr;
        if (!in_array && SQ_SUCCEEDED(sqstd_getblob(v, at, &data))) {
            const SQInteger size = sqstd_getblobsize(v, at);
            const size_t kept = std::min(static_cast<size_t>(size), jpt::script::kBlobMax);
            if (static_cast<size_t>(size) > kept) why = "a blob past " + std::to_string(jpt::script::kBlobMax) + " bytes";
            out.type = Value::kBlob;
            out.bytes.assign(static_cast<const char*>(data), kept);
            return true;
        }
        return why = in_array ? "a blob inside an array" : "an instance that is no blob", false;
    }
    default: return why = "a value of a type a save does not hold", false;
    }
}

void PushValue(HSQUIRRELVM v, const Value& value) {
    switch (value.type) {
    case Value::kNull: sq_pushnull(v); return;
    case Value::kInt: sq_pushinteger(v, static_cast<SQInteger>(value.i)); return;
    case Value::kFloat: sq_pushfloat(v, static_cast<SQFloat>(value.f)); return;
    case Value::kBool: sq_pushbool(v, value.b ? SQTrue : SQFalse); return;
    case Value::kString: sq_pushstring(v, value.bytes.data(), static_cast<SQInteger>(value.bytes.size())); return;
    case Value::kBlob: {
        SQUserPointer data = sqstd_createblob(v, static_cast<SQInteger>(value.bytes.size()));
        if (data && !value.bytes.empty()) memcpy(data, value.bytes.data(), value.bytes.size());
        if (!data) sq_pushnull(v);
        return;
    }
    case Value::kArray:
        sq_newarray(v, 0);
        for (const Value& item : value.items) {
            PushValue(v, item);
            sq_arrayappend(v, -2);
        }
        return;
    }
    sq_pushnull(v);
}

// The instance's nvram table pushed; false (nothing pushed) when it has none.
bool PushNvram(jpt_script_vm* vm, const HSQOBJECT& instance) {
    sq_pushobject(vm->v, instance);
    sq_pushstring(vm->v, "nvram", -1);
    if (SQ_FAILED(sq_get(vm->v, -2))) {
        sq_pop(vm->v, 1);
        return false;
    }
    sq_remove(vm->v, -2);
    if (sq_gettype(vm->v, -1) == OT_TABLE) return true;
    sq_pop(vm->v, 1);
    return false;
}

}  // namespace

extern "C" jpt_script_vm* jpt_script_vm_create(const jpt_script_config* config) {
    jpt_script_vm* vm = new jpt_script_vm;
    if (config) vm->config = *config;
    if (vm->config.instructions <= 0) vm->config.instructions = JPT_SCRIPT_DEFAULT_INSTRUCTIONS;
    if (vm->config.print_lines <= 0) vm->config.print_lines = JPT_SCRIPT_DEFAULT_PRINT_LINES;
    if (!vm->config.memory_limit) vm->config.memory_limit = JPT_SCRIPT_DEFAULT_MEMORY;
    vm->heap.reset(new ScriptHeap(vm->config.memory_limit, vm->config.allocator));
    const jpt_script_result opened = Guarded(vm, "the interpreter", "open", [vm] {
        Open(vm);
        return JPT_SCRIPT_OK;
    });
    if (opened != JPT_SCRIPT_OK) {
        delete vm;
        return nullptr;
    }
    return vm;
}

extern "C" void jpt_script_vm_destroy(jpt_script_vm* vm) {
    if (!vm) return;
    if (vm->v) {
        Guarded(vm, "the interpreter", "close", [vm] {
            for (HSQOBJECT& object : vm->instances) sq_release(vm->v, &object);
            vm->instances.clear();
            sq_close(vm->v);
            vm->v = nullptr;
            return JPT_SCRIPT_OK;
        });
    }
    if (vm->heap->Stats().current)
        Log(vm, "[script] %zu bytes were still held when the VM closed", vm->heap->Stats().current);
    delete vm;
}

extern "C" void jpt_script_vm_set_level(jpt_script_vm* vm, const char* level) {
    if (vm) vm->level = level ? level : "";
}

extern "C" jpt_script_result jpt_script_vm_load(jpt_script_vm* vm, const char* name, const char* source, size_t size) {
    const std::string owner = name ? name : "script";
    if (!vm || !source) return JPT_SCRIPT_FAILED;
    if (vm->abandoned) return JPT_SCRIPT_DISABLED;
    if (size > JPT_SCRIPT_MAX_SOURCE) {
        Log(vm, "[script] level=%s file=%s callback=load result=failed error=larger than %u bytes, not read", Field(vm->level).c_str(),
            Field(owner).c_str(), JPT_SCRIPT_MAX_SOURCE);
        return JPT_SCRIPT_FAILED;
    }
    if (size >= 3 && !memcmp(source, "\xEF\xBB\xBF", 3)) {
        source += 3;
        size -= 3;
    }
    return Guarded(vm, owner, "load", [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (SQ_FAILED(sq_compilebuffer(vm->v, source, static_cast<SQInteger>(size), owner.c_str(), SQTrue))) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_FAILED;
        }
        sq_pushroottable(vm->v);
        const jpt_script_result result = Call(vm, owner, "load", 1, false, "failed");
        sq_settop(vm->v, top);
        return result;
    });
}

extern "C" jpt_script_result jpt_script_vm_call(jpt_script_vm* vm, const char* owner, const char* function) {
    if (!vm || !function) return JPT_SCRIPT_ABSENT;
    const std::string who = owner ? owner : function;
    if (Disabled(vm, who)) return JPT_SCRIPT_DISABLED;
    return Guarded(vm, who, function, [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushRoot(vm->v, function)) return JPT_SCRIPT_ABSENT;
        const SQObjectType type = sq_gettype(vm->v, -1);
        if (type != OT_CLOSURE && type != OT_NATIVECLOSURE) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_ABSENT;
        }
        sq_pushroottable(vm->v);
        const jpt_script_result result = Call(vm, who, function, 1, false);
        sq_settop(vm->v, top);
        return result;
    });
}

extern "C" int jpt_script_vm_disabled(const jpt_script_vm* vm, const char* owner) {
    return vm && owner && Disabled(vm, owner);
}

extern "C" void jpt_script_vm_end_level(jpt_script_vm* vm) {
    if (!vm) return;
    vm->disabled.clear();
    vm->printed = 0;
}

extern "C" int jpt_script_vm_memory(const jpt_script_vm* vm, jpt_script_memory* out) {
    if (!vm || !out) return 0;
    const ScriptHeapStats& stats = vm->heap->Stats();
    out->current = stats.current;
    out->peak = stats.peak;
    out->limit = stats.limit;
    out->allocations = stats.allocations;
    out->reallocations = stats.reallocations;
    out->frees = stats.frees;
    out->freed_bytes = stats.freed_bytes;
    out->budget_failures = stats.budget_failures;
    out->system_failures = stats.system_failures;
    out->abandoned = vm->abandoned;
    return 1;
}

extern "C" int jpt_script_vm_define_class(jpt_script_vm* vm, const char* name) {
    if (!vm || !name || vm->abandoned) return 0;
    return Guarded(vm, name, "define", [&] {
        sq_pushroottable(vm->v);
        sq_pushstring(vm->v, name, -1);
        sq_newclass(vm->v, SQFalse);
        const SQRESULT result = sq_newslot(vm->v, -3, SQFalse);
        sq_pop(vm->v, 1);
        return SQ_SUCCEEDED(result) ? JPT_SCRIPT_OK : JPT_SCRIPT_FAILED;
    }) == JPT_SCRIPT_OK;
}

extern "C" int jpt_script_vm_extends(jpt_script_vm* vm, const char* name, const char* base) {
    if (!vm || !name || !base || vm->abandoned) return 0;
    bool found = false;
    Guarded(vm, name, "extends", [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushRoot(vm->v, base) || sq_gettype(vm->v, -1) != OT_CLASS || !PushRoot(vm->v, name) || sq_gettype(vm->v, -1) != OT_CLASS) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_OK;
        }
        HSQOBJECT want, at;
        sq_getstackobj(vm->v, -2, &want);
        for (int depth = 0; depth < 64 && sq_gettype(vm->v, -1) == OT_CLASS; ++depth) {
            sq_getstackobj(vm->v, -1, &at);
            if (at._unVal.pClass == want._unVal.pClass) {
                found = true;
                break;
            }
            if (SQ_FAILED(sq_getbase(vm->v, -1))) break;
            sq_remove(vm->v, -2);
        }
        sq_settop(vm->v, top);
        return JPT_SCRIPT_OK;
    });
    return found;
}

extern "C" int jpt_script_vm_instantiate(jpt_script_vm* vm, const char* owner, const char* class_name) {
    if (!vm || !class_name) return 0;
    const std::string who = owner ? owner : class_name;
    if (Disabled(vm, who)) return 0;
    int handle = 0;
    Guarded(vm, who, "constructor", [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushRoot(vm->v, class_name) || sq_gettype(vm->v, -1) != OT_CLASS) {
            vm->error = std::string("the class ") + Clean(class_name) + " is not declared";
            vm->err_file.clear();
            vm->err_function.clear();
            vm->err_line = 0;
            Stop(vm, who, "constructor", "disabled");
            sq_settop(vm->v, top);
            return JPT_SCRIPT_FAILED;
        }
        sq_pushroottable(vm->v);
        const jpt_script_result result = Call(vm, who, "constructor", 1, true);
        if (result == JPT_SCRIPT_OK && sq_gettype(vm->v, -1) == OT_INSTANCE) {
            HSQOBJECT object;
            sq_getstackobj(vm->v, -1, &object);
            sq_addref(vm->v, &object);
            vm->instances.push_back(object);
            handle = static_cast<int>(vm->instances.size());
        }
        sq_settop(vm->v, top);
        return result;
    });
    return handle;
}

extern "C" int jpt_script_vm_has_method(jpt_script_vm* vm, int instance, const char* method) {
    const HSQOBJECT* object = InstanceOf(vm, instance);
    if (!object || !method) return 0;
    bool has = false;
    const HSQOBJECT copy = *object;
    Guarded(vm, method, method, [&] {
        const SQInteger top = sq_gettop(vm->v);
        has = PushMethod(vm, copy, method);
        sq_settop(vm->v, top);
        return JPT_SCRIPT_OK;
    });
    return has;
}

extern "C" jpt_script_result jpt_script_vm_invoke(jpt_script_vm* vm, int instance, const char* owner, const char* method,
                                                  const jpt_script_arg* args, int count) {
    if (!vm || !method) return JPT_SCRIPT_ABSENT;
    const std::string who = owner ? owner : method;
    if (Disabled(vm, who)) return JPT_SCRIPT_DISABLED;
    const HSQOBJECT* object = InstanceOf(vm, instance);
    if (!object) return JPT_SCRIPT_ABSENT;
    const HSQOBJECT copy = *object;
    return Guarded(vm, who, method, [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushMethod(vm, copy, method)) return JPT_SCRIPT_ABSENT;
        sq_pushobject(vm->v, copy);
        for (int k = 0; k < count; ++k) {
            if (args[k].type == JPT_SCRIPT_ARG_BOOL) sq_pushbool(vm->v, args[k].b ? SQTrue : SQFalse);
            else if (args[k].type == JPT_SCRIPT_ARG_FLOAT) sq_pushfloat(vm->v, static_cast<SQFloat>(args[k].f));
            else sq_pushstring(vm->v, args[k].s ? args[k].s : "", -1);
        }
        const jpt_script_result result = Call(vm, who, method, 1 + count, false);
        sq_settop(vm->v, top);
        return result;
    });
}

bool jpt_script_vm_get_nvram(jpt_script_vm* vm, int instance, std::vector<Slot>& out, std::vector<std::string>& notes) {
    out.clear();
    const HSQOBJECT* object = InstanceOf(vm, instance);
    if (!object) return false;
    const HSQOBJECT copy = *object;
    bool has = false;
    Guarded(vm, "nvram", "OnPrepareSave", [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushNvram(vm, copy)) return JPT_SCRIPT_OK;
        has = true;
        sq_pushnull(vm->v);
        while (SQ_SUCCEEDED(sq_next(vm->v, -2))) {
            const SQChar* key = nullptr;
            if (sq_gettype(vm->v, -2) != OT_STRING || SQ_FAILED(sq_getstring(vm->v, -2, &key))) {
                notes.push_back("a slot whose name is not a string");
            } else {
                Slot slot;
                std::string why;
                slot.name = key;
                if (ToValue(vm->v, -1, slot.value, false, why)) out.push_back(slot);
                if (!why.empty()) notes.push_back(Clean(key) + ": " + why);
            }
            sq_pop(vm->v, 2);
        }
        sq_settop(vm->v, top);
        return JPT_SCRIPT_OK;
    });
    return has && !vm->abandoned;
}

bool jpt_script_vm_set_nvram(jpt_script_vm* vm, int instance, const std::vector<Slot>& slots) {
    const HSQOBJECT* object = InstanceOf(vm, instance);
    if (!object) return false;
    const HSQOBJECT copy = *object;
    bool has = false;
    Guarded(vm, "nvram", "load", [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (!PushNvram(vm, copy)) return JPT_SCRIPT_OK;
        has = true;
        for (const Slot& slot : slots) {
            sq_pushstring(vm->v, slot.name.data(), static_cast<SQInteger>(slot.name.size()));
            PushValue(vm->v, slot.value);
            sq_newslot(vm->v, -3, SQFalse);
        }
        sq_settop(vm->v, top);
        return JPT_SCRIPT_OK;
    });
    return has && !vm->abandoned;
}
