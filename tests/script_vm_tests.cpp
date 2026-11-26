// The interpreter of Trespasser CE scripts (jpt_script_vm.cpp, jpt_script_heap.cpp), headless: loading, calls, errors,
// the instruction and memory budgets, the heap's accounting, the libraries a script gets and does not get, print() and
// its cap, the [script] lines.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <squirrel.h>

#include "jpt_script_heap.h"
#include "jpt_script_vm.h"

// Squirrel's allocation entry points (squirrel/squtils.h), defined by jpt_script_heap.cpp.
void* sq_vm_malloc(SQUnsignedInteger size);
void* sq_vm_realloc(void* p, SQUnsignedInteger oldsize, SQUnsignedInteger size);
void sq_vm_free(void* p, SQUnsignedInteger size);

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static std::vector<std::string> g_lines;

static void Capture(void*, const char* line) { g_lines.push_back(line); }

static bool Has(const std::string& line) {
    for (const std::string& l : g_lines)
        if (l == line) return true;
    return false;
}

static bool Contains(const std::string& part) {
    for (const std::string& l : g_lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

static int Count(const std::string& prefix) {
    int n = 0;
    for (const std::string& l : g_lines)
        if (!l.compare(0, prefix.size(), prefix)) n++;
    return n;
}

// The system allocator under every heap of these tests: what is still out, and refusals on demand.
static std::unordered_map<void*, size_t> g_blocks;
static size_t g_bytes;
static long g_refuse_after = -1;

static bool Refuse() {
    if (g_refuse_after < 0) return false;
    if (g_refuse_after == 0) return true;
    g_refuse_after--;
    return false;
}

static void* FakeAllocate(size_t size) {
    if (Refuse()) return nullptr;
    void* block = malloc(size ? size : 1);
    g_blocks[block] = size;
    g_bytes += size;
    return block;
}

static void* FakeReallocate(void* block, size_t size) {
    if (Refuse()) return nullptr;
    const size_t old = g_blocks[block];
    void* moved = realloc(block, size ? size : 1);
    if (!moved) return nullptr;
    g_blocks.erase(block);
    g_blocks[moved] = size;
    g_bytes = g_bytes - old + size;
    return moved;
}

static void FakeRelease(void* block) {
    g_bytes -= g_blocks[block];
    g_blocks.erase(block);
    free(block);
}

static const jpt_script_allocator kFake = {FakeAllocate, FakeReallocate, FakeRelease};

static jpt_script_vm* Vm(long instructions = 0, int print_lines = 0, size_t memory = 0) {
    g_lines.clear();
    g_refuse_after = -1;
    jpt_script_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.instructions = instructions;
    config.print_lines = print_lines;
    config.memory_limit = memory;
    config.allocator = &kFake;
    return jpt_script_vm_create(&config);
}

static jpt_script_memory Memory(const jpt_script_vm* vm) {
    jpt_script_memory memory;
    memset(&memory, 0, sizeof memory);
    CHECK(jpt_script_vm_memory(vm, &memory));
    return memory;
}

// Every byte the VM held goes back, and nothing says it did not.
static void Destroy(jpt_script_vm* vm) {
    if (vm) CHECK(Memory(vm).current == g_bytes);
    jpt_script_vm_destroy(vm);
    CHECK(g_bytes == 0 && g_blocks.empty());
    CHECK(!Contains("still held"));
}

static jpt_script_result Load(jpt_script_vm* vm, const char* name, const std::string& text) {
    return jpt_script_vm_load(vm, name, text.data(), text.size());
}

static void test_load_and_call() {
    jpt_script_vm* vm = Vm();
    CHECK(vm != nullptr);
    CHECK(Load(vm, "hello.nut", "function Hello() { print(\"hello \" + (2 + 3)); }") == JPT_SCRIPT_OK);
    CHECK(g_lines.empty());
    CHECK(jpt_script_vm_call(vm, "Hello", "Hello") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: hello 5"));
    CHECK(jpt_script_vm_call(vm, "Hello", "Missing") == JPT_SCRIPT_ABSENT);
    CHECK(Load(vm, "value.nut", "NotAFunction <- 3;") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "x", "NotAFunction") == JPT_SCRIPT_ABSENT);
    Destroy(vm);
}

static void test_classes_and_state() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "class Counter {\n"
        "  nvram = null;\n"
        "  constructor() { nvram = {}; nvram.n <- 0; }\n"
        "  function Step() { nvram.n++; return nvram.n; }\n"
        "}\n"
        "gCounter <- Counter();\n"
        "function Tick() { print(\"n=\" + gCounter.Step()); }\n";
    CHECK(Load(vm, "counter.nut", text) == JPT_SCRIPT_OK);
    for (int i = 0; i < 1000; i++) CHECK(jpt_script_vm_call(vm, "Counter", "Tick") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: n=1"));
    CHECK(Has("[script] print: n=200"));
    CHECK(!Has("[script] print: n=201"));
    CHECK(Has("[script] print: further output of this level is not logged"));
    Destroy(vm);
}

static void test_compile_error() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "broken.nut", "function A() {\n  local x = ;\n}\n") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- file=broken.nut callback=load line=2 column="));
    CHECK(jpt_script_vm_call(vm, "A", "A") == JPT_SCRIPT_ABSENT);
    Destroy(vm);
}

static void test_runtime_error_stops_owner_for_the_level() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "function Good() { print(\"good\"); }\n"
        "function Bad() {\n"
        "  local t = {};\n"
        "  return t.missing;\n"
        "}\n"
        "function Thrown() { throw \"boom\"; }\n"
        "function Caught() { try { throw \"inner\"; } catch (e) { print(\"caught \" + e); } }\n";
    CHECK(Load(vm, "errors.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "Spitter", "Bad") == JPT_SCRIPT_FAILED);
    CHECK(Has("[script] level=- script=Spitter file=errors.nut callback=Bad line=4 result=disabled error=the index 'missing' does not exist"));
    CHECK(jpt_script_vm_disabled(vm, "Spitter"));
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "Spitter", "Good") == JPT_SCRIPT_DISABLED);
    CHECK(g_lines.empty());
    CHECK(jpt_script_vm_call(vm, "SpitterHelper", "Good") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: good"));
    CHECK(jpt_script_vm_call(vm, "Hud", "Thrown") == JPT_SCRIPT_FAILED);
    CHECK(Has("[script] level=- script=Hud file=errors.nut callback=Thrown line=6 result=disabled error=boom"));
    CHECK(jpt_script_vm_call(vm, "Map", "Caught") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: caught inner"));
    jpt_script_vm_end_level(vm);
    CHECK(!jpt_script_vm_disabled(vm, "Spitter"));
    CHECK(jpt_script_vm_call(vm, "Spitter", "Good") == JPT_SCRIPT_OK);
    Destroy(vm);
}

static void test_top_level_error() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "top.nut", "function Later() {}\nlocal a = null;\na.b = 1;\n") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- script=top.nut file=top.nut callback=load line=3 function=main result=failed error="));
    Destroy(vm);
}

static void test_budget() {
    jpt_script_vm* vm = Vm(10000);
    const char* text =
        "function Spin() { while (true) {} }\n"
        "function OneLine() { for (;;); }\n"
        "function Catches() { try { while (true) {} } catch (e) { while (true) {} } }\n"
        "function Swallows() { while (true) { try { while (true) {} } catch (e) {} } }\n"
        "function Recurse(n) { return Recurse(n + 1); }\n"
        "function Deep() { return Recurse(0); }\n"
        "function Sorts() { local a = [3, 1, 2]; a.sort(function(x, y) { while (true) {} }); }\n"
        "function Small() { local s = 0; for (local i = 0; i < 100; i++) s += i; print(\"sum \" + s); }\n";
    CHECK(Load(vm, "budget.nut", text) == JPT_SCRIPT_OK);
    const char* spinners[] = {"Spin", "OneLine", "Catches", "Swallows", "Deep", "Sorts"};
    for (const char* name : spinners) {
        g_lines.clear();
        CHECK(jpt_script_vm_call(vm, name, name) == JPT_SCRIPT_FAILED);
        CHECK(Contains("instruction budget exceeded (10000)") || (!strcmp(name, "Deep") && Count("[script] level=- script=Deep ") == 1));
        CHECK(Count(std::string("[script] level=- script=") + name + " ") == 1 && Contains("result=disabled"));
    }
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: sum 4950"));
    // The budget is per entry: many small calls never add up to it.
    for (int i = 0; i < 500; i++) CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    Destroy(vm);

    vm = Vm(10000);
    CHECK(Load(vm, "loop.nut", "while (true) {}") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- script=loop.nut") && Contains("result=failed error=instruction budget exceeded (10000)"));
    Destroy(vm);
}

static void test_libraries() {
    jpt_script_vm* vm = Vm();
    const char* text =
        "function Present() {\n"
        "  print(format(\"%d %s\", 3, \"x\"));\n"
        "  print(\"sqrt \" + sqrt(16.0));\n"
        "  local b = blob(4); print(\"blob \" + b.len());\n"
        "  print(\"lower \" + \"ABC\".tolower());\n"
        "  print(\"cos \" + (cos(0.0) == 1.0));\n"
        "  local r = regexp(\"a+\"); print(\"rex \" + r.match(\"aaa\"));\n"
        "}\n"
        "function Absent() {\n"
        "  foreach (n in [\"seterrorhandler\", \"setdebughook\", \"enabledebuginfo\", \"setroottable\", \"setconsttable\",\n"
        "                 \"newthread\", \"suspend\", \"dofile\", \"loadfile\", \"writeclosuretofile\", \"file\", \"system\",\n"
        "                 \"getenv\", \"remove\", \"rename\", \"date\", \"clock\", \"time\"])\n"
        "    print(n + \" \" + (n in getroottable()));\n"
        "}\n";
    CHECK(Load(vm, "libs.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "libs", "Present") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: 3 x"));
    CHECK(Has("[script] print: sqrt 4"));
    CHECK(Has("[script] print: blob 4"));
    CHECK(Has("[script] print: lower abc"));
    CHECK(Has("[script] print: cos true"));
    CHECK(Has("[script] print: rex true"));
    CHECK(jpt_script_vm_call(vm, "libs", "Absent") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: ") == 6 + 18);
    int present = 0;
    for (size_t i = 6; i < g_lines.size(); i++)
        if (g_lines[i].size() > 5 && !g_lines[i].compare(g_lines[i].size() - 5, 5, " true")) present++;
    CHECK(present == 0);
    Destroy(vm);
}

static void test_print_lines_and_log_text() {
    jpt_script_vm* vm = Vm(0, 3);
    const char* text =
        "function Many() { for (local i = 0; i < 5; i++) print(\"line \" + i); }\n"
        "function Split() { print(\"a\\nb\"); }\n"
        "function Forged() { print(\"x\\r[session] fake\\t1\"); }\n"
        "function Raw() { error(\"to stderr\"); }\n";
    CHECK(Load(vm, "print.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "p", "Many") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: line 0") && Has("[script] print: line 2"));
    CHECK(!Has("[script] print: line 3"));
    CHECK(Count("[script] print: further output of this level is not logged") == 1);
    CHECK(jpt_script_vm_call(vm, "p", "Many") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: further output of this level is not logged") == 1);
    jpt_script_vm_end_level(vm);
    g_lines.clear();
    CHECK(jpt_script_vm_call(vm, "p", "Split") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: a") && Has("[script] print: b"));
    CHECK(jpt_script_vm_call(vm, "p", "Forged") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: x [session] fake 1"));
    CHECK(jpt_script_vm_call(vm, "p", "Raw") == JPT_SCRIPT_OK);
    CHECK(Count("[script] print: ") == 4);
    for (const std::string& l : g_lines) CHECK(!l.compare(0, 9, "[script] ") && l.find_first_of("\r\n\t") == std::string::npos);
    Destroy(vm);
}

static void test_files() {
    jpt_script_vm* vm = Vm();
    CHECK(Load(vm, "bom.nut", "\xEF\xBB\xBF" "function Bom() { print(\"bom\"); }") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "bom", "Bom") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: bom"));
    std::string large(JPT_SCRIPT_MAX_SOURCE + 1, ' ');
    CHECK(Load(vm, "large.nut", large) == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- file=large.nut callback=load result=failed error=larger than"));
    CHECK(Load(vm, "empty.nut", "") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_load(vm, "null.nut", nullptr, 0) == JPT_SCRIPT_FAILED);
    Destroy(vm);
    jpt_script_vm_destroy(nullptr);
    CHECK(jpt_script_vm_call(nullptr, "a", "b") == JPT_SCRIPT_ABSENT);
}

static bool Refused(ScriptHeap& heap, size_t size, bool budget, void* pointer = nullptr, bool reallocate = false) {
    try {
        if (reallocate) heap.Reallocate(pointer, size);
        else heap.Allocate(size);
    } catch (const ScriptHeapFailure& failure) {
        return failure.budget == budget;
    }
    return false;
}

static void test_heap_accounting() {
    const size_t overhead = ScriptHeap::Overhead();
    CHECK(overhead >= 2 * sizeof(void*) && overhead % alignof(std::max_align_t) == 0);
    {
        ScriptHeap heap(4096, &kFake);
        ScriptHeapStats s = heap.Stats();
        CHECK(s.current == 0 && s.peak == 0 && s.limit == 4096 && s.allocations == 0 && s.frees == 0 && s.freed_bytes == 0);

        char* p = static_cast<char*>(heap.Allocate(100));
        CHECK(p && reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t) == 0);
        memset(p, 'a', 100);
        CHECK(heap.Stats().current == 100 + overhead && heap.Stats().allocations == 1 && g_bytes == heap.Stats().current);

        p = static_cast<char*>(heap.Reallocate(p, 1000));
        CHECK(p[0] == 'a' && p[99] == 'a');
        CHECK(heap.Stats().current == 1000 + overhead && heap.Stats().reallocations == 1 && heap.Stats().peak == 1000 + overhead);

        p = static_cast<char*>(heap.Reallocate(p, 10));
        CHECK(p[9] == 'a');
        CHECK(heap.Stats().current == 10 + overhead && heap.Stats().freed_bytes == 990 && heap.Stats().peak == 1000 + overhead);

        ScriptHeap::Free(nullptr);
        CHECK(heap.Stats().current == 10 + overhead && heap.Stats().frees == 0);
        ScriptHeap::Free(p);
        s = heap.Stats();
        CHECK(s.current == 0 && s.frees == 1 && s.freed_bytes == 990 + 10 + overhead && g_bytes == 0);
    }
    {
        ScriptHeap heap(1000, &kFake);
        void* full = heap.Allocate(1000 - overhead);
        CHECK(heap.Stats().current == 1000);
        CHECK(Refused(heap, 0, true));
        CHECK(Refused(heap, 1, true));
        CHECK(Refused(heap, 1000 - overhead + 1, true, full, true));
        CHECK(heap.Stats().current == 1000 && heap.Stats().budget_failures == 3);
        full = heap.Reallocate(full, 1);
        CHECK(heap.Stats().current == 1 + overhead);
        ScriptHeap::Free(full);
        CHECK(Refused(heap, 1000 - overhead + 1, true));
        CHECK(heap.Stats().current == 0 && g_bytes == 0);
    }
    {
        ScriptHeap heap(SIZE_MAX, &kFake);
        CHECK(Refused(heap, SIZE_MAX, true));
        CHECK(Refused(heap, SIZE_MAX - overhead + 1, true));
        void* p = heap.Allocate(8);
        CHECK(Refused(heap, SIZE_MAX, true, p, true));
        CHECK(heap.Stats().current == 8 + overhead && heap.Stats().budget_failures == 3);
        ScriptHeap::Free(p);
    }
    {
        ScriptHeap heap(1 << 20, &kFake);
        void* p = heap.Allocate(64);
        g_refuse_after = 0;
        CHECK(Refused(heap, 32, false));
        CHECK(Refused(heap, 4096, false, p, true));
        CHECK(heap.Reallocate(p, 16) == p);
        g_refuse_after = -1;
        CHECK(heap.Stats().current == 64 + overhead && heap.Stats().system_failures == 2 && heap.Stats().budget_failures == 0);
        ScriptHeap::Free(p);
        CHECK(heap.Stats().current == 0);
    }
    {
        ScriptHeap first(1 << 20, &kFake);
        ScriptHeap second(1 << 20, &kFake);
        void* a = first.Allocate(10);
        ScriptHeapScope scope(&second);
        ScriptHeap::Free(a);
        CHECK(first.Stats().current == 0 && second.Stats().current == 0);
        for (int i = 0; i < 100; i++) second.Allocate(static_cast<size_t>(i));
        second.ReleaseAll();
        CHECK(second.Stats().current == 0 && second.Stats().frees == 100 && g_bytes == 0);
        second.Allocate(5);
    }
    CHECK(g_bytes == 0 && g_blocks.empty());
    {
        CHECK(ScriptHeap::Current() == nullptr);
        void* p = sq_vm_malloc(24);
        CHECK(p != nullptr);
        p = sq_vm_realloc(p, 24, 48);
        CHECK(p != nullptr);
        sq_vm_free(p, 48);
        ScriptHeap heap(1 << 20, &kFake);
        {
            ScriptHeapScope scope(&heap);
            CHECK(ScriptHeap::Current() == &heap);
            p = sq_vm_malloc(24);
        }
        CHECK(ScriptHeap::Current() == nullptr && heap.Stats().current == 24 + overhead);
        p = sq_vm_realloc(p, 24, 100);
        CHECK(heap.Stats().current == 100 + overhead);
        sq_vm_free(p, 100);
        CHECK(heap.Stats().current == 0);
    }
}

static bool MemoryExceeded(const char* owner) {
    return Contains("result=vm_disabled error=memory budget exceeded (limit ") && Contains(std::string("script=") + owner + " ") &&
           Has("[script] level=- result=vm_disabled every script of this VM stops");
}

static void CheckAbandoned(jpt_script_vm* vm) {
    const jpt_script_memory m = Memory(vm);
    CHECK(m.abandoned && m.current == 0 && g_bytes == 0 && m.budget_failures + m.system_failures == 1);
    CHECK(jpt_script_vm_call(vm, "Other", "Small") == JPT_SCRIPT_DISABLED);
    CHECK(Load(vm, "later.nut", "function Later() {}") == JPT_SCRIPT_DISABLED);
    CHECK(jpt_script_vm_disabled(vm, "anyone"));
    jpt_script_vm_end_level(vm);
    CHECK(jpt_script_vm_call(vm, "Other", "Small") == JPT_SCRIPT_DISABLED);
    for (const std::string& l : g_lines) CHECK(l.find("0x") == std::string::npos && l.size() < 400);
}

static const char* kMemoryScripts =
    "function Small() { local s = 0; for (local i = 0; i < 100; i++) s += i; print(\"sum \" + s); }\n"
    "function Spin() { while (true) {} }\n"
    "function Bad() { local t = {}; return t.missing; }\n"
    "function GrowArray() { local a = []; while (true) a.append(a.len()); }\n"
    "function GrowTable() { local t = {}; for (local i = 0; ; i++) t[i] <- i; }\n"
    "function GrowString() { local s = \"x\"; while (true) s += s; }\n"
    "function BigBlob() { local b = blob(64 * 1024 * 1024); }\n"
    "function GrowBlob() { local b = blob(0); for (local n = 1024; ; n *= 2) b.resize(n); }\n"
    "function HugeArray() { return array(0x7fffffff); }\n"
    "function NegativeArray() { return array(-1); }\n"
    "function Recurse(n) { return Recurse(n + 1) + 1; }\n"
    "function Deep() { return Recurse(0); }\n"
    "gKeep <- [];\n"
    "function ManySmall() { for (local i = 0; i < 2000; i++) gKeep.append({ i = i, s = \"k\" + i }); print(\"kept \" + gKeep.len()); }\n"
    "function ManySmallForever() { while (true) gKeep.append({ s = \"k\" + gKeep.len() }); }\n";

static jpt_script_vm* MemoryVm(long instructions, size_t memory) {
    jpt_script_vm* vm = Vm(instructions, 0, memory);
    CHECK(vm != nullptr);
    if (vm) CHECK(Load(vm, "memory.nut", kMemoryScripts) == JPT_SCRIPT_OK);
    return vm;
}

static void test_memory_budget() {
    const size_t limit = 2u * 1024 * 1024;
    const long many = 2000000000L;

    jpt_script_vm* vm = MemoryVm(0, 0);
    jpt_script_memory m = Memory(vm);
    CHECK(m.limit == JPT_SCRIPT_DEFAULT_MEMORY && m.current > 0 && m.current == g_bytes && m.peak >= m.current);
    CHECK(m.allocations > 0 && m.budget_failures == 0 && m.system_failures == 0 && !m.abandoned);
    CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_call(vm, "Spin", "Spin") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- script=Spin ") && Contains("error=instruction budget exceeded (1000000)"));
    CHECK(jpt_script_vm_call(vm, "Bad", "Bad") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- script=Bad file=memory.nut callback=Bad line=3 result=disabled error=the index 'missing' does not exist"));
    CHECK(jpt_script_vm_call(vm, "ManySmall", "ManySmall") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: kept 2000"));
    m = Memory(vm);
    CHECK(!m.abandoned && m.budget_failures == 0 && m.current == g_bytes && m.current < m.limit);
    CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    Destroy(vm);

    vm = MemoryVm(20000, 0);
    CHECK(jpt_script_vm_call(vm, "Deep", "Deep") == JPT_SCRIPT_FAILED);
    CHECK(Contains("[script] level=- script=Deep ") && Contains("error=instruction budget exceeded (20000)") && !Memory(vm).abandoned);
    CHECK(jpt_script_vm_call(vm, "Small", "Small") == JPT_SCRIPT_OK);
    Destroy(vm);

    const char* growers[] = {"GrowArray", "GrowTable", "GrowString", "BigBlob", "GrowBlob", "HugeArray", "NegativeArray", "Deep",
                             "ManySmallForever"};
    for (const char* name : growers) {
        vm = MemoryVm(many, limit);
        CHECK(jpt_script_vm_call(vm, name, name) == JPT_SCRIPT_FAILED);
        CHECK(MemoryExceeded(name));
        if (!MemoryExceeded(name)) printf("  %s: %s\n", name, g_lines.empty() ? "" : g_lines.back().c_str());
        CHECK(Memory(vm).limit == limit && Memory(vm).peak <= limit && Memory(vm).budget_failures == 1);
        CheckAbandoned(vm);
        Destroy(vm);
    }

    jpt_script_vm* first = MemoryVm(many, limit);
    jpt_script_vm* second = MemoryVm(0, limit);
    CHECK(jpt_script_vm_call(first, "GrowArray", "GrowArray") == JPT_SCRIPT_FAILED);
    CHECK(Memory(first).abandoned && !Memory(second).abandoned);
    g_lines.clear();
    CHECK(jpt_script_vm_call(second, "Small", "Small") == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: sum 4950"));
    CHECK(jpt_script_vm_call(second, "Spin", "Spin") == JPT_SCRIPT_FAILED);
    CHECK(Contains("instruction budget exceeded (1000000)") && !Memory(second).abandoned);
    jpt_script_vm_destroy(first);
    CHECK(Memory(second).current == g_bytes);
    Destroy(second);
}

static void test_memory_failures() {
    g_lines.clear();
    jpt_script_config config;
    memset(&config, 0, sizeof config);
    config.log = Capture;
    config.allocator = &kFake;
    config.memory_limit = 1024;
    CHECK(jpt_script_vm_create(&config) == nullptr);
    CHECK(Contains("[script] level=- script=the_interpreter callback=open result=vm_disabled error=memory budget exceeded (limit 1024 bytes)"));
    CHECK(g_bytes == 0 && g_blocks.empty());

    jpt_script_vm* vm = MemoryVm(0, 0);
    const size_t before = Memory(vm).current;
    g_refuse_after = 50;
    CHECK(jpt_script_vm_call(vm, "ManySmall", "ManySmall") == JPT_SCRIPT_FAILED);
    g_refuse_after = -1;
    CHECK(Contains("result=vm_disabled error=out of memory (the system refused ") && Contains("script=ManySmall "));
    CHECK(!Contains("memory budget exceeded"));
    const jpt_script_memory m = Memory(vm);
    CHECK(m.system_failures == 1 && m.budget_failures == 0 && m.peak >= before);
    CheckAbandoned(vm);
    Destroy(vm);

    vm = MemoryVm(0, 0);
    g_refuse_after = 3;
    CHECK(Load(vm, "late.nut", "gLate <- [1, 2, 3, \"four\", { five = 5 }]; function Late() {}") == JPT_SCRIPT_FAILED);
    g_refuse_after = -1;
    CHECK(Contains("script=late.nut "));
    CheckAbandoned(vm);
    Destroy(vm);
}

static void test_instances() {
    jpt_script_vm* vm = Vm(10000);
    jpt_script_vm_set_level(vm, "alpha.scn");
    CHECK(jpt_script_vm_define_class(vm, "GameScript") && jpt_script_vm_define_class(vm, "LevelScript"));
    const char* text =
        "class Base extends GameScript { function OnLevelStart() { print(\"base start\"); } }\n"
        "class Hud extends Base {\n"
        "  n = 0;\n"
        "  constructor() { print(\"hud made\"); }\n"
        "  function OnSimStep(dt) { n++; print(\"step \" + dt + \" \" + n); }\n"
        "  function OnLevelEnd(b) { print(\"end \" + b); }\n"
        "  function Say(s) { print(\"say \" + s); }\n"
        "}\n"
        "class Odd extends LevelScript { constructor() { throw \"no\"; } }\n"
        "class Spin extends LevelScript { function OnSimStep(dt) { while (true) {} } }\n"
        "class Plain {}\n"
        "NotAClass <- 3;\n";
    CHECK(Load(vm, "classes.nut", text) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_extends(vm, "Hud", "GameScript") && jpt_script_vm_extends(vm, "Hud", "Base") && jpt_script_vm_extends(vm, "Hud", "Hud"));
    CHECK(!jpt_script_vm_extends(vm, "Hud", "LevelScript") && !jpt_script_vm_extends(vm, "Plain", "GameScript"));
    CHECK(!jpt_script_vm_extends(vm, "NotAClass", "GameScript") && !jpt_script_vm_extends(vm, "Missing", "GameScript"));
    const int hud = jpt_script_vm_instantiate(vm, "Hud", "Hud");
    CHECK(hud > 0 && Has("[script] print: hud made"));
    CHECK(jpt_script_vm_has_method(vm, hud, "OnSimStep") && jpt_script_vm_has_method(vm, hud, "OnLevelStart"));
    CHECK(!jpt_script_vm_has_method(vm, hud, "OnPaint") && !jpt_script_vm_has_method(vm, hud, "n"));
    const jpt_script_arg dt{JPT_SCRIPT_ARG_FLOAT, 0, 0.25f, nullptr, 0, {0, 0, 0}, 0};
    const jpt_script_arg yes{JPT_SCRIPT_ARG_BOOL, 1, 0.0f, nullptr, 0, {0, 0, 0}, 0};
    const jpt_script_arg text_arg{JPT_SCRIPT_ARG_STRING, 0, 0.0f, "hi", 0, {0, 0, 0}, 0};
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnSimStep", &dt, 1) == JPT_SCRIPT_OK && Has("[script] print: step 0.25 1"));
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnSimStep", &dt, 1) == JPT_SCRIPT_OK && Has("[script] print: step 0.25 2"));
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnLevelEnd", &yes, 1) == JPT_SCRIPT_OK && Has("[script] print: end true"));
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "Say", &text_arg, 1) == JPT_SCRIPT_OK && Has("[script] print: say hi"));
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnLevelStart", nullptr, 0) == JPT_SCRIPT_OK && Has("[script] print: base start"));
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnPaint", nullptr, 0) == JPT_SCRIPT_ABSENT);
    CHECK(jpt_script_vm_invoke(vm, 99, "Hud", "OnSimStep", &dt, 1) == JPT_SCRIPT_ABSENT);
    CHECK(jpt_script_vm_invoke(vm, 0, "Hud", "OnSimStep", &dt, 1) == JPT_SCRIPT_ABSENT);

    g_lines.clear();
    CHECK(jpt_script_vm_instantiate(vm, "Odd", "Odd") == 0);
    CHECK(Has("[script] level=alpha.scn script=Odd file=classes.nut callback=constructor line=9 result=disabled error=no"));
    CHECK(jpt_script_vm_instantiate(vm, "Odd", "Odd") == 0 && g_lines.size() == 1);
    CHECK(jpt_script_vm_instantiate(vm, "Ghost", "Ghost") == 0);
    CHECK(Has("[script] level=alpha.scn script=Ghost callback=constructor result=disabled error=the class Ghost is not declared"));
    CHECK(jpt_script_vm_instantiate(vm, "NotAClass", "NotAClass") == 0);

    // One script running out of its instruction budget stops only itself; the next call of another gets a whole budget.
    const int spin = jpt_script_vm_instantiate(vm, "Spin", "Spin");
    CHECK(spin > 0);
    g_lines.clear();
    CHECK(jpt_script_vm_invoke(vm, spin, "Spin", "OnSimStep", &dt, 1) == JPT_SCRIPT_FAILED);
    CHECK(Has("[script] level=alpha.scn script=Spin file=classes.nut callback=OnSimStep line=10 result=disabled error=instruction budget exceeded (10000)"));
    CHECK(jpt_script_vm_invoke(vm, spin, "Spin", "OnSimStep", &dt, 1) == JPT_SCRIPT_DISABLED);
    CHECK(jpt_script_vm_invoke(vm, hud, "Hud", "OnSimStep", &dt, 1) == JPT_SCRIPT_OK && Has("[script] print: step 0.25 3"));
    CHECK(Count("[script] level=alpha.scn script=Spin ") == 1);
    Destroy(vm);
}

static jpt::script::Value Int(int64_t i) {
    jpt::script::Value v;
    v.type = jpt::script::Value::kInt;
    v.i = i;
    return v;
}

static const jpt::script::Slot* SlotOf(const std::vector<jpt::script::Slot>& slots, const char* name) {
    for (const jpt::script::Slot& s : slots)
        if (s.name == name) return &s;
    return nullptr;
}

static void test_nvram() {
    using jpt::script::Value;
    jpt_script_vm* vm = Vm();
    const char* text =
        "class Keeper {\n"
        "  nvram = null;\n"
        "  constructor() {\n"
        "    nvram = { i = 7, f = 1.5, b = true, s = \"text\", n = null, list = [1, 2.5, false, \"x\"], extra = \"kept\" };\n"
        "    nvram.bin <- blob(0); nvram.bin.writen(65, 'b'); nvram.bin.writen(66, 'b'); nvram.bin.writen(67, 'b');\n"
        "  }\n"
        "  function Odd() {\n"
        "    nvram.big <- array(70000, 1);\n"
        "    nvram.huge <- blob(300 * 1024);\n"
        "    nvram.table <- { a = 1 };\n"
        "    nvram.fn <- function() {};\n"
        "    nvram.nested <- [[1], blob(1), 3];\n"
        "    nvram[5] <- 1;\n"
        "  }\n"
        "  function Show() { print(nvram.i + \" \" + nvram.f + \" \" + nvram.b + \" \" + nvram.s + \" \" + nvram.list.len() + \" \" + nvram.extra + \" \" + nvram.bin.len()); }\n"
        "}\n"
        "class NoTable { nvram = 3; }\n"
        "class Later { nvram = null; function Show() { print(\"later \" + nvram.i); } }\n"
        "class NoNvram {}\n";
    CHECK(Load(vm, "nvram.nut", text) == JPT_SCRIPT_OK);
    const int keeper = jpt_script_vm_instantiate(vm, "Keeper", "Keeper");
    std::vector<jpt::script::Slot> slots;
    std::vector<std::string> notes;
    CHECK(jpt_script_vm_get_nvram(vm, keeper, slots, notes) && notes.empty());
    CHECK(slots.size() == 8);
    const jpt::script::Slot* list = SlotOf(slots, "list");
    CHECK(SlotOf(slots, "i") && SlotOf(slots, "i")->value.type == Value::kInt && SlotOf(slots, "i")->value.i == 7);
    CHECK(SlotOf(slots, "f") && SlotOf(slots, "f")->value.type == Value::kFloat && SlotOf(slots, "f")->value.f == 1.5f);
    CHECK(SlotOf(slots, "b") && SlotOf(slots, "b")->value.type == Value::kBool && SlotOf(slots, "b")->value.b);
    CHECK(SlotOf(slots, "s") && SlotOf(slots, "s")->value.bytes == "text" && SlotOf(slots, "n")->value.type == Value::kNull);
    CHECK(SlotOf(slots, "bin") && SlotOf(slots, "bin")->value.type == Value::kBlob && SlotOf(slots, "bin")->value.bytes == "ABC");
    CHECK(list && list->value.type == Value::kArray && list->value.items.size() == 4 && list->value.items[3].bytes == "x");

    // Into another instance: saved slots replace and add, the others stay.
    const int other = jpt_script_vm_instantiate(vm, "Keeper", "Keeper");
    std::vector<jpt::script::Slot> saved = {{"i", Int(42)}, {"added", Int(5)}};
    CHECK(jpt_script_vm_set_nvram(vm, other, saved));
    std::vector<jpt::script::Slot> after;
    CHECK(jpt_script_vm_get_nvram(vm, other, after, notes));
    CHECK(SlotOf(after, "i") && SlotOf(after, "i")->value.i == 42 && SlotOf(after, "added") && SlotOf(after, "added")->value.i == 5);
    CHECK(SlotOf(after, "extra") && SlotOf(after, "extra")->value.bytes == "kept");
    CHECK(jpt_script_vm_set_nvram(vm, other, slots));
    g_lines.clear();
    CHECK(jpt_script_vm_invoke(vm, other, "Keeper", "Show", nullptr, 0) == JPT_SCRIPT_OK);
    CHECK(Has("[script] print: 7 1.5 true text 4 kept 3"));

    // What a save cannot hold: cut to CE's limits or left out, each named.
    CHECK(jpt_script_vm_invoke(vm, keeper, "Keeper", "Odd", nullptr, 0) == JPT_SCRIPT_OK);
    CHECK(jpt_script_vm_get_nvram(vm, keeper, slots, notes));
    CHECK(SlotOf(slots, "big") && SlotOf(slots, "big")->value.items.size() == jpt::script::kArrayMax);
    CHECK(SlotOf(slots, "huge") && SlotOf(slots, "huge")->value.bytes.size() == jpt::script::kBlobMax);
    CHECK(!SlotOf(slots, "table") && !SlotOf(slots, "fn"));
    CHECK(SlotOf(slots, "nested") && SlotOf(slots, "nested")->value.items.size() == 3 && SlotOf(slots, "nested")->value.items[0].type == Value::kNull);
    CHECK(notes.size() == 6);

    const int no_table = jpt_script_vm_instantiate(vm, "NoTable", "NoTable");
    const int no_nvram = jpt_script_vm_instantiate(vm, "NoNvram", "NoNvram");
    CHECK(!jpt_script_vm_get_nvram(vm, no_table, slots, notes) && slots.empty() && !jpt_script_vm_set_nvram(vm, no_table, saved));
    CHECK(!jpt_script_vm_get_nvram(vm, no_nvram, slots, notes) && !jpt_script_vm_set_nvram(vm, no_nvram, saved));
    CHECK(!jpt_script_vm_get_nvram(vm, 0, slots, notes) && !jpt_script_vm_set_nvram(vm, 77, saved));
    // A declared nvram still null (made only when not loading a save): the save's slots become its table, as in CE.
    const int later = jpt_script_vm_instantiate(vm, "Later", "Later");
    CHECK(!jpt_script_vm_get_nvram(vm, later, slots, notes) && jpt_script_vm_set_nvram(vm, later, saved));
    g_lines.clear();
    CHECK(jpt_script_vm_invoke(vm, later, "Later", "Show", nullptr, 0) == JPT_SCRIPT_OK && Has("[script] print: later 42"));
    Destroy(vm);
}

int main() {
    test_heap_accounting();
    test_memory_budget();
    test_memory_failures();
    test_load_and_call();
    test_classes_and_state();
    test_compile_error();
    test_runtime_error_stops_owner_for_the_level();
    test_top_level_error();
    test_budget();
    test_libraries();
    test_print_lines_and_log_text();
    test_files();
    test_instances();
    test_nvram();
    printf("script_vm_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
