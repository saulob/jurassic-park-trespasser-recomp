#include "jpt_script_vm.h"

#include "jpt_script_heap.h"

#include <squirrel.h>
#include <sqstdblob.h>
#include <sqstdmath.h>
#include <sqstdstring.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
    std::string error;
    std::string where;
};

namespace {

// What a script may not reach: the error handler, debug hooks, the root and const tables, threads and suspension.
const char* const kRemoved[] = {"seterrorhandler", "setdebughook", "enabledebuginfo", "setroottable", "setconsttable",
                                "newthread", "suspend"};
const size_t kLogText = 300;

jpt_script_vm* Owner(HSQUIRRELVM v) { return static_cast<jpt_script_vm*>(sq_getforeignptr(v)); }

// Script text never forms a line of its own in the log.
std::string Clean(const char* text) {
    std::string out;
    for (const char* p = text ? text : ""; *p && out.size() < kLogText; ++p)
        out += (static_cast<unsigned char>(*p) < 0x20 || *p == 0x7f) ? ' ' : *p;
    return out;
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
    Log(vm, "[script] %s line %lld column %lld: %s", Clean(source).c_str(), static_cast<long long>(line),
        static_cast<long long>(column), Clean(desc).c_str());
}

SQInteger RuntimeError(HSQUIRRELVM v) {
    jpt_script_vm* vm = Owner(v);
    const SQChar* text = "error";
    if (sq_gettop(v) >= 2 && SQ_SUCCEEDED(sq_tostring(v, 2))) sq_getstring(v, -1, &text);
    vm->error = Clean(text);
    vm->where.clear();
    SQStackInfos info;
    for (SQInteger level = 0; SQ_SUCCEEDED(sq_stackinfos(v, level, &info)); ++level) {
        if (!info.source || !strcmp(info.source, "NATIVE")) continue;
        vm->where = Clean(info.source) + " line " + std::to_string(static_cast<long long>(info.line)) + ", function " +
                    Clean(info.funcname ? info.funcname : "unknown");
        break;
    }
    return 0;
}

bool Disabled(const jpt_script_vm* vm, const std::string& owner) {
    if (vm->abandoned) return true;
    for (const std::string& name : vm->disabled)
        if (name == owner) return true;
    return false;
}

// Nothing of Squirrel runs again: its state may be half updated where the allocation failed.
void Abandon(jpt_script_vm* vm, const std::string& owner, const ScriptHeapFailure& failure) {
    if (failure.budget)
        Log(vm, "[script] disabled: memory budget exceeded (limit %zu bytes) while running %s", vm->heap->Stats().limit,
            Clean(owner.c_str()).c_str());
    else
        Log(vm, "[script] disabled: out of memory (the system refused %zu bytes) while running %s", failure.requested,
            Clean(owner.c_str()).c_str());
    Log(vm, "[script] every script of this VM stops");
    vm->v = nullptr;
    vm->abandoned = true;
    vm->heap->ReleaseAll();
}

template <typename Body>
jpt_script_result Guarded(jpt_script_vm* vm, const std::string& owner, Body body) {
    ScriptHeapScope scope(vm->heap.get());
    try {
        return body();
    } catch (const ScriptHeapFailure& failure) {
        Abandon(vm, owner, failure);
        return JPT_SCRIPT_FAILED;
    } catch (const std::bad_alloc&) {
        Abandon(vm, owner, ScriptHeapFailure{false, 0});
        return JPT_SCRIPT_FAILED;
    }
}

// Runs the closure on top of the stack with the root table as 'this'.
jpt_script_result Run(jpt_script_vm* vm, const std::string& owner) {
    vm->error.clear();
    vm->where.clear();
    sq_pushroottable(vm->v);
    sq_setinstructionbudget(vm->v, static_cast<SQInteger>(vm->config.instructions));
    const SQRESULT result = sq_call(vm->v, 1, SQFalse, SQTrue);
    const bool exhausted = sq_getinstructionbudget(vm->v) == 0 && vm->error == "instruction budget exceeded";
    sq_setinstructionbudget(vm->v, -1);
    if (SQ_SUCCEEDED(result)) return JPT_SCRIPT_OK;
    if (exhausted) vm->error += " (" + std::to_string(vm->config.instructions) + ")";
    if (vm->where.empty())
        Log(vm, "[script] %s: %s", Clean(owner.c_str()).c_str(), vm->error.empty() ? "error" : vm->error.c_str());
    else
        Log(vm, "[script] %s: %s (%s)", Clean(owner.c_str()).c_str(), vm->error.empty() ? "error" : vm->error.c_str(),
            vm->where.c_str());
    Log(vm, "[script] %s stops until the level ends", Clean(owner.c_str()).c_str());
    vm->disabled.push_back(owner);
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

}  // namespace

extern "C" jpt_script_vm* jpt_script_vm_create(const jpt_script_config* config) {
    jpt_script_vm* vm = new jpt_script_vm;
    if (config) vm->config = *config;
    if (vm->config.instructions <= 0) vm->config.instructions = JPT_SCRIPT_DEFAULT_INSTRUCTIONS;
    if (vm->config.print_lines <= 0) vm->config.print_lines = JPT_SCRIPT_DEFAULT_PRINT_LINES;
    if (!vm->config.memory_limit) vm->config.memory_limit = JPT_SCRIPT_DEFAULT_MEMORY;
    vm->heap.reset(new ScriptHeap(vm->config.memory_limit, vm->config.allocator));
    const jpt_script_result opened = Guarded(vm, "the interpreter", [vm] {
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
        Guarded(vm, "the interpreter", [vm] {
            sq_close(vm->v);
            vm->v = nullptr;
            return JPT_SCRIPT_OK;
        });
    }
    if (vm->heap->Stats().current)
        Log(vm, "[script] %zu bytes were still held when the VM closed", vm->heap->Stats().current);
    delete vm;
}

extern "C" jpt_script_result jpt_script_vm_load(jpt_script_vm* vm, const char* name, const char* source, size_t size) {
    const std::string owner = name ? name : "script";
    if (!vm || !source) return JPT_SCRIPT_FAILED;
    if (vm->abandoned) return JPT_SCRIPT_DISABLED;
    if (size > JPT_SCRIPT_MAX_SOURCE) {
        Log(vm, "[script] %s is larger than %u bytes and is not read", Clean(owner.c_str()).c_str(), JPT_SCRIPT_MAX_SOURCE);
        return JPT_SCRIPT_FAILED;
    }
    if (size >= 3 && !memcmp(source, "\xEF\xBB\xBF", 3)) {
        source += 3;
        size -= 3;
    }
    return Guarded(vm, owner, [&] {
        const SQInteger top = sq_gettop(vm->v);
        if (SQ_FAILED(sq_compilebuffer(vm->v, source, static_cast<SQInteger>(size), owner.c_str(), SQTrue))) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_FAILED;
        }
        const jpt_script_result result = Run(vm, owner);
        sq_settop(vm->v, top);
        return result;
    });
}

extern "C" jpt_script_result jpt_script_vm_call(jpt_script_vm* vm, const char* owner, const char* function) {
    if (!vm || !function) return JPT_SCRIPT_ABSENT;
    const std::string who = owner ? owner : function;
    if (Disabled(vm, who)) return JPT_SCRIPT_DISABLED;
    return Guarded(vm, who, [&] {
        const SQInteger top = sq_gettop(vm->v);
        sq_pushroottable(vm->v);
        sq_pushstring(vm->v, function, -1);
        if (SQ_FAILED(sq_rawget(vm->v, -2))) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_ABSENT;
        }
        const SQObjectType type = sq_gettype(vm->v, -1);
        if (type != OT_CLOSURE && type != OT_NATIVECLOSURE) {
            sq_settop(vm->v, top);
            return JPT_SCRIPT_ABSENT;
        }
        const jpt_script_result result = Run(vm, who);
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
